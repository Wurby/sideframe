// authtest — tries to break into a running `mac-capture serve`. Every attack must FAIL; the legitimate peer must still work.
// build: gcc -O2 -I../src -o authtest authtest.c -lsodium      run: ./authtest HOST[:PORT] [keyfile]   (Mac must be serving)
#include <unistd.h>
#include <poll.h>
#include <errno.h>
#include <time.h>
#include <fcntl.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include "../src/pairing_client.h"

static struct sockaddr_in dst;
#define send(fd, b, n, fl) sendto((fd), (b), (n), (fl), (struct sockaddr *)&dst, sizeof dst)
static int mk(const char *host, int port, int lport) {
    struct addrinfo h = { .ai_family = AF_INET, .ai_socktype = SOCK_DGRAM }, *r; char ps[16]; snprintf(ps, sizeof ps, "%d", port);
    if (getaddrinfo(host, ps, &h, &r) || !r) { fprintf(stderr, "resolve %s failed\n", host); exit(2); }
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (lport) { struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons(lport), .sin_addr.s_addr = INADDR_ANY }; bind(fd, (void *)&a, sizeof a); }
    memcpy(&dst, r->ai_addr, sizeof dst);   /* unconnected: the Mac may answer from another of its addresses */
    freeaddrinfo(r); return fd;
}
/* wait up to ms for a datagram; returns length or -1 */
static int rx(int fd, uint8_t *b, size_t n, int ms) { struct pollfd p = { fd, POLLIN, 0 }; if (poll(&p, 1, ms) <= 0) return -1; return (int)recv(fd, b, n, 0); }
static void drain(int fd, int ms) { uint8_t b[2048]; while (rx(fd, b, sizeof b, ms) >= 0) {} }
static int fails;
static void check(const char *name, bool ok) { printf("  [%s] %s\n", ok ? "PASS" : "FAIL", name); if (!ok) fails++; }

/* send a sealed ping (type 4) with seal; returns datagram so the caller can replay/tamper */
static size_t make_ping(struct sess *s, uint32_t token, uint8_t *out) {
    uint8_t m[12] = { 4, 0, 0, 0, 0, 0, 0, 0, token >> 24, token >> 16, token >> 8, token };
    return sess_seal(s, m, 12, out, UINT64_MAX);
}
/* did the Mac echo this ping token back to us? (video fragments also arrive; they are skipped) */
static bool got_echo(int fd, struct sess *s, uint32_t token, int ms) {
    uint8_t b[2048], pl[2048]; size_t pn; time_t t0 = time(NULL); (void)t0;
    for (int i = 0; i < 4000; i++) {
        int n = rx(fd, b, sizeof b, ms); if (n < 0) return false;
        if (b[0] == 3 && sess_open(s, b, (size_t)n, pl, &pn) && pn == 12 && pl[0] == 4) {
            uint32_t t = ((uint32_t)pl[8] << 24) | (pl[9] << 16) | (pl[10] << 8) | pl[11]; if (t == token) return true;
        }
    }
    return false;
}
static bool handshake(int fd, struct sess *s, const uint8_t *cn) {
    uint8_t p[41] = { 1 }, b[2048], tag[16]; memcpy(p + 1, psk_kid, 8); memcpy(p + 9, cn, 16); hmac16(p + 25, "sideframe-hello", psk_kid, 8, cn, 16);
    send(fd, p, sizeof p, 0);
    int n = rx(fd, b, sizeof b, 1500);
    if (n != 33 || b[0] != 2) { printf("  (debug: expected a 33-byte WELCOME, got n=%d first=0x%02x)\n", n, n > 0 ? b[0] : 0); return false; }
    hmac16(tag, "sideframe-welcome", cn, 16, b + 1, 16);
    if (sodium_memcmp(tag, b + 17, 16) != 0) { printf("  (debug: WELCOME tag did not verify)\n"); return false; }
    sess_derive(s, cn, b + 1, false); return true;
}

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: authtest HOST[:PORT] [keyfile]\n"); return 2; }
    char host[256]; snprintf(host, sizeof host, "%s", argv[1]); int port = 5000; char *c = strrchr(host, ':'); if (c) { *c = 0; port = atoi(c + 1); }
    if (sodium_init() < 0) return 2;
    struct mac_conf mc;
    if (!(argc > 2 ? mac_conf_load(argv[2], &mc) : mac_conf_pick(NULL, &mc))) { fprintf(stderr, "no paired Mac found (pair first)\n"); return 2; }
    set_psk(mc.key);
    uint8_t goodkey[32]; memcpy(goodkey, psk, 32);
    uint8_t b[2048], pkt[2048], cn[16];
    printf("authtest against %s:%d\n", host, port);

    printf("1. garbage and malformed packets are ignored (no reply)\n");
    { int fd = mk(host, port, 0); randombytes_buf(pkt, 100); send(fd, pkt, 100, 0); pkt[0] = 1; send(fd, pkt, 33, 0); pkt[0] = 3; send(fd, pkt, 60, 0); pkt[0] = 2; send(fd, pkt, 33, 0);
      check("no response to garbage / bad HELLO shape / bogus DATA", rx(fd, b, sizeof b, 800) < 0); close(fd); }

    printf("2. HELLO with the WRONG pairing key gets no WELCOME\n");
    { int fd = mk(host, port, 0); uint8_t wrong[32]; randombytes_buf(wrong, 32); memcpy(psk, wrong, 32);
      randombytes_buf(cn, 16); uint8_t p[41] = { 1 }; memcpy(p + 1, psk_kid, 8); memcpy(p + 9, cn, 16); hmac16(p + 25, "sideframe-hello", psk_kid, 8, cn, 16); send(fd, p, 41, 0);
      check("no WELCOME for a peer without the key (right key id, wrong key)", rx(fd, b, sizeof b, 1200) < 0); memcpy(psk, goodkey, 32);
      uint8_t kid2[8]; randombytes_buf(kid2, 8); memcpy(p + 1, kid2, 8); hmac16(p + 25, "sideframe-hello", kid2, 8, cn, 16); send(fd, p, 41, 0);
      check("no WELCOME for an unknown key id", rx(fd, b, sizeof b, 1200) < 0);
      /* pairing attacks: the Mac is not in pairing mode, so it may only answer with a refusal */
      uint8_t pr[51 + 4] = { 0x10, 1 }; randombytes_buf(pr + 2, 48); pr[50] = 4; memcpy(pr + 51, "evil", 4); send(fd, pr, sizeof pr, 0);
      int dn = rx(fd, b, sizeof b, 1200);
      check("pairing request while pairing is closed is refused (0x1F, reason 1) and gets no challenge", dn == 2 && b[0] == 0x1F && b[1] == 1);
      uint8_t cf[33] = { 0x12 }; randombytes_buf(cf + 1, 32); send(fd, cf, 33, 0);
      check("a confirmation out of the blue gets no reply", rx(fd, b, sizeof b, 800) < 0); close(fd); }

    printf("3. legitimate peer: handshake + authenticated ping/echo works\n");
    int fd = mk(host, port, 0); struct sess L; randombytes_buf(cn, 16);
    bool hs = handshake(fd, &L, cn); check("handshake with the right key succeeds (server proves the key too)", hs);
    if (!hs) { printf("cannot continue without a session — is `mac-capture serve` running and paired?\n"); return 1; }
    size_t pn = make_ping(&L, 0xA11CE001, pkt); send(fd, pkt, pn, 0);
    check("authenticated ping is echoed back", got_echo(fd, &L, 0xA11CE001, 1500));

    printf("4. replay: re-sending the exact same encrypted packet is rejected\n");
    { uint8_t saved[256]; size_t sn = make_ping(&L, 0xBEEF0002, saved); send(fd, saved, sn, 0);
      check("first copy accepted (echo received)", got_echo(fd, &L, 0xBEEF0002, 1500));
      send(fd, saved, sn, 0); send(fd, saved, sn, 0);
      check("replayed copies produce NO second echo", !got_echo(fd, &L, 0xBEEF0002, 800)); }

    printf("5. tampering: flipping one bit makes the packet useless\n");
    { size_t tn = make_ping(&L, 0xBEEF0003, pkt); pkt[tn - 5] ^= 0x01; send(fd, pkt, tn, 0);
      check("bit-flipped ciphertext/tag is not accepted", !got_echo(fd, &L, 0xBEEF0003, 800));
      tn = make_ping(&L, 0xBEEF0004, pkt); pkt[3] ^= 0x01; send(fd, pkt, tn, 0);
      check("modified counter (AAD) is not accepted", !got_echo(fd, &L, 0xBEEF0004, 800)); }

    printf("6. forgery: a packet sealed with some other session key is rejected\n");
    { struct sess F; uint8_t cn2[16], sn2[16]; randombytes_buf(cn2, 16); randombytes_buf(sn2, 16); sess_derive(&F, cn2, sn2, false);
      size_t fn = make_ping(&F, 0xBEEF0005, pkt); send(fd, pkt, fn, 0); check("forged packet is not accepted", !got_echo(fd, &L, 0xBEEF0005, 800)); }

    printf("7. HELLO replay from another address does not hijack the stream\n");
    { uint8_t hello[41] = { 1 }; memcpy(hello + 1, psk_kid, 8); memcpy(hello + 9, cn, 16); hmac16(hello + 25, "sideframe-hello", psk_kid, 8, cn, 16);   /* a captured, valid HELLO */
      int atk = mk(host, port, 0); send(atk, hello, 41, 0); int wn = rx(atk, b, sizeof b, 800);
      printf("  (attacker %s a WELCOME — harmless: it cannot produce valid DATA without the key)\n", wn == 33 ? "received" : "did not receive");
      uint8_t junk[40]; randombytes_buf(junk, 40); junk[0] = 3; send(atk, junk, 40, 0);
      pn = make_ping(&L, 0xA11CE006, pkt); send(fd, pkt, pn, 0);
      check("legitimate session still works afterwards", got_echo(fd, &L, 0xA11CE006, 1500));
      check("attacker receives no video / echo", rx(atk, b, sizeof b, 600) < 0 || b[0] != 3); close(atk); }

    printf("\n%s (%d failed)\n", fails ? "SOME CHECKS FAILED" : "ALL CHECKS PASSED", fails);
    return fails ? 1 : 0;
}
