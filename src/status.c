/* NXT Retro Tube — device status for the header: battery, charger, Wi-Fi, Bluetooth.
 * Read from sysfs / procfs (same on TrimUI, RK3326 and the other ROMs), refreshed every 2 s. */
#include "app.h"
#include <dirent.h>

static bool readline(const char *path, char *buf, size_t n) {
    FILE *f = fopen(path, "r"); if (!f) return false;
    bool ok = fgets(buf, (int)n, f) != NULL; fclose(f);
    if (ok) buf[strcspn(buf, "\r\n")] = 0;
    return ok;
}
static void poll_power(SysStatus *s) {
    s->battery = -1; s->charging = s->plugged = false;
    const char *base = getenv("NXT_TUBE_POWER_DIR"); if (!base) base = "/sys/class/power_supply";   /* QA */
    DIR *d = opendir(base); if (!d) return;
    struct dirent *e; char p[512], v[64];
    int best = -1; bool full = false;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        snprintf(p, sizeof(p), "%s/%s/type", base, e->d_name);
        if (!readline(p, v, sizeof(v))) continue;
        if (!strcasecmp(v, "Battery")) {
            snprintf(p, sizeof(p), "%s/%s/present", base, e->d_name);
            if (readline(p, v, sizeof(v)) && !strcmp(v, "0")) continue;
            snprintf(p, sizeof(p), "%s/%s/capacity", base, e->d_name);
            if (readline(p, v, sizeof(v)) && best < 0) { best = atoi(v); if (best > 100) best = 100; if (best < 0) best = 0; }
            snprintf(p, sizeof(p), "%s/%s/status", base, e->d_name);
            if (readline(p, v, sizeof(v))) {
                if (!strcasecmp(v, "Charging")) s->charging = true;
                if (!strcasecmp(v, "Full")) full = true;
            }
        } else {   /* Mains / USB / USB_PD ...: a charger is plugged in when it is online */
            snprintf(p, sizeof(p), "%s/%s/online", base, e->d_name);
            if (readline(p, v, sizeof(v)) && !strcmp(v, "1")) s->plugged = true;
        }
    }
    closedir(d);
    s->battery = best;
    if (s->charging || full) s->plugged = true;
    s->full = full || (s->plugged && best >= 100);
}
static void poll_wifi(SysStatus *s) {
    s->wifi = 0;
    const char *ifs[] = {"wlan0", "wlan1"};
    char p[128], v[32];
    for (int i = 0; i < 2 && !s->wifi; i++) {
        snprintf(p, sizeof(p), "/sys/class/net/%s/operstate", ifs[i]);
        if (!readline(p, v, sizeof(v)) || strcmp(v, "up")) continue;
        s->wifi = 3;
        FILE *f = fopen("/proc/net/wireless", "r"); if (!f) break;
        char line[256];
        while (fgets(line, sizeof(line), f)) {
            char *c = strstr(line, ifs[i]); if (!c || c[strlen(ifs[i])] != ':') continue;
            int st; float q = 0;
            if (sscanf(c + strlen(ifs[i]) + 1, "%x %f", &st, &q) == 2) s->wifi = q >= 45 ? 3 : q >= 25 ? 2 : 1;
        }
        fclose(f);
    }
}
static void poll_bt(SysStatus *s) {
    /* each Bluetooth link shows up as /sys/class/bluetooth/hciX:<handle> */
    s->bt = false;
    DIR *d = opendir("/sys/class/bluetooth"); if (!d) return;
    struct dirent *e;
    while ((e = readdir(d))) if (!strncmp(e->d_name, "hci", 3) && strchr(e->d_name, ':')) { s->bt = true; break; }
    closedir(d);
}
void Sys_status(SysStatus *out) {
    static SysStatus cur; static Uint32 next; static bool have;
    Uint32 now = SDL_GetTicks();
    if (!have || (Sint32)(now - next) >= 0) {
        poll_power(&cur); poll_wifi(&cur); poll_bt(&cur);
        const char *fake = getenv("NXT_TUBE_FAKE_STATUS");   /* QA: "w2b1" = Wi-Fi 2 bars, Bluetooth on */
        if (fake) { const char *w = strchr(fake, 'w'), *b = strchr(fake, 'b'); if (w) cur.wifi = atoi(w + 1); if (b) cur.bt = atoi(b + 1); }
        next = now + 2000; have = true;
    }
    *out = cur;
}
