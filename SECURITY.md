# Security

## Threat model

Sideframe streams a screen and accepts keyboard/mouse input, so it must only ever talk to a Mac you paired with.

* **Pairing** uses an 8-digit code shown on the Mac, a Diffie-Hellman exchange (X25519) and PBKDF2 (600k iterations) to bind the code to the
  exchange. A passive observer learns nothing useful; an active attacker gets at most 3 guesses at the code before it is replaced by a new one. The code is only valid while the pairing window is open.
  See [docs/PROTOCOL.md](docs/PROTOCOL.md) for the exact construction and its stated limits.
* **Session** traffic is ChaCha20-Poly1305 with per-direction keys derived (HKDF) from the pair key and per-session nonces, with a replay window.
  Unauthenticated, forged or replayed packets are dropped.
* **Input injection** on the Mac only happens for an authenticated session and requires the user's Accessibility permission.
  Held keys/buttons are released when a session ends or the tab is hidden.
* Nothing persistent is installed unasked (no login items or launch agents by default).

Out of scope: an attacker who already controls either device, or who can read the pair key files (`~/.config/sideframe` on the Frame,
`~/Library/Application Support/Sideframe` on the Mac; both are written mode 0600).

## Reporting a vulnerability

Please open a private security advisory on the repository (or email the maintainers) rather than a public issue. Include steps to reproduce.
