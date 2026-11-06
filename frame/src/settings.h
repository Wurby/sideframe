// settings.h — user settings (~/.config/sideframe/settings.conf), the thermal governor, and the opt-in "start with SteamVR" unit.
#ifndef SETTINGS_H
#define SETTINGS_H
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <unistd.h>
#include <dirent.h>
#include <limits.h>
#include <sys/stat.h>
#include <time.h>

/* Bitrate preset: how much video data the Mac sends and how large a picture the Frame hands to SteamVR. */
enum preset { P_QUALITY = 0, P_BALANCED = 1, P_PERFORMANCE = 2 };
static struct settings { int fps; int preset; bool autostart; } ST = { 30, P_BALANCED, false };   /* fps: the rate to aim for, 30 or 60 */

static void settings_path(char *out, size_t n) { const char *h = getenv("HOME"); snprintf(out, n, "%s/.config/sideframe/settings.conf", h ? h : "."); }
static void unit_paths(char *unit, char *link, size_t n) {
    const char *h = getenv("HOME"); if (!h) h = ".";
    snprintf(unit, n, "%s/.config/systemd/user/sideframe-autostart.service", h);
    snprintf(link, n, "%s/.config/systemd/user/steamvr.service.wants/sideframe-autostart.service", h);
}
static void settings_load(void) {
    char p[PATH_MAX]; settings_path(p, sizeof p); FILE *f = fopen(p, "r");
    if (f) {
        char l[128];
        while (fgets(l, sizeof l, f)) {
            if (!strncmp(l, "fps=60", 6)) ST.fps = 60; else if (!strncmp(l, "fps=30", 6)) ST.fps = 30;
            else if (!strncmp(l, "bitrate=quality", 15)) ST.preset = P_QUALITY; else if (!strncmp(l, "bitrate=balanced", 16)) ST.preset = P_BALANCED;
            else if (!strncmp(l, "bitrate=performance", 19)) ST.preset = P_PERFORMANCE;
        }
        fclose(f);
    }
    char u[PATH_MAX], k[PATH_MAX]; unit_paths(u, k, sizeof u); ST.autostart = access(k, F_OK) == 0;   /* the link's presence is the truth */
}
static void settings_save(void) {
    char p[PATH_MAX], d[PATH_MAX]; settings_path(p, sizeof p); snprintf(d, sizeof d, "%s", p); *strrchr(d, '/') = 0;
    char dd[PATH_MAX]; snprintf(dd, sizeof dd, "%s", d); *strrchr(dd, '/') = 0; mkdir(dd, 0755); mkdir(d, 0700);
    FILE *f = fopen(p, "w"); if (!f) return; fprintf(f, "fps=%d\nbitrate=%s\n", ST.fps, ST.preset == P_QUALITY ? "quality" : ST.preset == P_PERFORMANCE ? "performance" : "balanced"); fclose(f);
}

/* Opt-in: start the tab together with SteamVR (a user systemd unit like other dashboard apps use). Nothing is installed unless the user turns this on. */
static bool settings_set_autostart(bool on, char *err, size_t en) {
    char u[PATH_MAX], k[PATH_MAX]; unit_paths(u, k, sizeof u);
    if (!on) { unlink(k); unlink(u); ST.autostart = false; if (system("systemctl --user daemon-reload >/dev/null 2>&1") != 0) {} return true; }
    char exe[PATH_MAX]; ssize_t n = readlink("/proc/self/exe", exe, sizeof exe - 1);
    if (n <= 0) { snprintf(err, en, "Couldn’t find the Sideframe program."); return false; } exe[n] = 0;
    char d[PATH_MAX]; snprintf(d, sizeof d, "%s", u); *strrchr(d, '/') = 0;
    char dd[PATH_MAX]; snprintf(dd, sizeof dd, "%s", d); *strrchr(dd, '/') = 0; mkdir(dd, 0755); mkdir(d, 0755);
    char w[PATH_MAX]; snprintf(w, sizeof w, "%s/steamvr.service.wants", d); mkdir(w, 0755);
    FILE *f = fopen(u, "w"); if (!f) { snprintf(err, en, "Couldn’t write the startup file."); return false; }
    fprintf(f, "# Managed by Sideframe (Settings > Start with SteamVR)\n[Unit]\nDescription=Sideframe dashboard tab\nBindsTo=steamvr.service\nAfter=steamvr.service\n\n"
               "[Service]\nExecStart=\"%s\" --quiet\nRestart=on-failure\nRestartSec=5\nSlice=session.slice\nNice=5\n\n[Install]\nWantedBy=steamvr.service\n", exe);
    fclose(f); unlink(k);
    if (symlink("../sideframe-autostart.service", k) != 0) { snprintf(err, en, "Couldn’t enable it."); return false; }
    if (system("systemctl --user daemon-reload >/dev/null 2>&1") != 0) {}
    ST.autostart = true; return true;
}

/* ---------------------------------------------------------------- thermal governor */
static int therm_temp_c = 0;       /* hottest sensor, °C (0 = unknown) */
static int therm_level = 0;        /* 0 normal, 1 warm (ease off), 2 hot (ease off a lot) */
static double therm_t = -100;
static int therm_read(void) {
    int best = 0; DIR *d = opendir("/sys/class/thermal"); if (!d) return 0; struct dirent *e;
    while ((e = readdir(d))) {
        if (strncmp(e->d_name, "thermal_zone", 12)) continue;
        char p[300]; snprintf(p, sizeof p, "/sys/class/thermal/%s/temp", e->d_name); FILE *f = fopen(p, "r"); if (!f) continue;
        long v = 0; if (fscanf(f, "%ld", &v) == 1 && v > 0 && v < 150000 && v / 1000 > best) best = (int)(v / 1000); fclose(f);
    }
    closedir(d); return best;
}
/* call often (cheap: reads sysfs every 5 s). Returns true if the level changed.
 * Thresholds (hottest sensor): SteamVR alone keeps the Frame at 70-85 C under load and the kernel's own limits are 110 C (hot) / 115 C (critical),
 * so we only ease off when it is clearly running hotter than normal: warm at 88 C, hot at 93 C, with generous hysteresis so we don't flap. */
#define THERM_WARM 88
#define THERM_HOT 93
#define THERM_WARM_CLEAR 84
#define THERM_HOT_CLEAR 90
static bool therm_poll(double t) {
    if (t - therm_t < 5.0) return false; therm_t = t;
    therm_temp_c = therm_read(); int old = therm_level, c = therm_temp_c;
    if (c >= THERM_HOT) therm_level = 2;
    else if (therm_level == 2) { if (c <= THERM_HOT_CLEAR) therm_level = c >= THERM_WARM_CLEAR ? 1 : 0; }
    else if (c >= THERM_WARM) therm_level = 1;
    else if (therm_level == 1 && c <= THERM_WARM_CLEAR) therm_level = 0;
    return therm_level != old;
}

/* What the settings and the temperature add up to */
static double eff_min_interval(void) {            /* seconds between overlay updates */
    double q = ST.fps >= 60 ? 1.0 / 60 : 1.0 / 30;
    double th = therm_level == 2 ? 1.0 / 8 : therm_level == 1 ? 1.0 / 15 : 0;
    return th > q ? th : q;
}
/* widest texture sent, as a multiple of the headset's render width */
static double eff_size_factor(void) { return ST.preset == P_QUALITY ? 1.5 : ST.preset == P_BALANCED ? 1.2 : 1.0; }
static int eff_mac_fps(void) { int f = (int)(1.0 / eff_min_interval() + 0.5); return f < 8 ? 8 : f; }   /* the Mac sends only as many pictures as we will show */
/* percent of the Mac's automatic bitrate (about 5 Mbps per megapixel) */
static int eff_mac_bitrate_pct(void) {
    int p = ST.preset == P_QUALITY ? 140 : ST.preset == P_BALANCED ? 100 : 60;
    if (ST.fps >= 60) p = p * 3 / 2 > 200 ? 200 : p * 3 / 2;     /* twice the pictures need more data to look as good */
    if (therm_level) p = p * 6 / 10;
    return p < 25 ? 25 : p > 200 ? 200 : p;
}
#endif
