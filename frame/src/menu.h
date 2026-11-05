// menu.h — the Sideframe tab's screens: Home (your Macs), Add a Mac (find, then enter the code), Pairing, Connecting.
// Pure UI + state; the host program performs the side effects (connect / quit) when it sees menu.action.
#ifndef MENU_H
#define MENU_H
#include <pthread.h>
#include <ifaddrs.h>
#include <net/if.h>
#include "ui.h"
#include "discovery.h"
#include "pairing_client.h"

#ifndef SIDEFRAME_VERSION
#define SIDEFRAME_VERSION "dev"
#endif
#ifndef SIDEFRAME_UPDATE_URL
#define SIDEFRAME_UPDATE_URL ""   /* URL of a text file holding the latest version number, e.g. 0.1.1 ; empty = no update check */
#endif
#define MENU_WIDTH 1720
#define MENU_HEIGHT 720
#define MAX_MACS 8

enum scr { SCR_HOME, SCR_ADD_PICK, SCR_ADD_CODE, SCR_PAIRING, SCR_CONNECTING, SCR_CLOSING };
enum act { ACT_NONE, ACT_CONNECT, ACT_QUIT, ACT_CANCEL_CONNECT };

struct pair_job { char host[96]; int port; char code[16]; char name[96]; volatile int state; /* 0 idle 1 running 2 ok 3 failed */ struct mac_conf result; char err[256]; pthread_t th; };

static struct menu {
    enum scr scr; enum act action;
    struct mac_conf macs[MAX_MACS]; int nmacs;
    bool needs_repair[MAX_MACS];
    char banner[300]; uint32_t banner_color; double banner_until;
    /* add flow */
    char sel_name[96], sel_host[96], sel_id[17]; int sel_port;
    char code[16]; int ncode; char err[300];
    struct pair_job job;
    /* connecting */
    struct mac_conf target; double connect_t0; char connect_status[160];
    int forget_armed; double forget_t; double search_t0;   /* when the Add-a-Mac search started (for the "still nothing?" help) */
    bool dirty;
    double t_draw;
    /* health (checked every few seconds) and the manual update check */
    bool net_ok, dec_ok; double health_t;
    int upd_state; char upd_ver[40];          /* 0 idle, 1 checking, 2 up to date, 3 newer version exists, 4 couldn't check */
} M;
static void menu_health(double t);

/* ---------------------------------------------------------------- helpers */
static void menu_banner(uint32_t color, const char *text) { snprintf(M.banner, sizeof M.banner, "%s", text); M.banner_color = color; M.banner_until = disc_now() + 9; M.dirty = true; }
static void menu_refresh_macs(void) {
    struct mac_conf all[16]; int n = mac_conf_list(all, 16);
    bool keep[MAX_MACS]; memcpy(keep, M.needs_repair, sizeof keep); char ids[MAX_MACS][17]; for (int i = 0; i < MAX_MACS; i++) snprintf(ids[i], 17, "%s", M.macs[i].id);
    M.nmacs = n > MAX_MACS ? MAX_MACS : n;
    for (int i = 0; i < M.nmacs; i++) {
        M.macs[i] = all[i]; M.needs_repair[i] = false;
        for (int k = 0; k < MAX_MACS; k++) if (keep[k] && ids[k][0] && !strcmp(ids[k], all[i].id)) M.needs_repair[i] = true;
    }
    M.dirty = true;
}
#ifndef SIDEFRAME_URL
#define SIDEFRAME_URL ""   /* set at build time (make SIDEFRAME_URL=...) to show where to download the Mac app */
#endif
static void menu_goto(enum scr s) { if (s == SCR_ADD_PICK && M.scr != SCR_ADD_PICK) M.search_t0 = disc_now(); M.scr = s; M.dirty = true; M.err[0] = 0; M.forget_armed = -1; }
static void menu_init(void) {
    memset(&M, 0, sizeof M); M.forget_armed = -1; menu_refresh_macs(); menu_goto(SCR_HOME);
}
static const char *frame_name(void) { static char nm[96]; char hn[64] = "Frame"; gethostname(hn, sizeof hn - 1); snprintf(nm, sizeof nm, "Steam Frame (%.40s)", hn); return nm; }

static void *pair_thread(void *a) {
    struct pair_job *j = a; j->state = 1;
    int rc = pair_run(j->host, j->port, j->code, j->name, &j->result, j->err, sizeof j->err);
    if (rc == 0 && !mac_conf_save(&j->result)) { snprintf(j->err, sizeof j->err, "Paired, but this Frame couldn't save the key."); rc = -9; }
    j->state = rc == 0 ? 2 : 3;
    return NULL;
}
static void menu_start_pairing(void) {
    memset(&M.job, 0, sizeof M.job);
    snprintf(M.job.host, sizeof M.job.host, "%s", M.sel_host); M.job.port = M.sel_port; snprintf(M.job.code, sizeof M.job.code, "%s", M.code);
    snprintf(M.job.name, sizeof M.job.name, "%s", frame_name());
    M.job.state = 1; menu_goto(SCR_PAIRING);
    if (pthread_create(&M.job.th, NULL, pair_thread, &M.job) != 0) { M.job.state = 3; snprintf(M.job.err, sizeof M.job.err, "Couldn't start pairing."); }
    else pthread_detach(M.job.th);
}

/* ---------------------------------------------------------------- per-frame logic (call before drawing); returns true if a redraw is needed */
static bool menu_tick(double t) {
    bool d = M.dirty; M.dirty = false;
    if (disc_poll()) d = true;
    menu_health(t);
    if (M.banner[0] && t > M.banner_until) { M.banner[0] = 0; d = true; }
    if (M.forget_armed >= 0 && t - M.forget_t > 3.5) { M.forget_armed = -1; d = true; }
    if (M.scr == SCR_PAIRING) {
        if (M.job.state == 2) {
            menu_refresh_macs();
            for (int i = 0; i < M.nmacs; i++) if (!strcmp(M.macs[i].id, M.job.result.id)) M.needs_repair[i] = false;
            char b[200]; snprintf(b, sizeof b, "Paired with “%s”. Tap Connect to start.", M.job.result.name); menu_goto(SCR_HOME); menu_banner(C_GREEN, b); d = true;
        } else if (M.job.state == 3) {
            snprintf(M.err, sizeof M.err, "%s", M.job.err); M.code[0] = 0; M.ncode = 0; M.scr = SCR_ADD_CODE; d = true;
        }
        d = true;   /* spinner */
    }
    if (M.scr == SCR_CONNECTING || (M.scr == SCR_ADD_PICK && !disc_macs[0].used)) d = true;   /* spinner / dots */
    return d;
}

/* ---------------------------------------------------------------- drawing */
static void header(void) {
    ui_logo(56, 34, 84);
    ui_text(160, 32, "Sideframe", 54, true, C_TEXT);
    ui_text(162, 96, "Your Mac desktop, in your Steam Frame.", 24, false, C_TEXT_DIM);
}
/* ---------------------------------------------------------------- health: is there a network, can this Frame decode video? */
static bool health_has_network(void) {
    struct ifaddrs *ifa, *p; bool ok = false;
    if (getifaddrs(&ifa) != 0) return true;                    /* can't tell: don't nag */
    for (p = ifa; p; p = p->ifa_next) {
        if (!p->ifa_addr || p->ifa_addr->sa_family != AF_INET || !(p->ifa_flags & IFF_UP) || (p->ifa_flags & IFF_LOOPBACK)) continue;
        uint32_t a = ntohl(((struct sockaddr_in *)p->ifa_addr)->sin_addr.s_addr);
        if ((a >> 16) == 0xA9FE) continue;                     /* 169.254.x.x: no real address yet */
        ok = true; break;
    }
    freeifaddrs(ifa); return ok;
}
static void menu_health(double t) {
    if (t - M.health_t < 3.0) return;
    M.health_t = t; bool n = health_has_network(), d = access("/dev/video-dec0", R_OK | W_OK) == 0;
    if (n != M.net_ok || d != M.dec_ok) M.dirty = true;
    M.net_ok = n; M.dec_ok = d;
}
/* the most important problem right now, or NULL */
static const char *health_problem(void) {
    if (!M.dec_ok && M.health_t > 0) return "This Frame’s video decoder isn’t available, so Sideframe can’t show video. Restart the Frame and try again.";
    if (!M.net_ok && M.health_t > 0) return "This Frame isn’t connected to a network. Connect to the same Wi-Fi as your Mac.";
    return NULL;
}

/* ---------------------------------------------------------------- update check (manual; one small request, only when the user presses the button) */
static int ver_cmp(const char *a, const char *b) {            /* dotted numbers: 0.1.10 > 0.1.9 */
    for (int i = 0; i < 4; i++) { long x = strtol(a, (char **)&a, 10), y = strtol(b, (char **)&b, 10); if (x != y) return x < y ? -1 : 1; if (*a == '.') a++; if (*b == '.') b++; }
    return 0;
}
static void *update_thread(void *arg) {
    (void)arg; char cmd[400], out[64] = {0};
    snprintf(cmd, sizeof cmd, "curl -fsSL --max-time 8 --proto '=https' '%s' 2>/dev/null", SIDEFRAME_UPDATE_URL);
    FILE *f = popen(cmd, "r"); if (f) { if (!fgets(out, sizeof out, f)) out[0] = 0; pclose(f); }
    out[strcspn(out, "\r\n ")] = 0;
    bool ok = out[0] >= '0' && out[0] <= '9';
    if (!ok) M.upd_state = 4;
    else { snprintf(M.upd_ver, sizeof M.upd_ver, "%s", out); M.upd_state = ver_cmp(SIDEFRAME_VERSION, out) < 0 ? 3 : 2; }
    M.dirty = true; return NULL;
}
static void menu_check_update(void) {
    if (M.upd_state == 1 || !SIDEFRAME_UPDATE_URL[0]) return;
    M.upd_state = 1; pthread_t th; if (pthread_create(&th, NULL, update_thread, NULL) == 0) pthread_detach(th); else M.upd_state = 4;
    M.dirty = true;
}
static void draw_footer(float x) {                              /* version + update line, bottom centre of Home */
    char b[160]; uint32_t c = C_TEXT_FAINT;
    if (!SIDEFRAME_UPDATE_URL[0]) { snprintf(b, sizeof b, "Sideframe %s", SIDEFRAME_VERSION); }
    else switch (M.upd_state) {
        case 1: snprintf(b, sizeof b, "Sideframe %s  ·  checking for updates…", SIDEFRAME_VERSION); break;
        case 2: snprintf(b, sizeof b, "Sideframe %s  ·  up to date", SIDEFRAME_VERSION); c = C_GREEN; break;
        case 3: snprintf(b, sizeof b, "Sideframe %s  ·  version %s is available (see the project page)", SIDEFRAME_VERSION, M.upd_ver); c = C_AMBER; break;
        case 4: snprintf(b, sizeof b, "Sideframe %s  ·  couldn’t check for updates (no internet?)", SIDEFRAME_VERSION); break;
        default: snprintf(b, sizeof b, "Sideframe %s", SIDEFRAME_VERSION);
    }
    ui_text(x, 650, b, 22, false, c);
    if (SIDEFRAME_UPDATE_URL[0] && M.upd_state != 1) { float w = ui_text_w(b, 22, false); if (x + w + 24 + 220 < MENU_WIDTH - 56 - 300 - 16 && ui_button(x + w + 24, 636, 220, 48, "Check for updates", 3, true, 20)) menu_check_update(); }
}

static void draw_banner(void) {
    const char *msg = M.banner[0] ? M.banner : health_problem(); uint32_t col = M.banner[0] ? M.banner_color : C_AMBER;
    if (!msg) return;
    float w = 640, x = MENU_WIDTH - 56 - w;
    ui_dry = true; float h = ui_paragraph(0, 0, w - 56, msg, 24, false, C_TEXT); ui_dry = false;
    float bh = h + 32;
    ui_rrect(x, 36, w, bh, 20, RGBA(0x10, 0x18, 0x2a, 255));
    ui_rrect_line(x, 36, w, bh, 20, 2, col);
    ui_circle(x + 28, 36 + bh / 2, 8, col);
    ui_paragraph(x + 52, 36 + 16, w - 76, msg, 24, false, C_TEXT);
}
static const char *mac_status(int i, uint32_t *color, struct disc_mac **found) {
    struct disc_mac *f = disc_find_id(M.macs[i].id); if (found) *found = f;
    static char buf[200];
    if (M.needs_repair[i]) { *color = C_AMBER; return "Doesn’t recognize this Frame. Pair again."; }
    if (f) { *color = C_GREEN; snprintf(buf, sizeof buf, "Online  ·  %s", f->ip[0] ? f->ip : f->host); return buf; }
    *color = C_TEXT_FAINT; return "Not found on your network right now";
}

static void screen_home(double t) {
    header(); draw_banner();
    if (M.nmacs == 0) {
        ui_rrect(56, 190, 1608, 330, 28, C_CARD); ui_rrect_line(56, 190, 1608, 330, 28, 2, C_LINE);
        ui_text(100, 226, "No Mac paired yet", 42, true, C_TEXT);
        ui_paragraph(100, 288, 1500, "Install Sideframe on your Mac first if you haven’t (download it from the Sideframe project page).\n1.  On your Mac, click the Sideframe icon in the menu bar and choose “Pair a Steam Frame…”.\n2.  Come back here, tap Add a Mac, pick your Mac and type the code it shows.", 28, false, C_TEXT_DIM);
        if (ui_button(100, 440, 340, 72, "Add a Mac", 0, true, 30)) menu_goto(SCR_ADD_PICK);
        if (ui_button(MENU_WIDTH - 56 - 240, 628, 240, 60, "Close Sideframe", 3, true, 26)) M.action = ACT_QUIT;
        draw_footer(56);
        return;
    }
    ui_text(56, 168, "YOUR MACS", 20, true, C_TEXT_FAINT);
    int shown = M.nmacs > 3 ? 3 : M.nmacs;
    for (int i = 0; i < shown; i++) {
        float y = 200 + i * 128;
        uint32_t col; struct disc_mac *f; const char *st = mac_status(i, &col, &f);
        bool hot = ui_hot(56, y, 1608, 112);
        ui_rrect(56, y, 1608, 112, 26, hot ? C_CARD_HOT : C_CARD); ui_rrect_line(56, y, 1608, 112, 26, 2, C_LINE);
        ui_circle(98, y + 56, 9, col);
        ui_text(130, y + 18, M.macs[i].name, 34, true, C_TEXT);
        ui_text(130, y + 64, st, 24, false, C_TEXT_DIM);
        float bx = 56 + 1608 - 24;
        bool armed = M.forget_armed == i;
        bx -= 168; if (ui_button(bx, y + 22, 168, 68, armed ? "Sure?" : "Forget", armed ? 2 : 3, true, 24)) {
            if (armed) {
                remove(M.macs[i].path); char b[160]; snprintf(b, sizeof b, "Forgot “%s”. Add it again with Add a Mac.", M.macs[i].name);
                menu_refresh_macs(); menu_banner(C_TEXT_DIM, b); M.forget_armed = -1; return;
            } else { M.forget_armed = i; M.forget_t = t; M.dirty = true; }
        }
        bx -= 16 + 250;
        if (M.needs_repair[i]) {
            if (ui_button(bx, y + 22, 250, 68, "Pair again", 0, true, 28)) {
                snprintf(M.sel_name, sizeof M.sel_name, "%s", M.macs[i].name); snprintf(M.sel_id, sizeof M.sel_id, "%s", M.macs[i].id);
                snprintf(M.sel_host, sizeof M.sel_host, "%s", f && f->ip[0] ? f->ip : M.macs[i].host); M.sel_port = f ? f->port : M.macs[i].port;
                M.code[0] = 0; M.ncode = 0; menu_goto(SCR_ADD_CODE);
            }
        } else if (ui_button(bx, y + 22, 250, 68, f ? "Connect" : "Try anyway", f ? 0 : 1, true, 28)) {
            M.target = M.macs[i]; if (f && f->ip[0]) snprintf(M.target.host, sizeof M.target.host, "%s", f->ip);
            M.action = ACT_CONNECT;
        }
    }
    if (M.nmacs > 3) { char b[64]; snprintf(b, sizeof b, "…and %d more", M.nmacs - 3); ui_text(56, 590, b, 22, false, C_TEXT_FAINT); }
    if (ui_button(56, 632, 280, 60, "Add a Mac", 1, true, 26)) menu_goto(SCR_ADD_PICK);
    if (ui_button(MENU_WIDTH - 56 - 300, 632, 300, 60, "Close Sideframe", 3, true, 26)) M.action = ACT_QUIT;
    draw_footer(360);
}

static void screen_add_pick(double t) {
    draw_banner();
    if (ui_button(40, 36, 150, 56, "‹  Back", 3, true, 26)) { menu_goto(SCR_HOME); return; }
    ui_text(56, 110, "Add a Mac", 54, true, C_TEXT);
    ui_paragraph(56, 184, 1600, "On your Mac, click the Sideframe icon in the menu bar and choose “Pair a Steam Frame…”. Then pick your Mac below.", 28, false, C_TEXT_DIM);
    int n = 0;
    for (int k = 0; k < DISC_MAX; k++) {
        if (!disc_macs[k].used) continue;
        float y = 290 + n * 112; if (y > 560) break;
        bool paired = false; for (int i = 0; i < M.nmacs; i++) if (!strcmp(M.macs[i].id, disc_macs[k].id)) paired = true;
        bool hot = ui_hot(56, y, 1608, 96);
        ui_rrect(56, y, 1608, 96, 24, hot ? C_CARD_HOT : C_CARD); ui_rrect_line(56, y, 1608, 96, 24, 2, hot ? C_ACCENT : C_LINE);
        ui_circle(98, y + 48, 9, C_GREEN);
        ui_text(130, y + 14, disc_macs[k].name, 32, true, C_TEXT);
        char sub[200]; snprintf(sub, sizeof sub, "%s%s", disc_macs[k].ip[0] ? disc_macs[k].ip : disc_macs[k].host, paired ? "  ·  already paired (pair again to replace)" : "");
        ui_text(130, y + 56, sub, 22, false, C_TEXT_DIM);
        ui_text(MENU_WIDTH - 56 - 150, y + 26, "Choose  ›", 28, true, C_ACCENT);
        if (ui_clicked(56, y, 1608, 96)) {
            snprintf(M.sel_name, sizeof M.sel_name, "%s", disc_macs[k].name); snprintf(M.sel_id, sizeof M.sel_id, "%s", disc_macs[k].id);
            snprintf(M.sel_host, sizeof M.sel_host, "%s", disc_macs[k].ip[0] ? disc_macs[k].ip : disc_macs[k].host); M.sel_port = disc_macs[k].port;
            M.code[0] = 0; M.ncode = 0; menu_goto(SCR_ADD_CODE); return;
        }
        n++;
    }
    if (n == 0) {
        ui_spinner(100, 360, 26, t);
        char dots[8] = "..."; dots[(int)(t * 2) % 4] = 0;
        char b[64]; snprintf(b, sizeof b, "Looking for Macs%s", dots);
        ui_text(150, 336, b, 32, true, C_TEXT);
        if (disc_now() - M.search_t0 < 6.0) ui_paragraph(150, 392, 1300, "Make sure Sideframe is running on your Mac (look for its icon in the menu bar) and that your Mac and this Frame are on the same Wi-Fi.", 24, false, C_TEXT_DIM);
        else {   /* nothing found for a while: spell out the usual causes */
            ui_text(150, 392, "Still nothing? Check these:", 26, true, C_TEXT);
            float yy = 436;
            char l1[300]; snprintf(l1, sizeof l1, "1.  Is Sideframe installed on your Mac? %s%s", SIDEFRAME_URL[0] ? "Get it at " : "Download it from the Sideframe project page.", SIDEFRAME_URL);
            yy += ui_paragraph(150, yy, 1450, l1, 24, false, C_TEXT_DIM) + 8;
            yy += ui_paragraph(150, yy, 1450, "2.  Is its icon in the Mac’s menu bar? If not, open Sideframe from Applications.", 24, false, C_TEXT_DIM) + 8;
            yy += ui_paragraph(150, yy, 1450, "3.  Are your Mac and this Frame on the same Wi-Fi network (not a guest network)?", 24, false, C_TEXT_DIM) + 8;
            ui_paragraph(150, yy, 1450, "4.  If your Mac asked to allow incoming connections, choose Allow.", 24, false, C_TEXT_DIM);
        }
    }
}

static void screen_add_code(double t) {
    if (ui_button(40, 36, 150, 56, "‹  Back", 3, true, 26)) { menu_goto(SCR_ADD_PICK); return; }
    ui_text(56, 110, "Enter the code", 54, true, C_TEXT);
    char sub[200]; snprintf(sub, sizeof sub, "shown on “%s”", M.sel_name);
    ui_text(58, 176, sub, 30, false, C_TEXT_DIM);
    /* the 8 digit boxes */
    float bw = 76, bh = 104, gap = 10, x0 = 56, y0 = 250;
    for (int i = 0; i < 8; i++) {
        float x = x0 + i * (bw + gap) + (i >= 4 ? 34 : 0);
        bool cur = (i == M.ncode);
        ui_rrect(x, y0, bw, bh, 20, C_CARD); ui_rrect_line(x, y0, bw, bh, 20, cur ? 3 : 2, cur ? C_ACCENT : C_LINE);
        if (i < M.ncode) { char c[2] = { M.code[i], 0 }; ui_text_center(x + bw / 2, y0 + 20, c, 56, true, C_TEXT); }
    }
    if (M.err[0]) { ui_dry = true; float h = ui_paragraph(0, 0, 790, M.err, 24, false, C_RED); ui_dry = false; ui_paragraph(56, 380, 790, M.err, 24, false, C_RED); (void)h; }
    else ui_paragraph(56, 380, 790, "Open “Pair a Steam Frame…” on your Mac to see the code. It stays the same until you close that window or ask for a new one.", 22, false, C_TEXT_FAINT);
    if (ui_button(56, 560, 360, 80, "Pair", 0, M.ncode == 8, 32)) menu_start_pairing();
    if (ui_button(436, 560, 200, 80, "Clear", 1, M.ncode > 0, 28)) { M.ncode = 0; M.code[0] = 0; M.err[0] = 0; M.dirty = true; }
    /* number pad */
    const char *keys[12] = { "1", "2", "3", "4", "5", "6", "7", "8", "9", "Clear", "0", "Delete" };
    for (int r = 0; r < 4; r++) for (int c = 0; c < 3; c++) {
        int k = r * 3 + c; float x = 1000 + c * 216, y = 130 + r * 112;
        if (k == 9) continue;   /* Clear is on the left; leave the slot empty */
        bool del = (k == 11);
        if (ui_button(x, y, 200, 96, keys[k], del ? 1 : 1, true, del ? 26 : 40)) {
            if (del) { if (M.ncode > 0) { M.code[--M.ncode] = 0; M.err[0] = 0; M.dirty = true; } }
            else if (M.ncode < 8) { M.code[M.ncode++] = keys[k][0]; M.code[M.ncode] = 0; M.err[0] = 0; M.dirty = true;
                if (M.ncode == 8) menu_start_pairing(); }   /* eight digits: pair right away */
        }
    }
}

static void screen_pairing(double t) {
    ui_spinner(MENU_WIDTH / 2, 280, 48, t);
    char b[200]; snprintf(b, sizeof b, "Pairing with “%s”", M.sel_name);
    ui_text_center(MENU_WIDTH / 2, 360, b, 44, true, C_TEXT);
    ui_text_center(MENU_WIDTH / 2, 430, "This takes a few seconds.", 28, false, C_TEXT_DIM);
}

static void screen_connecting(double t) {
    ui_spinner(MENU_WIDTH / 2, 260, 48, t);
    char b[200]; snprintf(b, sizeof b, "Connecting to “%s”", M.target.name);
    ui_text_center(MENU_WIDTH / 2, 340, b, 44, true, C_TEXT);
    ui_text_center(MENU_WIDTH / 2, 410, M.connect_status[0] ? M.connect_status : "Starting the stream…", 26, false, C_TEXT_DIM);
    if (ui_button(MENU_WIDTH / 2 - 130, 520, 260, 72, "Cancel", 1, true, 28)) M.action = ACT_CANCEL_CONNECT;
}

static void screen_closing(double t) {
    ui_logo(MENU_WIDTH / 2 - 56, 230, 112);
    ui_text_center(MENU_WIDTH / 2, 370, "Closing Sideframe", 48, true, C_TEXT);
    ui_text_center(MENU_WIDTH / 2, 440, "Tap Sideframe in your app list to open it again.", 26, false, C_TEXT_DIM);
}

/* ---------------------------------------------------------------- the toolbar under the live stream */
static void toolbar_geom(int tb, int w, int i, float *x, float *y, float *bw, float *bh) { *bw = tb * 2.9f; *bh = tb - 16; *y = 8; *x = i == 0 ? 18 : w - 18 - *bw; }
/* Paint the strip into UI.px (which must be w x tb pixels): "Menu" (left), connection status, "Close" (right). hover: 0 / 1 / -1. */
static void toolbar_paint(int w, int tb, int hover, const char *mac_name) {
    for (int y = 0; y < tb; y++) {
        uint8_t *row = UI.px + (size_t)y * w * 4; uint8_t r = y < 2 ? 0x2b : 0x0c, g = y < 2 ? 0x3a : 0x12, b = y < 2 ? 0x5c : 0x22;
        for (int x = 0; x < w; x++) { row[x * 4] = r; row[x * 4 + 1] = g; row[x * 4 + 2] = b; row[x * 4 + 3] = 255; }
    }
    int fs = (int)(tb * 0.40f);
    bool hm = UI.have_mouse; float mx = UI.mx, my = UI.my; bool dn = UI.down, rl = UI.released;
    UI.down = false; UI.released = false;
    for (int i = 0; i < 2; i++) {
        float x, y, bw, bh; toolbar_geom(tb, w, i, &x, &y, &bw, &bh);
        UI.have_mouse = (hover == i); UI.mx = x + bw / 2; UI.my = y + bh / 2;           /* makes ui_button draw its hover style */
        ui_button(x, y, bw, bh, i == 0 ? "Menu" : "Close", i == 0 ? 1 : 2, true, fs);
    }
    float x0, y0, w0, h0; toolbar_geom(tb, w, 0, &x0, &y0, &w0, &h0);
    ui_circle(x0 + w0 + tb * 0.55f, tb / 2.0f, tb * 0.11f, C_GREEN);
    char name[200]; snprintf(name, sizeof name, "%s  ·  connected", mac_name && mac_name[0] ? mac_name : "Mac");
    ui_text(x0 + w0 + tb * 0.85f, (tb - fs) / 2.0f - fs * 0.05f, name, fs, false, C_TEXT_DIM);
    UI.have_mouse = hm; UI.mx = mx; UI.my = my; UI.down = dn; UI.released = rl;
}

/* Draw the current screen into UI.px. */
static void menu_draw(double t) {
    ui_clear_gradient(C_BG_TOP, C_BG_BOTTOM);
    switch (M.scr) {
    case SCR_HOME: screen_home(t); break;
    case SCR_ADD_PICK: screen_add_pick(t); break;
    case SCR_ADD_CODE: screen_add_code(t); break;
    case SCR_PAIRING: screen_pairing(t); break;
    case SCR_CONNECTING: screen_connecting(t); break;
    case SCR_CLOSING: screen_closing(t); break;
    }
    UI.released = false;
}
#endif
