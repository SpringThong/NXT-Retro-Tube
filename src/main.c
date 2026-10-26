/* NXT Retro Tube — screens, input and app lifecycle.
 * SPDX-License-Identifier: GPL-3.0-only */
#include "app.h"
#include "draw.h"
#ifndef NXT_TUBE_PW
#include <SDL2/SDL_image.h>
#endif
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/wait.h>
#include <dirent.h>
#include <signal.h>
#include <unistd.h>
#include <stdarg.h>
#include <stdio.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <math.h>
#include <ctype.h>

AppPaths paths;
static FILE *g_logf;
void Log_open(const char *path) { g_logf = fopen(path, "w"); }
void Log_msg(const char *fmt, ...) {
    va_list ap; char line[600];
    va_start(ap, fmt); vsnprintf(line, sizeof(line), fmt, ap); va_end(ap);
    time_t t = time(NULL); struct tm tm; localtime_r(&t, &tm);
    fprintf(stderr, "[%02d:%02d:%02d] %s\n", tm.tm_hour, tm.tm_min, tm.tm_sec, line);
    if (g_logf) { fprintf(g_logf, "[%02d:%02d:%02d] %s\n", tm.tm_hour, tm.tm_min, tm.tm_sec, line); fflush(g_logf); }
}

/* ---------- input ---------- */
typedef enum { K_NONE, K_UP, K_DOWN, K_LEFT, K_RIGHT, K_A, K_B, K_X, K_Y, K_L1, K_R1, K_L2, K_R2, K_START, K_SELECT, K_MENU, K_VOLUP, K_VOLDN, K_HOME, K_POWER, K_COUNT } Key;
/* order of Settings.keymap */
static const Key KM_KEYS[KM_COUNT] = {K_A, K_B, K_X, K_Y, K_L1, K_R1, K_L2, K_R2, K_START, K_SELECT};
static const char *KM_LABEL[KM_COUNT] = {"A", "B", "X", "Y", "L1", "R1", "L2", "R2", "START", "SELECT"};
static const char *KM_ROLE[KM_COUNT] = {"Chọn · phát/dừng", "Về · xóa chữ", "Xem sau · chất lượng", "Tìm kiếm · toàn màn hình",
    "Mục trước · video trước", "Mục sau · video sau", "Chuyển trang · tua lùi 60 giây", "Chuyển trang · tua tới 60 giây", "Làm mới · tốc độ · tìm", "Tùy chọn · Telex"};
static bool held[K_COUNT];
static Uint32 repeat_at[K_COUNT];

/* ---------- state ---------- */
typedef enum { SC_HOME, SC_SEARCH, SC_WATCH, SC_FULL, SC_LIBRARY, SC_SETTINGS, SC_KIDS } Screen;
typedef enum { PANEL_NONE, PANEL_QUALITY, PANEL_SPEED, PANEL_OPTIONS } Panel;
#define CHIPS 7
static const char *CHIP_LABEL[CHIPS] = {"Dành cho bạn", "Âm nhạc", "Game retro", "Chill & Lofi", "Hoạt hình", "Trực tiếp", "Mới tải lên"};
static const char *CHIP_QUERY[CHIPS] = {"video thịnh hành việt nam", "nhạc việt mới nhất", "retro game handheld", "lofi chill", "phim hoạt hình", "radio 24/7 live", "việt nam"};
static const bool CHIP_NEWEST[CHIPS] = {false, false, false, false, false, false, true};
/* kids mode home: tab 0 and the last tab follow the approved channels when there are some */
static const char *KIDS_LABEL[CHIPS] = {"Cho bé", "Hoạt hình", "Nhạc thiếu nhi", "Học tiếng Anh", "Truyện cổ tích", "Khoa học vui", "Kênh đã duyệt"};
static const char *KIDS_QUERY[CHIPS] = {"phim hoạt hình cho bé", "phim hoạt hình thiếu nhi", "nhạc thiếu nhi vui nhộn", "học tiếng anh cho bé", "truyện cổ tích việt nam", "khoa học vui cho bé", "phim hoạt hình cho bé"};
static const char *const *chip_labels(void) { return K.enabled ? KIDS_LABEL : CHIP_LABEL; }
static bool chip_newest(int t) { return K.enabled ? false : CHIP_NEWEST[t]; }
static const char *QUALITY_ID[] = {"auto", "1080", "720p60", "720", "480", "360", "audio"};
static const char *QUALITY_LABEL[] = {"Tự động", "1080p HD", "720p60 HD", "720p HD", "480p", "360p", "Chỉ âm thanh"};
static const char *QUALITY_NOTE[] = {"HD 720p · 60 khung/giây nếu video có", "Nặng máy · màn 1024×768 không nét hơn 720p", "Mượt hơn cho game, thể thao", "30 khung/giây · nhẹ máy nhất", "Tiết kiệm dữ liệu", "Khi Wi-Fi yếu", "Tắt hình, tiết kiệm pin"};
#define NQ 7
static const float SPEEDS[] = {0.5f, 0.75f, 1.0f, 1.25f, 1.5f, 2.0f};
static const char *SPEED_LABEL[] = {"0,5×", "0,75×", "1×", "1,25×", "1,5×", "2×"};
#define NS 6
static const char *KEYS[40] = {"1","2","3","4","5","6","7","8","9","0","q","w","e","r","t","y","u","i","o","p","a","s","d","f","g","h","j","k","l","del","z","x","c","v","b","n","m",".","space","go"};
#define MAXQ 6
#define PAGE_ONE 24

typedef struct {
    VideoList list;
    int jobs[MAXQ]; int njobs;
    char q[MAXQ][128]; int nq;
    int page;              /* pages requested so far */
    bool replacing;        /* first page of a refresh: clear the old list on first result */
    bool tried, exhausted; int added;
    char err[160];
} Feed;

enum { SET_LANG, SET_QUALITY, SET_AUTOPLAY, SET_PREFETCH, SET_SUBS, SET_SUBLANG, SET_SUBSIZE, SET_TELEX, SET_AVSYNC, SET_AUDIO, SET_BOTTOM, SET_OTA_AUTO, SET_HOME, SET_KEYS, SET_KIDS, SET_UPDATE,
       SET_CACHE, SET_HISTORY, SET_SEARCHES, SET_VERSION, SET_ABOUT, SET_ICON, SET_COUNT };
#ifdef NXT_TUBE_ROM_NXREDUX
#define SET_ROWS SET_COUNT          /* Tools-menu icon: NX Redux launcher only */
#else
#define SET_ROWS SET_ICON
#endif

static struct {
    SDL_Window *win; SDL_Renderer *ren; SDL_Joystick *joy;
    bool quit, hidden;
    Screen screen;
    /* home */
    int tab, sel, focus, navSel; float scroll;
    Feed feed[CHIPS];
    /* search */
    int subOverride;          /* this video only: 0 settings, 1 translate, 2 original, 3 off */
    Uint32 osdAskAt, osdGateUntil; bool osdAsk; bool restart; int otaToldState;   /* firmware OSD (TrimUI Task Manager) */
    char raw[96], query[300]; bool telexOff, rawLiteral; int keySel; bool results; int resultSel; float rscroll;
    int searchFocus, recentSel;
    VideoList found; int searchJob; bool channelView, foundAppend, foundEnd; char searchErr[160]; char resultTitle[160];
    char lastQuery[300], channelId[48];
    /* library */
    int libTab, libSel; float lscroll;
    /* settings */
    int setSel; float setScroll; bool keyPage; int keySelRow; int capture; Uint32 captureUntil; int confirmRow; Uint32 confirmUntil;
    char iconStatus[160]; int iconState;
    /* playback */
    VideoItem cur; bool hasCur;
    StreamInfo stream; bool streamReady; int resolveJob; char watchErr[160];
    VideoList queue; int qpos;
    int watchFocus, nextSel, nextFirst, moreJob, moreStart; char moreQuery[300]; bool moreDone; int moreEmpty; Uint32 moreFailedAt;
    int quality, speedIx;
    bool localPlayback;
    Uint32 lastHistory;
    bool endedHandled;
    /* overlays */
    bool osd; Uint32 osdUntil; Panel panel; int panelSel; bool stats;
    char toast[200]; Uint32 toastUntil; char flash[48]; Uint32 flashUntil;
    bool dialog; int dialogSel;
    bool about;            /* author / donate card */
    Uint32 resolveSince;
    bool seekPending; double seekTarget; int seekTotal; Uint32 seekAt;
    bool screenOff, quick; int quickSel;
    Uint32 volUntil; int lastVol, volExpect; Uint32 volCheckUntil; bool volExternal;
    /* prefetch */
    char hoverId[16]; Uint32 hoverSince; bool prefetched; int hoverJob, warmJob; Uint32 warmAfter;
    /* audio route */
    int usbCard; Uint32 nextUsbPoll; bool bottomSwitched;
    /* kids mode */
    int pinFor; char pin[5]; int pinPos; char pinFirst[5]; Uint32 pinErrUntil; bool pinThenEnable;
    bool settingsUnlocked;
    int kidsLock; bool kidsBreak; Uint32 breakUntil; Uint32 kidsTickAt;
    int kidsPage, kidsSel, kidsListSel; float kidsScroll; bool kidsConfirm; Uint32 kidsConfirmUntil;
    bool wordEntry;
    VideoList kidsPick;
} A;
static void pin_open(int purpose);
enum { PIN_NONE, PIN_NEW1, PIN_NEW2, PIN_SETTINGS, PIN_QUIT, PIN_EXTEND, PIN_HOURS };
static void fmt_minutes(int secs, char *out, size_t n);
/* the parent has opened Settings with the PIN: no lock screen over it */
static bool parent_mode(void) { return (A.screen == SC_SETTINGS || A.screen == SC_KIDS) && A.settingsUnlocked; }

static void toast(const char *fmt, ...) {
    va_list ap; va_start(ap, fmt); vsnprintf(A.toast, sizeof(A.toast), T(fmt), ap); va_end(ap);
    A.toastUntil = SDL_GetTicks() + 2600;
}
static void flash(const char *s) { snprintf(A.flash, sizeof(A.flash), "%s", s); A.flashUntil = SDL_GetTicks() + 800; }

static void fmt_time(double sec, char *out, size_t n) {
    long s = sec < 0 ? 0 : (long)sec;
    if (s >= 3600) snprintf(out, n, "%ld:%02ld:%02ld", s / 3600, (s % 3600) / 60, s % 60);
    else snprintf(out, n, "%ld:%02ld", s / 60, s % 60);
}
static void fmt_views(long long v, bool live, char *out, size_t n) {
    bool en = S.lang == 1;
    const char *tail = live ? T("đang xem") : T("lượt xem");
    if (v <= 0) { snprintf(out, n, "%s", live ? T("Trực tiếp") : ""); return; }
    if (v >= 1000000000LL) snprintf(out, n, "%.1f %s %s", v / 1e9, en ? "B" : "T", tail);
    else if (v >= 1000000) snprintf(out, n, "%.1f %s %s", v / 1e6, en ? "M" : "Tr", tail);
    else if (v >= 1000) snprintf(out, n, "%lld %s %s", v / 1000, en ? "K" : "N", tail);
    else snprintf(out, n, "%lld %s", v, tail);
    if (!en) for (char *p = out; *p; p++) if (*p == '.') *p = ',';
}
static SDL_Color avatar_color(const char *s) {
    static const SDL_Color pal[] = {{0x7A,0x4B,0x8F,255},{0x1E,0x5F,0x74,255},{0x2F,0x3A,0x56,255},{0x5B,0x1E,0x6E,255},{0x8A,0x55,0x30,255},{0xA8,0x43,0x2A,255},{0x2E,0x5A,0xA8,255},{0x2F,0x5E,0x46,255}};
    unsigned h = 5381; for (; s && *s; s++) h = h * 33 + (unsigned char)*s;
    return pal[h % 8];
}
static void initials(const char *name, char *out) {
    int k = 0; bool start = true;
    for (const unsigned char *p = (const unsigned char *)name; *p && k < 2; ) {
        int len = *p < 0x80 ? 1 : (*p >> 5) == 6 ? 2 : (*p >> 4) == 14 ? 3 : 4;
        if (*p == ' ') { start = true; p++; continue; }
        if (start && (isalnum(*p) || *p >= 0x80)) { memcpy(out, p, len); out += len; k++; start = false; }
        else start = false;
        p += len;
    }
    *out = 0;
}
/* name of the physical button bound to a logical key, for on-screen hints */
static const char *phys_name(int id) {
    static const char *names[] = {"B", "A", "Y", "X", "L1", "R1", "SELECT", "START", "MENU", "L3", "R3", "L4", "R4", "-", "+", "MENU"};
    if (id == 100) return "L2";
    if (id == 101) return "R2";
    if (id >= 0 && id < 16) return names[id];
    return "?";
}
static const char *KN(Key k) { for (int i = 0; i < KM_COUNT; i++) if (KM_KEYS[i] == k) return phys_name(S.keymap[i]); return "?"; }

/* ---------- playback control ---------- */
static bool starts_avc(const char *c) { return !strncmp(c, "avc1", 4) || !strcmp(c, "h264"); }
static int pick_video(void) {
    StreamInfo *s = &A.stream;
    if (!s->nvideo) return -1;
    const char *q = QUALITY_ID[A.quality];
    int want = !strcmp(q, "1080") ? 1080 : !strcmp(q, "480") ? 480 : !strcmp(q, "360") ? 360 : 720;
    /* 60 fps when asked (or Auto/1080); a 60 fps video has no 30 fps HD copy, so take it anyway */
    bool allow60 = !strcmp(q, "720p60") || !strcmp(q, "auto") || !strcmp(q, "1080");
#ifdef NXT_TUBE_ROM_DARKOS
    /* 640x480 screen and Cortex-A35 cores: Auto = 480p at 30 fps (same picture, far less work) */
    if (!strcmp(q, "auto")) { want = 480; allow60 = false; }
#endif
    int best = -1, best_key = -1;
    for (int i = 0; i < s->nvideo; i++) {
        StreamFormat *f = &s->video[i];
        if (f->height > want) continue;
        bool hi = f->fps > 30;
        int key = f->height * 4 + (allow60 ? (hi ? 2 : 1) : (hi ? 1 : 2)) + (starts_avc(f->codec) ? 0 : -3);
        if (key > best_key) { best_key = key; best = i; }
    }
    if (best < 0) best = s->nvideo - 1;
    return best;
}
static bool quality_available(int qi) {
    const char *q = QUALITY_ID[qi];
    if (!strcmp(q, "auto") || !strcmp(q, "audio")) return true;
    if (A.localPlayback) return false;
    for (int i = 0; i < A.stream.nvideo; i++) {
        StreamFormat *f = &A.stream.video[i];
        if (!strcmp(q, "1080") && f->height == 1080) return true;
        if (!strcmp(q, "720p60") && f->height == 720 && f->fps > 30) return true;
        if (!strcmp(q, "720") && f->height == 720 && f->fps <= 30) return true;
        if (!strcmp(q, "480") && f->height == 480) return true;
        if (!strcmp(q, "360") && f->height == 360) return true;
    }
    return false;
}
static void save_history(void) {
    if (!A.hasCur) return;
    PlayerStatus st; Player_status(&st);
    if (st.state == PS_IDLE || st.state == PS_ERROR || st.live) return;
    Store_addHistory(&A.cur, st.position, st.duration > 0 ? st.duration : A.cur.duration);
}
static void subs_restart(void);
static void start_player(double start, bool keep_position) {
    PlayerSource src; memset(&src, 0, sizeof(src));
    bool audio_only = !strcmp(QUALITY_ID[A.quality], "audio") || A.hidden || A.screenOff;
    if (A.localPlayback) {
        src.local_video = Store_downloadPath(A.cur.id, false);
        static char la[1200]; snprintf(la, sizeof(la), "%s", Store_downloadPath(A.cur.id, true)); src.local_audio = la;
        src.audio_only = audio_only;
    } else {
        StreamInfo *s = &A.stream;
        src.headers = s->headers; src.user_agent = s->user_agent;
        src.live = s->info.live;
        int vi = pick_video();
        if (s->info.live && s->hls) { src.muxed_url = s->hls; src.video_height = 720; }
        else if (vi >= 0 && s->audio.url) {
            StreamFormat *f = &s->video[vi];
            src.video_url = f->url; src.video_size = f->size; src.video_height = f->height; src.video_fps = f->fps; src.video_codec = f->codec; src.video_kbps = f->tbr;
            src.audio_url = s->audio.url; src.audio_size = s->audio.size;
            src.audio_only = audio_only;
        } else if (s->muxed.url) { src.muxed_url = s->muxed.url; src.video_height = s->muxed.height; }
        else if (s->audio.url) { src.audio_url = s->audio.url; src.audio_size = s->audio.size; src.audio_only = true; }
        else { snprintf(A.watchErr, sizeof(A.watchErr), "%s", T("Video không có luồng phát được")); return; }
    }
    src.start = start;
    src.duration_hint = A.cur.duration;
    A.endedHandled = false;
    if (keep_position) Player_switchVideo(&src); else { Player_open(&src); subs_restart(); }
    Player_setSpeed(SPEEDS[A.speedIx]);
}
static void open_video(const VideoItem *v, const VideoList *from, int index) {
    if (!Kids_allowed(v)) { toast("Chế độ trẻ em: video này không được xem"); return; }
    if (K.enabled && Kids_lockReason()) { A.kidsLock = Kids_lockReason(); return; }
    A.seekPending = false;
    save_history();
    Player_close(); Subs_close(); A.subOverride = 0;
    StreamInfo_free(&A.stream); A.streamReady = false;
    if (A.resolveJob) { Yt_forget(A.resolveJob); A.resolveJob = 0; }
    A.cur = *v; A.hasCur = true; A.watchErr[0] = 0;
    if (from && from != &A.queue) {
        VideoList_copy(&A.queue, from);
        if (A.moreJob) { Yt_forget(A.moreJob); A.moreJob = 0; }
        A.moreQuery[0] = 0; A.moreStart = 0; A.moreDone = false; A.moreEmpty = 0; A.nextFirst = 0;
    }
    A.qpos = index;
    A.watchFocus = 0; A.nextSel = 0; A.panel = PANEL_NONE; A.osd = true; A.osdUntil = SDL_GetTicks() + 3000;
    A.localPlayback = Store_hasDownload(v->id) && !v->live;
    A.lastHistory = SDL_GetTicks();
    if (A.localPlayback) { A.streamReady = true; start_player(Store_resumePosition(v->id), false); return; }
    if (Yt_cachedStream(v->id, &A.stream)) { A.streamReady = true; A.cur = A.stream.info.id[0] ? A.stream.info : *v; start_player(Store_resumePosition(v->id), false); return; }
    A.resolveJob = Yt_resolve(v->id, true); A.resolveSince = SDL_GetTicks();
}
static void play_index(int i) {
    if (i < 0 || i >= A.queue.count) return;
    VideoItem v = A.queue.items[i];
    open_video(&v, &A.queue, i);
}

/* ---------- display lifecycle ---------- */
static bool create_display(void) {
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "1");
    SDL_SetHint(SDL_HINT_FRAMEBUFFER_ACCELERATION, "1");
#if defined(NXT_TUBE_ROM_STOCK) || defined(NXT_TUBE_PW)
    SDL_SetHint(SDL_HINT_RENDER_DRIVER, "opengles2");
#else
    SDL_SetHint(SDL_HINT_RENDER_DRIVER, "opengl");
#endif
    const char *drv = getenv("SDL_VIDEODRIVER");
    Uint32 wflags = SDL_WINDOW_SHOWN;
    if (!drv || (strcmp(drv, "dummy") && strcmp(drv, "offscreen"))) wflags |= SDL_WINDOW_OPENGL;
#ifdef NXT_TUBE_DEV_TG5050
    /* Smart Pro S: a 1280x720 panel (KMS/DRM); the 4:3 layout is centred, full-screen video uses the whole panel */
    int ww = SCREEN_W, wh = SCREEN_H;
    if (wflags & SDL_WINDOW_OPENGL) {
        ww = 1280; wh = 720;
        SDL_DisplayMode dm;
        if (!SDL_GetDesktopDisplayMode(0, &dm) && dm.w > 0 && dm.h > 0) { ww = dm.w > dm.h ? dm.w : dm.h; wh = dm.w > dm.h ? dm.h : dm.w; }
        wflags |= SDL_WINDOW_FULLSCREEN_DESKTOP;
        Log_msg("display %dx%d", ww, wh);
    }
#else
    int ww = SCREEN_W, wh = SCREEN_H;
#endif
    { const char *wsz = getenv("NXT_TUBE_WINDOW"); int x, y; if (wsz && sscanf(wsz, "%dx%d", &x, &y) == 2 && x > 0 && y > 0) { ww = x; wh = y; } }   /* QA */
    A.win = SDL_CreateWindow(APP_NAME, SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED, ww, wh, wflags);
    if (!A.win) { Log_msg("window: %s", SDL_GetError()); return false; }
    A.ren = SDL_CreateRenderer(A.win, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!A.ren) A.ren = SDL_CreateRenderer(A.win, -1, 0);
    if (!A.ren) { Log_msg("renderer: %s", SDL_GetError()); return false; }
    SDL_RendererInfo info; SDL_GetRendererInfo(A.ren, &info);
    Log_msg("renderer %s, %d texture formats", info.name, info.num_texture_formats);
    SDL_RenderSetLogicalSize(A.ren, SCREEN_W, SCREEN_H);
    SDL_SetRenderDrawBlendMode(A.ren, SDL_BLENDMODE_BLEND);
    SDL_ShowCursor(SDL_DISABLE);
    return true;
}
static void open_joystick(void) {
#ifdef NXT_TUBE_PW
    /* buttons come from evdev by key code (see input_evdev.c) */
    if (!Pad_open()) Log_msg("pad: no input device yet (will rescan)");
    return;
#endif
    SDL_InitSubSystem(SDL_INIT_JOYSTICK);
    if (SDL_NumJoysticks() > 0) A.joy = SDL_JoystickOpen(0);
    Log_msg("joysticks: %d", SDL_NumJoysticks());
}
static void destroy_display(void) {
    Thumbs_releaseTextures(); D_dropTextures(); Player_releaseTexture();
    if (A.ren) SDL_DestroyRenderer(A.ren);
    if (A.win) SDL_DestroyWindow(A.win);
    A.ren = NULL; A.win = NULL;
    if (A.joy) SDL_JoystickClose(A.joy);
    A.joy = NULL;
#ifdef NXT_TUBE_PW
    Pad_close();
#endif
    SDL_QuitSubSystem(SDL_INIT_JOYSTICK);
    SDL_QuitSubSystem(SDL_INIT_VIDEO);
}
static void go_background(void) {
    Log_msg("background listening");
    A.hidden = true; A.panel = PANEL_NONE; A.dialog = false;
    destroy_display();
    if (A.hasCur && A.streamReady) start_player(0, true);   /* drop the video track, keep the sound */
    D_setRenderer(NULL); Thumbs_setRenderer(NULL);
    Session_visible(false);
    Sys_stockVisible(false);   /* firmware keys back while we are in the background */
#ifdef NXT_TUBE_PW
    Sys_pwVisible(false);
#endif
}
static bool come_back(void) {
    Log_msg("back to foreground");
    if (SDL_InitSubSystem(SDL_INIT_VIDEO)) { Log_msg("video init: %s", SDL_GetError()); return false; }
    if (!create_display()) return false;
    open_joystick();
    D_setRenderer(A.ren); Thumbs_setRenderer(A.ren);
    memset(held, 0, sizeof(held));
    A.hidden = false;
    if (A.hasCur && A.streamReady) { start_player(0, true); A.screen = SC_WATCH; }
    Session_visible(true);
    Sys_stockVisible(true);
#ifdef NXT_TUBE_PW
    Sys_pwVisible(true);
#endif
    return true;
}

/* ---------- feeds (infinite) ---------- */
static void feed_queries(int tab, Feed *f) {
    f->nq = 0;
    if (K.enabled) {
        /* kids: the approved channels' own videos, or child-friendly searches (Restricted Mode on) */
        if ((tab == 0 || tab == CHIPS - 1 || K.approved_only) && K.napproved)
            for (int i = 0; i < K.napproved && f->nq < MAXQ; i++) snprintf(f->q[f->nq++], 128, "@ch:%s", K.approved[i].id);
        if (!f->nq) snprintf(f->q[f->nq++], 128, "%s", KIDS_QUERY[tab]);
        return;
    }
    if (tab == 0 && S.home_mode == 0) {
        /* recommendations from what you searched and watched */
        const VideoList *rec = Store_recentSearches();
        for (int i = 0; i < rec->count && f->nq < 4; i++) snprintf(f->q[f->nq++], 128, "%s", rec->items[i].title);
        char ch[2][128]; int n = Store_recentChannels(ch, 2);
        for (int i = 0; i < n && f->nq < MAXQ; i++) {
            bool dup = false; for (int k = 0; k < f->nq; k++) if (!strcasecmp(f->q[k], ch[i])) dup = true;
            if (!dup) snprintf(f->q[f->nq++], 128, "%s", ch[i]);
        }
    }
    if (!f->nq) snprintf(f->q[f->nq++], 128, "%s", CHIP_QUERY[tab]);
}
static bool feed_busy(Feed *f) { for (int i = 0; i < f->njobs; i++) if (f->jobs[i]) return true; return false; }
static void request_page(int tab, bool priority) {
    Feed *f = &A.feed[tab];
    if (feed_busy(f) || f->exhausted) return;
    if (f->page == 0) feed_queries(tab, f);
    int per = f->nq > 1 ? 8 : PAGE_ONE;
    f->njobs = 0; f->added = 0; f->err[0] = 0;
    for (int i = 0; i < f->nq; i++)
        f->jobs[f->njobs++] = !strncmp(f->q[i], "@ch:", 4) ? Yt_channelPage(f->q[i] + 4, f->page * per, per)
                                                          : Yt_searchPage(f->q[i], f->page * per, per, chip_newest(tab), priority);
    f->page++;
}
static void load_feed(int tab, bool force) {
    Feed *f = &A.feed[tab];
    if (feed_busy(f)) return;
    char key[16]; snprintf(key, sizeof(key), "%stab%d%s", K.enabled ? "k" : "", tab, tab == 0 && S.home_mode == 0 ? "r" : "");
    long long age = 0;
    if (!f->list.count) { Store_loadFeed(key, &f->list, &age); Kids_filter(&f->list); }
    if (force || !f->list.count || age > 1800 || !f->tried) {
        f->tried = true; f->page = 0; f->exhausted = false; f->replacing = f->list.count > 0;
        request_page(tab, tab == A.tab);
    }
}
static void reset_feeds(void) {
    for (int t = 0; t < CHIPS; t++) { Feed *f = &A.feed[t]; for (int i = 0; i < f->njobs; i++) if (f->jobs[i]) Yt_forget(f->jobs[i]); VideoList_clear(&f->list); memset(f, 0, sizeof(*f)); }
    A.sel = 0; A.scroll = 0;
}
static void more_results(void);
static void poll_jobs(void) {
    for (int t = 0; t < CHIPS; t++) {
        Feed *f = &A.feed[t];
        bool any = false;
        for (int i = 0; i < f->njobs; i++) {
            if (!f->jobs[i]) continue;
            JobState s = Yt_state(f->jobs[i]);
            if (s == JOB_DONE) {
                VideoList l = {0};
                if (Yt_takeList(f->jobs[i], &l) && l.count) {
                    if (f->replacing) { VideoList_clear(&f->list); f->replacing = false; if (t == A.tab) { A.sel = 0; A.scroll = 0; } }
                    int before = f->list.count;
                    for (int k = 0; k < l.count; k++) if (Kids_allowed(&l.items[k])) VideoList_add(&f->list, &l.items[k]);
                    f->added += K.enabled ? l.count : f->list.count - before;   /* filtered pages still mean "more exist" */
                }
                VideoList_clear(&l); f->jobs[i] = 0; any = true;
            } else if (s == JOB_FAILED) {
                snprintf(f->err, sizeof(f->err), "%s", Yt_error(f->jobs[i])); Yt_forget(f->jobs[i]); f->jobs[i] = 0; any = true;
            }
        }
        if (any && !feed_busy(f)) {
            if (f->page == 1 && f->list.count && !f->replacing) { char key[16]; snprintf(key, sizeof(key), "%stab%d%s", K.enabled ? "k" : "", t, t == 0 && S.home_mode == 0 ? "r" : ""); Store_saveFeed(key, &f->list); }
            if (f->page > 1 && f->added == 0 && !f->err[0]) f->exhausted = true;
            f->replacing = false;
        }
    }
    if (A.searchJob) {
        JobState s = Yt_state(A.searchJob);
        if (s == JOB_DONE) {
            VideoList l = {0}; Yt_takeList(A.searchJob, &l);
            if (!A.foundAppend) { VideoList_clear(&A.found); A.resultSel = 0; }
            int before = A.found.count;
            for (int k = 0; k < l.count; k++) if (Kids_allowed(&l.items[k])) VideoList_add(&A.found, &l.items[k]);
            if (A.foundAppend && A.found.count == before) A.foundEnd = true;
            VideoList_clear(&l); A.searchJob = 0;
            if (!A.found.count) snprintf(A.searchErr, sizeof(A.searchErr), "%s", T("Không tìm thấy video nào"));
        } else if (s == JOB_FAILED) { snprintf(A.searchErr, sizeof(A.searchErr), "%s", Yt_error(A.searchJob)); Yt_forget(A.searchJob); A.searchJob = 0; if (A.foundAppend) A.foundEnd = true; }
    }
    if (A.resolveJob) {
        JobState s = Yt_state(A.resolveJob);
        if (s == JOB_DONE) {
            StreamInfo si; memset(&si, 0, sizeof(si));
            if (Yt_takeStream(A.resolveJob, &si)) {
                StreamInfo_free(&A.stream); A.stream = si; A.streamReady = true;
                VideoItem keep = A.cur; A.cur = si.info;
                if (!A.cur.title[0]) A.cur = keep;
                if (!A.cur.channel_id[0]) snprintf(A.cur.channel_id, sizeof(A.cur.channel_id), "%s", keep.channel_id);
                if (!quality_available(A.quality) && strcmp(QUALITY_ID[A.quality], "audio")) A.quality = 0;
                start_player(Store_resumePosition(A.cur.id), false);
            }
            A.resolveJob = 0;
        } else if (s == JOB_FAILED) { snprintf(A.watchErr, sizeof(A.watchErr), "%s", Yt_error(A.resolveJob)); Yt_forget(A.resolveJob); A.resolveJob = 0; }
    }
}

/* ---------- navigation ---------- */
static const VideoList *library_list(void) {
    return A.libTab == 0 ? Store_history() : A.libTab == 1 ? Store_later() : A.libTab == 2 ? Store_channels() : Store_downloads();
}
static int current_section(void) {
    switch (A.screen) {
        case SC_HOME: return 0;
        case SC_SEARCH: return A.channelView ? 2 : 1;
        case SC_LIBRARY: return A.libTab == 2 ? 2 : 3;
        case SC_SETTINGS: case SC_KIDS: return 4;
        default: return 0;
    }
}
static void open_search(void) { if (A.screen != SC_KIDS) A.wordEntry = false; A.screen = SC_SEARCH; A.results = false; A.channelView = false; A.searchFocus = 0; if (A.keySel < 0 || A.keySel >= 40) A.keySel = 10; }
static void icon_state(void);
static void go_section(int s) {
    A.panel = PANEL_NONE;
    switch ((s + 5) % 5) {
        case 0: A.screen = SC_HOME; A.focus = 0; break;
        case 1: open_search(); break;
        case 2: A.screen = SC_LIBRARY; A.libTab = 2; A.libSel = 0; A.lscroll = 0; break;
        case 3: A.screen = SC_LIBRARY; if (A.libTab == 2) A.libTab = 0; A.libSel = 0; A.lscroll = 0; break;
        case 4:
            if (K.enabled && !A.settingsUnlocked) { pin_open(PIN_SETTINGS); break; }
            A.screen = SC_SETTINGS; A.keyPage = false; A.capture = -1; icon_state(); break;
    }
    if ((s + 5) % 5 != 4) A.settingsUnlocked = false;
}
static void grid_move(Key k, int *sel, int count, int *focus_out_up, int *focus_out_down) {
    if (count <= 0) { if (k == K_UP && focus_out_up) *focus_out_up = 1; if (k == K_DOWN && focus_out_down) *focus_out_down = 1; return; }
    if (k == K_LEFT && *sel % 3 > 0) (*sel)--;
    else if (k == K_RIGHT && *sel % 3 < 2 && *sel + 1 < count) (*sel)++;
    else if (k == K_UP) { if (*sel >= 3) *sel -= 3; else if (focus_out_up) *focus_out_up = 1; }
    else if (k == K_DOWN) { if (*sel + 3 < count) *sel += 3; else if (*sel / 3 < (count - 1) / 3) *sel = count - 1; else if (focus_out_down) *focus_out_down = 1; }
}
static bool is_playing(void) {
    PlayerStatus st; Player_status(&st);
    return A.hasCur && (st.state == PS_PLAYING || st.state == PS_BUFFERING || st.state == PS_LOADING || (A.resolveJob && st.state == PS_IDLE));
}
static void request_quit(void) {
    if (K.enabled) { pin_open(PIN_QUIT); return; }   /* kids mode: leaving the app needs the parent */
    A.quit = true;
}
static void leave_home(void) {
    if (is_playing()) { A.dialog = true; A.dialogSel = 0; }
    else request_quit();
}
void Telex_convert(const char *raw, char *out, size_t cap);
static void update_query(void) {
    if (A.telexOff || A.rawLiteral) snprintf(A.query, sizeof(A.query), "%s", A.raw);
    else Telex_convert(A.raw, A.query, sizeof(A.query));
}
static void run_search(const char *q) {
    char buf[300]; snprintf(buf, sizeof(buf), "%s", q);
    char *s = buf; while (*s == ' ') s++;
    if (!*s) { toast("Gõ từ khóa trước khi tìm"); return; }
    if (Kids_queryBlocked(s)) { toast("Chế độ trẻ em: từ khóa này bị chặn"); return; }
    Store_addSearch(s); Store_save();
    VideoList_clear(&A.found); A.searchErr[0] = 0; A.foundAppend = false; A.foundEnd = false;
    if (A.searchJob) Yt_forget(A.searchJob);
    A.searchJob = Yt_searchPage(s, 0, PAGE_ONE, false, true);
    snprintf(A.lastQuery, sizeof(A.lastQuery), "%s", s);
    snprintf(A.resultTitle, sizeof(A.resultTitle), T("Kết quả cho “%s”"), s); A.channelView = false;
    A.screen = SC_SEARCH; A.results = true; A.resultSel = 0; A.rscroll = 0;
}
static void more_results(void) {
    if (A.searchJob || A.foundEnd || !A.found.count) return;
    A.foundAppend = true;
    if (A.channelView) A.searchJob = Yt_channelPage(A.channelId, A.found.count, 20);
    else A.searchJob = Yt_searchPage(A.lastQuery, A.found.count, 20, false, true);
}
static void open_channel(const VideoItem *c) {
    if (K.enabled && K.approved_only && !Kids_approved(c->channel_id)) { toast("Chế độ trẻ em: kênh này chưa được duyệt"); return; }
    VideoList_clear(&A.found); A.searchErr[0] = 0; A.foundAppend = false; A.foundEnd = false;
    snprintf(A.channelId, sizeof(A.channelId), "%s", c->channel_id);
    if (A.searchJob) Yt_forget(A.searchJob);
    A.searchJob = Yt_channelPage(c->channel_id, 0, PAGE_ONE);
    snprintf(A.resultTitle, sizeof(A.resultTitle), T("Kênh %s"), c->title); A.channelView = true;
    A.screen = SC_SEARCH; A.results = true; A.resultSel = 0; A.rscroll = 0;
}
static void open_panel(Panel p) {
    A.panel = p; A.osd = true;
    if (p == PANEL_QUALITY) A.panelSel = A.quality;
    else if (p == PANEL_SPEED) A.panelSel = A.speedIx;
    else A.panelSel = 0;
}
static int options_count(void) { return 8; }
/* row 2 is "Subtitles"; the rest keep their old numbers */
static int opt_id(int i) { return i < 2 ? i : i == 2 ? 7 : i - 1; }
static const char *SUB_OVERRIDE[] = {"Theo cài đặt", "Bật · dịch", "Ngôn ngữ gốc", "Tắt"};
static void subs_restart(void) {
    if (!A.hasCur || !A.streamReady) return;
    Subs_open(A.localPlayback ? NULL : &A.stream, A.subOverride);
}
static void run_option(int i) {
    switch (opt_id(i)) {
        case 7: A.subOverride = (A.subOverride + 1) % 4; subs_restart(); toast("Phụ đề: %s", T(SUB_OVERRIDE[A.subOverride])); break;
        case 0: open_panel(PANEL_QUALITY); break;
        case 1: open_panel(PANEL_SPEED); break;
        case 2: A.panel = PANEL_NONE; go_background(); break;
        case 3: toast(Store_toggleLater(&A.cur) ? "Đã thêm vào Xem sau" : "Đã bỏ khỏi Xem sau"); Store_save(); A.panel = PANEL_NONE; break;
        case 4: if (!A.cur.channel_id[0]) { toast("Không rõ kênh của video này"); break; }
            toast(Store_toggleChannel(&A.cur) ? "Đã theo dõi %s" : "Đã bỏ theo dõi %s", A.cur.channel); Store_save(); A.panel = PANEL_NONE; break;
        case 5: if (Store_hasDownload(A.cur.id)) toast("Video đã có trong máy");
            else if (A.cur.live) toast("Không tải được video trực tiếp");
            else if (Download_start(&A.cur, 720)) toast("Bắt đầu tải về máy (720p)");
            else toast("Đang tải video khác, chờ xong nhé");
            A.panel = PANEL_NONE; break;
        case 6: A.stats = !A.stats; A.panel = PANEL_NONE; break;
    }
}
static void panel_key(Key k) {
    int count = A.panel == PANEL_QUALITY ? NQ : A.panel == PANEL_SPEED ? NS : options_count();
    if (k == K_UP) { do A.panelSel = (A.panelSel + count - 1) % count; while (A.panel == PANEL_QUALITY && !quality_available(A.panelSel)); }
    else if (k == K_DOWN) { do A.panelSel = (A.panelSel + 1) % count; while (A.panel == PANEL_QUALITY && !quality_available(A.panelSel)); }
    else if (k == K_A) {
        if (A.panel == PANEL_QUALITY) {
            if (A.panelSel != A.quality) { A.quality = A.panelSel; Store_setQuality(QUALITY_ID[A.quality]); Store_save(); if (A.streamReady) start_player(0, true); }
            A.panel = PANEL_NONE;
        } else if (A.panel == PANEL_SPEED) { A.speedIx = A.panelSel; Player_setSpeed(SPEEDS[A.speedIx]); A.panel = PANEL_NONE; }
        else run_option(A.panelSel);
        A.osdUntil = SDL_GetTicks() + 3000;
    } else if (k == K_B || (k == K_X && A.panel == PANEL_QUALITY) || (k == K_START && A.panel == PANEL_SPEED) || (k == K_SELECT && A.panel == PANEL_OPTIONS)) A.panel = PANEL_NONE;
}
static void playback_key(Key k) {
    PlayerStatus st; Player_status(&st);
    Uint32 now = SDL_GetTicks();
    if (k == K_A) { if (A.watchErr[0] && !A.resolveJob) { VideoItem v = A.cur; open_video(&v, NULL, A.qpos); } else Player_togglePause(); A.osd = true; A.osdUntil = now + 3000; }
    else if (k == K_LEFT || k == K_RIGHT || k == K_L2 || k == K_R2) {
        int d = k == K_LEFT ? -10 : k == K_RIGHT ? 10 : k == K_L2 ? -60 : 60;
        if (st.live) flash(T("Trực tiếp · không tua được"));
        else {
            /* presses in quick succession add up and seek once */
            double base = A.seekPending ? A.seekTarget : st.position;
            A.seekTotal = A.seekPending ? A.seekTotal + d : d;
            A.seekTarget = base + d;
            if (A.seekTarget < 0) A.seekTarget = 0;
            if (st.duration > 0 && A.seekTarget > st.duration - 1) A.seekTarget = st.duration - 1;
            A.seekPending = true; A.seekAt = now + 260;
            char f[64], tm[24]; fmt_time(A.seekTarget, tm, sizeof(tm));
            snprintf(f, sizeof(f), A.seekTotal > 0 ? T("+%d giây") : T("−%d giây"), abs(A.seekTotal));
            size_t fl = strlen(f); snprintf(f + fl, sizeof(f) - fl, " · %s", tm);
            flash(f);
        }
        A.osd = true; A.osdUntil = now + 3000;
    }
    else if (k == K_L1 || k == K_R1) {
        int next = A.qpos + (k == K_R1 ? 1 : -1);
        if (next >= 0 && next < A.queue.count) play_index(next); else toast(k == K_R1 ? "Hết danh sách" : "Đây là video đầu tiên");
    }
    else if (k == K_X) open_panel(PANEL_QUALITY);
    else if (k == K_START) open_panel(PANEL_SPEED);
    else if (k == K_SELECT) open_panel(PANEL_OPTIONS);
}

/* ---------- settings ---------- */
static int run_icon_script(const char *arg, char *out, size_t n) {
    char cmd[1600]; snprintf(cmd, sizeof(cmd), "sh \"%s/integration/install-tools-icon.sh\" %s 2>&1", paths.app, arg);
    FILE *p = popen(cmd, "r");
    if (!p) { snprintf(out, n, "%s", T("Không chạy được trình cài icon")); return -1; }
    char line[256]; out[0] = 0;
    while (fgets(line, sizeof(line), p)) { size_t l = strlen(line); while (l && (line[l - 1] == '\n' || line[l - 1] == '\r')) line[--l] = 0; if (*line) snprintf(out, n, "%s", line); }
    int st = pclose(p);
    return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
}
static void icon_state(void) {
#ifndef NXT_TUBE_ROM_NXREDUX
    A.iconState = 2; return;
#endif
    char out[64]; run_icon_script("--status", out, sizeof(out));
    A.iconState = !strcmp(out, "installed") ? 1 : !strcmp(out, "ready") ? 0 : 2;
}
static void clear_cache(void) {
    char dir[1200]; snprintf(dir, sizeof(dir), "%s/thumbs", paths.cache);
    DIR *d = opendir(dir); struct dirent *e; int n = 0;
    while (d && (e = readdir(d))) { if (e->d_name[0] == '.') continue; char p[1500]; snprintf(p, sizeof(p), "%s/%s", dir, e->d_name); if (!unlink(p)) n++; }
    if (d) closedir(d);
    d = opendir(paths.cache);
    while (d && (e = readdir(d))) { if (!strncmp(e->d_name, "feed-", 5)) { char p[1500]; snprintf(p, sizeof(p), "%s/%s", paths.cache, e->d_name); unlink(p); } }
    if (d) closedir(d);
    Log_msg("cache cleared: %d thumbnails", n);
}
static void apply_audio(void) {
    Player_setAudioMode(S.audio_out); Sys_applyGain();
    A.usbCard = AudioRoute_usbCard();
}
static void setting_change(int row, int dir) {
    switch (row) {
        case SET_LANG: S.lang = 1 - S.lang; break;
        case SET_QUALITY: A.quality = (A.quality + NQ + dir) % NQ; Store_setQuality(QUALITY_ID[A.quality]); Store_save(); break;
        case SET_AUTOPLAY: S.autoplay = !S.autoplay; break;
        case SET_PREFETCH: S.prefetch = !S.prefetch; break;
        case SET_SUBS: S.sub_mode = (S.sub_mode + 3 + dir) % 3; Settings_save(); if (A.hasCur) subs_restart(); return;
        case SET_SUBLANG: {
            int i = -1;
            for (int k = 0; k < SUB_NLANGS; k++) if (!strcmp(SUB_LANGS[k].code, S.sub_lang)) i = k;
            i = (i + 1 + SUB_NLANGS + 1 + dir) % (SUB_NLANGS + 1) - 1;   /* -1 = follow the app's language */
            snprintf(S.sub_lang, sizeof(S.sub_lang), "%s", i < 0 ? "" : SUB_LANGS[i].code);
            Settings_save(); if (A.hasCur) subs_restart(); return;
        }
        case SET_SUBSIZE: S.sub_size = (S.sub_size + 3 + dir) % 3; break;
        case SET_OTA_AUTO: S.ota_auto = !S.ota_auto; break;
        case SET_TELEX: S.telex = !S.telex; A.telexOff = !S.telex; break;
        case SET_AVSYNC: S.av_offset_ms += dir * 20; if (S.av_offset_ms > 400) S.av_offset_ms = 400; if (S.av_offset_ms < -400) S.av_offset_ms = -400; Player_setAvOffset(S.av_offset_ms); break;
        case SET_AUDIO: S.audio_out = (S.audio_out + 3 + dir) % 3; apply_audio();
            toast(Player_usbOutput() ? "Âm thanh ra DAC USB" : S.audio_out == 2 ? "Không thấy DAC USB · đang dùng loa máy" : "Âm thanh ra loa / tai nghe"); break;
        case SET_BOTTOM: {
            S.bottom_dac = !S.bottom_dac;
            int r = AudioRoute_setBottom(S.bottom_dac);
            A.bottomSwitched = S.bottom_dac;
            if (S.bottom_dac) toast(r == 1 ? "Cổng sạc đã chuyển sang chế độ DAC" : "Đã gửi lệnh chuyển cổng sạc sang DAC");
            else toast(r == 2 ? "Cổng sạc đã trở lại chế độ sạc" : "Đã gửi lệnh trả cổng sạc về chế độ sạc");
            A.nextUsbPoll = SDL_GetTicks() + 1500;
            break;
        }
        case SET_HOME: S.home_mode = 1 - S.home_mode; { Feed *f = &A.feed[0]; for (int i = 0; i < f->njobs; i++) if (f->jobs[i]) Yt_forget(f->jobs[i]); VideoList_clear(&f->list); memset(f, 0, sizeof(*f)); } break;
        default: return;
    }
    Settings_save();
}
static void setting_action(int row) {
    Uint32 now = SDL_GetTicks();
    switch (row) {
        case SET_KEYS: A.keyPage = true; A.keySelRow = 0; A.capture = -1; return;
        case SET_SUBLANG: {
            int pk = Subs_packFor(S.sub_lang);
            if (pk && !Subs_packReady(pk)) {
                int pr = Subs_packProgress(pk);
                if (pr >= 0 && pr <= 100) toast("Đang tải gói chữ %s… %d%%", T(Subs_packName(pk)), pr);
                else if (Subs_packDownload(pk)) toast("Bắt đầu tải gói chữ %s (%d MB)", T(Subs_packName(pk)), Subs_packMB(pk));
                return;
            }
            setting_change(row, 1); return;
        }
        case SET_ICON: {
            char out[160];
            if (A.iconState == 1) {
                if (A.confirmRow == SET_ICON && now < A.confirmUntil) {
                    int r = run_icon_script("--restore", out, sizeof(out));
                    snprintf(A.iconStatus, sizeof(A.iconStatus), "%s", *out ? out : r == 0 ? T("Đã xong") : T("Không thực hiện được"));
                    toast("%s", A.iconStatus); A.confirmRow = -1;
                } else { A.confirmRow = SET_ICON; A.confirmUntil = now + 3000; toast("Bấm A lần nữa để gỡ icon (trả lại launcher cũ)"); }
            } else {
                int r = run_icon_script("", out, sizeof(out));
                snprintf(A.iconStatus, sizeof(A.iconStatus), "%s", *out ? out : r == 0 ? T("Đã xong") : T("Không thực hiện được"));
                toast("%s", A.iconStatus);
            }
            icon_state();
            return;
        }
        case SET_CACHE: clear_cache(); reset_feeds(); load_feed(A.tab, true); toast("Đã xóa bộ nhớ đệm ảnh và danh sách"); return;
        case SET_HISTORY:
            if (A.confirmRow == SET_HISTORY && now < A.confirmUntil) { Store_clearHistory(); Store_save(); toast("Đã xóa lịch sử xem"); A.confirmRow = -1; }
            else { A.confirmRow = SET_HISTORY; A.confirmUntil = now + 3000; toast("Bấm A lần nữa để xóa lịch sử xem"); }
            return;
        case SET_SEARCHES:
            if (A.confirmRow == SET_SEARCHES && now < A.confirmUntil) { Store_clearSearches(); Store_save(); toast("Đã xóa lịch sử tìm kiếm"); A.confirmRow = -1; }
            else { A.confirmRow = SET_SEARCHES; A.confirmUntil = now + 3000; toast("Bấm A lần nữa để xóa lịch sử tìm kiếm"); }
            return;
        case SET_VERSION: case SET_ABOUT: A.about = true; return;
        case SET_UPDATE: {
            int st = Ota_state();
            if (st == OTA_AVAILABLE) {
                if (A.confirmRow == SET_UPDATE && now < A.confirmUntil) { A.confirmRow = -1; if (Ota_install()) toast("Bắt đầu tải bản cập nhật"); }
                else { A.confirmRow = SET_UPDATE; A.confirmUntil = now + 4000; toast("Bấm A lần nữa để tải và cài bản %s", Ota_latest()); }
            } else if (st == OTA_READY) { A.restart = true; A.quit = true; }
            else if (st != OTA_CHECKING && st != OTA_DOWNLOADING) Ota_check(true);
            return;
        }
        case SET_KIDS: A.screen = SC_KIDS; A.kidsPage = 0; A.kidsSel = 0; A.kidsScroll = 0; return;
        default: setting_change(row, 1); return;
    }
}
static void capture_assign(int phys) {
    int row = A.capture; A.capture = -1;
    if (row < 0 || row >= KM_COUNT) return;
    for (int i = 0; i < KM_COUNT; i++) if (i != row && S.keymap[i] == phys) S.keymap[i] = S.keymap[row];   /* swap */
    S.keymap[row] = phys;
    Settings_save();
    toast(T("Đã gán %s cho chức năng %s"), phys_name(phys), KM_LABEL[row]);
}
static void settings_key(Key k) {
    if (A.keyPage) {
        if (A.capture >= 0) return;
        if (k == K_UP && A.keySelRow > 0) A.keySelRow--;
        else if (k == K_DOWN && A.keySelRow < KM_COUNT - 1) A.keySelRow++;
        else if (k == K_A) { A.capture = A.keySelRow; A.captureUntil = SDL_GetTicks() + 6000; }
        else if (k == K_X) { Settings_defaultKeys(); Settings_save(); toast("Đã khôi phục phím mặc định"); }
        else if (k == K_Y) { int a = S.keymap[0]; S.keymap[0] = S.keymap[1]; S.keymap[1] = a; Settings_save(); toast("Đã đổi chỗ A và B"); }
        else if (k == K_B) A.keyPage = false;
        return;
    }
    if (k == K_UP && A.setSel > 0) A.setSel--;
    else if (k == K_DOWN && A.setSel < SET_ROWS - 1) A.setSel++;
    else if (k == K_LEFT) setting_change(A.setSel, -1);
    else if (k == K_RIGHT) setting_change(A.setSel, 1);
    else if (k == K_A) setting_action(A.setSel);
    else if (k == K_B) { A.screen = SC_HOME; A.focus = 0; A.settingsUnlocked = false; }
}

/* ---------- volume, screen off, quick panel ---------- */
static void volume_step(int dir) {
#ifdef NXT_TUBE_PW
    Sys_volumeKey();
#endif
    int v = Sys_volume();
    bool dac = Player_usbOutput();
    if (!(A.volExternal && !dac)) {
        A.volExpect = v + dir < 0 ? 0 : v + dir > 20 ? 20 : v + dir;
        Sys_setVolume(A.volExpect);
        A.volCheckUntil = dac ? 0 : SDL_GetTicks() + 350;
    }
    A.lastVol = Sys_volume();
    A.volUntil = SDL_GetTicks() + 1600;
}
static void volume_tick(Uint32 now) {
    int v = Sys_volume();
    if (A.volCheckUntil && now < A.volCheckUntil && v != A.volExpect) {
        /* the firmware also reacted to the key: let it handle the speaker from now on */
        A.volExternal = true; A.volCheckUntil = 0;
        Sys_setVolume(A.volExpect); v = A.volExpect;
        Log_msg("volume keys are handled by the firmware; app only shows the level");
    }
    if (A.volCheckUntil && now >= A.volCheckUntil) A.volCheckUntil = 0;
    if (v != A.lastVol) { A.lastVol = v; A.volUntil = now + 1600; }
    Sys_tick(now);
}
static void screen_off(void) {
    A.screenOff = true; A.quick = false; A.panel = PANEL_NONE;
    if (A.hasCur && A.streamReady) start_player(0, true);   /* keep only the sound: saves battery */
    Sys_setBacklight(false);
    Sys_cpuLevel(0);
    Log_msg("screen off, audio continues");
}
static void screen_wake(void) {
    if (!A.screenOff) return;
    A.screenOff = false;
    Sys_cpuLevel(1);
    Sys_setBacklight(true);
    if (A.hasCur && A.streamReady) start_player(0, true);
    A.osd = true; A.osdUntil = SDL_GetTicks() + 3000;
    Log_msg("screen on");
}
#define QUICK_N 4
static void pin_key(Key k);
static void kids_key(Key k);
static void kids_overlay_key(Key k);
static void kids_tick(void);
static void quick_key(Key k) {
    if (k == K_UP) A.quickSel = (A.quickSel + QUICK_N - 1) % QUICK_N;
    else if (k == K_DOWN) A.quickSel = (A.quickSel + 1) % QUICK_N;
#ifdef NXT_TUBE_PW
    else if ((k == K_LEFT || k == K_RIGHT) && !Player_usbOutput()) {
        int v = Sys_volume() + (k == K_RIGHT ? 1 : -1); Sys_setVolume(v < 0 ? 0 : v > 20 ? 20 : v);
        A.lastVol = Sys_volume(); A.volUntil = SDL_GetTicks() + 1600;
    }
#endif
    else if (k == K_LEFT || k == K_RIGHT) volume_step(k == K_RIGHT ? 1 : -1);
    else if (k == K_B || k == K_HOME) A.quick = false;
    else if (k == K_A) {
        A.quick = false;
        if (A.quickSel == 1) screen_off();
        else if (A.quickSel == 2) { if (A.kidsLock) toast("Chế độ trẻ em: đã hết giờ xem"); else if (A.hasCur) go_background(); else toast("Chưa có video đang phát"); }
        else if (A.quickSel == 3) request_quit();
    }
}
/* The firmware's on-screen panel (trimui_osdd: Stock "Task Manager", NX Redux OSD).
 * Its daemon is meant to grab the pad, but on the Brick Pro the grab misses, so while
 * it is up we ignore every input; otherwise the app would move along with the panel. */
static bool osd_present(void) {
    static Uint32 next; static bool has;
    Uint32 now = SDL_GetTicks();
    if ((Sint32)(now - next) >= 0) { next = now + 2000; has = !access("/tmp/trimui_osd", F_OK); }
    return has;
}
static bool osd_shown(void) { return osd_present() && !access("/tmp/trimui_osd/osdd_show_up", F_OK); }
static void osd_request(void) {
    int fd = open("/tmp/show_osdd", O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd >= 0) close(fd);
    A.osdGateUntil = SDL_GetTicks() + 1500;      /* until the daemon raises its flag */
}
/* true while the firmware panel owns the buttons */
static bool osd_gate(void) {
    Uint32 now = SDL_GetTicks();
    if (A.osdAsk && (Sint32)(now - A.osdAskAt) >= 0) A.osdAsk = false;   /* keymon did not open it after all */
    if (osd_shown()) { A.osdGateUntil = now + 250; return true; }
    return A.osdAsk || (Sint32)(now - A.osdGateUntil) < 0;
}
static void handle_key(Key k) {
    if (k == K_VOLUP || k == K_VOLDN) { volume_step(k == K_VOLUP ? 1 : -1); return; }
    if (k == K_POWER) {
        /* the power button reaches us twice (keyboard event and input device): one press = one toggle */
        static Uint32 last_power;
        Uint32 t = SDL_GetTicks();
        if (t - last_power < 700) return;
        last_power = t;
        if (A.screenOff) screen_wake(); else screen_off();
        return;
    }
    if (A.screenOff) { screen_wake(); return; }        /* any other key turns the screen back on */
    if (A.pinFor) { pin_key(k); return; }

    if (k == K_HOME) {
        /* one press can arrive twice (pad button + keyboard key): count it once */
        static Uint32 last_home;
        Uint32 t = SDL_GetTicks();
        if (t - last_home < 450) return;
        last_home = t;
        /* firmware with its own panel (Stock Task Manager, NX Redux OSD): let it show that.
         * NX Redux's keymon opens it by itself; on Stock we paused keymon, so we ask for it */
        if (osd_present()) {
            if (osd_shown()) return;
            if (Sys_keymonActive()) { A.osdAsk = true; A.osdAskAt = t + 1500; }   /* keymon opens it: just wait for it */
            else osd_request();
            return;
        }
        A.quick = !A.quick; A.quickSel = 0; return;
    }
    if (A.about) { if (k == K_A || k == K_B || k == K_START || k == K_MENU) A.about = false; return; }
    if (A.quick) { quick_key(k); return; }
#ifdef NXT_TUBE_DEV_TG5050
    if (k == K_MENU) { A.quick = true; A.quickSel = 0; return; }   /* Smart Pro S: MENU opens the quick panel (Home is the firmware's) */
#else
    if (k == K_MENU) { request_quit(); return; }      /* logo button: quit at once */
#endif
    if (K.enabled && (A.kidsLock || A.kidsBreak) && !parent_mode()) { kids_overlay_key(k); return; }
    if (A.dialog) {
        if (k == K_UP || k == K_DOWN) A.dialogSel = 1 - A.dialogSel;
        else if (k == K_START) A.dialog = false;
        else if (k == K_A && A.dialogSel == 0) { A.dialog = false; go_background(); }
        else if (k == K_A || k == K_B) { A.dialog = false; request_quit(); }
        return;
    }
    if (A.panel && (A.screen == SC_WATCH || A.screen == SC_FULL)) { panel_key(k); return; }
    /* L2/R2 move between the main sections everywhere except while watching */
    if ((k == K_L2 || k == K_R2) && A.screen != SC_WATCH && A.screen != SC_FULL && !(A.screen == SC_SETTINGS && A.keyPage)) {
        go_section(current_section() + (k == K_R2 ? 1 : -1));
        return;
    }
    switch (A.screen) {
    case SC_HOME: {
        VideoList *l = &A.feed[A.tab].list;
        if (k == K_Y) { open_search(); return; }
        if (k == K_B) { if (A.focus != 0) { A.focus = 0; return; } leave_home(); return; }
        if (k == K_SELECT) { if (A.hasCur) A.screen = SC_WATCH; return; }
        if (k == K_L1 || k == K_R1) { A.tab = (A.tab + (k == K_R1 ? 1 : CHIPS - 1)) % CHIPS; A.sel = 0; A.scroll = 0; A.focus = 0; load_feed(A.tab, false); return; }
        if (k == K_START) { load_feed(A.tab, true); toast("Đang làm mới %s", T(chip_labels()[A.tab])); return; }
        if (A.focus == 1) {
            if (k == K_LEFT && A.tab > 0) { A.tab--; A.sel = 0; A.scroll = 0; load_feed(A.tab, false); }
            else if (k == K_RIGHT && A.tab < CHIPS - 1) { A.tab++; A.sel = 0; A.scroll = 0; load_feed(A.tab, false); }
            else if (k == K_DOWN || k == K_A) A.focus = 0;
            return;
        }
        if (A.focus == 2) {
            if (k == K_LEFT && A.navSel > 0) A.navSel--;
            else if (k == K_RIGHT && A.navSel < 4) A.navSel++;
            else if (k == K_UP) A.focus = 0;
            else if (k == K_A) { if (A.navSel == 0) A.focus = 0; else go_section(A.navSel); }
            return;
        }
        int up = 0, down = 0;
        if (k == K_A) { if (l->count) open_video(&l->items[A.sel], l, A.sel), A.screen = SC_WATCH; else load_feed(A.tab, true); return; }
        if (k == K_X && l->count) { toast(Store_toggleLater(&l->items[A.sel]) ? "Đã thêm vào Xem sau" : "Đã bỏ khỏi Xem sau"); Store_save(); return; }
        grid_move(k, &A.sel, l->count, &up, &down);
        if (up) A.focus = 1;
        if (down) { if (feed_busy(&A.feed[A.tab]) || !A.feed[A.tab].exhausted) request_page(A.tab, true); else { A.focus = 2; A.navSel = 0; } }
        return;
    }
    case SC_LIBRARY: {
        const VideoList *l = library_list();
        if (k == K_B) { A.screen = SC_HOME; A.focus = 0; return; }
        if (k == K_Y) { open_search(); return; }
        if (k == K_SELECT) { if (A.hasCur) A.screen = SC_WATCH; return; }
        if (k == K_L1 || k == K_R1) { A.libTab = (A.libTab + (k == K_R1 ? 1 : 3)) % 4; A.libSel = 0; A.lscroll = 0; return; }
        if (k == K_A && l->count) {
            if (A.libTab == 2) { VideoItem c = l->items[A.libSel]; open_channel(&c); }
            else { VideoList copy = {0}; VideoList_copy(&copy, l); open_video(&copy.items[A.libSel], &copy, A.libSel); VideoList_clear(&copy); A.screen = SC_WATCH; }
            return;
        }
        if (k == K_X && l->count && A.libTab == 1) { Store_toggleLater(&l->items[A.libSel]); Store_save(); if (A.libSel >= Store_later()->count && A.libSel) A.libSel--; toast("Đã bỏ khỏi Xem sau"); return; }
        grid_move(k, &A.libSel, l->count, NULL, NULL);
        return;
    }
    case SC_SETTINGS: settings_key(k); return;
    case SC_KIDS: kids_key(k); return;
    case SC_SEARCH: {
        if (A.wordEntry) {
            /* adding a blocked word for kids mode: the keyboard types it, START/Tìm saves it */
            bool go = k == K_START || (k == K_A && !strcmp(KEYS[A.keySel], "go"));
            if (go) {
                update_query();
                if (Kids_addWord(A.query)) toast("Đã thêm từ khóa cấm: %s", A.query); else toast("Từ khóa trống hoặc đã có");
                A.wordEntry = false; A.raw[0] = 0; A.screen = SC_KIDS; A.kidsPage = 3; A.kidsListSel = K.nwords ? K.nwords - 1 : 0; return;
            }
            if (k == K_B && !A.raw[0]) { A.wordEntry = false; A.screen = SC_KIDS; A.kidsPage = 3; return; }
            if (k == K_L1 || k == K_R1 || k == K_Y || (k == K_RIGHT && A.keySel % 10 == 9)) { if (k == K_Y && strlen(A.raw) + 1 < sizeof(A.raw)) strcat(A.raw, " "); return; }
        }
        if (A.results) {
            if (k == K_UP && A.resultSel > 0) A.resultSel--;
            else if (k == K_DOWN && A.resultSel + 1 < A.found.count) A.resultSel++;
            else if (k == K_DOWN) more_results();
            else if (k == K_A && A.found.count) { open_video(&A.found.items[A.resultSel], &A.found, A.resultSel); A.screen = SC_WATCH; }
            else if (k == K_X && A.found.count) { toast(Store_toggleLater(&A.found.items[A.resultSel]) ? "Đã thêm vào Xem sau" : "Đã bỏ khỏi Xem sau"); Store_save(); }
            else if (k == K_B) { if (A.channelView) { A.screen = SC_LIBRARY; A.libTab = 2; A.channelView = false; } else A.results = false; }
            else if (k == K_Y) { A.screen = SC_HOME; A.focus = 0; }
            if (A.resultSel + 4 >= A.found.count) more_results();
            return;
        }
        const VideoList *rec = Store_recentSearches();
        if (A.searchFocus == 1) {   /* recent searches */
            int n = rec->count < 8 ? rec->count : 8;
            if (!n) { A.searchFocus = 0; return; }
            if (k == K_UP && A.recentSel > 0) A.recentSel--;
            else if (k == K_DOWN && A.recentSel + 1 < n) A.recentSel++;
            else if (k == K_LEFT || k == K_B) A.searchFocus = 0;
            else if (k == K_A || k == K_START) {
                char q[256]; snprintf(q, sizeof(q), "%s", rec->items[A.recentSel].title);
                snprintf(A.raw, sizeof(A.raw), "%s", q); A.rawLiteral = true; update_query();
                run_search(q);
            } else if (k == K_X) { Store_removeSearch(A.recentSel); Store_save(); if (A.recentSel && A.recentSel >= Store_recentSearches()->count) A.recentSel--; if (!Store_recentSearches()->count) A.searchFocus = 0; }
            return;
        }
        int col = A.keySel % 10, row = A.keySel / 10;
        size_t qlen = strlen(A.raw);
        if (k == K_LEFT) A.keySel = row * 10 + (col + 9) % 10;
        else if (k == K_RIGHT) { if (col == 9 && rec->count) { A.searchFocus = 1; if (A.recentSel >= rec->count) A.recentSel = 0; } else A.keySel = row * 10 + (col + 1) % 10; }
        else if (k == K_UP) A.keySel = ((row + 3) % 4) * 10 + col;
        else if (k == K_DOWN) A.keySel = ((row + 1) % 4) * 10 + col;
        else if (k == K_B && qlen) { A.raw[qlen - 1] = 0; if (!A.raw[0]) A.rawLiteral = false; }
        else if (k == K_B) { A.screen = SC_HOME; A.focus = 0; }
        else if (k == K_Y) { if (qlen + 1 < sizeof(A.raw)) strcat(A.raw, " "); }
        else if (k == K_SELECT) { A.telexOff = !A.telexOff; toast(A.telexOff ? "Tắt gõ Telex · gõ chữ không dấu" : "Bật gõ Telex · vd: vieetj = việt"); }
        else if (k == K_X) { A.raw[0] = 0; A.rawLiteral = false; }
        else if (k == K_L1 || k == K_R1) { if (rec->count) { A.searchFocus = 1; A.recentSel = 0; } }
        else if (k == K_A || k == K_START) {
            const char *key = KEYS[A.keySel];
            if (k == K_A && !strcmp(key, "del")) { if (qlen) A.raw[qlen - 1] = 0; }
            else if (k == K_A && !strcmp(key, "space")) { if (qlen + 1 < sizeof(A.raw)) strcat(A.raw, " "); }
            else if (k == K_START || !strcmp(key, "go")) { update_query(); run_search(A.query); }
            else if (qlen + strlen(key) < sizeof(A.raw)) strcat(A.raw, key);
        }
        return;
    }
    case SC_WATCH: {
        if (k == K_B) { A.screen = SC_HOME; A.focus = 0; return; }
        if (k == K_Y) { A.screen = SC_FULL; A.osd = true; A.osdUntil = SDL_GetTicks() + 3000; return; }
        int nnext = A.queue.count - A.qpos - 1; if (nnext < 0) nnext = 0;
        if (A.watchFocus == 1) {
            /* the suggestion row only moves the selection: never seeks */
            if (k == K_LEFT) { if (A.nextSel > 0) A.nextSel--; }
            else if (k == K_RIGHT) { if (A.nextSel + 1 < nnext) A.nextSel++; else if (A.moreJob) flash(T("Đang tải thêm gợi ý…")); }
            else if (k == K_L2) { A.nextSel = A.nextSel >= 4 ? A.nextSel - 4 : 0; }
            else if (k == K_R2) { A.nextSel = A.nextSel + 4 < nnext ? A.nextSel + 4 : (nnext ? nnext - 1 : 0); }
            else if (k == K_UP) A.watchFocus = 0;
            else if (k == K_A && nnext > 0) play_index(A.qpos + 1 + A.nextSel);
            else if (k != K_DOWN) playback_key(k);
            return;
        }
        if (k == K_DOWN) { if (nnext > 0) { A.watchFocus = 1; A.nextSel = 0; } return; }
        playback_key(k);
        return;
    }
    case SC_FULL: {
        if (k == K_B || k == K_Y) { A.screen = SC_WATCH; return; }
        if (k == K_UP || k == K_DOWN) { A.osd = true; A.osdUntil = SDL_GetTicks() + 3000; return; }
        playback_key(k);
        return;
    }
    }
}

/* ---------- drawing ---------- */
static void draw_logo(int x, int y) {
    D_roundRing(x, y + 6, 36, 28, 6, 3, C_TOMATO);
    D_tri(x + 13, y + 12, x + 13, y + 26, x + 25, y + 19, C_TOMATO);
    D_line(x + 11, y, x + 18.5f, y + 6.5f, 2.6f, C_TOMATO); D_line(x + 26, y, x + 18.5f, y + 6.5f, 2.6f, C_TOMATO);
    int w = D_text(F_PIXEL, 46, "NXT", x + 48, y - 8, C_INK, 0, AL_LEFT);
    int w2 = D_text(F_PIXEL, 46, "TUBE", x + 48 + w, y - 8, C_TOMATO, 0, AL_LEFT);
    int bx = x + 48 + w + w2 + 10;
    D_round(bx, y + 10, 64, 24, 4, C_MUSTARD);
    D_text(F_PIXEL, 22, "RETRO", bx + 32, y + 9, C_DARK, 0, AL_CENTER);
}
static void draw_stripes(int y) { D_rect(0, y, SCREEN_W, 2, C_TOMATO); D_rect(0, y + 2, SCREEN_W, 2, C_MUSTARD); D_rect(0, y + 4, SCREEN_W, 2, C_INK); }

static void draw_card(int x, int y, int w, const VideoItem *v, bool selected, double progress, const char *meta_override, bool channel_card) {
    if (selected) { x -= 3; y -= 3; }
    int th = (w - 16) * 9 / 16, h = 8 + th + 10 + 55 + 4 + 23 + 8;
    if (selected) { D_shadow(x, y, w, h, 16, 9); D_round(x, y, w, h, 16, C_RAISED); D_focus(x + 4, y + 4, w - 8, h - 8, 12); }
    SDL_Color bg = selected ? C_RAISED : C_BG;
    SDL_Rect tr = {x + 8, y + 8, w - 16, th};
    if (channel_card) {
        D_round(tr.x, tr.y, tr.w, tr.h, 12, C_BAR); D_roundRing(tr.x, tr.y, tr.w, tr.h, 12, 2, C_BLACK);
        int r = 52, cx = tr.x + tr.w / 2, cy = tr.y + tr.h / 2;
        D_circle(cx + 6, cy + 6, r, C_BLACK); D_circle(cx, cy, r, avatar_color(v->title)); D_ring(cx, cy, r, 3, C_BLACK);
        char ini[16]; initials(v->title, ini); D_text(F_XBOLD, 34, ini, cx, cy - D_fontHeight(F_XBOLD, 34) / 2, C_INK, 0, AL_CENTER);
    } else {
        D_thumb(v->id, tr, bg);
        char badge[24];
        if (v->live) {
            int bw = D_textW(F_XBOLD, 15, "TRỰC TIẾP") + 16;
            D_round(tr.x + tr.w - bw - 8, tr.y + tr.h - 32, bw, 24, 4, C_TOMATO);
            D_text(F_XBOLD, 15, "TRỰC TIẾP", tr.x + tr.w - bw / 2 - 8, tr.y + tr.h - 30, C_DARK, 0, AL_CENTER);
        } else if (v->duration > 0) {
            fmt_time(v->duration, badge, sizeof(badge));
            int bw = D_textW(F_PIXEL, 24, badge) + 14;
            D_round(tr.x + tr.w - bw - 8, tr.y + tr.h - 36, bw, 28, 4, RGBA(8, 7, 12, 220));
            D_text(F_PIXEL, 24, badge, tr.x + tr.w - bw / 2 - 8, tr.y + tr.h - 36, C_INK, 0, AL_CENTER);
        }
        if (Store_hasDownload(v->id)) {
            int bw = D_textW(F_XBOLD, 14, "ĐÃ TẢI") + 14;
            D_round(tr.x + 8, tr.y + 8, bw, 22, 4, C_MUSTARD);
            D_text(F_XBOLD, 14, "ĐÃ TẢI", tr.x + 8 + bw / 2, tr.y + 10, C_DARK, 0, AL_CENTER);
        }
        if (progress > 0) { D_rect(tr.x, tr.y + tr.h - 6, tr.w, 6, RGBA(244, 239, 230, 80)); D_rect(tr.x, tr.y + tr.h - 6, (int)(tr.w * progress), 6, C_TOMATO); }
    }
    int ty = y + 8 + th + 10;
    int tx = x + 10, tw = w - 20;
    if (!channel_card) {
        SDL_Color ac = avatar_color(v->channel);
        D_circle(x + 30, ty + 20, 20, C_BLACK); D_circle(x + 30, ty + 20, 18, ac);
        char ini[16]; initials(v->channel, ini); D_text(F_XBOLD, 15, ini, x + 30, ty + 20 - D_fontHeight(F_XBOLD, 15) / 2, C_INK, 0, AL_CENTER);
        tx = x + 62; tw = w - 72;
    }
    D_textBox(F_SEMI, 22, v->title, tx, ty - 2, tw, 2, 28, C_INK);
    char meta[200], views[64];
    if (meta_override) snprintf(meta, sizeof(meta), "%s", meta_override);
    else { fmt_views(v->views, v->live, views, sizeof(views)); snprintf(meta, sizeof(meta), "%s%s%s", v->channel, *views ? " · " : "", views); }
    D_text(F_REG, 18, meta, tx, ty + 56, C_MUTED, tw, AL_LEFT);
}

static void draw_nav(int active, int focus_sel) {
    D_rect(0, 692, SCREEN_W, 76, C_BAR); D_rect(0, 692, SCREEN_W, 2, C_RAISED);
    const char *icons[5] = {"house", "search", "list-video", "library", "settings"}, *labels[5] = {"Trang chủ", "Tìm kiếm", "Kênh", "Thư viện", "Cài đặt"};
    int cw = SCREEN_W / 5;
    for (int i = 0; i < 5; i++) {
        int cx = i * cw + cw / 2;
        if (i == focus_sel) { D_round(i * cw + 8, 700, cw - 16, 62, 12, C_RAISED); D_roundRing(i * cw + 8, 700, cw - 16, 62, 12, 4, C_MUSTARD); }
        SDL_Color c = i == active ? C_INK : C_DIM;
        if (i == active) D_round(cx - 22, 696, 44, 4, 2, C_TOMATO);
        D_icon(icons[i], cx - 14, 706, 28, c);
        D_text(i == active ? F_BOLD : F_MED, 16, labels[i], cx, 738, c, cw - 8, AL_CENTER);
    }
    /* section switch hint at the bar edges */
    D_text(F_PIXEL, 20, KN(K_L2), 6, 708, C_DIM, 0, AL_LEFT);
    D_text(F_PIXEL, 20, KN(K_R2), SCREEN_W - 6, 708, C_DIM, 0, AL_RIGHT);
}
/* status for drawing: a seek still being collected shows its target already */
static void ui_status(PlayerStatus *st) {
    Player_status(st);
    if (A.seekPending) { st->position = A.seekTarget; st->buffered = 0; }
}
static void draw_mini(void) {
    D_rect(0, 608, SCREEN_W, 84, C_SURFACE); D_rect(0, 608, SCREEN_W, 2, C_BLACK);
    SDL_Texture *t; int w, h;
    Player_present(A.ren, &t, &w, &h);
    PlayerStatus st; ui_status(&st);
    SDL_Rect v = {12, 614, 128, 72};
    if (st.audio_only || !t) D_thumb(A.cur.id, v, C_SURFACE); else D_video(t, w, h, v);
    D_roundRing(v.x, v.y, v.w, v.h, 6, 2, C_BLACK);
    if (st.state == PS_PAUSED) D_tri(v.x + 52, v.y + 22, v.x + 52, v.y + 50, v.x + 76, v.y + 36, C_INK);
    if (st.duration > 0) { D_rect(v.x, v.y + v.h - 4, v.w, 4, RGBA(244, 239, 230, 70)); D_rect(v.x, v.y + v.h - 4, (int)(v.w * fmin(1, st.position / st.duration)), 4, C_TOMATO); }
    D_text(F_SEMI, 20, A.cur.title, 156, 622, C_INK, 600, AL_LEFT);
    char tm[64], a[24], b[24]; fmt_time(st.position, a, sizeof(a)); fmt_time(st.duration, b, sizeof(b));
    if (st.live) snprintf(tm, sizeof(tm), "%s", T("TRỰC TIẾP")); else if (A.resolveJob) snprintf(tm, sizeof(tm), "%s", T("Đang lấy luồng…")); else snprintf(tm, sizeof(tm), "%s / %s", a, b);
    D_text(A.resolveJob ? F_MED : F_PIXEL, A.resolveJob ? 18 : 24, tm, 156, 652, C_MUTED, 0, AL_LEFT);
    D_hint(810, 637, KN(K_SELECT), "Mở lại");
}

static int grid_vh(void) { return A.hasCur ? 462 : 546; }
static void card_meta(const VideoList *l, int i, int libtab, char *meta, size_t n, const char **mo, double *progress) {
    *mo = NULL; *progress = libtab == 0 ? Store_historyProgress(i) : 0;
    if (libtab == 0) { snprintf(meta, n, "%s · %s", l->items[i].channel, *progress >= 0.97 ? T("đã xem hết") : T("xem tiếp")); *mo = meta; }
    if (libtab == 2) { snprintf(meta, n, "%s", T("A xem video mới của kênh")); *mo = meta; }
    if (libtab == 3) { snprintf(meta, n, "%s · %s", l->items[i].channel, T("xem không cần mạng")); *mo = meta; }
}
static void draw_grid(const VideoList *l, int sel, bool focus, float *scroll, int libtab, bool loading_more, bool at_end) {
    int vh = grid_vh();
    int row = sel / 3; float target = row * 298 + 306 - vh; if (target < 0) target = 0;
    *scroll += (target - *scroll) * 0.35f; if (fabsf(target - *scroll) < 1) *scroll = target;
    SDL_RenderSetClipRect(A.ren, &(SDL_Rect){0, 146, SCREEN_W, vh});
    int first = (int)(*scroll / 298) * 3;
    for (int i = first; i < l->count && i < first + 9; i++) {
        int x = 24 + (i % 3) * 332, y = 146 + 18 + (i / 3) * 298 - (int)*scroll;
        if (y > 146 + vh) break;
        char meta[200]; const char *mo; double progress;
        card_meta(l, i, libtab, meta, sizeof(meta), &mo, &progress);
        if (!(focus && i == sel)) draw_card(x, y, 312, &l->items[i], false, progress, mo, libtab == 2);
    }
    if (focus && sel < l->count) {
        int i = sel, x = 24 + (i % 3) * 332, y = 146 + 18 + (i / 3) * 298 - (int)*scroll;
        char meta[200]; const char *mo; double progress;
        card_meta(l, i, libtab, meta, sizeof(meta), &mo, &progress);
        draw_card(x, y, 312, &l->items[i], true, progress, mo, libtab == 2);
    }
    /* footer below the last row: loading more / end of list */
    int fy = 146 + 18 + ((l->count + 2) / 3) * 298 - (int)*scroll;
    if (fy < 146 + vh) {
        if (loading_more) { D_spinner(SCREEN_W / 2 - 120, fy + 24, 12, C_MUSTARD); D_text(F_MED, 18, "Đang tải thêm video…", SCREEN_W / 2 - 96, fy + 12, C_MUTED, 0, AL_LEFT); }
        else if (at_end) D_text(F_MED, 18, "Hết danh sách", SCREEN_W / 2, fy + 12, C_DIM, 0, AL_CENTER);
    }
    SDL_RenderSetClipRect(A.ren, NULL);
}
static void draw_chips(const char *const *labels, int n, int active, bool focused, int y) {
    int x = 24;
    for (int i = 0; i < n; i++) {
        const char *lab = T(labels[i]);
        int w = D_textW(F_SEMI, 21, lab) + 36;
        bool on = i == active;
        if (x + w > SCREEN_W) break;
        D_round(x, y, w, 44, 10, on ? C_INK : C_SURFACE);
        if (!on) D_roundRing(x, y, w, 44, 10, 2, C_LINE);
        if (on && focused) D_roundRing(x - 7, y - 7, w + 14, 58, 14, 4, C_MUSTARD);
        D_text(F_SEMI, 21, lab, x + w / 2, y + 22 - D_fontHeight(F_SEMI, 21) / 2, on ? C_DARK : C_INK, 0, AL_CENTER);
        x += w + 12;
    }
}
static void draw_empty(const char *title, const char *note, bool loading) {
    int cy = 146 + grid_vh() / 2;
    if (loading) D_spinner(SCREEN_W / 2, cy - 40, 30, C_MUSTARD);
    D_text(F_BOLD, 24, title, SCREEN_W / 2, cy + 6, C_INK, 900, AL_CENTER);
    if (note && *note) D_text(F_REG, 19, note, SCREEN_W / 2, cy + 44, C_MUTED, 900, AL_CENTER);
}
static int header_hints(int right, const char *const *keys, const char *const *texts, int n) {
    int x = right;
    for (int i = 0; i < n; i++) {
        int kw = D_textW(F_PIXEL, 24, keys[i]) + (strlen(keys[i]) > 1 ? 14 : 0); if (kw < 28) kw = 28;
        x -= kw + 8 + D_textW(F_MED, 18, texts[i]) + 18;
        D_hint(x, 24, keys[i], texts[i]);
    }
    return x;
}
static void draw_home(void) {
    D_rect(0, 0, SCREEN_W, SCREEN_H, C_BG);
    draw_logo(24, 20);
    const char *k[2] = {KN(K_B), KN(K_Y)}, *t[2] = {"Thoát", "Tìm kiếm"};
    header_hints(1000 - 46 - 4, k, t, 2);
    D_circle(977, 38, 23, C_BLACK); D_circle(977, 38, 21, C_MUSTARD); D_text(F_XBOLD, 18, "NX", 977, 38 - D_fontHeight(F_XBOLD, 18) / 2, C_DARK, 0, AL_CENTER);
    draw_stripes(76);
    if (K.enabled) {
        char b[96], m[32];
        if (K.daily_min) { fmt_minutes(Kids_leftToday(), m, sizeof(m)); snprintf(b, sizeof(b), T("Trẻ em · còn %s"), m); }
        else snprintf(b, sizeof(b), "%s", T("Chế độ trẻ em"));
        int bw = D_textW(F_BOLD, 17, b) + 24;
        D_round(392, 24, bw, 32, 16, C_MUSTARD); D_text(F_BOLD, 17, b, 392 + bw / 2, 30, C_DARK, 0, AL_CENTER);
    }
    draw_chips(chip_labels(), CHIPS, A.tab, A.focus == 1, 92);
    Feed *f = &A.feed[A.tab];
    VideoList *l = &f->list;
    bool busy = feed_busy(f);
    if (A.sel >= l->count && l->count) A.sel = l->count - 1;
    if (l->count) draw_grid(l, A.sel, A.focus == 0, &A.scroll, -1, busy && !f->replacing, f->exhausted);
    else if (busy) draw_empty(T("Đang tải danh sách video…"), T("Lần đầu có thể mất vài giây"), true);
    else if (f->err[0]) draw_empty(T("Chưa tải được danh sách"), f->err, false);
    else draw_empty(T("Chưa có video"), T("Bấm A để tải lại"), false);
    if (busy && f->replacing && l->count) D_spinner(1000 - 18, 146 + 18, 10, C_MUSTARD);
    if (A.hasCur) draw_mini();
    draw_nav(0, A.focus == 2 ? A.navSel : -1);
}
static void draw_library(void) {
    D_rect(0, 0, SCREEN_W, SCREEN_H, C_BG);
    D_text(F_XBOLD, 34, A.libTab == 2 ? "Kênh" : "Thư viện", 24, 16, C_INK, 0, AL_LEFT);
    if (Download_busy()) D_text(F_MED, 17, Download_status(), 230, 28, C_MUSTARD, 420, AL_LEFT);
    char l1r1[16]; snprintf(l1r1, sizeof(l1r1), "%s %s", KN(K_L1), KN(K_R1));
    const char *k[2] = {KN(K_B), l1r1}, *t[2] = {"Về", "Đổi mục"};
    header_hints(1000, k, t, 2);
    draw_stripes(76);
    const char *tabs[4] = {"Lịch sử", "Xem sau", "Kênh theo dõi", "Đã tải"};
    char lbl[4][64]; const char *lp[4];
    int counts[4] = {Store_history()->count, Store_later()->count, Store_channels()->count, Store_downloads()->count};
    for (int i = 0; i < 4; i++) { snprintf(lbl[i], sizeof(lbl[i]), "%s %d", T(tabs[i]), counts[i]); lp[i] = lbl[i]; }
    draw_chips(lp, 4, A.libTab, false, 92);
    const VideoList *l = library_list();
    if (A.libSel >= l->count) A.libSel = l->count ? l->count - 1 : 0;
    if (l->count) draw_grid(l, A.libSel, true, &A.lscroll, A.libTab, false, false);
    else {
        const char *notes[4] = {"Video bạn xem sẽ hiện ở đây, kèm chỗ xem dở", "Bấm X trên một video để lưu xem sau", "Khi xem video: SELECT › Theo dõi kênh", "Khi xem video: SELECT › Tải về máy"};
        draw_empty(T("Chưa có gì ở mục này"), T(notes[A.libTab]), false);
    }
    if (A.hasCur) draw_mini();
    draw_nav(A.libTab == 2 ? 2 : 3, -1);
}
static void draw_search(void) {
    D_rect(0, 0, SCREEN_W, SCREEN_H, C_BG);
    if (A.results && A.channelView) {
        D_text(F_XBOLD, 30, A.resultTitle, 24, 26, C_INK, 960, AL_LEFT);
    } else {
        D_shadow(24, 24, 970, 72, 14, 6); D_round(24, 24, 970, 72, 14, C_SURFACE); D_roundRing(24, 24, 970, 72, 14, 3, C_BLACK);
        D_icon("search", 44, 45, 30, C_MUSTARD);
        int tw = 0;
        update_query();
        if (A.query[0]) tw = D_text(F_SEMI, 30, A.query, 88, 60 - D_fontHeight(F_SEMI, 30) / 2, C_INK, 556, AL_LEFT);
        else D_text(F_REG, 28, A.wordEntry ? "Nhập từ khóa cấm…" : "Tìm video, kênh…", 96, 60 - D_fontHeight(F_REG, 28) / 2, RGBA(0x8F, 0x87, 0x99, 255), 0, AL_LEFT);
        if (!A.results && A.searchFocus == 0 && (SDL_GetTicks() / 500) % 2 == 0) D_rect(tw ? 88 + tw + 2 : 88, 43, 3, 34, C_MUSTARD);
        D_hint(860, 46, KN(K_X), "Xóa hết");
        D_hint(660, 46, KN(K_SELECT), A.telexOff ? "Telex: tắt" : "Telex: bật");
    }
    if (!A.results) {
        for (int i = 0; i < 40; i++) {
            int x = 24 + (i % 10) * 61, y = 124 + (i / 10) * 72;
            bool on = i == A.keySel && A.searchFocus == 0, go = !strcmp(KEYS[i], "go");
            const char *label = !strcmp(KEYS[i], "del") ? T("Xóa") : !strcmp(KEYS[i], "space") ? T("Cách") : go ? (A.wordEntry ? T("Thêm") : T("Tìm")) : KEYS[i];
            if (on) { D_round(x + 3, y + 3, 53, 64, 10, C_BLACK); x -= 2; y -= 2; }
            D_round(x, y, 53, 64, 10, on ? C_MUSTARD : go ? C_TOMATO : C_SURFACE);
            D_roundRing(x, y, 53, 64, 10, 2, on || go ? C_BLACK : C_LINE);
            bool small = strlen(label) > 3;
            FontFace f = !strcmp(KEYS[i], "del") ? F_REG : F_BOLD;
            int size = small ? 16 : 28;
            D_text(f, size, label, x + 26, y + 32 - D_fontHeight(f, size) / 2, on || go ? C_DARK : C_INK, 50, AL_CENTER);
        }
        D_roundRing(24, 428, 600, 92, 14, 2, C_LINE);
        D_textBox(F_REG, 17, A.telexOff ? "Telex đang tắt: gõ không dấu vẫn tìm được. SELECT để bật lại."
                                          : "Telex: aa â · aw ă · ee ê · oo ô · ow ơ · uw ư · dd đ · s f r x j = sắc huyền hỏi ngã nặng · gõ 2 lần để giữ chữ (ff = f).", 40, 440, 570, 3, 25, C_MUTED);
        const VideoList *rec = Store_recentSearches();
        D_text(F_XBOLD, 17, "TÌM GẦN ĐÂY", 648, 124, A.searchFocus == 1 ? C_MUSTARD : C_DIM, 0, AL_LEFT);
        if (rec->count) D_text(F_MED, 15, A.searchFocus == 1 ? "A tìm lại · X xóa · Trái: bàn phím" : "Phải để chọn", 1000, 126, C_DIM, 0, AL_RIGHT);
        for (int i = 0; i < rec->count && i < 8; i++) {
            bool on = A.searchFocus == 1 && i == A.recentSel;
            int y = 152 + i * 58;
            if (on) { D_round(651, y + 3, 352, 52, 10, C_BLACK); }
            D_round(648, y, 352, 52, 10, on ? C_MUSTARD : C_BAR);
            D_icon("clock", 662, y + 14, 22, on ? C_DARK : C_DIM);
            D_text(F_REG, 20, rec->items[i].title, 696, y + 12, on ? C_DARK : C_INK, 290, AL_LEFT);
        }
        if (!rec->count) D_text(F_REG, 19, "Chưa có lượt tìm nào", 648, 160, C_DIM, 0, AL_LEFT);
    } else {
        if (!A.channelView) D_text(F_MED, 18, A.resultTitle, 24, 106, C_MUTED, 960, AL_LEFT);
        if (A.searchJob && !A.found.count) { D_spinner(SCREEN_W / 2, 380, 30, C_MUSTARD); D_text(F_BOLD, 22, "Đang tìm trên YouTube…", SCREEN_W / 2, 430, C_INK, 0, AL_CENTER); }
        else if (!A.found.count) D_text(F_BOLD, 22, A.searchErr[0] ? A.searchErr : T("Không có kết quả"), SCREEN_W / 2, 400, C_INK, 900, AL_CENTER);
        float target = A.resultSel > 3 ? (A.resultSel - 3) * 114 : 0;
        A.rscroll += (target - A.rscroll) * 0.35f;
        SDL_RenderSetClipRect(A.ren, &(SDL_Rect){0, 136, SCREEN_W, 578});
        for (int i = 0; i < A.found.count; i++) {
            int y = 140 + i * 114 - (int)A.rscroll;
            if (y < 30 || y > 716) continue;
            VideoItem *v = &A.found.items[i];
            bool on = i == A.resultSel;
            if (on) { D_shadow(24, y, 976, 106, 14, 8); D_round(24, y, 976, 106, 14, C_RAISED); D_roundRing(18, y - 6, 988, 118, 18, 4, C_MUSTARD); }
            D_thumb(v->id, (SDL_Rect){30, y + 4, 176, 99}, on ? C_RAISED : C_BG);
            if (v->live) { D_round(140, y + 76, 60, 22, 4, C_TOMATO); D_text(F_XBOLD, 13, "LIVE", 170, y + 79, C_DARK, 0, AL_CENTER); }
            else if (v->duration > 0) { char d[24]; fmt_time(v->duration, d, sizeof(d)); int bw = D_textW(F_PIXEL, 20, d) + 10; D_round(200 - bw, y + 76, bw, 22, 4, RGBA(8, 7, 12, 220)); D_text(F_PIXEL, 20, d, 200 - bw / 2, y + 75, C_INK, 0, AL_CENTER); }
            D_textBox(F_SEMI, 22, v->title, 226, y + 14, 760, 2, 28, C_INK);
            char views[64], meta[220]; fmt_views(v->views, v->live, views, sizeof(views));
            snprintf(meta, sizeof(meta), "%s%s%s", v->channel, *views ? " · " : "", views);
            D_text(F_REG, 17, meta, 226, y + 74, C_MUTED, 760, AL_LEFT);
        }
        int fy = 140 + A.found.count * 114 - (int)A.rscroll;
        if (A.found.count && fy < 716) {
            if (A.searchJob) { D_spinner(SCREEN_W / 2 - 120, fy + 24, 12, C_MUSTARD); D_text(F_MED, 18, "Đang tải thêm video…", SCREEN_W / 2 - 96, fy + 12, C_MUTED, 0, AL_LEFT); }
            else if (A.foundEnd) D_text(F_MED, 18, "Hết danh sách", SCREEN_W / 2, fy + 12, C_DIM, 0, AL_CENTER);
        }
        SDL_RenderSetClipRect(A.ren, NULL);
    }
    D_rect(0, 716, SCREEN_W, 52, C_BAR); D_rect(0, 716, SCREEN_W, 2, C_RAISED);
    int x = 24;
    char l2r2[16]; snprintf(l2r2, sizeof(l2r2), "%s %s", KN(K_L2), KN(K_R2));
    if (!A.results) { x += D_hint(x, 728, KN(K_A), "Gõ") + 20; x += D_hint(x, 728, KN(K_B), "Xóa chữ / về") + 20; x += D_hint(x, 728, KN(K_Y), "Dấu cách") + 20; x += D_hint(x, 728, KN(K_START), "Tìm") + 20; D_hint(x, 728, l2r2, "Đổi mục"); }
    else { x += D_hint(x, 728, "UD", "Chọn") + 20; x += D_hint(x, 728, KN(K_A), "Xem") + 20; x += D_hint(x, 728, KN(K_X), "Xem sau") + 20; x += D_hint(x, 728, KN(K_B), A.channelView ? "Về thư viện" : "Sửa từ khóa") + 20; D_hint(x, 728, l2r2, "Đổi mục"); }
}

/* ---------- settings screen ---------- */
static void setting_text(int row, char *label, size_t ln, char *value, size_t vn, char *note, size_t nn) {
    value[0] = note[0] = 0;
    static const char *onoff[2] = {"Tắt", "Bật"};
    switch (row) {
        case SET_LANG: snprintf(label, ln, "%s", T("Ngôn ngữ / Language")); snprintf(value, vn, "%s", S.lang ? "English" : "Tiếng Việt"); snprintf(note, nn, "%s", T("Đổi ngôn ngữ giao diện")); break;
        case SET_QUALITY: snprintf(label, ln, "%s", T("Chất lượng mặc định")); snprintf(value, vn, "%s", T(QUALITY_LABEL[A.quality])); snprintf(note, nn, "%s", T(QUALITY_NOTE[A.quality])); break;
        case SET_AUTOPLAY: snprintf(label, ln, "%s", T("Tự phát video tiếp theo")); snprintf(value, vn, "%s", T(onoff[S.autoplay])); snprintf(note, nn, "%s", T("Hết video sẽ phát video kế trong danh sách")); break;
        case SET_PREFETCH: snprintf(label, ln, "%s", T("Lấy luồng trước")); snprintf(value, vn, "%s", T(onoff[S.prefetch])); snprintf(note, nn, "%s", T("Chuẩn bị sẵn video đang chọn để bấm là phát ngay")); break;
        case SET_SUBS: {
            static const char *m[3] = {"Tự động", "Luôn bật", "Tắt"};
            snprintf(label, ln, "%s", T("Phụ đề"));
            snprintf(value, vn, "%s", T(m[S.sub_mode]));
            snprintf(note, nn, "%s", T(S.sub_mode == 0 ? "Video nói tiếng khác: tự hiện phụ đề YouTube dịch sang ngôn ngữ của bạn. Cần mạng." :
                                       S.sub_mode == 1 ? "Luôn hiện phụ đề, kể cả khi video cùng ngôn ngữ với bạn" : "Không hiện phụ đề (đổi riêng từng video trong Tùy chọn video)"));
            break;
        }
        case SET_SUBLANG: {
            const char *code = S.sub_lang[0] ? S.sub_lang : S.lang ? "en" : "vi";
            const char *name = code;
            for (int k = 0; k < SUB_NLANGS; k++) if (!strcmp(SUB_LANGS[k].code, code)) name = T(SUB_LANGS[k].name);
            snprintf(label, ln, "%s", T("Ngôn ngữ phụ đề"));
            if (S.sub_lang[0]) snprintf(value, vn, "%s", name); else snprintf(value, vn, T("Theo app · %s"), name);
            int pk = Subs_packFor(code), pr = Subs_packProgress(pk);
            if (!pk) snprintf(note, nn, "%s", T("Có sẵn trong app · YouTube dịch phụ đề sang ngôn ngữ này"));
            else if (Subs_packReady(pk)) snprintf(note, nn, "%s", T("Gói chữ đã cài · YouTube dịch phụ đề sang ngôn ngữ này"));
            else if (pr >= 0 && pr <= 100) snprintf(note, nn, T("Đang tải gói chữ %s… %d%%"), T(Subs_packName(pk)), pr);
            else if (pr > 100) snprintf(note, nn, T("Tải gói chữ lỗi · bấm A để thử lại (%d MB)"), Subs_packMB(pk));
            else snprintf(note, nn, T("Cần gói chữ %s · bấm A để tải (%d MB, một lần)"), T(Subs_packName(pk)), Subs_packMB(pk));
            break;
        }
        case SET_SUBSIZE: {
            static const char *z[3] = {"Nhỏ", "Vừa", "Lớn"};
            snprintf(label, ln, "%s", T("Cỡ chữ phụ đề")); snprintf(value, vn, "%s", T(z[S.sub_size]));
            snprintf(note, nn, "%s", T("Chữ trắng trên nền tối, ở cuối khung video"));
            break;
        }
        case SET_TELEX: snprintf(label, ln, "%s", T("Gõ Telex khi tìm kiếm")); snprintf(value, vn, "%s", T(onoff[S.telex])); snprintf(note, nn, "%s", T("vieetj = việt · tắt để gõ không dấu")); break;
        case SET_AVSYNC: snprintf(label, ln, "%s", T("Bù trễ tiếng – hình")); snprintf(value, vn, "%+d ms", S.av_offset_ms);
            snprintf(note, nn, T("Hình chạy trước tiếng thì tăng, sau tiếng thì giảm · trễ ước tính %.0f ms"), Player_latencyMs()); break;
        case SET_AUDIO: { static const char *m[3] = {"Tự động", "Loa / tai nghe", "DAC USB"};
            snprintf(label, ln, "%s", T("Đầu ra âm thanh")); snprintf(value, vn, "%s", T(m[S.audio_out]));
            if (A.usbCard >= 0) snprintf(note, nn, T("Đang cắm: %s · đang phát ra %s"), AudioRoute_dacName(), Player_usbOutput() ? "DAC" : T("loa"));
            else snprintf(note, nn, "%s", T("Chưa thấy DAC USB · Tự động sẽ chuyển khi bạn cắm vào"));
            break; }
        case SET_BOTTOM: { int r = AudioRoute_bottomRole();
            snprintf(label, ln, "%s", T("Cổng sạc dùng cho DAC")); snprintf(value, vn, "%s", T(onoff[S.bottom_dac]));
            snprintf(note, nn, "%s · %s", r == 1 ? T("Cổng dưới đang ở chế độ DAC") : r == 2 ? T("Cổng dưới đang ở chế độ sạc") : T("Không đọc được trạng thái cổng"), T("không sạc được khi bật")); break; }
        case SET_HOME: snprintf(label, ln, "%s", T("Trang chủ gợi ý")); snprintf(value, vn, "%s", S.home_mode ? T("Thịnh hành") : T("Theo lịch sử")); snprintf(note, nn, "%s", T("Gợi ý từ từ khóa đã tìm và kênh đã xem")); break;
        case SET_KEYS: snprintf(label, ln, "%s", T("Đổi phím")); snprintf(value, vn, "%s", "A"); snprintf(note, nn, "%s", T("Gán lại A, B, X, Y, L1, R1, L2, R2, START, SELECT")); break;
        case SET_ICON: snprintf(label, ln, "%s", T("Icon trong menu Tools")); snprintf(value, vn, "%s", A.iconState == 1 ? T("Đã cài") : A.iconState == 0 ? T("Chưa cài") : T("Không hỗ trợ")); snprintf(note, nn, "%s", A.iconStatus[0] ? A.iconStatus : T("NX Redux: thêm icon cho NXT Retro Tube vào launcher")); break;
        case SET_CACHE: snprintf(label, ln, "%s", T("Xóa bộ nhớ đệm")); snprintf(value, vn, "%s", "A"); snprintf(note, nn, "%s", T("Ảnh thu nhỏ và danh sách đã lưu")); break;
        case SET_HISTORY: snprintf(label, ln, "%s", T("Xóa lịch sử xem")); snprintf(value, vn, "%s", "A"); snprintf(note, nn, "%s", T("Bấm A hai lần để xác nhận")); break;
        case SET_SEARCHES: snprintf(label, ln, "%s", T("Xóa lịch sử tìm kiếm")); snprintf(value, vn, "%s", "A"); snprintf(note, nn, "%s", T("Bấm A hai lần để xác nhận")); break;
        case SET_OTA_AUTO: snprintf(label, ln, "%s", T("Tự kiểm tra cập nhật")); snprintf(value, vn, "%s", T(onoff[S.ota_auto])); snprintf(note, nn, "%s", T("Mỗi ngày một lần khi có Wi-Fi · chỉ báo, không tự cài")); break;
        case SET_UPDATE: {
            int st = Ota_state();
            snprintf(label, ln, "%s", T("Cập nhật (OTA)"));
            if (st == OTA_AVAILABLE) snprintf(value, vn, T("Có %s"), Ota_latest());
            else if (st == OTA_DOWNLOADING) snprintf(value, vn, "%d%%", Ota_progress());
            else if (st == OTA_READY) snprintf(value, vn, "%s", T("Khởi động lại"));
            else if (st == OTA_CHECKING) snprintf(value, vn, "%s", "…");
            else snprintf(value, vn, "%s", T("Kiểm tra"));
            const char *m = st == OTA_IDLE ? T("Bấm A để kiểm tra bản mới trên GitHub của tác giả") : Ota_message();
            const char *nt = st == OTA_AVAILABLE ? Ota_notes() : "";
            snprintf(note, nn, "%s%s%s%s", m, st == OTA_AVAILABLE ? T(" · A để cập nhật") : st == OTA_READY ? T(" · bấm A") : "", *nt ? " · " : "", nt);
            break;
        }
        case SET_VERSION: snprintf(label, ln, "%s", T("Phiên bản")); snprintf(value, vn, "%s", APP_VERSION); 
#ifdef NXT_TUBE_PW
            snprintf(note, nn, "NXT Retro Tube %s · %s · %s · %s · yt-dlp · Deno · FFmpeg 6.1.6 · %s", APP_VERSION, ROM_LABEL, Ota_device(), APP_AUTHOR, __DATE__); break;
#else
            snprintf(note, nn, "NXT Retro Tube %s · %s · %s · %s · yt-dlp · Deno · FFmpeg 6.1.6 · %s", APP_VERSION, ROM_LABEL, Ota_device(), APP_AUTHOR, __DATE__); break;
#endif
        case SET_KIDS: snprintf(label, ln, "%s", T("Chế độ trẻ em")); snprintf(value, vn, "%s", K.enabled ? T("Bật") : T("Tắt"));
            snprintf(note, nn, "%s", T("Phụ huynh: mã PIN, giới hạn thời gian, khung giờ, nhắc nghỉ mắt, kênh được duyệt, từ khóa cấm, báo cáo xem")); break;
        case SET_ABOUT: snprintf(label, ln, "%s", T("Tác giả · Ủng hộ")); snprintf(value, vn, "%s", "A");
            snprintf(note, nn, "%s · MoMo %s · %s · PayPal %s", APP_AUTHOR, DONATE_MOMO, DONATE_BANK, DONATE_PAYPAL); break;
    }
}
static void draw_settings(void) {
    D_rect(0, 0, SCREEN_W, SCREEN_H, C_BG);
    D_text(F_XBOLD, 34, A.keyPage ? "Đổi phím" : "Cài đặt", 24, 16, C_INK, 0, AL_LEFT);
    const char *k[2] = {KN(K_B), KN(K_A)}, *t[2] = {"Về", A.keyPage ? "Gán phím" : "Chọn"};
    header_hints(1000, k, t, 2);
    draw_stripes(76);
    if (A.keyPage) {
        for (int i = 0; i < KM_COUNT; i++) {
            int y = 96 + i * 58; bool on = i == A.keySelRow;
            if (on) { D_round(27, y + 3, 970, 50, 10, C_BLACK); }
            D_round(24, y, 970, 50, 10, on ? C_INK : C_SURFACE);
            SDL_Color fg = on ? C_DARK : C_INK, sub = on ? C_LINE : C_MUTED;
            D_text(F_BOLD, 22, KM_LABEL[i], 44, y + 12, fg, 0, AL_LEFT);
            D_text(F_REG, 18, KM_ROLE[i], 190, y + 14, sub, 560, AL_LEFT);
            const char *val = A.capture == i ? T("Nhấn nút mới…") : phys_name(S.keymap[i]);
            D_round(800, y + 9, 170, 32, 8, on ? C_MUSTARD : C_RAISED);
            D_text(A.capture == i ? F_BOLD : F_PIXEL, A.capture == i ? 16 : 24, val, 885, y + (A.capture == i ? 15 : 10), on ? C_DARK : C_INK, 160, AL_CENTER);
        }
        D_rect(0, 716, SCREEN_W, 52, C_BAR);
        int x = 24;
        x += D_hint(x, 728, KN(K_A), "Gán nút mới") + 20; x += D_hint(x, 728, KN(K_Y), "Đổi chỗ A / B") + 20; x += D_hint(x, 728, KN(K_X), "Mặc định") + 20; D_hint(x, 728, KN(K_B), "Xong");
        return;
    }
    int rowh = 60, top = 96, vis = 9;
    float target = A.setSel > vis - 3 ? (A.setSel - (vis - 3)) * rowh : 0;
    A.setScroll += (target - A.setScroll) * 0.35f;
    SDL_RenderSetClipRect(A.ren, &(SDL_Rect){0, top - 6, SCREEN_W, vis * rowh + 6});
    for (int i = 0; i < SET_ROWS; i++) {
        int y = top + i * rowh - (int)A.setScroll;
        if (y < top - rowh || y > top + vis * rowh) continue;
        char label[160], value[96], note[300];
        setting_text(i, label, sizeof(label), value, sizeof(value), note, sizeof(note));
        bool on = i == A.setSel;
        if (on) D_round(27, y + 3, 640, 52, 10, C_BLACK);
        D_round(24, y, 640, 52, 10, on ? C_INK : C_SURFACE);
        D_text(F_SEMI, 20, label, 42, y + 14, on ? C_DARK : C_INK, 400, AL_LEFT);
        int vw = D_textW(F_BOLD, 18, value) + 24;
        bool cyclic = i <= SET_HOME;
        D_round(648 - vw, y + 10, vw, 32, 8, on ? C_MUSTARD : C_RAISED);
        D_text(F_BOLD, 18, value, 648 - vw / 2, y + 15, on ? C_DARK : C_INK, 0, AL_CENTER);
        if (on && cyclic) { D_tri(648 - vw - 18, y + 26, 648 - vw - 10, y + 20, 648 - vw - 10, y + 32, C_DARK); }
    }
    SDL_RenderSetClipRect(A.ren, NULL);
    /* detail card */
    char label[160], value[96], note[300];
    setting_text(A.setSel, label, sizeof(label), value, sizeof(value), note, sizeof(note));
    D_shadow(688, 96, 312, 300, 14, 6); D_round(688, 96, 312, 300, 14, C_SURFACE); D_roundRing(688, 96, 312, 300, 14, 2, C_BLACK);
    D_icon("settings", 708, 116, 30, C_MUSTARD);
    D_textBox(F_BOLD, 22, label, 708, 156, 272, 2, 28, C_INK);
    D_text(F_PIXEL, 30, value, 708, 214, C_MUSTARD, 272, AL_LEFT);
    D_textBox(F_REG, 17, note, 708, 258, 272, 5, 24, C_MUTED);
    D_textBox(F_REG, 16, A.setSel <= SET_HOME ? "Trái / Phải để đổi giá trị" : "A để thực hiện", 708, 416, 290, 2, 22, C_DIM);
    draw_nav(4, -1);
}

static const char *quality_badge(void) {
    PlayerStatus st; Player_status(&st);
    static char b[48];
    if (st.state == PS_IDLE || st.state == PS_ERROR || A.resolveJob) return T(QUALITY_LABEL[A.quality]);
    if (st.audio_only) return T("Chỉ âm thanh");
    if (A.localPlayback) { snprintf(b, sizeof(b), T("Đã tải · %dp"), st.height); return b; }
    if (st.height > 0) snprintf(b, sizeof(b), "%dp%s%s", st.height, st.fps > 31 ? "60" : "", st.height >= 720 ? " HD" : "");
    else snprintf(b, sizeof(b), "%s", T(QUALITY_LABEL[A.quality]));
    return b;
}
static void draw_progress(int x, int y, int w, int h, PlayerStatus *st) {
    D_round(x, y, w, h, h / 2, RGBA(244, 239, 230, 60));
    if (st->duration > 0 && !st->live) {
        double played = fmin(1, st->position / st->duration), buf = fmin(1, (st->position + st->buffered) / st->duration);
        D_round(x, y, (int)(w * buf), h, h / 2, RGBA(244, 239, 230, 120));
        D_round(x, y, (int)(w * played), h, h / 2, C_TOMATO);
        int kx = x + (int)(w * played);
        D_circle(kx, y + h / 2, h + 3, C_BLACK); D_circle(kx, y + h / 2, h + 1, C_MUSTARD);
    } else if (st->live) D_round(x, y, w, h, h / 2, C_TOMATO);
}
static void draw_stats(int x, int y, PlayerStatus *st) {
    char lines[8][2][128];
    snprintf(lines[0][0], 128, "%s", T("Luồng")); snprintf(lines[0][1], 128, "%s", A.localPlayback ? T("Tệp đã tải") : st->live ? T("HLS trực tiếp") : T("DASH · tải từng phần 10 MB"));
    snprintf(lines[1][0], 128, "%s", T("Hình"));
    if (st->audio_only) snprintf(lines[1][1], 128, "%s", T("Tắt"));
    else snprintf(lines[1][1], 128, T("%dx%d · %.0f khung/s · %s"), st->width, st->height, st->fps, st->vcodec);
    snprintf(lines[2][0], 128, "%s", T("Tiếng")); snprintf(lines[2][1], 128, "%s · %s", st->acodec, Player_usbOutput() ? "DAC USB" : T("loa"));
    snprintf(lines[3][0], 128, "%s", T("Bộ đệm")); snprintf(lines[3][1], 128, T("%.1f giây phía trước"), st->buffered); if (!S.lang) for (char *p = lines[3][1]; *p; p++) if (*p == '.') *p = ',';
    snprintf(lines[4][0], 128, "%s", T("Giải mã")); snprintf(lines[4][1], 128, "%s", st->audio_only ? T("Chỉ giải mã tiếng") : T("Phần mềm · 4 nhân"));
    snprintf(lines[5][0], 128, "%s", T("Bỏ khung")); snprintf(lines[5][1], 128, "%u / %u", st->dropped, st->decoded);
    snprintf(lines[6][0], 128, "%s", T("Lệch A/V")); snprintf(lines[6][1], 128, T("%+d ms · đồng bộ theo tiếng"), st->av_offset_ms);
    snprintf(lines[7][0], 128, "%s", T("Trễ loa")); snprintf(lines[7][1], 128, "%.0f ms %+d ms", Player_latencyMs(), S.av_offset_ms);
    D_round(x, y, 500, 262, 12, RGBA(10, 9, 14, 225)); D_roundRing(x, y, 500, 262, 12, 2, C_LINE);
    D_text(F_XBOLD, 15, "THÔNG TIN LUỒNG", x + 18, y + 12, C_MUSTARD, 0, AL_LEFT);
    for (int i = 0; i < 8; i++) {
        D_text(F_REG, 17, lines[i][0], x + 18, y + 40 + i * 27, C_MUTED, 0, AL_LEFT);
        D_text(F_PIXEL, 24, lines[i][1], x + 136, y + 36 + i * 27, C_INK, 350, AL_LEFT);
    }
}
static void draw_player_overlays(SDL_Rect a, PlayerStatus *st, bool big) {
    int cx = a.x + a.w / 2, cy = a.y + a.h / 2;
    bool loading = A.resolveJob || st->state == PS_LOADING || st->state == PS_BUFFERING;
    if (A.watchErr[0] || st->state == PS_ERROR) {
        D_rect(a.x, a.y, a.w, a.h, RGBA(8, 7, 12, 200));
        D_text(F_BOLD, big ? 26 : 22, "Không phát được video", cx, cy - 34, C_INK, a.w - 40, AL_CENTER);
        D_text(F_REG, 18, A.watchErr[0] ? A.watchErr : st->error, cx, cy + 2, C_MUTED, a.w - 40, AL_CENTER);
        D_text(F_MED, 18, "Bấm A để thử lại", cx, cy + 34, C_MUSTARD, 0, AL_CENTER);
        return;
    }
    if (loading) {
        D_rect(a.x, a.y, a.w, a.h, RGBA(8, 7, 12, 150));
        D_spinner(cx, cy - 16, big ? 36 : 29, C_MUSTARD);
        bool slow = A.resolveJob && SDL_GetTicks() - A.resolveSince > 6000;
        const char *msg = slow ? "Video thiếu nhi / được bảo vệ · đang giải mã luồng (10–40 giây)…" : A.resolveJob ? "Đang lấy luồng YouTube…" : st->state == PS_BUFFERING ? "Mạng chậm · đang nạp thêm…" : "Đang mở luồng…";
        D_text(F_SEMI, big ? 22 : 20, msg, cx, cy + 28, C_INK, a.w - 40, AL_CENTER);
    } else if (st->state == PS_PAUSED || st->state == PS_ENDED) {
        D_circle(cx, cy, big ? 48 : 44, RGBA(8, 7, 12, 160)); D_ring(cx, cy, big ? 48 : 44, 3, C_INK);
        D_tri(cx - 12, cy - 20, cx - 12, cy + 20, cx + 22, cy, C_INK);
    }
    if (Player_switching()) { D_round(a.x + 12, a.y + 12, 210, 30, 8, RGBA(8, 7, 12, 200)); D_text(F_MED, 16, "Đang đổi chất lượng…", a.x + 22, a.y + 17, C_INK, 190, AL_LEFT); }
    if (A.flash[0] && SDL_GetTicks() < A.flashUntil) {
        int w = D_textW(F_BOLD, big ? 30 : 26, A.flash) + 44;
        D_round(cx - w / 2, cy - 30, w, 60, 30, RGBA(8, 7, 12, 210));
        D_text(F_BOLD, big ? 30 : 26, A.flash, cx, cy - D_fontHeight(F_BOLD, big ? 30 : 26) / 2, C_INK, 0, AL_CENTER);
    }
}
static void draw_panel(void) {
    if (!A.panel) return;
    int x = SCREEN_W - 392;
    D_rect(x, 0, 392, SCREEN_H, C_SURFACE); D_rect(x, 0, 3, SCREEN_H, C_BLACK);
    const char *title = A.panel == PANEL_QUALITY ? "Chất lượng video" : A.panel == PANEL_SPEED ? "Tốc độ phát" : "Tùy chọn video";
    const char *sub = A.panel == PANEL_QUALITY ? "Đổi chất lượng mà tiếng vẫn chạy liền" : A.panel == PANEL_SPEED ? "Tiếng giữ đúng cao độ khi tua nhanh" : A.cur.title;
    D_text(F_XBOLD, 28, title, x + 20, 22, C_INK, 352, AL_LEFT);
    D_text(F_REG, 17, sub, x + 20, 62, C_MUTED, 352, AL_LEFT);
    int n = A.panel == PANEL_QUALITY ? NQ : A.panel == PANEL_SPEED ? NS : options_count();
    for (int i = 0; i < n; i++) {
        int y = 96 + i * (n > 7 ? 64 : 68);
        bool sel = i == A.panelSel, avail = A.panel != PANEL_QUALITY || quality_available(i);
        bool current = A.panel == PANEL_QUALITY ? i == A.quality : A.panel == PANEL_SPEED ? i == A.speedIx : false;
        char label[160], note[200];
        if (A.panel == PANEL_QUALITY) { snprintf(label, sizeof(label), "%s", T(QUALITY_LABEL[i])); snprintf(note, sizeof(note), "%s", avail ? T(QUALITY_NOTE[i]) : T("Video này không có")); }
        else if (A.panel == PANEL_SPEED) { snprintf(label, sizeof(label), "%s", SPEED_LABEL[i]); snprintf(note, sizeof(note), "%s", i == 2 ? T("Bình thường") : i == 0 ? T("Chậm") : i == 5 ? T("Nhanh") : ""); }
        else {
            switch (opt_id(i)) {
                case 7: snprintf(label, sizeof(label), "%s · %s", T("Phụ đề"), T(SUB_OVERRIDE[A.subOverride])); snprintf(note, sizeof(note), "%s", Subs_status()); break;
                case 0: snprintf(label, sizeof(label), "%s", T("Chất lượng")); snprintf(note, sizeof(note), T("%s · phím %s"), T(QUALITY_LABEL[A.quality]), KN(K_X)); break;
                case 1: snprintf(label, sizeof(label), "%s", T("Tốc độ phát")); snprintf(note, sizeof(note), T("%s · phím %s"), SPEED_LABEL[A.speedIx], KN(K_START)); break;
                case 2: snprintf(label, sizeof(label), "%s", T("Nghe nền (chỉ tiếng)")); snprintf(note, sizeof(note), "%s", T("Về menu máy, tiếng vẫn phát · mở lại app để xem tiếp")); break;
                case 3: snprintf(label, sizeof(label), "%s", Store_isLater(A.cur.id) ? T("Bỏ khỏi Xem sau") : T("Lưu vào Xem sau")); snprintf(note, sizeof(note), "%s", T("Thư viện › Xem sau")); break;
                case 4: snprintf(label, sizeof(label), "%s", Store_isChannel(A.cur.channel_id) ? T("Bỏ theo dõi kênh") : T("Theo dõi kênh")); snprintf(note, sizeof(note), "%s", A.cur.channel); break;
                case 5: snprintf(label, sizeof(label), "%s", Store_hasDownload(A.cur.id) ? T("Đã tải về máy") : Download_busy() ? T("Đang tải video khác…") : T("Tải về máy · 720p")); snprintf(note, sizeof(note), "%s", T("Xem lại không cần mạng")); break;
                default: snprintf(label, sizeof(label), "%s", A.stats ? T("Ẩn thông tin luồng") : T("Hiện thông tin luồng")); snprintf(note, sizeof(note), "%s", T("Bộ đệm, khung hình, lệch tiếng–hình")); break;
            }
        }
        D_round(x + 20, y, 352, 60, 12, sel ? C_INK : C_RAISED);
        if (sel) D_roundRing(x + 15, y - 5, 362, 70, 15, 4, C_MUSTARD);
        SDL_Color fg = sel ? C_DARK : avail ? C_INK : C_DIM, sub2 = sel ? C_LINE : C_MUTED;
        if (current) D_icon("check", x + 32, y + 17, 26, sel ? C_DARK : C_MUSTARD);
        D_text(F_BOLD, 21, label, x + 70, y + (note[0] ? 6 : 16), fg, 290, AL_LEFT);
        if (note[0]) D_text(F_REG, 15, note, x + 70, y + 35, sub2, 290, AL_LEFT);
    }
    D_text(F_MED, 17, "Lên/Xuống chọn · A Áp dụng · B Đóng", x + 20, SCREEN_H - 40, C_MUTED, 0, AL_LEFT);
}
static void draw_subs(const PlayerStatus *st, int cx, int bottom, int max_w, bool full) {
    if (st->state == PS_IDLE || st->state == PS_ERROR || A.watchErr[0]) return;
    const char *line = Subs_text(st->position);
    if (!line) return;
    long idx = 0; const char *f = Subs_fontFor(Subs_target(), &idx);
    D_setSubFont(f, idx);
    static const int SZ[2][3] = {{18, 22, 26}, {24, 30, 36}};
    D_subtitle(line, SZ[full][S.sub_size], cx, bottom, max_w, full ? 3 : 2);
}
static void draw_watch(void) {
    D_rect(0, 0, SCREEN_W, SCREEN_H, C_BG);
    PlayerStatus st; ui_status(&st);
    SDL_Rect a = {24, 24, 640, 360};
    SDL_Texture *t; int w, h;
    Player_present(A.ren, &t, &w, &h);
    if (st.audio_only || !t || st.state == PS_IDLE) {
        D_rect(a.x, a.y, a.w, a.h, RGBA(0x12, 0x0F, 0x18, 255));
        D_thumb(A.cur.id, (SDL_Rect){a.x + 160, a.y + 60, 320, 180}, RGBA(0x12, 0x0F, 0x18, 255));
        if (st.audio_only && st.state != PS_IDLE && st.state != PS_ERROR && !A.watchErr[0]) D_text(F_SEMI, 20, "Chỉ âm thanh · không giải mã hình", a.x + a.w / 2, a.y + 262, C_MUTED, 0, AL_CENTER);
    } else D_video(t, w, h, a);
    draw_player_overlays(a, &st, false);
    draw_subs(&st, a.x + a.w / 2, a.y + a.h - 92, a.w - 40, false);
    D_rect(a.x, a.y + a.h - 86, a.w, 86, RGBA(0, 0, 0, 120));
    draw_progress(a.x + 16, a.y + a.h - 50, a.w - 32, 6, &st);
    char tm[64], p1[24], p2[24]; fmt_time(st.position, p1, sizeof(p1)); fmt_time(st.duration > 0 ? st.duration : A.cur.duration, p2, sizeof(p2));
    if (st.live || A.cur.live) snprintf(tm, sizeof(tm), "%s", T("TRỰC TIẾP")); else snprintf(tm, sizeof(tm), "%s / %s", p1, p2);
    D_text(F_PIXEL, 26, tm, a.x + 16, a.y + a.h - 38, C_INK, 0, AL_LEFT);
    const char *qb = quality_badge(); int qw = D_textW(F_XBOLD, 15, qb) + 16;
    int sw = D_textW(F_BOLD, 15, SPEED_LABEL[A.speedIx]) + 16;
    D_round(a.x + a.w - 16 - sw, a.y + a.h - 36, sw, 26, 4, RGBA(244, 239, 230, 40)); D_text(F_BOLD, 15, SPEED_LABEL[A.speedIx], a.x + a.w - 16 - sw / 2, a.y + a.h - 33, C_INK, 0, AL_CENTER);
    D_round(a.x + a.w - 24 - sw - qw, a.y + a.h - 36, qw, 26, 4, C_MUSTARD); D_text(F_XBOLD, 15, qb, a.x + a.w - 24 - sw - qw / 2, a.y + a.h - 33, C_DARK, 0, AL_CENTER);
    D_corners(a.x, a.y, a.w, a.h, 14, C_BG);
    D_roundRing(a.x, a.y, a.w, a.h, 14, 3, C_BLACK);
    if (A.watchFocus == 0) D_roundRing(a.x - 8, a.y - 8, a.w + 16, a.h + 16, 20, 4, C_MUSTARD);
    if (A.stats) draw_stats(a.x + 12, a.y + 12, &st);
    int x = 688;
    int used = D_textBox(F_BOLD, 25, A.cur.title, x, 20, 312, 4, 31, C_INK);
    int y = 20 + used * 31 + 8;
    char views[64], meta[160]; fmt_views(A.cur.views, A.cur.live, views, sizeof(views));
    if (strlen(A.stream.upload_date) == 8) snprintf(meta, sizeof(meta), "%s%s%.2s/%.2s/%.4s", views, *views ? " · " : "", A.stream.upload_date + 6, A.stream.upload_date + 4, A.stream.upload_date);
    else snprintf(meta, sizeof(meta), "%s", views);
    D_text(F_REG, 18, meta, x, y, C_MUTED, 312, AL_LEFT); y += 40;
    D_circle(x + 24, y + 24, 24, C_BLACK); D_circle(x + 24, y + 24, 22, avatar_color(A.cur.channel));
    char ini[16]; initials(A.cur.channel, ini); D_text(F_XBOLD, 17, ini, x + 24, y + 24 - D_fontHeight(F_XBOLD, 17) / 2, C_INK, 0, AL_CENTER);
    bool following = Store_isChannel(A.cur.channel_id);
    const char *fl = T(following ? "Đang theo dõi" : "Theo dõi");
    int fw = D_textW(F_BOLD, 16, fl) + 24;
    D_text(F_SEMI, 21, A.cur.channel, x + 60, y + 10, C_INK, 312 - 72 - fw, AL_LEFT);
    D_round(x + 312 - fw, y + 4, fw, 40, 10, following ? C_RAISED : C_INK);
    D_text(F_BOLD, 16, fl, x + 312 - fw / 2, y + 24 - D_fontHeight(F_BOLD, 16) / 2, following ? C_INK : C_DARK, 0, AL_CENTER);
    y += 66;
    const char *acts[4][2] = {{"clock", Store_isLater(A.cur.id) ? "Đã lưu" : "Xem sau"}, {"download", Store_hasDownload(A.cur.id) ? "Đã tải" : "Tải về"}, {"sliders-horizontal", QUALITY_LABEL[A.quality]}, {"gauge", "Tốc độ"}};
    for (int i = 0; i < 4; i++) {
        int bx = x + (i % 2) * 162, by = y + (i / 2) * 62;
        D_round(bx, by, 150, 52, 12, C_SURFACE); D_roundRing(bx, by, 150, 52, 12, 2, C_LINE);
        D_icon(acts[i][0], bx + 14, by + 15, 22, C_INK);
        D_text(F_SEMI, 17, acts[i][1], bx + 44, by + 26 - D_fontHeight(F_SEMI, 17) / 2, C_INK, 98, AL_LEFT);
    }
    D_text(F_XBOLD, 24, "Tiếp theo", 24, 402, C_INK, 0, AL_LEFT);
    D_text(F_MED, 17, "Xuống để chọn · L1/R1 video trước/sau · SELECT tùy chọn", 1000, 408, C_MUTED, 0, AL_RIGHT);
    int nnext = A.queue.count - A.qpos - 1; if (nnext < 0) nnext = 0;
    if (A.nextSel >= nnext) A.nextSel = nnext ? nnext - 1 : 0;
    /* a sliding window of 4 over all suggestions */
    if (A.nextSel < A.nextFirst) A.nextFirst = A.nextSel;
    if (A.nextSel > A.nextFirst + 3) A.nextFirst = A.nextSel - 3;
    if (A.nextFirst > (nnext > 4 ? nnext - 4 : 0)) A.nextFirst = nnext > 4 ? nnext - 4 : 0;
    if (nnext <= 0) D_text(F_REG, 19, A.moreJob ? "Đang tải thêm gợi ý…" : "Hết danh sách · bấm B để chọn video khác", 24, 470, C_DIM, 0, AL_LEFT);
    if (nnext > 4) {
        char pos[32]; snprintf(pos, sizeof(pos), "%d / %d", A.nextSel + 1, nnext);
        D_text(F_PIXEL, 22, pos, 150, 404, C_DIM, 0, AL_LEFT);
    }
    for (int i = A.nextFirst; i < nnext && i < A.nextFirst + 4; i++) {
        VideoItem *v = &A.queue.items[A.qpos + 1 + i];
        int cx = 24 + (i - A.nextFirst) * 248, cy = 444;
        bool on = A.watchFocus == 1 && i == A.nextSel;
        if (on) { cx -= 3; cy -= 3; D_shadow(cx, cy, 232, 216, 14, 8); D_round(cx, cy, 232, 216, 14, C_RAISED); D_roundRing(cx - 4, cy - 4, 240, 224, 16, 4, C_MUSTARD); }
        D_thumb(v->id, (SDL_Rect){cx + 6, cy + 6, 220, 124}, on ? C_RAISED : C_BG);
        if (v->duration > 0) { char d[24]; fmt_time(v->duration, d, sizeof(d)); int bw = D_textW(F_PIXEL, 20, d) + 10; D_round(cx + 220 - bw, cy + 104, bw, 22, 4, RGBA(8, 7, 12, 220)); D_text(F_PIXEL, 20, d, cx + 220 - bw / 2, cy + 103, C_INK, 0, AL_CENTER); }
        D_textBox(F_SEMI, 18, v->title, cx + 8, cy + 136, 216, 2, 23, C_INK);
        D_text(F_REG, 15, v->channel, cx + 8, cy + 186, C_MUTED, 216, AL_LEFT);
    }
    D_rect(0, 716, SCREEN_W, 52, C_BAR); D_rect(0, 716, SCREEN_W, 2, C_RAISED);
    int hx = 24;
    hx += D_hint(hx, 728, KN(K_A), "Phát/dừng") + 20; hx += D_hint(hx, 728, "LR", "Tua 10 giây") + 20; hx += D_hint(hx, 728, KN(K_Y), "Toàn màn hình") + 20; hx += D_hint(hx, 728, KN(K_X), "Chất lượng") + 20; D_hint(hx, 728, KN(K_B), "Về");
    draw_panel();
}
static void draw_full(void) {
    D_rect(0, 0, SCREEN_W, SCREEN_H, C_BLACK);
    PlayerStatus st; ui_status(&st);
    SDL_Rect a = {0, 96, 1024, 576};
    SDL_Texture *t; int w, h;
    Player_present(A.ren, &t, &w, &h);
    if (st.audio_only || !t) {
        D_rect(a.x, a.y, a.w, a.h, RGBA(0x12, 0x0F, 0x18, 255));
        D_thumb(A.cur.id, (SDL_Rect){352, 260, 320, 180}, RGBA(0x12, 0x0F, 0x18, 255));
        if (st.audio_only && st.state != PS_IDLE && st.state != PS_ERROR && !A.watchErr[0]) D_text(F_BOLD, 24, "Chỉ âm thanh · tiết kiệm pin", 512, 470, C_INK, 0, AL_CENTER);
    } else D_videoNative(t, w, h);
    Uint32 now = SDL_GetTicks();
    bool osd = A.osd && (now < A.osdUntil || st.state != PS_PLAYING || A.panel);
    if (A.osd && now >= A.osdUntil && st.state == PS_PLAYING && !A.panel) A.osd = false;
    draw_player_overlays(a, &st, true);
    draw_subs(&st, 512, osd ? 668 : 656, 900, true);
    if (osd) {
        D_text(F_BOLD, 25, A.cur.title, 24, 16, C_INK, 700, AL_LEFT);
        char views[64], meta[200]; fmt_views(A.cur.views, A.cur.live, views, sizeof(views));
        snprintf(meta, sizeof(meta), "%s%s%s", A.cur.channel, *views ? " · " : "", views);
        D_text(F_REG, 18, meta, 24, 54, C_MUTED, 700, AL_LEFT);
        const char *qb = quality_badge(); int qw = D_textW(F_XBOLD, 17, qb) + 20, sw = D_textW(F_BOLD, 17, SPEED_LABEL[A.speedIx]) + 20;
        D_round(1000 - sw, 32, sw, 30, 4, RGBA(244, 239, 230, 40)); D_text(F_BOLD, 17, SPEED_LABEL[A.speedIx], 1000 - sw / 2, 35, C_INK, 0, AL_CENTER);
        D_round(1000 - sw - 10 - qw, 32, qw, 30, 4, C_MUSTARD); D_text(F_XBOLD, 17, qb, 1000 - sw - 10 - qw / 2, 35, C_DARK, 0, AL_CENTER);
        if (st.state == PS_PLAYING && !A.panel) { D_circle(512, 384, 48, RGBA(8, 7, 12, 130)); D_ring(512, 384, 48, 3, C_INK); D_round(496, 364, 10, 40, 3, C_INK); D_round(518, 364, 10, 40, 3, C_INK); }
        draw_progress(24, 684, 976, 8, &st);
        char tm[64], p1[24], p2[24]; fmt_time(st.position, p1, sizeof(p1)); fmt_time(st.duration, p2, sizeof(p2));
        if (st.live) snprintf(tm, sizeof(tm), "%s", T("TRỰC TIẾP")); else snprintf(tm, sizeof(tm), "%s / %s", p1, p2);
        D_text(F_PIXEL, 30, tm, 24, 708, C_INK, 0, AL_LEFT);
        int hx = 1000;
        const char *hk[4][2] = {{KN(K_B), "Thu nhỏ"}, {KN(K_SELECT), "Tùy chọn"}, {KN(K_START), "Tốc độ"}, {KN(K_X), "Chất lượng"}};
        for (int i = 0; i < 4; i++) {
            int kw = D_textW(F_PIXEL, 24, hk[i][0]) + (strlen(hk[i][0]) > 1 ? 14 : 0); if (kw < 28) kw = 28;
            int ww = kw + 8 + D_textW(F_MED, 18, hk[i][1]) + 24;
            hx -= ww; D_round(hx, 708, ww - 8, 42, 10, C_SURFACE); D_roundRing(hx, 708, ww - 8, 42, 10, 2, C_LINE);
            D_hint(hx + 8, 715, hk[i][0], hk[i][1]);
        }
    }
    if (A.stats) draw_stats(24, 112, &st);
    draw_panel();
}
static void draw_volume(void) {
    if (SDL_GetTicks() >= A.volUntil) return;
    int v = Sys_volume();
    int x = 944, y = 170, w = 60, h = 400;
    D_round(x + 6, y + 6, w, h, 16, C_BLACK); D_round(x, y, w, h, 16, C_SURFACE); D_roundRing(x, y, w, h, 16, 2, C_LINE);
    int bx = x + 20, by = y + 58, bh = h - 120;
    D_round(bx, by, 20, bh, 10, C_RAISED);
    int fill = bh * v / 20;
    if (fill > 0) D_round(bx, by + bh - fill, 20, fill, 10, v ? C_MUSTARD : C_DIM);
    char num[8]; snprintf(num, sizeof(num), "%d", v * 5);
    D_text(F_PIXEL, 30, num, x + w / 2, y + 12, C_INK, 0, AL_CENTER);
    D_icon("headphones", x + 16, y + h - 54, 28, C_MUTED);
    D_text(F_MED, 13, Sys_output(), x + w / 2, y + h - 22, C_MUTED, w - 6, AL_CENTER);
}
static void draw_quick(void) {
    if (!A.quick) return;
    D_rect(0, 0, SCREEN_W, SCREEN_H, RGBA(8, 7, 12, 170));
    int x = 232, y = 150, w = 560, h = 470;
    D_round(x + 10, y + 10, w, h, 18, C_BLACK); D_round(x, y, w, h, 18, C_SURFACE); D_roundRing(x, y, w, h, 18, 3, C_BLACK);
    D_text(F_XBOLD, 28, "Bảng nhanh", x + 28, y + 22, C_INK, 0, AL_LEFT);
    char vol[64]; snprintf(vol, sizeof(vol), "%s · %d%%", T(Sys_output()), Sys_volume() * 5);
    D_text(F_MED, 18, vol, x + w - 28, y + 30, C_MUTED, 0, AL_RIGHT);
    const char *t[QUICK_N] = {"Tiếp tục", "Tắt màn hình · vẫn nghe", "Nghe nền (về menu)", "Thoát ứng dụng"};
    const char *n[QUICK_N] = {"Đóng bảng này", "Như YouTube Premium · bấm nút bất kỳ để bật lại", "Ra menu máy, tiếng vẫn phát", "Dừng phát và đóng NXT Retro Tube"};
    for (int i = 0; i < QUICK_N; i++) {
        int ry = y + 80 + i * 86; bool on = i == A.quickSel;
        D_round(x + 24, ry, w - 48, 74, 12, on ? C_INK : C_RAISED);
        if (on) D_roundRing(x + 19, ry - 5, w - 38, 84, 15, 4, C_MUSTARD);
        D_text(F_BOLD, 22, t[i], x + 44, ry + 10, on ? C_DARK : C_INK, w - 88, AL_LEFT);
        D_text(F_REG, 16, n[i], x + 44, ry + 42, on ? C_LINE : C_MUTED, w - 88, AL_LEFT);
    }
    D_text(F_MED, 17, "A Chọn · Trái/Phải âm lượng · B đóng", x + 28, y + h - 40, C_MUTED, 0, AL_LEFT);
}
static void draw_about(void) {
    if (!A.about) return;
    D_rect(0, 0, SCREEN_W, SCREEN_H, RGBA(8, 7, 12, 190));
    int x = 172, y = 120, w = 680, h = 520;
    D_round(x + 10, y + 10, w, h, 18, C_BLACK); D_round(x, y, w, h, 18, C_SURFACE); D_roundRing(x, y, w, h, 18, 3, C_BLACK);
    D_icon("thumbs-up", x + 30, y + 28, 34, C_MUSTARD);
    D_text(F_XBOLD, 30, "NXT Retro Tube", x + 76, y + 26, C_INK, 0, AL_LEFT);
    char ver[96]; snprintf(ver, sizeof(ver), "%s %s · %s", T("Phiên bản"), APP_VERSION, ROM_LABEL);
    D_text(F_MED, 18, ver, x + w - 30, y + 36, C_MUTED, 0, AL_RIGHT);
    D_text(F_REG, 18, T("Ứng dụng được làm bởi"), x + 30, y + 86, C_MUTED, 0, AL_LEFT);
    D_text(F_XBOLD, 34, APP_AUTHOR, x + 30, y + 112, C_INK, w - 60, AL_LEFT);
    D_textBox(F_REG, 18, T("Nếu bạn thấy app hữu ích, có thể ủng hộ tác giả một ly cà phê. Cảm ơn bạn!"), x + 30, y + 168, w - 60, 2, 26, C_MUTED);
    const char *lab[3] = {"MoMo", "Bank", "PayPal"}, *val[3] = {DONATE_MOMO, DONATE_BANK, DONATE_PAYPAL};
    for (int i = 0; i < 3; i++) {
        int ry = y + 236 + i * 76;
        D_round(x + 30, ry, w - 60, 64, 12, C_RAISED);
        D_round(x + 42, ry + 14, 104, 36, 8, C_MUSTARD);
        D_text(F_BOLD, 19, lab[i], x + 94, ry + 21, C_DARK, 0, AL_CENTER);
        D_text(F_PIXEL, 34, val[i], x + 168, ry + 13, C_INK, w - 220, AL_LEFT);
    }
    D_text(F_MED, 17, T("A / B đóng"), x + 30, y + h - 40, C_MUTED, 0, AL_LEFT);
}
/* ---------- kids mode (parental controls) ---------- */
enum { KR_ON, KR_PIN, KR_LIMIT, KR_FROM, KR_TO, KR_BREAK, KR_ONLY, KR_CHANNELS, KR_WORDS, KR_REPORT, KR_EXTRA, KR_COUNT };
static const int BREAKS[] = {0, 15, 20, 30, 45, 60};
static void kids_apply(void) { Innertube_setSafety(K.enabled); reset_feeds(); load_feed(A.tab, true); }
static void pin_open(int purpose) {
    A.pinFor = purpose; A.pinPos = 0; A.pinErrUntil = 0;
    memcpy(A.pin, "0000", 5);
}
static void kids_resume(void) {
    PlayerStatus st; Player_status(&st);
    if (A.hasCur && st.state == PS_PAUSED) Player_setPaused(false);
}
static void pin_submit(void) {
    int purpose = A.pinFor;
    if (purpose == PIN_NEW1) { memcpy(A.pinFirst, A.pin, 5); pin_open(PIN_NEW2); return; }
    if (purpose == PIN_NEW2) {
        if (strcmp(A.pinFirst, A.pin)) { toast("Hai lần nhập không khớp · nhập lại mã PIN mới"); pin_open(PIN_NEW1); return; }
        Kids_setPin(A.pin); A.pinFor = PIN_NONE;
        if (A.pinThenEnable) { A.pinThenEnable = false; K.enabled = true; A.settingsUnlocked = true; Kids_changed(); kids_apply(); toast("Đã bật Chế độ trẻ em"); }
        else toast("Đã lưu mã PIN mới");
        return;
    }
    if (!Kids_checkPin(A.pin)) { A.pinErrUntil = SDL_GetTicks() + 1500; memcpy(A.pin, "0000", 5); A.pinPos = 0; return; }
    A.pinFor = PIN_NONE;
    switch (purpose) {
        case PIN_SETTINGS: A.settingsUnlocked = true; A.screen = SC_SETTINGS; A.keyPage = false; A.capture = -1; icon_state(); break;
        case PIN_QUIT: A.quit = true; break;
        case PIN_EXTEND: Kids_grantExtra(15); A.kidsLock = 0; kids_resume(); toast("Đã cho xem thêm 15 phút"); break;
        case PIN_HOURS: Kids_waiveHoursToday(); A.kidsLock = 0; kids_resume(); toast("Hôm nay được xem ngoài khung giờ"); break;
    }
}
static void pin_key(Key k) {
    char *d = &A.pin[A.pinPos];
    if (k == K_UP) *d = *d == '9' ? '0' : *d + 1;
    else if (k == K_DOWN) *d = *d == '0' ? '9' : *d - 1;
    else if (k == K_LEFT && A.pinPos > 0) A.pinPos--;
    else if (k == K_RIGHT && A.pinPos < 3) A.pinPos++;
    else if (k == K_X) { memcpy(A.pin, "0000", 5); A.pinPos = 0; }
    else if (k == K_A || k == K_START) { if (A.pinPos < 3 && k == K_A) A.pinPos++; else pin_submit(); }
    else if (k == K_B) { A.pinFor = PIN_NONE; A.pinThenEnable = false; }
}
static void kids_build_pick(void) {
    VideoList_clear(&A.kidsPick);
    const VideoList *srcs[2] = {Store_channels(), Store_history()};
    VideoItem cur = A.cur;
    for (int s = -1; s < 2; s++) {
        int n = s < 0 ? (A.hasCur ? 1 : 0) : srcs[s]->count;
        for (int i = 0; i < n; i++) {
            const VideoItem *v = s < 0 ? &cur : &srcs[s]->items[i];
            const char *name = s == 0 ? v->title : v->channel;
            if (!v->channel_id[0] || Kids_approved(v->channel_id)) continue;
            bool dup = false;
            for (int j = 0; j < A.kidsPick.count; j++) if (!strcmp(A.kidsPick.items[j].channel_id, v->channel_id)) dup = true;
            if (dup) continue;
            VideoItem c; memset(&c, 0, sizeof(c));
            snprintf(c.id, sizeof(c.id), "c%d", A.kidsPick.count);     /* unique key for the list */
            snprintf(c.channel_id, sizeof(c.channel_id), "%s", v->channel_id);
            snprintf(c.channel, sizeof(c.channel), "%s", name);
            snprintf(c.title, sizeof(c.title), "%s", s == 0 ? T("Kênh đang theo dõi") : s < 0 ? T("Kênh của video đang phát") : T("Đã xem gần đây"));
            VideoList_add(&A.kidsPick, &c);
        }
    }
}
static void kids_change(int row, int dir) {
    switch (row) {
        case KR_ON:
            if (!K.enabled) {
                if (!Kids_hasPin()) { A.pinThenEnable = true; pin_open(PIN_NEW1); toast("Đặt mã PIN phụ huynh (4 số) trước"); return; }
                K.enabled = true; A.settingsUnlocked = true; toast("Đã bật Chế độ trẻ em");
            } else { K.enabled = false; A.kidsLock = 0; A.kidsBreak = false; toast("Đã tắt Chế độ trẻ em"); }
            Kids_changed(); kids_apply(); return;
        case KR_LIMIT: K.daily_min += dir * 15; if (K.daily_min < 0) K.daily_min = 480; if (K.daily_min > 480) K.daily_min = 0; break;
        case KR_FROM: K.from_h = (K.from_h + dir + 24) % 24; break;
        case KR_TO: K.to_h += dir; if (K.to_h < 1) K.to_h = 24; if (K.to_h > 24) K.to_h = 1; break;
        case KR_BREAK: { int i = 0; while (i < 5 && BREAKS[i] != K.break_min) i++; i = (i + 6 + dir) % 6; K.break_min = BREAKS[i]; Kids_breakDone(); break; }
        case KR_ONLY:
            K.approved_only = !K.approved_only;
            if (K.approved_only && !K.napproved) toast("Chưa có kênh nào được duyệt · thêm kênh ở mục bên dưới");
            Kids_changed(); kids_apply(); return;
        default: return;
    }
    Kids_changed();
}
static void kids_key(Key k) {
    Uint32 now = SDL_GetTicks();
    if (A.kidsPage == 0) {
        if (k == K_UP && A.kidsSel > 0) A.kidsSel--;
        else if (k == K_DOWN && A.kidsSel < KR_COUNT - 1) A.kidsSel++;
        else if ((k == K_LEFT || k == K_RIGHT) && A.kidsSel >= KR_LIMIT && A.kidsSel <= KR_BREAK) kids_change(A.kidsSel, k == K_RIGHT ? 1 : -1);
        else if (k == K_A) {
            switch (A.kidsSel) {
                case KR_ON: case KR_ONLY: kids_change(A.kidsSel, 1); break;
                case KR_PIN: A.pinThenEnable = false; pin_open(PIN_NEW1); break;
                case KR_LIMIT: case KR_FROM: case KR_TO: case KR_BREAK: kids_change(A.kidsSel, 1); break;
                case KR_CHANNELS: A.kidsPage = 1; A.kidsListSel = 0; break;
                case KR_WORDS: A.kidsPage = 3; A.kidsListSel = 0; break;
                case KR_REPORT: A.kidsPage = 4; A.kidsListSel = 0; break;
                case KR_EXTRA: if (!K.enabled) toast("Chế độ trẻ em đang tắt"); else { Kids_grantExtra(15); A.kidsLock = 0; toast("Hôm nay được xem thêm 15 phút"); } break;
            }
        } else if (k == K_B) { A.screen = SC_SETTINGS; }
        return;
    }
    int count = A.kidsPage == 1 ? K.napproved : A.kidsPage == 2 ? A.kidsPick.count : A.kidsPage == 3 ? K.nwords : K.nlog;
    if (k == K_UP && A.kidsListSel > 0) A.kidsListSel--;
    else if (k == K_DOWN && A.kidsListSel + 1 < count) A.kidsListSel++;
    else if (k == K_B) { A.kidsPage = A.kidsPage == 2 ? 1 : 0; A.kidsListSel = 0; }
    else if (A.kidsPage == 1) {
        if (k == K_Y) { kids_build_pick(); A.kidsPage = 2; A.kidsListSel = 0; if (!A.kidsPick.count) toast("Chưa có kênh nào để chọn · hãy xem hoặc theo dõi kênh trước"); }
        else if (k == K_X && count) { Kids_removeApproved(A.kidsListSel); if (A.kidsListSel >= K.napproved && A.kidsListSel) A.kidsListSel--; kids_apply(); toast("Đã bỏ duyệt kênh"); }
    } else if (A.kidsPage == 2) {
        if (k == K_A && count) {
            VideoItem *c = &A.kidsPick.items[A.kidsListSel];
            if (Kids_toggleApproved(c->channel_id, c->channel)) toast("Đã duyệt kênh %s", c->channel);
            memmove(&A.kidsPick.items[A.kidsListSel], &A.kidsPick.items[A.kidsListSel + 1], sizeof(VideoItem) * (A.kidsPick.count - A.kidsListSel - 1));
            A.kidsPick.count--; if (A.kidsListSel >= A.kidsPick.count && A.kidsListSel) A.kidsListSel--;
            kids_apply();
        }
    } else if (A.kidsPage == 3) {
        if (k == K_Y) { A.wordEntry = true; A.raw[0] = 0; A.rawLiteral = false; open_search(); }
        else if (k == K_X && count) { Kids_removeWord(A.kidsListSel); if (A.kidsListSel >= K.nwords && A.kidsListSel) A.kidsListSel--; toast("Đã xóa từ khóa"); }
    } else if (A.kidsPage == 4) {
        if (k == K_X) {
            if (A.kidsConfirm && now < A.kidsConfirmUntil) { Kids_clearReport(); A.kidsConfirm = false; A.kidsListSel = 0; toast("Đã xóa danh sách video đã xem"); }
            else { A.kidsConfirm = true; A.kidsConfirmUntil = now + 3000; toast("Bấm X lần nữa để xóa danh sách video đã xem"); }
        }
    }
}
static void kids_tick(void) {
    if (!K.enabled) { A.kidsLock = 0; A.kidsBreak = false; return; }
    Uint32 now = SDL_GetTicks();
    if (now - A.kidsTickAt < 1000) return;
    A.kidsTickAt = now;
    PlayerStatus ks; Player_status(&ks);
    if (A.hasCur && ks.state == PS_PLAYING) Kids_tick(&A.cur);
    int r = Kids_lockReason();
    if (r && !A.kidsLock) {
        A.kidsLock = r; A.quick = false;
        Log_msg("kids: locked (%s)", r == 1 ? "daily time used up" : "outside the allowed hours");
        if (A.hidden) Player_close(); else if (A.hasCur && (ks.state == PS_PLAYING || ks.state == PS_BUFFERING)) Player_setPaused(true);
    } else if (!r && A.kidsLock) A.kidsLock = 0;
    if (!A.hidden && !A.kidsBreak && !A.kidsLock && !A.screenOff && ks.state == PS_PLAYING && Kids_breakDue()) {
        A.kidsBreak = true; A.breakUntil = now + 60000; Player_setPaused(true);
        Log_msg("kids: eye break");
    }
}
static void kids_overlay_key(Key k) {
    if (A.kidsBreak) {
        if (k == K_A && SDL_GetTicks() >= A.breakUntil) { A.kidsBreak = false; Kids_breakDone(); kids_resume(); }
        return;
    }
    if (A.kidsLock && k == K_A) pin_open(A.kidsLock == 1 ? PIN_EXTEND : PIN_HOURS);
}
static void fmt_minutes(int secs, char *out, size_t n) {
    int m = secs / 60;
    if (m >= 60) snprintf(out, n, T("%d giờ %02d phút"), m / 60, m % 60); else snprintf(out, n, T("%d phút"), m);
}
static void kids_row_text(int row, char *label, size_t ln, char *value, size_t vn, char *note, size_t nn) {
    char a[48], b[48];
    switch (row) {
        case KR_ON: snprintf(label, ln, "%s", T("Chế độ trẻ em")); snprintf(value, vn, "%s", K.enabled ? T("Bật") : T("Tắt"));
            snprintf(note, nn, "%s", T("Lọc nội dung (Chế độ hạn chế của YouTube), trang chủ cho bé, cần mã PIN để vào Cài đặt và thoát app")); break;
        case KR_PIN: snprintf(label, ln, "%s", T("Mã PIN phụ huynh")); snprintf(value, vn, "%s", Kids_hasPin() ? T("Đổi") : T("Đặt"));
            snprintf(note, nn, "%s", T("4 chữ số · dùng để mở Cài đặt, thoát app, cho xem thêm giờ")); break;
        case KR_LIMIT:
            snprintf(label, ln, "%s", T("Giới hạn mỗi ngày"));
            if (K.daily_min) { fmt_minutes(K.daily_min * 60, value, vn); } else snprintf(value, vn, "%s", T("Không giới hạn"));
            fmt_minutes(Kids_usedToday(), a, sizeof(a));
            if (K.daily_min) { fmt_minutes(Kids_leftToday(), b, sizeof(b)); snprintf(note, nn, T("Hôm nay đã xem %s · còn %s · Trái/Phải đổi 15 phút"), a, b); }
            else snprintf(note, nn, T("Hôm nay đã xem %s · Trái/Phải đổi 15 phút"), a);
            break;
        case KR_FROM: snprintf(label, ln, "%s", T("Được xem từ")); if (K.from_h == K.to_h || (K.from_h == 0 && K.to_h == 24)) snprintf(value, vn, "%s", T("Cả ngày")); else snprintf(value, vn, T("%d giờ"), K.from_h);
            snprintf(note, nn, "%s", K.from_h == K.to_h || (K.from_h == 0 && K.to_h == 24) ? T("Xem được cả ngày (từ = đến)") : T("Ngoài khung giờ này app khóa xem, cần mã PIN")); break;
        case KR_TO: snprintf(label, ln, "%s", T("Đến")); snprintf(value, vn, T("%d giờ"), K.to_h);
            snprintf(note, nn, "%s", T("Ví dụ 7 giờ đến 21 giờ · đặt 0 đến 24 để không giới hạn giờ · cần đồng hồ máy đúng giờ")); break;
        case KR_BREAK: snprintf(label, ln, "%s", T("Nhắc nghỉ mắt")); if (K.break_min) snprintf(value, vn, T("%d phút"), K.break_min); else snprintf(value, vn, "%s", T("Tắt"));
            snprintf(note, nn, "%s", T("Xem liền chừng ấy phút thì tạm dừng 1 phút cho bé nghỉ mắt")); break;
        case KR_ONLY: snprintf(label, ln, "%s", T("Chỉ xem kênh đã duyệt")); snprintf(value, vn, "%s", K.approved_only ? T("Bật") : T("Tắt"));
            snprintf(note, nn, "%s", T("Bật: bé chỉ thấy video của các kênh phụ huynh đã duyệt (an toàn nhất)")); break;
        case KR_CHANNELS: snprintf(label, ln, "%s", T("Kênh đã duyệt")); snprintf(value, vn, T("%d kênh"), K.napproved);
            snprintf(note, nn, "%s", T("A mở danh sách · Y thêm từ kênh đang theo dõi / đã xem · X bỏ duyệt")); break;
        case KR_WORDS: snprintf(label, ln, "%s", T("Từ khóa cấm")); snprintf(value, vn, T("%d từ"), K.nwords);
            snprintf(note, nn, "%s", T("Video có tiêu đề / tên kênh chứa từ này bị ẩn, tìm từ này bị chặn (không phân biệt dấu)")); break;
        case KR_REPORT: snprintf(label, ln, "%s", T("Báo cáo xem của bé")); fmt_minutes(Kids_usedToday(), value, vn);
            snprintf(note, nn, "%s", T("Thời gian xem từng ngày và các video đã xem")); break;
        case KR_EXTRA: snprintf(label, ln, "%s", T("Cho xem thêm 15 phút hôm nay")); snprintf(value, vn, "%s", "A");
            snprintf(note, nn, T("Đã cho thêm hôm nay: %d phút"), Kids_extraToday()); break;
    }
}
static void draw_kids(void) {
    D_rect(0, 0, SCREEN_W, SCREEN_H, C_BG);
    const char *titles[5] = {"Chế độ trẻ em", "Kênh đã duyệt", "Chọn kênh để duyệt", "Từ khóa cấm", "Báo cáo xem của bé"};
    D_text(F_XBOLD, 34, titles[A.kidsPage], 24, 16, C_INK, 0, AL_LEFT);
    draw_stripes(76);
    if (A.kidsPage == 0) {
        const char *k[2] = {KN(K_B), KN(K_A)}, *t[2] = {"Về", "Chọn"};
        header_hints(1000, k, t, 2);
        int rowh = 56, top = 96, vis = 10;
        float target = A.kidsSel > vis - 3 ? (A.kidsSel - (vis - 3)) * rowh : 0;
        A.kidsScroll += (target - A.kidsScroll) * 0.35f;
        SDL_RenderSetClipRect(A.ren, &(SDL_Rect){0, top - 6, SCREEN_W, vis * rowh + 6});
        for (int i = 0; i < KR_COUNT; i++) {
            int y = top + i * rowh - (int)A.kidsScroll;
            if (y < top - rowh || y > top + vis * rowh) continue;
            char label[160], value[96], note[300]; kids_row_text(i, label, sizeof(label), value, sizeof(value), note, sizeof(note));
            bool on = i == A.kidsSel;
            if (on) D_round(27, y + 3, 640, 48, 10, C_BLACK);
            D_round(24, y, 640, 48, 10, on ? C_INK : C_SURFACE);
            D_text(F_SEMI, 20, label, 42, y + 12, on ? C_DARK : C_INK, 400, AL_LEFT);
            int vw = D_textW(F_BOLD, 18, value) + 24;
            D_round(648 - vw, y + 8, vw, 32, 8, on ? C_MUSTARD : C_RAISED);
            D_text(F_BOLD, 18, value, 648 - vw / 2, y + 13, on ? C_DARK : C_INK, 0, AL_CENTER);
        }
        SDL_RenderSetClipRect(A.ren, NULL);
        char label[160], value[96], note[300]; kids_row_text(A.kidsSel, label, sizeof(label), value, sizeof(value), note, sizeof(note));
        D_shadow(688, 96, 312, 330, 14, 6); D_round(688, 96, 312, 330, 14, C_SURFACE); D_roundRing(688, 96, 312, 330, 14, 2, C_BLACK);
        D_icon("users", 708, 116, 30, C_MUSTARD);
        D_textBox(F_BOLD, 22, label, 708, 156, 272, 2, 28, C_INK);
        D_text(F_PIXEL, 30, value, 708, 214, C_MUSTARD, 272, AL_LEFT);
        D_textBox(F_REG, 17, note, 708, 258, 272, 6, 24, C_MUTED);
        D_textBox(F_REG, 16, A.kidsSel >= KR_LIMIT && A.kidsSel <= KR_BREAK ? "Trái / Phải để đổi giá trị" : "A để thực hiện", 708, 440, 290, 2, 22, C_DIM);
        draw_nav(4, -1);
        return;
    }
    /* list pages */
    const char *keys[3]; const char *texts[3]; int nk = 0;
    keys[nk] = KN(K_B); texts[nk++] = "Về";
    if (A.kidsPage == 1) { keys[nk] = KN(K_Y); texts[nk++] = "Thêm kênh"; keys[nk] = KN(K_X); texts[nk++] = "Bỏ duyệt"; }
    else if (A.kidsPage == 2) { keys[nk] = KN(K_A); texts[nk++] = "Duyệt kênh"; }
    else if (A.kidsPage == 3) { keys[nk] = KN(K_Y); texts[nk++] = "Thêm từ"; keys[nk] = KN(K_X); texts[nk++] = "Xóa"; }
    else { keys[nk] = KN(K_X); texts[nk++] = "Xóa danh sách"; }
    header_hints(1000, keys, texts, nk);
    int top = 96, rowh = 62;
    if (A.kidsPage == 4) {
        /* the last 7 days as bars */
        int maxs = 60 * 60; for (int i = 0; i < K.ndays && i < 7; i++) if (K.days[i].secs > maxs) maxs = K.days[i].secs;
        for (int i = 0; i < 7; i++) {
            int x = 24 + i * 140, h = i < K.ndays ? 90 * K.days[i].secs / maxs : 0;
            D_round(x, top, 128, 128, 10, C_SURFACE);
            if (h > 0) D_round(x + 44, top + 100 - h, 40, h, 6, i == 0 ? C_MUSTARD : C_TOMATO);
            char m[32]; if (i < K.ndays) fmt_minutes(K.days[i].secs, m, sizeof(m)); else snprintf(m, sizeof(m), "-");
            D_text(F_MED, 14, i < K.ndays ? K.days[i].day + 5 : "", x + 64, top + 102, C_MUTED, 120, AL_CENTER);
            D_text(F_BOLD, 15, m, x + 64, top + 4, C_INK, 120, AL_CENTER);
        }
        top += 142;
    }
    int count = A.kidsPage == 1 ? K.napproved : A.kidsPage == 2 ? A.kidsPick.count : A.kidsPage == 3 ? K.nwords : K.nlog;
    int vis = (690 - top) / rowh;
    float target = A.kidsListSel > vis - 2 ? (A.kidsListSel - (vis - 2)) * rowh : 0;
    A.kidsScroll += (target - A.kidsScroll) * 0.35f;
    if (!count) {
        const char *empty = A.kidsPage == 1 ? "Chưa duyệt kênh nào · bấm Y để thêm" : A.kidsPage == 2 ? "Chưa có kênh để chọn · hãy xem hoặc theo dõi vài kênh trước" :
                            A.kidsPage == 3 ? "Chưa có từ khóa cấm · bấm Y để thêm" : "Chưa có video nào được xem ở Chế độ trẻ em";
        D_text(F_BOLD, 22, empty, SCREEN_W / 2, top + 120, C_MUTED, 900, AL_CENTER);
    }
    SDL_RenderSetClipRect(A.ren, &(SDL_Rect){0, top - 4, SCREEN_W, 690 - top + 4});
    for (int i = 0; i < count; i++) {
        int y = top + i * rowh - (int)A.kidsScroll;
        if (y < top - rowh || y > 690) continue;
        bool on = i == A.kidsListSel;
        if (on) D_round(27, y + 3, 976, 54, 10, C_BLACK);
        D_round(24, y, 976, 54, 10, on ? C_INK : C_SURFACE);
        char l1[300], l2[200];
        if (A.kidsPage == 1) { snprintf(l1, sizeof(l1), "%s", K.approved[i].name); snprintf(l2, sizeof(l2), "%s", K.approved[i].id); }
        else if (A.kidsPage == 2) { snprintf(l1, sizeof(l1), "%s", A.kidsPick.items[i].channel); snprintf(l2, sizeof(l2), "%s", A.kidsPick.items[i].title); }
        else if (A.kidsPage == 3) { snprintf(l1, sizeof(l1), "%s", K.words[i]); l2[0] = 0; }
        else { char m[32]; fmt_minutes(K.log[i].secs, m, sizeof(m)); snprintf(l1, sizeof(l1), "%s", K.log[i].title); snprintf(l2, sizeof(l2), "%s %s · %s · %s", K.log[i].day + 5, K.log[i].at, K.log[i].channel, m); }
        D_text(F_SEMI, 20, l1, 44, y + (l2[0] ? 6 : 15), on ? C_DARK : C_INK, 930, AL_LEFT);
        if (l2[0]) D_text(F_REG, 15, l2, 44, y + 32, on ? C_LINE : C_MUTED, 930, AL_LEFT);
    }
    SDL_RenderSetClipRect(A.ren, NULL);
    draw_nav(4, -1);
}
static void draw_pin(void) {
    if (!A.pinFor) return;
    D_rect(0, 0, SCREEN_W, SCREEN_H, RGBA(8, 7, 12, 200));
    int x = 232, y = 170, w = 560, h = 420;
    D_round(x + 10, y + 10, w, h, 18, C_BLACK); D_round(x, y, w, h, 18, C_SURFACE); D_roundRing(x, y, w, h, 18, 3, C_BLACK);
    const char *title = A.pinFor == PIN_NEW1 ? "Đặt mã PIN phụ huynh" : A.pinFor == PIN_NEW2 ? "Nhập lại mã PIN mới" :
                        A.pinFor == PIN_SETTINGS ? "Mã PIN để mở Cài đặt" : A.pinFor == PIN_QUIT ? "Mã PIN để thoát app" :
                        A.pinFor == PIN_EXTEND ? "Mã PIN để xem thêm 15 phút" : "Mã PIN để xem ngoài giờ hôm nay";
    D_icon("users", x + 28, y + 26, 30, C_MUSTARD);
    D_text(F_XBOLD, 26, title, x + 70, y + 24, C_INK, w - 100, AL_LEFT);
    D_text(F_REG, 17, "Dành cho phụ huynh", x + 70, y + 60, C_MUTED, 0, AL_LEFT);
    for (int i = 0; i < 4; i++) {
        int bx = x + 70 + i * 110, by = y + 120;
        bool on = i == A.pinPos;
        D_round(bx, by, 90, 120, 14, on ? C_INK : C_RAISED);
        if (on) D_roundRing(bx - 5, by - 5, 100, 130, 17, 4, C_MUSTARD);
        char d[4] = {A.pin[i], 0};
        bool show = A.pinFor == PIN_NEW1 || A.pinFor == PIN_NEW2 || on;
        D_text(F_PIXEL, 72, show ? d : "*", bx + 45, by + 18, on ? C_DARK : C_INK, 0, AL_CENTER);
        if (on) { D_tri(bx + 35, by - 14, bx + 55, by - 14, bx + 45, by - 26, C_MUSTARD); D_tri(bx + 35, by + 134, bx + 55, by + 134, bx + 45, by + 146, C_MUSTARD); }
    }
    if (SDL_GetTicks() < A.pinErrUntil) D_text(F_BOLD, 20, "Sai mã PIN", x + w / 2, y + 286, C_TOMATO, 0, AL_CENTER);
    D_textBox(F_MED, 17, "Lên/Xuống đổi số · Trái/Phải chuyển ô · A xác nhận · X nhập lại · B hủy", x + 28, y + h - 64, w - 56, 2, 24, C_MUTED);
}
static void draw_kids_lock(void) {
    if (!K.enabled || (!A.kidsLock && !A.kidsBreak) || parent_mode()) return;
    D_rect(0, 0, SCREEN_W, SCREEN_H, RGBA(8, 7, 12, 235));
    int x = 172, y = 170, w = 680, h = 420;
    D_round(x + 10, y + 10, w, h, 18, C_BLACK); D_round(x, y, w, h, 18, C_SURFACE); D_roundRing(x, y, w, h, 18, 3, C_BLACK);
    D_icon("clock", x + w / 2 - 30, y + 34, 60, C_MUSTARD);
    if (A.kidsBreak) {
        Uint32 now = SDL_GetTicks(); int left = now < A.breakUntil ? (int)((A.breakUntil - now + 999) / 1000) : 0;
        D_text(F_XBOLD, 32, "Nghỉ mắt một chút nhé!", x + w / 2, y + 120, C_INK, w - 60, AL_CENTER);
        D_textBox(F_REG, 20, "Nhìn ra xa, chớp mắt vài lần và vươn vai. Video sẽ chờ bé.", x + 50, y + 180, w - 100, 2, 28, C_MUTED);
        char c[48]; if (left) snprintf(c, sizeof(c), T("Còn %d giây"), left); else snprintf(c, sizeof(c), "%s", T("A để xem tiếp"));
        D_text(F_PIXEL, 44, c, x + w / 2, y + 270, left ? C_MUSTARD : C_INK, 0, AL_CENTER);
        return;
    }
    D_text(F_XBOLD, 32, A.kidsLock == 1 ? "Hết giờ xem hôm nay rồi!" : "Bây giờ chưa phải giờ xem", x + w / 2, y + 120, C_INK, w - 60, AL_CENTER);
    char note[200];
    if (A.kidsLock == 1) { char m[32]; fmt_minutes(Kids_usedToday(), m, sizeof(m)); snprintf(note, sizeof(note), T("Hôm nay bé đã xem %s. Hẹn gặp lại ngày mai nhé!"), m); }
    else snprintf(note, sizeof(note), T("Bé được xem từ %d giờ đến %d giờ. Hẹn gặp lại sau nhé!"), K.from_h, K.to_h);
    D_textBox(F_REG, 20, note, x + 50, y + 180, w - 100, 2, 28, C_MUTED);
    char hint[120]; snprintf(hint, sizeof(hint), T("Phụ huynh: %s nhập mã PIN để %s"), KN(K_A), A.kidsLock == 1 ? T("cho xem thêm 15 phút") : T("cho xem hôm nay"));
    D_text(F_MED, 18, hint, x + w / 2, y + h - 60, C_DIM, w - 40, AL_CENTER);
}

static void draw_overlays(void) {
    draw_kids_lock();
    draw_about();
    draw_quick();
    draw_pin();
    draw_volume();
    if (A.toast[0] && SDL_GetTicks() < A.toastUntil) {
        int w = D_textW(F_SEMI, 20, A.toast) + 44; if (w > 960) w = 960;
        int y = A.screen == SC_FULL ? 600 : 540;
        D_round(512 - w / 2 + 6, y + 6, w, 50, 12, C_BLACK); D_round(512 - w / 2, y, w, 50, 12, C_INK);
        D_text(F_SEMI, 20, A.toast, 512, y + 25 - D_fontHeight(F_SEMI, 20) / 2, C_DARK, w - 30, AL_CENTER);
    }
    if (A.dialog) {
        D_rect(0, 0, SCREEN_W, SCREEN_H, RGBA(8, 7, 12, 190));
        int x = 182, y = 214, w = 660, h = 340;
        D_round(x + 10, y + 10, w, h, 18, C_BLACK); D_round(x, y, w, h, 18, C_SURFACE); D_roundRing(x, y, w, h, 18, 3, C_BLACK);
        D_text(F_XBOLD, 28, "Đang phát video · thoát NXT Retro Tube?", x + 30, y + 28, C_INK, w - 60, AL_LEFT);
        const char *t[2] = {"Nghe nền (chỉ tiếng)", "Thoát hẳn ứng dụng"}, *n[2] = {"Thoát ra ngoài, tiếng vẫn phát · tắt hình để tiết kiệm pin", "Dừng video và đóng hoàn toàn NXT Retro Tube"};
        for (int i = 0; i < 2; i++) {
            int ry = y + 86 + i * 96; bool on = i == A.dialogSel;
            D_round(x + 30, ry, w - 60, 82, 12, on ? C_INK : C_RAISED);
            if (on) D_roundRing(x + 25, ry - 5, w - 50, 92, 15, 4, C_MUSTARD);
            D_text(F_BOLD, 24, t[i], x + 50, ry + 12, on ? C_DARK : C_INK, w - 100, AL_LEFT);
            D_text(F_REG, 18, n[i], x + 50, ry + 46, on ? C_LINE : C_MUTED, w - 100, AL_LEFT);
        }
        char hint[160]; snprintf(hint, sizeof(hint), T("%s Chọn · %s Thoát hẳn · %s Ở lại"), KN(K_A), KN(K_B), KN(K_START));
        D_text(F_MED, 18, hint, x + 30, y + h - 44, C_MUTED, 0, AL_LEFT);
    }
}

/* ---------- main loop ---------- */
static Key map_phys(int phys) {
    if (phys == 8) return K_MENU;          /* TrimUI logo */
    if (phys == 15) return K_HOME;         /* side Home (Brick Pro) */
    if (phys == 14) return K_VOLUP;
    if (phys == 13) return K_VOLDN;
    for (int i = 0; i < KM_COUNT; i++) if (S.keymap[i] == phys) return KM_KEYS[i];
    return K_NONE;
}
static Key map_key(SDL_Keycode k) {
    switch (k) {
        case SDLK_UP: return K_UP; case SDLK_DOWN: return K_DOWN; case SDLK_LEFT: return K_LEFT; case SDLK_RIGHT: return K_RIGHT;
        case SDLK_z: case SDLK_RETURN: return K_A; case SDLK_x: case SDLK_BACKSPACE: return K_B; case SDLK_a: return K_X; case SDLK_s: return K_Y;
        case SDLK_q: return K_L1; case SDLK_w: return K_R1; case SDLK_1: return K_L2; case SDLK_2: return K_R2;
        case SDLK_SPACE: return K_START; case SDLK_TAB: return K_SELECT; case SDLK_ESCAPE: return K_MENU;
        case SDLK_VOLUMEUP: case SDLK_EQUALS: return K_VOLUP; case SDLK_VOLUMEDOWN: case SDLK_MINUS: return K_VOLDN;
        case SDLK_HOME: case SDLK_h: return K_HOME; case SDLK_POWER: case SDLK_p: return K_POWER;
        default: return K_NONE;
    }
}
static Uint32 last_input;
static void press(Key k) { if (k == K_NONE) return; last_input = SDL_GetTicks(); if (!held[k]) { held[k] = true; repeat_at[k] = SDL_GetTicks() + 320; handle_key(k); } }
static void release(Key k) { if (k != K_NONE) held[k] = false; }
static bool trig_down[2];
static void phys_press(int phys) {
    if (A.capture >= 0 && A.screen == SC_SETTINGS && A.keyPage) { if (phys != 8 && phys != 15 && phys != 13 && phys != 14) capture_assign(phys); return; }
    press(map_phys(phys));
}
#ifdef NXT_TUBE_PW
/* MENU is the OS hotkey on muOS / Knulli (MENU + volume = brightness, ...):
 * act on it only when it is released without another button in between */
static bool menu_down, menu_used;
static void pad_cb(int phys, bool down) {
    if (phys == 8) {
        if (down) { menu_down = true; menu_used = false; return; }
        bool fire = menu_down && !menu_used; menu_down = false;
#ifdef NXT_TUBE_ROM_DARKOS
        /* R36S: no Home / logo button; the hotkey (R3) alone opens the quick panel */
        if (fire && !(A.capture >= 0 && A.screen == SC_SETTINGS && A.keyPage)) { press(K_HOME); release(K_HOME); }
#else
        if (fire && !(A.capture >= 0 && A.screen == SC_SETTINGS && A.keyPage)) { press(K_MENU); release(K_MENU); }
#endif
        return;
    }
    if (menu_down && down) menu_used = true;
    if (menu_down && (phys == 13 || phys == 14)) return;     /* MENU + volume: the OS changes brightness */
    if (down) phys_press(phys); else release(map_phys(phys));
}
static void hat_cb(int u, int d, int l, int r) {
    if (menu_down && (u || d || l || r)) menu_used = true;
#ifdef NXT_TUBE_ROM_DARKOS
    if (menu_down && (u || d || l || r)) return;     /* hotkey + D-pad: dArkOS brightness */
#endif
    if (u) press(K_UP); else release(K_UP);
    if (d) press(K_DOWN); else release(K_DOWN);
    if (l) press(K_LEFT); else release(K_LEFT);
    if (r) press(K_RIGHT); else release(K_RIGHT);
}
#endif
static void poll_input(void) {
    SDL_Event e;
#ifndef NXT_TUBE_PW
    if (osd_gate()) {
        while (SDL_PollEvent(&e)) if (e.type == SDL_QUIT) A.quit = true;
        memset(held, 0, sizeof(held)); trig_down[0] = trig_down[1] = false;
        Sys_powerPressed();                       /* drop power presses meant for the panel */
        last_input = SDL_GetTicks();
        return;
    }
#endif
#ifdef NXT_TUBE_PW
    Pad_poll(pad_cb, hat_cb);
#endif
    while (SDL_PollEvent(&e)) {
#ifdef NXT_TUBE_PW
        /* the pad (and the OS keys) come from evdev; SDL would repeat them */
        if (e.type == SDL_JOYBUTTONDOWN || e.type == SDL_JOYBUTTONUP || e.type == SDL_JOYHATMOTION || e.type == SDL_JOYAXISMOTION || e.type == SDL_JOYDEVICEADDED) continue;
        if ((e.type == SDL_KEYDOWN || e.type == SDL_KEYUP)) {
            SDL_Keycode kc = e.key.keysym.sym;
            if (kc == SDLK_VOLUMEUP || kc == SDLK_VOLUMEDOWN || kc == SDLK_POWER || kc == SDLK_HOME || kc == SDLK_AC_HOME) continue;
            if (e.type == SDL_KEYDOWN && kc == SDLK_F4 && (e.key.keysym.mod & KMOD_ALT)) { A.quit = true; continue; }   /* Knulli exit hotkey */
        }
#endif
        switch (e.type) {
            case SDL_QUIT: A.quit = true; break;
            case SDL_JOYBUTTONDOWN: phys_press(e.jbutton.button); break;
            case SDL_JOYBUTTONUP: release(map_phys(e.jbutton.button)); break;
            case SDL_JOYHATMOTION: {
                int v = e.jhat.value;
                if (v & SDL_HAT_UP) press(K_UP); else release(K_UP);
                if (v & SDL_HAT_DOWN) press(K_DOWN); else release(K_DOWN);
                if (v & SDL_HAT_LEFT) press(K_LEFT); else release(K_LEFT);
                if (v & SDL_HAT_RIGHT) press(K_RIGHT); else release(K_RIGHT);
                break;
            }
            case SDL_JOYAXISMOTION:
                if (e.jaxis.axis == 2 || e.jaxis.axis == 5) {
                    int t = e.jaxis.axis == 2 ? 0 : 1, phys = 100 + t;
                    if (e.jaxis.value > 12000 && !trig_down[t]) { trig_down[t] = true; phys_press(phys); }
                    else if (e.jaxis.value < 6000 && trig_down[t]) { trig_down[t] = false; release(map_phys(phys)); }
                }
                break;
            case SDL_KEYDOWN: if (!e.key.repeat) press(map_key(e.key.keysym.sym)); break;
            case SDL_KEYUP: release(map_key(e.key.keysym.sym)); break;
            case SDL_JOYDEVICEADDED: if (!A.joy) A.joy = SDL_JoystickOpen(e.jdevice.which); break;
        }
    }
    Uint32 now = SDL_GetTicks();
    if (A.capture >= 0 && now > A.captureUntil) { A.capture = -1; toast("Hết giờ chờ · chưa đổi phím"); }
    if (Sys_powerPressed()) handle_key(K_POWER);
    Key rep[] = {K_UP, K_DOWN, K_LEFT, K_RIGHT, K_VOLUP, K_VOLDN};
    for (unsigned i = 0; i < sizeof(rep) / sizeof(rep[0]); i++) {
        Key k = rep[i];
        if (held[k] && now >= repeat_at[k]) { repeat_at[k] = now + (k == K_VOLUP || k == K_VOLDN ? 140 : 90); handle_key(k); }
    }
}
static bool wants_prefetch(const VideoItem *v) {
    if (v->live || strlen(v->id) != 11 || Store_hasDownload(v->id)) return false;
    if (A.hasCur && !strcmp(v->id, A.cur.id) && (A.streamReady || A.resolveJob)) return false;
    return !Yt_hasStream(v->id) && !Yt_isResolving(v->id) && !Yt_deferred(v->id);
}
static void poll_usb(Uint32 now) {
    if (now < A.nextUsbPoll) return;
    A.nextUsbPoll = now + 2000;
    int card = AudioRoute_usbCard();
#ifdef NXT_TUBE_PW
    /* PipeWire publishes the DAC a moment after the kernel card appears: retry a few times */
    static int pw_retry;
    if (card != A.usbCard) pw_retry = 4;
    else if (card >= 0 && S.audio_out != 1 && !Player_usbOutput() && pw_retry > 0) {
        pw_retry--;
        if (Player_setAudioMode(S.audio_out) && Player_usbOutput()) { Sys_applyGain(); pw_retry = 0; if (!A.hidden) toast("Đã chuyển âm thanh sang DAC USB"); }
        return;
    }
#endif
    if (card != A.usbCard) {
        Log_msg("USB audio %s (card %d)", card >= 0 ? "connected" : "removed", card);
        A.usbCard = card;
        if (S.audio_out != 1) {
            Player_setAudioMode(S.audio_out); Sys_applyGain();
            if (!A.hidden) toast(card >= 0 && Player_usbOutput() ? "Đã chuyển âm thanh sang DAC USB" : "Âm thanh trở lại loa máy");
        }
    }
}
/* CPU policy every half second: saver with the screen off / in the background,
 * full speed only for 60 fps / 1080p video or when the decoder falls behind */
static void cpu_tick(Uint32 now) {
    static Uint32 next;
    if ((Sint32)(now - next) < 0) return;
    next = now + 500;
    Sys_cpuLevel(A.screenOff || A.hidden ? 0 : Player_wantsBoost() ? 2 : 1);
}
static void update(void) {
    poll_jobs();
    cpu_tick(SDL_GetTicks());
    kids_tick();
#ifdef NXT_TUBE_ROM_DARKOS
    { PlayerStatus ks; Player_status(&ks); Sys_keepAwake(ks.state == PS_PLAYING || ks.state == PS_BUFFERING || A.resolveJob); }
#endif
    Download_tick();
    Uint32 now = SDL_GetTicks();
    if (A.seekPending && (Sint32)(now - A.seekAt) >= 0) { A.seekPending = false; Player_seek(A.seekTarget); }
    volume_tick(now);
    Ota_tick(now);
    if (Ota_state() != A.otaToldState) {
        int st = Ota_state(); A.otaToldState = st;
        if (st == OTA_AVAILABLE && A.screen != SC_SETTINGS) toast("Có bản cập nhật %s · Cài đặt › Cập nhật", Ota_latest());
        if (st == OTA_READY) toast("Đã cài bản %s · Cài đặt › Cập nhật › A để khởi động lại", Ota_latest());
    }
    PlayerStatus st; Player_status(&st);
    poll_usb(now);
    if (A.hasCur && (st.state == PS_PLAYING) && now - A.lastHistory > 15000) { save_history(); Store_save(); A.lastHistory = now; }
    if (A.hasCur && st.state == PS_ENDED && !A.endedHandled) {
        A.endedHandled = true;
        Store_addHistory(&A.cur, st.duration, st.duration); Store_save();
        if (S.autoplay && A.qpos + 1 < A.queue.count) { toast("Video tiếp theo: %s", A.queue.items[A.qpos + 1].title); play_index(A.qpos + 1); }
    }
    /* suggestions after the current video: top up the queue when it runs short */
    if (A.hasCur) {
        int remaining = A.queue.count - A.qpos - 1;
        if (A.moreJob) {
            JobState js = Yt_state(A.moreJob);
            if (js == JOB_DONE) {
                VideoList l = {0};
                if (Yt_takeList(A.moreJob, &l)) {
                    int before = A.queue.count;
                    for (int i = 0; i < l.count; i++) if (strcmp(l.items[i].id, A.cur.id) && Kids_allowed(&l.items[i])) VideoList_add(&A.queue, &l.items[i]);
                    A.moreStart += 20;
                    if (!l.count || (A.queue.count == before && ++A.moreEmpty >= 3)) A.moreDone = true;
                    if (A.queue.count > before) A.moreEmpty = 0;
                    Log_msg("suggestions: +%d (queue %d)", A.queue.count - before, A.queue.count);
                }
                VideoList_clear(&l); A.moreJob = 0;
            } else if (js == JOB_FAILED) { Yt_forget(A.moreJob); A.moreJob = 0; A.moreFailedAt = now; }
        } else if ((remaining < 8 || (A.watchFocus == 1 && A.nextSel >= remaining - 4)) && !A.moreDone && (!A.moreFailedAt || now - A.moreFailedAt > 30000)) {
            if (!A.moreQuery[0]) {
                /* related videos: the title's first words (plus the channel when the title is short) */
                char q[300] = ""; int words = 0; size_t k = 0;
                for (const char *p = A.cur.title; *p && words < 6 && k + 1 < sizeof(q); p++) {
                    if (*p == '|' || *p == '(' || *p == '[' || *p == '#') break;
                    if (*p == ' ' && k && q[k - 1] != ' ') words++;
                    q[k++] = *p;
                }
                q[k] = 0;
                if (words < 2 && A.cur.channel[0]) { size_t l = strlen(q); snprintf(q + l, sizeof(q) - l, " %s", A.cur.channel); }
                snprintf(A.moreQuery, sizeof(A.moreQuery), "%s", q);
            }
            if (A.moreQuery[0]) { A.moreJob = Yt_searchPage(A.moreQuery, A.moreStart, 20, false, false); Log_msg("suggestions: asking \"%s\" from %d (job %d)", A.moreQuery, A.moreStart, A.moreJob); }
        }
    }
    /* home grid near the end: load the next page */
    if (A.screen == SC_HOME && A.focus == 0) {
        Feed *f = &A.feed[A.tab];
        if (f->list.count && A.sel / 3 >= (f->list.count - 1) / 3 - 1 && !feed_busy(f) && !f->exhausted && !f->err[0]) request_page(A.tab, true);
    }
    /* Prefetch: the item under the cursor (after a short rest) and the next few,
     * so pressing A starts at once. Paused while a video decodes. */
    bool decoding = st.state == PS_PLAYING && !st.audio_only;
    if (A.hidden || decoding || !S.prefetch) return;
    const VideoList *list = NULL; int at = -1;
    if (A.screen == SC_HOME && A.focus == 0) { list = &A.feed[A.tab].list; at = A.sel; }
    else if (A.screen == SC_SEARCH && A.results) { list = &A.found; at = A.resultSel; }
    else if (A.screen == SC_LIBRARY && A.libTab != 2) { list = library_list(); at = A.libSel; }
    else if ((A.screen == SC_WATCH || A.screen == SC_FULL) && A.hasCur) { list = &A.queue; at = A.watchFocus == 1 ? A.qpos + 1 + A.nextSel : A.qpos + 1; }
    if (!list || at < 0 || at >= list->count) return;
    const VideoItem *hover = &list->items[at];
    if (strcmp(hover->id, A.hoverId)) {
        if (A.hoverJob && A.hoverJob != A.resolveJob && Yt_state(A.hoverJob) == JOB_NONE) Yt_forget(A.hoverJob);
        A.hoverJob = 0;
        snprintf(A.hoverId, sizeof(A.hoverId), "%s", hover->id); A.hoverSince = now; A.prefetched = false;
    } else if (!A.prefetched && now - A.hoverSince > 250) {
        A.prefetched = true;
        if (wants_prefetch(hover)) A.hoverJob = Yt_resolve(hover->id, false);
    }
    if (A.warmJob) { JobState js = Yt_state(A.warmJob); if (js == JOB_NONE || js == JOB_RUNNING) return; A.warmJob = 0; }
    if (now < A.warmAfter) return;
    A.warmAfter = now + 800;
    for (int i = at; i < list->count && i < at + 6; i++)
        if (wants_prefetch(&list->items[i])) { A.warmJob = Yt_resolve(list->items[i].id, false); break; }
}
static void draw(void) {
    D_frame(); Thumbs_tick();
    switch (A.screen) {
        case SC_HOME: draw_home(); break;
        case SC_SEARCH: draw_search(); break;
        case SC_LIBRARY: draw_library(); break;
        case SC_SETTINGS: draw_settings(); break;
        case SC_KIDS: draw_kids(); break;
        case SC_WATCH: draw_watch(); break;
        case SC_FULL: draw_full(); break;
    }
    draw_overlays();
    SDL_RenderPresent(A.ren);
}

static void setup_paths(void) {
    if (!getcwd(paths.app, sizeof(paths.app))) snprintf(paths.app, sizeof(paths.app), ".");
    const char *shared = getenv("SHARED_USERDATA_PATH");
    if (shared && *shared) snprintf(paths.data, sizeof(paths.data), "%s/nxt-retro-tube", shared);
    else snprintf(paths.data, sizeof(paths.data), "%s/data", paths.app);
    snprintf(paths.cache, sizeof(paths.cache), "%s/cache", paths.data);
    snprintf(paths.bin, sizeof(paths.bin), "%s/bin", paths.app);
    /* every host we talk to (youtube.com, googlevideo.com, ytimg.com) chains to these roots;
     * the short file is parsed on each TLS connect, so it keeps seeks and opens fast */
    snprintf(paths.cacert, sizeof(paths.cacert), "%s/res/google-roots.pem", paths.app);
    if (access(paths.cacert, R_OK)) snprintf(paths.cacert, sizeof(paths.cacert), "%s/res/cacert.pem", paths.app);
    mkdir(paths.data, 0755); mkdir(paths.cache, 0755);
    char p[1300];
    snprintf(p, sizeof(p), "%s/home", paths.data); mkdir(p, 0755);
    snprintf(p, sizeof(p), "%s/downloads", paths.data); mkdir(p, 0755);
}
static void on_signal(int s) { (void)s; A.quit = true; }

/* QA hooks: NXT_TUBE_KEYS="RRDA.." replays keys (one each 10 frames; ',' waits),
 * NXT_TUBE_SHOT=prefix saves PNG screenshots at each '#' and at the end, then quits. */
static int test_frame, test_pos, test_shots; static bool shot_now;
static void save_shot(void) {
    const char *prefix = getenv("NXT_TUBE_SHOT"); if (!prefix) return;
    int ow = SCREEN_W, oh = SCREEN_H; SDL_GetRendererOutputSize(A.ren, &ow, &oh);
    SDL_Surface *sf = SDL_CreateRGBSurfaceWithFormat(0, ow, oh, 32, SDL_PIXELFORMAT_ABGR8888);
    if (!sf) return;
    SDL_RenderSetLogicalSize(A.ren, 0, 0); SDL_RenderSetViewport(A.ren, NULL);
    SDL_RenderReadPixels(A.ren, NULL, SDL_PIXELFORMAT_ABGR8888, sf->pixels, sf->pitch);
    SDL_RenderSetLogicalSize(A.ren, SCREEN_W, SCREEN_H);
    char p[1200]; snprintf(p, sizeof(p), "%s%02d.png", prefix, test_shots++);
#ifdef NXT_TUBE_PW
    size_t pl = strlen(p); if (pl > 4) snprintf(p + pl - 4, sizeof(p) - (pl - 4), ".bmp");
    SDL_SaveBMP(sf, p); SDL_FreeSurface(sf); Log_msg("shot %s", p);
#else
    IMG_SavePNG(sf, p); SDL_FreeSurface(sf); Log_msg("shot %s", p);
#endif
}
static void test_script(void) {
    const char *keys = getenv("NXT_TUBE_KEYS");
    if (!keys && !getenv("NXT_TUBE_SHOT")) return;
    if (!keys) keys = "";
    test_frame++;
    if (test_frame < 40 || test_frame % 10) return;
    char c = keys[test_pos];
    if (!c) { if (test_frame > 40 + (test_pos + 4) * 10) { shot_now = true; if (test_frame > 40 + (test_pos + 5) * 10) A.quit = true; } return; }
    test_pos++;
    Key k = K_NONE;
    switch (c) { case 'U': k = K_UP; break; case 'D': k = K_DOWN; break; case 'L': k = K_LEFT; break; case 'R': k = K_RIGHT; break;
        case 'A': k = K_A; break; case 'B': k = K_B; break; case 'X': k = K_X; break; case 'Y': k = K_Y; break;
        case 'l': k = K_L1; break; case 'r': k = K_R1; break; case '[': k = K_L2; break; case ']': k = K_R2; break;
        case 'S': k = K_START; break; case 's': k = K_SELECT; break; case 'V': k = K_VOLUP; break; case 'v': k = K_VOLDN; break; case 'H': k = K_HOME; break; case 'P': k = K_POWER; break; case '#': shot_now = true; break;
        case 'E': S.lang = 1 - S.lang; break; }
    if (k != K_NONE) handle_key(k);
}

int psa_crypto_init(void);
static char self_exe[1024];
int main(int argc, char **argv) {
    { ssize_t n = readlink("/proc/self/exe", self_exe, sizeof(self_exe) - 1); self_exe[n > 0 ? n : 0] = 0;
      if (n > 0 && strstr(self_exe, " (deleted)")) *strstr(self_exe, " (deleted)") = 0; }
    for (int i = 1; i < argc; i++) if (!strcmp(argv[i], "--version")) { printf("%s %s\n", APP_NAME, APP_VERSION); return 0; }
    setup_paths();
    char logp[1200]; snprintf(logp, sizeof(logp), "%s/NXT_Tube_Log.txt", paths.app);
    Log_open(logp);
    Log_msg("%s %s starting, data %s", APP_NAME, APP_VERSION, paths.data);
    int s = Session_start(argc, argv);
    if (s == 1) { Log_msg("handed over to the running instance"); return 0; }
    if (s < 0) return 1;
    signal(SIGTERM, on_signal); signal(SIGINT, on_signal); signal(SIGPIPE, SIG_IGN);
#ifdef NXT_TUBE_PW
    setsid();   /* outlive the launcher's process group when listening in the background */
#endif
    psa_crypto_init();   /* mbedTLS 3.6 needs PSA for TLS 1.3 */
    Settings_load();
    Kids_load();
    Innertube_setSafety(K.enabled);
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS)) { Log_msg("SDL_Init: %s", SDL_GetError()); Session_quit(); return 1; }
    if (!create_display()) { Session_quit(); SDL_Quit(); return 1; }
    open_joystick();
    char res[1200]; snprintf(res, sizeof(res), "%s/res", paths.app);
    if (!D_init(A.ren, res)) { Log_msg("fonts missing in %s", res); }
    Store_init();
    for (int i = 0; i < NQ; i++) if (!strcmp(QUALITY_ID[i], Store_quality())) A.quality = i;
    A.speedIx = 2; A.capture = -1; A.confirmRow = -1;
    A.telexOff = !S.telex;
    if (S.bottom_dac) { AudioRoute_setBottom(true); A.bottomSwitched = true; SDL_Delay(400); }
    Sys_init();
    Sys_stockInit();
#ifdef NXT_TUBE_DEV_TG5050
    A.volExternal = true;          /* Smart Pro S: the firmware's keymon keeps the volume keys */
#endif
#ifdef NXT_TUBE_PW
    A.volExternal = true;          /* the OS mixer reacts to the volume keys itself */
    unsetenv("LD_PRELOAD");        /* muOS preloads its SDL overlay hooks: keep them out of the helpers */
#endif
    Player_init(S.audio_out);
    Sys_applyGain();
    A.lastVol = Sys_volume();
    Player_setAvOffset(S.av_offset_ms);
    A.usbCard = AudioRoute_usbCard(); A.nextUsbPoll = SDL_GetTicks() + 3000;
    Yt_init();
    Ota_init();
    Thumbs_init(A.ren);
    A.screen = SC_HOME; A.keySel = 10;
    load_feed(0, false);
    bool pacing_off = (getenv("NXT_TUBE_KEYS") || getenv("NXT_TUBE_SHOT")) && !getenv("NXT_TUBE_PACE");   /* QA replays count frames */
    Uint32 draws = 0, draws_at = SDL_GetTicks();
    Uint32 last_draw = 0;
    while (!A.quit) {
        if (A.hidden) {
            update();
            if (Session_poll() && !come_back()) { Log_msg("cannot restore display"); break; }
            if (A.hidden) {
                PlayerStatus st; Player_status(&st);
                if (!A.hasCur || st.state == PS_ERROR || (st.state == PS_ENDED && (!S.autoplay || A.qpos + 1 >= A.queue.count)) || st.state == PS_IDLE) {
                    if (!A.resolveJob) { Log_msg("background playback finished"); break; }
                }
                SDL_Delay(50);
            }
            continue;
        }
        poll_input();
        if (A.quit || A.hidden) continue;
        update();
        test_script();
        if (A.screenOff) {
            /* screen off: no drawing at all, sound and controls keep going */
            if (shot_now) { SDL_SetRenderDrawColor(A.ren, 0, 0, 0, 255); SDL_RenderClear(A.ren); save_shot(); shot_now = false; }
            SDL_Delay(40);
            continue;
        }
        /* redraw only when something can have changed: every vsync while you press
         * buttons, at the video's own rate while it plays, slowly otherwise */
        if (!pacing_off) {
            Uint32 now = SDL_GetTicks();
            for (int k = 0; k < K_COUNT; k++) if (held[k]) { last_input = now; break; }
            bool active = now - last_input < 1500;
            PlayerStatus pst; Player_status(&pst);
            bool playing = pst.state == PS_PLAYING && !pst.audio_only;
            bool loading = A.resolveJob || pst.state == PS_BUFFERING || pst.state == PS_LOADING;
            bool due;
            if (active) due = true;
            else if (playing) due = Player_frameDue() || now - last_draw >= 250;
            else due = now - last_draw >= (Uint32)(loading ? 66 : 100);
            if (!due) { SDL_Delay(playing ? 3 : 8); continue; }
            last_draw = now;
        }
        draw();
        draws++;
        if (SDL_GetTicks() - draws_at >= 30000) { Log_msg("ui: %.1f redraws/s", draws * 1000.0 / (SDL_GetTicks() - draws_at)); draws = 0; draws_at = SDL_GetTicks(); }
        if (shot_now) { save_shot(); shot_now = false; }
    }
    Log_msg("quitting");
    Sys_cpuRestore();
    Sys_quit();
    Sys_stockQuit();
    save_history(); Store_save(); Settings_save(); Kids_save();
    Player_close();
    Thumbs_quit();
    Yt_quit();
    Player_quit();
    AudioRoute_restore();
    if (A.bottomSwitched) AudioRoute_setBottom(false);   /* give the charging port back */
    if (!A.hidden) { D_quit(); destroy_display(); }
    Session_quit();
    SDL_Quit();
    if (A.restart && self_exe[0]) {
        /* after an update: start the new program in place (same pid, launch.sh keeps waiting) */
        Log_msg("restarting %s", self_exe);
        execv(self_exe, argv);
        Log_msg("restart failed: %s", strerror(errno));
    }
    return 0;
}
