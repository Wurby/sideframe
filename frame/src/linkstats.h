// linkstats.h — what the connection is doing right now, what the Mac says it applied, which network we are on, and a small memory of how
// each network performed last time (so Settings > Advanced can compare, e.g., home Wi-Fi with a direct link to the Mac).
#ifndef LINKSTATS_H
#define LINKSTATS_H
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>
#include <time.h>
#include <limits.h>
#include <unistd.h>
#include <arpa/inet.h>

#define PROTOCOL_VERSION 1          /* docs/PROTOCOL.md; bump only for a change older apps cannot ignore */
#define LS_RING 200                 /* ~50 s of ping round trips (4 per second) */
#define LS_SESS_MAX 20000

struct netinfo { bool ok, wifi, direct; char name[64]; char iface[24]; int freq; double signal, rate; int width; };
struct linkrec { char name[64]; bool direct, wifi; int freq; double rate, rtt50, rtt95, repair, drops_pm, secs; long when; };

static struct linkstats {
    /* reported by the Mac */
    int mac_proto; uint32_t mac_app; int ap_fps, ap_pct, ap_qp; unsigned ap_kbps; double ap_t;
    /* live numbers (updated once a second while pictures are arriving) */
    bool live; double fps, mbps, repair_pct, rtt50, rtt95;
    float ring[LS_RING]; int ring_n, ring_i;
    /* this session, for the comparison */
    float *sess; int sess_n; unsigned long pk0, retx0, resync0; double t0, live_secs, save_t;
    /* sampling state */
    double tick_t; unsigned long p_present, p_bytes, p_pk, p_retx, p_present_prev, p_bytes_prev;
    unsigned long w_pk[10], w_retx[10]; int w_i;
    uint32_t peer_ip;                /* network byte order: the Mac's address */
    char sess_net[96];               /* which network the running measurement belongs to */
    int hello_tries; double hello_t;
} LS;

static uint32_t ls_pack_version(const char *v) { unsigned a = 0, b = 0, c = 0; sscanf(v ? v : "", "%u.%u.%u", &a, &b, &c); return a << 16 | b << 8 | c; }
static void ls_fmt_version(uint32_t v, char *out, size_t n) { snprintf(out, n, "%u.%u.%u", v >> 16, (v >> 8) & 255, v & 255); }

static int ls_cmpf(const void *a, const void *b) { float x = *(const float *)a, y = *(const float *)b; return x < y ? -1 : x > y; }
static double ls_pct(const float *v, int n, double p) {
    if (n <= 0) return 0; float *c = malloc(sizeof(float) * n); memcpy(c, v, sizeof(float) * n); qsort(c, n, sizeof(float), ls_cmpf);
    double r = c[(int)((n - 1) * p + 0.5)]; free(c); return r;
}
static void ls_rtt_add(double s) {
    if (s < 0 || s > 2.0) return;
    LS.ring[LS.ring_i] = (float)(s * 1e3); LS.ring_i = (LS.ring_i + 1) % LS_RING; if (LS.ring_n < LS_RING) LS.ring_n++;
    if (!LS.sess) LS.sess = malloc(sizeof(float) * LS_SESS_MAX);
    if (LS.sess && LS.sess_n < LS_SESS_MAX) LS.sess[LS.sess_n++] = (float)(s * 1e3);
}
/* a 12-byte status message from the Mac (see docs/PROTOCOL.md: 0x20 VERSION, 0x21 APPLIED) */
static void ls_mac_status(const uint8_t *p) {
    uint32_t a = (uint32_t)p[4] << 24 | p[5] << 16 | p[6] << 8 | p[7], b = (uint32_t)p[8] << 24 | p[9] << 16 | p[10] << 8 | p[11];
    if (p[0] == 0x20) { LS.mac_proto = p[1]; LS.mac_app = a; }
    else if (p[0] == 0x21) { LS.ap_qp = p[1]; LS.ap_fps = (int)(a >> 16); LS.ap_pct = (int)(a & 0xFFFF); LS.ap_kbps = b; LS.ap_t = 0; /* stamped by the caller */ }
}
static void ls_net_read(struct netinfo *ni);
static void ls_session_reset(uint32_t peer_ip_be) {
    LS.mac_proto = 0; LS.mac_app = 0; LS.ap_fps = 0; LS.hello_tries = 0; LS.hello_t = 0; LS.tick_t = 0;
    LS.live = false; LS.peer_ip = peer_ip_be; LS.w_i = 0; memset(LS.w_pk, 0, sizeof LS.w_pk); memset(LS.w_retx, 0, sizeof LS.w_retx);
    /* a reconnect on the same network keeps adding to the same measurement; a different network starts a new one */
    struct netinfo ni; ls_net_read(&ni); char key[96]; snprintf(key, sizeof key, "%s|%d", ni.name, ni.direct);
    if (strcmp(key, LS.sess_net) != 0) { snprintf(LS.sess_net, sizeof LS.sess_net, "%s", key); LS.sess_n = 0; LS.live_secs = 0; LS.save_t = 0; LS.ring_n = LS.ring_i = 0; }
}

/* ---------- which network are we on ---------- */
static uint32_t ls_default_gateway(char *iface, size_t n) {   /* /proc/net/route: Destination 00000000 -> Gateway (little-endian hex) */
    FILE *f = fopen("/proc/net/route", "r"); if (!f) return 0; char l[256], ifn[32]; unsigned long dest, gw; uint32_t r = 0;
    if (!fgets(l, sizeof l, f)) { fclose(f); return 0; }
    while (fgets(l, sizeof l, f)) if (sscanf(l, "%31s %lx %lx", ifn, &dest, &gw) == 3 && dest == 0) { r = (uint32_t)gw; if (iface) snprintf(iface, n, "%s", ifn); break; }
    fclose(f); return r;
}
static void ls_unescape(char *s) {   /* iw prints non-ASCII SSID bytes as \xNN */
    char *o = s; for (char *i = s; *i; ) { unsigned v; if (i[0] == '\\' && i[1] == 'x' && sscanf(i + 2, "%2x", &v) == 1) { *o++ = (char)v; i += 4; } else *o++ = *i++; } *o = 0;
}
static void ls_net_read(struct netinfo *ni) {
    memset(ni, 0, sizeof *ni); uint32_t gw = ls_default_gateway(ni->iface, sizeof ni->iface); if (!ni->iface[0]) return;
    char p[96]; snprintf(p, sizeof p, "/sys/class/net/%s/wireless", ni->iface); ni->wifi = access(p, F_OK) == 0;
    ni->direct = gw && LS.peer_ip && gw == LS.peer_ip;        /* the Mac is this network's router: Internet Sharing / direct link */
    ni->ok = true;
    if (!ni->wifi) { snprintf(ni->name, sizeof ni->name, "Wired network"); return; }
    char cmd[96]; snprintf(cmd, sizeof cmd, "iw dev %s link 2>/dev/null", ni->iface); FILE *f = popen(cmd, "r"); if (!f) return; char l[256];
    while (fgets(l, sizeof l, f)) {
        char *s = l; while (*s == ' ' || *s == '\t') s++;
        if (!strncmp(s, "SSID: ", 6)) { snprintf(ni->name, sizeof ni->name, "%s", s + 6); ni->name[strcspn(ni->name, "\n")] = 0; ls_unescape(ni->name); }
        else if (!strncmp(s, "freq: ", 6)) ni->freq = atoi(s + 6);
        else if (!strncmp(s, "signal: ", 8)) ni->signal = atof(s + 8);
        else if (!strncmp(s, "rx bitrate: ", 12)) { ni->rate = atof(s + 12); char *m = strstr(s, "MHz"); if (m) { char *q = m; while (q > s && q[-1] >= '0' && q[-1] <= '9') q--; ni->width = atoi(q); } }
    }
    pclose(f);
}
static const char *ls_band(int freq) { return freq >= 5925 ? "6 GHz" : freq >= 4900 ? "5 GHz" : freq >= 2400 ? "2.4 GHz" : ""; }

/* ---------- memory of how each network did (~/.config/sideframe/links.conf) ---------- */
#define LS_RECS 8
static struct linkrec LR[LS_RECS]; static int LR_N;
static void ls_links_path(char *out, size_t n) { const char *h = getenv("HOME"); snprintf(out, n, "%s/.config/sideframe/links.conf", h ? h : "."); }
static void ls_links_load(void) {
    char p[PATH_MAX]; ls_links_path(p, sizeof p); FILE *f = fopen(p, "r"); LR_N = 0; if (!f) return; char l[400];
    while (LR_N < LS_RECS && fgets(l, sizeof l, f)) {
        struct linkrec r; memset(&r, 0, sizeof r); int d, w; char name[64] = "";
        char *tab = strchr(l, '\t'); if (!tab) continue; *tab = 0; snprintf(name, sizeof name, "%s", l);
        if (sscanf(tab + 1, "%d %d %d %lf %lf %lf %lf %lf %lf %ld", &d, &w, &r.freq, &r.rate, &r.rtt50, &r.rtt95, &r.repair, &r.drops_pm, &r.secs, &r.when) == 10) {
            r.direct = d; r.wifi = w; snprintf(r.name, sizeof r.name, "%s", name); LR[LR_N++] = r;
        }
    }
    fclose(f);
}
static void ls_links_save(void) {
    char p[PATH_MAX]; ls_links_path(p, sizeof p); FILE *f = fopen(p, "w"); if (!f) return;
    for (int i = 0; i < LR_N; i++) fprintf(f, "%s\t%d %d %d %.1f %.2f %.2f %.3f %.2f %.0f %ld\n", LR[i].name, LR[i].direct, LR[i].wifi, LR[i].freq, LR[i].rate, LR[i].rtt50, LR[i].rtt95, LR[i].repair, LR[i].drops_pm, LR[i].secs, LR[i].when);
    fclose(f);
}
static struct linkrec *ls_links_find(const char *name, bool direct) { for (int i = 0; i < LR_N; i++) if (!strcmp(LR[i].name, name) && LR[i].direct == direct) return &LR[i]; return NULL; }
static void ls_links_store(const struct netinfo *ni, double rtt50, double rtt95, double repair, double drops_pm, double secs) {
    struct linkrec *r = ls_links_find(ni->name, ni->direct);
    if (!r) { if (LR_N == LS_RECS) { memmove(LR, LR + 1, sizeof(LR[0]) * (LS_RECS - 1)); LR_N--; } r = &LR[LR_N++]; memset(r, 0, sizeof *r); }
    snprintf(r->name, sizeof r->name, "%s", ni->name); r->direct = ni->direct; r->wifi = ni->wifi; r->freq = ni->freq; r->rate = ni->rate;
    r->rtt50 = rtt50; r->rtt95 = rtt95; r->repair = repair; r->drops_pm = drops_pm; r->secs = secs; r->when = (long)time(NULL);
    ls_links_save();
}

/* Once a second from the receive loop: turn the raw counters into the numbers Settings > Advanced shows; every 15 s remember them for the comparison. */
static void ls_tick(double t, bool net_up, unsigned long presented, unsigned long bytes, unsigned long pk, unsigned long retx, unsigned long resyncs) {
    if (t - LS.tick_t < 1.0) return; bool first = LS.tick_t == 0; double dt = first ? 1.0 : t - LS.tick_t; LS.tick_t = t;
    if (first) { LS.p_pk = pk; LS.p_retx = retx; LS.p_present_prev = presented; LS.p_bytes_prev = bytes; return; }   /* baseline only: counters run from program start */
    unsigned long dpk = pk - LS.p_pk, dretx = retx - LS.p_retx;
    LS.live = net_up && dpk > 0;
    LS.p_present = presented; LS.p_bytes = bytes; LS.p_pk = pk; LS.p_retx = retx;
    if (LS.live) {      /* while a menu is open the Mac pauses: keep showing the last measured picture numbers instead of zeros */
        LS.fps = (presented - LS.p_present_prev) / dt; LS.mbps = (bytes - LS.p_bytes_prev) * 8.0 / dt / 1e6;
        LS.w_pk[LS.w_i] = dpk; LS.w_retx[LS.w_i] = dretx; LS.w_i = (LS.w_i + 1) % 10;
        unsigned long sp = 0, sr = 0; for (int i = 0; i < 10; i++) { sp += LS.w_pk[i]; sr += LS.w_retx[i]; }
        LS.repair_pct = sp ? 100.0 * sr / sp : 0;
    }
    LS.p_present_prev = presented; LS.p_bytes_prev = bytes;
    if (LS.ring_n) { LS.rtt50 = ls_pct(LS.ring, LS.ring_n, 0.5); LS.rtt95 = ls_pct(LS.ring, LS.ring_n, 0.95); }
    if (!LS.live) return;
    if (LS.live_secs == 0) { LS.pk0 = pk; LS.retx0 = retx; LS.resync0 = resyncs; }
    LS.live_secs += dt;
    if (LS.live_secs >= 20 && t - LS.save_t >= 15.0 && LS.sess_n >= 40) {
        LS.save_t = t; struct netinfo ni; ls_net_read(&ni);
        if (ni.ok && ni.name[0]) {
            unsigned long tp = pk - LS.pk0, tr = retx - LS.retx0;
            ls_links_store(&ni, ls_pct(LS.sess, LS.sess_n, 0.5), ls_pct(LS.sess, LS.sess_n, 0.95), tp ? 100.0 * tr / tp : 0, (resyncs - LS.resync0) * 60.0 / LS.live_secs, LS.live_secs);
        }
    }
}
#endif
