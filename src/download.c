/* Offline downloads: H.264 video (<= chosen height) + AAC audio as two files,
 * played back by the same player without network. One download at a time.
 * SPDX-License-Identifier: GPL-3.0-only */
#include "app.h"
#include <pthread.h>
#include <spawn.h>
#include <signal.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern char **environ;
static pthread_t thread;
static bool busy, started;
static char status[160];
static VideoItem current; static int height, kind;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static bool finished_ok, finished;

static int run(const char *format, const char *out, bool fast) {
    char exe[1200], deno[1200], pathv[2400], home[1100], xdg[1100], denodir[1100], url[80];
    snprintf(exe, sizeof(exe), "%s/yt-dlp-runtime/yt-dlp", paths.bin);
    snprintf(deno, sizeof(deno), "deno:%s/deno", paths.bin);
    snprintf(url, sizeof(url), "https://www.youtube.com/watch?v=%s", current.id);
    const char *argv[] = {"yt-dlp", "--ignore-config", "--no-warnings", "--no-progress", "--js-runtimes", deno, "--no-playlist",
                          "-f", format, "-o", out, "--no-part", "--force-overwrites", "--socket-timeout", "20", url,
                          fast ? "--extractor-args" : NULL, "youtube:player_client=visionos;player_skip=webpage,configs", NULL};
    const char *op = getenv("PATH");
    snprintf(pathv, sizeof(pathv), "PATH=%s:%s", paths.bin, op ? op : "/usr/bin:/bin");
    snprintf(home, sizeof(home), "HOME=%s/home", paths.data);
    snprintf(xdg, sizeof(xdg), "XDG_CACHE_HOME=%s/ytdlp", paths.cache);
    snprintf(denodir, sizeof(denodir), "DENO_DIR=%s/deno", paths.cache);
    int n = 0; while (environ[n]) n++;
    char **env = calloc(n + 6, sizeof(char *)); int k = 0;
    for (int i = 0; i < n; i++) if (strncmp(environ[i], "PATH=", 5) && strncmp(environ[i], "HOME=", 5)) env[k++] = environ[i];
    env[k++] = pathv; env[k++] = home; env[k++] = xdg; env[k++] = denodir; env[k] = NULL;
    char log[1200]; snprintf(log, sizeof(log), "%s/download-log.txt", paths.cache);
    posix_spawn_file_actions_t fa; posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_addopen(&fa, 0, "/dev/null", O_RDONLY, 0);
    posix_spawn_file_actions_addopen(&fa, 1, log, O_WRONLY | O_CREAT | O_APPEND, 0644);
    posix_spawn_file_actions_addopen(&fa, 2, log, O_WRONLY | O_CREAT | O_APPEND, 0644);
    pid_t pid; int rc = posix_spawn(&pid, exe, &fa, NULL, (char *const *)argv, env);
    posix_spawn_file_actions_destroy(&fa); free(env);
    if (rc) return -1;
    int st = 0; while (waitpid(pid, &st, 0) < 0) {}
    return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
}
static void set_status(const char *fmt, const char *arg) { pthread_mutex_lock(&lock); snprintf(status, sizeof(status), T(fmt), arg); pthread_mutex_unlock(&lock); }
static struct stat st_;
static bool have(const char *p) { return !stat(p, &st_) && st_.st_size > 0; }
/* music: the AAC track (kept for offline listening in the app) and the file in the export folder */
static bool work_music(char *msg, size_t n) {
    char audio[1300]; snprintf(audio, sizeof(audio), "%s", Store_downloadPath(current.id, true));
    if (!have(audio)) {
        set_status("Đang tải nhạc · %.60s", current.title);
        int r = run("140/ba[ext=m4a]/ba", audio, true);
        if (r) r = run("140/ba[ext=m4a]/ba", audio, false);
        if (r) { unlink(audio); snprintf(msg, n, "%s", T("Không tải được tiếng")); return false; }
    }
    if (kind == DL_OPUS) {
        char tmp[1300]; snprintf(tmp, sizeof(tmp), "%s/dl-%s.webm", paths.cache, current.id);
        set_status("Đang tải Opus · %.60s", current.title);
        int r = run("251/ba[acodec=opus]", tmp, true);
        if (r) r = run("251/ba[acodec=opus]", tmp, false);
        bool ok = !r && Export_audio(&current, DL_OPUS, tmp, msg, n);
        unlink(tmp);
        if (!r && !ok) return false;
        if (r) { snprintf(msg, n, "%s", T("Video này không có Opus")); return false; }
        return true;
    }
    set_status(kind == DL_MP3 ? "Đang chuyển sang MP3 · %.60s" : "Đang lưu M4A · %.60s", current.title);
    return Export_audio(&current, kind, audio, msg, n);
}
static void *work(void *u) {
    (void)u;
    char dir[1200]; snprintf(dir, sizeof(dir), "%s/downloads", paths.data); mkdir(dir, 0755);
    if (kind >= DL_M4A) {
        char msg[400] = "";
        bool ok = work_music(msg, sizeof(msg));
        pthread_mutex_lock(&lock);
        finished_ok = have(Store_downloadPath(current.id, true));
        snprintf(status, sizeof(status), "%s", ok ? msg : msg[0] ? msg : T("Tải thất bại"));
        finished = true; busy = false;
        pthread_mutex_unlock(&lock);
        Log_msg("download music %s kind %d: %s", current.id, kind, ok ? "ok" : msg);
        return NULL;
    }
    char vf[200];
    snprintf(vf, sizeof(vf), "bv*[vcodec^=avc1][height<=%d][ext=mp4]/bv*[height<=%d][ext=mp4]/18", height, height);
    char video[1300], audio[1300];
    snprintf(video, sizeof(video), "%s", Store_downloadPath(current.id, false));
    snprintf(audio, sizeof(audio), "%s", Store_downloadPath(current.id, true));
    pthread_mutex_lock(&lock); snprintf(status, sizeof(status), T("Đang tải hình · %.60s"), current.title); pthread_mutex_unlock(&lock);
    int r1 = run(vf, video, true);
    if (r1) r1 = run(vf, video, false);
    pthread_mutex_lock(&lock); snprintf(status, sizeof(status), T("Đang tải tiếng · %.60s"), current.title); pthread_mutex_unlock(&lock);
    int r2 = r1 == 0 ? run("140/ba[ext=m4a]/ba", audio, true) : -1;
    if (r1 == 0 && r2) r2 = run("140/ba[ext=m4a]/ba", audio, false);
    pthread_mutex_lock(&lock);
    finished_ok = r1 == 0 && r2 == 0;
    if (!finished_ok) { unlink(video); unlink(audio); }
    snprintf(status, sizeof(status), finished_ok ? T("Đã tải xong · %.60s") : T("Tải thất bại · %.60s"), current.title);
    if (!(finished_ok && kind == DL_VIDEO_MP4)) { finished = true; busy = false; }
    pthread_mutex_unlock(&lock);
    if (finished_ok && kind == DL_VIDEO_MP4) {
        set_status("Đang lưu file MP4 · %.60s", current.title);
        char msg[400] = ""; Export_video(&current, msg, sizeof(msg));
        pthread_mutex_lock(&lock); snprintf(status, sizeof(status), "%s", msg); finished = true; busy = false; pthread_mutex_unlock(&lock);
    }
    Log_msg("download %s: %s", current.id, finished_ok ? "ok" : "failed");
    return NULL;
}
int Download_start(const VideoItem *v, int max_height, int k) {
    pthread_mutex_lock(&lock);
    if (busy) { pthread_mutex_unlock(&lock); return 0; }
    if (started) pthread_join(thread, NULL);
    current = *v; height = max_height > 0 ? max_height : 720; kind = k; busy = true; started = true; finished = false;
    snprintf(status, sizeof(status), T("Chuẩn bị tải · %.60s"), v->title);
    pthread_mutex_unlock(&lock);
    pthread_create(&thread, NULL, work, NULL);
    return 1;
}
bool Download_finishedMsg(char *out, size_t n) {   /* the result line, once, for a toast */
    static bool shown = true; bool r = false;
    pthread_mutex_lock(&lock);
    if (busy) shown = false;
    else if (!shown && started) { shown = true; snprintf(out, n, "%s", status); r = true; }
    pthread_mutex_unlock(&lock);
    return r;
}
bool Download_busy(void) { pthread_mutex_lock(&lock); bool b = busy; pthread_mutex_unlock(&lock); return b; }
const char *Download_status(void) { static char copy[160]; pthread_mutex_lock(&lock); snprintf(copy, sizeof(copy), "%s", status); pthread_mutex_unlock(&lock); return copy; }
void Download_tick(void) {
    pthread_mutex_lock(&lock);
    if (finished) { finished = false; if (finished_ok) { VideoItem v = current; pthread_mutex_unlock(&lock); Store_addDownload(&v); return; } }
    pthread_mutex_unlock(&lock);
}
