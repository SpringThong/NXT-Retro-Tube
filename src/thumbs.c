/* Video thumbnails: i.ytimg.com mqdefault (320x180), cached on the SD card.
 * Downloads run on two worker threads; textures are made on the UI thread.
 * SPDX-License-Identifier: GPL-3.0-only */
#include "app.h"
#ifndef NXT_TUBE_PW
#include <SDL2/SDL_image.h>
#endif
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
#include <stdlib.h>
#include <pthread.h>
#include <sys/stat.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#define TEX_N 96
#define QUEUE_N 64
typedef struct { char id[16]; SDL_Texture *tex; unsigned used; bool failed; } Slot;
static Slot slots[TEX_N];
static unsigned tick_counter;
static SDL_Renderer *renderer;
static char queue[QUEUE_N][16];
static int qcount;
static char failed[64][16]; static Uint32 failed_at[64]; static int failed_next;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t wake = PTHREAD_COND_INITIALIZER;
static pthread_t workers[2];
static bool running;
static int loads_this_frame;
static char inflight[2][16];

#define READY_N 12
#define TW 320
#define TH 180
typedef struct { char id[16]; unsigned char *rgb; } Ready;
static Ready ready[READY_N];
static int nready;

/* JPEG -> 320x180 RGB24 with FFmpeg's mjpeg decoder (the ROM's SDL_image may lack JPEG). */
static unsigned char *decode_jpeg(const char *path) {
    FILE *f = fopen(path, "rb"); if (!f) return NULL;
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    if (n < 100 || n > 4 * 1024 * 1024) { fclose(f); return NULL; }
    AVPacket *pkt = av_packet_alloc();
    if (!pkt || av_new_packet(pkt, (int)n) < 0) { fclose(f); av_packet_free(&pkt); return NULL; }
    bool rd = fread(pkt->data, 1, n, f) == (size_t)n; fclose(f);
    unsigned char *out = NULL;
    const AVCodec *c = avcodec_find_decoder(AV_CODEC_ID_MJPEG);
    AVCodecContext *cx = c ? avcodec_alloc_context3(c) : NULL;
    AVFrame *fr = av_frame_alloc();
    if (rd && cx && fr && avcodec_open2(cx, c, NULL) >= 0 && avcodec_send_packet(cx, pkt) >= 0 && avcodec_receive_frame(cx, fr) >= 0) {
        struct SwsContext *sw = sws_getContext(fr->width, fr->height, fr->format, TW, TH, AV_PIX_FMT_RGB24, SWS_BILINEAR, NULL, NULL, NULL);
        if (sw) {
            out = malloc(TW * TH * 3);
            uint8_t *dst[4] = {out}; int ls[4] = {TW * 3};
            if (out) sws_scale(sw, (const uint8_t *const *)fr->data, fr->linesize, 0, fr->height, dst, ls);
            sws_freeContext(sw);
        }
    }
    av_frame_free(&fr); avcodec_free_context(&cx); av_packet_free(&pkt);
    return out;
}
static void thumb_path(const char *id, char *out, size_t n) { snprintf(out, n, "%s/thumbs/%s.jpg", paths.cache, id); }
static bool is_failed(const char *id) { for (int i = 0; i < 64; i++) if (!strcmp(failed[i], id)) return SDL_GetTicks() - failed_at[i] < 45000; return false; }

static int logged;
static bool fetch_url(const char *url, const char *path, const char *tmp, bool verify) {
    AVDictionary *o = NULL;
    av_dict_set(&o, "ca_file", paths.cacert, 0);
    av_dict_set(&o, "tls_verify", verify ? "1" : "0", 0);
    av_dict_set(&o, "rw_timeout", "12000000", 0);
    av_dict_set(&o, "timeout", "12000000", 0);
    av_dict_set(&o, "icy", "0", 0);
    av_dict_set(&o, "user_agent", "Mozilla/5.0 (X11; Linux aarch64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/126.0 Safari/537.36", 0);
    av_dict_set(&o, "headers", "Accept: image/jpeg,image/*;q=0.8\r\n", 0);
    AVIOContext *in = NULL;
    int r = avio_open2(&in, url, AVIO_FLAG_READ, NULL, &o);
    av_dict_free(&o);
    if (r < 0) { if (logged++ < 8) Log_msg("thumb %s: %s", url, av_err2str(r)); return false; }
    FILE *f = fopen(tmp, "wb");
    if (!f) { avio_closep(&in); if (logged++ < 8) Log_msg("thumb: cannot write %s", tmp); return false; }
    unsigned char buf[16384]; long total = 0; bool ok = true;
    for (;;) {
        int n = avio_read(in, buf, sizeof(buf));
        if (n == AVERROR_EOF || n == 0) break;
        if (n < 0) { ok = false; if (logged++ < 8) Log_msg("thumb read %s: %s", url, av_err2str(n)); break; }
        if (fwrite(buf, 1, n, f) != (size_t)n) { ok = false; break; }
        total += n; if (total > 2 * 1024 * 1024) { ok = false; break; }
    }
    avio_closep(&in);
    if (fclose(f)) ok = false;
    if (ok && total > 500) return !rename(tmp, path);
    if (ok && logged++ < 8) Log_msg("thumb %s: only %ld bytes", url, total);
    unlink(tmp); return false;
}
static bool fetch(const char *id) {
    char url[200], path[1200], tmp[1260];
    thumb_path(id, path, sizeof(path));
    snprintf(tmp, sizeof(tmp), "%s.%lx.part", path, (unsigned long)pthread_self());
    snprintf(url, sizeof(url), "https://i.ytimg.com/vi/%s/mqdefault.jpg", id);
    if (fetch_url(url, path, tmp, true)) return true;
    snprintf(url, sizeof(url), "https://i.ytimg.com/vi/%s/hqdefault.jpg", id);
    if (fetch_url(url, path, tmp, true)) return true;
    /* thumbnails are public pictures: accept them even if the clock or CA store is off */
    snprintf(url, sizeof(url), "https://i.ytimg.com/vi/%s/mqdefault.jpg", id);
    if (fetch_url(url, path, tmp, false)) return true;
    snprintf(url, sizeof(url), "http://i.ytimg.com/vi/%s/mqdefault.jpg", id);
    return fetch_url(url, path, tmp, false);
}
static void *work(void *u) {
    (void)u;
    pthread_mutex_lock(&lock);
    while (running) {
        if (!qcount) { pthread_cond_wait(&wake, &lock); continue; }
        char id[16]; snprintf(id, sizeof(id), "%s", queue[--qcount]); /* newest request first */
        int me = !strcmp(inflight[0], "") ? 0 : 1; snprintf(inflight[me], 16, "%s", id);
        pthread_mutex_unlock(&lock);
        char path[1200]; thumb_path(id, path, sizeof(path));
        struct stat st; bool ok = !stat(path, &st) || fetch(id);
        unsigned char *rgb = ok ? decode_jpeg(path) : NULL;
        if (ok && !rgb) { if (logged++ < 8) Log_msg("thumb %s: cannot decode", id); unlink(path); ok = false; }
        pthread_mutex_lock(&lock);
        inflight[me][0] = 0;
        if (rgb) {
            if (nready == READY_N) { free(ready[0].rgb); memmove(&ready[0], &ready[1], (READY_N - 1) * sizeof(Ready)); nready--; }
            snprintf(ready[nready].id, 16, "%s", id); ready[nready++].rgb = rgb;
        }
        if (!ok) { snprintf(failed[failed_next], 16, "%s", id); failed_at[failed_next] = SDL_GetTicks(); failed_next = (failed_next + 1) % 64; }
    }
    pthread_mutex_unlock(&lock);
    return NULL;
}
void Thumbs_init(SDL_Renderer *r) {
    renderer = r;
    char dir[1200]; snprintf(dir, sizeof(dir), "%s/thumbs", paths.cache); mkdir(dir, 0755);
    running = true;
    for (int i = 0; i < 2; i++) pthread_create(&workers[i], NULL, work, NULL);
}
void Thumbs_releaseTextures(void) {
    for (int i = 0; i < TEX_N; i++) { if (slots[i].tex) SDL_DestroyTexture(slots[i].tex); memset(&slots[i], 0, sizeof(slots[i])); }
}
void Thumbs_quit(void) {
    pthread_mutex_lock(&lock); running = false; pthread_cond_broadcast(&wake); pthread_mutex_unlock(&lock);
    for (int i = 0; i < 2; i++) pthread_join(workers[i], NULL);
    for (int r = 0; r < nready; r++) free(ready[r].rgb);
    nready = 0;
    Thumbs_releaseTextures();
}
void Thumbs_setRenderer(SDL_Renderer *r) { renderer = r; }
void Thumbs_tick(void) { tick_counter++; loads_this_frame = 0; }
SDL_Texture *Thumbs_get(const char *id) {
    if (!id || strlen(id) != 11 || !renderer) return NULL;
    for (int i = 0; i < TEX_N; i++) if (!strcmp(slots[i].id, id)) { slots[i].used = tick_counter; return slots[i].tex; }
    pthread_mutex_lock(&lock);
    for (int r = 0; r < nready; r++) if (!strcmp(ready[r].id, id)) {
        if (loads_this_frame >= 3) { pthread_mutex_unlock(&lock); return NULL; }
        loads_this_frame++;
        unsigned char *rgb = ready[r].rgb;
        memmove(&ready[r], &ready[r + 1], (nready - r - 1) * sizeof(Ready)); nready--;
        pthread_mutex_unlock(&lock);
        SDL_Texture *t = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGB24, SDL_TEXTUREACCESS_STATIC, TW, TH);
        if (t) { SDL_UpdateTexture(t, NULL, rgb, TW * 3); SDL_SetTextureScaleMode(t, SDL_ScaleModeLinear); }
        free(rgb);
        if (!t) return NULL;
        int victim = 0;
        for (int i = 0; i < TEX_N; i++) { if (!slots[i].id[0]) { victim = i; break; } if (slots[i].used < slots[victim].used) victim = i; }
        if (slots[victim].tex) SDL_DestroyTexture(slots[victim].tex);
        snprintf(slots[victim].id, sizeof(slots[victim].id), "%s", id);
        slots[victim].tex = t; slots[victim].used = tick_counter;
        return t;
    }
    pthread_mutex_unlock(&lock);
    pthread_mutex_lock(&lock);
    if (!is_failed(id) && strcmp(inflight[0], id) && strcmp(inflight[1], id)) {
        bool queued = false;
        for (int i = 0; i < qcount; i++) if (!strcmp(queue[i], id)) { queued = true; /* move to top */ char t[16]; memcpy(t, queue[i], 16); memmove(&queue[i], &queue[i + 1], (qcount - i - 1) * 16); memcpy(queue[qcount - 1], t, 16); break; }
        if (!queued) {
            if (qcount == QUEUE_N) { memmove(&queue[0], &queue[1], (QUEUE_N - 1) * 16); qcount--; }
            snprintf(queue[qcount++], 16, "%s", id);
            pthread_cond_signal(&wake);
        }
    }
    pthread_mutex_unlock(&lock);
    return NULL;
}
