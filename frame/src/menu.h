// menu.h — the Sideframe tab's screens: Home (your Macs), Add a Mac (find, then enter the code), Pairing, Connecting.
// Pure UI + state; the host program performs the side effects (connect / quit) when it sees menu.action.
#ifndef MENU_H
#define MENU_H
#include <pthread.h>
#include <ifaddrs.h>
#include <net/if.h>
#include "ui.h"
#include "settings.h"
#include "discovery.h"
#include "pairing_client.h"
#include "linkstats.h"

#ifndef SIDEFRAME_VERSION
#define SIDEFRAME_VERSION "dev"
#endif
#ifndef SIDEFRAME_UPDATE_URL
#define SIDEFRAME_UPDATE_URL ""   /* URL of a text file holding the latest version number, e.g. 0.1.1 ; empty = no update check */
#endif
#define MENU_WIDTH 1280      /* 4:3 panel, like SteamVR's own menus (not the shape of any particular Mac display) */
#define MENU_HEIGHT 960
#define CW (MENU_WIDTH - 112)   /* content width inside the 56 px side margins */
#define MAX_MACS 8

enum scr { SCR_HOME, SCR_ADD_PICK, SCR_ADD_CODE, SCR_PAIRING, SCR_CONNECTING, SCR_CLOSING, SCR_SETTINGS, SCR_ADVANCED, SCR_DIRECT };
enum act { ACT_NONE, ACT_CONNECT, ACT_QUIT, ACT_CANCEL_CONNECT, ACT_DISCONNECT, ACT_RESUME };

struct pair_job { char host[96]; int port; char code[16]; char name[96]; volatile int state; /* 0 idle 1 running 2 ok 3 failed */ struct mac_conf result; char err[256]; pthread_t th; };

static struct menu {
    enum scr scr; enum act action;
    struct mac_conf macs[MAX_MACS]; int nmacs;
    bool needs_repair[MAX_MACS]; int repair_strikes;
    char banner[300]; uint32_t banner_color; double banner_until;
    /* add flow */
    char sel_name[96], sel_host[96], sel_id[17]; int sel_port;
    char code[16]; int ncode; char err[300];
    struct pair_job job;
    /* connecting */
    struct mac_conf target; double connect_t0; char connect_status[160];
    int forget_armed; double forget_t; double search_t0;   /* when the Add-a-Mac search started (for the "still nothing?" help) */
    bool dirty;
    bool live;                                 /* a Mac is on screen in the Sideframe tab right now */
    double t_draw;
    /* health (checked every few seconds) and the manual update check */
    bool net_ok, dec_ok; double health_t;
    char set_err[200];
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
    memset(&M, 0, sizeof M); M.forget_armed = -1; menu_refresh_macs(); ls_links_load(); menu_goto(SCR_HOME);
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
    { static double lt; if (M.scr == SCR_SETTINGS && therm_t != lt) { lt = therm_t; d = true; } }
    { static double lt2; if (M.scr == SCR_ADVANCED && LS.tick_t != lt2) { lt2 = LS.tick_t; d = true; } }   /* live numbers: redraw once a second */
    if (M.banner[0] && t > M.banner_until) { M.banner[0] = 0; d = true; }
    if (M.forget_armed >= 0 && t - M.forget_t > 3.5) { M.forget_armed = -1; d = true; }
    if (M.scr == SCR_PAIRING) {
        if (M.job.state == 2) {
            menu_refresh_macs();
            for (int i = 0; i < M.nmacs; i++) if (!strcmp(M.macs[i].id, M.job.result.id)) M.needs_repair[i] = false;
            M.repair_strikes = 0;
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
        ui_rrect(56, 230, CW, 440, 28, C_CARD); ui_rrect_line(56, 230, CW, 440, 28, 2, C_LINE);
        ui_text(100, 266, "No Mac paired yet", 42, true, C_TEXT);
        ui_paragraph(100, 336, CW - 90, "Install Sideframe on your Mac first if you haven’t (download it from the Sideframe project page).\n1.  On your Mac, click the Sideframe icon in the menu bar and choose “Pair a Steam Frame…”.\n2.  Come back here, tap Add a Mac, pick your Mac and type the code it shows.", 28, false, C_TEXT_DIM);
        if (ui_button(100, 570, 340, 72, "Add a Mac", 0, true, 30)) menu_goto(SCR_ADD_PICK);
        if (ui_button(MENU_WIDTH - 56 - 240, MENU_HEIGHT - 92, 240, 60, "Close Sideframe", 3, true, 26)) M.action = ACT_QUIT;
        if (ui_button(56, MENU_HEIGHT - 92, 240, 60, "Settings", 3, true, 26)) menu_goto(SCR_SETTINGS);
        return;
    }
    if (M.live) {   /* streaming: the Mac is in the Sideframe tab; this tab is for Disconnect and Settings */
        ui_rrect(56, 230, CW, 290, 28, C_CARD); ui_rrect_line(56, 230, CW, 290, 28, 2, C_LINE);
        ui_circle(100, 292, 11, C_GREEN);
        char b[200]; snprintf(b, sizeof b, "Connected to %s", M.target.name); ui_text(130, 272, b, 40, true, C_TEXT);
        ui_paragraph(100, 346, CW - 90, "Your Mac is paused while this menu is open. Close Sideframe with the Close button under the tab in the dashboard.", 25, false, C_TEXT_DIM);
        if (ui_button(100, 430, 300, 60, "Back to Mac", 0, true, 28)) { M.action = ACT_RESUME; M.dirty = true; }
        if (ui_button(420, 430, 300, 60, "Disconnect", 2, true, 28)) { M.action = ACT_DISCONNECT; M.dirty = true; }
        if (ui_button(56, MENU_HEIGHT - 92, 240, 60, "Settings", 1, true, 26)) menu_goto(SCR_SETTINGS);
        return;
    }
    ui_text(56, 200, "YOUR MACS", 20, true, C_TEXT_FAINT);
    int shown = M.nmacs > 3 ? 3 : M.nmacs;
    for (int i = 0; i < shown; i++) {
        float y = 236 + i * 148;
        uint32_t col; struct disc_mac *f; const char *st = mac_status(i, &col, &f);
        bool hot = ui_hot(56, y, CW, 112);
        ui_rrect(56, y, CW, 112, 26, hot ? C_CARD_HOT : C_CARD); ui_rrect_line(56, y, CW, 112, 26, 2, C_LINE);
        ui_circle(98, y + 56, 9, col);
        ui_text(130, y + 18, M.macs[i].name, 34, true, C_TEXT);
        ui_text(130, y + 64, st, 24, false, C_TEXT_DIM);
        float bx = 56 + CW - 24;
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
    if (M.nmacs > 3) { char b[64]; snprintf(b, sizeof b, "…and %d more", M.nmacs - 3); ui_text(56, 700, b, 22, false, C_TEXT_FAINT); }
    if (ui_button(56, MENU_HEIGHT - 92, 280, 60, "Add a Mac", 1, true, 26)) menu_goto(SCR_ADD_PICK);
    if (ui_button(MENU_WIDTH - 56 - 300, MENU_HEIGHT - 92, 300, 60, "Close Sideframe", 3, true, 26)) M.action = ACT_QUIT;
    if (ui_button(356, MENU_HEIGHT - 92, 240, 60, "Settings", 1, true, 26)) menu_goto(SCR_SETTINGS);
}

static void screen_add_pick(double t) {
    draw_banner();
    if (ui_button(40, 36, 150, 56, "‹  Back", 3, true, 26)) { menu_goto(SCR_HOME); return; }
    ui_text(56, 110, "Add a Mac", 54, true, C_TEXT);
    ui_paragraph(56, 184, CW, "On your Mac, click the Sideframe icon in the menu bar and choose “Pair a Steam Frame…”. Then pick your Mac below.", 28, false, C_TEXT_DIM);
    int n = 0;
    for (int k = 0; k < DISC_MAX; k++) {
        if (!disc_macs[k].used) continue;
        float y = 300 + n * 120; if (y > 780) break;
        bool paired = false; for (int i = 0; i < M.nmacs; i++) if (!strcmp(M.macs[i].id, disc_macs[k].id)) paired = true;
        bool hot = ui_hot(56, y, CW, 96);
        ui_rrect(56, y, CW, 96, 24, hot ? C_CARD_HOT : C_CARD); ui_rrect_line(56, y, CW, 96, 24, 2, hot ? C_ACCENT : C_LINE);
        ui_circle(98, y + 48, 9, C_GREEN);
        ui_text(130, y + 14, disc_macs[k].name, 32, true, C_TEXT);
        char sub[200]; snprintf(sub, sizeof sub, "%s%s", disc_macs[k].ip[0] ? disc_macs[k].ip : disc_macs[k].host, paired ? "  ·  already paired (pair again to replace)" : "");
        ui_text(130, y + 56, sub, 22, false, C_TEXT_DIM);
        ui_text(MENU_WIDTH - 56 - 150, y + 26, "Choose  ›", 28, true, C_ACCENT);
        if (ui_clicked(56, y, CW, 96)) {
            snprintf(M.sel_name, sizeof M.sel_name, "%s", disc_macs[k].name); snprintf(M.sel_id, sizeof M.sel_id, "%s", disc_macs[k].id);
            snprintf(M.sel_host, sizeof M.sel_host, "%s", disc_macs[k].ip[0] ? disc_macs[k].ip : disc_macs[k].host); M.sel_port = disc_macs[k].port;
            M.code[0] = 0; M.ncode = 0; menu_goto(SCR_ADD_CODE); return;
        }
        n++;
    }
    if (n == 0) {
        ui_spinner(100, 420, 26, t);
        char dots[8] = "..."; dots[(int)(t * 2) % 4] = 0;
        char b[64]; snprintf(b, sizeof b, "Looking for Macs%s", dots);
        ui_text(150, 396, b, 32, true, C_TEXT);
        if (disc_now() - M.search_t0 < 6.0) ui_paragraph(150, 452, CW - 100, "Make sure Sideframe is running on your Mac (look for its icon in the menu bar) and that your Mac and this Frame are on the same Wi-Fi.", 24, false, C_TEXT_DIM);
        else {   /* nothing found for a while: spell out the usual causes */
            ui_text(150, 452, "Still nothing? Check these:", 26, true, C_TEXT);
            float yy = 500;
            char l1[300]; snprintf(l1, sizeof l1, "1.  Is Sideframe installed on your Mac? %s%s", SIDEFRAME_URL[0] ? "Get it at " : "Download it from the Sideframe project page.", SIDEFRAME_URL);
            yy += ui_paragraph(150, yy, CW - 100, l1, 24, false, C_TEXT_DIM) + 8;
            yy += ui_paragraph(150, yy, CW - 100, "2.  Is its icon in the Mac’s menu bar? If not, open Sideframe from Applications.", 24, false, C_TEXT_DIM) + 8;
            yy += ui_paragraph(150, yy, CW - 100, "3.  Are your Mac and this Frame on the same Wi-Fi network (not a guest network)?", 24, false, C_TEXT_DIM) + 8;
            ui_paragraph(150, yy, CW - 100, "4.  If your Mac asked to allow incoming connections, choose Allow.", 24, false, C_TEXT_DIM);
        }
    }
}

static void screen_add_code(double t) {
    if (ui_button(40, 36, 150, 56, "‹  Back", 3, true, 26)) { menu_goto(SCR_ADD_PICK); return; }
    ui_text(56, 110, "Enter the code", 54, true, C_TEXT);
    char sub[200]; snprintf(sub, sizeof sub, "shown on “%s”", M.sel_name);
    ui_text(58, 176, sub, 30, false, C_TEXT_DIM);
    /* the 8 digit boxes */
    float bw = 62, bh = 96, gap = 8, x0 = 56, y0 = 300;
    for (int i = 0; i < 8; i++) {
        float x = x0 + i * (bw + gap) + (i >= 4 ? 28 : 0);
        bool cur = (i == M.ncode);
        ui_rrect(x, y0, bw, bh, 20, C_CARD); ui_rrect_line(x, y0, bw, bh, 20, cur ? 3 : 2, cur ? C_ACCENT : C_LINE);
        if (i < M.ncode) { char c[2] = { M.code[i], 0 }; ui_text_center(x + bw / 2, y0 + 20, c, 48, true, C_TEXT); }
    }
    if (M.err[0]) { ui_dry = true; float h = ui_paragraph(0, 0, 620, M.err, 24, false, C_RED); ui_dry = false; ui_paragraph(56, 440, 620, M.err, 24, false, C_RED); (void)h; }
    else ui_paragraph(56, 440, 620, "Open “Pair a Steam Frame…” on your Mac to see the code. It stays the same until you close that window or ask for a new one.", 22, false, C_TEXT_FAINT);
    if (ui_button(56, 760, 300, 80, "Pair", 0, M.ncode == 8, 32)) menu_start_pairing();
    if (ui_button(376, 760, 180, 80, "Clear", 1, M.ncode > 0, 28)) { M.ncode = 0; M.code[0] = 0; M.err[0] = 0; M.dirty = true; }
    /* number pad */
    const char *keys[12] = { "1", "2", "3", "4", "5", "6", "7", "8", "9", "Clear", "0", "Delete" };
    for (int r = 0; r < 4; r++) for (int c = 0; c < 3; c++) {
        int k = r * 3 + c; float x = MENU_WIDTH - 56 - 474 + c * 162, y = 200 + r * 136;
        if (k == 9) continue;   /* Clear is on the left; leave the slot empty */
        bool del = (k == 11);
        if (ui_button(x, y, 150, 116, keys[k], del ? 1 : 1, true, del ? 24 : 40)) {
            if (del) { if (M.ncode > 0) { M.code[--M.ncode] = 0; M.err[0] = 0; M.dirty = true; } }
            else if (M.ncode < 8) { M.code[M.ncode++] = keys[k][0]; M.code[M.ncode] = 0; M.err[0] = 0; M.dirty = true;
                if (M.ncode == 8) menu_start_pairing(); }   /* eight digits: pair right away */
        }
    }
}

static void screen_pairing(double t) {
    ui_spinner(MENU_WIDTH / 2, 400, 48, t);
    char b[200]; snprintf(b, sizeof b, "Pairing with “%s”", M.sel_name);
    ui_text_center(MENU_WIDTH / 2, 480, b, 44, true, C_TEXT);
    ui_text_center(MENU_WIDTH / 2, 550, "This takes a few seconds.", 28, false, C_TEXT_DIM);
}

static void screen_connecting(double t) {
    ui_spinner(MENU_WIDTH / 2, 380, 48, t);
    char b[200]; snprintf(b, sizeof b, "Connecting to “%s”", M.target.name);
    ui_text_center(MENU_WIDTH / 2, 460, b, 44, true, C_TEXT);
    ui_text_center(MENU_WIDTH / 2, 530, M.connect_status[0] ? M.connect_status : "Starting the stream…", 26, false, C_TEXT_DIM);
    if (ui_button(MENU_WIDTH / 2 - 130, 640, 260, 72, "Cancel", 1, true, 28)) M.action = ACT_CANCEL_CONNECT;
}

static void screen_closing(double t) {
    ui_logo(MENU_WIDTH / 2 - 56, 350, 112);
    ui_text_center(MENU_WIDTH / 2, 490, "Closing Sideframe", 48, true, C_TEXT);
    ui_text_center(MENU_WIDTH / 2, 560, "Tap Sideframe in your app list to open it again.", 26, false, C_TEXT_DIM);
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
static void screen_settings(double t) {
    if (ui_button(40, 36, M.live ? 260 : 150, 56, M.live ? "‹  Back to Mac" : "‹  Back", 3, true, 26)) { if (M.live) M.action = ACT_RESUME; else menu_goto(SCR_HOME); return; }
    ui_text(56, 112, "Settings", 50, true, C_TEXT);
    if (ui_button(MENU_WIDTH - 56 - 230, 108, 230, 56, "Advanced  ›", 1, true, 26)) menu_goto(SCR_ADVANCED);
    /* frame rate */
    float y = 214;
    ui_text(56, y, "FRAME RATE", 18, true, C_TEXT_FAINT);
    if (ui_button(56, y + 28, 150, 58, "30", ST.fps == 30 ? 0 : 1, true, 26)) { ST.fps = 30; settings_save(); M.dirty = true; }
    if (ui_button(220, y + 28, 150, 58, "60", ST.fps == 60 ? 0 : 1, true, 26)) { ST.fps = 60; settings_save(); M.dirty = true; }
    ui_paragraph(400, y + 28, MENU_WIDTH - 56 - 400, ST.fps == 30 ? "30 pictures a second. Each one gets more detail, and it is easier on the Frame and your Wi-Fi."
                                                       : "60 pictures a second: smoother cursor and motion. Needs a good Wi-Fi link and runs warmer.", 22, false, C_TEXT_DIM);
    /* picture quality */
    y = 360;
    ui_text(56, y, "PICTURE QUALITY", 18, true, C_TEXT_FAINT);
    if (ui_button(56, y + 28, 200, 58, "Quality", ST.preset == P_QUALITY ? 0 : 1, true, 24)) { ST.preset = P_QUALITY; settings_save(); M.dirty = true; }
    if (ui_button(270, y + 28, 200, 58, "Balanced", ST.preset == P_BALANCED ? 0 : 1, true, 24)) { ST.preset = P_BALANCED; settings_save(); M.dirty = true; }
    if (ui_button(484, y + 28, 200, 58, "Performance", ST.preset == P_PERFORMANCE ? 0 : 1, true, 24)) { ST.preset = P_PERFORMANCE; settings_save(); M.dirty = true; }
    float h = ui_paragraph(56, y + 106, CW, ST.preset == P_QUALITY ? "Your Mac’s full resolution and the most data: the sharpest text. Warmest, and wants a strong Wi-Fi link."
                              : ST.preset == P_BALANCED ? "Three quarters of your Mac’s resolution: a good middle that is easy on the Frame and the network."
                              : "Half your Mac’s resolution and less data: coolest and quietest, softer text.", 22, false, C_TEXT_DIM);
    ui_text(56, y + 106 + h + 6, "Changes apply right away, even while you are connected.", 19, false, C_TEXT_FAINT);
    /* start with SteamVR */
    y = 560;
    ui_text(56, y, "STARTUP", 18, true, C_TEXT_FAINT);
    if (ui_button(56, y + 28, 150, 58, "Off", !ST.autostart ? 0 : 1, true, 26)) { if (ST.autostart) { M.set_err[0] = 0; settings_set_autostart(false, M.set_err, sizeof M.set_err); M.dirty = true; } }
    if (ui_button(220, y + 28, 150, 58, "On", ST.autostart ? 0 : 1, true, 26)) { if (!ST.autostart) { M.set_err[0] = 0; settings_set_autostart(true, M.set_err, sizeof M.set_err); M.dirty = true; } }
    ui_paragraph(400, y + 24, MENU_WIDTH - 56 - 400, "Start Sideframe together with SteamVR, so its tab is always in the dashboard. Off by default; turning it off removes it again.", 22, false, C_TEXT_DIM);
    if (M.set_err[0]) ui_text(56, y + 100, M.set_err, 20, false, C_RED);
    /* about + updates + temperature */
    float cy = MENU_HEIGHT - 56 - 150;
    ui_rrect(56, cy, CW, 150, 24, C_CARD); ui_rrect_line(56, cy, CW, 150, 24, 2, C_LINE);
    char b[160]; uint32_t c = C_TEXT_DIM;
    ui_text(84, cy + 16, "Sideframe", 28, true, C_TEXT); ui_text(84 + ui_text_w("Sideframe", 28, true) + 12, cy + 20, SIDEFRAME_VERSION, 22, false, C_TEXT_DIM);
    if (!SIDEFRAME_UPDATE_URL[0]) b[0] = 0;
    else switch (M.upd_state) {
        case 1: snprintf(b, sizeof b, "Checking for updates…"); break;
        case 2: snprintf(b, sizeof b, "You have the latest version."); c = C_GREEN; break;
        case 3: snprintf(b, sizeof b, "Version %s is available — download it from %s", M.upd_ver, SIDEFRAME_URL[0] ? SIDEFRAME_URL : "the project page"); c = C_AMBER; break;
        case 4: snprintf(b, sizeof b, "Couldn’t check for updates (no internet?)."); break;
        default: snprintf(b, sizeof b, "Updates are only checked when you press the button."); c = C_TEXT_FAINT;
    }
    if (b[0]) ui_text(84, cy + 58, b, 20, false, c);
    if (SIDEFRAME_UPDATE_URL[0] && M.upd_state != 1 && ui_button(56 + CW - 28 - 240, cy + 18, 240, 44, "Check for updates", 1, true, 20)) menu_check_update();
    char tb[120]; if (therm_temp_c > 0) snprintf(tb, sizeof tb, "Frame temperature %d°C  ·  %s", therm_temp_c, therm_level == 0 ? "running normally" : therm_level == 1 ? "warm: easing off to cool down" : "hot: easing off a lot");
    else snprintf(tb, sizeof tb, "Frame temperature unavailable");
    ui_text(84, cy + 96, tb, 21, false, therm_level ? C_AMBER : C_TEXT_DIM);
    ui_text(84, cy + 122, "Sideframe eases off by itself above 88°C to protect the headset.", 18, false, C_TEXT_FAINT);
}

/* ---------------- Settings > Advanced: how the connection is doing, compared with other networks ---------------- */
static struct netinfo adv_ni; static double adv_ni_t;
static void adv_name(const struct netinfo *ni, char *out, size_t n) {
    if (ni->direct) snprintf(out, n, "Direct to your Mac  ·  %s", ni->name);
    else if (ni->wifi) snprintf(out, n, "Wi-Fi  ·  %s", ni->name[0] ? ni->name : "(unknown)");
    else snprintf(out, n, "%s", ni->name[0] ? ni->name : "Network");
}
static void adv_rec_name(const struct linkrec *r, char *out, size_t n) {
    if (r->direct) snprintf(out, n, "Direct link"); else if (!r->wifi) snprintf(out, n, "Wired network"); else snprintf(out, n, "%s", r->name);
}
/* a row of the comparison table: label, then one value per column */
static void adv_row(float y, const char *label, const char *a, const char *bv, uint32_t ca, uint32_t cb) {
    ui_text(686, y, label, 20, false, C_TEXT_DIM);
    ui_text(900, y - 1, a, 23, true, ca); ui_text(1058, y - 1, bv, 23, true, cb);
}
static uint32_t adv_better(double a, double b) { return a <= b * 0.8 ? C_GREEN : a >= b * 1.25 ? C_AMBER : C_TEXT; }   /* lower is better */
static void screen_advanced(double t) {
    if (ui_button(40, 36, 150, 56, "‹  Back", 3, true, 26)) { menu_goto(SCR_SETTINGS); return; }
    ui_text(56, 112, "Advanced", 50, true, C_TEXT);
    if (ui_button(MENU_WIDTH - 56 - 340, 108, 340, 56, "Set up a direct link  ›", 1, true, 24)) { menu_goto(SCR_DIRECT); return; }
    if (t - adv_ni_t > 2.0) { adv_ni_t = t; ls_net_read(&adv_ni); }
    char b[200], b2[200];
    float cy = 196, ch = 590;
    /* left: this connection */
    ui_rrect(56, cy, 580, ch, 24, C_CARD); ui_rrect_line(56, cy, 580, ch, 24, 2, C_LINE);
    ui_text(84, cy + 20, "THIS CONNECTION", 17, true, C_TEXT_FAINT);
    adv_name(&adv_ni, b, sizeof b);
    { float w = ui_text_w(b, 24, true); ui_text(84, cy + 52, b, w > 520 ? 20 : 24, true, C_TEXT); }
    if (adv_ni.wifi && adv_ni.freq) {
        snprintf(b, sizeof b, "%s  ·  %.0f Mbit/s link  ·  %d MHz  ·  %.0f dBm", ls_band(adv_ni.freq), adv_ni.rate, adv_ni.width, adv_ni.signal);
        ui_text(84, cy + 92, b, 19, false, C_TEXT_DIM);
    }
    float y = cy + 160;
    ui_text(84, y, "DELAY", 16, true, C_TEXT_FAINT);
    if (LS.ring_n) { snprintf(b, sizeof b, "typical %.0f ms  ·  worst %.0f ms", LS.rtt50, LS.rtt95); ui_text(84, y + 24, b, 25, true, C_TEXT); }
    else ui_text(84, y + 24, "connect to a Mac to measure", 22, false, C_TEXT_DIM);
    y += 104;
    ui_text(84, y, "PICTURE", 16, true, C_TEXT_FAINT);
    if (LS.fps > 0 || LS.mbps > 0) { snprintf(b, sizeof b, "%.0f/s  ·  %.1f Mbit/s  ·  %.1f%% re-sent", LS.fps, LS.mbps, LS.repair_pct); ui_text(84, y + 24, b, 25, true, C_TEXT); }
    else ui_text(84, y + 24, "no picture numbers yet", 22, false, C_TEXT_DIM);
    y += 104;
    ui_text(84, y, "MAC", 16, true, C_TEXT_FAINT);
    if (LS.mac_proto) {
        ls_fmt_version(LS.mac_app, b2, sizeof b2);
        if (LS.ap_fps) snprintf(b, sizeof b, "app %s  ·  up to %d/s at %.0f Mbit/s, quality level %d", b2, LS.ap_fps, LS.ap_kbps / 1000.0, LS.ap_qp);
        else snprintf(b, sizeof b, "app %s", b2);
        ui_paragraph(84, y + 24, 520, b, 21, false, C_TEXT);
    } else if (LS.ring_n) ui_paragraph(84, y + 24, 520, "an older Mac app (it doesn’t report its version): update it for the full picture", 21, false, C_AMBER);
    else ui_text(84, y + 24, "—", 22, false, C_TEXT_DIM);

    /* right: compare with the other networks */
    ui_rrect(660, cy, 564, ch, 24, C_CARD); ui_rrect_line(660, cy, 564, ch, 24, 2, C_LINE);
    ui_text(686, cy + 20, "COMPARE", 17, true, C_TEXT_FAINT);
    const struct linkrec *cur = ls_links_find(adv_ni.name, adv_ni.direct), *oth = NULL;
    for (int i = LR_N - 1; i >= 0; i--) if (&LR[i] != cur && LR[i].secs >= 20) { oth = &LR[i]; break; }
    if (!cur || !oth) {
        ui_paragraph(686, cy + 60, 512, !cur ? "Keep streaming for about 30 seconds on this connection and its numbers will appear here."
                                              : "Use Sideframe on another connection (for example a direct link to your Mac) and the two will be compared here.", 22, false, C_TEXT_DIM);
        ui_paragraph(686, cy + 250, 512, "Sideframe remembers the last stretch of streaming on each network it has run on. Nothing leaves your Frame.", 19, false, C_TEXT_FAINT);
    } else {
        adv_rec_name(cur, b, sizeof b); adv_rec_name(oth, b2, sizeof b2);
        if (strlen(b) > 11) strcpy(b + 9, "…"); if (strlen(b2) > 11) strcpy(b2 + 9, "…");
        ui_text(900, cy + 60, b, 20, true, C_ACCENT); ui_text(1058, cy + 60, b2, 20, true, C_TEXT);
        char va[40], vb[40]; float ry = cy + 130;
        snprintf(va, sizeof va, "%.0f ms", cur->rtt50); snprintf(vb, sizeof vb, "%.0f ms", oth->rtt50); adv_row(ry, "Typical delay", va, vb, adv_better(cur->rtt50, oth->rtt50), adv_better(oth->rtt50, cur->rtt50)); ry += 64;
        snprintf(va, sizeof va, "%.0f ms", cur->rtt95); snprintf(vb, sizeof vb, "%.0f ms", oth->rtt95); adv_row(ry, "Worst delay", va, vb, adv_better(cur->rtt95, oth->rtt95), adv_better(oth->rtt95, cur->rtt95)); ry += 64;
        snprintf(va, sizeof va, "%.1f%%", cur->repair); snprintf(vb, sizeof vb, "%.1f%%", oth->repair); adv_row(ry, "Re-sent", va, vb, adv_better(cur->repair + 0.05, oth->repair + 0.05), adv_better(oth->repair + 0.05, cur->repair + 0.05)); ry += 64;
        snprintf(va, sizeof va, "%.1f/min", cur->drops_pm); snprintf(vb, sizeof vb, "%.1f/min", oth->drops_pm); adv_row(ry, "Dropouts", va, vb, adv_better(cur->drops_pm + 0.05, oth->drops_pm + 0.05), adv_better(oth->drops_pm + 0.05, cur->drops_pm + 0.05)); ry += 64;
        snprintf(va, sizeof va, "%.0f Mb/s", cur->rate); snprintf(vb, sizeof vb, "%.0f Mb/s", oth->rate); adv_row(ry, "Link speed", va, vb, C_TEXT, C_TEXT);
        double sc = cur->rtt95 + 100 * cur->repair + 20 * cur->drops_pm, so = oth->rtt95 + 100 * oth->repair + 20 * oth->drops_pm;
        const char *v = sc < so * 0.8 ? "This connection is steadier." : sc > so * 1.25 ? "The other one was steadier." : "About the same.";
        ui_text(686, cy + ch - 64, v, 24, true, sc < so * 0.8 ? C_GREEN : sc > so * 1.25 ? C_AMBER : C_TEXT_DIM);
    }
    ui_paragraph(56, cy + ch + 20, CW, "Delay is the round trip to your Mac: lower and steadier means a smoother cursor. “Re-sent” counts packets lost by Wi-Fi that the Frame had to ask for again. "
                 "The Mac pauses while a menu is open, so picture numbers show the last stretch of streaming.", 18, false, C_TEXT_FAINT);
}

/* ---------------- Settings > Advanced > Direct link: step-by-step ---------------- */
static void screen_direct(double t) {
    (void)t;
    if (ui_button(40, 36, 150, 56, "‹  Back", 3, true, 26)) { menu_goto(SCR_ADVANCED); return; }
    ui_text(56, 112, "Direct link to your Mac", 46, true, C_TEXT);
    ui_paragraph(56, 190, CW, "Optional. If your Mac is plugged into your router with a cable, it can make its own Wi-Fi network just for the Frame. "
                 "That skips your router’s Wi-Fi, which is often the busiest part, and is usually smoother and steadier. Your Mac keeps its internet through the cable.", 22, false, C_TEXT_DIM);
    float cw2 = (CW - 24) / 2.0f, cy = 330, ch = 400;
    ui_rrect(56, cy, cw2, ch, 24, C_CARD); ui_rrect_line(56, cy, cw2, ch, 24, 2, C_LINE);
    ui_text(84, cy + 22, "ON YOUR MAC", 19, true, C_ACCENT);
    float y = cy + 64;
    y += ui_paragraph(84, y, cw2 - 56, "1.  Open System Settings › General › Sharing, and find Internet Sharing.", 22, false, C_TEXT) + 16;
    y += ui_paragraph(84, y, cw2 - 56, "2.  Share your connection from your Ethernet adapter, to Wi-Fi.", 22, false, C_TEXT) + 16;
    y += ui_paragraph(84, y, cw2 - 56, "3.  Click Wi-Fi Options: pick a network name and a password (WPA2/WPA3), then switch Internet Sharing on.", 22, false, C_TEXT);
    float x2 = 56 + cw2 + 24;
    ui_rrect(x2, cy, cw2, ch, 24, C_CARD); ui_rrect_line(x2, cy, cw2, ch, 24, 2, C_LINE);
    ui_text(x2 + 28, cy + 22, "ON YOUR FRAME", 19, true, C_ACCENT);
    y = cy + 64;
    y += ui_paragraph(x2 + 28, y, cw2 - 56, "1.  In the Frame’s Wi-Fi settings (Desktop Mode), join the network your Mac just made.", 22, false, C_TEXT) + 16;
    y += ui_paragraph(x2 + 28, y, cw2 - 56, "2.  Open Sideframe and connect as usual: your Mac appears on the new network.", 22, false, C_TEXT) + 16;
    y += ui_paragraph(x2 + 28, y, cw2 - 56, "3.  After a minute of use, open Settings › Advanced to see how it compares.", 22, false, C_TEXT);
    ui_paragraph(56, cy + ch + 24, CW, "To go back, join your home Wi-Fi again on the Frame. It can only be on one Wi-Fi network at a time, so it leaves your home network while it is on your Mac’s.", 19, false, C_TEXT_FAINT);
}

static void menu_draw(double t) {
    ui_clear_gradient(C_BG_TOP, C_BG_BOTTOM);
    switch (M.scr) {
    case SCR_SETTINGS: screen_settings(t); break;
    case SCR_ADVANCED: screen_advanced(t); break;
    case SCR_DIRECT: screen_direct(t); break;
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
