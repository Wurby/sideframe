# Sideframe protocol (v1)

Sideframe shows a Mac's screen in a Steam Frame (SteamVR dashboard tab). The Mac runs the **Sideframe** menu-bar app (server); the
Frame runs the Sideframe dashboard app (client). Everything is UDP on **port 5000** (configurable), except discovery.

Contents: 1. Discovery · 2. Pairing · 3. Session handshake · 4. Encrypted datagrams · 5. Inner messages · 6. Video · 7. Input · 8. Threat model

All integers are big-endian. `||` is concatenation. `HMAC` is HMAC-SHA256, `H` is SHA-256, `HKDF` is HKDF-SHA256 (RFC 5869).

## 1. Discovery

The Mac advertises a Bonjour/mDNS service **`_sideframe._udp`** on its UDP port. TXT records: `v=1`, `id=<16 hex chars, the Mac's random id>`,
`name=<display name>`. The Frame lists the Macs it finds; the user can also type an address.

## 2. Pairing (done once per Frame↔Mac pair)

The user opens **Pair a Steam Frame…** in the Mac app, which shows a single-use **8-digit code** (valid 2 minutes, 3 wrong attempts).
On the Frame the user picks the Mac and types the code. No file, key or SSH access is ever handled by the user.

```
Frame                                                     Mac (pairing mode on, code C)
 skF = random X25519 key, pkF = X25519(skF, 9)
 nF  = 16 random bytes
 PAIR_REQ   0x10 | ver=1 | pkF(32) | nF(16) | len(1) | frameName  ───►
                                                      skM, pkM = fresh X25519 key; nM = 16 random; salt = 16 random
              ◄───  PAIR_CHALLENGE 0x11 | pkM(32) | nM(16) | salt(16) | macId(8) | len(1) | macName
 codeKey = PBKDF2-HMAC-SHA256(password = C as ASCII digits, salt = salt||nF||nM, iterations = 600000, dkLen = 32)
 dh      = X25519(skF, pkM)                                dh = X25519(skM, pkF)  (reject all-zero)
 T       = H("sideframe-pair-v1" || pkF || pkM || nF || nM || salt)
 confF   = HMAC(codeKey, "F" || T || dh)
 PAIR_CONFIRM 0x12 | confF(32)  ───►                  verifies confF (constant time); wrong → attempts++, PAIR_DENIED
              ◄───  PAIR_DONE 0x13 | confM(32) | macId(8)      confM = HMAC(codeKey, "M" || T || dh)
 verifies confM
 psk = HKDF(ikm = dh, salt = T, info = "sideframe psk v1", L = 32);  kid = H(psk)[0..8]
 both sides store (kid, psk, peer name); the code is now spent.
 PAIR_DENIED 0x1F | reason(1)      1 = not in pairing mode, 2 = wrong code, 3 = busy, 4 = too many attempts
```

The code authenticates the key exchange; the shared secret `dh` is never derivable from the code, so learning the code later reveals nothing about `psk`.
A machine-in-the-middle must guess the code inside one handshake window; the 600 000-iteration KDF, the 3-attempt limit, the 2-minute lifetime and
the Frame's short timeout make that impractical. (A password-authenticated key exchange such as CPace would remove the residual offline-guess
concern during the pairing window; this is a possible future upgrade.)

## 3. Session handshake (every connection)

```
HELLO   (Frame→Mac) 0x01 | kid(8) | cn(16) | tag(16)      tag = HMAC(psk, "sideframe-hello"   || kid || cn)[0..16]
WELCOME (Mac→Frame) 0x02 | sn(16) | tag(16)                tag = HMAC(psk, "sideframe-welcome" || cn  || sn)[0..16]
```
The Mac looks up `psk` by `kid` in its paired list; unknown `kid` or a bad tag gets **no reply**. Both sides prove knowledge of `psk`.
Keys: `HKDF(ikm = psk, salt = cn||sn, info = "sideframe m2f" | "sideframe f2m", L = 36)` → key(32) ‖ nonce prefix(4) per direction.
The first authenticated DATA packet from the Frame promotes the session (a replayed HELLO cannot take over a stream).

## 4. Encrypted datagrams

`0x03 | counter(8) | ChaCha20-Poly1305 ciphertext | tag(16)`, AAD = the first 9 bytes, nonce = prefix(4) ‖ counter(8).
Receivers keep a 2048-packet replay window and only update it after authentication.

## 5. Inner messages (inside DATA)

Frame→Mac control: `[type u8][flags u8][n u16][a u32][b u32]` (+ n bytes payload)
1 ack (a = highest consumed frame, b = µs from receipt to presentation or 0xFFFFFFFF) · 2 NACK (a = frame, n fragment indices; n=0xFFFF = whole frame) ·
3 keyframe request · 4 ping (echoed) · 5–10 input (section 7).

## 6. Video

Mac→Frame: `[seq u32][idx u16][cnt u16][flags u8: 1=key 2=retx][0][len u16][total u32]` + ≤1350 bytes of an Annex-B HEVC access unit.
HEVC Main, no B-frames, keyframes carry VPS/SPS/PPS. Loss repair: NACK for missing fragments; unrecoverable frames → resync at the next keyframe.
Flow control: the Mac skips capture frames before encoding while 6 frames are unacknowledged.

## 7. Input (Frame→Mac), only on an authenticated session

Same 12-byte header; `a`/`b` as below. Positions are fractions of the mirrored display.

| type | name | a | b |
|---|---|---|---|
| 5 | MOVE | x · 1 000 000 (0…1 000 000) | y · 1 000 000 |
| 6 | BUTTON | 0 left, 1 right, 2 middle, 3/4 extra | 1 down, 0 up |
| 7 | SCROLL | dx (int32, 1/100 line) | dy (int32, 1/100 line) |
| 8 | KEY | USB HID usage (page 0x07) | 1 down, 0 up |
| 9 | TEXT | – | – (n bytes of UTF-8 follow the header) |
| 10 | RELEASE_ALL | – | – |

The Mac injects these with CGEvent (needs the Accessibility permission) and releases everything when the session ends.
The user can switch **Allow remote control** off in the Mac menu.

## 8. Threat model (summary)

Protected against: passive eavesdropping, forged/replayed/modified packets, unpaired devices, session hijack by a replayed HELLO.
Not protected: a compromised Mac or Frame; someone who obtains `paired.json` / the Frame's stored key (guard those files); traffic analysis
(packet sizes and timing are visible); denial of service by flooding the port.
