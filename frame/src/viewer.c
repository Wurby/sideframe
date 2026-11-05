// Sideframe — Frame side: zero-copy HEVC viewer (window build) and SteamVR dashboard tab (-DMD_VR build).
// HEVC (Annex B file) -> Qualcomm iris V4L2 stateful decoder -> VIDIOC_EXPBUF dmabuf -> Wayland linux-dmabuf.
// Decoded frames are never touched by the CPU.
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <poll.h>
#include <time.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <linux/videodev2.h>
#include <drm_fourcc.h>
#ifndef MD_VR
#include <wayland-client.h>
#endif
#ifdef HAVE_FFMPEG   /* optional: only for playing a raw .h265 file (a test mode). Without it Sideframe needs no ffmpeg at all, so system ffmpeg updates can't break it. */
#include <libavformat/avformat.h>
#else
typedef struct { int codec_id; } AVCodecParameters; typedef struct { AVCodecParameters *codecpar; } AVStream;
typedef struct AVFormatContext { AVStream **streams; } AVFormatContext; typedef struct { uint8_t *data; int size, stream_index; } AVPacket;
#define AVMEDIA_TYPE_VIDEO 0
#define AV_CODEC_ID_HEVC 173
static int avformat_open_input(AVFormatContext **c, const char *p, void *a, void *b) { (void)c; (void)p; (void)a; (void)b; return -1; }
static int avformat_find_stream_info(AVFormatContext *c, void *o) { (void)c; (void)o; return -1; }
static int av_find_best_stream(AVFormatContext *c, int t, int a, int b, void *d, int f) { (void)c; (void)t; (void)a; (void)b; (void)d; (void)f; return -1; }
static int av_read_frame(AVFormatContext *c, AVPacket *p) { (void)c; (void)p; return -1; }
static void avformat_close_input(AVFormatContext **c) { (void)c; }
static AVPacket *av_packet_alloc(void) { return calloc(1, sizeof(AVPacket)); }
static void av_packet_unref(AVPacket *p) { (void)p; }
#endif
#include <signal.h>
#include <netdb.h>
#include <sodium.h>
#include "mdcrypto.h"
#include "pairing_client.h"
#include <math.h>
#include <linux/input-event-codes.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#ifndef MD_VR
#include "xdg-shell-client-protocol.h"
#include "linux-dmabuf-v1-client-protocol.h"
#include "viewporter-client-protocol.h"
#include "xdg-decoration-unstable-v1-client-protocol.h"
#include "cursor-shape-v1-client-protocol.h"
#endif

#define OUT_N 4
#define OUT_SIZE (8u << 20)
#define CAP_MAX 32
#define OUT_T V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE
#define CAP_T V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE
#define ALIGN(x, a) (((x) + (a) - 1) & ~((a) - 1))
#define CHK(x) do { if ((x) < 0) { fprintf(stderr, "%s:%d %s failed: %s\n", __FILE__, __LINE__, #x, strerror(errno)); exit(1); } } while (0)

static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec * 1e-9; }
static int xioctl(int fd, unsigned long r, void *a) { int x; do x = ioctl(fd, r, a); while (x == -1 && errno == EINTR); return x; }

#ifndef MD_VR
/* ---------------- Wayland ---------------- */
static struct wl_display *dpy;
static struct wl_compositor *comp;
static struct xdg_wm_base *wm;
static struct zwp_linux_dmabuf_v1 *dmabuf;
static struct wl_surface *surf;
static struct xdg_toplevel *top;
static struct wl_subcompositor *subc; static struct wp_viewporter *vpr; static struct wl_shm *shm; static struct wl_seat *seat;
static struct zxdg_decoration_manager_v1 *deco_mgr; static struct wp_cursor_shape_manager_v1 *cshape_mgr;
static struct wl_surface *vsurf; static struct wl_subsurface *vsub; static struct wp_viewport *vp_bg, *vp_video;
static struct wl_pointer *ptr; static struct wl_keyboard *kbd; static struct wp_cursor_shape_device_v1 *cshape_dev;
static int win_w = 1280, win_h = 720, vid_x, vid_y, vid_w, vid_h;   /* window + letterboxed video rect, logical px */
static bool is_fullscreen;
static double ptr_x, ptr_y; static bool ptr_in;   /* pointer position in window coords */
static void layout(void);
static bool configured, nv12_linear_ok, nv12_any;
static uint64_t nv12_mod = DRM_FORMAT_MOD_INVALID;

static void dmabuf_format(void *d, struct zwp_linux_dmabuf_v1 *z, uint32_t f) { if (f == DRM_FORMAT_NV12) nv12_any = true; }
static void dmabuf_modifier(void *d, struct zwp_linux_dmabuf_v1 *z, uint32_t f, uint32_t hi, uint32_t lo) {
    if (f != DRM_FORMAT_NV12) return;
    uint64_t m = ((uint64_t)hi << 32) | lo; nv12_any = true;
    fprintf(stderr, "compositor accepts NV12 modifier 0x%016llx%s\n", (unsigned long long)m, m == DRM_FORMAT_MOD_LINEAR ? " (LINEAR)" : "");
    if (m == DRM_FORMAT_MOD_LINEAR) { nv12_linear_ok = true; nv12_mod = m; }
}
static const struct zwp_linux_dmabuf_v1_listener dmabuf_l = { dmabuf_format, dmabuf_modifier };
static void wm_ping(void *d, struct xdg_wm_base *w, uint32_t s) { xdg_wm_base_pong(w, s); }
static const struct xdg_wm_base_listener wm_l = { wm_ping };
/* ---- input: pointer + keyboard (F11 / Esc handled locally; the rest is plumbing for forwarding to the Mac) ---- */
static void seat_setup(void);
static void toggle_fullscreen(void) { if (is_fullscreen) xdg_toplevel_unset_fullscreen(top); else xdg_toplevel_set_fullscreen(top, NULL); }
static void kb_keymap(void *d, struct wl_keyboard *k, uint32_t fmt, int fd, uint32_t sz) { close(fd); }
static void kb_enter(void *d, struct wl_keyboard *k, uint32_t ser, struct wl_surface *s, struct wl_array *keys) {}
static void kb_leave(void *d, struct wl_keyboard *k, uint32_t ser, struct wl_surface *s) {}
static void kb_key(void *d, struct wl_keyboard *k, uint32_t ser, uint32_t t, uint32_t key, uint32_t st) {
    if (st != WL_KEYBOARD_KEY_STATE_PRESSED) return;
    if (key == KEY_F11) toggle_fullscreen();
    else if (key == KEY_ESC && is_fullscreen) xdg_toplevel_unset_fullscreen(top);
}
static void kb_mods(void *d, struct wl_keyboard *k, uint32_t ser, uint32_t a, uint32_t b, uint32_t c, uint32_t g) {}
static void kb_repeat(void *d, struct wl_keyboard *k, int32_t rate, int32_t delay) {}
static const struct wl_keyboard_listener kb_l = { .keymap = kb_keymap, .enter = kb_enter, .leave = kb_leave, .key = kb_key, .modifiers = kb_mods, .repeat_info = kb_repeat };
static void ptr_enter(void *d, struct wl_pointer *p, uint32_t ser, struct wl_surface *s, wl_fixed_t x, wl_fixed_t y) {
    ptr_in = true; ptr_x = wl_fixed_to_double(x) + (s == vsurf ? vid_x : 0); ptr_y = wl_fixed_to_double(y) + (s == vsurf ? vid_y : 0);
    if (cshape_dev) wp_cursor_shape_device_v1_set_shape(cshape_dev, ser, WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_DEFAULT);
}
static void ptr_leave(void *d, struct wl_pointer *p, uint32_t ser, struct wl_surface *s) { ptr_in = false; }
static struct wl_surface *ptr_surf_dummy;
static void ptr_motion(void *d, struct wl_pointer *p, uint32_t t, wl_fixed_t x, wl_fixed_t y) { ptr_x = wl_fixed_to_double(x); ptr_y = wl_fixed_to_double(y); }
static void ptr_button(void *d, struct wl_pointer *p, uint32_t ser, uint32_t t, uint32_t b, uint32_t st) {}
static void ptr_axis(void *d, struct wl_pointer *p, uint32_t t, uint32_t axis, wl_fixed_t v) {}
static void ptr_frame(void *d, struct wl_pointer *p) {}
static void ptr_axis_source(void *d, struct wl_pointer *p, uint32_t s) {}
static void ptr_axis_stop(void *d, struct wl_pointer *p, uint32_t t, uint32_t a) {}
static void ptr_axis_discrete(void *d, struct wl_pointer *p, uint32_t a, int32_t v) {}
static const struct wl_pointer_listener ptr_l = { .enter = ptr_enter, .leave = ptr_leave, .motion = ptr_motion, .button = ptr_button, .axis = ptr_axis,
    .frame = ptr_frame, .axis_source = ptr_axis_source, .axis_stop = ptr_axis_stop, .axis_discrete = ptr_axis_discrete };
static void seat_caps(void *d, struct wl_seat *st, uint32_t caps) {
    if ((caps & WL_SEAT_CAPABILITY_POINTER) && !ptr) {
        ptr = wl_seat_get_pointer(st); wl_pointer_add_listener(ptr, &ptr_l, NULL);
        if (cshape_mgr) cshape_dev = wp_cursor_shape_manager_v1_get_pointer(cshape_mgr, ptr);
    }
    if ((caps & WL_SEAT_CAPABILITY_KEYBOARD) && !kbd) { kbd = wl_seat_get_keyboard(st); wl_keyboard_add_listener(kbd, &kb_l, NULL); }
}
static void seat_name(void *d, struct wl_seat *st, const char *n) {}
static const struct wl_seat_listener seat_l = { seat_caps, seat_name };
static void seat_setup(void) { wl_seat_add_listener(seat, &seat_l, NULL); }

static void reg_global(void *d, struct wl_registry *r, uint32_t id, const char *i, uint32_t v) {
    if (!strcmp(i, "wl_compositor")) comp = wl_registry_bind(r, id, &wl_compositor_interface, v < 4 ? v : 4);
    else if (!strcmp(i, "xdg_wm_base")) { wm = wl_registry_bind(r, id, &xdg_wm_base_interface, 1); xdg_wm_base_add_listener(wm, &wm_l, NULL); }
    else if (!strcmp(i, "wl_subcompositor")) subc = wl_registry_bind(r, id, &wl_subcompositor_interface, 1);
    else if (!strcmp(i, "wp_viewporter")) vpr = wl_registry_bind(r, id, &wp_viewporter_interface, 1);
    else if (!strcmp(i, "wl_shm")) shm = wl_registry_bind(r, id, &wl_shm_interface, 1);
    else if (!strcmp(i, "wl_seat")) { seat = wl_registry_bind(r, id, &wl_seat_interface, v < 5 ? v : 5); seat_setup(); }
    else if (!strcmp(i, "zxdg_decoration_manager_v1")) deco_mgr = wl_registry_bind(r, id, &zxdg_decoration_manager_v1_interface, 1);
    else if (!strcmp(i, "wp_cursor_shape_manager_v1")) cshape_mgr = wl_registry_bind(r, id, &wp_cursor_shape_manager_v1_interface, 1);
    else if (!strcmp(i, "zwp_linux_dmabuf_v1")) { dmabuf = wl_registry_bind(r, id, &zwp_linux_dmabuf_v1_interface, v < 3 ? v : 3); zwp_linux_dmabuf_v1_add_listener(dmabuf, &dmabuf_l, NULL); }
}
static void reg_remove(void *d, struct wl_registry *r, uint32_t id) {}
static const struct wl_registry_listener reg_l = { reg_global, reg_remove };
static volatile bool running = true;
static bool want_rgba; static int rgba_dump; static size_t cap_sizeimage;   /* --rgba / --rgba-dump N: decoder-side RGBA experiment (no Wayland output) */
static bool quiet;   /* --quiet: no per-second stats lines (used by the dashboard launcher so its log stays small) */
static void on_term(int sig) { running = false; }
static void xs_conf(void *d, struct xdg_surface *s, uint32_t serial) { xdg_surface_ack_configure(s, serial); configured = true; layout(); }
static const struct xdg_surface_listener xs_l = { xs_conf };
static void tl_conf(void *d, struct xdg_toplevel *t, int32_t w, int32_t h, struct wl_array *st) {
    if (w > 0 && h > 0) { win_w = w; win_h = h; }
    is_fullscreen = false;
    uint32_t *e; wl_array_for_each(e, st) if (*e == XDG_TOPLEVEL_STATE_FULLSCREEN) is_fullscreen = true;
}
static void tl_close(void *d, struct xdg_toplevel *t) { running = false; }
static const struct xdg_toplevel_listener tl_l = { tl_conf, tl_close, NULL, NULL };

#else  /* MD_VR: SteamVR dashboard-tab backend instead of a Wayland window */
static volatile bool running = true;
static bool quiet;
static bool want_rgba; static int rgba_dump; static size_t cap_sizeimage;   /* decoder RGBA experiment flags (see --rgba-dump) */
static void on_term(int sig) { running = false; }
struct wl_buffer;   /* cbuf below only carries this pointer for the Wayland build */
#include "vr_backend.h"
#endif

/* ---------------- V4L2 ---------------- */
struct obuf { void *map; size_t len; bool free; };
struct cbuf { int fd; struct wl_buffer *wl; bool with_compositor; };
static int vfd = -1;
static struct obuf ob[OUT_N];
static struct cbuf cb[CAP_MAX];
static int ncap, cap_w, cap_h, cap_stride;
static bool cap_ready;
static unsigned long n_dec, n_present, n_rel, n_fed;
static double recv_t[256]; static double proc_sum, proc_max, proc_min = 1e9; static unsigned long proc_n; static int ts_missing;
static double *proc_all; static size_t proc_all_n, proc_all_cap;
static double last_present; static double max_gap; static unsigned gaps100, gaps250, gaps500;
static void request_keyframe(void);
static int ufd = -1; static void udp_msg(uint8_t type, uint32_t seq, uint32_t val, const uint16_t *idx, int n); static void udp_text(const char *utf8, int n);

#ifndef MD_VR
static int vid_dw, vid_dh;
static void layout(void) {
    if (!configured || !vp_bg) return;
    wp_viewport_set_destination(vp_bg, win_w, win_h);
    if (cap_w > 0 && cap_h > 0) {
        double sc = fmin((double)win_w / cap_w, (double)win_h / cap_h);
        vid_w = (int)lround(cap_w * sc); vid_h = (int)lround(cap_h * sc);
        if (vid_w < 1) vid_w = 1;
        if (vid_h < 1) vid_h = 1;
        vid_x = (win_w - vid_w) / 2; vid_y = (win_h - vid_h) / 2;
        wp_viewport_set_destination(vp_video, vid_w, vid_h);
        wl_subsurface_set_position(vsub, vid_x, vid_y);
        if (cap_ready) wl_surface_commit(vsurf);
    }
    wl_surface_commit(surf);
}
static void rel(void *d, struct wl_buffer *b) {
    int i = (int)(intptr_t)d; cb[i].with_compositor = false; n_rel++;
    struct v4l2_plane pl[VIDEO_MAX_PLANES] = {0};
    struct v4l2_buffer q = { .type = CAP_T, .memory = V4L2_MEMORY_MMAP, .index = i, .length = 1, .m.planes = pl };
    if (xioctl(vfd, VIDIOC_QBUF, &q) < 0) fprintf(stderr, "requeue cap %d: %s\n", i, strerror(errno));
}
static const struct wl_buffer_listener buf_l = { rel };

#endif
#include <linux/dma-buf.h>
static void dump_rgba(int idx) {   /* print the pixel at the middle of each of 7 vertical bars (SMPTE-style test clip), one third down */
    void *m = mmap(NULL, cap_sizeimage, PROT_READ, MAP_SHARED, cb[idx].fd, 0);
    if (m == MAP_FAILED) { perror("mmap dmabuf"); return; }
    struct dma_buf_sync sy = { .flags = DMA_BUF_SYNC_START | DMA_BUF_SYNC_READ }; ioctl(cb[idx].fd, DMA_BUF_IOCTL_SYNC, &sy);
    const uint8_t *base = (const uint8_t *)m; size_t nz = 0, first = (size_t)-1;
    for (size_t i = 0; i < cap_sizeimage; i += 4) if (base[i] | base[i+1] | base[i+2] | base[i+3]) { nz++; if (first == (size_t)-1) first = i; }
    fprintf(stderr, "RGBA buffer: sizeimage %zu, bytesperline reported %d, non-zero pixels %zu, first at byte %zd (row %zd, x %zd if stride=%d)\n",
            cap_sizeimage, cap_stride, nz, (ssize_t)first, first == (size_t)-1 ? -1 : (ssize_t)(first / (cap_w * 4 > cap_stride ? cap_w * 4 : cap_stride)), (ssize_t)0, cap_stride);
    for (int pass = 0; pass < 2; pass++) {
        int stride = pass == 0 ? cap_stride : cap_w * 4 + (cap_stride < cap_w * 4 ? 0 : 0);
        if (pass == 1) stride = 3456 * 4;
        fprintf(stderr, "samples with stride %d (R,G,B,A) at y=%d:", stride, cap_h / 3);
        for (int i = 0; i < 7; i++) {
            int x = (int)((i + 0.5) * cap_w / 7); const uint8_t *q = base + (size_t)(cap_h / 3) * stride + (size_t)x * 4;
            if ((size_t)(q - base) + 4 > cap_sizeimage) { fprintf(stderr, "  [out of range]"); continue; }
            fprintf(stderr, "  [%d,%d,%d,%d]", q[0], q[1], q[2], q[3]);
        }
        fprintf(stderr, "\n");
    }
    sy.flags = DMA_BUF_SYNC_END | DMA_BUF_SYNC_READ; ioctl(cb[idx].fd, DMA_BUF_IOCTL_SYNC, &sy);
    munmap(m, cap_sizeimage);
}
static void setup_capture(void) {
    struct v4l2_format f = { .type = CAP_T };
    CHK(xioctl(vfd, VIDIOC_G_FMT, &f));
    f.fmt.pix_mp.pixelformat = want_rgba ? V4L2_PIX_FMT_RGBA32 : V4L2_PIX_FMT_NV12;
    CHK(xioctl(vfd, VIDIOC_S_FMT, &f));
    struct v4l2_pix_format_mplane *p = &f.fmt.pix_mp;
    if (want_rgba && p->pixelformat != V4L2_PIX_FMT_RGBA32) fprintf(stderr, "WARNING: decoder refused RGBA, gave %.4s\n", (char *)&p->pixelformat);
    cap_sizeimage = p->plane_fmt[0].sizeimage;
    cap_w = p->width; cap_h = p->height; cap_stride = p->plane_fmt[0].bytesperline;
    struct v4l2_selection sel = { .type = CAP_T, .target = V4L2_SEL_TGT_COMPOSE };
    if (xioctl(vfd, VIDIOC_G_SELECTION, &sel) == 0) { cap_w = sel.r.width; cap_h = sel.r.height; }
    uint32_t ysc = ALIGN(p->height, 32), uvsc = ALIGN(p->height / 2, 16);
    uint32_t uv_off = cap_stride * ysc;
    fprintf(stderr, "capture: coded %ux%u visible %dx%d stride %d planes %u sizeimage %u (expect ~%u) uv_off %u pixfmt %.4s\n",
            p->width, p->height, cap_w, cap_h, cap_stride, p->num_planes, p->plane_fmt[0].sizeimage,
            cap_stride * (ysc + uvsc), uv_off, (char *)&p->pixelformat);
    struct v4l2_control c = { .id = V4L2_CID_MIN_BUFFERS_FOR_CAPTURE };
    int minb = xioctl(vfd, VIDIOC_G_CTRL, &c) == 0 ? c.value : 4;
    struct v4l2_requestbuffers rq = { .type = CAP_T, .memory = V4L2_MEMORY_MMAP, .count = minb + 4 };
    CHK(xioctl(vfd, VIDIOC_REQBUFS, &rq));
    ncap = rq.count > CAP_MAX ? CAP_MAX : rq.count;
    fprintf(stderr, "capture buffers: %d (min %d)\n", ncap, minb);
#ifdef MD_VR
    vr_setup_output(cap_w, cap_h);
#endif
    for (int i = 0; i < ncap; i++) {
        struct v4l2_plane pl[VIDEO_MAX_PLANES] = {0};
        struct v4l2_buffer b = { .type = CAP_T, .memory = V4L2_MEMORY_MMAP, .index = i, .length = 1, .m.planes = pl };
        CHK(xioctl(vfd, VIDIOC_QUERYBUF, &b));
        struct v4l2_exportbuffer e = { .type = CAP_T, .index = i, .plane = 0, .flags = O_RDWR | O_CLOEXEC };
        CHK(xioctl(vfd, VIDIOC_EXPBUF, &e));
        cb[i].fd = e.fd;
        if (want_rgba) { cb[i].wl = NULL; CHK(xioctl(vfd, VIDIOC_QBUF, &b)); continue; }   /* experiment: no Wayland import */
#ifdef MD_VR
        vr_import_buffer(i, e.fd, cap_w, cap_h, cap_stride, uv_off);
#else
        struct zwp_linux_buffer_params_v1 *pp = zwp_linux_dmabuf_v1_create_params(dmabuf);
        zwp_linux_buffer_params_v1_add(pp, e.fd, 0, 0, cap_stride, nv12_mod >> 32, nv12_mod & 0xffffffff);
        zwp_linux_buffer_params_v1_add(pp, e.fd, 1, uv_off, cap_stride, nv12_mod >> 32, nv12_mod & 0xffffffff);
        cb[i].wl = zwp_linux_buffer_params_v1_create_immed(pp, cap_w, cap_h, DRM_FORMAT_NV12, 0);
        zwp_linux_buffer_params_v1_destroy(pp);
        wl_buffer_add_listener(cb[i].wl, &buf_l, (void *)(intptr_t)i);
#endif
        CHK(xioctl(vfd, VIDIOC_QBUF, &b));
    }
#ifndef MD_VR
    wl_display_roundtrip(dpy);   // surfaces any dmabuf import error now
#endif
    enum v4l2_buf_type t = CAP_T;
    CHK(xioctl(vfd, VIDIOC_STREAMON, &t));
    cap_ready = true;
#ifndef MD_VR
    if (!is_fullscreen) { win_w = 1280; win_h = (int)lround(1280.0 * cap_h / cap_w); }   // initial window: video aspect, 1280 wide
    layout();
#endif
}

static void open_decoder(void) {
    vfd = open("/dev/video-dec0", O_RDWR | O_NONBLOCK | O_CLOEXEC);
    if (vfd < 0) { perror("open /dev/video-dec0"); exit(1); }
    struct v4l2_format f = { .type = OUT_T };
    f.fmt.pix_mp.pixelformat = V4L2_PIX_FMT_HEVC; f.fmt.pix_mp.width = 1920; f.fmt.pix_mp.height = 1080;
    f.fmt.pix_mp.num_planes = 1; f.fmt.pix_mp.plane_fmt[0].sizeimage = OUT_SIZE;
    CHK(xioctl(vfd, VIDIOC_S_FMT, &f));
    struct v4l2_event_subscription s = { .type = V4L2_EVENT_SOURCE_CHANGE };
    CHK(xioctl(vfd, VIDIOC_SUBSCRIBE_EVENT, &s));
    struct v4l2_requestbuffers rq = { .type = OUT_T, .memory = V4L2_MEMORY_MMAP, .count = OUT_N };
    CHK(xioctl(vfd, VIDIOC_REQBUFS, &rq));
    for (int i = 0; i < OUT_N; i++) {
        struct v4l2_plane pl[VIDEO_MAX_PLANES] = {0};
        struct v4l2_buffer b = { .type = OUT_T, .memory = V4L2_MEMORY_MMAP, .index = i, .length = 1, .m.planes = pl };
        CHK(xioctl(vfd, VIDIOC_QUERYBUF, &b));
        ob[i].len = pl[0].length;
        ob[i].map = mmap(NULL, pl[0].length, PROT_READ | PROT_WRITE, MAP_SHARED, vfd, pl[0].m.mem_offset);
        if (ob[i].map == MAP_FAILED) { perror("mmap"); exit(1); }
        ob[i].free = true;
    }
    enum v4l2_buf_type t = OUT_T;
    CHK(xioctl(vfd, VIDIOC_STREAMON, &t));
}

static uint32_t net_seq; static double net_recv_t; static bool net_closed;

/* ---------------- UDP source: fragments + NACK repair + keyframe resync ----------------
 * Everything below rides inside the encrypted session (see "security" block); this describes the INNER plaintext.
 * Mac -> Frame packet: [u32 seq][u16 idx][u16 cnt][u8 flags(1=key,2=retx)][u8 0][u16 len][u32 total] + payload (<=1350)
 * Frame -> Mac msg:    [u8 type][u8 0][u16 n][u32 seq][u32 val] (+ n * u16 for NACK)
 *   type 1 = ack (seq = highest consumed, val = recv->present us, or 0xFFFFFFFF = "skipped, no sample")
 *   type 2 = NACK (seq, n fragment indices; n = 0xFFFF means "whole frame")   type 3 = keyframe request
 * Strategy: repair small losses with NACK; if a frame is not complete after U_DEADLINE, drop everything
 * and wait for the next keyframe (requested immediately). */
#define UPAY 1350   /* must match the Mac sender */
#define URING 128
#define U_NACK_EVERY 0.010
#define U_DEADLINE 0.100
struct ufr { bool used; uint32_t seq; uint16_t cnt, got; uint32_t total; uint8_t key; uint8_t *buf; size_t cap; uint8_t have[8192]; double t_first, t_nack; };
static double u_srtt = 0.03; static unsigned u_rtt_n; static double u_ping_t;   /* smoothed NACK->retransmit round trip */
static double nack_gap(void) { double g = 1.3 * u_srtt; return g < U_NACK_EVERY ? U_NACK_EVERY : g; }
static double deadline(void) { double d = 3 * u_srtt; return d < U_DEADLINE ? U_DEADLINE : (d > 0.4 ? 0.4 : d); }
static struct ufr ur[URING];
static uint32_t u_expected, u_high; static bool u_synced, u_any;
static int u_release = -1; static double u_gap_t, u_nack_t, u_key_t;
static unsigned long u_pk, u_retx_rx, u_nack_tx, u_resync, u_dup, u_skipped;
/* ---- connection state machine: HELLO until WELCOME, then session; fall back to HELLO if the Mac goes silent ---- */
static struct sockaddr_in hello_addr, peer_addr;   /* hello_addr: what the user typed; peer_addr: where the Mac actually answers from (it may be multi-homed) */
static bool net_up; static uint8_t cn[16]; static double hello_t, last_rx; static unsigned long u_auth_fail;
static void udp_msg(uint8_t type, uint32_t seq, uint32_t val, const uint16_t *idx, int n) {
    if (!net_up) return;
    uint8_t m[12 + 2 * 200], out[9 + 12 + 2 * 200 + 16]; int cnt = (idx && n > 200) ? 200 : n;
    m[0] = type; m[1] = 0; m[2] = (n >> 8) & 255; m[3] = n & 255;
    if (n == 0xFFFF) cnt = 0;
    uint32_t v[2] = { htonl(seq), htonl(val) }; memcpy(m + 4, v, 8);
    for (int i = 0; i < cnt; i++) { m[12 + 2 * i] = idx[i] >> 8; m[13 + 2 * i] = idx[i] & 255; }
    if (idx && n != 0xFFFF) { m[2] = (cnt >> 8) & 255; m[3] = cnt & 255; }
    size_t on = sess_seal(&S, m, 12 + 2 * cnt, out, UINT64_MAX);
    sendto(ufd, out, on, MSG_DONTWAIT, (void *)&peer_addr, sizeof peer_addr);
}
/* typed text (UTF-8, up to 64 bytes) for the Mac to insert at the cursor */
static void udp_text(const char *utf8, int n) {
    if (!net_up || n <= 0 || n > 64) return;
    uint8_t m[12 + 64], out[9 + 12 + 64 + 16]; memset(m, 0, 12); m[0] = 9; m[2] = (n >> 8) & 255; m[3] = n & 255; memcpy(m + 12, utf8, n);
    size_t on = sess_seal(&S, m, 12 + n, out, UINT64_MAX);
    sendto(ufd, out, on, MSG_DONTWAIT, (void *)&peer_addr, sizeof peer_addr);
}
static void request_keyframe(void) { udp_msg(3, 0, 0, NULL, 0); }
static void udp_free_all(void);
static bool udp_connect(const char *host, int port) {
    struct addrinfo hints = { .ai_family = AF_INET, .ai_socktype = SOCK_DGRAM }, *res = NULL; char ps[16]; snprintf(ps, sizeof ps, "%d", port);
    if (getaddrinfo(host, ps, &hints, &res) != 0 || !res) { fprintf(stderr, "cannot resolve %s\n", host); return false; }
    if (ufd >= 0) close(ufd);
    ufd = socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK, 0);
    int sz = 8 << 20; setsockopt(ufd, SOL_SOCKET, SO_RCVBUF, &sz, sizeof sz);
    memcpy(&hello_addr, res->ai_addr, sizeof hello_addr); peer_addr = hello_addr;   /* unconnected socket: authenticated replies may come from another address of the Mac */
    freeaddrinfo(res);
    randombytes_buf(cn, sizeof cn);
    net_up = false; hello_t = 0; last_rx = 0; u_synced = false; u_any = false; udp_free_all();
    fprintf(stderr, "connecting to %s:%d (pairing key loaded) ...\n", host, port);
    return true;
}
static void net_hello(void) {
    uint8_t p[41]; p[0] = 1; memcpy(p + 1, psk_kid, 8); memcpy(p + 9, cn, 16); hmac16(p + 25, "sideframe-hello", psk_kid, 8, cn, 16);
    sendto(ufd, p, sizeof p, MSG_DONTWAIT, (void *)&hello_addr, sizeof hello_addr);
}
static void udp_free_all(void) { for (int i = 0; i < URING; i++) ur[i].used = false; u_release = -1; }
static void net_session_up(const uint8_t sn[16]) {
    sess_derive(&S, cn, sn, false); net_up = true; last_rx = now();
    udp_free_all(); u_synced = false; u_any = false; u_gap_t = 0; u_key_t = now();
    fprintf(stderr, "authenticated with the Mac — encrypted session up\n");
    udp_msg(3, 0, 0, NULL, 0);   /* first DATA packet: proves we completed the handshake, and asks for a keyframe */
}
static void udp_resync(uint32_t consumed_through) {
    udp_free_all(); u_synced = false; u_gap_t = 0; u_resync++;
    udp_msg(1, consumed_through, 0xFFFFFFFFu, NULL, 0);     // let the sender's window drain
    udp_msg(3, 0, 0, NULL, 0); u_key_t = now();
}
static int udp_next(uint8_t **d, size_t *n) {
    if (u_release >= 0) { ur[u_release].used = false; u_release = -1; }
    { /* connection management */
        double tn = now();
        if (!net_up) { if (tn - hello_t > 0.5) { net_hello(); hello_t = tn; } }
        else if (tn - last_rx > 3.0) {
            net_up = false; hello_t = 0; randombytes_buf(cn, sizeof cn);
            udp_free_all(); u_synced = false; u_any = false;
            fprintf(stderr, "the Mac went quiet — reconnecting\n");
        }
    }
    uint8_t raw[1600], pkt[1600];
    for (;;) {
        struct sockaddr_in from; socklen_t fl = sizeof from;
        ssize_t rr = recvfrom(ufd, raw, sizeof raw, 0, (void *)&from, &fl);
        if (rr < 0) break;
        if (rr == 33 && raw[0] == 2 && !net_up) {   /* WELCOME */
            uint8_t tag[16]; hmac16(tag, "sideframe-welcome", cn, 16, raw + 1, 16);
            if (sodium_memcmp(tag, raw + 17, 16) == 0) { peer_addr = from; net_session_up(raw + 1); } else u_auth_fail++;
            continue;
        }
        size_t plen = 0;
        if (!net_up || raw[0] != 3 || !sess_open(&S, raw, (size_t)rr, pkt, &plen)) { u_auth_fail++; continue; }   /* forged / corrupted / replayed */
        last_rx = now(); peer_addr = from;   /* follow the Mac (only reached after authentication + replay check) */
        ssize_t r = (ssize_t)plen;
        if (r == 12 && pkt[0] == 4) {   // echo of our ping: real round-trip time
            uint32_t v = ((uint32_t)pkt[8] << 24) | (pkt[9] << 16) | (pkt[10] << 8) | pkt[11];
            double rtt = (uint32_t)((uint32_t)(uint64_t)(now() * 1e6) - v) / 1e6;
            if (rtt >= 0 && rtt < 2.0) { u_srtt = u_rtt_n++ ? 0.8 * u_srtt + 0.2 * rtt : rtt; }
            continue;
        }
        if (r < 16) continue;
        uint32_t seq = ((uint32_t)pkt[0] << 24) | (pkt[1] << 16) | (pkt[2] << 8) | pkt[3];
        uint16_t idx = (pkt[4] << 8) | pkt[5], cnt = (pkt[6] << 8) | pkt[7], len = (pkt[10] << 8) | pkt[11];
        uint8_t flags = pkt[8]; uint32_t total = ((uint32_t)pkt[12] << 24) | (pkt[13] << 16) | (pkt[14] << 8) | pkt[15];
        if (len != r - 16 || cnt == 0 || idx >= cnt || cnt > 8192 || total > OUT_SIZE || len > UPAY) continue;
        u_pk++;
        if (u_synced && (int32_t)(seq - u_expected) < 0) { u_dup++; continue; }
        struct ufr *f = &ur[seq % URING];
        if (f->used && f->seq != seq) { if ((int32_t)(f->seq - seq) > 0) continue; f->used = false; }
        if (!f->used) {
            f->used = true; f->seq = seq; f->cnt = cnt; f->got = 0; f->total = total; f->key = flags & 1; f->t_first = now(); f->t_nack = 0;
            if (f->cap < (size_t)cnt * UPAY) { f->buf = realloc(f->buf, (size_t)cnt * UPAY); f->cap = (size_t)cnt * UPAY; }
            memset(f->have, 0, cnt);
        }
        if (cnt != f->cnt || f->have[idx]) { u_dup++; continue; }
        memcpy(f->buf + (size_t)idx * UPAY, pkt + 16, len); f->have[idx] = 1; f->got++;
        if (flags & 2) u_retx_rx++;
        if (!u_any || (int32_t)(seq - u_high) > 0) { u_high = seq; u_any = true; }
    }
    double t = now();
    if (!net_up) return 0;
    if (t - u_ping_t > 0.25) { udp_msg(4, 0, (uint32_t)(uint64_t)(t * 1e6), NULL, 0); u_ping_t = t; }
    if (!u_synced) {   // wait for the first COMPLETE keyframe; start decoding there
        int best = -1;
        for (int i = 0; i < URING; i++) if (ur[i].used && ur[i].key && ur[i].got == ur[i].cnt && (best < 0 || (int32_t)(ur[i].seq - ur[best].seq) < 0)) best = i;
        if (best >= 0) {
            u_expected = ur[best].seq; u_synced = true; u_gap_t = 0;
            for (int i = 0; i < URING; i++) if (ur[i].used && (int32_t)(ur[i].seq - u_expected) < 0) ur[i].used = false;
        } else {
            // repair keyframes that are partly here (otherwise a 70-packet keyframe at 3% loss almost never arrives intact)
            for (int i = 0; i < URING; i++) {
                struct ufr *g = &ur[i];
                if (!g->used || !g->key || g->got == g->cnt || t - g->t_first < 0.012 || t - g->t_nack < nack_gap()) continue;
                uint16_t miss[200]; int m = 0;
                for (int k = 0; k < g->cnt && m < 200; k++) if (!g->have[k]) miss[m++] = k;
                g->t_nack = t; udp_msg(2, g->seq, 0, miss, m); u_nack_tx++;
            }
            if (u_any && t - u_key_t > 0.6) { udp_msg(3, 0, 0, NULL, 0); u_key_t = t; }   // keep asking, but not in a flood
            return 0;
        }
    }
    struct ufr *f = &ur[u_expected % URING];
    if (f->used && f->seq == u_expected && f->got == f->cnt) {
        *d = f->buf; *n = f->total; net_seq = f->seq; net_recv_t = t; u_release = u_expected % URING;
        u_expected++; u_gap_t = 0; return 1;
    }
    // is something wrong with u_expected?
    bool later = false;
    for (int i = 0; i < URING && !later; i++) if (ur[i].used && (int32_t)(ur[i].seq - u_expected) > 0) later = true;
    bool present = f->used && f->seq == u_expected;
    bool lossy = (present && t - f->t_first > 0.012) || (!present && later);
    if (!lossy) { u_gap_t = 0; return 0; }
    if (u_gap_t == 0) u_gap_t = t;
    if (t - u_gap_t > deadline() || (int32_t)(u_high - u_expected) > 40) {
        fprintf(stderr, "UDP: frame %u unrecoverable after %.0f ms -> resync at next keyframe\n", u_expected, (t - u_gap_t) * 1e3);
        udp_resync(u_high); return 0;
    }
    for (uint32_t s = u_expected; s != u_expected + 4; s++) {
        struct ufr *g = &ur[s % URING];
        if (g->used && g->seq == s) {
            if (g->got == g->cnt || t - g->t_first < 0.012 || t - g->t_nack < nack_gap()) continue;   // age check: frames still arriving are not lost
            uint16_t miss[200]; int m = 0;
            for (int i = 0; i < g->cnt && m < 200; i++) if (!g->have[i]) miss[m++] = i;
            g->t_nack = t; udp_msg(2, s, 0, miss, m); u_nack_tx++;
        } else if (s == u_expected && t - u_nack_t >= nack_gap()) { u_nack_t = t; udp_msg(2, s, 0, NULL, 0xFFFF); u_nack_tx++; }   // nothing of it arrived
    }
    return 0;
}
static void send_ack(uint32_t seq, uint32_t us) {
    udp_msg(1, seq, us, NULL, 0);
}

static bool net_is_up(void) { return net_up; }
/* hand a capture buffer back to the decoder */
static void requeue_capture(int idx) {
    struct v4l2_plane pl[VIDEO_MAX_PLANES] = {0};
    struct v4l2_buffer b = { .type = CAP_T, .memory = V4L2_MEMORY_MMAP, .index = idx, .length = 1, .m.planes = pl };
    if (xioctl(vfd, VIDIOC_QBUF, &b) < 0) fprintf(stderr, "requeue cap %d: %s\n", idx, strerror(errno));
}
/* true while the network side needs fine-grained timers (waiting for a keyframe, or a frame is incomplete and being repaired) */
static bool udp_busy(void) {
    if (u_gap_t != 0) return true;                                   /* a frame is overdue: NACK timers need ~ms resolution */
    for (int i = 0; i < URING; i++) if (ur[i].used && ur[i].got != ur[i].cnt) return true;   /* a frame is still arriving */
    return false;                                                     /* nothing in flight: sleep until a packet arrives */
}

/* ---------------- session lifecycle: start a stream from the menu, stop it again ---------------- */
static void decoder_close(void) {
    if (vfd < 0) return;
    enum v4l2_buf_type t = CAP_T; xioctl(vfd, VIDIOC_STREAMOFF, &t); t = OUT_T; xioctl(vfd, VIDIOC_STREAMOFF, &t);
    for (int i = 0; i < OUT_N; i++) if (ob[i].map && ob[i].map != MAP_FAILED) { munmap(ob[i].map, ob[i].len); ob[i].map = NULL; }
    for (int i = 0; i < ncap; i++) if (cb[i].fd > 0) { close(cb[i].fd); cb[i].fd = -1; }
    struct v4l2_requestbuffers rq = { .type = CAP_T, .memory = V4L2_MEMORY_MMAP, .count = 0 }; xioctl(vfd, VIDIOC_REQBUFS, &rq);
    rq.type = OUT_T; xioctl(vfd, VIDIOC_REQBUFS, &rq);
    close(vfd); vfd = -1; cap_ready = false; ncap = 0;
}
static bool session_start(const struct mac_conf *mc, char *err, size_t en) {
    if (access("/dev/video-dec0", R_OK | W_OK) != 0) { snprintf(err, en, "This Frame’s video decoder isn’t available. Restart the Frame and try again."); return false; }
    set_psk(mc->key);
    if (!udp_connect(mc->host, mc->port)) { snprintf(err, en, "Can’t find “%s” on the network.", mc->name); return false; }
    open_decoder();
    cap_ready = false; ncap = 0; udp_free_all(); u_synced = false; u_any = false;
    return true;
}
static void session_stop(void) {
    if (net_up) for (int i = 0; i < 3; i++) udp_msg(13, 0, 0, NULL, 0);        /* goodbye: the Mac ends the session right away */
#ifdef MD_VR
    vr_teardown_output();                                                          /* GPU objects first: they reference the decoder's buffers */
#endif
    decoder_close();
    if (ufd >= 0) { close(ufd); ufd = -1; }
    net_up = false; u_synced = false; u_any = false; udp_free_all();
}

/* ---------------- main ---------------- */
int main(int argc, char **argv) {
#ifdef MD_VR
    bool allow_multiple = false;
#endif
    const char *path = NULL; bool loop = false; double secs = 0, pace_fps = 60; const char *conn_host = NULL; int conn_port = 5000; const char *keyfile = NULL; const char *pair_host = NULL, *pair_code = NULL, *frame_name = NULL; bool list_macs = false;   /* pace_fps: file playback is capped (default 60) so test runs cannot cook the Frame; 0 = unlimited */
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--loop")) loop = true;
        else if (!strcmp(argv[i], "--seconds") && i + 1 < argc) secs = atof(argv[++i]);
        else if (!strcmp(argv[i], "--connect") && i + 1 < argc) {   /* HOST[:PORT] */
            conn_host = argv[++i]; char *c = strrchr(argv[i], ':'); if (c) { *c = 0; conn_port = atoi(c + 1); }
        }
        else if (!strcmp(argv[i], "--key") && i + 1 < argc) keyfile = argv[++i];
        else if (!strcmp(argv[i], "--pair") && i + 1 < argc) { pair_host = argv[++i]; char *c = strrchr(argv[i], ':'); if (c) { *c = 0; conn_port = atoi(c + 1); } }
        else if (!strcmp(argv[i], "--code") && i + 1 < argc) pair_code = argv[++i];
        else if (!strcmp(argv[i], "--name") && i + 1 < argc) frame_name = argv[++i];
        else if (!strcmp(argv[i], "--list-macs")) list_macs = true;
        else if (!strcmp(argv[i], "--quiet")) quiet = true;
#ifdef MD_VR
        else if (!strcmp(argv[i], "--max-fps") && i + 1 < argc) { double f = atof(argv[++i]); if (f >= 1 && f <= 120) vr_min_interval = 1.0 / f; }
        else if (!strcmp(argv[i], "--idle-exit") && i + 1 < argc) vr_idle_exit_s = atof(argv[++i]);
        else if (!strcmp(argv[i], "--scale") && i + 1 < argc) { double f = atof(argv[++i]); if (f >= 0.25 && f <= 1.0) vr_out_scale = f; }
        else if (!strcmp(argv[i], "--allow-multiple")) allow_multiple = true;
        else if (!strcmp(argv[i], "--toggle")) vr_toggle_flag = true;
#endif
        else if (!strcmp(argv[i], "--rgba")) want_rgba = true;
        else if (!strcmp(argv[i], "--rgba-dump") && i + 1 < argc) { want_rgba = true; rgba_dump = atoi(argv[++i]); }
        else if (!strcmp(argv[i], "--selftest")) { if (sodium_init() < 0) return 1; return crypto_selftest(); }
        else if (!strcmp(argv[i], "--fps") && i + 1 < argc) pace_fps = atof(argv[++i]);
        else path = argv[i];
    }
    if (sodium_init() < 0) { fprintf(stderr, "libsodium init failed\n"); return 1; }
    if (list_macs) {
        struct mac_conf all[16]; int n = mac_conf_list(all, 16);
        if (!n) printf("No paired Macs yet. Pair with:  --pair HOST --code NNNNNNNN\n");
        for (int i = 0; i < n; i++) printf("%s  (%s)  id %s  %s:%d\n", all[i].name, all[i].path, all[i].id, all[i].host, all[i].port);
        return 0;
    }
    if (pair_host) {   /* one-shot pairing: no display needed */
        char hn[64] = "Steam Frame", nm[96]; if (!frame_name) { gethostname(hn, sizeof hn - 1); snprintf(nm, sizeof nm, "Steam Frame (%s)", hn); frame_name = nm; }
        if (!pair_code) { fprintf(stderr, "usage: --pair HOST[:PORT] --code NNNNNNNN [--name NAME]\n"); return 2; }
        struct mac_conf mc; char err[256] = {0};
        fprintf(stderr, "pairing with %s … (the key derivation takes a moment)\n", pair_host);
        int rc = pair_run(pair_host, conn_port, pair_code, frame_name, &mc, err, sizeof err);
        if (rc != 0) { fprintf(stderr, "Pairing failed: %s\n", err); return 1; }
        if (!mac_conf_save(&mc)) { fprintf(stderr, "Paired, but couldn't save the key under ~/.config/sideframe/macs/\n"); return 1; }
        printf("Paired with \"%s\" (id %s). Saved to ~/.config/sideframe/macs/%s.conf\n", mc.name, mc.id, mc.id);
        return 0;
    }
#ifdef MD_VR
    bool menu_mode = !path && !conn_host;   /* no file and no --connect: the app shows its menu */
#else
    if (!path && !conn_host) {   /* no arguments: use the one paired Mac, if any */
        struct mac_conf mc; if (mac_conf_pick(NULL, &mc) && mc.host[0]) { conn_host = strdup(mc.host); conn_port = mc.port; }
    }
#endif
    bool netmode = conn_host != NULL
#ifdef MD_VR
                   || menu_mode
#endif
                   ;
    if (!path && !netmode) { fprintf(stderr, "usage: viewer <file.h265> [--loop] [--seconds N] [--fps N (default 60, 0=unlimited)]\n       viewer --connect HOST[:PORT] [--key FILE] [--seconds N]   (key default: ~/.config/mac-display/psk)\n       viewer --selftest\n"); return 2; }

    signal(SIGTERM, on_term); signal(SIGINT, on_term);
#ifndef MD_VR
    dpy = wl_display_connect(NULL);
    if (!dpy) { fprintf(stderr, "cannot connect to Wayland\n"); return 1; }
    struct wl_registry *reg = wl_display_get_registry(dpy);
    wl_registry_add_listener(reg, &reg_l, NULL);
    wl_display_roundtrip(dpy); wl_display_roundtrip(dpy);
    if (!comp || !wm || !dmabuf) { fprintf(stderr, "missing wayland globals (comp=%p wm=%p dmabuf=%p)\n", (void*)comp, (void*)wm, (void*)dmabuf); return 1; }
    if (!nv12_any) { fprintf(stderr, "compositor does not advertise NV12 dmabuf\n"); return 1; }
    if (!nv12_linear_ok) fprintf(stderr, "note: LINEAR not advertised for NV12; trying INVALID modifier\n");
    if (!subc || !vpr || !shm) { fprintf(stderr, "missing wayland globals (subcompositor=%p viewporter=%p shm=%p)\n", (void*)subc, (void*)vpr, (void*)shm); return 1; }
    surf = wl_compositor_create_surface(comp);
    vsurf = wl_compositor_create_surface(comp);
    vsub = wl_subcompositor_get_subsurface(subc, vsurf, surf);
    wl_subsurface_set_desync(vsub);                      // video commits independently of the (static) background
    vp_bg = wp_viewporter_get_viewport(vpr, surf);
    vp_video = wp_viewporter_get_viewport(vpr, vsurf);
    struct xdg_surface *xs = xdg_wm_base_get_xdg_surface(wm, surf);
    xdg_surface_add_listener(xs, &xs_l, NULL);
    top = xdg_surface_get_toplevel(xs);
    xdg_toplevel_add_listener(top, &tl_l, NULL);
    xdg_toplevel_set_title(top, "Mac Display (viewer prototype)");
    xdg_toplevel_set_app_id(top, "sideframe-window");
    xdg_toplevel_set_min_size(top, 320, 180);
    if (deco_mgr) zxdg_toplevel_decoration_v1_set_mode(zxdg_decoration_manager_v1_get_toplevel_decoration(deco_mgr, top), ZXDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);
    wl_surface_commit(surf);
    while (!configured) wl_display_dispatch(dpy);
    {   // black 1x1 background, scaled to the window by the viewport (letterbox bars)
        int mfd = memfd_create("bg", MFD_CLOEXEC); if (ftruncate(mfd, 4) < 0) perror("ftruncate");
        struct wl_shm_pool *pool = wl_shm_create_pool(shm, mfd, 4);
        struct wl_buffer *black = wl_shm_pool_create_buffer(pool, 0, 1, 1, 4, WL_SHM_FORMAT_XRGB8888);
        wl_surface_attach(surf, black, 0, 0); wl_shm_pool_destroy(pool); close(mfd);
        layout();
    }

#else
    if (!allow_multiple && netmode) vr_single_instance_toggle();   /* tapping the app entry again closes the running one */
    vr_init();
#endif

    AVFormatContext *fc = NULL; int si = -1;
    if (netmode
#ifdef MD_VR
        && !menu_mode
#endif
        ) {
        if (sodium_init() < 0) { fprintf(stderr, "libsodium init failed\n"); return 1; }
        struct mac_conf mc;
        bool have = keyfile ? mac_conf_load(keyfile, &mc) : mac_conf_pick(conn_host, &mc);
        if (!have) { fprintf(stderr, "this Frame isn't paired with a Mac yet — open \"Pair a Steam Frame…\" in the Sideframe menu on the Mac, then run:\n  --pair %s --code NNNNNNNN\n", conn_host); return 1; }
        set_psk(mc.key);
#ifdef MD_VR
        M.target = mc; M.scr = SCR_CONNECTING; M.connect_t0 = now();
#endif
        fprintf(stderr, "using the pairing with \"%s\"\n", mc.name);
        if (!udp_connect(conn_host, conn_port)) return 1;
    } else
#ifdef MD_VR
    if (!menu_mode)
#endif
    {
        if (avformat_open_input(&fc, path, NULL, NULL) < 0) { fprintf(stderr, "cannot open %s (playing files needs a build with HAVE_FFMPEG=1)\n", path); return 1; }
        avformat_find_stream_info(fc, NULL);
        si = av_find_best_stream(fc, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);
        if (si < 0 || fc->streams[si]->codecpar->codec_id != AV_CODEC_ID_HEVC) { fprintf(stderr, "not an HEVC stream\n"); return 1; }
    }
#ifdef MD_VR
    if (!menu_mode) open_decoder();
    bool streaming = !menu_mode; double last_net_up = now(), net_up_since = 0;
#else
    open_decoder();
#endif

    AVPacket *pkt = av_packet_alloc(); uint8_t *au_d = NULL; size_t au_n = 0; bool have_pkt = false, eof = false, stop_sent = false, drained = false;
    double t0 = now(), tlast = t0; unsigned long ld = 0;

    while (running && !drained) {
#ifdef MD_VR
        if (menu_mode) {
            vr_poll();                                       /* SteamVR events, visibility, menu / toolbar redraw */
            double tn = now();
            if (net_is_up()) { last_net_up = tn; if (!net_up_since) net_up_since = tn; } else net_up_since = 0;
            /* ---- test script (MD_TEST_SCRIPT="2:connect,14:menu,18:connect,30:close"): drives the same actions a click would ---- */
            {   static const char *scr; static double ts0; static int done_upto = -1;
                if (!ts0) { scr = getenv("MD_TEST_SCRIPT"); ts0 = tn; }
                if (scr) { int idx = 0; const char *q = scr;
                    while (*q) { double at = atof(q); const char *colon = strchr(q, ':'); if (!colon) break; const char *act = colon + 1; size_t al = strcspn(act, ",");
                        if (idx > done_upto && tn - ts0 >= at) {
                            done_upto = idx; fprintf(stderr, "[test script] %.*s\n", (int)al, act);
                            if (!strncmp(act, "connect", al) && M.nmacs > 0) { M.target = M.macs[0]; struct disc_mac *f = disc_find_id(M.target.id); if (f && f->ip[0]) snprintf(M.target.host, sizeof M.target.host, "%s", f->ip); M.action = ACT_CONNECT; }
                            else if (!strncmp(act, "cancel", al)) M.action = ACT_CANCEL_CONNECT;
                            else if (!strncmp(act, "menu", al)) vr_toolbar_action = 1;
                            else if (!strncmp(act, "close", al)) vr_toolbar_action = 2;
                            else if (!strncmp(act, "quit", al)) M.action = ACT_QUIT;
                        }
                        idx++; q = act + al; if (*q == ',') q++; }
                }
            }
            /* ---- requests from the menu or the toolbar ---- */
            if (M.action == ACT_QUIT || vr_toolbar_action == 2) {
                M.action = ACT_NONE; vr_toolbar_action = 0;
                if (streaming) { session_stop(); streaming = false; }
                vr_show_closing(); usleep(1300000); running = false; continue;   /* like Framecorder: a short "Closing" screen, then exit */
            }
            if (vr_toolbar_action == 1) { vr_toolbar_action = 0; session_stop(); streaming = false; menu_goto(SCR_HOME); continue; }
            if (M.action == ACT_CANCEL_CONNECT) { M.action = ACT_NONE; if (streaming) { session_stop(); streaming = false; } menu_goto(SCR_HOME); continue; }
            if (M.action == ACT_CONNECT) {
                M.action = ACT_NONE; char err[200] = {0};
                if (session_start(&M.target, err, sizeof err)) {
                    streaming = true; have_pkt = eof = stop_sent = drained = false;
                    M.scr = SCR_CONNECTING; M.connect_t0 = tn; last_net_up = tn; net_up_since = 0; M.dirty = true;
                } else { menu_banner(C_RED, err); menu_goto(SCR_HOME); }
            }
            /* ---- supervision of a running session ---- */
            if (streaming) {
                if (vr_view == VIEW_MENU) {                  /* still connecting (or reconnecting) */
                    snprintf(M.connect_status, sizeof M.connect_status, "%s", !net_is_up() ? "Contacting the Mac…" : "Connected. Waiting for the Mac’s screen…");
                    bool fail = false; char b[360] = {0}; uint32_t col = C_RED;
                    if (!net_is_up() && tn - M.connect_t0 > 7) {
                        fail = true;
                        int idx = -1; for (int i = 0; i < M.nmacs; i++) if (!strcmp(M.macs[i].id, M.target.id)) idx = i;
                        if (disc_find_id(M.target.id)) {     /* the Mac is on the network but ignores us: it doesn't know this Frame any more */
                            snprintf(b, sizeof b, "“%s” is on your network but doesn’t recognize this Frame. It was probably forgotten on the Mac. Tap Pair again.", M.target.name);
                            if (idx >= 0) M.needs_repair[idx] = true;
                            col = C_AMBER;
                        } else snprintf(b, sizeof b, "Couldn’t reach “%s”. Is the Mac awake, with Sideframe running (its icon is in the menu bar)?", M.target.name);
                    } else if (net_is_up() && !vr_have_video && net_up_since && tn - net_up_since > 14) {
                        fail = true; snprintf(b, sizeof b, "Connected to “%s”, but its screen isn’t coming through. Is the Mac’s screen locked or asleep, or Screen Recording not allowed?", M.target.name);
                        col = C_AMBER;
                    }
                    if (fail) { session_stop(); streaming = false; menu_goto(SCR_HOME); menu_banner(col, b); continue; }
                } else if (!net_is_up() && tn - last_net_up > 4) {   /* in the stream view and the Mac went away: back to "connecting" */
                    vr_enter_menu(); M.scr = SCR_CONNECTING; M.connect_t0 = tn; vr_have_video = false; M.dirty = true;
                }
            }
            if (!streaming) {                                /* idle in the menu: sleep until a Mac announces itself (or 60-250 ms) */
                struct pollfd dp[2] = { { disc_fd, POLLIN, 0 }, { disc_fd2, POLLIN, 0 } };
                poll(dp, 2, tab_visible ? 60 : 250);
                continue;
            }
        }
#endif
        // 1. reclaim finished bitstream buffers
        for (;;) {
            struct v4l2_plane pl[VIDEO_MAX_PLANES] = {0};
            struct v4l2_buffer b = { .type = OUT_T, .memory = V4L2_MEMORY_MMAP, .length = 1, .m.planes = pl };
            if (xioctl(vfd, VIDIOC_DQBUF, &b) < 0) break;
            ob[b.index].free = true;
        }
        // 2. feed bitstream (from file or network)
        while (!eof || have_pkt) {
            if (!have_pkt) {
                if (netmode) {
                    if (!udp_next(&au_d, &au_n)) { if (net_closed) eof = true; break; }
                    have_pkt = true;
                } else {
                    if (pace_fps > 0 && (double)n_fed >= (now() - t0) * pace_fps + 2) break;   // file playback: stay at pace_fps
                    int r = av_read_frame(fc, pkt);
                    if (r < 0) {
                        if (loop) {   // raw HEVC isn't seekable: reopen the file for the next pass
                            avformat_close_input(&fc);
                            if (avformat_open_input(&fc, path, NULL, NULL) < 0) { eof = true; break; }
                            avformat_find_stream_info(fc, NULL);
                            si = av_find_best_stream(fc, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);
                            break;
                        }
                        eof = true; break;
                    }
                    if (pkt->stream_index != si) { av_packet_unref(pkt); continue; }
                    au_d = pkt->data; au_n = pkt->size; have_pkt = true;
                }
            }
            int fi = -1; for (int i = 0; i < OUT_N; i++) if (ob[i].free) { fi = i; break; }
            if (fi < 0) break;
            if (au_n > ob[fi].len) { fprintf(stderr, "packet %zu > buffer\n", au_n); have_pkt = false; if (!netmode) av_packet_unref(pkt); continue; }
            memcpy(ob[fi].map, au_d, au_n);
            struct v4l2_plane pl[VIDEO_MAX_PLANES] = {0}; pl[0].bytesused = au_n;
            uint32_t cur_seq = netmode ? net_seq : (uint32_t)n_fed;
            recv_t[cur_seq & 255] = netmode ? net_recv_t : now();
            struct v4l2_buffer b = { .type = OUT_T, .memory = V4L2_MEMORY_MMAP, .index = fi, .length = 1, .m.planes = pl };
            b.timestamp.tv_sec = 0; b.timestamp.tv_usec = cur_seq;
            CHK(xioctl(vfd, VIDIOC_QBUF, &b));
            ob[fi].free = false; n_fed++; have_pkt = false; if (!netmode) av_packet_unref(pkt);
        }
        if (eof && !stop_sent && !(loop && !netmode)) { struct v4l2_decoder_cmd dc = { .cmd = V4L2_DEC_CMD_STOP }; xioctl(vfd, VIDIOC_DECODER_CMD, &dc); stop_sent = true; }

        // 3. wayland + v4l2 events
#ifndef MD_VR
        while (wl_display_prepare_read(dpy) != 0) wl_display_dispatch_pending(dpy);
        wl_display_flush(dpy);
        struct pollfd p[3] = { { wl_display_get_fd(dpy), POLLIN, 0 }, { vfd, POLLIN | POLLPRI, 0 }, { ufd, POLLIN, 0 } };
        poll(p, 3, netmode && !udp_busy() ? 50 : 3);   /* idle: sleep until a packet / decoder event arrives (max 25 ms) instead of spinning every 3 ms */
        if (p[0].revents & POLLIN) wl_display_read_events(dpy); else wl_display_cancel_read(dpy);
        wl_display_dispatch_pending(dpy);
#else
        struct pollfd p[3] = { { -1, 0, 0 }, { vfd, POLLIN | POLLPRI, 0 }, { ufd, POLLIN, 0 } };
        int wait_ms = netmode && !udp_busy() ? 50 : 3;   /* idle: sleep until a packet / decoder event arrives instead of spinning */
        if (vr_held >= 0) { int due = vr_due_ms(); if (due < wait_ms) wait_ms = due; }
        if (kbd_grabbed && wait_ms > 8) wait_ms = 8;                 /* a physical keyboard is typing into the Mac: poll it often */
        poll(p, 3, wait_ms);
        vr_poll();
#endif

        // 4. decoder events (resolution known -> allocate capture buffers)
        if (p[1].revents & POLLPRI) {
            struct v4l2_event ev;
            while (xioctl(vfd, VIDIOC_DQEVENT, &ev) == 0)
                if (ev.type == V4L2_EVENT_SOURCE_CHANGE && !cap_ready) setup_capture();
        }
        // 5. decoded frames -> compositor
        if (cap_ready) for (;;) {
            struct v4l2_plane pl[VIDEO_MAX_PLANES] = {0};
            struct v4l2_buffer b = { .type = CAP_T, .memory = V4L2_MEMORY_MMAP, .length = 1, .m.planes = pl };
            if (xioctl(vfd, VIDIOC_DQBUF, &b) < 0) break;
            if (b.flags & V4L2_BUF_FLAG_LAST) { drained = true; if (pl[0].bytesused == 0) continue; }
            if (pl[0].bytesused == 0) { // empty buffer: give it straight back
                xioctl(vfd, VIDIOC_QBUF, &b); continue; }
            if (b.flags & V4L2_BUF_FLAG_ERROR) { fprintf(stderr, "decoder flagged frame error -> requesting keyframe\n"); request_keyframe(); }
            n_dec++;
            if (want_rgba) {   /* experiment: look at the pixels, give the buffer straight back */
                if (rgba_dump > 0) { rgba_dump--; dump_rgba(b.index); }
                struct v4l2_plane rpl[VIDEO_MAX_PLANES] = {0};
                struct v4l2_buffer rb = { .type = CAP_T, .memory = V4L2_MEMORY_MMAP, .index = b.index, .length = 1, .m.planes = rpl };
                xioctl(vfd, VIDIOC_QBUF, &rb); n_present++; continue;
            }
#ifdef MD_VR
            { int give = vr_offer(b.index);   /* newest frame wins; the older, never-shown one goes straight back to the decoder */
              if (give >= 0) requeue_capture(give); }
#else
            cb[b.index].with_compositor = true;
            wl_surface_attach(vsurf, cb[b.index].wl, 0, 0);
            wl_surface_damage_buffer(vsurf, 0, 0, cap_w, cap_h);
            wl_surface_commit(vsurf);
#endif
            n_present++;
            { // latency bookkeeping: recv-complete -> committed to compositor (includes HW decode)
                uint32_t seq = (uint32_t)b.timestamp.tv_usec;
                if (b.timestamp.tv_sec != 0 || (seq == 0 && n_dec > 1)) ts_missing++;
                double tn = now();
                if (last_present > 0) { double g = tn - last_present; if (g > max_gap) max_gap = g; if (g > 0.5) gaps500++; if (g > 0.25) gaps250++; if (g > 0.1) gaps100++; }
                last_present = tn;
                double proc = now() - recv_t[seq & 255];
                proc_sum += proc; proc_n++; if (proc > proc_max) proc_max = proc; if (proc < proc_min) proc_min = proc;
                if (proc_all_n == proc_all_cap) { proc_all_cap = proc_all_cap ? proc_all_cap * 2 : 4096; proc_all = realloc(proc_all, proc_all_cap * sizeof *proc_all); }
                proc_all[proc_all_n++] = proc;
                if (netmode) send_ack(seq, (uint32_t)(proc * 1e6));
            }
        }
#ifdef MD_VR
        { int done = vr_service(); if (done >= 0) requeue_capture(done); }   /* show the newest frame if it is due, then give its buffer back */
#endif
        double t = now();
        if (t - tlast >= 1.0) {
            if (!quiet) {
                fprintf(stderr, "t=%4.1fs  decoded+presented %3.0f fps   (fed %lu, released %lu)\n", t - t0,
                        (n_dec - ld) / (t - tlast), n_fed, n_rel);
                if (proc_n) fprintf(stderr, "          frame-side recv->present: avg %.1f ms  min %.1f  max %.1f  (n=%lu)\n", proc_sum / proc_n * 1e3, proc_min * 1e3, proc_max * 1e3, proc_n);
            }
            ld = n_dec; tlast = t;
        }
        if (secs > 0 && t - t0 >= secs) break;
    }
#ifdef MD_VR
    if (menu_mode && streaming) session_stop();
#endif
    if (proc_all_n) {
        int cmp(const void *a, const void *b) { double x = *(double *)a, y = *(double *)b; return (x > y) - (x < y); }
        qsort(proc_all, proc_all_n, sizeof(double), cmp);
        fprintf(stderr, "FRAME-SIDE recv->present (incl. HW decode), n=%zu: p50 %.1f ms  p95 %.1f ms  p99 %.1f ms  max %.1f ms%s\n", proc_all_n,
                proc_all[proc_all_n / 2] * 1e3, proc_all[proc_all_n * 95 / 100] * 1e3, proc_all[proc_all_n * 99 / 100] * 1e3, proc_all[proc_all_n - 1] * 1e3,
                ts_missing ? "  [WARNING: decoder did not echo timestamps for some frames]" : "");
    }
    fprintf(stderr, "STUTTER: gaps between presented frames >100ms: %u, >250ms: %u, >500ms: %u, longest %.0f ms\n", gaps100, gaps250, gaps500, max_gap * 1e3);
    if (netmode) fprintf(stderr, "UDP: %lu packets, %lu retransmits received, %lu NACKs sent, %lu resyncs, %lu duplicates/late, srtt %.0f ms; rejected (forged/replayed/wrong-key) packets: %lu\n", u_pk, u_retx_rx, u_nack_tx, u_resync, u_dup, u_srtt * 1e3, u_auth_fail);
    double el = now() - t0;
    fprintf(stderr, "DONE: %lu frames presented in %.2fs = %.1f fps average, zero CPU copies\n", n_present, el, n_present / el);
#ifdef MD_VR
    fprintf(stderr, "dashboard tab: %lu frames pushed to SteamVR, %lu skipped while the tab was hidden\n", vr_pushed, vr_skipped_hidden);
    vr_shutdown();
#endif
    return 0;
}
