// discovery.h — finds Sideframe Macs on the network (Bonjour / mDNS service "_sideframe._udp.local"), no dependencies.
// Sends a multicast DNS-SD query from an ephemeral port with the "unicast response" bit set (RFC 6762 §5.4), and also listens on 5353
// for announcements; parses PTR / SRV / TXT / A records (with name compression).
#ifndef DISCOVERY_H
#define DISCOVERY_H
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <time.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#define DISC_MAX 16
struct disc_mac { bool used; char name[96]; char id[17]; char ip[48]; char host[128]; int port; double seen; };
static struct disc_mac disc_macs[DISC_MAX];
static int disc_fd = -1, disc_fd2 = -1;
static double disc_query_t, disc_changed_t;

static double disc_now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec * 1e-9; }

static void disc_open(void) {
    if (disc_fd >= 0) return;
    disc_fd = socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK, 0);
    unsigned char ttl = 255; setsockopt(disc_fd, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof ttl);
    unsigned char loop = 0; setsockopt(disc_fd, IPPROTO_IP, IP_MULTICAST_LOOP, &loop, sizeof loop);
    /* second socket: join the mDNS group on 5353 (shared with other mDNS software) to also hear multicast announcements */
    disc_fd2 = socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK, 0);
    int one = 1; setsockopt(disc_fd2, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one); setsockopt(disc_fd2, SOL_SOCKET, SO_REUSEPORT, &one, sizeof one);
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons(5353), .sin_addr.s_addr = INADDR_ANY };
    if (bind(disc_fd2, (struct sockaddr *)&a, sizeof a) == 0) {
        struct ip_mreq mr = { .imr_multiaddr.s_addr = inet_addr("224.0.0.251"), .imr_interface.s_addr = INADDR_ANY };
        setsockopt(disc_fd2, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mr, sizeof mr);
    } else { close(disc_fd2); disc_fd2 = -1; }
}

static void disc_query(void) {
    if (disc_fd < 0) return;
    uint8_t q[128]; size_t n = 0;
    memset(q, 0, 12); q[5] = 1; n = 12;                                   /* one question */
    const char *labels[] = { "_sideframe", "_udp", "local" };
    for (int i = 0; i < 3; i++) { size_t l = strlen(labels[i]); q[n++] = (uint8_t)l; memcpy(q + n, labels[i], l); n += l; }
    q[n++] = 0; q[n++] = 0; q[n++] = 12; q[n++] = 0x80; q[n++] = 1;      /* PTR, class IN with the unicast-response bit */
    struct sockaddr_in to = { .sin_family = AF_INET, .sin_port = htons(5353), .sin_addr.s_addr = inet_addr("224.0.0.251") };
    sendto(disc_fd, q, n, 0, (struct sockaddr *)&to, sizeof to);
    disc_query_t = disc_now();
}

/* read a (possibly compressed) DNS name into out as dotted text; returns the offset just past the name in the packet */
static int dns_name(const uint8_t *p, int len, int off, char *out, size_t outn) {
    size_t o = 0; int jumped = 0, ret = -1, guard = 0;
    while (off < len && guard++ < 64) {
        uint8_t c = p[off];
        if (c == 0) { off++; break; }
        if ((c & 0xC0) == 0xC0) { if (off + 1 >= len) return -1; int ptr = ((c & 0x3F) << 8) | p[off + 1]; if (!jumped) ret = off + 2; jumped = 1; off = ptr; continue; }
        off++; if (off + c > len) return -1;
        if (o && o < outn - 1) out[o++] = '.';
        for (int i = 0; i < c && o < outn - 1; i++) out[o++] = (char)p[off + i];
        off += c;
    }
    out[o < outn ? o : outn - 1] = 0;
    return jumped ? ret : off;
}

struct rr { char owner[160]; int type; int off, len; };
static void disc_parse(const uint8_t *p, int len) {
    if (len < 12) return;
    int qd = (p[4] << 8) | p[5], an = (p[6] << 8) | p[7], ns = (p[8] << 8) | p[9], ar = (p[10] << 8) | p[11];
    int off = 12; char nm[256];
    for (int i = 0; i < qd; i++) { off = dns_name(p, len, off, nm, sizeof nm); if (off < 0) return; off += 4; }
    struct rr rrs[64]; int nr = 0;
    for (int i = 0; i < an + ns + ar && nr < 64; i++) {
        off = dns_name(p, len, off, rrs[nr].owner, sizeof rrs[nr].owner); if (off < 0 || off + 10 > len) return;
        int type = (p[off] << 8) | p[off + 1], rdlen = (p[off + 8] << 8) | p[off + 9]; off += 10;
        if (off + rdlen > len) return;
        rrs[nr].type = type; rrs[nr].off = off; rrs[nr].len = rdlen; nr++; off += rdlen;
    }
    for (int i = 0; i < nr; i++) {
        if (rrs[i].type != 12) continue;                                   /* PTR: _sideframe._udp.local -> instance */
        if (strcasecmp(rrs[i].owner, "_sideframe._udp.local")) continue;
        char inst[256]; dns_name(p, len, rrs[i].off, inst, sizeof inst);
        struct disc_mac m; memset(&m, 0, sizeof m); m.port = 5000; char target[160] = {0};
        char *dot = strstr(inst, "._sideframe"); if (dot) *dot = 0;
        snprintf(m.name, sizeof m.name, "%s", inst);
        char full[256]; snprintf(full, sizeof full, "%s._sideframe._udp.local", inst);
        for (int j = 0; j < nr; j++) {
            if (!strcasecmp(rrs[j].owner, full) && rrs[j].type == 33 && rrs[j].len >= 7) {            /* SRV: port, target host */
                m.port = (p[rrs[j].off + 4] << 8) | p[rrs[j].off + 5]; dns_name(p, len, rrs[j].off + 6, target, sizeof target);
            } else if (!strcasecmp(rrs[j].owner, full) && rrs[j].type == 16) {                         /* TXT: id=, name= */
                int o = rrs[j].off, e = rrs[j].off + rrs[j].len;
                while (o < e) { int l = p[o++]; if (o + l > e) break; char kv[200]; int c = l < 199 ? l : 199; memcpy(kv, p + o, c); kv[c] = 0; o += l;
                    if (!strncmp(kv, "id=", 3)) snprintf(m.id, sizeof m.id, "%s", kv + 3); else if (!strncmp(kv, "name=", 5)) snprintf(m.name, sizeof m.name, "%s", kv + 5); }
            }
        }
        snprintf(m.host, sizeof m.host, "%s", target);
        for (int j = 0; j < nr; j++) if (rrs[j].type == 1 && rrs[j].len == 4 && target[0] && !strcasecmp(rrs[j].owner, target)) {   /* A record of the target host */
            inet_ntop(AF_INET, p + rrs[j].off, m.ip, sizeof m.ip); break; }
        if (!m.id[0] && !m.ip[0]) continue;
        /* insert or refresh (key: id, else name) */
        int slot = -1;
        for (int k = 0; k < DISC_MAX; k++) if (disc_macs[k].used && ((m.id[0] && !strcmp(disc_macs[k].id, m.id)) || (!m.id[0] && !strcmp(disc_macs[k].name, m.name)))) { slot = k; break; }
        if (slot < 0) for (int k = 0; k < DISC_MAX; k++) if (!disc_macs[k].used) { slot = k; break; }
        if (slot < 0) continue;
        bool changed = !disc_macs[slot].used || strcmp(disc_macs[slot].ip, m.ip) || strcmp(disc_macs[slot].name, m.name) || disc_macs[slot].port != m.port;
        if (!m.ip[0] && disc_macs[slot].used) snprintf(m.ip, sizeof m.ip, "%s", disc_macs[slot].ip);   /* keep a known address when this answer has none */
        m.used = true; m.seen = disc_now(); disc_macs[slot] = m;
        if (changed) disc_changed_t = disc_now();
    }
}

/* call regularly (cheap): sends a query every 3 s, reads any answers, forgets Macs not heard from for 12 s. Returns true if the list changed. */
static bool disc_poll(void) {
    double t = disc_now(); bool changed = false;
    if (disc_fd < 0) disc_open();
    if (t - disc_query_t > 3.0) disc_query();
    double before = disc_changed_t;
    for (int s = 0; s < 2; s++) {
        int fd = s ? disc_fd2 : disc_fd; if (fd < 0) continue;
        uint8_t buf[2048]; ssize_t n;
        while ((n = recv(fd, buf, sizeof buf, 0)) > 0) disc_parse(buf, (int)n);
    }
    for (int k = 0; k < DISC_MAX; k++) if (disc_macs[k].used && t - disc_macs[k].seen > 12.0) { disc_macs[k].used = false; changed = true; }
    return changed || disc_changed_t != before;
}
static struct disc_mac *disc_find_id(const char *id) { for (int k = 0; k < DISC_MAX; k++) if (disc_macs[k].used && id[0] && !strcmp(disc_macs[k].id, id)) return &disc_macs[k]; return NULL; }
static void disc_close(void) { if (disc_fd >= 0) close(disc_fd); if (disc_fd2 >= 0) close(disc_fd2); disc_fd = disc_fd2 = -1; memset(disc_macs, 0, sizeof disc_macs); }
#endif
