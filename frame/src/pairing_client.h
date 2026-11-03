// pairing_client.h — the Frame's list of paired Macs (~/.config/sideframe/macs/*.conf) and the pairing client (docs/PROTOCOL.md section 2).
#ifndef PAIRING_CLIENT_H
#define PAIRING_CLIENT_H
#include <dirent.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <netdb.h>
#include <poll.h>
#include <unistd.h>
#include <time.h>
#include <errno.h>
#include <ctype.h>
#include <arpa/inet.h>
#include "mdcrypto.h"

struct mac_conf { char name[96]; char id[17]; uint8_t key[32]; char host[256]; int port; char path[512]; };

static bool cfg_dir(char *buf, size_t n, bool create) {
    const char *h = getenv("HOME"); if (!h) return false;
    char base[512]; snprintf(base, sizeof base, "%s/.config/sideframe", h);
    snprintf(buf, n, "%s/macs", base);
    if (create) { mkdir(base, 0700); mkdir(buf, 0700); }
    return true;
}

/* Reads a Mac config (key=value lines) or, for prototype-era installs, a bare 64-hex-digit key file. */
static bool mac_conf_load(const char *path, struct mac_conf *c) {
    memset(c, 0, sizeof *c); c->port = 5000; snprintf(c->path, sizeof c->path, "%s", path);
    FILE *f = fopen(path, "r"); if (!f) return false;
    char line[512]; bool have_key = false; int lines = 0;
    while (fgets(line, sizeof line, f)) {
        size_t n = strlen(line); while (n && (line[n-1] == '\n' || line[n-1] == '\r' || line[n-1] == ' ')) line[--n] = 0;
        lines++;
        if (!strncmp(line, "name=", 5)) snprintf(c->name, sizeof c->name, "%s", line + 5);
        else if (!strncmp(line, "id=", 3)) snprintf(c->id, sizeof c->id, "%s", line + 3);
        else if (!strncmp(line, "host=", 5)) snprintf(c->host, sizeof c->host, "%s", line + 5);
        else if (!strncmp(line, "port=", 5)) c->port = atoi(line + 5);
        else if (!strncmp(line, "key=", 4)) have_key = hex_decode(line + 4, c->key, 32);
        else if (lines == 1 && hex_decode(line, c->key, 32)) { have_key = true; snprintf(c->name, sizeof c->name, "Mac (earlier pairing)"); }
    }
    fclose(f);
    if (!c->name[0]) snprintf(c->name, sizeof c->name, "Mac");
    return have_key;
}

static bool mac_conf_save(const struct mac_conf *c) {
    char dir[512], path[640]; if (!cfg_dir(dir, sizeof dir, true)) return false;
    snprintf(path, sizeof path, "%s/%s.conf", dir, c->id[0] ? c->id : "mac");
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600); if (fd < 0) return false;
    FILE *f = fdopen(fd, "w"); if (!f) { close(fd); return false; }
    char hex[65]; sodium_bin2hex(hex, sizeof hex, c->key, 32);
    fprintf(f, "name=%s\nid=%s\nhost=%s\nport=%d\nkey=%s\n", c->name, c->id, c->host, c->port, hex);
    fclose(f); sodium_memzero(hex, sizeof hex); return true;
}

static int mac_conf_list(struct mac_conf *out, int max) {
    char dir[512]; if (!cfg_dir(dir, sizeof dir, false)) return 0;
    DIR *d = opendir(dir); if (!d) return 0;
    int n = 0; struct dirent *e;
    while ((e = readdir(d)) && n < max) {
        size_t l = strlen(e->d_name); if (l < 6 || strcmp(e->d_name + l - 5, ".conf")) continue;
        char p[640]; snprintf(p, sizeof p, "%s/%s", dir, e->d_name);
        if (mac_conf_load(p, &out[n])) n++;
    }
    closedir(d); return n;
}

/* Choose which paired Mac to use: the one matching host_hint, else the only one, else the prototype-era key file. */
static bool mac_conf_pick(const char *host_hint, struct mac_conf *out) {
    struct mac_conf all[16]; int n = mac_conf_list(all, 16);
    if (n > 0) {
        int pick = -1;
        if (host_hint) for (int i = 0; i < n; i++) if (!strcmp(all[i].host, host_hint)) { pick = i; break; }
        if (pick < 0 && n == 1) pick = 0;
        if (pick < 0 && !host_hint) pick = 0;
        if (pick < 0) pick = 0;   /* several Macs and no match: use the first; the app UI will let the user choose */
        *out = all[pick]; return true;
    }
    return false;
}

/* ---- the pairing client ---- */
static const char *pair_denied_text(int reason) {
    switch (reason) {
    case 1: return "Pairing isn't open on the Mac. Choose \"Pair a Steam Frame…\" in the Sideframe menu first.";
    case 2: return "That code is wrong.";
    case 3: return "The Mac is busy pairing another device. Try again in a few seconds.";
    case 4: return "Too many wrong codes. Start pairing again on the Mac to get a new code.";
    default: return "The Mac refused to pair.";
    }
}
static double pc_now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec * 1e-9; }

/* wait up to ms for one datagram; returns its length or -1 */
static int pc_recv(int fd, uint8_t *buf, size_t n, struct sockaddr_in *from, int ms) {
    struct pollfd p = { fd, POLLIN, 0 }; if (poll(&p, 1, ms) <= 0) return -1;
    socklen_t fl = sizeof *from; return (int)recvfrom(fd, buf, n, 0, (struct sockaddr *)from, &fl);
}

/* Returns 0 on success (and fills *out, saved to disk), or a negative code with a human-readable message in err. */
static int pair_run(const char *host, int port, const char *code_in, const char *frame_name, struct mac_conf *out, char *err, size_t errn) {
    char code[16] = {0}; size_t cl = 0;
    for (const char *p = code_in; *p && cl < 12; p++) if (isdigit((unsigned char)*p)) code[cl++] = *p;
    if (cl != 8) { snprintf(err, errn, "The pairing code is 8 digits."); return -1; }

    struct addrinfo hints = { .ai_family = AF_INET, .ai_socktype = SOCK_DGRAM }, *res = NULL; char ps[16]; snprintf(ps, sizeof ps, "%d", port);
    if (getaddrinfo(host, ps, &hints, &res) != 0 || !res) { snprintf(err, errn, "Can't find %s on the network.", host); return -2; }
    struct sockaddr_in dst; memcpy(&dst, res->ai_addr, sizeof dst); freeaddrinfo(res);
    int fd = socket(AF_INET, SOCK_DGRAM, 0);

    uint8_t skF[32], pkF[32], nF[16], req[1 + 1 + 32 + 16 + 1 + 64], buf[512];
    randombytes_buf(skF, 32); crypto_scalarmult_curve25519_base(pkF, skF); randombytes_buf(nF, 16);
    size_t nl = strlen(frame_name); if (nl > 48) nl = 48;
    req[0] = 0x10; req[1] = 1; memcpy(req + 2, pkF, 32); memcpy(req + 34, nF, 16); req[50] = (uint8_t)nl; memcpy(req + 51, frame_name, nl);

    struct sockaddr_in from = dst; int n = -1;
    for (int attempt = 0; attempt < 4 && n < 0; attempt++) {
        sendto(fd, req, 51 + nl, 0, (struct sockaddr *)&dst, sizeof dst);
        double t_end = pc_now() + 1.5;
        while (pc_now() < t_end) {
            int r = pc_recv(fd, buf, sizeof buf, &from, (int)((t_end - pc_now()) * 1000) + 1);
            if (r < 0) break;
            if (buf[0] == 0x1F && r >= 2) { close(fd); snprintf(err, errn, "%s", pair_denied_text(buf[1])); return -3; }
            if (buf[0] == 0x11 && r >= 1 + 32 + 16 + 16 + 8 + 1) { n = r; break; }
        }
    }
    if (n < 0) { close(fd); snprintf(err, errn, "No answer from %s. Is Sideframe running on the Mac?", host); return -4; }

    const uint8_t *pkM = buf + 1, *nM = buf + 33, *salt = buf + 49;
    uint8_t macId[8]; memcpy(macId, buf + 65, 8);                 /* copy now: buf is reused for the next packet */
    char macName[96] = {0}; size_t mnl = buf[73];
    if (74 + mnl > (size_t)n) mnl = (size_t)n > 74 ? (size_t)n - 74 : 0;
    if (mnl > sizeof macName - 1) mnl = sizeof macName - 1;
    for (size_t i = 0; i < mnl; i++) macName[i] = (buf[74 + i] < 32) ? ' ' : (char)buf[74 + i];
    uint8_t dh[32], ck[32], T[32], confF[32], confM_expect[32];
    if (crypto_scalarmult_curve25519(dh, skF, pkM) != 0) { close(fd); snprintf(err, errn, "The Mac sent an invalid key."); return -5; }
    pair_code_key(ck, code, salt, nF, nM, PAIR_ITERATIONS);
    pair_transcript(T, pkF, pkM, nF, nM, salt);
    pair_confirm(confF, 'F', ck, T, dh);
    uint8_t conf[33]; conf[0] = 0x12; memcpy(conf + 1, confF, 32);
    sendto(fd, conf, sizeof conf, 0, (struct sockaddr *)&from, sizeof from);

    int r = -1; double t_end = pc_now() + 8;
    while (pc_now() < t_end) {
        r = pc_recv(fd, buf, sizeof buf, &from, (int)((t_end - pc_now()) * 1000) + 1);
        if (r < 0) break;
        if (buf[0] == 0x1F && r >= 2) { close(fd); snprintf(err, errn, "%s", pair_denied_text(buf[1])); return -3; }
        if (buf[0] == 0x13 && r == 1 + 32 + 8) break;
        r = -1;
    }
    close(fd);
    if (r < 0) { snprintf(err, errn, "The Mac didn't answer. Start pairing again."); return -4; }
    pair_confirm(confM_expect, 'M', ck, T, dh);
    if (sodium_memcmp(confM_expect, buf + 1, 32) != 0) { snprintf(err, errn, "The Mac's answer didn't check out — this isn't the Mac that showed the code."); return -6; }

    memset(out, 0, sizeof *out);
    pair_psk(out->key, dh, T);
    snprintf(out->name, sizeof out->name, "%s", macName[0] ? macName : "Mac");
    sodium_bin2hex(out->id, sizeof out->id, macId, 8);
    snprintf(out->host, sizeof out->host, "%s", host); out->port = port;
    sodium_memzero(skF, 32); sodium_memzero(dh, 32); sodium_memzero(ck, 32);
    return 0;
}
#endif
