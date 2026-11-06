// vr_backend.h — SteamVR dashboard-tab display backend. Included by viewer.c when built with -DMD_VR (binary: sideframe).
//
//   decoder dmabuf (NV12) --import--> VkImage (zero copy) --sampler YCbCr(709) + fullscreen triangle--> RGBA8_SRGB target
//   --SetOverlayTexture--> SteamVR dashboard overlay "Mac Display".
// Before the first video frame (and when the stream stalls) a placeholder card is shown instead.
#ifndef VR_BACKEND_H
#define VR_BACKEND_H
#include <vulkan/vulkan.h>
#include <sys/stat.h>
#include <limits.h>
#include <math.h>
#include "openvr_capi.h"
#include "menu.h"

/* The C header keeps the global entry points in a disabled block; they are plain C exports of libopenvr_api.so. We load that library when the
 * program starts (dlopen) instead of linking it: it then always matches the SteamVR on the Frame, and Sideframe can be built on a machine
 * that has no SteamVR. The names below are function pointers so the rest of the code calls them like normal functions. */
#include <dlfcn.h>
static intptr_t (*VR_InitInternal)(EVRInitError *peError, EVRApplicationType eType);
static void (*VR_ShutdownInternal)(void);
static intptr_t (*VR_GetGenericInterface)(const char *pchInterfaceVersion, EVRInitError *peError);
static bool (*VR_IsRuntimeInstalled)(void);
static const char *(*VR_GetVRInitErrorAsEnglishDescription)(EVRInitError error);
static bool vr_load_api(void) {
    const char *paths[] = { "/opt/steamvr/bin/linuxarm64/libopenvr_api.so", "/opt/steamvr/bin/linux64/libopenvr_api.so", "libopenvr_api.so", NULL };
    void *h = NULL; for (int i = 0; paths[i] && !h; i++) h = dlopen(paths[i], RTLD_NOW | RTLD_GLOBAL);
    if (!h) { fprintf(stderr, "can't load SteamVR's libopenvr_api.so (%s) — is SteamVR installed?\n", dlerror()); return false; }
    VR_InitInternal = (void *)dlsym(h, "VR_InitInternal"); VR_ShutdownInternal = (void *)dlsym(h, "VR_ShutdownInternal");
    VR_GetGenericInterface = (void *)dlsym(h, "VR_GetGenericInterface"); VR_IsRuntimeInstalled = (void *)dlsym(h, "VR_IsRuntimeInstalled");
    VR_GetVRInitErrorAsEnglishDescription = (void *)dlsym(h, "VR_GetVRInitErrorAsEnglishDescription");
    if (!VR_InitInternal || !VR_ShutdownInternal || !VR_GetGenericInterface || !VR_IsRuntimeInstalled || !VR_GetVRInitErrorAsEnglishDescription) { fprintf(stderr, "libopenvr_api.so is missing expected functions\n"); return false; }
    return true;
}

static const uint32_t vert_spv[] =
#include "vert.spv.h"
;
static const uint32_t frag_spv[] =
#include "frag.spv.h"
;

#define APP_KEY      "org.sideframe.dashboard"
#define OVERLAY_KEY  "org.sideframe.dashboard.tab"
#define VK(x) do { VkResult r_ = (x); if (r_ != VK_SUCCESS) { fprintf(stderr, "%s:%d %s -> VkResult %d\n", __FILE__, __LINE__, #x, r_); exit(1); } } while (0)

static struct VR_IVROverlay_FnTable *ovl;
static struct VR_IVRApplications_FnTable *vapps;
static struct VR_IVRCompositor_FnTable *vcomp;
static VROverlayHandle_t ov_main, ov_thumb;
#include "kbd.h"
static bool kb_focus = false; static double kb_focus_t;   /* the laser is on the tab (with a short grace): physical keyboards type into the Mac */
static bool tab_visible = true;           /* refreshed from IsOverlayVisible() */
static double vr_ptr_t, vis_check_t, last_video_t, last_card_t, last_pause_t, vr_ev_t, hidden_since, vr_last_push;
static int vr_held = -1;                        /* newest decoded frame that has not been shown yet (decoder buffer index) */
static double vr_min_interval = 1.0 / 30;       /* cap on overlay updates (--max-fps N); each update makes SteamVR copy a ~20 MB image */
static double vr_out_scale = 0;                 /* --scale F (0.25...1) forces a fixed fraction of the Mac display; 0 (default) = automatic: sized from the headset, see vr_auto_cap_w */
static int vr_auto_cap_w = 2560;                /* widest texture worth sending: ~1.2x the headset's per-eye render width (read from SteamVR in vr_init) */
static double vr_idle_exit_s = 0;               /* --idle-exit SECONDS: quit after the tab has been hidden this long (0 = never) */
static void udp_msg(uint8_t type, uint32_t seq, uint32_t val, const uint16_t *idx, int n);   /* defined in viewer.c */
static void udp_text(const char *utf8, int n);   /* defined in viewer.c */
static void kb_send(int hid, int down) { udp_msg(8, (uint32_t)hid, (uint32_t)down, NULL, 0); }
static bool net_is_up(void);                                                                   /* defined in viewer.c */
static bool vr_have_video;                       /* a decoded picture from the Mac has arrived */
static int sw_nosubmit = -1, sw_noconv = -1;     /* test switches, read once */
static unsigned long vr_pushed, vr_skipped_hidden;
static bool vr_force_visible;                   /* MD_VR_ALWAYS=1: render even when the tab is hidden (testing) */
static int vr_checks_left;                      /* MD_VR_CHECK=1: read back + print a few converted pixels (testing) */
static VkBuffer chk; static VkDeviceMemory chk_mem; static uint8_t *chk_map;
static double vr_conv_sum, vr_conv_max; static unsigned long vr_conv_n;

/* ------------------------------------------------------------------ Vulkan state */
static VkInstance inst; static VkPhysicalDevice phys; static VkDevice dev; static VkQueue queue; static uint32_t qfam;
static VkCommandPool pool; static VkCommandBuffer cmd; static VkFence fence;
static PFN_vkGetMemoryFdPropertiesKHR pfnGetMemoryFdProperties;

struct tex { VkImage img; VkDeviceMemory mem; VkImageView view; VkFramebuffer fb; int w, h; VkImageLayout layout; };
static struct tex card;                  /* placeholder, CPU-drawn */
static struct tex outt;                  /* live video target (size of the Mac display) */
static VkBuffer stg; static VkDeviceMemory stg_mem; static uint8_t *stg_map;
#define CARD_W 1720
#define CARD_H 720

static VkRenderPass rpass; static VkPipelineLayout pipe_layout; static VkPipeline pipeline;
static VkDescriptorSetLayout dsl; static VkDescriptorPool dpool; static VkSampler ysampler; static VkSamplerYcbcrConversion yconv;
struct imp { VkImage img; VkDeviceMemory mem; VkImageView view; VkDescriptorSet ds; bool used; };
static struct imp imports[CAP_MAX];

static uint32_t find_mem(uint32_t bits, VkMemoryPropertyFlags want) {
    VkPhysicalDeviceMemoryProperties mp; vkGetPhysicalDeviceMemoryProperties(phys, &mp);
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++) if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & want) == want) return i;
    fprintf(stderr, "no suitable Vulkan memory type (bits 0x%x)\n", bits); exit(1);
}

/* ------------------------------------------------------------------ OpenVR helpers */
static void *vr_iface(const char *name) {
    EVRInitError e = EVRInitError_VRInitError_None; char buf[96]; snprintf(buf, sizeof buf, "FnTable:%s", name);
    void *p = (void *)VR_GetGenericInterface(buf, &e);
    if (!p) fprintf(stderr, "SteamVR doesn't offer %s: %s (%d)\n", name, VR_GetVRInitErrorAsEnglishDescription(e), (int)e);
    return p;
}
static int split_exts(char *s, const char **out, int max) { int n = 0; for (char *p = strtok(s, " "); p && n < max; p = strtok(NULL, " ")) out[n++] = p; return n; }
static int add_ext(const char **list, int n, const char *name) { for (int i = 0; i < n; i++) if (!strcmp(list[i], name)) return n; list[n] = name; return n + 1; }

static bool write_manifest(char *path, size_t plen, const char *icon) {
    char exe[PATH_MAX]; ssize_t n = readlink("/proc/self/exe", exe, sizeof exe - 1);
    if (n <= 0) return false;
    exe[n] = 0;
    const char *home = getenv("HOME"); if (!home) return false;
    char dir[PATH_MAX]; snprintf(dir, sizeof dir, "%s/.local/share/sideframe", home); mkdir(dir, 0755);
    snprintf(path, plen, "%s/sideframe.vrmanifest", dir);
    FILE *f = fopen(path, "w"); if (!f) return false;
    fprintf(f, "{\n  \"source\": \"user\",\n  \"applications\": [\n    {\n      \"app_key\": \"" APP_KEY "\",\n      \"launch_type\": \"binary\",\n"
               "      \"binary_path_linux\": \"%s\",\n      \"binary_path_linux_arm\": \"%s\",\n      \"image_path\": \"%s\",\n"
               "      \"is_dashboard_overlay\": true,\n      \"strings\": { \"en_us\": { \"name\": \"Sideframe\", \"description\": \"Your Mac desktop, in your Steam Frame\" } }\n    }\n  ]\n}\n", exe, exe, icon);
    fclose(f); return true;
}

/* ------------------------------------------------------------------ images */
static void make_tex(struct tex *t, int w, int h, VkImageUsageFlags usage) {
    t->w = w; t->h = h; t->layout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkImageCreateInfo ic = { .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = VK_FORMAT_R8G8B8A8_SRGB,
        .extent = { w, h, 1 }, .mipLevels = 1, .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = usage, .sharingMode = VK_SHARING_MODE_EXCLUSIVE, .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED };
    VK(vkCreateImage(dev, &ic, NULL, &t->img));
    VkMemoryRequirements mr; vkGetImageMemoryRequirements(dev, t->img, &mr);
    VkMemoryAllocateInfo ma = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size, .memoryTypeIndex = find_mem(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) };
    VK(vkAllocateMemory(dev, &ma, NULL, &t->mem)); VK(vkBindImageMemory(dev, t->img, t->mem, 0));
}

static void img_barrier(VkCommandBuffer c, VkImage im, VkImageLayout from, VkImageLayout to, uint32_t srcQ, uint32_t dstQ, VkAccessFlags sa, VkAccessFlags da) {
    VkImageMemoryBarrier b = { .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .srcAccessMask = sa, .dstAccessMask = da, .oldLayout = from, .newLayout = to,
        .srcQueueFamilyIndex = srcQ, .dstQueueFamilyIndex = dstQ, .image = im, .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };
    vkCmdPipelineBarrier(c, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, NULL, 0, NULL, 1, &b);
}
static void submit_wait(void) {
    VK(vkEndCommandBuffer(cmd));
    VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cmd };
    VK(vkQueueSubmit(queue, 1, &si, fence));
    VK(vkWaitForFences(dev, 1, &fence, VK_TRUE, UINT64_MAX)); VK(vkResetFences(dev, 1, &fence));
}
static void begin_cmd(void) {
    VK(vkResetCommandBuffer(cmd, 0));
    VkCommandBufferBeginInfo bi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT };
    VK(vkBeginCommandBuffer(cmd, &bi));
}

static void tex_submit(struct tex *t) {   /* hand a finished texture to SteamVR */
    VRVulkanTextureData_t vd = { .m_nImage = (uint64_t)t->img, .m_pDevice = dev, .m_pPhysicalDevice = phys, .m_pInstance = inst, .m_pQueue = queue,
                                 .m_nQueueFamilyIndex = qfam, .m_nWidth = t->w, .m_nHeight = t->h, .m_nFormat = VK_FORMAT_R8G8B8A8_SRGB, .m_nSampleCount = 1 };
    Texture_t tex = { &vd, ETextureType_TextureType_Vulkan, EColorSpace_ColorSpace_Auto };
    EVROverlayError e = ovl->SetOverlayTexture(ov_main, &tex);
    if (e != EVROverlayError_VROverlayError_None && vr_pushed < 3) fprintf(stderr, "SetOverlayTexture: %s (%d)\n", ovl->GetOverlayErrorNameFromEnum(e), (int)e);
    vr_pushed++;
}

/* ------------------------------------------------------------------ what the tab shows: the menu, or the live stream with a toolbar */
enum vr_view_t { VIEW_MENU, VIEW_STREAM };
static enum vr_view_t vr_view = VIEW_MENU;
static uint8_t *ui_buf;                       /* CPU canvas for the menu and the toolbar */
static int vid_w, vid_h, tb_h;                /* stream texture = video area (vid_w x vid_h) + a toolbar strip (tb_h rows) below it */
static int tb_hover = -1, tb_down = -1;       /* toolbar button under the pointer / pressed: 0 = Menu, 1 = Close */
static int vr_toolbar_action;                 /* set by a toolbar click: 1 = Menu (disconnect), 2 = Close the app */
static bool menu_need = true, tb_need;        /* something changed: redraw the menu / the toolbar */
static double menu_draw_t;

/* copy the staging buffer into a region of a texture; the texture ends up in COLOR_ATTACHMENT_OPTIMAL (what SteamVR is given) */
static void upload_region(struct tex *t, int x, int y, int w, int h) {
    begin_cmd();
    img_barrier(cmd, t->img, t->layout, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_MEMORY_WRITE_BIT);
    VkBufferImageCopy reg = { .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, .imageOffset = { x, y, 0 }, .imageExtent = { (uint32_t)w, (uint32_t)h, 1 } };
    vkCmdCopyBufferToImage(cmd, stg, t->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &reg);
    img_barrier(cmd, t->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_MEMORY_READ_BIT);
    t->layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    submit_wait();
}

/* draw the current menu screen and hand it to SteamVR */
static void menu_present(double t) {
    UI.px = ui_buf; UI.w = CARD_W; UI.h = CARD_H;
    menu_draw(t);
    memcpy(stg_map, ui_buf, (size_t)CARD_W * CARD_H * 4);
    upload_region(&card, 0, 0, CARD_W, CARD_H);
    tex_submit(&card);
}

static void tb_geom(int i, float *x, float *y, float *w, float *h) { toolbar_geom(tb_h, vid_w, i, x, y, w, h); }
static void toolbar_render(void) {
    if (!outt.img || tb_h <= 0) return;
    struct ui save = UI;
    UI.px = ui_buf; UI.w = vid_w; UI.h = tb_h;
    toolbar_paint(vid_w, tb_h, tb_hover, M.target.name);
    memcpy(stg_map, ui_buf, (size_t)vid_w * tb_h * 4);
    UI = save;
    upload_region(&outt, 0, vid_h, vid_w, tb_h);
}

/* menu <-> stream: which texture is on the tab, and how pointer coordinates are scaled */
static void vr_enter_menu(void) {
    vr_view = VIEW_MENU; menu_need = true; tb_hover = tb_down = -1;
    ovl->SetOverlayFlag(ov_main, VROverlayFlags_HideLaserIntersection, false);   /* the menu uses SteamVR's own cursor dot */
    HmdVector2_t sc = { (float)CARD_W, (float)CARD_H }; ovl->SetOverlayMouseScale(ov_main, &sc);
}
static void vr_enter_stream(void) {
    vr_view = VIEW_STREAM; tb_hover = tb_down = -1; vr_toolbar_action = 0;
    ovl->SetOverlayFlag(ov_main, VROverlayFlags_HideLaserIntersection, true);    /* the Mac draws its own cursor in the video: hide SteamVR's dot */
    HmdVector2_t sc = { (float)outt.w, (float)outt.h }; ovl->SetOverlayMouseScale(ov_main, &sc);
    toolbar_render();
}
static void vr_show_closing(void) {
    if (vr_view != VIEW_MENU) vr_enter_menu();
    M.scr = SCR_CLOSING; menu_present(now());
}

static void menu_frame(double t) {
    if (menu_tick(t)) menu_need = true;
    if (!menu_need || t - menu_draw_t < 0.05) return;     /* at most 20 redraws per second; a skipped redraw stays pending */
    menu_need = false; menu_draw_t = t;
    menu_present(t);
}

static void vr_mouse_move(float x, float y) {
    kb_focus = true; kb_focus_t = now();              /* SteamVR overlay mouse: origin bottom-left, in the scaled coordinates we set */
    if (vr_view == VIEW_MENU) { UI.mx = x; UI.my = CARD_H - y; UI.have_mouse = true; menu_need = true; return; }
    if (vid_w <= 0 || vid_h <= 0) return;
    vr_ptr_t = now();                                       /* pointer in use: poll events faster for a while (see vr_poll) */
    double nx = x / vid_w, ny = 1.0 - y / vid_h;            /* overlay origin is bottom-left; the Mac's is top-left */
    if (nx < 0) nx = 0; if (nx > 1) nx = 1; if (ny < 0) ny = 0; if (ny > 1) ny = 1;
    udp_msg(5, (uint32_t)(nx * 1e6), (uint32_t)(ny * 1e6), NULL, 0);   /* MOVE: position as millionths of the Mac display */
}
static void vr_mouse_button(bool down, uint32_t button) {
    if (vr_view == VIEW_MENU) {
        if (button != 1) return;
        if (down) { UI.down = true; UI.down_x = UI.mx; UI.down_y = UI.my; } else { UI.down = false; UI.released = true; }
        menu_need = true; return;
    }
    int mb = button == 1 ? 0 : button == 2 ? 1 : 2;           /* OpenVR left/right/middle (1/2/4-ish) -> Mac 0/1/2 */
    if (mb < 3) { udp_msg(6, mb, down ? 1 : 0, NULL, 0); vr_ptr_t = now(); }
}

/* ------------------------------------------------------------------ Vulkan device */
static void vk_setup(void) {
    uint32_t n = vcomp->GetVulkanInstanceExtensionsRequired(NULL, 0);
    char *ie = calloc(1, n + 1); vcomp->GetVulkanInstanceExtensionsRequired(ie, n);
    const char *iexts[64]; int nie = split_exts(ie, iexts, 64);
    VkApplicationInfo ai = { .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .pApplicationName = "sideframe", .apiVersion = VK_API_VERSION_1_1 };
    VkInstanceCreateInfo ici = { .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &ai, .enabledExtensionCount = nie, .ppEnabledExtensionNames = iexts };
    VK(vkCreateInstance(&ici, NULL, &inst));

    uint32_t np = 0; vkEnumeratePhysicalDevices(inst, &np, NULL);
    if (!np) { fprintf(stderr, "no Vulkan device\n"); exit(1); }
    VkPhysicalDevice pds[8]; if (np > 8) np = 8; vkEnumeratePhysicalDevices(inst, &np, pds); phys = pds[0];
    VkPhysicalDeviceProperties pp; vkGetPhysicalDeviceProperties(phys, &pp); fprintf(stderr, "Vulkan device: %s\n", pp.deviceName);

    uint32_t nq = 0; vkGetPhysicalDeviceQueueFamilyProperties(phys, &nq, NULL);
    VkQueueFamilyProperties qf[16]; if (nq > 16) nq = 16; vkGetPhysicalDeviceQueueFamilyProperties(phys, &nq, qf);
    qfam = UINT32_MAX; for (uint32_t i = 0; i < nq; i++) if (qf[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) { qfam = i; break; }
    if (qfam == UINT32_MAX) { fprintf(stderr, "no graphics queue\n"); exit(1); }

    uint32_t nd = vcomp->GetVulkanDeviceExtensionsRequired(phys, NULL, 0);
    char *de = calloc(1, nd + 1); vcomp->GetVulkanDeviceExtensionsRequired(phys, de, nd);
    const char *dexts[64]; int nde = split_exts(de, dexts, 40);
    /* plus what importing the decoder's dmabufs needs */
    nde = add_ext(dexts, nde, "VK_EXT_external_memory_dma_buf");
    nde = add_ext(dexts, nde, "VK_EXT_image_drm_format_modifier");
    nde = add_ext(dexts, nde, "VK_EXT_queue_family_foreign");
    nde = add_ext(dexts, nde, "VK_KHR_sampler_ycbcr_conversion");
    nde = add_ext(dexts, nde, "VK_KHR_bind_memory2");
    nde = add_ext(dexts, nde, "VK_KHR_maintenance1");
    nde = add_ext(dexts, nde, "VK_KHR_image_format_list");
    nde = add_ext(dexts, nde, "VK_KHR_external_memory_fd");
    float prio = 1.0f;
    VkDeviceQueueCreateInfo qi = { .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueFamilyIndex = qfam, .queueCount = 1, .pQueuePriorities = &prio };
    VkPhysicalDeviceSamplerYcbcrConversionFeatures yf = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SAMPLER_YCBCR_CONVERSION_FEATURES, .samplerYcbcrConversion = VK_TRUE };
    VkDeviceCreateInfo dci = { .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .pNext = &yf, .queueCreateInfoCount = 1, .pQueueCreateInfos = &qi,
                               .enabledExtensionCount = nde, .ppEnabledExtensionNames = dexts };
    VK(vkCreateDevice(phys, &dci, NULL, &dev));
    vkGetDeviceQueue(dev, qfam, 0, &queue);
    pfnGetMemoryFdProperties = (PFN_vkGetMemoryFdPropertiesKHR)vkGetDeviceProcAddr(dev, "vkGetMemoryFdPropertiesKHR");

    VkCommandPoolCreateInfo pc = { .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, .queueFamilyIndex = qfam };
    VK(vkCreateCommandPool(dev, &pc, NULL, &pool));
    VkCommandBufferAllocateInfo ca = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1 };
    VK(vkAllocateCommandBuffers(dev, &ca, &cmd));
    VkFenceCreateInfo fc = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    VK(vkCreateFence(dev, &fc, NULL, &fence));

    /* placeholder card + its staging buffer */
    make_tex(&card, CARD_W, CARD_H, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
    VkBufferCreateInfo bc = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = (VkDeviceSize)CARD_W * CARD_H * 4, .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT, .sharingMode = VK_SHARING_MODE_EXCLUSIVE };
    VK(vkCreateBuffer(dev, &bc, NULL, &stg));
    VkMemoryRequirements mr; vkGetBufferMemoryRequirements(dev, stg, &mr);
    VkMemoryAllocateInfo mb = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size,
        .memoryTypeIndex = find_mem(mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) };
    VK(vkAllocateMemory(dev, &mb, NULL, &stg_mem)); VK(vkBindBufferMemory(dev, stg, stg_mem, 0));
    VK(vkMapMemory(dev, stg_mem, 0, VK_WHOLE_SIZE, 0, (void **)&stg_map));
}

/* ------------------------------------------------------------------ NV12 -> RGBA conversion pipeline (needs the video size) */
static VkShaderModule shader(const uint32_t *code, size_t bytes) {
    VkShaderModuleCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = bytes, .pCode = code };
    VkShaderModule m; VK(vkCreateShaderModule(dev, &ci, NULL, &m)); return m;
}

static void vr_setup_output(int srcw, int srch) {
    /* Automatic sizing: the tab has a fixed physical width, so the headset can only show about vr_auto_cap_w pixels across it, whatever
     * the Mac display is. Never upscale; keep the Mac's aspect ratio (works for 16:9, 16:10, ultrawide, 5K, ...). */
    double sc = vr_out_scale > 0 ? vr_out_scale : (srcw > vr_auto_cap_w ? (double)vr_auto_cap_w / srcw : 1.0);
    int w = (int)(srcw * sc + 0.5) & ~1, h = (int)(srch * sc + 0.5) & ~1;   /* even sizes */
    if (sc < 0.999) fprintf(stderr, "efficient mode: SteamVR texture %dx%d (%.0f%% of the Mac display)\n", w, h, sc * 100);
    vid_w = w; vid_h = h; tb_h = 0;   /* the stream is the Mac screen and nothing else: no toolbar strip */
    int th = h + tb_h;
    make_tex(&outt, w, th, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
    VkImageViewCreateInfo vi = { .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = outt.img, .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = VK_FORMAT_R8G8B8A8_SRGB,
        .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };
    VK(vkCreateImageView(dev, &vi, NULL, &outt.view));

    VkAttachmentDescription att = { .format = VK_FORMAT_R8G8B8A8_SRGB, .samples = VK_SAMPLE_COUNT_1_BIT, .loadOp = VK_ATTACHMENT_LOAD_OP_LOAD,   /* keep the toolbar strip: only the video area is redrawn */
        .storeOp = VK_ATTACHMENT_STORE_OP_STORE, .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE, .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
        .initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, .finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
    VkAttachmentReference ref = { 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
    VkSubpassDescription sp = { .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS, .colorAttachmentCount = 1, .pColorAttachments = &ref };
    VkRenderPassCreateInfo rpi = { .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO, .attachmentCount = 1, .pAttachments = &att, .subpassCount = 1, .pSubpasses = &sp };
    VK(vkCreateRenderPass(dev, &rpi, NULL, &rpass));
    VkFramebufferCreateInfo fbi = { .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO, .renderPass = rpass, .attachmentCount = 1, .pAttachments = &outt.view, .width = w, .height = th, .layers = 1 };
    VK(vkCreateFramebuffer(dev, &fbi, NULL, &outt.fb));

    /* the target starts in COLOR_ATTACHMENT_OPTIMAL (what the render pass expects and what SteamVR is given) */
    begin_cmd();
    img_barrier(cmd, outt.img, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, 0, VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT);
    submit_wait(); outt.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

    /* hardware YCbCr -> RGB: BT.709, limited range (what VideoToolbox produced), 4:2:0 "left" chroma siting */
    VkFormatProperties fp; vkGetPhysicalDeviceFormatProperties(phys, VK_FORMAT_G8_B8R8_2PLANE_420_UNORM, &fp);
    fprintf(stderr, "NV12 (G8_B8R8_2PLANE_420) optimal features 0x%x\n", fp.optimalTilingFeatures);
    VkSamplerYcbcrConversionCreateInfo yci = { .sType = VK_STRUCTURE_TYPE_SAMPLER_YCBCR_CONVERSION_CREATE_INFO, .format = VK_FORMAT_G8_B8R8_2PLANE_420_UNORM,
        .ycbcrModel = VK_SAMPLER_YCBCR_MODEL_CONVERSION_YCBCR_709, .ycbcrRange = VK_SAMPLER_YCBCR_RANGE_ITU_NARROW,
        .components = { VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY },
        .xChromaOffset = VK_CHROMA_LOCATION_COSITED_EVEN, .yChromaOffset = VK_CHROMA_LOCATION_MIDPOINT, .chromaFilter = VK_FILTER_LINEAR };
    VK(vkCreateSamplerYcbcrConversion(dev, &yci, NULL, &yconv));
    VkSamplerYcbcrConversionInfo sci = { .sType = VK_STRUCTURE_TYPE_SAMPLER_YCBCR_CONVERSION_INFO, .conversion = yconv };
    VkSamplerCreateInfo smi = { .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO, .pNext = &sci, .magFilter = VK_FILTER_LINEAR, .minFilter = VK_FILTER_LINEAR,
        .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK };
    VK(vkCreateSampler(dev, &smi, NULL, &ysampler));

    VkDescriptorSetLayoutBinding bind = { .binding = 0, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .descriptorCount = 1,
        .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT, .pImmutableSamplers = &ysampler };
    VkDescriptorSetLayoutCreateInfo dli = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = 1, .pBindings = &bind };
    VK(vkCreateDescriptorSetLayout(dev, &dli, NULL, &dsl));
    VkDescriptorPoolSize ps = { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, CAP_MAX };
    VkDescriptorPoolCreateInfo dpi = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .maxSets = CAP_MAX, .poolSizeCount = 1, .pPoolSizes = &ps };
    VK(vkCreateDescriptorPool(dev, &dpi, NULL, &dpool));
    VkPipelineLayoutCreateInfo pli = { .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1, .pSetLayouts = &dsl };
    VK(vkCreatePipelineLayout(dev, &pli, NULL, &pipe_layout));

    VkShaderModule vs = shader(vert_spv, sizeof vert_spv), fs = shader(frag_spv, sizeof frag_spv);
    VkPipelineShaderStageCreateInfo stages[2] = {
        { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = vs, .pName = "main" },
        { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = fs, .pName = "main" } };
    VkPipelineVertexInputStateCreateInfo vin = { .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
    VkPipelineInputAssemblyStateCreateInfo ia = { .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO, .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST };
    VkPipelineViewportStateCreateInfo vp = { .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, .viewportCount = 1, .scissorCount = 1 };
    VkPipelineRasterizationStateCreateInfo rs = { .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO, .polygonMode = VK_POLYGON_MODE_FILL, .cullMode = VK_CULL_MODE_NONE, .frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE, .lineWidth = 1.0f };
    VkPipelineMultisampleStateCreateInfo ms = { .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO, .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT };
    VkPipelineColorBlendAttachmentState cba = { .colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT };
    VkPipelineColorBlendStateCreateInfo cb = { .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO, .attachmentCount = 1, .pAttachments = &cba };
    VkDynamicState dyn[2] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo ds = { .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO, .dynamicStateCount = 2, .pDynamicStates = dyn };
    VkGraphicsPipelineCreateInfo gp = { .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .stageCount = 2, .pStages = stages, .pVertexInputState = &vin,
        .pInputAssemblyState = &ia, .pViewportState = &vp, .pRasterizationState = &rs, .pMultisampleState = &ms, .pColorBlendState = &cb, .pDynamicState = &ds,
        .layout = pipe_layout, .renderPass = rpass, .subpass = 0 };
    VK(vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &gp, NULL, &pipeline));
    vkDestroyShaderModule(dev, vs, NULL); vkDestroyShaderModule(dev, fs, NULL);

    ovl->SetOverlayWidthInMeters(ov_main, 2.4f);
    if (getenv("MD_VR_CHECK")) {
        vr_checks_left = 2;
        VkBufferCreateInfo cbi = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = (VkDeviceSize)w * th * 4, .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT, .sharingMode = VK_SHARING_MODE_EXCLUSIVE };
        VK(vkCreateBuffer(dev, &cbi, NULL, &chk)); VkMemoryRequirements cmr; vkGetBufferMemoryRequirements(dev, chk, &cmr);
        VkMemoryAllocateInfo cma = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = cmr.size,
            .memoryTypeIndex = find_mem(cmr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) };
        VK(vkAllocateMemory(dev, &cma, NULL, &chk_mem)); VK(vkBindBufferMemory(dev, chk, chk_mem, 0)); VK(vkMapMemory(dev, chk_mem, 0, VK_WHOLE_SIZE, 0, (void **)&chk_map));
    }
    fprintf(stderr, "video output texture %dx%d ready (video %dx%d + %d-row toolbar)\n", w, th, w, h, tb_h);
}

/* Import decoder capture buffer i (an NV12 dmabuf) as a Vulkan image without copying it. */
static void vr_import_buffer(int i, int fd, int w, int h, int stride, uint32_t uv_off) {
    struct imp *m = &imports[i];
    VkExternalMemoryImageCreateInfo emi = { .sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO, .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT };
    VkSubresourceLayout planes[2] = { { .offset = 0, .rowPitch = stride }, { .offset = uv_off, .rowPitch = stride } };
    VkImageDrmFormatModifierExplicitCreateInfoEXT mod = { .sType = VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_EXPLICIT_CREATE_INFO_EXT, .pNext = &emi,
        .drmFormatModifier = DRM_FORMAT_MOD_LINEAR, .drmFormatModifierPlaneCount = 2, .pPlaneLayouts = planes };
    VkImageCreateInfo ic = { .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .pNext = &mod, .imageType = VK_IMAGE_TYPE_2D, .format = VK_FORMAT_G8_B8R8_2PLANE_420_UNORM,
        .extent = { w, h, 1 }, .mipLevels = 1, .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT,
        .usage = VK_IMAGE_USAGE_SAMPLED_BIT, .sharingMode = VK_SHARING_MODE_EXCLUSIVE, .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED };
    VkResult r = vkCreateImage(dev, &ic, NULL, &m->img);
    if (r != VK_SUCCESS) { fprintf(stderr, "importing decoder buffer %d as an NV12 Vulkan image failed (VkResult %d) — modifier LINEAR, pitch %d, uv offset %u\n", i, r, stride, uv_off); exit(1); }

    VkImageMemoryRequirementsInfo2 mi = { .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_REQUIREMENTS_INFO_2, .image = m->img };
    VkMemoryRequirements2 mr = { .sType = VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2 };
    vkGetImageMemoryRequirements2(dev, &mi, &mr);
    VkMemoryFdPropertiesKHR fdp = { .sType = VK_STRUCTURE_TYPE_MEMORY_FD_PROPERTIES_KHR };
    VK(pfnGetMemoryFdProperties(dev, VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT, fd, &fdp));
    uint32_t bits = mr.memoryRequirements.memoryTypeBits & fdp.memoryTypeBits;
    if (!bits) { fprintf(stderr, "no memory type is compatible with the dmabuf (image 0x%x, fd 0x%x)\n", mr.memoryRequirements.memoryTypeBits, fdp.memoryTypeBits); exit(1); }
    uint32_t mt = (uint32_t)__builtin_ctz(bits);
    VkMemoryDedicatedAllocateInfo ded = { .sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO, .image = m->img };
    VkImportMemoryFdInfoKHR imp = { .sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR, .pNext = &ded, .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT, .fd = dup(fd) };
    VkMemoryAllocateInfo ma = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .pNext = &imp, .allocationSize = mr.memoryRequirements.size, .memoryTypeIndex = mt };
    VK(vkAllocateMemory(dev, &ma, NULL, &m->mem));
    VK(vkBindImageMemory(dev, m->img, m->mem, 0));

    VkSamplerYcbcrConversionInfo sci = { .sType = VK_STRUCTURE_TYPE_SAMPLER_YCBCR_CONVERSION_INFO, .conversion = yconv };
    VkImageViewCreateInfo vi = { .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .pNext = &sci, .image = m->img, .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format = VK_FORMAT_G8_B8R8_2PLANE_420_UNORM, .components = { VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY },
        .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };
    VK(vkCreateImageView(dev, &vi, NULL, &m->view));
    VkDescriptorSetAllocateInfo dai = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, .descriptorPool = dpool, .descriptorSetCount = 1, .pSetLayouts = &dsl };
    VK(vkAllocateDescriptorSets(dev, &dai, &m->ds));
    VkDescriptorImageInfo dii = { .sampler = ysampler, .imageView = m->view, .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
    VkWriteDescriptorSet wds = { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = m->ds, .dstBinding = 0, .descriptorCount = 1,
        .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = &dii };
    vkUpdateDescriptorSets(dev, 1, &wds, 0, NULL);
    m->used = false;
    if (i == 0) fprintf(stderr, "imported decoder buffer 0 as a Vulkan NV12 image (zero copy): %dx%d pitch %d, uv offset %u, alloc %llu B\n", w, h, stride, uv_off, (unsigned long long)mr.memoryRequirements.size);
}

/* Convert decoder buffer i into the video texture and give it to SteamVR. Returns once the GPU is done reading the buffer,
 * so the caller can immediately hand the buffer back to the decoder. */
static void vr_present(int i) {
    struct imp *m = &imports[i];
    if (sw_noconv < 0) { sw_noconv = getenv("MD_VR_NOCONV") != NULL; sw_nosubmit = getenv("MD_VR_NOSUBMIT") != NULL; }
    if (sw_noconv) { tex_submit(&outt); vr_conv_n++; return; }   /* test switch: hand over the old texture, no GPU conversion */
    double tc0 = now();       /* nobody is looking: don't spend GPU/heat on it */
    begin_cmd();
    VkImageLayout from = m->used ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_UNDEFINED;
    img_barrier(cmd, m->img, from, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_QUEUE_FAMILY_FOREIGN_EXT, qfam, 0, VK_ACCESS_SHADER_READ_BIT);
    VkRenderPassBeginInfo rb = { .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, .renderPass = rpass, .framebuffer = outt.fb, .renderArea = { { 0, 0 }, { (uint32_t)outt.w, (uint32_t)outt.h } } };
    vkCmdBeginRenderPass(cmd, &rb, VK_SUBPASS_CONTENTS_INLINE);
    VkViewport vpt = { 0, 0, (float)vid_w, (float)vid_h, 0, 1 }; VkRect2D sc = { { 0, 0 }, { (uint32_t)vid_w, (uint32_t)vid_h } };
    vkCmdSetViewport(cmd, 0, 1, &vpt); vkCmdSetScissor(cmd, 0, 1, &sc);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe_layout, 0, 1, &m->ds, 0, NULL);
    vkCmdDraw(cmd, 3, 1, 0, 0);
    vkCmdEndRenderPass(cmd);
    img_barrier(cmd, m->img, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL, qfam, VK_QUEUE_FAMILY_FOREIGN_EXT, VK_ACCESS_SHADER_READ_BIT, 0);
    bool checking = vr_checks_left > 0;
    if (checking) {   /* testing only: copy the converted texture to host memory */
        img_barrier(cmd, outt.img, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_MEMORY_READ_BIT);
        VkBufferImageCopy reg = { .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, .imageExtent = { (uint32_t)outt.w, (uint32_t)outt.h, 1 } };
        vkCmdCopyImageToBuffer(cmd, outt.img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, chk, 1, &reg);
        img_barrier(cmd, outt.img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, VK_ACCESS_MEMORY_READ_BIT, VK_ACCESS_MEMORY_WRITE_BIT);
    }
    submit_wait();
    m->used = true;
    if (vr_view != VIEW_STREAM) vr_enter_stream();   /* first picture is ready: show the stream view with its toolbar */
    if (checking) {
        vr_checks_left--;
        fprintf(stderr, "converted pixels (R,G,B,A) one third down, middle of 7 bars:");
        for (int k = 0; k < 7; k++) { int x = (int)((k + 0.5) * outt.w / 7); const uint8_t *q = chk_map + ((size_t)(outt.h / 3) * outt.w + x) * 4; fprintf(stderr, "  [%d,%d,%d,%d]", q[0], q[1], q[2], q[3]); }
        fprintf(stderr, "\n   (expected for the 75%% bars clip: [191,191,191] [191,191,0] [0,191,191] [0,191,0] [191,0,191] [191,0,0] [0,0,191])\n");
        const uint8_t *g = chk_map + ((size_t)(outt.h * 5 / 6) * outt.w + outt.w / 2) * 4;
        fprintf(stderr, "   grey ramp at 50%% across: [%d,%d,%d] (expect ~128 each)\n", g[0], g[1], g[2]);
    }
    if (!sw_nosubmit) tex_submit(&outt);   /* test switch: convert but do not hand to SteamVR */
    double dc = now() - tc0; vr_conv_sum += dc; vr_conv_n++; if (dc > vr_conv_max) vr_conv_max = dc;
}

/* ------------------------------------------------------------------ frame pacing: newest frame wins, capped update rate */
/* A decoded frame arrived. The newest one wins: returns the buffer index of the older, never-shown frame that the caller must hand back
 * to the decoder right away (or -1). Nothing is converted or sent to SteamVR here, so dropped frames cost no GPU work. */
static int vr_offer(int idx) { int give = vr_held; vr_held = idx; last_video_t = now(); vr_have_video = true; return give; }

/* Show the held frame when it is due and someone is looking at the tab. Returns the buffer index to hand back to the decoder, or -1. */
static int vr_service(void) {
    if (vr_held < 0) return -1;
    double t = now();
    if (!tab_visible && !vr_force_visible) { int g = vr_held; vr_held = -1; vr_skipped_hidden++; return g; }
    if (t - vr_last_push < vr_min_interval) return -1;
    int g = vr_held; vr_held = -1;
    vr_present(g); vr_last_push = t;
    return g;
}
/* milliseconds until the held frame is due (a long time if nothing is held), so the main loop can sleep exactly that long */
static int vr_due_ms(void) { if (vr_held < 0) return 1000; double d = vr_last_push + vr_min_interval - now(); return d <= 0 ? 0 : (int)(d * 1000) + 1; }

/* ------------------------------------------------------------------ tear down a stream (back to the menu) */
/* Destroys everything that belongs to one stream: the imported decoder buffers, the video texture and the conversion pipeline.
 * Call BEFORE the decoder's buffers are closed. The menu keeps working afterwards. */
static void vr_teardown_output(void) {
    if (!dev) return;
    vkDeviceWaitIdle(dev);
    for (int i = 0; i < CAP_MAX; i++) {
        struct imp *m = &imports[i];
        if (m->view) vkDestroyImageView(dev, m->view, NULL);
        if (m->img) vkDestroyImage(dev, m->img, NULL);
        if (m->mem) vkFreeMemory(dev, m->mem, NULL);            /* also closes the dup'd dmabuf fd */
        memset(m, 0, sizeof *m);
    }
    if (dpool) { vkDestroyDescriptorPool(dev, dpool, NULL); dpool = VK_NULL_HANDLE; }
    if (pipeline) { vkDestroyPipeline(dev, pipeline, NULL); pipeline = VK_NULL_HANDLE; }
    if (pipe_layout) { vkDestroyPipelineLayout(dev, pipe_layout, NULL); pipe_layout = VK_NULL_HANDLE; }
    if (dsl) { vkDestroyDescriptorSetLayout(dev, dsl, NULL); dsl = VK_NULL_HANDLE; }
    if (ysampler) { vkDestroySampler(dev, ysampler, NULL); ysampler = VK_NULL_HANDLE; }
    if (yconv) { vkDestroySamplerYcbcrConversion(dev, yconv, NULL); yconv = VK_NULL_HANDLE; }
    if (outt.fb) vkDestroyFramebuffer(dev, outt.fb, NULL);
    if (rpass) { vkDestroyRenderPass(dev, rpass, NULL); rpass = VK_NULL_HANDLE; }
    if (outt.view) vkDestroyImageView(dev, outt.view, NULL);
    if (outt.img) vkDestroyImage(dev, outt.img, NULL);
    if (outt.mem) vkFreeMemory(dev, outt.mem, NULL);
    memset(&outt, 0, sizeof outt);
    vid_w = vid_h = tb_h = 0; vr_held = -1; vr_have_video = false; vr_checks_left = 0;
    vr_enter_menu();
}

/* ------------------------------------------------------------------ events, visibility, drawing */
static void vr_poll(void) {
    double t = now();
    if (t - vr_ev_t >= (vr_view == VIEW_STREAM && t - vr_ptr_t < 1.0 ? 0.008 : 0.033)) {                 /* every overlay-event poll is an IPC round trip to SteamVR: at most ~30 per second */
        vr_ev_t = t;
        struct VREvent_t ev;
        while (ovl->PollNextOverlayEvent(ov_main, &ev, sizeof ev)) {
            switch (ev.eventType) {
            case EVREventType_VREvent_OverlayClosed: fprintf(stderr, "the tab was closed in SteamVR — quitting\n"); running = false; break;
            case EVREventType_VREvent_Quit: case EVREventType_VREvent_ProcessQuit: fprintf(stderr, "SteamVR asked us to quit\n"); running = false; break;
            case EVREventType_VREvent_FocusEnter: kb_focus = true; kb_focus_t = t; break;
            case EVREventType_VREvent_FocusLeave: kb_focus_t = t; kb_focus = false; break;
            case EVREventType_VREvent_MouseMove: vr_mouse_move(ev.data.mouse.x, ev.data.mouse.y); break;
            case EVREventType_VREvent_MouseButtonDown: vr_mouse_button(true, ev.data.mouse.button); break;
            case EVREventType_VREvent_MouseButtonUp: vr_mouse_button(false, ev.data.mouse.button); break;
            case EVREventType_VREvent_KeyboardCharInput:        /* the SteamVR keyboard (control-bar button) types into the Mac */
                if (vr_view == VIEW_STREAM) {
                    const char *c = ev.data.keyboard.cNewInput; int n = (int)strnlen(c, 8);
                    if (n == 1 && (c[0] == 8 || c[0] == 127)) { udp_msg(8, 0x2A, 1, NULL, 0); udp_msg(8, 0x2A, 0, NULL, 0); }       /* backspace */
                    else if (n == 1 && (c[0] == '\n' || c[0] == '\r')) { udp_msg(8, 0x28, 1, NULL, 0); udp_msg(8, 0x28, 0, NULL, 0); }   /* return */
                    else if (n > 0) udp_text(c, n);
                }
                break;
            case EVREventType_VREvent_ScrollDiscrete:
                if (vr_view == VIEW_STREAM) udp_msg(7, (uint32_t)(int32_t)(ev.data.scroll.xdelta * 100), (uint32_t)(int32_t)(ev.data.scroll.ydelta * 100), NULL, 0);
                break;
            default: break;
            }
        }
        if (vr_force_visible) tab_visible = true;
        else if (t - vis_check_t >= 0.25) {
            bool v = ovl->IsOverlayVisible(ov_main);
            if (v != tab_visible) {
                tab_visible = v; if (!quiet) fprintf(stderr, "tab %s\n", v ? "visible" : "hidden");
                if (!v) udp_msg(10, 0, 0, NULL, 0);                                  /* let go of any key/button held on the Mac */
                udp_msg(11, v ? 0 : 1, 0, NULL, 0); last_pause_t = t;          /* tell the Mac to stop/start capturing */
                if (v) { menu_need = true; tb_need = true; }
            }
            vis_check_t = t;
        }
    }
    /* physical keyboards: only while streaming, the tab visible, and the laser on it (1.5 s grace after it leaves) */
    kbd_service(t, vr_view == VIEW_STREAM && tab_visible && (kb_focus || t - kb_focus_t < 1.5), kb_send);
    if (!tab_visible && !vr_force_visible && t - last_pause_t >= 1.0) { udp_msg(11, 1, 0, NULL, 0); last_pause_t = t; }   /* idempotent re-send: survives reconnects and lost packets */
    if (tab_visible || vr_force_visible) hidden_since = 0;
    else if (!hidden_since) hidden_since = t;
    else if (vr_idle_exit_s > 0 && t - hidden_since > vr_idle_exit_s) { fprintf(stderr, "the tab has been hidden for %.0f s — quitting to save power (tap the Sideframe entry to start it again)\n", vr_idle_exit_s); running = false; }
    if (tab_visible || vr_force_visible) {
        if (vr_view == VIEW_MENU) menu_frame(t);
        else if (tb_need && t - menu_draw_t >= 0.05) { tb_need = false; menu_draw_t = t; toolbar_render(); }
    }
}

/* One tab at a time. Starting the app while it is already running CLOSES the running one (so tapping the app entry again is the
 * way to quit it from the Frame). A second start within 3 s of the first (a double tap) is just ignored. */
static char vr_pid_path[PATH_MAX];
static bool vr_toggle_flag = false;   /* --toggle: a deliberate launch from the app entry; only then does a second start close the first */
static void vr_single_instance_toggle(void) {
    char *path = vr_pid_path; const char *rt = getenv("XDG_RUNTIME_DIR");
    snprintf(vr_pid_path, sizeof vr_pid_path, "%s/sideframe.pid", rt ? rt : "/tmp");
    FILE *f = fopen(path, "r");
    if (f) {
        long pid = 0, started = 0;
        if (fscanf(f, "%ld %ld", &pid, &started) == 2 && pid > 1 && pid != getpid()) {
            char comm[64] = {0}, cp[64]; snprintf(cp, sizeof cp, "/proc/%ld/comm", pid);
            FILE *c = fopen(cp, "r"); if (c) { if (fgets(comm, sizeof comm, c)) comm[strcspn(comm, "\n")] = 0; fclose(c); }
            if (!strcmp(comm, "sideframe")) {
                if (!vr_toggle_flag) fprintf(stderr, "Sideframe is already running — leaving it alone\n");
                else if (time(NULL) - started >= 3) { fprintf(stderr, "Sideframe is already running — closing it\n"); kill((pid_t)pid, SIGTERM); }
                else fprintf(stderr, "Sideframe is starting — ignoring the second start\n");
                fclose(f); exit(0);
            }
        }
        fclose(f);
    }
    f = fopen(path, "w"); if (f) { fprintf(f, "%ld %ld\n", (long)getpid(), (long)time(NULL)); fclose(f); }
}

static void vr_init(void) {
    if (!vr_load_api()) exit(1);
    if (!VR_IsRuntimeInstalled()) { fprintf(stderr, "SteamVR runtime isn't installed\n"); exit(1); }
    EVRInitError err = EVRInitError_VRInitError_None;
    VR_InitInternal(&err, EVRApplicationType_VRApplication_Overlay);
    if (err != EVRInitError_VRInitError_None) {
        fprintf(stderr, "can't connect to SteamVR: %s (%d) — SteamVR must be running\n", VR_GetVRInitErrorAsEnglishDescription(err), (int)err);
        exit(3);
    }
    fprintf(stderr, "connected to SteamVR as an overlay app\n");
    ovl = vr_iface(IVROverlay_Version); vapps = vr_iface(IVRApplications_Version); vcomp = vr_iface(IVRCompositor_Version);
    if (!ovl || !vapps || !vcomp) { VR_ShutdownInternal(); exit(1); }
    {   /* how many pixels can this headset actually show? SteamVR's recommended per-eye render width, with 1.2x headroom for the tab's tilt */
        struct VR_IVRSystem_FnTable *vsys = vr_iface(IVRSystem_Version); uint32_t rw = 0, rh = 0;
        if (vsys && vsys->GetRecommendedRenderTargetSize) vsys->GetRecommendedRenderTargetSize(&rw, &rh);
        if (rw >= 1000 && rw <= 8000) vr_auto_cap_w = (int)(rw * 1.2) & ~1;
        fprintf(stderr, "headset render width %u -> the tab texture is capped at %d px wide\n", rw, vr_auto_cap_w);
    }

    char icon[PATH_MAX]; snprintf(icon, sizeof icon, "%s/.local/share/icons/hicolor/256x256/apps/sideframe.png", getenv("HOME") ? getenv("HOME") : "");
    char manifest[PATH_MAX];
    if (write_manifest(manifest, sizeof manifest, icon)) {   /* temporary registration: nothing persists in SteamVR */
        EVRApplicationError ae = vapps->AddApplicationManifest(manifest, true);
        fprintf(stderr, "AddApplicationManifest(temporary) -> %d\n", (int)ae);
        ae = vapps->IdentifyApplication((uint32_t)getpid(), APP_KEY);
        fprintf(stderr, "IdentifyApplication -> %d\n", (int)ae);
        vapps->SetApplicationAutoLaunch(APP_KEY, false);   /* SteamVR must not start a second copy of us */
    }
    EVROverlayError oe = ovl->CreateDashboardOverlay(OVERLAY_KEY, "Sideframe", &ov_main, &ov_thumb);
    if (oe != EVROverlayError_VROverlayError_None) { fprintf(stderr, "CreateDashboardOverlay failed: %s (%d)\n", ovl->GetOverlayErrorNameFromEnum(oe), (int)oe); VR_ShutdownInternal(); exit(1); }
    ovl->SetOverlayWidthInMeters(ov_main, 2.4f);
    ovl->SetOverlayInputMethod(ov_main, VROverlayInputMethod_Mouse);
    ovl->SetOverlayFlag(ov_main, VROverlayFlags_SendVRDiscreteScrollEvents, true);
    /* the dashboard's own control bar under the tab: a Close button that sends us VREvent_OverlayClosed (handled in vr_poll: we quit) */
    ovl->SetOverlayFlag(ov_main, VROverlayFlags_EnableControlBar, true);   /* EnableControlBar */
    ovl->SetOverlayFlag(ov_main, VROverlayFlags_EnableControlBarClose, true);
    ovl->SetOverlayFlag(ov_main, VROverlayFlags_EnableControlBarKeyboard, true);   /* keyboard button: typed text arrives as VREvent_KeyboardCharInput */   /* EnableControlBarClose */
    HmdVector2_t scale = { (float)CARD_W, (float)CARD_H };
    ovl->SetOverlayMouseScale(ov_main, &scale);
    if (access(icon, R_OK) == 0) ovl->SetOverlayFromFile(ov_thumb, icon);
    vr_force_visible = getenv("MD_VR_ALWAYS") != NULL;
    vk_setup();
    ui_buf = malloc((size_t)CARD_W * CARD_H * 4);
    {   char exe[PATH_MAX] = {0}, fdir[PATH_MAX + 32]; ssize_t n = readlink("/proc/self/exe", exe, sizeof exe - 1);
        if (n > 0) { char *sl = strrchr(exe, '/'); if (sl) *sl = 0; snprintf(fdir, sizeof fdir, "%s/assets/fonts", exe); } else fdir[0] = 0;
        ui_load_fonts(fdir);
    }
    menu_init(); disc_open();
    menu_present(now());
    fprintf(stderr, "dashboard tab \"Sideframe\" is ready\n");
}

static void vr_shutdown(void) {
    if (vr_pid_path[0]) { FILE *f = fopen(vr_pid_path, "r"); long p = 0; if (f) { if (fscanf(f, "%ld", &p) == 1 && p == (long)getpid()) { fclose(f); unlink(vr_pid_path); } else fclose(f); } }
    if (vr_conv_n) fprintf(stderr, "NV12->RGBA conversion + hand-off to SteamVR: avg %.2f ms, max %.2f ms over %lu frames\n", vr_conv_sum / vr_conv_n * 1e3, vr_conv_max * 1e3, vr_conv_n);
    if (ovl && ov_main) ovl->DestroyOverlay(ov_main);
    VR_ShutdownInternal();
}
#endif
