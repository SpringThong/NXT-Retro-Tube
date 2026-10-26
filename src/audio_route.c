/* Audio output routing: speaker/headphones or a USB DAC on either USB-C port.
 * A USB DAC gets a private ALSA config (the firmware's shared USB dmix on
 * NX Redux, like NXT Retro Music); the speaker path keeps the firmware default.
 * The bottom (charging) port must be switched to host mode for a DAC there.
 * SPDX-License-Identifier: GPL-3.0-only */
#include "app.h"
#include <dlfcn.h>
#include <fcntl.h>
#include <unistd.h>
#include <ctype.h>
#include <sys/stat.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define BOTTOM_USB "/sys/devices/platform/soc/usbc0"
static char saved_alsa[1024]; static bool had_alsa, saved;
static char dac_label[128];
static int routed_card = -2;

static bool readtext(const char *path, char *out, size_t n) {
    int fd = open(path, O_RDONLY); if (fd < 0) return false;
    ssize_t got = read(fd, out, n - 1); close(fd);
    if (got < 0) return false;
    out[got] = 0;
    while (got > 0 && isspace((unsigned char)out[got - 1])) out[--got] = 0;
    return true;
}
int AudioRoute_usbCard(void) {
    for (int card = 0; card < 16; card++) {
        char p[128], buf[128]; snprintf(p, sizeof(p), "/proc/asound/card%d/usbbus", card);
        if (readtext(p, buf, sizeof(buf))) {
            char idp[128]; snprintf(idp, sizeof(idp), "/proc/asound/card%d/id", card);
            if (!readtext(idp, dac_label, sizeof(dac_label))) snprintf(dac_label, sizeof(dac_label), "USB DAC");
            return card;
        }
    }
    return -1;
}
const char *AudioRoute_dacName(void) { return dac_label; }
int AudioRoute_current(void) { return routed_card; }

/* Reset ALSA's cached global config so the next open reads our file. */
static void alsa_reload(void) {
    void *h = dlopen("libasound.so.2", RTLD_NOW | RTLD_NOLOAD);
    if (!h) h = dlopen("libasound.so.2", RTLD_NOW);
    if (!h) return;
    int (*fn)(void) = (int (*)(void))dlsym(h, "snd_config_update_free_global");
    if (fn) fn();
    dlclose(h);
}
/* Chooses the sink for mode (0 auto, 1 speaker, 2 USB DAC) and prepares ALSA.
 * Call with the SDL audio device closed. Returns the USB card or -1 (speaker). */
int AudioRoute_prepare(int mode) {
    if (!saved) { const char *e = getenv("ALSA_CONFIG_PATH"); had_alsa = e != NULL; snprintf(saved_alsa, sizeof(saved_alsa), "%s", e ? e : ""); saved = true; }
    int card = mode == 1 ? -1 : AudioRoute_usbCard();
    if (card < 0) {
        if (had_alsa) setenv("ALSA_CONFIG_PATH", saved_alsa, 1); else unsetenv("ALSA_CONFIG_PATH");
        unsetenv("AUDIODEV");
        if (routed_card != -1) alsa_reload();
        routed_card = -1;
        return -1;
    }
    const char *base = had_alsa && *saved_alsa ? saved_alsa : "/usr/share/alsa/alsa.conf";
    if (access(base, R_OK)) { Log_msg("audio: no ALSA base config, keeping default output"); routed_card = -1; return -1; }
    int rate = 48000;
    char state[512];
    /* NX Redux publishes the DAC rate it negotiated for its shared USB mixer */
    if (readtext("/tmp/nx_audio_sink", state, sizeof(state)) && strstr(state, "sink=usb")) {
        int c = -1, r = 0; char *pc = strstr(state, "card="), *pr = strstr(state, "rate=");
        if (pc && pr && sscanf(pc, "card=%d", &c) == 1 && sscanf(pr, "rate=%d", &r) == 1 && c == card && r >= 8000 && r <= 384000) rate = r;
    }
    char dir[1200], path[1300], tmp[1400], text[2048];
    snprintf(dir, sizeof(dir), "%s/audio", paths.data); mkdir(dir, 0755);
    snprintf(path, sizeof(path), "%s/asound.conf", dir);
    snprintf(text, sizeof(text),
        "<%s>\n"
        "# NXT Retro Tube: USB DAC output (shares the firmware USB mixer)\n"
        "pcm.!nxt_tube_mix {\n type dmix\n ipc_key %d\n ipc_key_add_uid false\n ipc_perm 0666\n"
        " slave { pcm \"hw:%d,0\" rate %d format S16_LE channels 2 period_size 1024 buffer_size 4096 }\n bindings { 0 0 1 1 }\n}\n"
        "pcm.!nxt_tube_out { type plug slave.pcm \"nxt_tube_mix\" }\n",
        base, 0x4e5858, card, rate);
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    FILE *f = fopen(tmp, "w");
    if (!f || fputs(text, f) < 0 || fclose(f)) { if (f) fclose(f); Log_msg("audio: cannot write %s", path); routed_card = -1; return -1; }
    rename(tmp, path);
    setenv("ALSA_CONFIG_PATH", path, 1);
    setenv("AUDIODEV", "nxt_tube_out", 1);
    alsa_reload();
    routed_card = card;
    Log_msg("audio: USB DAC card %d (%s) at %d Hz", card, dac_label, rate);
    return card;
}
void AudioRoute_restore(void) {
    if (!saved) return;
    if (had_alsa) setenv("ALSA_CONFIG_PATH", saved_alsa, 1); else unsetenv("ALSA_CONFIG_PATH");
    unsetenv("AUDIODEV");
}

/* bottom USB-C port role: 1 host (DAC), 2 device (charging), 0 unknown */
static bool read_node(const char *name, char *out, size_t n) {
    char p[256]; snprintf(p, sizeof(p), "%s/%s", BOTTOM_USB, name);
    return readtext(p, out, n);
}
int AudioRoute_bottomRole(void) {
    char r[64]; if (!read_node("otg_role", r, sizeof(r))) return 0;
    return !strcmp(r, "usb_host") ? 1 : !strcmp(r, "usb_device") ? 2 : 0;
}
/* The vendor nodes perform the switch when read. Returns the new role. */
int AudioRoute_setBottom(bool host) {
    int want = host ? 1 : 2;
    if (AudioRoute_bottomRole() == want) return want;
    char reply[128];
    read_node(host ? "usb_host" : "usb_device", reply, sizeof(reply));
    for (int i = 0; i < 20; i++) { if (AudioRoute_bottomRole() == want) break; usleep(50000); }
    int after = AudioRoute_bottomRole();
    Log_msg("bottom USB: requested %s, now %d", host ? "host" : "device", after);
    return after;
}
