/* Device controls: volume (speaker / 3.5 mm / USB DAC), backlight, power key.
 * Speaker and headphones use the firmware settings library (the same calls the
 * launcher and NXT Retro Music make), so the level is shared with the system.
 * A USB DAC is fed by our own ALSA path, so its level is a software gain.
 * SPDX-License-Identifier: GPL-3.0-only */
#include "app.h"
#include <dlfcn.h>
#include <dirent.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <linux/input.h>
#include <sys/ioctl.h>

static void *ms;
static void (*ms_init)(void), (*ms_quit)(void), (*ms_setvol)(int), (*ms_setrawbright)(int), (*ms_setbright)(int);
static int (*ms_getvol)(void), (*ms_getjack)(void), (*ms_getbright)(void);
static void (*ms_setjack)(int);
static int soft_level = 14;          /* used when the settings library is missing */
static bool backlight_off;
static Uint32 save_due;
void Sys_stockTick(Uint32 now);
#ifdef NXT_TUBE_PW
static void pw_init(void); static void pw_quit(void); static void pw_tick(Uint32 now);
static int pw_volume(void); static void pw_setVolume(int level); static bool pw_jack(void); static void pw_backlight(bool on);
void Sys_tick(Uint32 now) { pw_tick(now); if (save_due && (Sint32)(now - save_due) >= 0) { save_due = 0; Settings_save(); } }
#else
void Sys_tick(Uint32 now) { Sys_stockTick(now); if (save_due && (Sint32)(now - save_due) >= 0) { save_due = 0; Settings_save(); } }
#endif

static void *sym(const char *n) { return ms ? dlsym(ms, n) : NULL; }

#ifdef NXT_TUBE_DEV_TG5050
/* Smart Pro S (stock): the firmware keymon keeps the volume / power / Home keys. The
 * speaker level is the codec's "DAC Volume", read and set with amixer from a worker
 * thread so the UI never waits on it. */
#include <pthread.h>
static pthread_mutex_t sv_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t sv_cond = PTHREAD_COND_INITIALIZER;
static int sv_level = -1, sv_want = -1; static bool sv_read;
static int sv_amixer_get(void) {
    FILE *p = popen("amixer sget 'DAC Volume' 2>/dev/null", "r"); if (!p) return -1;
    char line[256]; int v = -1;
    while (fgets(line, sizeof(line), p)) { char *b = strchr(line, '['); int x; if (b && sscanf(b + 1, "%d%%", &x) == 1) { v = x; break; } }
    pclose(p);
    return v;
}
static void *sv_worker(void *arg) {
    (void)arg;
    pthread_mutex_lock(&sv_lock);
    for (;;) {
        while (sv_want < 0 && !sv_read) {
            struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts); ts.tv_sec += 2;
            if (pthread_cond_timedwait(&sv_cond, &sv_lock, &ts)) break;      /* re-read every 2 s */
        }
        int want = sv_want; sv_want = -1; sv_read = false;
        pthread_mutex_unlock(&sv_lock);
        if (want >= 0) {
            char cmd[96]; snprintf(cmd, sizeof(cmd), "amixer -q sset 'DAC Volume' %d%% 2>/dev/null", want);
            if (system(cmd)) {}
            int fd = open("/sys/class/speaker/mute", O_WRONLY | O_CLOEXEC); if (fd >= 0) { (void)!write(fd, want ? "0" : "1", 1); close(fd); }
        }
        int v = sv_amixer_get();
        pthread_mutex_lock(&sv_lock);
        if (v >= 0 && sv_want < 0) sv_level = v;
    }
    return NULL;
}
static void sps_init(void) {
    pthread_t t;
    if (!pthread_create(&t, NULL, sv_worker, NULL)) pthread_detach(t);
}
static int sps_volume(void) { pthread_mutex_lock(&sv_lock); int v = sv_want >= 0 ? sv_want : sv_level; pthread_mutex_unlock(&sv_lock); return v < 0 ? 10 : (v + 2) / 5; }
static void sps_setVolume(int level) { pthread_mutex_lock(&sv_lock); sv_want = level * 5; pthread_cond_signal(&sv_cond); pthread_mutex_unlock(&sv_lock); }
static bool sps_jack(void) {
    static Uint32 next; static bool jack;
    Uint32 now = SDL_GetTicks();
    if ((Sint32)(now - next) < 0) return jack;
    next = now + 1000; jack = false;
    DIR *d = opendir("/dev/input"); if (!d) return false;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (strncmp(e->d_name, "event", 5)) continue;
        char path[300]; snprintf(path, sizeof(path), "/dev/input/%s", e->d_name);
        int fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC); if (fd < 0) continue;
        unsigned long sup = 0, state = 0;
        if (ioctl(fd, EVIOCGBIT(EV_SW, sizeof(sup)), &sup) >= 0 && (sup & (1UL << SW_HEADPHONE_INSERT)) && ioctl(fd, EVIOCGSW(sizeof(state)), &state) >= 0) {
            jack = !!(state & (1UL << SW_HEADPHONE_INSERT)); close(fd); break;
        }
        close(fd);
    }
    closedir(d);
    return jack;
}
#endif

#ifdef NXT_TUBE_ROM_STOCK
/* Stock firmware: the display belongs to the firmware. Our settings library keeps its own
 * brightness / colour values and InitSettings() applies them, which made the picture darker
 * than the same setting outside the app. So the panel's state is read before and written
 * back right after, and the screen-off key restores the exact level it found. */
#define DISP_SET_BRIGHTNESS 0x102
#define DISP_GET_BRIGHTNESS 0x103
static const char *const DISP_ATTR[] = {"enhance_bright", "enhance_contrast", "enhance_saturation", "color_temperature", "enhance_mode"};
#define NATTR (int)(sizeof(DISP_ATTR) / sizeof(DISP_ATTR[0]))
typedef struct { long raw; bool raw_ok; char bl_path[4][200]; char bl[4][16]; int nbl; char attr[NATTR][32]; bool attr_ok[NATTR]; } DispState;
static DispState disp_boot, disp_off;
static bool read_small(const char *p, char *out, size_t n) {
    int fd = open(p, O_RDONLY | O_CLOEXEC); if (fd < 0) return false;
    ssize_t r = read(fd, out, n - 1); close(fd);
    if (r <= 0) return false;
    out[r] = 0; while (r > 0 && (out[r - 1] == '\n' || out[r - 1] == ' ')) out[--r] = 0;
    return true;
}
static void write_small(const char *p, const char *v) {
    char cur[32];
    if (read_small(p, cur, sizeof(cur)) && !strcmp(cur, v)) return;
    int fd = open(p, O_WRONLY | O_CLOEXEC); if (fd < 0) return;
    (void)!write(fd, v, strlen(v)); close(fd);
}
static void disp_save(DispState *d) {
    memset(d, 0, sizeof(*d));
    int fd = open("/dev/disp", O_RDWR | O_CLOEXEC);
    if (fd >= 0) { unsigned long a[4] = {0, 0, 0, 0}; long v = ioctl(fd, DISP_GET_BRIGHTNESS, a); if (v >= 0) { d->raw = v; d->raw_ok = true; } close(fd); }
    DIR *dir = opendir("/sys/class/backlight");
    struct dirent *e;
    while (dir && (e = readdir(dir)) && d->nbl < 4) {
        if (e->d_name[0] == '.') continue;
        snprintf(d->bl_path[d->nbl], sizeof(d->bl_path[0]), "/sys/class/backlight/%s/brightness", e->d_name);
        if (read_small(d->bl_path[d->nbl], d->bl[d->nbl], sizeof(d->bl[0]))) d->nbl++;
    }
    if (dir) closedir(dir);
    for (int i = 0; i < NATTR; i++) {
        char p[160]; snprintf(p, sizeof(p), "/sys/class/disp/disp/attr/%s", DISP_ATTR[i]);
        d->attr_ok[i] = read_small(p, d->attr[i], sizeof(d->attr[i]));
    }
}
static void disp_restore(const DispState *d, bool attrs) {
    if (d->raw_ok) {
        int fd = open("/dev/disp", O_RDWR | O_CLOEXEC);
        if (fd >= 0) { unsigned long a[4] = {0, (unsigned long)d->raw, 0, 0}; ioctl(fd, DISP_SET_BRIGHTNESS, a); close(fd); }
    }
    for (int i = 0; i < d->nbl; i++) write_small(d->bl_path[i], d->bl[i]);
    if (attrs) for (int i = 0; i < NATTR; i++) if (d->attr_ok[i]) {
        char p[160]; snprintf(p, sizeof(p), "/sys/class/disp/disp/attr/%s", DISP_ATTR[i]);
        write_small(p, d->attr[i]);
    }
}
static void disp_dark(void) {
    int fd = open("/dev/disp", O_RDWR | O_CLOEXEC);
    if (fd >= 0) { unsigned long a[4] = {0, 0, 0, 0}; ioctl(fd, DISP_SET_BRIGHTNESS, a); close(fd); }
    for (int i = 0; i < disp_off.nbl; i++) write_small(disp_off.bl_path[i], "0");
}
#endif

void Sys_init(void) {
#ifdef NXT_TUBE_PW
    pw_init(); return;
#endif
#ifdef NXT_TUBE_DEV_TG5050
    sps_init();
    disp_save(&disp_boot);
    Log_msg("Smart Pro S: firmware keeps volume / power / Home; display level %ld", disp_boot.raw_ok ? disp_boot.raw : -1L);
    return;
#endif
    const char *sys = getenv("SYSTEM_PATH");
    char p[1024];
#ifdef NXT_TUBE_ROM_NXREDUX
    /* NX Redux: the firmware's own library (shared with its launcher) */
    if (sys && *sys) { snprintf(p, sizeof(p), "%s/lib/libmsettings.so", sys); ms = dlopen(p, RTLD_NOW | RTLD_GLOBAL); }
#endif
    /* NextUI / Stock: the copy shipped in bin/tg5040 (NextUI's, or its Stock build) */
    if (!ms) ms = dlopen("libmsettings.so", RTLD_NOW | RTLD_GLOBAL);
    if (!ms && sys && *sys) { snprintf(p, sizeof(p), "%s/lib/libmsettings.so", sys); ms = dlopen(p, RTLD_NOW | RTLD_GLOBAL); }
    if (!ms) { Log_msg("settings library not found: volume/brightness use fallbacks (%s)", dlerror()); return; }
    ms_init = sym("InitSettings"); ms_quit = sym("QuitSettings");
    ms_getvol = sym("GetVolume"); ms_setvol = sym("SetVolume"); ms_getjack = sym("GetJack"); ms_setjack = sym("SetJack");
    ms_getbright = sym("GetBrightness"); ms_setbright = sym("SetBrightness"); ms_setrawbright = sym("SetRawBrightness");
#ifdef NXT_TUBE_ROM_STOCK
    disp_save(&disp_boot);
#endif
    if (ms_init) ms_init();
#ifdef NXT_TUBE_ROM_STOCK
    disp_restore(&disp_boot, true);
    Log_msg("display kept as the firmware set it (level %ld)", disp_boot.raw_ok ? disp_boot.raw : -1L);
#endif
    Log_msg("settings library ready: volume %d/20, brightness %d", ms_getvol ? ms_getvol() : -1, ms_getbright ? ms_getbright() : -1);
}
void Sys_quit(void) {
    if (backlight_off) Sys_setBacklight(true);
#ifdef NXT_TUBE_PW
    pw_quit();
#endif
    if (ms_quit) ms_quit();
}

static bool dac(void) { return Player_usbOutput(); }
/* perceptual taper for the software gain: 0..20 -> about -50..0 dB */
static float gain_for(int level) {
    if (level <= 0) return 0.0f;
    if (level >= 20) return 1.0f;
    double db = -50.0 * pow(1.0 - level / 20.0, 1.6);
    return (float)pow(10.0, db / 20.0);
}
int Sys_volume(void) {
    if (dac()) return S.dac_volume;
#ifdef NXT_TUBE_PW
    return pw_volume();
#endif
#ifdef NXT_TUBE_DEV_TG5050
    return sps_volume();
#endif
    if (ms_getvol) { int v = ms_getvol(); return v < 0 ? 0 : v > 20 ? 20 : v; }
    return soft_level;
}
const char *Sys_output(void) {
    if (dac()) return "DAC USB";
#ifdef NXT_TUBE_PW
    return pw_jack() ? "Tai nghe" : "Loa";
#endif
#ifdef NXT_TUBE_DEV_TG5050
    return sps_jack() ? "Tai nghe" : "Loa";
#endif
    if (ms_getjack && ms_getjack()) return "Tai nghe";
    return "Loa";
}
/* re-apply the right gain after the output changed */
void Sys_applyGain(void) {
#ifdef NXT_TUBE_PW
    Player_setGain(dac() ? gain_for(S.dac_volume) : 1.0f);   /* the OS mixer sets the speaker / headphone level */
    return;
#endif
#ifdef NXT_TUBE_DEV_TG5050
    Player_setGain(dac() ? gain_for(S.dac_volume) : 1.0f); return;
#endif
    if (dac()) Player_setGain(gain_for(S.dac_volume));
    else Player_setGain(ms_setvol ? 1.0f : gain_for(soft_level));
}
void Sys_setVolume(int level) {
    if (level < 0) level = 0; if (level > 20) level = 20;
    if (dac()) { S.dac_volume = level; save_due = SDL_GetTicks() + 1200; }
#ifdef NXT_TUBE_PW
    else pw_setVolume(level);
    if (0) {}
#endif
#ifdef NXT_TUBE_DEV_TG5050
    else sps_setVolume(level);
    if (0) {}
#endif
    else if (ms_setvol) ms_setvol(level);
    else soft_level = level;
    Sys_applyGain();
}

void Sys_setBacklight(bool on) {
    if (on == !backlight_off) return;
    backlight_off = !on;
#ifdef NXT_TUBE_PW
    pw_backlight(on);
    Log_msg("backlight %s", on ? "on" : "off");
    return;
#endif
#ifdef NXT_TUBE_ROM_STOCK
    /* the level the firmware (or its Task Manager) set, not our library's own */
    if (!on) { disp_save(&disp_off); disp_dark(); }
    else disp_restore(&disp_off, false);
    Log_msg("backlight %s", on ? "on" : "off");
    return;
#endif
    if (ms_setrawbright) {
        if (on) { ms_setrawbright(8); if (ms_setbright && ms_getbright) ms_setbright(ms_getbright()); }
        else ms_setrawbright(0);
    } else {
        int fd = open("/sys/class/backlight/backlight/bl_power", O_WRONLY);
        if (fd >= 0) { (void)!write(fd, on ? "0" : "4", 1); close(fd); }
    }
    Log_msg("backlight %s", on ? "on" : "off");
}
bool Sys_backlightOff(void) { return backlight_off; }

/* power key: read straight from the input devices that report it */
#define MAXFD 8
static int pfd[MAXFD], npfd = -1;
static void open_power(void) {
    npfd = 0;
    DIR *d = opendir("/dev/input");
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d)) && npfd < MAXFD) {
        if (strncmp(e->d_name, "event", 5)) continue;
        char p[300]; snprintf(p, sizeof(p), "/dev/input/%s", e->d_name);
        int fd = open(p, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0) continue;
        unsigned long bits[(KEY_MAX + 1) / (8 * sizeof(long)) + 1]; memset(bits, 0, sizeof(bits));
#ifdef NXT_TUBE_PW
        bool has = ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(bits)), bits) >= 0 &&
                   ((bits[KEY_POWER / (8 * sizeof(long))] >> (KEY_POWER % (8 * sizeof(long)))) & 1);
#else
        bool has = ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(bits)), bits) >= 0 &&
                   ((bits[KEY_POWER / (8 * sizeof(long))] >> (KEY_POWER % (8 * sizeof(long)))) & 1 ||
                    (bits[KEY_HOME / (8 * sizeof(long))] >> (KEY_HOME % (8 * sizeof(long)))) & 1);
#endif
        if (has) { pfd[npfd++] = fd; Log_msg("power key on %s", p); } else close(fd);
    }
    closedir(d);
}
bool Sys_powerAvailable(void) { if (npfd < 0) open_power(); return npfd > 0; }
/* true once per short press of the power button */
bool Sys_powerPressed(void) {
    if (npfd < 0) open_power();
    bool pressed = false;
    for (int i = 0; i < npfd; i++) {
        struct input_event ev;
        while (read(pfd[i], &ev, sizeof(ev)) == (ssize_t)sizeof(ev))
#ifdef NXT_TUBE_PW
            if (ev.type == EV_KEY && ev.code == KEY_POWER) {
#else
            if (ev.type == EV_KEY && (ev.code == KEY_POWER || ev.code == 102)) {
#endif
                if (ev.value == 1) pressed = true;
                Log_msg("power key code %d value %d", ev.code, ev.value);
            }
    }
    return pressed;
}

/* Is the firmware's key daemon (TrimUI keymon) running and not paused by us?
 * It opens the firmware panel on the Home key by itself. */
bool Sys_keymonActive(void) {
    static Uint32 next; static bool active;
    Uint32 now = SDL_GetTicks();
    if ((Sint32)(now - next) < 0) return active;
    next = now + 3000; active = false;
    DIR *d = opendir("/proc"); if (!d) return false;
    struct dirent *e;
    while (!active && (e = readdir(d))) {
        char *end; long n = strtol(e->d_name, &end, 10);
        if (*end || n <= 1) continue;
        char p[64], name[32] = "", line[128]; snprintf(p, sizeof(p), "/proc/%ld/comm", n);
        FILE *f = fopen(p, "r"); if (!f) continue;
        bool km = fgets(name, sizeof(name), f) && !strcmp(name, "keymon\n"); fclose(f);
        if (!km) continue;
        snprintf(p, sizeof(p), "/proc/%ld/status", n);
        f = fopen(p, "r"); if (!f) continue;
        bool stopped = false;
        while (fgets(line, sizeof(line), f)) if (!strncmp(line, "State:", 6)) { const char *c = line + 6; while (*c == ' ' || *c == '\t') c++; stopped = *c == 'T' || *c == 't'; break; }
        fclose(f);
        if (!stopped) active = true;
    }
    closedir(d);
    return active;
}

/* CPU policy (less heat, same picture):
 *   0 saver   screen off / background listening: on-demand governor, clocks capped low
 *   1 normal  menus and ordinary video: on-demand governor (clocks rise only under load)
 *   2 boost   60 fps / 1080p video, or the decoder fell behind: all cores at full speed
 * The governor found at start is put back on exit. */
#define CPU_PATH "/sys/devices/system/cpu/cpu%d/cpufreq/%s"
static char gov_saved[32], gov_dynamic[32]; static int cpu_level = -1; static long fmax_saved, fmin_cur;
static bool cpu_read(int c, const char *node, char *out, size_t n) {
    char p[160]; snprintf(p, sizeof(p), CPU_PATH, c, node);
    int fd = open(p, O_RDONLY); if (fd < 0) return false;
    ssize_t r = read(fd, out, n - 1); close(fd);
    if (r <= 0) return false;
    out[r] = 0; while (r > 0 && (out[r - 1] == '\n' || out[r - 1] == ' ')) out[--r] = 0;
    return true;
}
static void cpu_write_all(const char *node, const char *v) {
    for (int c = 0; c < 8; c++) {
        char p[160]; snprintf(p, sizeof(p), CPU_PATH, c, node);
        int fd = open(p, O_WRONLY); if (fd < 0) continue;
        (void)!write(fd, v, strlen(v)); close(fd);
    }
}
static void cpu_init(void) {
    if (gov_dynamic[0]) return;
    char avail[256] = "";
    cpu_read(0, "scaling_governor", gov_saved, sizeof(gov_saved));
    cpu_read(0, "scaling_available_governors", avail, sizeof(avail));
    const char *pref[] = {"ondemand", "schedutil", "interactive", "conservative"};
    for (int i = 0; i < 4 && !gov_dynamic[0]; i++) {
        const char *p = strstr(avail, pref[i]); size_t l = strlen(pref[i]);
        if (p && (p == avail || p[-1] == ' ') && (p[l] == 0 || p[l] == ' ')) snprintf(gov_dynamic, sizeof(gov_dynamic), "%s", pref[i]);
    }
    if (!gov_dynamic[0]) snprintf(gov_dynamic, sizeof(gov_dynamic), "ondemand");
    char v[32];
    if (cpu_read(0, "cpuinfo_max_freq", v, sizeof(v))) fmax_saved = atol(v);
    if (cpu_read(0, "cpuinfo_min_freq", v, sizeof(v))) fmin_cur = atol(v);
    Log_msg("cpu: governor %s at start, dynamic %s, %ld-%ld kHz", gov_saved[0] ? gov_saved : "?", gov_dynamic, fmin_cur, fmax_saved);
}
void Sys_cpuLevel(int level) {
    cpu_init();
    if (level == cpu_level) return;
    cpu_level = level;
    char f[32];
    if (level == 2) cpu_write_all("scaling_governor", "performance");
    else cpu_write_all("scaling_governor", gov_dynamic);
    /* saver: the audio decoder needs very little; keep clocks near the bottom */
    if (fmax_saved > 0) {
        long cap = level == 0 ? (fmax_saved / 2 > fmin_cur ? fmax_saved / 2 : fmax_saved) : fmax_saved;
        snprintf(f, sizeof(f), "%ld", cap); cpu_write_all("scaling_max_freq", f);
    }
    Log_msg("cpu: %s", level == 2 ? "boost (performance)" : level == 1 ? gov_dynamic : "saver");
}
void Sys_cpuRestore(void) {
    if (!gov_dynamic[0]) return;
    char f[32];
    if (fmax_saved > 0) { snprintf(f, sizeof(f), "%ld", fmax_saved); cpu_write_all("scaling_max_freq", f); }
    /* the launcher set "performance" for us in older releases; hand back an on-demand policy then */
    cpu_write_all("scaling_governor", gov_saved[0] && strcmp(gov_saved, "performance") ? gov_saved : gov_dynamic);
    cpu_level = -1;
}
void Sys_setGovernor(bool performance) { Sys_cpuLevel(performance ? 1 : 0); }

/* ---------- official TrimUI firmware (Stock) ----------
 * Like NXT Retro Music: while our window is up, the firmware's keymon is
 * paused (it would otherwise sleep the device on Power and change volume on its
 * own), recovery markers let launch.sh resume it after a crash, and the
 * stay_awake / stay_alive flags keep the firmware from auto-sleeping playback. */
#ifdef NXT_TUBE_ROM_STOCK
#include <signal.h>
#include <errno.h>
#include <ctype.h>
#include <sys/stat.h>
static char runtime[512];
static pid_t paused[16]; static int npaused;
static bool stock_active, stock_shown;
static bool is_keymon(pid_t pid) {
    char path[64], name[64] = ""; snprintf(path, sizeof(path), "/proc/%ld/comm", (long)pid);
    FILE *f = fopen(path, "r"); if (!f) return false;
    bool ok = fgets(name, sizeof(name), f) && !strcmp(name, "keymon\n");
    fclose(f); return ok;
}
static bool stopped(pid_t pid) {
    char path[64], line[128]; snprintf(path, sizeof(path), "/proc/%ld/status", (long)pid);
    FILE *f = fopen(path, "r"); if (!f) return true;
    bool st = false;
    while (fgets(line, sizeof(line), f)) if (!strncmp(line, "State:", 6)) { char *c = line + 6; while (isspace((unsigned char)*c)) c++; st = *c == 'T' || *c == 't'; break; }
    fclose(f); return st;
}
static void keymon_pause(void) {
    DIR *d = opendir("/proc"); if (!d) return;
    struct dirent *e;
    while ((e = readdir(d)) && npaused < 16) {
        char *end; long n = strtol(e->d_name, &end, 10);
        if (*end || n <= 1 || n == getpid() || !is_keymon((pid_t)n) || stopped((pid_t)n)) continue;
        char m[640]; snprintf(m, sizeof(m), "%s/keymon-%ld", runtime, n);
        int fd = open(m, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600); if (fd < 0) continue; close(fd);
        if (!kill((pid_t)n, SIGSTOP)) paused[npaused++] = (pid_t)n; else unlink(m);
    }
    closedir(d);
    if (npaused) Log_msg("firmware keymon paused (%d)", npaused);
}
static void keymon_resume(void) {
    for (int i = 0; i < npaused; i++) {
        if (is_keymon(paused[i])) kill(paused[i], SIGCONT);
        char m[640]; snprintf(m, sizeof(m), "%s/keymon-%ld", runtime, (long)paused[i]); unlink(m);
    }
    npaused = 0;
}
static void hold_flag(const char *name) {
    char path[128], marker[640]; snprintf(path, sizeof(path), "/tmp/%s", name); snprintf(marker, sizeof(marker), "%s/owned-%s", runtime, name);
    int fd = open(path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0644);
    if (fd < 0) return;                       /* someone else's flag: leave it */
    if (write(fd, "1\n", 2) != 2 || link(path, marker)) unlink(path);
    close(fd);
}
static void release_flag(const char *name) {
    char path[128], marker[640]; struct stat a, b;
    snprintf(path, sizeof(path), "/tmp/%s", name); snprintf(marker, sizeof(marker), "%s/owned-%s", runtime, name);
    if (!lstat(path, &a) && !lstat(marker, &b) && a.st_dev == b.st_dev && a.st_ino == b.st_ino) unlink(path);
    unlink(marker);
}
void Sys_stockVisible(bool visible) {
#ifdef NXT_TUBE_DEV_TG5050
    (void)visible; return;     /* Smart Pro S: keymon keeps running (volume, power, Home panel) */
#endif
    if (!stock_active || stock_shown == visible) return;
    stock_shown = visible;
    if (visible) keymon_pause(); else keymon_resume();
}
void Sys_stockInit(void) {
    const char *dir = getenv("NXT_TUBE_RUNTIME_DIR");
    snprintf(runtime, sizeof(runtime), "%s", dir && *dir ? dir : "/tmp/nxt-retro-tube-stock");
    if (mkdir(runtime, 0700) && errno != EEXIST) return;
    stock_active = true;
    hold_flag("stay_awake"); hold_flag("stay_alive");
    Sys_stockVisible(true);
}
void Sys_stockQuit(void) {
    if (!stock_active) return;
    keymon_resume();
    release_flag("stay_awake"); release_flag("stay_alive");
    stock_active = stock_shown = false;
}
/* the stock keymon does not publish the headphone jack: read the switch state */
void Sys_stockTick(Uint32 now) {
    static Uint32 next;
    if (!stock_active || !ms_setjack || !ms_getjack || (Sint32)(now - next) < 0) return;
    next = now + 500;
    DIR *d = opendir("/dev/input"); if (!d) return;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (strncmp(e->d_name, "event", 5)) continue;
        char path[300]; snprintf(path, sizeof(path), "/dev/input/%s", e->d_name);
        int fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC); if (fd < 0) continue;
        unsigned long sup = 0, state = 0;
        if (ioctl(fd, EVIOCGBIT(EV_SW, sizeof(sup)), &sup) >= 0 && (sup & (1UL << SW_HEADPHONE_INSERT)) && ioctl(fd, EVIOCGSW(sizeof(state)), &state) >= 0) {
            int jack = !!(state & (1UL << SW_HEADPHONE_INSERT));
            if (ms_getjack() != jack) ms_setjack(jack);
            close(fd); break;
        }
        close(fd);
    }
    closedir(d);
}
#else
void Sys_stockVisible(bool visible) { (void)visible; }
void Sys_stockInit(void) {}
void Sys_stockQuit(void) {}
void Sys_stockTick(Uint32 now) { (void)now; }
#endif

/* ---------- muOS / Knulli ----------
 * Both run PipeWire and own the volume keys, power key and idle timers:
 *  - volume: the OS changes the default sink on the volume keys; we read the
 *    level back (wpctl / knulli-audio) for the on-screen meter, and set it from
 *    the quick panel. A USB DAC picked by us keeps our software gain.
 *  - power: while our window is up the OS is told not to suspend on a short
 *    press (muOS "caffeine", Knulli power-button wake flag), so the key turns
 *    the screen off and the sound goes on, like NXT Retro Music.
 *  - idle: dimming / auto-sleep are held off while we run (muOS
 *    idle_game_inhibit, Knulli battery-saver pause).
 * Every flag we create carries our pid and is removed on exit; launch.sh also
 * clears flags left by a crashed run. */
#ifdef NXT_TUBE_PW
#include <pthread.h>
#include <errno.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <spawn.h>
extern char **environ;

static pthread_t pw_thread; static bool pw_thread_on, pw_stop;
static pthread_mutex_t pw_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t pw_cond = PTHREAD_COND_INITIALIZER;
static int pw_level = -1;            /* 0..100, -1 unknown */
static int pw_want = -1;             /* pending set request */
static int pw_reads;                 /* pending reads */
static bool pw_visible;
static int jack_state = -1; static Uint32 jack_next;
static unsigned long disp_saved; static bool disp_saved_ok;

#ifdef NXT_TUBE_ROM_MUOS
#define FLAG_POWER "/run/muos/caffeine"
#define FLAG_IDLE  "/run/muos/idle_game_inhibit"
#elif defined(NXT_TUBE_ROM_DARKOS)
/* dArkOS (R36S & co.): ogage owns volume / brightness / power; ALSA, no flags to set */
#else
#define FLAG_POWER "/var/run/power-button.wake"      /* Knulli 2026: short press consumes it, no suspend */
#define FLAG_POWER_OLD "/var/run/suspend.debounce"   /* Knulli 2025 (Gladiator) */
#define FLAG_IDLE  "/var/run/battery-saver/nxt-retro-tube.pause"
#endif

/* run a command, capture the first line of stdout (no shell) */
static bool run_cmd(char *const argv[], char *out, size_t n) {
    int fds[2]; if (pipe(fds)) return false;
    posix_spawn_file_actions_t fa; posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_addopen(&fa, 0, "/dev/null", O_RDONLY, 0);
    posix_spawn_file_actions_adddup2(&fa, fds[1], 1);
    posix_spawn_file_actions_addopen(&fa, 2, "/dev/null", O_WRONLY, 0);
    posix_spawn_file_actions_addclose(&fa, fds[0]);
    pid_t pid; int rc = posix_spawnp(&pid, argv[0], &fa, NULL, argv, environ);
    posix_spawn_file_actions_destroy(&fa); close(fds[1]);
    if (rc) { close(fds[0]); return false; }
    size_t got = 0; if (out && n) out[0] = 0;
    for (;;) {
        char buf[256]; ssize_t r = read(fds[0], buf, sizeof(buf));
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) break;
        if (out) for (ssize_t i = 0; i < r && got + 1 < n; i++) out[got++] = buf[i];
    }
    if (out && n) out[got] = 0;
    close(fds[0]);
    int st = 0; while (waitpid(pid, &st, 0) < 0 && errno == EINTR) {}
    return WIFEXITED(st) && WEXITSTATUS(st) == 0;
}
static bool exists_exec(const char *p) { return !access(p, X_OK); }
static int read_level(void) {
    char out[256];
#ifdef NXT_TUBE_ROM_DARKOS
    /* the firmware's own reader: "75%" (softvol Master or the dmixer Playback control) */
    char *cv[] = {"/usr/local/bin/current_volume", NULL};
    if (exists_exec(cv[0]) && run_cmd(cv, out, sizeof(out))) { int v; if (sscanf(out, "%d", &v) == 1) return v < 0 ? 0 : v > 100 ? 100 : v; }
    const char *ctl[] = {"Playback", "Master"};
    for (int i = 0; i < 2; i++) {
        char *am[] = {"amixer", "sget", (char *)ctl[i], NULL};
        if (run_cmd(am, out, sizeof(out))) { char *p = strchr(out, '['); int v; if (p && sscanf(p + 1, "%d%%", &v) == 1) return v; }
    }
    return -1;
#endif
#ifdef NXT_TUBE_ROM_KNULLI
    const char *tool = exists_exec("/usr/bin/knulli-audio") ? "/usr/bin/knulli-audio" : exists_exec("/usr/bin/batocera-audio") ? "/usr/bin/batocera-audio" : NULL;
    if (tool) {
        char *a[] = {(char *)tool, "getSystemVolume", NULL};
        if (run_cmd(a, out, sizeof(out))) { int v; if (sscanf(out, "%d", &v) == 1) return v < 0 ? 0 : v > 100 ? 100 : v; }
    }
#endif
    char *w[] = {"wpctl", "get-volume", "@DEFAULT_AUDIO_SINK@", NULL};
    if (run_cmd(w, out, sizeof(out))) {
        char *p = strstr(out, "Volume:"); double f;
        if (p && sscanf(p + 7, "%lf", &f) == 1) { int v = (int)(f * 100 + 0.5); return v < 0 ? 0 : v > 100 ? 100 : v; }
    }
    return -1;
}
static void write_level(int pct) {
    char v[16];
#ifdef NXT_TUBE_ROM_DARKOS
    snprintf(v, sizeof(v), "%d%%", pct);
    char *a[] = {"amixer", "-q", "sset", "Playback", v, NULL};
    if (!run_cmd(a, NULL, 0)) { char *b[] = {"amixer", "-q", "sset", "Master", v, NULL}; run_cmd(b, NULL, 0); }
    return;
#endif
#ifdef NXT_TUBE_ROM_KNULLI
    const char *tool = exists_exec("/usr/bin/knulli-audio") ? "/usr/bin/knulli-audio" : exists_exec("/usr/bin/batocera-audio") ? "/usr/bin/batocera-audio" : NULL;
    if (tool) { snprintf(v, sizeof(v), "%d", pct); char *a[] = {(char *)tool, "setSystemVolume", v, NULL}; if (run_cmd(a, NULL, 0)) return; }
#endif
    snprintf(v, sizeof(v), "%d%%", pct);
    char *w[] = {"wpctl", "set-volume", "@DEFAULT_AUDIO_SINK@", v, NULL};
    run_cmd(w, NULL, 0);
#ifdef NXT_TUBE_ROM_MUOS
    char *m[] = {"wpctl", "set-mute", "@DEFAULT_AUDIO_SINK@", "0", NULL};
    run_cmd(m, NULL, 0);
#endif
}
static void *pw_worker(void *arg) {
    (void)arg;
    pthread_mutex_lock(&pw_lock);
    while (!pw_stop) {
        if (pw_want < 0 && pw_reads <= 0) { pthread_cond_wait(&pw_cond, &pw_lock); continue; }
        int want = pw_want; pw_want = -1;
        if (pw_reads > 0) pw_reads--;
        pthread_mutex_unlock(&pw_lock);
        if (want >= 0) write_level(want);
        int v = read_level();
        pthread_mutex_lock(&pw_lock);
        if (v >= 0 && pw_want < 0) pw_level = v;
        if (pw_reads > 0 && !pw_stop) {     /* key repeat: sample again a little later */
            pthread_mutex_unlock(&pw_lock); usleep(180000); pthread_mutex_lock(&pw_lock);
        }
    }
    pthread_mutex_unlock(&pw_lock);
    return NULL;
}
static void pw_request(int reads) {
    pthread_mutex_lock(&pw_lock); if (pw_reads < reads) pw_reads = reads; pthread_cond_signal(&pw_cond); pthread_mutex_unlock(&pw_lock);
}
static int pw_volume(void) {
    pthread_mutex_lock(&pw_lock); int v = pw_want >= 0 ? pw_want : pw_level; pthread_mutex_unlock(&pw_lock);
    if (v < 0) v = 50;
    return (v + 2) / 5;
}
static void pw_setVolume(int level) {
    pthread_mutex_lock(&pw_lock); pw_want = level * 5; pthread_cond_signal(&pw_cond); pthread_mutex_unlock(&pw_lock);
}
bool Sys_volumeExternal(void) { return true; }
/* a volume key was pressed: the OS changes the level, sample it a few times */
void Sys_volumeKey(void) { pw_request(4); }

/* headphone jack from the switch device */
static bool pw_jack(void) {
    Uint32 now = SDL_GetTicks();
    if (jack_state >= 0 && (Sint32)(now - jack_next) < 0) return jack_state == 1;
    jack_next = now + 1000; jack_state = 0;
    DIR *d = opendir("/dev/input"); if (!d) return false;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (strncmp(e->d_name, "event", 5)) continue;
        char path[300]; snprintf(path, sizeof(path), "/dev/input/%s", e->d_name);
        int fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC); if (fd < 0) continue;
        unsigned long sup = 0, state = 0;
        if (ioctl(fd, EVIOCGBIT(EV_SW, sizeof(sup)), &sup) >= 0 && (sup & (1UL << SW_HEADPHONE_INSERT)) && ioctl(fd, EVIOCGSW(sizeof(state)), &state) >= 0) {
            jack_state = !!(state & (1UL << SW_HEADPHONE_INSERT)); close(fd); break;
        }
        close(fd);
    }
    closedir(d);
    return jack_state == 1;
}

/* backlight: the sunxi display driver (same vendor kernel on both) */
#define DISP_LCD_SET_BRIGHTNESS 0x102
#define DISP_LCD_GET_BRIGHTNESS 0x103
static void bl_power(bool on) {
    DIR *d = opendir("/sys/class/backlight"); if (!d) return;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        char p[300]; snprintf(p, sizeof(p), "/sys/class/backlight/%s/bl_power", e->d_name);
        int fd = open(p, O_WRONLY); if (fd >= 0) { (void)!write(fd, on ? "0" : "4", 1); close(fd); }
    }
    closedir(d);
}
#ifdef NXT_TUBE_ROM_DARKOS
/* Rockchip PWM backlight: remember the level, write 0, put it back (brightnessctl if sysfs is root-only) */
static char bl_saved[16];
static void bl_level(bool on) {
    DIR *d = opendir("/sys/class/backlight"); struct dirent *e; bool done = false;
    while (d && (e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        char p[300]; snprintf(p, sizeof(p), "/sys/class/backlight/%s/brightness", e->d_name);
        if (!on) { int fd = open(p, O_RDONLY); if (fd >= 0) { ssize_t r = read(fd, bl_saved, sizeof(bl_saved) - 1); bl_saved[r > 0 ? r : 0] = 0; close(fd); } }
        int fd = open(p, O_WRONLY);
        if (fd >= 0) { const char *v = on ? bl_saved : "0"; if (*v && write(fd, v, strlen(v)) > 0) done = true; close(fd); }
    }
    if (d) closedir(d);
    if (!done) {
        char *off[] = {"brightnessctl", "-q", "-s", "set", "0", NULL}, *back[] = {"brightnessctl", "-q", "-r", NULL};
        run_cmd(on ? back : off, NULL, 0);
    }
}
#endif
static void pw_backlight(bool on) {
#ifdef NXT_TUBE_ROM_DARKOS
    bl_level(on);
#endif
    int fd = open("/dev/disp", O_RDWR | O_CLOEXEC);
    if (fd >= 0) {
        unsigned long a[4] = {0, 0, 0, 0};
        if (!on) {
            long cur = ioctl(fd, DISP_LCD_GET_BRIGHTNESS, a);
            if (cur > 0) { disp_saved = (unsigned long)cur; disp_saved_ok = true; }
            a[0] = 0; a[1] = 0; ioctl(fd, DISP_LCD_SET_BRIGHTNESS, a);
        } else if (disp_saved_ok) {
            a[0] = 0; a[1] = disp_saved; ioctl(fd, DISP_LCD_SET_BRIGHTNESS, a);
        }
        close(fd);
    }
    bl_power(on);
}

/* OS flags: created only if absent, tagged with our pid, removed only if still ours */
static bool flag_hold(const char *path) {
    char dir[256]; snprintf(dir, sizeof(dir), "%s", path);
    char *sl = strrchr(dir, '/'); if (sl) { *sl = 0; mkdir(dir, 0755); }
    int fd = open(path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0644);
    if (fd < 0) {
        /* left by a crashed run of ours? (launch.sh normally clears those) */
        FILE *f = fopen(path, "r"); long pid = 0;
        if (f) { if (fscanf(f, "%ld", &pid) != 1) pid = 0; fclose(f); }
        if (pid == getpid()) return true;
        if (pid <= 0 || !kill((pid_t)pid, 0) || errno != ESRCH) return false;   /* someone else's: leave it */
        unlink(path);
        fd = open(path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0644);
        if (fd < 0) return false;
    }
    char t[32]; int n = snprintf(t, sizeof(t), "%ld\n", (long)getpid());
    (void)!write(fd, t, n); close(fd);
    return true;
}
static void flag_drop(const char *path) {
    FILE *f = fopen(path, "r"); long pid = 0;
    if (!f) return;
    if (fscanf(f, "%ld", &pid) != 1) pid = 0;
    fclose(f);
    if (pid == getpid()) unlink(path);
}
static void power_flags(bool hold) {
#ifndef FLAG_POWER
    (void)hold; return;
#else
    if (hold) { flag_hold(FLAG_POWER);
#ifdef FLAG_POWER_OLD
        flag_hold(FLAG_POWER_OLD);
#endif
    } else { flag_drop(FLAG_POWER);
#ifdef FLAG_POWER_OLD
        flag_drop(FLAG_POWER_OLD);
#endif
    }
#endif
}
void Sys_pwVisible(bool visible) {
    if (pw_visible == visible) return;
    pw_visible = visible;
    power_flags(visible);
#ifdef FLAG_POWER
    Log_msg("OS power key %s", visible ? "held by the app (screen off)" : "back to the system");
#endif
}
static void pw_init(void) {
#ifdef FLAG_IDLE
    flag_hold(FLAG_IDLE);
#endif
    Sys_pwVisible(true);
    pw_level = read_level();
    pw_stop = false;
    pw_thread_on = !pthread_create(&pw_thread, NULL, pw_worker, NULL);
#ifdef NXT_TUBE_ROM_DARKOS
    Log_msg("%s: ALSA volume %d%%", ROM_LABEL, pw_level);
#else
    Log_msg("%s: PipeWire volume %d%%, power/idle flags set", ROM_LABEL, pw_level);
#endif
}
static void pw_quit(void) {
    Sys_pwVisible(false);
#ifdef FLAG_IDLE
    flag_drop(FLAG_IDLE);
#endif
    Sys_keepAwake(false);
    if (pw_thread_on) {
        pthread_mutex_lock(&pw_lock); pw_stop = true; pthread_cond_broadcast(&pw_cond); pthread_mutex_unlock(&pw_lock);
        pthread_join(pw_thread, NULL); pw_thread_on = false;
    }
}
static void pw_tick(Uint32 now) {
    /* Knulli's power-button script deletes its wake flag on every press: put it back */
    static Uint32 next, next_read;
    if ((Sint32)(now - next_read) >= 0) { next_read = now + 3000; if (!backlight_off) pw_request(1); }
    if (!pw_visible || (Sint32)(now - next) < 0) return;
    next = now + 250;
#ifdef NXT_TUBE_ROM_KNULLI
    if (access(FLAG_POWER, F_OK)) flag_hold(FLAG_POWER);
#endif
}

/* dArkOS suspends after its idle timeout unless "kodi-gbm" or "ffplay" runs
 * (/usr/local/bin/processcheck.sh): while a video plays, a tiny child named
 * "ffplay-nxttube" keeps that check happy. It dies with us (PDEATHSIG). */
#ifdef NXT_TUBE_ROM_DARKOS
#include <sys/prctl.h>
static pid_t awake_pid;
void Sys_keepAwake(bool on) {
    if (on == (awake_pid > 0)) return;
    if (on) {
        pid_t p = fork();
        if (p == 0) {
            prctl(PR_SET_PDEATHSIG, SIGKILL);
            prctl(PR_SET_NAME, "ffplay-nxttube");
            for (int fd = 3; fd < 256; fd++) close(fd);
            for (;;) pause();
        }
        if (p > 0) awake_pid = p;
    } else {
        kill(awake_pid, SIGKILL); waitpid(awake_pid, NULL, 0); awake_pid = 0;
    }
}
#else
void Sys_keepAwake(bool on) { (void)on; }
#endif
#else
void Sys_keepAwake(bool on) { (void)on; }
#ifdef NXT_TUBE_DEV_TG5050
bool Sys_volumeExternal(void) { return true; }
#else
bool Sys_volumeExternal(void) { return false; }
#endif
void Sys_volumeKey(void) {}
void Sys_pwVisible(bool visible) { (void)visible; }
#endif
