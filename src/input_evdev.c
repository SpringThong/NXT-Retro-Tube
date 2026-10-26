/* Pad reader for muOS and Knulli: buttons are taken from evdev by key code.
 * SDL numbers joystick buttons differently on each firmware ("TRIMUI Player1"
 * on the stock userspace, "TRIMUI Brick Pro Controller" on Knulli, "muOS-Keys"
 * on muOS), but the key codes are the same everywhere, so they are translated
 * here into the physical ids the key map uses (the TrimUI layout):
 *   0 B  1 A  2 Y  3 X  4 L1  5 R1  6 SELECT  7 START  8 MENU  9 L3  10 R3
 *   13 VOL-  14 VOL+  15 side Home  100 L2  101 R2
 * Devices are opened read-only and never grabbed, so the OS hotkey daemons
 * (volume, brightness, power) keep working.
 * SPDX-License-Identifier: GPL-3.0-only */
#include "app.h"
#include <dirent.h>
#include <errno.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>
#include <linux/input.h>
#include <sys/ioctl.h>

#ifndef BTN_DPAD_UP
#define BTN_DPAD_UP 0x220
#define BTN_DPAD_DOWN 0x221
#define BTN_DPAD_LEFT 0x222
#define BTN_DPAD_RIGHT 0x223
#endif

#define MAXDEV 8
typedef struct { int fd; int zmax, rzmax; bool z_down, rz_down; int hx, hy; int du, dd, dl, dr; char name[80]; } PadDev;
static PadDev dev[MAXDEV]; static int ndev;
static Uint32 next_scan;

#define BITS(n) (((n) + 8 * sizeof(long) - 1) / (8 * sizeof(long)))
static bool has_bit(const unsigned long *b, int n) { return (b[n / (8 * sizeof(long))] >> (n % (8 * sizeof(long)))) & 1; }

static int phys_for(int code) {
    switch (code) {
        case BTN_SOUTH: return 0;
        case BTN_EAST: return 1;
        case BTN_WEST: return 2;      /* left face button: Y */
        case BTN_NORTH: return 3;     /* top face button: X */
        case BTN_TL: return 4;
        case BTN_TR: return 5;
        case BTN_SELECT: return 6;
        case BTN_START: return 7;
        case BTN_MODE: return 8;
        case BTN_THUMBL: return 9;
        case BTN_THUMBR: return 10;
        case KEY_VOLUMEDOWN: return 13;
        case KEY_VOLUMEUP: return 14;
        case KEY_HOMEPAGE: case KEY_HOME: return 15;
        case BTN_TL2: return 100;
        case BTN_TR2: return 101;
#ifdef NXT_TUBE_ROM_DARKOS
        /* R36S / RG351MP "GO-Super Gamepad" (odroidgo3-joypad): SELECT, START and the
         * stick clicks are F1..F4; F4 (R3) is also the dArkOS hotkey */
        case BTN_TRIGGER_HAPPY1: return 6;
        case BTN_TRIGGER_HAPPY2: return 7;
        case BTN_TRIGGER_HAPPY3: return 9;
        case BTN_TRIGGER_HAPPY4: return 8;
#endif
        default: return -1;
    }
}

static void scan(void) {
    DIR *d = opendir("/dev/input");
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d)) && ndev < MAXDEV) {
        if (strncmp(e->d_name, "event", 5)) continue;
        char p[300]; snprintf(p, sizeof(p), "/dev/input/%s", e->d_name);
        int fd = open(p, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0) continue;
        unsigned long keys[BITS(KEY_MAX + 1)]; memset(keys, 0, sizeof(keys));
        if (ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(keys)), keys) < 0) { close(fd); continue; }
        bool pad = has_bit(keys, BTN_SOUTH) || has_bit(keys, BTN_EAST);
        bool extra = has_bit(keys, KEY_VOLUMEUP) || has_bit(keys, KEY_HOMEPAGE);
        if (!pad && !extra) { close(fd); continue; }
        /* the same node twice (e.g. after a rescan)? */
        struct stat st; bool dup = false;
        if (!fstat(fd, &st)) for (int i = 0; i < ndev; i++) { struct stat o; if (!fstat(dev[i].fd, &o) && o.st_rdev == st.st_rdev) dup = true; }
        if (dup) { close(fd); continue; }
        PadDev *v = &dev[ndev]; memset(v, 0, sizeof(*v)); v->fd = fd;
        if (ioctl(fd, EVIOCGNAME(sizeof(v->name)), v->name) < 0) snprintf(v->name, sizeof(v->name), "%s", e->d_name);
        struct input_absinfo ai;
        v->zmax = ioctl(fd, EVIOCGABS(ABS_Z), &ai) >= 0 && ai.maximum > 0 ? ai.maximum : 0;
        v->rzmax = ioctl(fd, EVIOCGABS(ABS_RZ), &ai) >= 0 && ai.maximum > 0 ? ai.maximum : 0;
        Log_msg("pad: %s (%s)%s", p, v->name, pad ? "" : " [keys]");
        ndev++;
    }
    closedir(d);
}
bool Pad_open(void) {
    Pad_close();
    scan();
    next_scan = SDL_GetTicks() + 3000;
    return ndev > 0;
}
void Pad_close(void) {
    for (int i = 0; i < ndev; i++) close(dev[i].fd);
    ndev = 0;
}
static void hat_report(HatCb hat) {
    int u = 0, dn = 0, l = 0, r = 0;
    for (int i = 0; i < ndev; i++) {
        PadDev *v = &dev[i];
        if (v->hy < 0 || v->du) u = 1;
        if (v->hy > 0 || v->dd) dn = 1;
        if (v->hx < 0 || v->dl) l = 1;
        if (v->hx > 0 || v->dr) r = 1;
    }
    hat(u, dn, l, r);
}
void Pad_poll(PadCb cb, HatCb hat) {
    Uint32 now = SDL_GetTicks();
    if (!ndev && (Sint32)(now - next_scan) >= 0) { scan(); next_scan = now + 3000; }
    for (int i = 0; i < ndev; i++) {
        PadDev *v = &dev[i];
        struct input_event ev; bool hat_changed = false;
        for (;;) {
            ssize_t n = read(v->fd, &ev, sizeof(ev));
            if (n != (ssize_t)sizeof(ev)) {
                if (n < 0 && errno != EAGAIN && errno != EINTR) {   /* unplugged: drop it, rescan later */
                    Log_msg("pad: %s gone", v->name); close(v->fd);
                    dev[i] = dev[--ndev]; i--; next_scan = now + 1000; hat_changed = true;
                }
                break;
            }
            if (ev.type == EV_KEY && ev.value != 2) {
                bool down = ev.value == 1;
                switch (ev.code) {
                    case BTN_DPAD_UP: v->du = down; hat_report(hat); continue;
                    case BTN_DPAD_DOWN: v->dd = down; hat_report(hat); continue;
                    case BTN_DPAD_LEFT: v->dl = down; hat_report(hat); continue;
                    case BTN_DPAD_RIGHT: v->dr = down; hat_report(hat); continue;
                }
                int phys = phys_for(ev.code);
                if (phys >= 0) cb(phys, down);
            } else if (ev.type == EV_ABS) {
                if (ev.code == ABS_HAT0X) { v->hx = ev.value; hat_report(hat); }
                else if (ev.code == ABS_HAT0Y) { v->hy = ev.value; hat_report(hat); }
                else if ((ev.code == ABS_Z && v->zmax) || (ev.code == ABS_RZ && v->rzmax)) {
                    bool rz = ev.code == ABS_RZ; int mx = rz ? v->rzmax : v->zmax;
                    bool *st = rz ? &v->rz_down : &v->z_down;
                    if (!*st && ev.value > mx * 4 / 10) { *st = true; cb(rz ? 101 : 100, true); }
                    else if (*st && ev.value < mx / 5) { *st = false; cb(rz ? 101 : 100, false); }
                }
            }
        }
        if (hat_changed) hat_report(hat);
    }
}
