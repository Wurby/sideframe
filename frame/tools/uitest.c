// uitest — renders every menu screen to a PPM file so the layout can be checked without a headset.
//   gcc -O2 -I../src $(pkg-config --cflags freetype2) -o uitest uitest.c $(pkg-config --libs freetype2) -lm -lpthread   ; ./uitest outdir
#define _GNU_SOURCE
#include "../src/menu.h"

static void dump(const char *dir, const char *name) {
    char p[512]; snprintf(p, sizeof p, "%s/%s.ppm", dir, name);
    FILE *f = fopen(p, "wb"); if (!f) { perror(p); return; }
    fprintf(f, "P6\n%d %d\n255\n", UI.w, UI.h);
    for (int i = 0; i < UI.w * UI.h; i++) fwrite(UI.px + i * 4, 1, 3, f);
    fclose(f);
}
static void fake_mac(int slot, const char *name, const char *id) {
    struct mac_conf *c = &M.macs[slot]; memset(c, 0, sizeof *c);
    snprintf(c->name, sizeof c->name, "%s", name); snprintf(c->id, sizeof c->id, "%s", id); snprintf(c->host, sizeof c->host, "Alexs-MacBook-Air.local"); c->port = 5000;
}
int main(int argc, char **argv) {
    const char *dir = argc > 1 ? argv[1] : ".";
    UI.w = MENU_WIDTH; UI.h = MENU_HEIGHT; UI.px = malloc((size_t)UI.w * UI.h * 4);
    if (!ui_load_fonts("../assets/fonts")) return 1;
    memset(&M, 0, sizeof M); M.forget_armed = -1;
    struct disc_mac *d = &disc_macs[0]; memset(d, 0, sizeof *d); d->used = true; d->port = 5000; d->seen = disc_now();
    snprintf(d->name, sizeof d->name, "Alex’s MacBook Air"); snprintf(d->id, sizeof d->id, "457ac4cc5d950565"); snprintf(d->ip, sizeof d->ip, "192.168.0.109"); snprintf(d->host, sizeof d->host, "Alexs-MacBook-Air.local");
    UI.have_mouse = true; UI.mx = 1250; UI.my = 250;       /* pretend the pointer is over the first card's Connect button */

    fake_mac(0, "Alex’s MacBook Air", "457ac4cc5d950565"); M.nmacs = 1;
    M.scr = SCR_HOME; menu_draw(0); dump(dir, "1_home_online");

    snprintf(M.banner, sizeof M.banner, "Paired with “Alex’s MacBook Air”. Tap Connect to start."); M.banner_color = C_GREEN; M.banner_until = 1e9;
    M.needs_repair[0] = true; UI.mx = 1100; menu_draw(0); dump(dir, "2_home_pair_again_banner");

    M.banner[0] = 0; M.needs_repair[0] = false; fake_mac(1, "Studio Mac mini", "1122334455667788"); M.nmacs = 2; M.net_ok = true; M.dec_ok = true; M.health_t = 1; M.upd_state = 3; snprintf(M.upd_ver, sizeof M.upd_ver, "0.2.0"); menu_draw(0); dump(dir, "3_home_two_macs");
    M.net_ok = false; menu_draw(0); dump(dir, "3b_home_no_network"); M.net_ok = true; M.upd_state = 0;

    M.nmacs = 0; UI.mx = 300; UI.my = 450; menu_draw(0); dump(dir, "4_home_empty");

    M.scr = SCR_ADD_PICK; menu_draw(0.5); dump(dir, "5_add_pick");
    disc_macs[0].used = false; M.search_t0 = disc_now(); menu_draw(0.5); dump(dir, "6_add_pick_searching"); M.search_t0 = disc_now() - 10; menu_draw(0.5); dump(dir, "6b_add_pick_still_nothing");
    disc_macs[0].used = true;

    snprintf(M.sel_name, sizeof M.sel_name, "Alex’s MacBook Air"); M.scr = SCR_ADD_CODE; strcpy(M.code, "889"); M.ncode = 3; menu_draw(0); dump(dir, "7_add_code");
    snprintf(M.err, sizeof M.err, "That code is wrong."); strcpy(M.code, "88973370"); M.ncode = 8; UI.mx = 200; UI.my = 600; menu_draw(0); dump(dir, "8_add_code_error_full");

    M.scr = SCR_PAIRING; menu_draw(0.3); dump(dir, "9_pairing");
    M.scr = SCR_CLOSING; menu_draw(0); dump(dir, "11_closing");
    M.scr = SCR_CONNECTING; snprintf(M.target.name, sizeof M.target.name, "Alex’s MacBook Air"); snprintf(M.connect_status, sizeof M.connect_status, "Waiting for the Mac to answer…"); menu_draw(0.3); dump(dir, "10_connecting");
    {   /* the stream view: a stand-in Mac screen (3440x1440) with the toolbar (74 rows) below it, downscaled by dumping every 2nd pixel */
        int W = 3440, VH = 1440, TB = 74; uint8_t *fb = malloc((size_t)W * (VH + TB) * 4);
        for (int y = 0; y < VH; y++) for (int x = 0; x < W; x++) { uint8_t *p = fb + ((size_t)y * W + x) * 4; p[0] = 70 + 60 * x / W; p[1] = 90 + 50 * y / VH; p[2] = 150; p[3] = 255; }
        struct ui save = UI; for (int hv = -1; hv <= 1; hv += 2) {
            UI.px = fb + (size_t)VH * W * 4; UI.w = W; UI.h = TB; toolbar_paint(W, TB, hv < 0 ? -1 : 1, "Alex’s MacBook Air");
            char path[512]; snprintf(path, sizeof path, "%s/12_stream_toolbar%s.ppm", dir, hv < 0 ? "" : "_hover_close");
            FILE *f = fopen(path, "wb"); fprintf(f, "P6\n%d %d\n255\n", W, VH + TB);
            for (int i = 0; i < W * (VH + TB); i++) fwrite(fb + i * 4, 1, 3, f); fclose(f);
        }
        UI = save; free(fb);
    }
    printf("rendered 11 screens + the stream toolbar to %s\n", dir);
    return 0;
}
