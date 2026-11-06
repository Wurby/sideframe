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

enum quality { Q_SHARP = 0, Q_EFFICIENT = 1 };
static struct settings { int quality; bool autostart; } ST = { Q_SHARP, false };

static void settings_path(char *out, size_t n) { const char *h = getenv("HOME"); snprintf(out, n, "%s/.config/sideframe/settings.conf", h ? h : "."); }
static void unit_paths(char *unit, char *link, size_t n) {
    const char *h = getenv("HOME"); if (!h) h = ".";
    snprintf(unit, n, "%s/.config/systemd/user/sideframe-autostart.service", h);
    snprintf(link, n, "%s/.config/systemd/user/steamvr.service.wants/sideframe-autostart.service", h);
}
static void settings_load(void) {
    char p[PATH_MAX]; settings_path(p, sizeof p); FILE *f = fopen(p, "r");
    if (f) { char l[128]; while (fgets(l, sizeof l, f)) { if (!strncmp(l, "quality=efficient", 17)) ST.quality = Q_EFFICIENT; else if (!strncmp(l, "quality=sharp", 13)) ST.quality = Q_SHARP; } fclose(f); }
    char u[PATH_MAX], k[PATH_MAX]; unit_paths(u, k, sizeof u); ST.autostart = access(k, F_OK) == 0;   /* the link's presence is the truth */
}
static void settings_save(void) {
    char p[PATH_MAX], d[PATH_MAX]; settings_path(p, sizeof p); snprintf(d, sizeof d, "%s", p); *strrchr(d, '/') = 0;
    char dd[PATH_MAX]; snprintf(dd, sizeof dd, "%s", d); *strrchr(dd, '/') = 0; mkdir(dd, 0755); mkdir(d, 0700);
    FILE *f = fopen(p, "w"); if (!f) return; fprintf(f, "quality=%s\n", ST.quality == Q_EFFICIENT ? "efficient" : "sharp"); fclose(f);
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
static int therm_level = 0;        /* 0 normal, 1 warm (>=85 °C: ease off), 2 hot (>=90 °C: ease off a lot) */
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
/* call often (cheap: reads sysfs every 5 s). Returns true if the level changed. */
static bool therm_poll(double t) {
    if (t - therm_t < 5.0) return false; therm_t = t;
    therm_temp_c = therm_read(); int old = therm_level, c = therm_temp_c;
    if (c >= 90) therm_level = 2;
    else if (c >= 85) { if (therm_level < 1) therm_level = 1; else if (therm_level == 2 && c <= 86) therm_level = 1; }
    else if (c <= 80) therm_level = 0;
    else if (therm_level == 2) therm_level = 1;                 /* between 80 and 85: step down one level, then hold */
    return therm_level != old;
}

/* What the settings and the temperature add up to */
static double eff_min_interval(void) {            /* seconds between overlay updates */
    double q = ST.quality == Q_EFFICIENT ? 1.0 / 20 : 1.0 / 30;
    double th = therm_level == 2 ? 1.0 / 8 : therm_level == 1 ? 1.0 / 15 : 0;
    return th > q ? th : q;
}
static double eff_size_factor(void) { return ST.quality == Q_EFFICIENT ? 0.9 : 1.2; }   /* x the headset's render width: widest texture sent */
static int eff_mac_fps(void) { return therm_level == 2 ? 24 : (therm_level == 1 || ST.quality == Q_EFFICIENT) ? 30 : 60; }
static int eff_mac_bitrate_pct(void) { return therm_level ? 50 : ST.quality == Q_EFFICIENT ? 60 : 100; }
#endif
