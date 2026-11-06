// mdcrypto.h — Sideframe crypto for the Frame (libsodium): handshake tags, encrypted session, pairing maths.
// Must stay byte-for-byte compatible with the Mac's Crypto.swift (check: `sideframe --selftest` vs `mac-capture selftest`). See docs/PROTOCOL.md.
#ifndef MDCRYPTO_H
#define MDCRYPTO_H
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <sodium.h>

/* ---- hex helpers ---- */
static bool hex_decode(const char *hex, uint8_t *out, size_t want) {
    size_t bl = 0;
    return strlen(hex) == want * 2 && sodium_hex2bin(out, want, hex, want * 2, NULL, &bl, NULL) == 0 && bl == want;
}

/* ---- the pairing key currently in use (32 bytes) and its id ---- */
static uint8_t psk[32];
static uint8_t psk_kid[8];
static void key_id(const uint8_t key[32], uint8_t kid[8]) { uint8_t h[32]; crypto_hash_sha256(h, key, 32); memcpy(kid, h, 8); }
static void set_psk(const uint8_t key[32]) { memcpy(psk, key, 32); key_id(psk, psk_kid); }

/* HMAC-SHA256(psk, label || a || b) truncated to 16 bytes */
static void hmac16(uint8_t out[16], const char *label, const uint8_t *a, size_t al, const uint8_t *b, size_t bl) {
    uint8_t msg[128], full[32]; size_t o = strlen(label); memcpy(msg, label, o);
    if (a) { memcpy(msg + o, a, al); o += al; }
    if (b) { memcpy(msg + o, b, bl); o += bl; }
    crypto_auth_hmacsha256(full, msg, o, psk); memcpy(out, full, 16);
}

/* HKDF-SHA256 (RFC 5869), built on HMAC-SHA256 so it works with any libsodium (the built-in crypto_kdf_hkdf_* needs 1.0.19+). */
#define HKDF_KEYBYTES 32
static void hkdf_extract(uint8_t prk[32], const uint8_t *salt, size_t sl, const uint8_t *ikm, size_t il) {
    crypto_auth_hmacsha256_state st; crypto_auth_hmacsha256_init(&st, salt, sl); crypto_auth_hmacsha256_update(&st, ikm, il); crypto_auth_hmacsha256_final(&st, prk);
}
static void hkdf_expand(uint8_t *out, size_t olen, const char *info, size_t ilen, const uint8_t prk[32]) {
    uint8_t t[32]; size_t tl = 0, done = 0; uint8_t ctr = 1;
    while (done < olen) {
        crypto_auth_hmacsha256_state st; crypto_auth_hmacsha256_init(&st, prk, 32);
        crypto_auth_hmacsha256_update(&st, t, tl); crypto_auth_hmacsha256_update(&st, (const uint8_t *)info, ilen); crypto_auth_hmacsha256_update(&st, &ctr, 1);
        crypto_auth_hmacsha256_final(&st, t); tl = 32; ctr++;
        size_t n = olen - done < 32 ? olen - done : 32; memcpy(out + done, t, n); done += n;
    }
    sodium_memzero(t, sizeof t);
}

/* ---- encrypted session ---- */
struct sess { uint8_t sk[32], rk[32], sp[4], rp[4]; uint64_t sctr, hi; bool any; uint8_t seen[2048]; };
static struct sess S;
static void sess_derive(struct sess *s, const uint8_t cn_[16], const uint8_t sn[16], bool server) {
    uint8_t salt[32], prk[HKDF_KEYBYTES], m2f[36], f2m[36];
    memcpy(salt, cn_, 16); memcpy(salt + 16, sn, 16);
    hkdf_extract(prk, salt, sizeof salt, psk, sizeof psk);
    hkdf_expand(m2f, sizeof m2f, "sideframe m2f", 13, prk);
    hkdf_expand(f2m, sizeof f2m, "sideframe f2m", 13, prk);
    const uint8_t *snd = server ? m2f : f2m, *rcv = server ? f2m : m2f;
    memcpy(s->sk, snd, 32); memcpy(s->sp, snd + 32, 4); memcpy(s->rk, rcv, 32); memcpy(s->rp, rcv + 32, 4);
    s->sctr = 0; s->any = false; s->hi = 0; memset(s->seen, 0, sizeof s->seen);
    sodium_memzero(prk, sizeof prk); sodium_memzero(m2f, sizeof m2f); sodium_memzero(f2m, sizeof f2m);
}
static size_t sess_seal(struct sess *s, const uint8_t *pl, size_t pn, uint8_t *out, uint64_t ctr_override) {
    uint64_t c = ctr_override != UINT64_MAX ? ctr_override : s->sctr++;
    out[0] = 3; for (int i = 0; i < 8; i++) out[1 + i] = (c >> (56 - 8 * i)) & 255;
    uint8_t nonce[12]; memcpy(nonce, s->sp, 4); memcpy(nonce + 4, out + 1, 8);
    unsigned long long cl;
    crypto_aead_chacha20poly1305_ietf_encrypt(out + 9, &cl, pl, pn, out, 9, NULL, nonce, s->sk);
    return 9 + (size_t)cl;
}
static bool replay_ok(struct sess *s, uint64_t c) {   /* only call for packets that already authenticated */
    if (!s->any) { s->any = true; s->hi = c; s->seen[c % 2048] = 1; return true; }
    if (c > s->hi) {
        uint64_t gap = c - s->hi;
        if (gap >= 2048) memset(s->seen, 0, sizeof s->seen); else for (uint64_t k = 1; k <= gap; k++) s->seen[(s->hi + k) % 2048] = 0;
        s->hi = c; s->seen[c % 2048] = 1; return true;
    }
    if (s->hi - c >= 2048) return false;
    if (s->seen[c % 2048]) return false;
    s->seen[c % 2048] = 1; return true;
}
static bool sess_open(struct sess *s, const uint8_t *pkt, size_t n, uint8_t *out, size_t *on) {
    if (n < 9 + 16 || pkt[0] != 3) return false;
    uint64_t c = 0; for (int i = 0; i < 8; i++) c = (c << 8) | pkt[1 + i];
    uint8_t nonce[12]; memcpy(nonce, s->rp, 4); memcpy(nonce + 4, pkt + 1, 8);
    unsigned long long ml;
    if (crypto_aead_chacha20poly1305_ietf_decrypt(out, &ml, NULL, pkt + 9, n - 9, pkt, 9, nonce, s->rk) != 0) return false;
    if (!replay_ok(s, c)) return false;
    *on = (size_t)ml; return true;
}

/* ---- pairing maths (docs/PROTOCOL.md section 2) ---- */
#define PAIR_ITERATIONS 600000
/* PBKDF2-HMAC-SHA256, one 32-byte output block (dkLen = 32) */
static void pbkdf2_sha256(const uint8_t *pw, size_t pwlen, const uint8_t *salt, size_t slen, uint32_t iters, uint8_t out[32]) {
    crypto_auth_hmacsha256_state base, st; uint8_t u[32], t[32], be1[4] = { 0, 0, 0, 1 };
    crypto_auth_hmacsha256_init(&base, pw, pwlen);
    st = base; crypto_auth_hmacsha256_update(&st, salt, slen); crypto_auth_hmacsha256_update(&st, be1, 4); crypto_auth_hmacsha256_final(&st, u);
    memcpy(t, u, 32);
    for (uint32_t i = 1; i < iters; i++) {
        st = base; crypto_auth_hmacsha256_update(&st, u, 32); crypto_auth_hmacsha256_final(&st, u);
        for (int k = 0; k < 32; k++) t[k] ^= u[k];
    }
    memcpy(out, t, 32); sodium_memzero(u, 32); sodium_memzero(t, 32);
}
static void pair_code_key(uint8_t out[32], const char *code, const uint8_t salt[16], const uint8_t nF[16], const uint8_t nM[16], uint32_t iters) {
    uint8_t s[48]; memcpy(s, salt, 16); memcpy(s + 16, nF, 16); memcpy(s + 32, nM, 16);
    pbkdf2_sha256((const uint8_t *)code, strlen(code), s, sizeof s, iters, out);
}
static void pair_transcript(uint8_t T[32], const uint8_t pkF[32], const uint8_t pkM[32], const uint8_t nF[16], const uint8_t nM[16], const uint8_t salt[16]) {
    crypto_hash_sha256_state h; crypto_hash_sha256_init(&h);
    crypto_hash_sha256_update(&h, (const uint8_t *)"sideframe-pair-v1", 17);
    crypto_hash_sha256_update(&h, pkF, 32); crypto_hash_sha256_update(&h, pkM, 32);
    crypto_hash_sha256_update(&h, nF, 16); crypto_hash_sha256_update(&h, nM, 16); crypto_hash_sha256_update(&h, salt, 16);
    crypto_hash_sha256_final(&h, T);
}
static void pair_confirm(uint8_t out[32], char role, const uint8_t codeKey[32], const uint8_t T[32], const uint8_t dh[32]) {
    uint8_t m[65]; m[0] = (uint8_t)role; memcpy(m + 1, T, 32); memcpy(m + 33, dh, 32);
    crypto_auth_hmacsha256(out, m, sizeof m, codeKey);
}
static void pair_psk(uint8_t out[32], const uint8_t dh[32], const uint8_t T[32]) {
    uint8_t prk[HKDF_KEYBYTES];
    hkdf_extract(prk, T, 32, dh, 32);
    hkdf_expand(out, 32, "sideframe psk v1", 16, prk);
    sodium_memzero(prk, sizeof prk);
}

/* ---- self-test: same fixed public test values as `mac-capture selftest`; compare the printed lines ---- */
static void hexprint(const char *label, const uint8_t *b, size_t n) { printf("%-13s ", label); for (size_t i = 0; i < n; i++) printf("%02x", b[i]); printf("\n"); }
static int crypto_selftest(void) {
    uint8_t k[32]; for (int i = 0; i < 32; i++) k[i] = i; set_psk(k);
    uint8_t cn_[16], sn[16], tag[16]; for (int i = 0; i < 16; i++) { cn_[i] = 0x10 + i; sn[i] = 0x20 + i; }
    hexprint("kid", psk_kid, 8);
    hmac16(tag, "sideframe-hello", psk_kid, 8, cn_, 16); hexprint("hello_tag", tag, 16);
    hmac16(tag, "sideframe-welcome", cn_, 16, sn, 16); hexprint("welcome_tag", tag, 16);
    static const char *mac_pkt_hex = "03000000000000000769b43e42cc60750706c95c9c324d6c0925dbb52e0dfe73c3af5fc7b9885e5d4658";
    uint8_t mp[128], pl[128]; size_t mpn = 0, pln = 0; sodium_hex2bin(mp, sizeof mp, mac_pkt_hex, strlen(mac_pkt_hex), NULL, &mpn, NULL);
    sess_derive(&S, cn_, sn, false);
    bool ok = sess_open(&S, mp, mpn, pl, &pln);
    printf("frame_opens_mac_packet %s: \"%.*s\"\n", ok ? "OK" : "FAILED", ok ? (int)pln : 0, pl);
    printf("replay_rejected %s\n", sess_open(&S, mp, mpn, pl, &pln) ? "FAILED" : "true");
    uint8_t bad[128]; memcpy(bad, mp, mpn); bad[12] ^= 1; sess_derive(&S, cn_, sn, false);
    printf("tamper_rejected %s\n", sess_open(&S, bad, mpn, pl, &pln) ? "FAILED" : "true");
    uint8_t out[128]; size_t on = sess_seal(&S, (const uint8_t *)"from frame", 10, out, 3); hexprint("f2m_packet", out, on);

    /* pairing */
    uint8_t skF[32], skM[32], pkF[32], pkM[32], nF[16], nM[16], salt[16], dhF[32], dhM[32], ck[32], T[32], c1[32], c2[32], pp[32], kid2[8];
    for (int i = 0; i < 32; i++) { skF[i] = 0x40 + i; skM[i] = 0x80 + i; }
    for (int i = 0; i < 16; i++) { nF[i] = 0x50 + i; nM[i] = 0x60 + i; salt[i] = 0x70 + i; }
    crypto_scalarmult_curve25519_base(pkF, skF); crypto_scalarmult_curve25519_base(pkM, skM);
    bool dok = crypto_scalarmult_curve25519(dhF, skF, pkM) == 0 && crypto_scalarmult_curve25519(dhM, skM, pkF) == 0;
    pair_code_key(ck, "12345678", salt, nF, nM, PAIR_ITERATIONS);
    pair_transcript(T, pkF, pkM, nF, nM, salt);
    pair_confirm(c1, 'F', ck, T, dhF); pair_confirm(c2, 'M', ck, T, dhF); pair_psk(pp, dhF, T); key_id(pp, kid2);
    hexprint("pair_pkF", pkF, 32); hexprint("pair_pkM", pkM, 32); hexprint("pair_dh", dhF, 32);
    printf("pair_dh_equal %s\n", dok && !memcmp(dhF, dhM, 32) ? "true" : "FAILED");
    hexprint("pair_codeKey", ck, 32); hexprint("pair_T", T, 32); hexprint("pair_confF", c1, 32); hexprint("pair_confM", c2, 32);
    hexprint("pair_psk", pp, 32); hexprint("pair_kid", kid2, 8);
    return ok ? 0 : 1;
}

#endif
