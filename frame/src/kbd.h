// kbd.h — physical / Bluetooth keyboards on the Frame -> the Mac.
// Reads Linux evdev keyboards (group "input"), and only while the laser is on the Sideframe tab ("hover to type"). While the tab has
// the keyboard the devices are grabbed (EVIOCGRAB) so the same keystrokes don't also land in a Frame window; the kernel drops a
// grab automatically if we exit or crash. Key codes are translated to USB HID usages (what the Mac app expects, message type 8).
#ifndef KBD_H
#define KBD_H
#include <dirent.h>
#include <linux/input.h>
#include <sys/ioctl.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stdio.h>

#define KBD_MAX 8
static int kbd_fd[KBD_MAX]; static char kbd_path[KBD_MAX][64]; static int kbd_n = 0;
static bool kbd_grabbed = false; static double kbd_scan_t = -100;
static bool kbd_down[256];                         /* HID usages currently held on the Mac (for release-all) */

static uint8_t kbd_hid(int c) {                    /* evdev KEY_* -> HID usage, 0 = unmapped */
    static const uint8_t row1[] = { 0, 0x29, 0x1E, 0x1F, 0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27, 0x2D, 0x2E, 0x2A, 0x2B,        /* 0..15 */
        0x14, 0x1A, 0x08, 0x15, 0x17, 0x1C, 0x18, 0x0C, 0x12, 0x13, 0x2F, 0x30, 0x28, 0xE0,                                              /* 16..29: q..p [ ] enter lctrl */
        0x04, 0x16, 0x07, 0x09, 0x0A, 0x0B, 0x0D, 0x0E, 0x0F, 0x33, 0x34, 0x35, 0xE1, 0x31,                                              /* 30..43: a..l ; ' ` lshift \ */
        0x1D, 0x1B, 0x06, 0x19, 0x05, 0x11, 0x10, 0x36, 0x37, 0x38, 0xE5, 0, 0xE2, 0x2C, 0x39 };                                         /* 44..58: z..m , . / rshift - lalt space caps */
    if (c > 0 && c < (int)sizeof row1) return row1[c];
    if (c >= 59 && c <= 68) return (uint8_t)(0x3A + (c - 59));       /* F1..F10 */
    switch (c) {
    case 87: return 0x44; case 88: return 0x45;                        /* F11 F12 */
    case 97: return 0xE4; case 100: return 0xE6; case 125: return 0xE3; case 126: return 0xE7;   /* rctrl ralt lmeta rmeta */
    case 102: return 0x4A; case 103: return 0x52; case 104: return 0x4B; case 105: return 0x50; case 106: return 0x4F;
    case 107: return 0x4D; case 108: return 0x51; case 109: return 0x4E; case 111: return 0x4C;   /* home up pgup left right end down pgdn delete */
    }
    return 0;
}

static bool kbd_is_keyboard(int fd) {
    unsigned long ev[(EV_MAX + 63) / 64] = {0}, key[(KEY_MAX + 63) / 64] = {0};
    if (ioctl(fd, EVIOCGBIT(0, sizeof ev), ev) < 0 || !(ev[0] & (1UL << EV_KEY))) return false;
    if (ioctl(fd, EVIOCGBIT(EV_KEY, sizeof key), key) < 0) return false;
    for (int k = KEY_Q; k <= KEY_P; k++) if (!(key[k / 64] & (1UL << (k % 64)))) return false;   /* has the letter keys: a real keyboard, not a gamepad */
    return !(ev[0] & (1UL << EV_ABS));
}
static void kbd_scan(double t) {                   /* find keyboards (also picks up a Bluetooth keyboard that connects later) */
    if (t - kbd_scan_t < 3.0) return; kbd_scan_t = t;
    for (int i = 0; i < kbd_n; i++) if (access(kbd_path[i], F_OK) != 0) { close(kbd_fd[i]); kbd_fd[i] = kbd_fd[--kbd_n]; memcpy(kbd_path[i], kbd_path[kbd_n], 64); i--; }   /* unplugged */
    DIR *d = opendir("/dev/input"); if (!d) return; struct dirent *e;
    while ((e = readdir(d))) {
        if (strncmp(e->d_name, "event", 5) || kbd_n >= KBD_MAX) continue;
        char p[64]; snprintf(p, sizeof p, "/dev/input/%s", e->d_name); bool have = false;
        for (int i = 0; i < kbd_n; i++) if (!strcmp(kbd_path[i], p)) have = true;
        if (have) continue;
        int fd = open(p, O_RDONLY | O_NONBLOCK | O_CLOEXEC); if (fd < 0) continue;
        if (!kbd_is_keyboard(fd)) { close(fd); continue; }
        kbd_fd[kbd_n] = fd; snprintf(kbd_path[kbd_n], 64, "%s", p); kbd_n++;
        if (kbd_grabbed) ioctl(fd, EVIOCGRAB, 1);
    }
    closedir(d);
}
static void kbd_grab(bool on) { if (on == kbd_grabbed) return; kbd_grabbed = on; for (int i = 0; i < kbd_n; i++) ioctl(kbd_fd[i], EVIOCGRAB, on ? 1 : 0); }

/* Drain pending key events. With `forward` false (the tab doesn't have the keyboard) nothing is read here, so the keys go where they always did. */
static void kbd_service(double t, bool forward, void (*send_key)(int hid, int down)) {
    kbd_scan(t);
    if (!forward) {
        if (kbd_grabbed) { kbd_grab(false); for (int u = 0; u < 256; u++) if (kbd_down[u]) { send_key(u, 0); kbd_down[u] = false; } }
        return;
    }
    if (!kbd_grabbed) kbd_grab(true);
    for (int i = 0; i < kbd_n; i++) {
        struct input_event ie[32]; ssize_t n;
        while ((n = read(kbd_fd[i], ie, sizeof ie)) > 0)
            for (int k = 0; k < (int)(n / sizeof ie[0]); k++) {
                if (ie[k].type != EV_KEY || ie[k].value > 2) continue;
                uint8_t h = kbd_hid(ie[k].code); if (!h) continue;
                int down = ie[k].value != 0; kbd_down[h] = down; send_key(h, down);
            }
    }
}
#endif
