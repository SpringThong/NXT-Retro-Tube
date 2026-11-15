/* 2.3.10 extras for the player:
 *  - SponsorBlock: community-marked sponsor / self-promo / "subscribe" segments, looked up by a
 *    4-character hash prefix of the video id (the server never learns which video you watch)
 *  - seek preview: YouTube's storyboard sprites (small frames every few seconds), fetched on demand
 * SPDX-License-Identifier: GPL-3.0-only */
#include "app.h"
#include "parson.h"
#include <mbedtls/sha256.h>
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---------- SponsorBlock ---------- */
static pthread_mutex_t slock = PTHREAD_MUTEX_INITIALIZER;
static char s_id[16], s_want[16];
static SponsorSeg s_seg[24]; static int s_n;
static bool s_busy; static pthread_t s_thr; static bool s_started;

static const char *CATS[] = {"sponsor", "selfpromo", "interaction", "intro", "outro", "music_offtopic", "preview"};
static int cat_of(const char *c) { for (int i = 0; i < 7; i++) if (c && !strcmp(c, CATS[i])) return i; return -1; }

static void *sponsor_work(void *u) {
    (void)u;
    for (;;) {
        char id[16];
        pthread_mutex_lock(&slock);
        snprintf(id, sizeof(id), "%s", s_want);
        if (!strcmp(id, s_id)) { s_busy = false; pthread_mutex_unlock(&slock); return NULL; }
        pthread_mutex_unlock(&slock);
        unsigned char h[32]; mbedtls_sha256((const unsigned char *)id, strlen(id), h, 0);
        char url[400], err[160] = "";
        snprintf(url, sizeof(url), "https://sponsor.ajay.app/api/skipSegments/%02x%02x?categories=%%5B%%22sponsor%%22,%%22selfpromo%%22,%%22interaction%%22,%%22music_offtopic%%22%%5D", h[0], h[1]);
        char *body = Innertube_get(url, err, sizeof(err));
        SponsorSeg seg[24]; int n = 0;
        JSON_Value *v = body ? json_parse_string(body) : NULL;
        JSON_Array *a = json_value_get_array(v);
        for (size_t i = 0; a && i < json_array_get_count(a); i++) {
            JSON_Object *e = json_array_get_object(a, i);
            const char *vid = json_object_get_string(e, "videoID");
            if (!vid || strcmp(vid, id)) continue;
            JSON_Array *ss = json_object_get_array(e, "segments");
            for (size_t k = 0; ss && k < json_array_get_count(ss) && n < 24; k++) {
                JSON_Object *g = json_array_get_object(ss, k);
                JSON_Array *t = json_object_get_array(g, "segment");
                const char *act = json_object_get_string(g, "actionType");
                if (!t || json_array_get_count(t) < 2 || (act && strcmp(act, "skip"))) continue;
                seg[n].start = json_array_get_number(t, 0); seg[n].end = json_array_get_number(t, 1);
                seg[n].cat = cat_of(json_object_get_string(g, "category"));
                if (seg[n].end - seg[n].start >= 1) n++;
            }
        }
        json_value_free(v); free(body);
        if (!body && err[0]) Log_msg("sponsorblock %s: %s", id, err);
        else Log_msg("sponsorblock %s: %d segments", id, n);
        pthread_mutex_lock(&slock);
        snprintf(s_id, sizeof(s_id), "%s", id);
        if (!strcmp(id, s_want)) { memcpy(s_seg, seg, sizeof(seg)); s_n = n; }
        pthread_mutex_unlock(&slock);
    }
}
void Sponsor_fetch(const char *id) {
    pthread_mutex_lock(&slock);
    if (!id || !strcmp(id, s_want)) { pthread_mutex_unlock(&slock); return; }
    snprintf(s_want, sizeof(s_want), "%s", id); s_n = 0;
    bool start = !s_busy;
    if (start) { if (s_started) pthread_join(s_thr, NULL); s_busy = s_started = true; }
    pthread_mutex_unlock(&slock);
    if (start) pthread_create(&s_thr, NULL, sponsor_work, NULL);
}
int Sponsor_get(const char *id, SponsorSeg *out, int max) {
    pthread_mutex_lock(&slock);
    int n = 0;
    if (id && !strcmp(id, s_id) && !strcmp(id, s_want)) for (; n < s_n && n < max; n++) out[n] = s_seg[n];
    pthread_mutex_unlock(&slock);
    return n;
}
const char *Sponsor_catName(int c) {
    static const char *N[] = {"đoạn tài trợ", "tự quảng cáo", "nhắc đăng ký", "mở đầu", "kết thúc", "đoạn không phải nhạc", "xem trước"};
    return c >= 0 && c < 7 ? N[c] : "đoạn quảng cáo";
}

/* ---------- seek preview sprites ---------- */
typedef struct { char url[1600]; int w, h, cols, rows, cnt; double iv; } Sb;
static Sb sb; static char sb_vid[16];
#define SPR_N 4
static struct { int m; SDL_Texture *tex; unsigned used; } spr[SPR_N];
static unsigned spr_tick;
static pthread_mutex_t plock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t pwake = PTHREAD_COND_INITIALIZER;
static int p_want = -1, p_inflight = -1, p_gen;
static unsigned char *p_rgb; static int p_m = -1, p_w, p_h, p_rgen;
static bool p_running; static pthread_t p_thr;
static int p_failed[8]; static int p_nfailed;

static unsigned char *fetch_decode(const char *url, int *w, int *h) {
    AVDictionary *o = NULL;
    av_dict_set(&o, "ca_file", paths.cacert, 0);
    av_dict_set(&o, "tls_verify", "0", 0);   /* public pictures */
    av_dict_set(&o, "rw_timeout", "10000000", 0);
    av_dict_set(&o, "user_agent", "Mozilla/5.0 (X11; Linux aarch64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/126.0 Safari/537.36", 0);
    AVIOContext *in = NULL;
    int r = avio_open2(&in, url, AVIO_FLAG_READ, NULL, &o);
    av_dict_free(&o);
    if (r < 0) return NULL;
    size_t cap = 65536, len = 0; unsigned char *buf = malloc(cap + AV_INPUT_BUFFER_PADDING_SIZE);
    for (;;) {
        if (len + 16384 > cap) { if (cap > 3 * 1024 * 1024) break; cap *= 2; unsigned char *nb = realloc(buf, cap + AV_INPUT_BUFFER_PADDING_SIZE); if (!nb) break; buf = nb; }
        int n = avio_read(in, buf + len, 16384);
        if (n <= 0) break;
        len += n;
    }
    avio_closep(&in);
    unsigned char *out = NULL;
    if (len > 200) {
        memset(buf + len, 0, AV_INPUT_BUFFER_PADDING_SIZE);
        AVPacket *pkt = av_packet_alloc();
        const AVCodec *c = avcodec_find_decoder(AV_CODEC_ID_MJPEG);
        AVCodecContext *cx = c ? avcodec_alloc_context3(c) : NULL;
        AVFrame *fr = av_frame_alloc();
        if (pkt && cx && fr) {
            pkt->data = buf; pkt->size = (int)len;
            if (avcodec_open2(cx, c, NULL) >= 0 && avcodec_send_packet(cx, pkt) >= 0 && avcodec_receive_frame(cx, fr) >= 0) {
                struct SwsContext *sw = sws_getContext(fr->width, fr->height, fr->format, fr->width, fr->height, AV_PIX_FMT_RGB24, SWS_POINT, NULL, NULL, NULL);
                if (sw) {
                    out = malloc((size_t)fr->width * fr->height * 3);
                    uint8_t *dst[4] = {out}; int ls[4] = {fr->width * 3};
                    if (out) { sws_scale(sw, (const uint8_t *const *)fr->data, fr->linesize, 0, fr->height, dst, ls); *w = fr->width; *h = fr->height; }
                    sws_freeContext(sw);
                }
            }
            pkt->data = NULL; pkt->size = 0;
        }
        av_frame_free(&fr); avcodec_free_context(&cx); av_packet_free(&pkt);
    }
    free(buf);
    return out;
}
static void *preview_work(void *u) {
    (void)u;
    pthread_mutex_lock(&plock);
    while (p_running) {
        if (p_want < 0) { pthread_cond_wait(&pwake, &plock); continue; }
        int m = p_want, gen = p_gen; p_want = -1; p_inflight = m;
        char url[1700]; size_t k = 0;
        for (const char *p = sb.url; *p && k + 12 < sizeof(url); p++) {
            if (p[0] == '$' && p[1] == 'M') { k += snprintf(url + k, sizeof(url) - k, "%d", m); p++; } else url[k++] = *p;
        }
        url[k] = 0;
        pthread_mutex_unlock(&plock);
        int w = 0, h = 0; unsigned char *rgb = fetch_decode(url, &w, &h);
        pthread_mutex_lock(&plock);
        p_inflight = -1;
        if (gen != p_gen) { free(rgb); continue; }
        if (!rgb) { if (p_nfailed < 8) p_failed[p_nfailed++] = m; continue; }
        free(p_rgb); p_rgb = rgb; p_m = m; p_w = w; p_h = h; p_rgen = gen;
    }
    pthread_mutex_unlock(&plock);
    return NULL;
}
void Preview_set(const char *video_id, const char *spec) {
    pthread_mutex_lock(&plock);
    if (video_id && !strcmp(video_id, sb_vid) && sb.cnt) { pthread_mutex_unlock(&plock); return; }
    p_gen++; p_want = -1; p_nfailed = 0; free(p_rgb); p_rgb = NULL; p_m = -1;
    memset(&sb, 0, sizeof(sb)); snprintf(sb_vid, sizeof(sb_vid), "%s", video_id ? video_id : "");
    if (spec && sscanf(spec, "%1599[^\t]\t%d\t%d\t%d\t%d\t%lf\t%d", sb.url, &sb.w, &sb.h, &sb.cols, &sb.rows, &sb.iv, &sb.cnt) != 7) memset(&sb, 0, sizeof(sb));
    if (sb.cols <= 0 || sb.rows <= 0 || sb.iv <= 0) sb.cnt = 0;
    if (!p_running && sb.cnt) { p_running = true; pthread_create(&p_thr, NULL, preview_work, NULL); }
    pthread_mutex_unlock(&plock);
    for (int i = 0; i < SPR_N; i++) { if (spr[i].tex) SDL_DestroyTexture(spr[i].tex); spr[i].tex = NULL; spr[i].m = -1; }
}
bool Preview_has(void) { return sb.cnt > 0; }
/* UI thread: the frame for time t, or false while its sprite sheet is still loading */
bool Preview_frame(SDL_Renderer *r, double t, SDL_Texture **tex, SDL_Rect *src) {
    if (!sb.cnt || t < 0) return false;
    int idx = (int)(t / sb.iv); if (idx >= sb.cnt) idx = sb.cnt - 1;
    int per = sb.cols * sb.rows, m = idx / per, in = idx % per;
    spr_tick++;
    pthread_mutex_lock(&plock);
    if (p_rgb && p_rgen == p_gen) {
        int v = 0; for (int i = 0; i < SPR_N; i++) { if (!spr[i].tex) { v = i; break; } if (spr[i].used < spr[v].used) v = i; }
        if (spr[v].tex) SDL_DestroyTexture(spr[v].tex);
        spr[v].tex = SDL_CreateTexture(r, SDL_PIXELFORMAT_RGB24, SDL_TEXTUREACCESS_STATIC, p_w, p_h);
        if (spr[v].tex) { SDL_UpdateTexture(spr[v].tex, NULL, p_rgb, p_w * 3); SDL_SetTextureScaleMode(spr[v].tex, SDL_ScaleModeLinear); }
        spr[v].m = p_m; spr[v].used = spr_tick;
        free(p_rgb); p_rgb = NULL;
    }
    for (int i = 0; i < SPR_N; i++) if (spr[i].tex && spr[i].m == m) {
        spr[i].used = spr_tick;
        pthread_mutex_unlock(&plock);
        int tw = 0, th = 0; SDL_QueryTexture(spr[i].tex, NULL, NULL, &tw, &th);
        int fw = tw / sb.cols, fh = th / sb.rows;
        *tex = spr[i].tex; *src = (SDL_Rect){(in % sb.cols) * fw, (in / sb.cols) * fh, fw, fh};
        return true;
    }
    bool failed = false; for (int i = 0; i < p_nfailed; i++) if (p_failed[i] == m) failed = true;
    if (!failed && p_inflight != m) { p_want = m; pthread_cond_signal(&pwake); }
    pthread_mutex_unlock(&plock);
    return false;
}
void Preview_dropTextures(void) {
    for (int i = 0; i < SPR_N; i++) { if (spr[i].tex) SDL_DestroyTexture(spr[i].tex); spr[i].tex = NULL; spr[i].m = -1; }
}
