/* NXT Retro Tube — YouTube viewer for TrimUI Brick Pro.
 * SPDX-License-Identifier: GPL-3.0-only */
#ifndef NXT_TUBE_APP_H
#define NXT_TUBE_APP_H
#include <SDL2/SDL.h>
#include <stdbool.h>
#include <stdint.h>

#define APP_NAME "NXT Retro Tube"
#define APP_VERSION "2.0.1"
#define APP_AUTHOR "Nguyễn Xuân Thông"
#define DONATE_MOMO "0339838267"
#define DONATE_BANK "VCB 0581000786219"
#define DONATE_PAYPAL "thongkhung2@gmail.com"
/* muOS and Knulli: PipeWire audio, the OS owns volume / power / idle, and the
 * pad is read by key code from evdev (button numbering differs per firmware) */
#if defined(NXT_TUBE_ROM_MUOS) || defined(NXT_TUBE_ROM_KNULLI) || defined(NXT_TUBE_ROM_DARKOS)
#define NXT_TUBE_PW 1
#endif
#if defined(NXT_TUBE_ROM_MUOS)
#define ROM_LABEL "muOS"
#elif defined(NXT_TUBE_ROM_KNULLI)
#define ROM_LABEL "Knulli"
#elif defined(NXT_TUBE_ROM_DARKOS)
#define ROM_LABEL "dArkOS"
#elif defined(NXT_TUBE_ROM_STOCK) && defined(NXT_TUBE_DEV_TG5050)
#define ROM_LABEL "Stock · Smart Pro S"
#elif defined(NXT_TUBE_ROM_STOCK)
#define ROM_LABEL "Stock"
#elif defined(NXT_TUBE_ROM_NEXTUI)
#define ROM_LABEL "NextUI"
#else
#define ROM_LABEL "NX Redux"
#endif
#define SCREEN_W 1024
#define SCREEN_H 768

/* ---------- logging ---------- */
void Log_open(const char *path);
void Log_msg(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/* ---------- paths ---------- */
typedef struct {
    char app[1024];      /* pak directory (cwd) */
    char data[1024];     /* persistent data */
    char cache[1024];    /* thumbnails, search cache */
    char bin[1024];      /* bundled helpers */
    char cacert[1100];
} AppPaths;
extern AppPaths paths;

/* ---------- YouTube catalog ---------- */
typedef struct {
    char id[16];
    char title[256];
    char channel[128];
    char channel_id[40];
    int duration;          /* seconds, 0 for live / unknown */
    long long views;
    bool live;
} VideoItem;

typedef struct {
    VideoItem *items;
    int count, capacity;
} VideoList;
void VideoList_clear(VideoList *);
bool VideoList_add(VideoList *, const VideoItem *);
void VideoList_copy(VideoList *dst, const VideoList *src);

typedef struct {
    int height, fps, itag;
    long long size;
    double tbr;
    char codec[16];
    char *url;
} StreamFormat;

#define MAX_FORMATS 12
#define MAX_CAPS 16
typedef struct { char lang[24]; char name[64]; char *url; bool asr, translatable; } CaptionTrack;
typedef struct {
    VideoItem info;
    char description[600];
    char upload_date[16];
    long long likes;
    StreamFormat video[MAX_FORMATS];
    int nvideo;
    StreamFormat audio;       /* audio-only DASH */
    StreamFormat muxed;       /* progressive fallback (itag 18) */
    char *hls;                /* live HLS manifest */
    char *headers;            /* "Key: value\r\n" lines */
    char user_agent[300];
    long long fetched_at;     /* unix seconds */
    CaptionTrack caps[MAX_CAPS];   /* original-language subtitle tracks (json3 timedtext URLs) */
    int ncaps;
} StreamInfo;
void StreamInfo_free(StreamInfo *);

typedef enum { JOB_NONE, JOB_RUNNING, JOB_DONE, JOB_FAILED } JobState;

/* Asynchronous yt-dlp jobs; at most one helper runs at a time. */
void Yt_init(void);
void Yt_quit(void);
int Yt_search(const char *query, int count, bool newest, bool priority);
int Yt_channel(const char *channel_id, int count);
int Yt_searchPage(const char *query, int start, int count, bool newest, bool priority);
int Yt_channelPage(const char *channel_id, int start, int count);
int Yt_resolve(const char *id, bool priority);
JobState Yt_state(int job);
bool Yt_takeList(int job, VideoList *out);          /* moves results */
bool Yt_takeStream(int job, StreamInfo *out);       /* moves results */
int Yt_fetch(const char *url);                      /* GET through yt-dlp (browser TLS), for subtitles */
char *Yt_takeText(int job);                         /* malloc'd body or NULL */
const char *Yt_error(int job);
void Yt_forget(int job);
bool Yt_cachedStream(const char *id, StreamInfo *out); /* fresh resolve cache */
bool Yt_isResolving(const char *id);
bool Yt_hasStream(const char *id);

/* Thumbnails (downloaded in the background, decoded on the UI thread). */
void Thumbs_init(SDL_Renderer *);
void Thumbs_quit(void);
SDL_Texture *Thumbs_get(const char *id);  /* NULL until ready; queues download */
void Thumbs_tick(void);
void Thumbs_releaseTextures(void);
void Thumbs_setRenderer(SDL_Renderer *);

/* ---------- player ---------- */
typedef enum { PS_IDLE, PS_LOADING, PS_BUFFERING, PS_PLAYING, PS_PAUSED, PS_ENDED, PS_ERROR } PlayerStateKind;
typedef struct {
    PlayerStateKind state;
    double position, duration, buffered;   /* seconds; buffered = seconds ahead */
    int width, height;
    double fps;
    char vcodec[24], acodec[24];
    double video_kbps;
    unsigned decoded, dropped;
    int av_offset_ms;
    bool live, audio_only, hw;
    float speed;
    char error[160];
} PlayerStatus;

typedef struct {
    const char *video_url; long long video_size; int video_height; double video_fps; const char *video_codec; double video_kbps;
    const char *audio_url; long long audio_size;
    const char *muxed_url;   /* progressive file or HLS manifest */
    const char *headers, *user_agent;
    bool live, audio_only;
    double start;
    const char *local_video, *local_audio;  /* downloaded files */
    double duration_hint;
} PlayerSource;

bool Player_init(int audio_mode);
bool Player_setAudioMode(int mode);   /* 0 auto, 1 speaker, 2 USB DAC */
bool Player_usbOutput(void);
void Player_quit(void);
bool Player_open(const PlayerSource *);
void Player_close(void);
void Player_togglePause(void);
void Player_setPaused(bool);
void Player_seek(double seconds);
void Player_setSpeed(float);
bool Player_switchVideo(const PlayerSource *); /* keeps audio and position */
void Player_setAvOffset(int ms);
double Player_latencyMs(void);
bool Player_switching(void);
void Player_status(PlayerStatus *);
/* Upload the frame due now into *texture (created/resized as needed). */
bool Player_present(SDL_Renderer *, SDL_Texture **texture, int *w, int *h);
bool Player_frameDue(void);
bool Yt_deferred(const char *id);
bool Player_wantsBoost(void);
void Player_releaseTexture(void);
bool Player_active(void);

/* ---------- local library ---------- */
typedef struct { VideoItem item; double position; long long when; } HistoryEntry;
void Store_init(void);
void Store_save(void);
void Store_addHistory(const VideoItem *, double position, double duration);
double Store_resumePosition(const char *id);
const VideoList *Store_history(void);
double Store_historyProgress(int index);  /* 0..1 */
const VideoList *Store_later(void);
bool Store_toggleLater(const VideoItem *);
bool Store_isLater(const char *id);
const VideoList *Store_channels(void);       /* item.title = channel name */
bool Store_toggleChannel(const VideoItem *);
bool Store_isChannel(const char *channel_id);
const VideoList *Store_downloads(void);
const char *Store_downloadPath(const char *id, bool audio); /* static buffer */
bool Store_hasDownload(const char *id);
void Store_addDownload(const VideoItem *);
const char *Store_quality(void);
void Store_setQuality(const char *);
const VideoList *Store_recentSearches(void);  /* item.title = query */
void Store_addSearch(const char *);
void Store_removeSearch(int index);
void Store_clearHistory(void);
void Store_clearSearches(void);
int Store_recentChannels(char out[][128], int max);
bool Store_saveFeed(const char *key, const VideoList *);
bool Store_loadFeed(const char *key, VideoList *, long long *age_seconds);

/* ---------- downloads ---------- */
int Download_start(const VideoItem *, int max_height);
bool Download_busy(void);
const char *Download_status(void);
void Download_tick(void);

/* ---------- session (background listening) ---------- */
int Session_start(int argc, char **argv); /* 1 = attached to running instance, exit; 0 = continue; <0 error */
void Session_visible(bool);
bool Session_poll(void);
/* audio_route.c */
int AudioRoute_usbCard(void);
const char *AudioRoute_dacName(void);
int AudioRoute_prepare(int mode);
void AudioRoute_restore(void);
int AudioRoute_bottomRole(void);
int AudioRoute_setBottom(bool host);
/* settings.c */
#define KM_COUNT 10
typedef struct { int lang, av_offset_ms, audio_out, home_mode, dac_volume; bool autoplay, prefetch, telex, bottom_dac; int keymap[KM_COUNT];
                 int sub_mode, sub_size; char sub_lang[16]; bool ota_auto; } Settings;
extern Settings S;
void Settings_load(void);
void Settings_save(void);
void Settings_defaultKeys(void);
const char *T(const char *vi);  /* UI text in the chosen language */   /* true when a new launch asked us to come back */
void Session_quit(void);

#endif

/* system.c */
void Sys_init(void);
void Sys_quit(void);
void Sys_tick(Uint32 now);
int Sys_volume(void);
const char *Sys_output(void);
void Sys_setVolume(int level);
void Sys_applyGain(void);
void Sys_setBacklight(bool on);
bool Sys_backlightOff(void);
bool Sys_powerAvailable(void);
bool Sys_powerPressed(void);
void Player_setGain(float g);
const char *Innertube_visitor(void);
void Sys_setGovernor(bool performance);
void Sys_cpuLevel(int level);
bool Sys_keymonActive(void);
/* OTA updates (ota.c) */
#ifndef NXT_TUBE_OTA_H
#define NXT_TUBE_OTA_H
enum { OTA_IDLE, OTA_CHECKING, OTA_UPTODATE, OTA_AVAILABLE, OTA_DOWNLOADING, OTA_READY, OTA_ERROR };
void Ota_init(void); void Ota_tick(Uint32 now); bool Ota_check(bool manual); bool Ota_install(void);
int Ota_state(void); int Ota_progress(void); long Ota_bytes(void);
const char *Ota_latest(void); const char *Ota_notes(void); const char *Ota_message(void);
const char *Ota_channel(void); const char *Ota_device(void);
#endif
void Sys_cpuRestore(void);
void Sys_stockInit(void);
void Sys_stockQuit(void);
void Sys_stockVisible(bool visible);

/* muOS / Knulli integration (system.c) */
void Sys_pwVisible(bool visible);
bool Sys_volumeExternal(void);
void Sys_volumeKey(void);
void Sys_keepAwake(bool on);
/* evdev pad reader (input_evdev.c): physical ids as in the TrimUI layout */
typedef void (*PadCb)(int phys, bool down);
typedef void (*HatCb)(int up, int down, int left, int right);
bool Pad_open(void);
void Pad_poll(PadCb cb, HatCb hat);
void Pad_close(void);

/* kids.c: parental controls */
#ifndef NXT_TUBE_KIDS_H
#define NXT_TUBE_KIDS_H
#define KIDS_CHANNELS 64
#define KIDS_WORDS 64
#define KIDS_DAYS 14
#define KIDS_LOG 200
typedef struct { char day[12]; int secs, extra_min; bool hours_waived; } KidsDay;
typedef struct { char day[12], at[8], id[16], title[160], channel[96]; int secs; } KidsLog;
typedef struct {
    bool enabled, pin_set; unsigned long pin;
    int daily_min;              /* 0 = no limit */
    int from_h, to_h;           /* allowed hours [from, to); equal = always */
    int break_min;              /* eye-break reminder, 0 = off */
    bool approved_only;
    int napproved; struct { char id[48]; char name[128]; } approved[KIDS_CHANNELS];
    int nwords; char words[KIDS_WORDS][48];
    int ndays; KidsDay days[KIDS_DAYS];
    int nlog; KidsLog log[KIDS_LOG];
    int since_break;            /* seconds watched since the last break (not saved) */
} Kids;
extern Kids K;
void Kids_load(void);
void Kids_save(void);
void Kids_changed(void);
void Kids_fold(const char *in, char *out, size_t n);
bool Kids_hasPin(void);
bool Kids_checkPin(const char *pin);
void Kids_setPin(const char *pin);
int Kids_usedToday(void);
int Kids_extraToday(void);
int Kids_leftToday(void);       /* seconds, -1 = no limit */
void Kids_grantExtra(int minutes);
bool Kids_outsideHours(void);
void Kids_waiveHoursToday(void);
int Kids_lockReason(void);      /* 0 ok, 1 time used up, 2 outside hours */
void Kids_tick(const VideoItem *v);
bool Kids_breakDue(void);
void Kids_breakDone(void);
bool Kids_approved(const char *channel_id);
bool Kids_toggleApproved(const char *channel_id, const char *name);
void Kids_removeApproved(int i);
bool Kids_addWord(const char *w);
void Kids_removeWord(int i);
bool Kids_queryBlocked(const char *q);
bool Kids_allowed(const VideoItem *v);
int Kids_filter(VideoList *l);
void Kids_clearReport(void);
void Innertube_setSafety(bool on);
#endif

/* subs.c: subtitles (YouTube captions, auto-translated by YouTube) */
#ifndef NXT_TUBE_SUBS_H
#define NXT_TUBE_SUBS_H
typedef struct { const char *code, *name; int pack; } SubLang;   /* pack: 0 built in, else font pack index */
extern const SubLang SUB_LANGS[];
extern const int SUB_NLANGS;
void Subs_init(void);
void Subs_quit(void);
void Subs_open(const StreamInfo *s, int override);   /* override: 0 settings, 1 translated, 2 original, 3 off */
void Subs_close(void);
const char *Subs_text(double pos);                   /* current cue or NULL */
const char *Subs_status(void);                       /* short label for the player ("Phụ đề: Tiếng Việt (dịch)") */
const char *Subs_target(void);                       /* language code shown */
bool Subs_needsPack(const char *code);
int Subs_packFor(const char *code);
bool Subs_packReady(int pack);
bool Subs_packDownload(int pack);                    /* starts in background */
int Subs_packProgress(int pack);                     /* -1 idle, 0..100 downloading, 101 failed */
const char *Subs_packName(int pack);
int Subs_packMB(int pack);
const char *Subs_fontFor(const char *code, long *index);   /* NULL = app font */
char *Innertube_get(const char *url, char *err, size_t en);
#endif
