/* yt-dlp bridge: search, channel lists and stream resolution.
 * One helper process at a time (yt-dlp + Deno are heavy on 1 GB RAM).
 * SPDX-License-Identifier: GPL-3.0-only */
#include "app.h"
#include "parson.h"
#include <pthread.h>
#include <spawn.h>
#include <signal.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <poll.h>
#include <unistd.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <stdint.h>

extern char **environ;

typedef enum { K_SEARCH, K_CHANNEL, K_RESOLVE, K_FETCH } JobKind;
typedef struct {
    int id;
    JobKind kind;
    bool used, priority;
    JobState state;
    char arg[300];
    int count, start;
    bool newest, need_helper;
    bool skip_visionos;    /* the visionOS client already refused it (e.g. "made for kids") */
    VideoList list;
    StreamInfo stream;
    char *big, *text;      /* K_FETCH: the URL and the body */
    char error[200];
} Job;

#define MAX_JOBS 48
static Job jobs[MAX_JOBS];
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t wake = PTHREAD_COND_INITIALIZER;
static bool running;
static int next_id = 1;

/* resolve cache */
#define CACHE_N 24
static StreamInfo cache[CACHE_N];
static int cache_next;

void VideoList_clear(VideoList *l) { free(l->items); l->items = NULL; l->count = l->capacity = 0; }
bool VideoList_add(VideoList *l, const VideoItem *v) {
    for (int i = 0; i < l->count; i++) if (!strcmp(l->items[i].id, v->id)) return true;
    if (l->count == l->capacity) {
        int cap = l->capacity ? l->capacity * 2 : 32;
        VideoItem *n = realloc(l->items, cap * sizeof(*n));
        if (!n) return false;
        l->items = n; l->capacity = cap;
    }
    l->items[l->count++] = *v;
    return true;
}
void VideoList_copy(VideoList *dst, const VideoList *src) {
    VideoList_clear(dst);
    for (int i = 0; i < src->count; i++) VideoList_add(dst, &src->items[i]);
}

static void free_format(StreamFormat *f) { free(f->url); f->url = NULL; }
void StreamInfo_free(StreamInfo *s) {
    for (int i = 0; i < s->nvideo; i++) free_format(&s->video[i]);
    free_format(&s->audio); free_format(&s->muxed);
    free(s->hls); free(s->headers);
    for (int i = 0; i < s->ncaps; i++) free(s->caps[i].url);
    for (int i = 0; i < s->nhls; i++) { free(s->hlsv[i].url); free(s->hlsv[i].audio); }
    free(s->hls_master);
    memset(s, 0, sizeof(*s));
}
static char *dup0(const char *s) { return s ? strdup(s) : NULL; }
static void copy_stream(StreamInfo *dst, const StreamInfo *src) {
    *dst = *src;
    for (int i = 0; i < src->nvideo; i++) dst->video[i].url = dup0(src->video[i].url);
    dst->audio.url = dup0(src->audio.url); dst->muxed.url = dup0(src->muxed.url);
    dst->hls = dup0(src->hls); dst->headers = dup0(src->headers);
    for (int i = 0; i < src->ncaps; i++) dst->caps[i].url = dup0(src->caps[i].url);
    for (int i = 0; i < src->nhls; i++) { dst->hlsv[i].url = dup0(src->hlsv[i].url); dst->hlsv[i].audio = dup0(src->hlsv[i].audio); }
    dst->hls_master = dup0(src->hls_master);
}
static void hls_add(StreamInfo *s, const char *url, int h, int fps, long bw, bool split, const char *audio) {
    if (!url || h <= 0 || s->nhls >= 10) return;
    for (int i = 0; i < s->nhls; i++) if (s->hlsv[i].height == h && s->hlsv[i].fps == fps) {
        /* same quality twice (e.g. with and without sound): keep the one that has sound */
        if (s->hlsv[i].split && !split) { free(s->hlsv[i].url); free(s->hlsv[i].audio); s->hlsv[i].url = strdup(url); s->hlsv[i].audio = NULL; s->hlsv[i].split = false; s->hlsv[i].bw = bw; }
        return;
    }
    s->hlsv[s->nhls].url = strdup(url); s->hlsv[s->nhls].height = h; s->hlsv[s->nhls].fps = fps; s->hlsv[s->nhls].bw = bw;
    s->hlsv[s->nhls].split = split; s->hlsv[s->nhls].audio = audio && split ? strdup(audio) : NULL; s->nhls++;
}
/* value of KEY= in an m3u8 attribute list (quoted or not) */
static bool m3u8_attr(const char *line, const char *key, char *out, size_t n) {
    out[0] = 0;
    size_t kl = strlen(key);
    for (const char *p = strstr(line, key); p; p = strstr(p + 1, key)) {
        if (p[kl] != '=' || (p > line && p[-1] != ',' && p[-1] != ':')) continue;
        p += kl + 1;
        size_t k = 0;
        if (*p == '"') { p++; while (*p && *p != '"' && k + 1 < n) out[k++] = *p++; }
        else while (*p && *p != ',' && k + 1 < n) out[k++] = *p++;
        out[k] = 0;
        return true;
    }
    return false;
}
/* Live: YouTube's master playlist lists 144p..1080p. Opened as is, FFmpeg's HLS demuxer loads
 * every quality and then picks one by itself (often 240p). Read the list here instead, so the
 * player opens exactly the quality asked for (and only that one: also faster to start). */
char *Innertube_get(const char *url, char *err, size_t en);
static void hls_expand(StreamInfo *s) {
    if (!s->hls || s->nhls) return;
    char err[160] = "";
    char *text = Innertube_get(s->hls, err, sizeof(err));
    if (!text) { Log_msg("live: master playlist: %s", err); return; }
    /* sound kept apart: #EXT-X-MEDIA:TYPE=AUDIO,GROUP-ID="x",URI="..." (the default one of each group) */
    struct { char group[64], uri[1600]; bool def; } aud[8]; int naud = 0;
    char *copy = strdup(text);
    for (char *line = strtok(copy, "\r\n"); line; line = strtok(NULL, "\r\n")) {
        char type[16], gid[64], uri[1600], def[8];
        if (strncmp(line, "#EXT-X-MEDIA:", 13) || !m3u8_attr(line, "TYPE", type, sizeof(type)) || strcmp(type, "AUDIO")) continue;
        if (!m3u8_attr(line, "GROUP-ID", gid, sizeof(gid)) || !m3u8_attr(line, "URI", uri, sizeof(uri)) || strncmp(uri, "http", 4)) continue;
        bool d = m3u8_attr(line, "DEFAULT", def, sizeof(def)) && !strcmp(def, "YES");
        int k = -1; for (int i = 0; i < naud; i++) if (!strcmp(aud[i].group, gid)) k = i;
        if (k < 0 && naud < 8) k = naud++; else if (k < 0 || aud[k].def || !d) continue;
        snprintf(aud[k].group, sizeof(aud[k].group), "%s", gid); snprintf(aud[k].uri, sizeof(aud[k].uri), "%s", uri); aud[k].def = d;
    }
    free(copy);
    int h = 0, fps = 0; long bw = 0; bool want = false, split = false; const char *audio = NULL; int nsplit = 0;
    for (char *line = strtok(text, "\r\n"); line; line = strtok(NULL, "\r\n")) {
        if (!strncmp(line, "#EXT-X-STREAM-INF:", 18)) {
            char v[200];
            int w = 0; h = 0; fps = 30; bw = 0; split = false; audio = NULL;
            if (m3u8_attr(line, "RESOLUTION", v, sizeof(v))) sscanf(v, "%dx%d", &w, &h);
            if (m3u8_attr(line, "FRAME-RATE", v, sizeof(v))) fps = (int)(atof(v) + 0.5);
            if (m3u8_attr(line, "BANDWIDTH", v, sizeof(v))) bw = atol(v);
            if (m3u8_attr(line, "AUDIO", v, sizeof(v)) && v[0]) {
                split = true;
                for (int i = 0; i < naud; i++) if (!strcmp(aud[i].group, v)) audio = aud[i].uri;
            } else if (m3u8_attr(line, "CODECS", v, sizeof(v)) && v[0] && !strstr(v, "mp4a") && !strstr(v, "opus") && !strstr(v, "ac-3") && !strstr(v, "ec-3"))
                split = true;          /* video only, and no sound group named */
            want = h > 0;
        } else if (want && line[0] != '#') {
            hls_add(s, line, h, fps, bw, split, audio); want = false; nsplit += split;
        }
    }
    free(text);
    if (!s->hls_master) s->hls_master = strdup(s->hls);
    Log_msg("live: %d qualities in the master playlist (%d without own sound, %d sound playlists)", s->nhls, nsplit, naud);
}

static void scopy(char *dst, size_t n, const char *src) {
    if (!src) src = "";
    size_t k = strlen(src);
    if (k >= n) { k = n - 1; while (k && ((unsigned char)src[k] & 0xC0) == 0x80) k--; }
    memcpy(dst, src, k); dst[k] = 0;
    for (size_t i = 0; i < k; i++) if (dst[i] == '\n' || dst[i] == '\r' || dst[i] == '\t') dst[i] = ' ';
}

/* ---------- process runner ---------- */
static char **build_env(void) {
    static char pathv[2400], home[1100], xdg[1100], deno[1100], ld[2400];
    const char *old_path = getenv("PATH"), *old_ld = getenv("LD_LIBRARY_PATH");
    snprintf(pathv, sizeof(pathv), "PATH=%s:%s", paths.bin, old_path ? old_path : "/usr/bin:/bin");
    snprintf(home, sizeof(home), "HOME=%s/home", paths.data);
    snprintf(xdg, sizeof(xdg), "XDG_CACHE_HOME=%s/ytdlp", paths.cache);
    snprintf(deno, sizeof(deno), "DENO_DIR=%s/deno", paths.cache);
    snprintf(ld, sizeof(ld), "LD_LIBRARY_PATH=%s", old_ld ? old_ld : "");
    int n = 0; while (environ[n]) n++;
    char **env = calloc(n + 8, sizeof(char *));
    int k = 0;
    for (int i = 0; i < n; i++) {
        const char *e = environ[i];
        if (!strncmp(e, "PATH=", 5) || !strncmp(e, "HOME=", 5) || !strncmp(e, "XDG_CACHE_HOME=", 15) || !strncmp(e, "DENO_DIR=", 9) || !strncmp(e, "LD_LIBRARY_PATH=", 16)) continue;
        env[k++] = (char *)e;
    }
    env[k++] = pathv; env[k++] = home; env[k++] = xdg; env[k++] = deno; env[k++] = ld;
    env[k++] = "PYTHONUNBUFFERED=1"; env[k++] = "NO_COLOR=1";
    env[k] = NULL;
    return env;
}
static void map_error(const char *last, char *error, size_t error_size) {
    if (strstr(last, "Sign in to confirm")) snprintf(error, error_size, "YouTube yêu cầu xác minh (thử lại sau)");
    else if (strstr(last, "Private video")) snprintf(error, error_size, "Video riêng tư");
    else if (strstr(last, "members-only") || strstr(last, "Join this channel")) snprintf(error, error_size, "Video chỉ dành cho hội viên kênh");
    else if (strstr(last, "age") && strstr(last, "confirm")) snprintf(error, error_size, "Video giới hạn độ tuổi");
    else if (strstr(last, "unavailable")) snprintf(error, error_size, "Video không khả dụng");
    else if (strstr(last, "Unable to download") || strstr(last, "resolve") || strstr(last, "Network") || strstr(last, "timed out") || strstr(last, "connect")) snprintf(error, error_size, "Lỗi mạng · kiểm tra Wi-Fi");
    else snprintf(error, error_size, "%.180s", *last ? last : "yt-dlp lỗi");
}

/* One-shot yt-dlp (fallback when the resident helper cannot start).
 * Returns malloc'd stdout or NULL (error filled). */
static char *run_ytdlp(const char *const *extra, int timeout_s, char *error, size_t error_size) {
    char exe[1200], deno[1200], errpath[1200];
    snprintf(exe, sizeof(exe), "%s/yt-dlp-runtime/yt-dlp", paths.bin);
    snprintf(deno, sizeof(deno), "deno:%s/deno", paths.bin);
    snprintf(errpath, sizeof(errpath), "%s/ytdlp-last-error.txt", paths.cache);
    const char *argv[48]; int a = 0;
    argv[a++] = "yt-dlp"; argv[a++] = "--ignore-config"; argv[a++] = "--no-warnings"; argv[a++] = "--no-progress";
    argv[a++] = "--socket-timeout"; argv[a++] = "15";
    argv[a++] = "--js-runtimes"; argv[a++] = deno;
    for (int i = 0; extra[i] && a < 46; i++) argv[a++] = extra[i];
    argv[a] = NULL;
    int out[2];
    if (pipe2(out, O_CLOEXEC)) { snprintf(error, error_size, "pipe"); return NULL; }
    posix_spawn_file_actions_t fa; posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_addopen(&fa, 0, "/dev/null", O_RDONLY, 0);
    posix_spawn_file_actions_adddup2(&fa, out[1], 1);
    posix_spawn_file_actions_addopen(&fa, 2, errpath, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    char **env = build_env();
    pid_t pid = -1;
    int rc = posix_spawn(&pid, exe, &fa, NULL, (char *const *)argv, env);
    posix_spawn_file_actions_destroy(&fa); free(env);
    close(out[1]);
    if (rc) { close(out[0]); snprintf(error, error_size, "yt-dlp missing (%d)", rc); return NULL; }
    size_t cap = 1 << 16, used = 0; char *buf = malloc(cap);
    time_t start = time(NULL); bool killed = false;
    for (;;) {
        struct pollfd p = {out[0], POLLIN, 0};
        int r = poll(&p, 1, 500);
        if (r > 0) {
            if (used + 65536 + 1 > cap) {
                if (cap >= (16u << 20)) { kill(pid, SIGKILL); killed = true; break; }
                cap *= 2; char *n = realloc(buf, cap); if (!n) { kill(pid, SIGKILL); killed = true; break; } buf = n;
            }
            ssize_t got = read(out[0], buf + used, cap - used - 1);
            if (got > 0) { used += got; continue; }
            if (got == 0) break;
            if (errno == EINTR) continue;
            break;
        }
        if (!running || time(NULL) - start > timeout_s) { kill(pid, SIGKILL); killed = true; break; }
    }
    close(out[0]);
    int status = 0; while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
    buf[used] = 0;
    if (killed || !WIFEXITED(status) || WEXITSTATUS(status) != 0 || !used) {
        char line[400] = "", last[400] = "";
        FILE *f = fopen(errpath, "r");
        if (f) { while (fgets(line, sizeof(line), f)) if (strstr(line, "ERROR") || !*last) scopy(last, sizeof(last), line); fclose(f); }
        if (killed) snprintf(error, error_size, "Quá thời gian chờ YouTube");
        else map_error(last, error, error_size);
        Log_msg("yt-dlp failed (exit %d): %s", WIFEXITED(status) ? WEXITSTATUS(status) : -1, last);
        if (used && WIFEXITED(status) && WEXITSTATUS(status) == 1 && buf[0] == '{') return buf; /* partial but usable */
        free(buf); return NULL;
    }
    return buf;
}

/* ---------- resident helper (yt-dlp plugin "nxttube:serve") ----------
 * Python + yt-dlp start once and stay warm; each request is one JSON line. */
#define NSERVERS 2
typedef struct { pid_t pid; int in, out; char *buf; size_t len, cap; int failures; } Server;
static Server servers[NSERVERS];
static bool helper_disabled;

static void server_stop(Server *sv) {
    if (sv->pid > 0) {
        if (sv->in >= 0) close(sv->in);
        int st; bool gone = false;
        for (int i = 0; i < 20 && !gone; i++) { if (waitpid(sv->pid, &st, WNOHANG) == sv->pid) gone = true; else usleep(50000); }
        if (!gone) { kill(sv->pid, SIGKILL); waitpid(sv->pid, &st, 0); }
        if (sv->out >= 0) close(sv->out);
    }
    sv->pid = 0; sv->in = sv->out = -1; sv->len = 0;
}
/* Reads one "@@NXT " line; returns malloc'd payload or NULL on timeout/exit. */
static char *server_line(Server *sv, int timeout_ms) {
    Uint32 until = SDL_GetTicks() + timeout_ms;
    for (;;) {
        char *nl;
        while ((nl = memchr(sv->buf, '\n', sv->len))) {
            size_t n = nl - sv->buf;
            char *line = NULL;
            if (n >= 6 && !memcmp(sv->buf, "@@NXT ", 6)) { line = malloc(n - 5); memcpy(line, sv->buf + 6, n - 6); line[n - 6] = 0; }
            memmove(sv->buf, nl + 1, sv->len - n - 1); sv->len -= n + 1;
            if (line) return line;
        }
        if (!running) return NULL;
        int left = (int)(until - SDL_GetTicks());
        if (left <= 0) return NULL;
        struct pollfd p = {sv->out, POLLIN, 0};
        int r = poll(&p, 1, left > 300 ? 300 : left);
        if (r <= 0) continue;
        if (sv->len + 65536 + 1 > sv->cap) {
            if (sv->cap >= (24u << 20)) return NULL;
            size_t cap = sv->cap ? sv->cap * 2 : (256 << 10);
            char *b = realloc(sv->buf, cap); if (!b) return NULL;
            sv->buf = b; sv->cap = cap;
        }
        ssize_t got = read(sv->out, sv->buf + sv->len, sv->cap - sv->len - 1);
        if (got > 0) sv->len += got;
        else if (got == 0 || (errno != EINTR && errno != EAGAIN)) return NULL;
    }
}
static bool server_start(Server *sv, int index) {
    char exe[1200], deno[1200], plugins[1200], errpath[1200];
    snprintf(exe, sizeof(exe), "%s/yt-dlp-runtime/yt-dlp", paths.bin);
    snprintf(deno, sizeof(deno), "deno:%s/deno", paths.bin);
    snprintf(plugins, sizeof(plugins), "%s/yt-dlp-plugins", paths.bin);
    snprintf(errpath, sizeof(errpath), "%s/ytdlp-helper-%d.txt", paths.cache, index);
    const char *argv[] = {"yt-dlp", "--ignore-config", "--quiet", "--no-warnings", "--no-progress", "--socket-timeout", "15",
                          "--js-runtimes", deno, "--plugin-dirs", plugins, "nxttube:serve", NULL};
    int in[2], out[2];
    if (pipe2(in, O_CLOEXEC)) return false;
    if (pipe2(out, O_CLOEXEC)) { close(in[0]); close(in[1]); return false; }
    posix_spawn_file_actions_t fa; posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_adddup2(&fa, in[0], 0);
    posix_spawn_file_actions_adddup2(&fa, out[1], 1);
    posix_spawn_file_actions_addopen(&fa, 2, errpath, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    char **env = build_env();
    Uint32 t0 = SDL_GetTicks();
    int rc = posix_spawn(&sv->pid, exe, &fa, NULL, (char *const *)argv, env);
    posix_spawn_file_actions_destroy(&fa); free(env);
    close(in[0]); close(out[1]);
    if (rc) { close(in[1]); close(out[0]); sv->pid = 0; return false; }
    sv->in = in[1]; sv->out = out[0]; sv->len = 0;
    char *hello = server_line(sv, 60000);
    bool ok = hello && strstr(hello, "\"ready\"");
    free(hello);
    if (!ok) { Log_msg("helper %d did not start", index); server_stop(sv); return false; }
    Log_msg("helper %d ready in %.1fs", index, (SDL_GetTicks() - t0) / 1000.0);
    return true;
}
/* Sends a request; returns the parsed reply root (caller frees) or NULL. */
static JSON_Value *server_call(Server *sv, int index, JSON_Value *req, int timeout_s, char *error, size_t error_size) {
    if (sv->pid <= 0 && !server_start(sv, index)) {
        if (++sv->failures >= 2) { helper_disabled = true; Log_msg("helper disabled, using one-shot yt-dlp"); }
        snprintf(error, error_size, "Không khởi động được yt-dlp");
        return NULL;
    }
    char *line = json_serialize_to_string(req);
    size_t n = strlen(line);
    line = realloc(line, n + 2); line[n] = '\n'; line[n + 1] = 0;
    bool sent = true;
    for (size_t off = 0; off < n + 1; ) {
        ssize_t w = write(sv->in, line + off, n + 1 - off);
        if (w > 0) off += w; else if (errno != EINTR) { sent = false; break; }
    }
    free(line);
    char *reply = sent ? server_line(sv, timeout_s * 1000) : NULL;
    if (!reply) {
        Log_msg("helper %d %s, restarting", index, sent ? "timed out or exited" : "pipe closed");
        server_stop(sv);
        snprintf(error, error_size, running ? "Quá thời gian chờ YouTube" : "Đang thoát");
        return NULL;
    }
    sv->failures = 0;
    JSON_Value *root = json_parse_string(reply);
    free(reply);
    JSON_Object *o = json_value_get_object(root);
    if (!o) { json_value_free(root); snprintf(error, error_size, "Trả lời lạ từ yt-dlp"); return NULL; }
    if (json_object_get_boolean(o, "ok") != 1) {
        const char *e = json_object_get_string(o, "error");
        map_error(e ? e : "", error, error_size);
        Log_msg("helper error: %.300s", e ? e : "?");
        json_value_free(root);
        return NULL;
    }
    return root;
}

/* ---------- parsing ---------- */
static void parse_entry(JSON_Object *e, VideoItem *v) {
    memset(v, 0, sizeof(*v));
    scopy(v->id, sizeof(v->id), json_object_get_string(e, "id"));
    scopy(v->title, sizeof(v->title), json_object_get_string(e, "title"));
    const char *ch = json_object_get_string(e, "channel"); if (!ch) ch = json_object_get_string(e, "uploader");
    scopy(v->channel, sizeof(v->channel), ch);
    const char *cid = json_object_get_string(e, "channel_id"); if (!cid) cid = json_object_get_string(e, "uploader_id");
    scopy(v->channel_id, sizeof(v->channel_id), cid);
    v->duration = (int)json_object_get_number(e, "duration");
    v->views = (long long)json_object_get_number(e, "view_count");
    const char *ls = json_object_get_string(e, "live_status");
    v->live = json_object_get_boolean(e, "is_live") == 1 || (ls && !strcmp(ls, "is_live"));
}
static bool parse_list_obj(JSON_Object *root_obj, VideoList *out) {
    JSON_Array *entries = json_object_get_array(root_obj, "entries");
    if (!entries) return false;
    for (size_t i = 0; i < json_array_get_count(entries); i++) {
        JSON_Object *e = json_array_get_object(entries, i);
        if (!e) continue;
        const char *type = json_object_get_string(e, "_type"), *ie = json_object_get_string(e, "ie_key");
        if (type && strcmp(type, "url")) continue;
        if (ie && strcmp(ie, "Youtube")) continue; /* skip channels/playlists/shorts tabs */
        VideoItem v; parse_entry(e, &v);
        if (strlen(v.id) != 11) continue;
        VideoList_add(out, &v);
    }
    return true;
}
static bool parse_list(const char *text, VideoList *out) {
    JSON_Value *root = json_parse_string(text);
    bool ok = parse_list_obj(json_value_get_object(root), out);
    json_value_free(root);
    return ok;
}
static bool starts(const char *s, const char *p) { return s && !strncmp(s, p, strlen(p)); }
static void set_format(StreamFormat *f, JSON_Object *o) {
    free(f->url);
    f->url = strdup(json_object_get_string(o, "url"));
    f->height = (int)json_object_get_number(o, "height");
    f->fps = (int)json_object_get_number(o, "fps");
    f->tbr = json_object_get_number(o, "tbr");
    f->size = (long long)json_object_get_number(o, "filesize");
    if (!f->size) f->size = (long long)json_object_get_number(o, "filesize_approx");
    f->itag = atoi(json_object_get_string(o, "format_id") ? json_object_get_string(o, "format_id") : "0");
    const char *vc = json_object_get_string(o, "vcodec");
    const char *ac = json_object_get_string(o, "acodec");
    const char *c = vc && strcmp(vc, "none") ? vc : ac;
    scopy(f->codec, sizeof(f->codec), c);
    char *dot = strchr(f->codec, '.'); if (dot) *dot = 0;
}
static bool parse_stream_obj(JSON_Object *o, StreamInfo *s) {
    if (!o) return false;
    memset(s, 0, sizeof(*s));
    parse_entry(o, &s->info);
    s->likes = (long long)json_object_get_number(o, "like_count");
    scopy(s->upload_date, sizeof(s->upload_date), json_object_get_string(o, "upload_date"));
    scopy(s->description, sizeof(s->description), json_object_get_string(o, "description"));
    JSON_Array *formats = json_object_get_array(o, "formats");
    int best_audio_score = -1, best_hls_height = -1;
    JSON_Object *header_src = NULL;
    for (size_t i = 0; formats && i < json_array_get_count(formats); i++) {
        JSON_Object *f = json_array_get_object(formats, i);
        const char *url = json_object_get_string(f, "url"), *proto = json_object_get_string(f, "protocol");
        const char *vc = json_object_get_string(f, "vcodec"), *ac = json_object_get_string(f, "acodec");
        if (!url || !proto) continue;
        bool hasv = vc && strcmp(vc, "none"), hasa = ac && strcmp(ac, "none");
        int h = (int)json_object_get_number(f, "height");
        if (starts(proto, "m3u8")) {
            const char *fid = json_object_get_string(f, "format_id");
            if (hasv && !(fid && !strcmp(fid, "hls"))) {
                hls_add(s, url, h, (int)(json_object_get_number(f, "fps") + 0.5), (long)(json_object_get_number(f, "tbr") * 1000), !hasa, NULL);
                const char *mu = json_object_get_string(f, "manifest_url");
                if (mu && !s->hls_master) s->hls_master = strdup(mu);
            }
            if (fid && !strcmp(fid, "hls") && !s->hls_master) s->hls_master = strdup(url);
            if (hasv && hasa && h <= 720 && h > best_hls_height) { free(s->hls); s->hls = strdup(url); best_hls_height = h; header_src = f; }
            continue;
        }
        if (strcmp(proto, "https")) continue;
        if (hasv && !hasa) {
            bool avc = starts(vc, "avc1"), vp9 = starts(vc, "vp09") || !strcmp(vc, "vp9");
            if ((!avc && !vp9) || h <= 0 || h > 1080) continue;
            int fps = (int)json_object_get_number(f, "fps");
            int slot = -1;
            for (int k = 0; k < s->nvideo; k++) if (s->video[k].height == h && (s->video[k].fps > 30) == (fps > 30)) slot = k;
            if (slot >= 0) {
                bool cur_avc = starts(s->video[slot].codec, "avc1");
                if (cur_avc && !avc) continue;            /* prefer H.264 at the same size */
                if (cur_avc == avc && json_object_get_number(f, "tbr") <= s->video[slot].tbr) continue;
            } else if (s->nvideo < MAX_FORMATS) slot = s->nvideo++;
            else continue;
            set_format(&s->video[slot], f); header_src = f;
        } else if (hasa && !hasv) {
            int score = starts(ac, "mp4a") ? 1000 : starts(ac, "opus") ? 500 : 0;
            double abr = json_object_get_number(f, "abr");
            if (abr > 0 && abr <= 160) score += (int)abr;
            const char *note = json_object_get_string(f, "format_note");
            if (note && strstr(note, "original")) score += 2000;   /* original language track */
            if (json_object_get_string(f, "language") && !strstr(json_object_get_string(f, "format_id"), "-") ) score += 1;
            if (score > best_audio_score) { best_audio_score = score; set_format(&s->audio, f); header_src = f; }
        } else if (hasa && hasv) {
            if (!s->muxed.url || h > s->muxed.height) { set_format(&s->muxed, f); if (!header_src) header_src = f; }
        }
    }
    /* sort video formats: height desc, fps desc */
    for (int i = 0; i < s->nvideo; i++) for (int j = i + 1; j < s->nvideo; j++) {
        StreamFormat *a = &s->video[i], *b = &s->video[j];
        if (b->height > a->height || (b->height == a->height && b->fps > a->fps)) { StreamFormat t = *a; *a = *b; *b = t; }
    }
    JSON_Object *hdr = header_src ? json_object_get_object(header_src, "http_headers") : json_object_get_object(o, "http_headers");
    char headers[2048] = "";
    for (size_t i = 0; hdr && i < json_object_get_count(hdr); i++) {
        const char *k = json_object_get_name(hdr, i), *v = json_string(json_object_get_value_at(hdr, i));
        if (!k || !v) continue;
        if (!strcasecmp(k, "User-Agent")) { scopy(s->user_agent, sizeof(s->user_agent), v); continue; }
        size_t len = strlen(headers);
        snprintf(headers + len, sizeof(headers) - len, "%s: %s\r\n", k, v);
    }
    s->headers = strdup(headers);
    s->fetched_at = time(NULL);
    JSON_Array *caps = json_object_get_array(o, "nxt_captions");
    for (size_t i = 0; caps && i < json_array_get_count(caps) && s->ncaps < MAX_CAPS; i++) {
        JSON_Object *c = json_array_get_object(caps, i);
        const char *url = json_object_get_string(c, "url"), *lang = json_object_get_string(c, "lang");
        if (!url || !lang) continue;
        CaptionTrack *t = &s->caps[s->ncaps++];
        scopy(t->lang, sizeof(t->lang), lang);
        scopy(t->name, sizeof(t->name), json_object_get_string(c, "name") ? json_object_get_string(c, "name") : lang);
        t->url = strdup(url);
        t->asr = json_object_get_boolean(c, "asr") == 1;
        t->translatable = json_object_get_boolean(c, "translatable") != 0;
    }
    return s->nvideo > 0 || s->muxed.url || s->hls || s->hls_master || s->nhls || s->audio.url;
}
static bool parse_stream(const char *text, StreamInfo *s) {
    JSON_Value *root = json_parse_string(text);
    bool ok = parse_stream_obj(json_value_get_object(root), s);
    json_value_free(root);
    return ok;
}

/* ---------- workers ---------- */
#define NFAST 2
static pthread_t workers[NSERVERS], fast[NFAST];
static int bg_running, bg_fast;   /* background (prefetch) jobs in progress */
static char deferred[32][16]; static int deferred_next;   /* prefetches left for later (JS player needed) */
/* "made for kids" videos: the client that last worked is tried first, and once the
 * helper's resident JS solver answers quickly, they are prefetched like other videos */
static char kids_best[24]; static volatile bool kids_fast;
static Uint32 native_off_until; static int native_fails;
static bool native_ok(void) { return !native_off_until || (Sint32)(SDL_GetTicks() - native_off_until) >= 0; }
static Job *find(int id) {
    for (int i = 0; i < MAX_JOBS; i++) if (jobs[i].used && jobs[i].id == id) return &jobs[i];
    return NULL;
}
/* fast lanes take resolves and searches (direct API); helper lanes take channel
 * lists and whatever the direct API could not answer */
static bool for_fast(const Job *j) { return !j->need_helper && j->kind != K_CHANNEL && j->kind != K_FETCH && native_ok(); }
static Job *next_job(bool fast_lane) {
    Job *best = NULL;
    for (int i = 0; i < MAX_JOBS; i++) {
        Job *j = &jobs[i];
        if (!j->used || j->state != JOB_NONE) continue;
        if (for_fast(j) != fast_lane) continue;
        if (!j->priority && (fast_lane ? bg_fast : bg_running) > 0) continue;   /* at most one prefetch at a time */
        if (!best || (j->priority && !best->priority) || (j->priority == best->priority && (j->priority ? j->id > best->id : j->id < best->id))) best = j;
    }
    return best;
}
static bool do_list(int w, const Job *job, VideoList *list, char *error, size_t es) {
    char target[400], count[16];
    snprintf(count, sizeof(count), "%d", job->count);
    if (!helper_disabled) {
        JSON_Value *req = json_value_init_object(); JSON_Object *r = json_value_get_object(req);
        json_object_set_string(r, "op", job->kind == K_SEARCH ? "search" : "channel");
        json_object_set_string(r, job->kind == K_SEARCH ? "q" : "id", job->arg);
        json_object_set_number(r, "n", job->count);
        json_object_set_number(r, "start", job->start);
        json_object_set_boolean(r, "newest", job->newest);
        JSON_Value *root = server_call(&servers[w], w, req, 45, error, es);
        json_value_free(req);
        if (root) {
            bool ok = parse_list_obj(json_object_get_object(json_value_get_object(root), "data"), list);
            json_value_free(root);
            return ok;
        }
        if (!helper_disabled) return false;
    }
    char first[16]; snprintf(first, sizeof(first), "%d", job->start + 1);
    snprintf(count, sizeof(count), "%d", job->start + job->count);
    if (job->kind == K_SEARCH) snprintf(target, sizeof(target), "%s%d:%s", job->newest ? "ytsearchdate" : "ytsearch", job->start + job->count, job->arg);
    else snprintf(target, sizeof(target), "https://www.youtube.com/channel/%s/videos", job->arg);
    const char *extra[] = {"--flat-playlist", "-J", "--playlist-start", first, "--playlist-end", count, target, NULL};
    char *text = run_ytdlp(extra, 60, error, es);
    bool ok = text && parse_list(text, list); free(text);
    return ok;
}
char *Innertube_player(const char *video_id, char *err, size_t en);
static bool do_resolve(int w, const Job *job, StreamInfo *stream, char *error, size_t es) {
    if (!helper_disabled) {
        JSON_Value *req = json_value_init_object(); JSON_Object *r = json_value_get_object(req);
        json_object_set_string(r, "op", "resolve");
        json_object_set_string(r, "id", job->arg);
        const char *vis = Innertube_visitor();
        if (vis && *vis) json_object_set_string(r, "visitor_data", vis);
        /* visionOS cannot play "made for kids" videos (cartoons etc.): those go straight to the
         * TV / embedded clients, which need the JS player (solved with Deno) */
        JSON_Value *cl = json_value_init_array(); JSON_Array *ca = json_value_get_array(cl);
        static const char *const JS_CLIENTS[] = {"tv_samsung", "web_embedded", "tv", "tv_downgraded"};
        char best[24]; pthread_mutex_lock(&lock); scopy(best, sizeof(best), kids_best); pthread_mutex_unlock(&lock);
        if (!job->skip_visionos) json_array_append_string(ca, "visionos");
        if (best[0]) json_array_append_string(ca, best);
        for (int i = 0; i < 4; i++) if (strcmp(best, JS_CLIENTS[i])) json_array_append_string(ca, JS_CLIENTS[i]);
        json_array_append_string(ca, "default");
        if (job->skip_visionos) json_array_append_string(ca, "visionos");
        json_array_append_string(ca, "visionos-page");
        json_array_append_string(ca, "web_safari");
        json_object_set_value(r, "clients", cl);
        Uint32 t0 = SDL_GetTicks();
        JSON_Value *root = server_call(&servers[w], w, req, 150, error, es);   /* JS clients are slow on the A133 */
        json_value_free(req);
        if (root) {
            JSON_Object *data = json_object_get_object(json_value_get_object(root), "data");
            const char *client = json_object_get_string(data, "nxt_client");
            bool ok = parse_stream_obj(data, stream);
            if (ok) {
                Uint32 took = SDL_GetTicks() - t0;
                Log_msg("resolved via %s in %.1fs", client ? client : "?", took / 1000.0);
                if (client && strncmp(client, "visionos", 8) && strcmp(client, "default")) {
                    pthread_mutex_lock(&lock); scopy(kids_best, sizeof(kids_best), client); pthread_mutex_unlock(&lock);
                    kids_fast = took < 12000;
                }
            }
            else snprintf(error, es, "Video không có định dạng phát được");
            json_value_free(root);
            return ok;
        }
        if (!helper_disabled) return false;
    }
    char target[200];
    snprintf(target, sizeof(target), "https://www.youtube.com/watch?v=%s", job->arg);
    const char *fast[] = {"-J", "--no-playlist", "--extractor-args", "youtube:player_client=visionos;player_skip=webpage,configs", target, NULL};
    char *text = run_ytdlp(fast, 60, error, es);
    bool ok = text && parse_stream(text, stream); free(text);
    if (!ok) {
        memset(stream, 0, sizeof(*stream));
        const char *kids[] = {"-J", "--no-playlist", "--extractor-args", "youtube:player_client=tv_samsung,web_embedded,tv", target, NULL};
        text = run_ytdlp(kids, 90, error, es);
        ok = text && parse_stream(text, stream); free(text);
    }
    if (!ok) {
        memset(stream, 0, sizeof(*stream));
        const char *full[] = {"-J", "--no-playlist", target, NULL};
        text = run_ytdlp(full, 90, error, es);
        ok = text && parse_stream(text, stream); free(text);
    }
    return ok;
}
static void *work(void *arg) {
    int w = (int)(intptr_t)arg;
    if (w == 0) {
        /* warm the helper only after the app has settled, so it does not slow the first video */
        pthread_mutex_lock(&lock);
        struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts); ts.tv_sec += 8;
        while (running && !next_job(false)) if (pthread_cond_timedwait(&wake, &lock, &ts)) break;
        bool r = running; pthread_mutex_unlock(&lock);
        if (r && !helper_disabled) server_start(&servers[0], 0);
    }
    pthread_mutex_lock(&lock);
    while (running) {
        Job *j = next_job(false);
        if (!j) { pthread_cond_wait(&wake, &lock); continue; }
        j->state = JOB_RUNNING;
        bool bg = !j->priority;
        if (bg) bg_running++;
        Job copy = *j;
        pthread_mutex_unlock(&lock);
        char error[200] = "";
        VideoList list = {0};
        StreamInfo stream; memset(&stream, 0, sizeof(stream));
        bool ok;
        Uint32 t0 = SDL_GetTicks();
        char *text = NULL;
        if (copy.kind == K_FETCH) {
            ok = false;
            if (!helper_disabled && copy.big) {
                JSON_Value *req = json_value_init_object(); JSON_Object *r = json_value_get_object(req);
                json_object_set_string(r, "op", "fetch");
                json_object_set_string(r, "url", copy.big);
                JSON_Value *root = server_call(&servers[w], w, req, 40, error, sizeof(error));
                json_value_free(req);
                const char *t = root ? json_object_dotget_string(json_value_get_object(root), "data.text") : NULL;
                if (t && *t) { text = strdup(t); ok = true; }
                json_value_free(root);
            }
            if (!ok && !*error) snprintf(error, sizeof(error), "no subtitles");
        } else if (copy.kind == K_RESOLVE) {
            ok = do_resolve(w, &copy, &stream, error, sizeof(error));
            Log_msg("resolve %s %s in %.1fs (%s): %d video formats, audio %s, hls %s", copy.arg, ok ? "ok" : "failed", (SDL_GetTicks() - t0) / 1000.0, bg ? "prefetch" : "now", stream.nvideo, stream.audio.url ? stream.audio.codec : "-", stream.hls ? "yes" : "no");
            if (ok) {
                pthread_mutex_lock(&lock);
                StreamInfo_free(&cache[cache_next]); copy_stream(&cache[cache_next], &stream); cache_next = (cache_next + 1) % CACHE_N;
                pthread_mutex_unlock(&lock);
            }
        } else {
            ok = do_list(w, &copy, &list, error, sizeof(error));
            Log_msg("%s \"%s\" %s in %.1fs: %d items", copy.kind == K_SEARCH ? "search" : "channel", copy.arg, ok ? "ok" : "failed", (SDL_GetTicks() - t0) / 1000.0, list.count);
        }
        if (!ok && !*error) snprintf(error, sizeof(error), "Không đọc được dữ liệu YouTube");
        pthread_mutex_lock(&lock);
        if (bg) bg_running--;
        j = find(copy.id);
        if (j) {
            j->list = list; j->stream = stream; free(j->text); j->text = text;
            j->state = ok ? JOB_DONE : JOB_FAILED;
            scopy(j->error, sizeof(j->error), error);
        } else { VideoList_clear(&list); StreamInfo_free(&stream); free(text); }
        pthread_cond_broadcast(&wake);
    }
    pthread_mutex_unlock(&lock);
    server_stop(&servers[w]);
    return NULL;
}

char *Innertube_player(const char *video_id, char *err, size_t en);
bool Innertube_search(const char *q, bool newest, int start, int count, VideoList *out, char *err, size_t en);
bool Innertube_warmup(bool force);
static void *fast_work(void *arg) {
    int lane = (int)(intptr_t)arg;
    if (lane == 0) Innertube_warmup(false);
    pthread_mutex_lock(&lock);
    while (running) {
        Job *j = next_job(true);
        if (!j) { pthread_cond_wait(&wake, &lock); continue; }
        j->state = JOB_RUNNING;
        bool bg = !j->priority;
        if (bg) bg_fast++;
        Job copy = *j;
        pthread_mutex_unlock(&lock);
        char why[200] = "";
        VideoList list = {0};
        StreamInfo stream; memset(&stream, 0, sizeof(stream));
        bool ok = false;
        Uint32 t0 = SDL_GetTicks();
        if (copy.kind == K_RESOLVE) {
            char *text = Innertube_player(copy.arg, why, sizeof(why));
            if (text) {
                ok = parse_stream(text, &stream); free(text);
                ok = ok && stream.audio.url && (stream.nvideo || stream.muxed.url || stream.hls || stream.nhls);
                if (ok && stream.info.live) hls_expand(&stream);
                if (!ok) { StreamInfo_free(&stream); memset(&stream, 0, sizeof(stream)); snprintf(why, sizeof(why), "incomplete formats"); }
            }
            if (ok) {
                Log_msg("resolve %s direct in %.2fs (%s): %d video formats", copy.arg, (SDL_GetTicks() - t0) / 1000.0, bg ? "prefetch" : "now", stream.nvideo);
                pthread_mutex_lock(&lock);
                StreamInfo_free(&cache[cache_next]); copy_stream(&cache[cache_next], &stream); cache_next = (cache_next + 1) % CACHE_N;
                pthread_mutex_unlock(&lock);
            }
        } else {
            ok = Innertube_search(copy.arg, copy.newest, copy.start, copy.count, &list, why, sizeof(why));
            if (ok) Log_msg("search \"%s\" direct in %.2fs: %d items", copy.arg, (SDL_GetTicks() - t0) / 1000.0, list.count);
        }
        pthread_mutex_lock(&lock);
        if (bg) bg_fast--;
        j = find(copy.id);
        if (ok) {
            native_fails = 0;
            if (j) { j->list = list; j->stream = stream; j->state = JOB_DONE; }
            else { VideoList_clear(&list); StreamInfo_free(&stream); }
        } else {
            VideoList_clear(&list);
            /* hand it to yt-dlp; an unplayable video says so there too */
            Log_msg("%s %s: direct API failed (%s), using yt-dlp", copy.kind == K_RESOLVE ? "resolve" : "search", copy.arg, why);
            bool hard = strstr(why, "LOGIN_REQUIRED") || strstr(why, "no results") || strstr(why, "bad JSON");
            if (hard && ++native_fails >= 4) { native_off_until = SDL_GetTicks() + 10 * 60 * 1000; native_fails = 0; Log_msg("direct API paused for 10 min"); }
            if (j) {
                j->need_helper = true; j->state = JOB_NONE;
                /* refused by status (UNPLAYABLE / ERROR / no formats), not for want of a visitor id */
                if (copy.kind == K_RESOLVE && *why && !strstr(why, "LOGIN_REQUIRED") && !strstr(why, "bad JSON") && !strstr(why, "HTTP") && !strstr(why, "timed out")) j->skip_visionos = true;
                /* a prefetch that needs the JS player (Deno) costs a lot of CPU and heat:
                 * leave it for when the video is actually opened */
                if (j->skip_visionos && bg && !kids_fast) {
                    j->state = JOB_FAILED; scopy(j->error, sizeof(j->error), "deferred");
                    scopy(deferred[deferred_next], sizeof(deferred[0]), copy.arg); deferred_next = (deferred_next + 1) % 32;
                }
            }
        }
        pthread_cond_broadcast(&wake);
    }
    pthread_mutex_unlock(&lock);
    return NULL;
}
void Yt_init(void) {
    running = true;
    for (int i = 0; i < NSERVERS; i++) { servers[i].in = servers[i].out = -1; pthread_create(&workers[i], NULL, work, (void *)(intptr_t)i); }
    for (int i = 0; i < NFAST; i++) pthread_create(&fast[i], NULL, fast_work, (void *)(intptr_t)i);
}
void Yt_quit(void) {
    pthread_mutex_lock(&lock); running = false; pthread_cond_broadcast(&wake); pthread_mutex_unlock(&lock);
    for (int i = 0; i < NSERVERS; i++) pthread_join(workers[i], NULL);
    for (int i = 0; i < NFAST; i++) pthread_join(fast[i], NULL);
}
bool Yt_hasStream(const char *id) {
    bool ok = false;
    pthread_mutex_lock(&lock);
    for (int i = 0; i < CACHE_N; i++) if (cache[i].fetched_at && !strcmp(cache[i].info.id, id) && time(NULL) - cache[i].fetched_at < 4 * 3600) ok = true;
    pthread_mutex_unlock(&lock);
    return ok;
}
bool Yt_isResolving(const char *id) {
    bool r = false;
    pthread_mutex_lock(&lock);
    for (int i = 0; i < MAX_JOBS; i++) if (jobs[i].used && jobs[i].kind == K_RESOLVE && !strcmp(jobs[i].arg, id) && (jobs[i].state == JOB_NONE || jobs[i].state == JOB_RUNNING)) r = true;
    pthread_mutex_unlock(&lock);
    return r;
}
static int add_job2(JobKind kind, const char *arg, int start, int count, bool newest, bool priority);
static int add_job(JobKind kind, const char *arg, int count, bool newest, bool priority) { return add_job2(kind, arg, 0, count, newest, priority); }
static int add_job3(JobKind kind, const char *arg, int start, int count, bool newest, bool priority, const char *big);
static int add_job2(JobKind kind, const char *arg, int start, int count, bool newest, bool priority) { return add_job3(kind, arg, start, count, newest, priority, NULL); }
static int add_job3(JobKind kind, const char *arg, int start, int count, bool newest, bool priority, const char *big) {
    pthread_mutex_lock(&lock);
    /* reuse a pending identical job */
    for (int i = 0; i < MAX_JOBS; i++) {
        Job *j = &jobs[i];
        if (j->used && j->kind == kind && kind != K_FETCH && !strcmp(j->arg, arg) && j->newest == newest && j->start == start && (j->state == JOB_NONE || j->state == JOB_RUNNING)) {
            if (priority) { j->priority = true; pthread_cond_broadcast(&wake); }
            int id = j->id; pthread_mutex_unlock(&lock); return id;
        }
    }
    Job *slot = NULL;
    for (int i = 0; i < MAX_JOBS && !slot; i++) if (!jobs[i].used) slot = &jobs[i];
    if (!slot) { /* evict the oldest finished job */
        for (int i = 0; i < MAX_JOBS; i++) if (jobs[i].state == JOB_DONE || jobs[i].state == JOB_FAILED) if (!slot || jobs[i].id < slot->id) slot = &jobs[i];
        if (slot) { VideoList_clear(&slot->list); StreamInfo_free(&slot->stream); free(slot->big); free(slot->text); }
    }
    if (!slot) { pthread_mutex_unlock(&lock); return 0; }
    memset(slot, 0, sizeof(*slot));
    slot->used = true; slot->id = next_id++; slot->kind = kind; slot->count = count; slot->start = start; slot->newest = newest; slot->priority = priority;
    scopy(slot->arg, sizeof(slot->arg), arg);
    if (big) slot->big = strdup(big);
    int id = slot->id;
    pthread_cond_broadcast(&wake);
    pthread_mutex_unlock(&lock);
    return id;
}
int Yt_search(const char *q, int count, bool newest, bool priority) { return add_job(K_SEARCH, q, count, newest, priority); }
int Yt_channel(const char *cid, int count) { return add_job(K_CHANNEL, cid, count, false, true); }
int Yt_searchPage(const char *q, int start, int count, bool newest, bool priority) { return add_job2(K_SEARCH, q, start, count, newest, priority); }
int Yt_channelPage(const char *cid, int start, int count) { return add_job2(K_CHANNEL, cid, start, count, false, true); }
bool Yt_deferred(const char *id) {
    bool r = false;
    if (kids_fast) return false;     /* the resident solver is warm: prefetch them like any video */
    pthread_mutex_lock(&lock);
    for (int i = 0; i < 32; i++) if (!strcmp(deferred[i], id)) r = true;
    pthread_mutex_unlock(&lock);
    return r;
}
int Yt_resolve(const char *id, bool priority) { return add_job(K_RESOLVE, id, 0, false, priority); }
int Yt_fetch(const char *url) { return add_job3(K_FETCH, "fetch", 0, 0, false, true, url); }
char *Yt_takeText(int id) {
    pthread_mutex_lock(&lock); Job *j = find(id); char *t = NULL;
    if (j && j->state == JOB_DONE) { t = j->text; j->text = NULL; free(j->big); j->big = NULL; j->used = false; }
    pthread_mutex_unlock(&lock); return t;
}
JobState Yt_state(int id) {
    pthread_mutex_lock(&lock); Job *j = find(id); JobState s = j ? j->state : JOB_FAILED; pthread_mutex_unlock(&lock); return s;
}
bool Yt_takeList(int id, VideoList *out) {
    pthread_mutex_lock(&lock); Job *j = find(id); bool ok = j && j->state == JOB_DONE;
    if (ok) { VideoList_clear(out); *out = j->list; memset(&j->list, 0, sizeof(j->list)); j->used = false; }
    pthread_mutex_unlock(&lock); return ok;
}
bool Yt_takeStream(int id, StreamInfo *out) {
    pthread_mutex_lock(&lock); Job *j = find(id); bool ok = j && j->state == JOB_DONE;
    if (ok) { *out = j->stream; memset(&j->stream, 0, sizeof(j->stream)); j->used = false; }
    pthread_mutex_unlock(&lock); return ok;
}
const char *Yt_error(int id) {
    static char copy[200];
    pthread_mutex_lock(&lock); Job *j = find(id); scopy(copy, sizeof(copy), j ? j->error : "Không có dữ liệu"); pthread_mutex_unlock(&lock); return copy;
}
void Yt_forget(int id) {
    pthread_mutex_lock(&lock); Job *j = find(id);
    if (j && j->state != JOB_RUNNING) { VideoList_clear(&j->list); StreamInfo_free(&j->stream); free(j->big); free(j->text); j->big = j->text = NULL; j->used = false; }
    else if (j) j->priority = false;
    pthread_mutex_unlock(&lock);
}
bool Yt_cachedStream(const char *id, StreamInfo *out) {
    bool ok = false;
    pthread_mutex_lock(&lock);
    for (int i = 0; i < CACHE_N; i++) {
        if (cache[i].fetched_at && !strcmp(cache[i].info.id, id) && time(NULL) - cache[i].fetched_at < 4 * 3600) { copy_stream(out, &cache[i]); ok = true; break; }
    }
    pthread_mutex_unlock(&lock);
    return ok;
}
