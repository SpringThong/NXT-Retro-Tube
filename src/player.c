/* Streaming player: DASH video + audio (or one muxed/HLS input), packet
 * read-ahead, threaded software decoding and an audio master clock.
 *
 *  demux threads ──packets──► decode threads ──► video frame queue ─► UI thread
 *       (read ahead ~45 s)                    └─► PCM ring ─────────► SDL audio
 *
 * Audio is the clock: video frames are shown when their timestamp is due and
 * dropped when late, so picture and sound stay together even when decoding
 * falls behind. Network stalls pause the clock (buffering) instead of
 * producing gaps. SPDX-License-Identifier: GPL-3.0-only */
#include "app.h"
#include "sonic.h"
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
#include <libavutil/opt.h>
#include <libavutil/imgutils.h>
#include <libavutil/channel_layout.h>
#include <libavutil/time.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <math.h>

#define OUT_RATE 48000
#define PKT_AHEAD_SEC 45.0
#define PKT_MAX_BYTES (40 << 20)
#define RING_FRAMES (OUT_RATE * 2)
#define VFRAMES 6
#define START_SEC 0.5
#define HIST_SEC 40.0          /* packets kept behind the play position for instant back-seeks */
#define HIST_BYTES (28 << 20)
#define RESUME_SEC 2.0
#define CHUNK_BYTES (10LL << 20)
#define SEGS 512

/* ---------- chunked HTTP input (like yt-dlp's 10 MB ranges) ---------- */
typedef struct {
    char *url;
    AVDictionary *opts;
    int64_t pos, size, end;
    AVIOContext *cur;
    AVIOInterruptCB *icb;
    bool insecure;
} ChunkIO;

static int chunk_open(ChunkIO *c) {
    AVDictionary *o = NULL;
    av_dict_copy(&o, c->opts, 0);
    char v[32];
    snprintf(v, sizeof(v), "%lld", (long long)c->pos); av_dict_set(&o, "offset", v, 0);
    c->end = c->pos + CHUNK_BYTES;
    if (c->size > 0 && c->end > c->size) c->end = c->size;
    snprintf(v, sizeof(v), "%lld", (long long)c->end); av_dict_set(&o, "end_offset", v, 0);
    if (c->insecure) av_dict_set(&o, "tls_verify", "0", 0);
    int r = avio_open2(&c->cur, c->url, AVIO_FLAG_READ, c->icb, &o);
    if (r < 0 && !c->insecure && r != AVERROR_EXIT && !strncmp(c->url, "https:", 6)) {
        /* wrong clock or missing root: the stream is still worth playing */
        Log_msg("https open failed (%s), retrying without certificate check", av_err2str(r));
        c->insecure = true;
        av_dict_set(&o, "tls_verify", "0", 0);
        r = avio_open2(&c->cur, c->url, AVIO_FLAG_READ, c->icb, &o);
    }
    av_dict_free(&o);
    if (r < 0) { c->cur = NULL; return r; }
    if (c->size <= 0) { int64_t s = avio_size(c->cur); if (s > 0) { c->size = s; if (c->end > s) c->end = s; } }
    return 0;
}
static int chunk_read(void *opaque, uint8_t *buf, int n) {
    ChunkIO *c = opaque;
    for (int attempt = 0; attempt < 4; attempt++) {
        if (c->size > 0 && c->pos >= c->size) return AVERROR_EOF;
        if (c->icb && c->icb->callback && c->icb->callback(c->icb->opaque)) return AVERROR_EXIT;
        if (!c->cur || c->pos >= c->end) {
            if (c->cur) avio_closep(&c->cur);
            int r = chunk_open(c);
            if (r == AVERROR_EXIT) return r;
            if (r < 0) { Log_msg("http open failed at %lld: %s", (long long)c->pos, av_err2str(r)); av_usleep(400000 * (attempt + 1)); continue; }
        }
        int want = n;
        if (c->end > 0 && c->pos + want > c->end) want = (int)(c->end - c->pos);
        int got = avio_read_partial(c->cur, buf, want);
        if (got > 0) { c->pos += got; return got; }
        avio_closep(&c->cur);   /* EOF of this range or error: reopen at the same position */
        if (got == AVERROR_EXIT) return got;
        if (got < 0 && got != AVERROR_EOF) { Log_msg("http read error at %lld: %s", (long long)c->pos, av_err2str(got)); av_usleep(300000 * (attempt + 1)); }
    }
    return AVERROR(EIO);
}
static int64_t chunk_seek(void *opaque, int64_t offset, int whence) {
    ChunkIO *c = opaque;
    if (whence & AVSEEK_SIZE) return c->size > 0 ? c->size : -1;
    whence &= ~AVSEEK_FORCE;
    int64_t target = whence == SEEK_SET ? offset : whence == SEEK_CUR ? c->pos + offset : whence == SEEK_END && c->size > 0 ? c->size + offset : -1;
    if (target < 0) return -1;
    if (target != c->pos || !c->cur) { if (c->cur) avio_closep(&c->cur); c->pos = target; }
    return target;
}
static void chunk_free(ChunkIO *c) {
    if (!c) return;
    if (c->cur) avio_closep(&c->cur);
    av_dict_free(&c->opts); free(c->url); free(c);
}

/* ---------- queues ---------- */
typedef struct { AVPacket *pkt; int serial; double ts; } QItem;
typedef struct {
    QItem *items; int cap, head, count;
    int64_t bytes;
    double last_ts;
    bool eof;
} PQueue;

typedef struct {
    AVFormatContext *fmt;
    ChunkIO *cio;
    AVIOContext *pb;
    int vidx, aidx;
    pthread_t thread; bool started;
    atomic_bool seek_pending, stop;
    double seek_to;
} Demux;

typedef struct { double pts; int frames; float speed; } Seg;

static struct {
    pthread_mutex_t lock;
    pthread_cond_t cond;
    SDL_AudioDeviceID dev;
    atomic_bool quit;
    bool active;
    pthread_t opener; bool opener_started;
    pthread_t vthread, athread; bool vthread_on, athread_on;
    Demux dx[2]; int ndx;
    PQueue vq, aq, vh, ah;       /* queues ahead, histories behind */
    int dserial;                  /* demux epoch: bumped only when the network position changes */
    bool snap_pending, snap_forward; double snap_t, snap_pos;   /* network seek: start at the keyframe */
    AVCodecContext *vdec, *adec;
    AVRational vtb, atb;
    int serial;
    double drop_v, drop_a;          /* decode-side seek targets */
    /* video frames */
    AVFrame *frames[VFRAMES]; double fpts[VFRAMES]; int fhead, fcount;
    bool preview;                   /* show the first frame after a seek while paused */
    /* audio */
    float *ring; int rr, rw, rcount;
    Seg seg[SEGS]; int shead, scount;
    double next_pts;                /* timeline position after the last pushed sample */
    double clock_pts; Uint32 clock_ticks;
    sonicStream sonic; float speed;
    SwrContext *swr;
    /* state */
    PlayerStateKind state;
    bool paused, buffering, started, ended, audio_eof_done, has_video, has_audio, live, audio_only;
    double duration, start;
    char error[160];
    unsigned decoded, dropped;
    int av_offset_ms;
    int width, height; double fps; char vcodec[24], acodec[24]; double kbps;
    struct SwsContext *sws; uint8_t *conv[4]; int conv_ls[4]; int conv_w, conv_h;
    int degrade; bool hifps, resyncing;
    bool full60, cap30;   /* full60: setting; cap30: this 60 fps video shows up to ~30 pictures a second */
    float fade;
    bool boost;            /* this video needed full CPU speed once: keep it */
    Uint32 last_show; unsigned shown, stat_frames, stat_drop; Uint32 stat_at, stat_upload_ms; 
    double latency, user_offset; bool usb_out; float gain;    /* audio output latency estimate + user A/V offset (s) */
    bool vswitch, vparked; int vgen;  /* live video-track swap */
    pthread_t switcher; bool switcher_on; atomic_bool switch_done;
    /* source copy */
    PlayerSource src;
    char *s_video, *s_audio, *s_muxed, *s_fallback, *s_headers, *s_ua, *s_vcodec, *s_lv, *s_la;
} P;

static SDL_Texture *frame_tex; static int tex_w, tex_h; static bool tex_yuv;

static void q_init(PQueue *q) { q->cap = 4096; q->items = calloc(q->cap, sizeof(QItem)); q->head = q->count = 0; q->bytes = 0; q->last_ts = -1e9; q->eof = false; }
static void q_flush(PQueue *q) {
    for (int i = 0; i < q->count; i++) av_packet_free(&q->items[(q->head + i) % q->cap].pkt);
    q->head = q->count = 0; q->bytes = 0; q->eof = false; q->last_ts = -1e9;
}
static void q_free(PQueue *q) { q_flush(q); free(q->items); q->items = NULL; }
static bool q_push(PQueue *q, AVPacket *p, int serial, double ts) {
    if (q->count == q->cap) return false;
    QItem *it = &q->items[(q->head + q->count++) % q->cap];
    it->pkt = p; it->serial = serial; it->ts = ts;
    q->bytes += p->size;
    if (ts > q->last_ts || q->count == 1) q->last_ts = ts;
    return true;
}
static AVPacket *q_pop(PQueue *q, int *serial, double *ts) {
    if (!q->count) return NULL;
    QItem *it = &q->items[q->head]; q->head = (q->head + 1) % q->cap; q->count--;
    q->bytes -= it->pkt->size; *serial = it->serial; if (ts) *ts = it->ts;
    return it->pkt;
}
static QItem *q_at(PQueue *q, int i) { return &q->items[(q->head + i) % q->cap]; }
/* history of packets already handed to a decoder (refcounted clones) */
static void hist_push(PQueue *h, AVPacket *p, double ts) {
    if (!p || !h->items) { av_packet_free(&p); return; }
    while (h->count && (h->count == h->cap || h->bytes > HIST_BYTES || (ts >= 0 && q_at(h, 0)->ts >= 0 && ts - q_at(h, 0)->ts > HIST_SEC))) {
        QItem *o = q_at(h, 0); h->bytes -= o->pkt->size; av_packet_free(&o->pkt); h->head = (h->head + 1) % h->cap; h->count--;
    }
    QItem *it = &h->items[(h->head + h->count++) % h->cap];
    it->pkt = p; it->serial = 0; it->ts = ts; h->bytes += p->size;
    if (ts > h->last_ts || h->count == 1) h->last_ts = ts;
}
static double q_first_ts(PQueue *q) { return q->count ? q->items[q->head].ts : q->last_ts; }

static double now_position_locked(void) {
    double pos = P.clock_pts;
    if (P.started && !P.paused && !P.buffering && P.state == PS_PLAYING)
        pos += (SDL_GetTicks() - P.clock_ticks) / 1000.0 * P.speed;
    pos -= (P.latency + P.user_offset) * P.speed;   /* what the listener hears now */
    if (pos < 0) pos = 0;
    return pos;
}

static int interrupt_cb(void *opaque) {
    Demux *d = opaque;
    return atomic_load(&P.quit) || (d && (d->seek_pending || d->stop));
}

/* ---------- audio ---------- */
static void audio_callback(void *u, Uint8 *stream, int len) {
    (void)u;
    float *out = (float *)stream; int frames = len / (int)(sizeof(float) * 2), done = 0;
    pthread_mutex_lock(&P.lock);
    bool run = P.active && P.started && !P.paused && !P.buffering && P.state == PS_PLAYING;
    if (run) {
        while (done < frames && P.rcount > 0) {
            int n = frames - done; if (n > P.rcount) n = P.rcount;
            int till_wrap = RING_FRAMES - P.rr; if (n > till_wrap) n = till_wrap;
            if (P.scount) { Seg *s = &P.seg[P.shead]; if (n > s->frames) n = s->frames; }
            memcpy(out + done * 2, P.ring + P.rr * 2, n * 2 * sizeof(float));
            P.rr = (P.rr + n) % RING_FRAMES; P.rcount -= n; done += n;
            if (P.scount) {
                Seg *s = &P.seg[P.shead];
                s->pts += n / (double)OUT_RATE * s->speed; s->frames -= n;
                P.clock_pts = s->pts;
                if (s->frames <= 0) { P.shead = (P.shead + 1) % SEGS; P.scount--; }
            }
        }
        P.clock_ticks = SDL_GetTicks();
        { float g = P.gain * P.fade; if (g < 0.999f) for (int i = 0; i < done * 2; i++) out[i] *= g; }
        if (done < frames) {
            if (P.aq.eof && P.audio_eof_done && !P.rcount) P.ended = true;
            else if (!P.live || P.rcount == 0) { P.buffering = true; Log_msg("underrun at %.2f s", P.clock_pts); }
        }
        pthread_cond_broadcast(&P.cond);
    }
    pthread_mutex_unlock(&P.lock);
    if (done < frames) memset(out + done * 2, 0, (frames - done) * 2 * sizeof(float));
}

static void push_pcm(const float *pcm, int frames, double pts, float speed, int serial) {
    int off = 0;
    while (off < frames) {
        pthread_mutex_lock(&P.lock);
        while (!atomic_load(&P.quit) && P.serial == serial && P.rcount >= RING_FRAMES - 256) pthread_cond_wait(&P.cond, &P.lock);
        if (atomic_load(&P.quit) || P.serial != serial) { pthread_mutex_unlock(&P.lock); return; }
        int n = frames - off, room = RING_FRAMES - P.rcount; if (n > room) n = room;
        int till_wrap = RING_FRAMES - P.rw; if (n > till_wrap) n = till_wrap;
        memcpy(P.ring + P.rw * 2, pcm + off * 2, n * 2 * sizeof(float));
        P.rw = (P.rw + n) % RING_FRAMES; P.rcount += n;
        double seg_pts = pts + off / (double)OUT_RATE * speed;
        if (P.scount && P.seg[(P.shead + P.scount - 1) % SEGS].speed == speed &&
            fabs(P.seg[(P.shead + P.scount - 1) % SEGS].pts + P.seg[(P.shead + P.scount - 1) % SEGS].frames / (double)OUT_RATE * speed - seg_pts) < 0.002) {
            P.seg[(P.shead + P.scount - 1) % SEGS].frames += n;
        } else if (P.scount < SEGS) {
            P.seg[(P.shead + P.scount) % SEGS] = (Seg){seg_pts, n, speed}; P.scount++;
        } else P.seg[(P.shead + P.scount - 1) % SEGS].frames += n;
        if (!P.started && P.scount == 1 && P.rcount == n) P.clock_pts = seg_pts;
        P.next_pts = seg_pts + n / (double)OUT_RATE * speed;
        off += n;
        pthread_cond_broadcast(&P.cond);
        pthread_mutex_unlock(&P.lock);
    }
}

static void *audio_thread(void *u) {
    (void)u;
    AVFrame *fr = av_frame_alloc();
    float *buf = NULL; int buf_cap = 0;
    float *sbuf = NULL; int sbuf_cap = 0;
    int my_serial = -1;
    for (;;) {
        pthread_mutex_lock(&P.lock);
        while (!atomic_load(&P.quit) && (P.snap_pending || (!P.aq.count && !(P.aq.eof && !P.audio_eof_done)))) pthread_cond_wait(&P.cond, &P.lock);
        if (atomic_load(&P.quit)) { pthread_mutex_unlock(&P.lock); break; }
        int serial = 0; double pts_in = -1; AVPacket *pkt = q_pop(&P.aq, &serial, &pts_in);
        if (pkt && serial == P.serial) hist_push(&P.ah, av_packet_clone(pkt), pts_in);
        bool flush_eof = !pkt && P.aq.eof;
        int cur = P.serial; double drop = P.drop_a; float speed = P.speed;
        pthread_cond_broadcast(&P.cond);
        pthread_mutex_unlock(&P.lock);
        if (pkt && serial != cur) { av_packet_free(&pkt); continue; }
        if (cur != my_serial) { avcodec_flush_buffers(P.adec); if (P.sonic) sonicFlushStream(P.sonic), sonicDestroyStream(P.sonic); P.sonic = NULL; my_serial = cur; }
        int r = avcodec_send_packet(P.adec, pkt);  /* NULL pkt drains at EOF */
        av_packet_free(&pkt);
        if (r < 0 && r != AVERROR(EAGAIN) && r != AVERROR_EOF) continue;
        while (avcodec_receive_frame(P.adec, fr) == 0) {
            double pts = fr->best_effort_timestamp == AV_NOPTS_VALUE ? -1 : fr->best_effort_timestamp * av_q2d(P.atb);
            if (pts >= 0 && pts + fr->nb_samples / (double)fr->sample_rate < drop) { av_frame_unref(fr); continue; }
            int out_max = swr_get_out_samples(P.swr, fr->nb_samples) + 256;
            if (out_max > buf_cap) { buf_cap = out_max; buf = realloc(buf, buf_cap * 2 * sizeof(float)); }
            uint8_t *outp[1] = {(uint8_t *)buf};
            int got = swr_convert(P.swr, outp, out_max, (const uint8_t **)fr->extended_data, fr->nb_samples);
            av_frame_unref(fr);
            if (got <= 0) continue;
            double start_pts = pts >= 0 ? pts : P.next_pts;
            /* trim the part before a seek target */
            if (start_pts < drop) { int skip = (int)((drop - start_pts) * OUT_RATE); if (skip >= got) continue; memmove(buf, buf + skip * 2, (got - skip) * 2 * sizeof(float)); got -= skip; start_pts = drop; }
            if (fabsf(speed - 1.0f) < 0.01f) { push_pcm(buf, got, start_pts, 1.0f, cur); continue; }
            if (!P.sonic) { P.sonic = sonicCreateStream(OUT_RATE, 2); sonicSetSpeed(P.sonic, speed); }
            else if (sonicGetSpeed(P.sonic) != speed) sonicSetSpeed(P.sonic, speed);
            sonicWriteFloatToStream(P.sonic, buf, got);
            int avail = sonicSamplesAvailable(P.sonic);
            if (avail > sbuf_cap) { sbuf_cap = avail + 1024; sbuf = realloc(sbuf, sbuf_cap * 2 * sizeof(float)); }
            int out = sonicReadFloatFromStream(P.sonic, sbuf, sbuf_cap);
            pthread_mutex_lock(&P.lock); double base = P.started || P.scount ? P.next_pts : start_pts; pthread_mutex_unlock(&P.lock);
            if (out > 0) push_pcm(sbuf, out, base, speed, cur);
        }
        if (flush_eof) {
            pthread_mutex_lock(&P.lock); P.audio_eof_done = true; pthread_cond_broadcast(&P.cond); pthread_mutex_unlock(&P.lock);
            Log_msg("audio decode reached end");
        }
    }
    av_frame_free(&fr); free(buf); free(sbuf);
    return NULL;
}

/* ---------- video ---------- */
static void *video_thread(void *u) {
    (void)u;
    AVFrame *fr = av_frame_alloc();
    int my_serial = -1, my_gen = -1;
    bool eof_sent = false;
    for (;;) {
        pthread_mutex_lock(&P.lock);
        while (!atomic_load(&P.quit) && (P.vswitch || (!P.vq.count && !(P.vq.eof && !eof_sent)))) {
            if (P.vswitch && !P.vparked) { P.vparked = true; pthread_cond_broadcast(&P.cond); }
            pthread_cond_wait(&P.cond, &P.lock);
        }
        P.vparked = false;
        if (atomic_load(&P.quit)) { pthread_mutex_unlock(&P.lock); break; }
        if (my_gen != P.vgen) { my_gen = P.vgen; my_serial = -1; eof_sent = false; }
        if (!P.vdec) { eof_sent = true; pthread_mutex_unlock(&P.lock); continue; }
        int serial = 0; double pts_in = -1; AVPacket *pkt = q_pop(&P.vq, &serial, &pts_in);
        if (pkt && serial == P.serial) hist_push(&P.vh, av_packet_clone(pkt), pts_in);
        int cur = P.serial; double drop = P.drop_v;
        double behind = pkt ? (now_position_locked() - pkt->pts * av_q2d(P.vtb)) : 0;
        bool late = P.started && P.state == PS_PLAYING && !P.paused && !P.buffering && P.fcount == 0;
        pthread_cond_broadcast(&P.cond);
        pthread_mutex_unlock(&P.lock);
        if (!pkt) eof_sent = true;
        if (pkt && serial != cur) { av_packet_free(&pkt); continue; }
        if (cur != my_serial) { avcodec_flush_buffers(P.vdec); my_serial = cur; eof_sent = false; P.degrade = 0; P.vdec->skip_loop_filter = AVDISCARD_DEFAULT; P.vdec->skip_frame = AVDISCARD_DEFAULT; }
        /* Adaptive quality: when decoding falls behind the audio clock, skip the
         * deblocking filter first, then non-reference frames, instead of
         * letting picture and sound drift apart. */
        if (pkt && late && behind > 0.25 && P.degrade < 2) {
            P.degrade++; P.boost = true; Log_msg("decoder behind %.2fs: degrade level %d", behind, P.degrade);
            /* "real 60 fps" asked but this device cannot keep up with this video: back to the light mode */
            if (P.degrade >= 2 && P.hifps && !P.cap30) { P.cap30 = true; Log_msg("60 fps: decoder cannot keep up, showing up to 30 pictures a second"); }
        }
        else if (pkt && P.degrade > 0 && behind < -0.5 && !P.hifps) { P.degrade = 0; }
        /* far behind the sound: stop decoding until the next keyframe and rejoin there,
         * instead of showing pictures that trail the audio */
        if (pkt && P.started && !P.paused && behind > 0.7 && !(pkt->flags & AV_PKT_FLAG_KEY)) {
            if (!P.resyncing) { P.resyncing = true; Log_msg("video %.2fs behind: skipping to the next keyframe", behind); }
            if (P.hifps && !P.cap30) { P.cap30 = true; P.degrade = 2; Log_msg("60 fps: decoder cannot keep up, showing up to 30 pictures a second"); }
            pthread_mutex_lock(&P.lock); P.dropped++; pthread_mutex_unlock(&P.lock);
            av_packet_free(&pkt); continue;
        }
        if (pkt && P.resyncing && (pkt->flags & AV_PKT_FLAG_KEY)) P.resyncing = false;
        /* 1080p+ on four A53 cores: deblock only reference frames from the start */
        /* 60 fps on four A53 cores: no deblocking and no non-reference pictures from the
         * start (about half the work); shown at up to 30 fps, in step with the sound */
        P.vdec->skip_loop_filter = P.degrade >= 1 || P.hifps ? AVDISCARD_ALL : P.vdec->height >= 1000 ? AVDISCARD_NONREF : AVDISCARD_DEFAULT;
        P.vdec->skip_frame = P.cap30 || (P.degrade >= 2 && behind > 0.4) ? AVDISCARD_NONREF : AVDISCARD_DEFAULT;
        int r = avcodec_send_packet(P.vdec, pkt);
        av_packet_free(&pkt);
        if (r < 0 && r != AVERROR(EAGAIN) && r != AVERROR_EOF) continue;
        while (avcodec_receive_frame(P.vdec, fr) == 0) {
            double pts = fr->best_effort_timestamp == AV_NOPTS_VALUE ? -1 : fr->best_effort_timestamp * av_q2d(P.vtb);
            if (pts >= 0 && pts < drop - 0.001) { av_frame_unref(fr); continue; }
            pthread_mutex_lock(&P.lock);
            P.decoded++;
            while (!atomic_load(&P.quit) && P.serial == cur && P.fcount >= VFRAMES && !P.vswitch) pthread_cond_wait(&P.cond, &P.lock);
            if (atomic_load(&P.quit) || P.serial != cur || P.vswitch) { pthread_mutex_unlock(&P.lock); av_frame_unref(fr); break; }
            int slot = (P.fhead + P.fcount) % VFRAMES;
            if (!P.frames[slot]) P.frames[slot] = av_frame_alloc();
            av_frame_move_ref(P.frames[slot], fr);
            P.fpts[slot] = pts >= 0 ? pts : (P.fcount ? P.fpts[(slot + VFRAMES - 1) % VFRAMES] + 1.0 / (P.fps > 0 ? P.fps : 30) : 0);
            P.fcount++;
            P.width = P.frames[slot]->width; P.height = P.frames[slot]->height;
            pthread_cond_broadcast(&P.cond);
            pthread_mutex_unlock(&P.lock);
        }
    }
    av_frame_free(&fr);
    return NULL;
}

/* ---------- demux ---------- */
static double pkt_ts(AVFormatContext *f, AVPacket *p) {
    AVRational tb = f->streams[p->stream_index]->time_base;
    int64_t t = p->pts != AV_NOPTS_VALUE ? p->pts : p->dts;
    return t == AV_NOPTS_VALUE ? -1 : t * av_q2d(tb);
}
static void *demux_thread(void *u) {
    Demux *d = u;
    int errors = 0;
    for (;;) {
        pthread_mutex_lock(&P.lock);
        for (;;) {
            if (atomic_load(&P.quit) || d->seek_pending || d->stop) break;
            bool vfull = d->vidx >= 0 && ((P.vq.last_ts - now_position_locked() > PKT_AHEAD_SEC && P.vq.count > 2) || P.vq.bytes > PKT_MAX_BYTES || P.vq.count >= P.vq.cap - 8);
            bool afull = d->aidx >= 0 && ((P.aq.last_ts - now_position_locked() > PKT_AHEAD_SEC && P.aq.count > 2) || P.aq.count >= P.aq.cap - 8);
            bool done = (d->vidx < 0 || P.vq.eof) && (d->aidx < 0 || P.aq.eof);
            if (!vfull && !afull && !done) break;
            struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts); ts.tv_nsec += 100000000; if (ts.tv_nsec >= 1000000000) { ts.tv_sec++; ts.tv_nsec -= 1000000000; }
            pthread_cond_timedwait(&P.cond, &P.lock, &ts);
        }
        if (atomic_load(&P.quit) || d->stop) { pthread_mutex_unlock(&P.lock); break; }
        if (d->seek_pending) {
            double to = d->seek_to;
            pthread_mutex_unlock(&P.lock);
            int64_t ts = (int64_t)(to * AV_TIME_BASE);
            d->seek_pending = false;  /* allow IO again */
            int r = avformat_seek_file(d->fmt, -1, INT64_MIN, ts, ts, 0);
            if (r < 0) r = av_seek_frame(d->fmt, -1, ts, AVSEEK_FLAG_BACKWARD);
            Log_msg("seek to %.2f: %s", to, r < 0 ? av_err2str(r) : "ok");
            pthread_mutex_lock(&P.lock);
            if (d->vidx >= 0) { P.vq.eof = false; }
            if (d->aidx >= 0) { P.aq.eof = false; P.audio_eof_done = false; }
            pthread_mutex_unlock(&P.lock);
            errors = 0;
            continue;
        }
        int dser = P.dserial;
        pthread_mutex_unlock(&P.lock);
        AVPacket *pkt = av_packet_alloc();
        int r = av_read_frame(d->fmt, pkt);
        if (r < 0) {
            av_packet_free(&pkt);
            if (d->stop) break;
            if (r == AVERROR_EXIT || d->seek_pending || atomic_load(&P.quit)) continue;
            if (r == AVERROR(EAGAIN)) { av_usleep(10000); continue; }
            if (r != AVERROR_EOF && ++errors < 6 && !(d->fmt->pb && avio_feof(d->fmt->pb))) { Log_msg("read error: %s (retry)", av_err2str(r)); av_usleep(500000); continue; }
            Log_msg("demux end: %s", av_err2str(r));
            pthread_mutex_lock(&P.lock);
            if (d->vidx >= 0) { P.vq.eof = true; P.snap_pending = false; }
            if (d->aidx >= 0) P.aq.eof = true;
            if (r != AVERROR_EOF && !P.vq.count && !P.aq.count && !P.started) { P.state = PS_ERROR; snprintf(P.error, sizeof(P.error), "Mất kết nối luồng video"); }
            pthread_cond_broadcast(&P.cond);
            pthread_mutex_unlock(&P.lock);
            continue;
        }
        errors = 0;
        double ts = pkt_ts(d->fmt, pkt);
        pthread_mutex_lock(&P.lock);
        if (P.dserial != dser || d->seek_pending) { pthread_mutex_unlock(&P.lock); av_packet_free(&pkt); continue; }
        if (pkt->stream_index == d->vidx && P.snap_pending) {
            /* first picture after a network seek: start everything at this keyframe instead of
             * decoding up to the exact target (that is what made seeking feel slow) */
            double k = ts, target = P.snap_t;
            if (k >= 0 && k <= P.snap_t + 0.01 && k >= P.snap_t - 6.5 && !(P.snap_forward && k <= P.snap_pos + 0.5)) target = k;
            P.drop_v = P.drop_a = target; P.clock_pts = P.next_pts = target; P.clock_ticks = SDL_GetTicks();
            P.snap_pending = false;
            Log_msg("seek lands at %.2f (asked %.2f)", target, P.snap_t);
        }
        if (pkt->stream_index == d->vidx) { if (!q_push(&P.vq, pkt, P.serial, ts)) av_packet_free(&pkt); }
        else if (pkt->stream_index == d->aidx) { if (!q_push(&P.aq, pkt, P.serial, ts)) av_packet_free(&pkt); }
        else av_packet_free(&pkt);
        pthread_cond_broadcast(&P.cond);
        pthread_mutex_unlock(&P.lock);
    }
    return NULL;
}

/* ---------- open ---------- */
static AVDictionary *http_opts(void) {
    AVDictionary *o = NULL;
    av_dict_set(&o, "ca_file", paths.cacert, 0);
    av_dict_set(&o, "tls_verify", "1", 0);
    av_dict_set(&o, "rw_timeout", "20000000", 0);
    av_dict_set(&o, "timeout", "20000000", 0);
    av_dict_set(&o, "reconnect", "1", 0);
    /* no automatic retry loops on connect/handshake errors: they cost up to ~15 s
     * before an error surfaces; ChunkIO reopens by itself and falls back quickly */
    av_dict_set(&o, "reconnect_on_network_error", "0", 0);
    av_dict_set(&o, "reconnect_delay_max", "2", 0);
    if (P.s_ua && *P.s_ua) av_dict_set(&o, "user_agent", P.s_ua, 0);
    if (P.s_headers && *P.s_headers) av_dict_set(&o, "headers", P.s_headers, 0);
    return o;
}
/* An HLS master holds every quality. Keep only the one asked for (P.src.want_*) plus the sound that
 * goes with it, before reading any data: FFmpeg then downloads just those playlists (otherwise it
 * picks a quality by itself, often 240p, and fetches all of them). */
static void close_demux(Demux *d);
static void pick_variant(Demux *d) {
    AVFormatContext *f = d->fmt;
    int nv = 0, best = -1; long best_key = -1;
    for (unsigned i = 0; i < f->nb_streams; i++) if (f->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) nv++;
    if (nv < 2 || P.src.want_height <= 0) return;
    for (unsigned i = 0; i < f->nb_streams; i++) {
        AVStream *st = f->streams[i];
        if (st->codecpar->codec_type != AVMEDIA_TYPE_VIDEO) continue;
        int h = st->codecpar->height;
        AVDictionaryEntry *e = av_dict_get(st->metadata, "variant_bitrate", NULL, 0);
        long bw = e ? atol(e->value) : 0;
        double fps = av_q2d(st->avg_frame_rate) > 0 ? av_q2d(st->avg_frame_rate) : av_q2d(st->r_frame_rate);
        long key;
        if (P.src.want_bw > 0 && bw == P.src.want_bw) key = 1L << 40;                 /* the exact entry of the list */
        else if (h > 0 && h <= P.src.want_height) key = (long)h * 4 + ((fps > 30) == (P.src.want_fps > 30) ? 2 : 1);
        else if (h > 0) key = 0;                                                         /* larger than asked: last choice */
        else key = bw > 0 && bw <= P.src.want_bw ? 1 + bw / 100000 : 0;
        if (key > best_key || (key == best_key && best >= 0 && h > 0 && h < f->streams[best]->codecpar->height)) { best_key = key; best = (int)i; }
    }
    if (best < 0) return;
    d->vidx = best;
    d->aidx = av_find_best_stream(f, AVMEDIA_TYPE_AUDIO, -1, best, NULL, 0);
    for (unsigned i = 0; i < f->nb_streams; i++) if ((int)i != d->vidx && (int)i != d->aidx) f->streams[i]->discard = AVDISCARD_ALL;
    Log_msg("hls: kept stream %d of %u for %dp (%s), sound stream %d", best, f->nb_streams, P.src.want_height,
            best_key == 1L << 40 ? "exact" : "closest", d->aidx);
}
static int open_demux(Demux *d, const char *url, long long size, bool chunked, bool local) {
    d->vidx = d->aidx = -1;
    d->fmt = avformat_alloc_context();
    d->fmt->interrupt_callback.callback = interrupt_cb;
    d->fmt->interrupt_callback.opaque = d;
    d->fmt->probesize = 2 << 20;
    AVDictionary *o = local ? NULL : http_opts();
    if (chunked) {
        d->cio = calloc(1, sizeof(ChunkIO));
        d->cio->url = strdup(url); d->cio->size = size; d->cio->icb = &d->fmt->interrupt_callback;
        av_dict_copy(&d->cio->opts, o, 0);
        unsigned char *iob = av_malloc(65536);
        d->pb = avio_alloc_context(iob, 65536, 0, d->cio, chunk_read, NULL, chunk_seek);
        d->fmt->pb = d->pb;
        d->fmt->flags |= AVFMT_FLAG_CUSTOM_IO;
        av_dict_free(&o);
        o = NULL;
    }
    int r = avformat_open_input(&d->fmt, chunked ? NULL : url, NULL, &o);
    av_dict_free(&o);
    if (r < 0) { Log_msg("open input failed: %s", av_err2str(r)); return r; }
    bool need_info = !chunked;
    if (!chunked && !local) pick_variant(d);
    if (need_info) { d->fmt->max_analyze_duration = 2 * AV_TIME_BASE; r = avformat_find_stream_info(d->fmt, NULL); if (r < 0) Log_msg("stream info: %s", av_err2str(r)); }
    if (d->vidx < 0) d->vidx = av_find_best_stream(d->fmt, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);
    if (d->aidx < 0) d->aidx = av_find_best_stream(d->fmt, AVMEDIA_TYPE_AUDIO, -1, d->vidx >= 0 ? d->vidx : -1, NULL, 0);
    if (d->aidx < 0) d->aidx = av_find_best_stream(d->fmt, AVMEDIA_TYPE_AUDIO, -1, -1, NULL, 0);
    if (d->vidx < 0) d->vidx = -1;
    if (d->aidx < 0) d->aidx = -1;
    for (unsigned i = 0; i < d->fmt->nb_streams; i++) if ((int)i != d->vidx && (int)i != d->aidx) d->fmt->streams[i]->discard = AVDISCARD_ALL;
    return 0;
}
static int open_decoder(AVStream *st, AVCodecContext **out, bool video) {
    const AVCodec *c = avcodec_find_decoder(st->codecpar->codec_id);
    if (!c) return AVERROR_DECODER_NOT_FOUND;
    AVCodecContext *ctx = avcodec_alloc_context3(c);
    avcodec_parameters_to_context(ctx, st->codecpar);
    ctx->pkt_timebase = st->time_base;
    if (video) {
        ctx->thread_count = 4;
        ctx->thread_type = FF_THREAD_FRAME | FF_THREAD_SLICE;
        ctx->flags2 |= AV_CODEC_FLAG2_FAST;
        if (st->codecpar->height >= 1080) ctx->skip_loop_filter = AVDISCARD_NONREF;
    }
    int r = avcodec_open2(ctx, c, NULL);
    if (r < 0) { avcodec_free_context(&ctx); return r; }
    *out = ctx;
    return 0;
}

typedef struct { Demux *d; const char *url; long long size; int result; } ParOpen;
static void *par_open(void *u) { ParOpen *p = u; p->result = open_demux(p->d, p->url, p->size, true, false); return NULL; }
static void *opener(void *u) {
    (void)u;
    const PlayerSource *s = &P.src;
    int r = 0;
    double t0 = SDL_GetTicks() / 1000.0;
    if (s->local_video || s->local_audio) {
        if (s->local_video && !s->audio_only) r = open_demux(&P.dx[0], s->local_video, 0, false, true);
        if (r >= 0 && s->local_audio) r = open_demux(&P.dx[1], s->local_audio, 0, false, true);
    } else if (s->muxed_url) {
        r = open_demux(&P.dx[0], s->muxed_url, 0, false, false);
        if ((r < 0 || (P.dx[0].aidx < 0 && !s->audio_only) || (P.dx[0].aidx < 0 && P.dx[0].vidx < 0)) && s->fallback_url && !atomic_load(&P.quit)) {
            Log_msg("open: %s, trying the master playlist", r < 0 ? av_err2str(r) : "no sound in this playlist");
            close_demux(&P.dx[0]);
            r = open_demux(&P.dx[0], s->fallback_url, 0, false, false);
        }
    } else {
        /* open video and audio connections at the same time (TLS + moov in parallel) */
        bool want_v = s->video_url && !s->audio_only;
        ParOpen po = {&P.dx[0], s->video_url, s->video_size, 0};
        pthread_t th; bool threaded = want_v && s->audio_url && !pthread_create(&th, NULL, par_open, &po);
        int rv = 0, ra = 0;
        if (want_v && !threaded) rv = open_demux(&P.dx[0], s->video_url, s->video_size, true, false);
        if (s->audio_url) ra = open_demux(&P.dx[1], s->audio_url, s->audio_size, true, false);
        if (threaded) { pthread_join(th, NULL); rv = po.result; }
        r = rv < 0 ? rv : ra;
        if (r < 0) Log_msg("parallel open: video %d audio %d", rv, ra);
    }
    if (atomic_load(&P.quit)) return NULL;
    /* choose one video and one audio stream across the inputs */
    Demux *vd = NULL, *ad = NULL;
    P.ndx = 2;
    for (int i = 0; i < 2; i++) {
        if (!P.dx[i].fmt) continue;
        if (!vd && P.dx[i].vidx >= 0 && !s->audio_only) vd = &P.dx[i]; else P.dx[i].vidx = -1;
    }
    for (int i = 1; i >= 0; i--) { if (!P.dx[i].fmt) continue; if (!ad && P.dx[i].aidx >= 0) ad = &P.dx[i]; else P.dx[i].aidx = -1; }
    if (vd && vd->vidx >= 0) vd->fmt->streams[vd->vidx]->discard = AVDISCARD_DEFAULT;
    for (int i = 0; i < 2; i++) if (P.dx[i].fmt) for (unsigned k = 0; k < P.dx[i].fmt->nb_streams; k++)
        if ((int)k != P.dx[i].vidx && (int)k != P.dx[i].aidx) P.dx[i].fmt->streams[k]->discard = AVDISCARD_ALL;
    if (r >= 0 && !ad) r = AVERROR_STREAM_NOT_FOUND;
    if (r >= 0 && vd) r = open_decoder(vd->fmt->streams[vd->vidx], &P.vdec, true);
    if (r >= 0) r = open_decoder(ad->fmt->streams[ad->aidx], &P.adec, false);
    if (r >= 0) {
        AVChannelLayout stereo = AV_CHANNEL_LAYOUT_STEREO;
        AVChannelLayout in = P.adec->ch_layout;
        if (!in.nb_channels) av_channel_layout_default(&in, 2);
        r = swr_alloc_set_opts2(&P.swr, &stereo, AV_SAMPLE_FMT_FLT, OUT_RATE, &in, P.adec->sample_fmt, P.adec->sample_rate, 0, NULL);
        if (r >= 0) r = swr_init(P.swr);
    }
    if (r < 0) {
        pthread_mutex_lock(&P.lock);
        P.state = PS_ERROR;
        snprintf(P.error, sizeof(P.error), "Không mở được luồng (%s)", av_err2str(r));
        pthread_mutex_unlock(&P.lock);
        Log_msg("open failed: %s", av_err2str(r));
        return NULL;
    }
    pthread_mutex_lock(&P.lock);
    P.has_video = vd != NULL; P.has_audio = true;
    if (vd) {
        AVStream *st = vd->fmt->streams[vd->vidx];
        P.vtb = st->time_base;
        P.fps = av_q2d(st->avg_frame_rate) > 0 ? av_q2d(st->avg_frame_rate) : s->video_fps;
        P.hifps = P.fps > 40 || (P.fps > 31 && st->codecpar->height >= 1000); P.cap30 = P.hifps && !P.full60;
        snprintf(P.vcodec, sizeof(P.vcodec), "%s", avcodec_get_name(st->codecpar->codec_id));
        P.width = st->codecpar->width; P.height = st->codecpar->height;
    }
    AVStream *ast = ad->fmt->streams[ad->aidx];
    P.atb = ast->time_base;
    snprintf(P.acodec, sizeof(P.acodec), "%s %d kHz", avcodec_get_name(ast->codecpar->codec_id), ast->codecpar->sample_rate / 1000);
    double dur = 0;
    for (int i = 0; i < 2; i++) {
        AVFormatContext *f = P.dx[i].fmt;
        if (!f) continue;
        if (f->duration > 0 && f->duration != AV_NOPTS_VALUE) { double d2 = f->duration / (double)AV_TIME_BASE; if (d2 > dur) dur = d2; }
        for (unsigned k = 0; k < f->nb_streams; k++) {   /* stream info is skipped for DASH, so read the track durations */
            AVStream *st = f->streams[k];
            if (st->duration > 0 && st->duration != AV_NOPTS_VALUE) { double d2 = st->duration * av_q2d(st->time_base); if (d2 > dur) dur = d2; }
        }
    }
    if (dur <= 0 && s->duration_hint > 0) dur = s->duration_hint;
    if (dur > 0 && !P.live) P.duration = dur;
    if (s->start > 0.5 && !P.live) {
        bool two = P.ndx == 2 && vd;          /* resume: start at the keyframe before the saved spot */
        for (int i = 0; i < 2; i++) if (P.dx[i].fmt) { P.dx[i].seek_pending = true; P.dx[i].seek_to = two && i == 1 ? fmax(0, s->start - 6.5) : s->start; }
        if (vd) { P.snap_pending = true; P.snap_t = s->start; P.snap_pos = 0; P.snap_forward = false; }
    }
    P.drop_v = P.drop_a = P.live ? 0 : s->start;
    P.clock_pts = P.next_pts = P.live ? 0 : s->start;
    pthread_mutex_unlock(&P.lock);
    for (int i = 0; i < 2; i++) if (P.dx[i].fmt) { pthread_create(&P.dx[i].thread, NULL, demux_thread, &P.dx[i]); P.dx[i].started = true; }
    if (P.vdec) { pthread_create(&P.vthread, NULL, video_thread, NULL); P.vthread_on = true; }
    pthread_create(&P.athread, NULL, audio_thread, NULL); P.athread_on = true;
    Log_msg("opened in %.2fs: video %s %dx%d %.0ffps, audio %s, duration %.1f", SDL_GetTicks() / 1000.0 - t0, P.vcodec, P.width, P.height, P.fps, P.acodec, P.duration);
    return NULL;
}

static char *sdup(const char *s) { return s ? strdup(s) : NULL; }

static int audio_mode;
#ifdef NXT_TUBE_PW
/* muOS / Knulli: PipeWire owns the cards, so we go through it (its own SDL
 * backend, or its PulseAudio server) and never open hw: devices ourselves.
 * A USB DAC is picked by device name when the setting asks for it. */
static bool open_device(void) {
    static const char *drivers[] = {NULL, "pipewire", "pulseaudio", "alsa", "alsa"};
    const char *forced = getenv("SDL_AUDIODRIVER");
    char forced_copy[64] = ""; if (forced) snprintf(forced_copy, sizeof(forced_copy), "%s", forced);
    for (int d = forced_copy[0] ? 0 : 1; d < 5; d++) {
        if (SDL_WasInit(SDL_INIT_AUDIO)) SDL_QuitSubSystem(SDL_INIT_AUDIO);
        if (d == 0) setenv("SDL_AUDIODRIVER", forced_copy, 1); else setenv("SDL_AUDIODRIVER", drivers[d], 1);
        if (d == 4) setenv("AUDIODEV", "pipewire", 1);      /* ALSA plugin into PipeWire (muOS ships it) */
        if (SDL_InitSubSystem(SDL_INIT_AUDIO)) { Log_msg("audio driver %s: %s", d ? drivers[d] : forced_copy, SDL_GetError()); continue; }
        const char *drv = SDL_GetCurrentAudioDriver(); if (!drv) drv = "?";
        bool via_pw = !strcmp(drv, "pipewire") || !strcmp(drv, "pulseaudio");
        /* USB DAC by name (auto / DAC modes) */
        const char *pick = NULL; char name[256] = "";
        if (audio_mode != 1 && AudioRoute_usbCard() >= 0 && via_pw) {
            int n = SDL_GetNumAudioDevices(0);
            for (int i = 0; i < n && !pick; i++) {
                const char *nm = SDL_GetAudioDeviceName(i, 0);
                if (nm && (strstr(nm, "USB") || strstr(nm, "usb") || (AudioRoute_dacName()[0] && strstr(nm, AudioRoute_dacName())))) { snprintf(name, sizeof(name), "%s", nm); pick = name; }
            }
        }
        for (int attempt = 0; attempt < 2; attempt++) {
            const char *dev = attempt ? NULL : pick;
            if (attempt && !pick) break;
            SDL_AudioSpec want = {0}, have;
            want.freq = OUT_RATE; want.format = AUDIO_F32SYS; want.channels = 2; want.samples = 1024; want.callback = audio_callback;
            P.dev = SDL_OpenAudioDevice(dev, 0, &want, &have, 0);
            if (!P.dev) { Log_msg("audio open failed (%s, %s): %s", drv, dev ? dev : "default", SDL_GetError()); continue; }
            /* the PipeWire graph adds about one quantum on top of our buffer */
            P.latency = 2.5 * have.samples / (have.freq > 0 ? have.freq : OUT_RATE) + (via_pw ? 0.06 : 0.04);
            P.usb_out = dev != NULL;
            Log_msg("audio out %s via %s: %d Hz, %d frames/buffer, latency estimate %.0f ms", dev ? dev : "system default", drv, have.freq, have.samples, P.latency * 1000);
            SDL_PauseAudioDevice(P.dev, 0);
            if (!forced_copy[0]) setenv("SDL_AUDIODRIVER", drv, 1);   /* reopen with the same backend */
            return true;
        }
    }
    if (forced_copy[0]) setenv("SDL_AUDIODRIVER", forced_copy, 1); else unsetenv("SDL_AUDIODRIVER");
    return false;
}
#else
static bool open_device(void) {
    for (int attempt = 0; attempt < 2; attempt++) {
        int mode = attempt ? 1 : audio_mode;   /* fall back to the speaker */
        if (SDL_WasInit(SDL_INIT_AUDIO)) SDL_QuitSubSystem(SDL_INIT_AUDIO);
        int card = AudioRoute_prepare(mode);
        if (SDL_InitSubSystem(SDL_INIT_AUDIO)) { Log_msg("audio init: %s", SDL_GetError()); continue; }
        SDL_AudioSpec want = {0}, have;
        want.freq = OUT_RATE; want.format = AUDIO_F32SYS; want.channels = 2; want.samples = 1024; want.callback = audio_callback;
        P.dev = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
        if (!P.dev) { Log_msg("audio open failed (%s): %s", card >= 0 ? "USB DAC" : "speaker", SDL_GetError()); if (card < 0) return false; continue; }
        /* one SDL buffer being played + about one more queued in the driver */
        P.latency = 2.5 * have.samples / (have.freq > 0 ? have.freq : OUT_RATE) + (card >= 0 ? 0.07 : 0.04);
        P.usb_out = card >= 0;
        Log_msg("audio out %s: %d Hz, %d frames/buffer, latency estimate %.0f ms", card >= 0 ? "USB DAC" : "speaker", have.freq, have.samples, P.latency * 1000);
        SDL_PauseAudioDevice(P.dev, 0);
        return true;
    }
    return false;
}
#endif
bool Player_init(int mode) {
    P.gain = 1.0f; P.fade = 1.0f;
    pthread_mutex_init(&P.lock, NULL);
    pthread_cond_init(&P.cond, NULL);
    avformat_network_init();
    av_log_set_level(getenv("NXT_TUBE_AVLOG") ? atoi(getenv("NXT_TUBE_AVLOG")) : AV_LOG_ERROR);
    P.ring = calloc(RING_FRAMES * 2, sizeof(float));
    P.speed = 1.0f;
    audio_mode = mode;
    return open_device();
}
/* Re-route sound (speaker <-> USB DAC); playback continues from the ring. */
bool Player_setAudioMode(int mode) {
    audio_mode = mode;
    if (P.dev) { SDL_CloseAudioDevice(P.dev); P.dev = 0; }
    return open_device();
}
bool Player_usbOutput(void) { return P.usb_out; }
void Player_quit(void) {
    Player_close();
    if (P.dev) SDL_CloseAudioDevice(P.dev);
    P.dev = 0;
    free(P.ring); P.ring = NULL;
    avformat_network_deinit();
}

void Player_close(void) {
    if (!P.active) return;
    atomic_store(&P.quit, true);
    pthread_mutex_lock(&P.lock); pthread_cond_broadcast(&P.cond); pthread_mutex_unlock(&P.lock);
    if (P.opener_started) pthread_join(P.opener, NULL);
    if (P.switcher_on) { pthread_join(P.switcher, NULL); P.switcher_on = false; }
    for (int i = 0; i < 2; i++) if (P.dx[i].started) pthread_join(P.dx[i].thread, NULL);
    if (P.vthread_on) pthread_join(P.vthread, NULL);
    if (P.athread_on) pthread_join(P.athread, NULL);
    pthread_mutex_lock(&P.lock);
    P.active = false;
    for (int i = 0; i < 2; i++) {
        Demux *d = &P.dx[i];
        if (d->fmt) avformat_close_input(&d->fmt);
        if (d->pb) { av_freep(&d->pb->buffer); avio_context_free(&d->pb); }
        chunk_free(d->cio);
        memset(d, 0, sizeof(*d));
    }
    P.ndx = 0;
    avcodec_free_context(&P.vdec); avcodec_free_context(&P.adec);
    swr_free(&P.swr);
    if (P.sonic) { sonicDestroyStream(P.sonic); P.sonic = NULL; }
    q_free(&P.vq); q_free(&P.aq); q_free(&P.vh); q_free(&P.ah); P.snap_pending = false;
    for (int i = 0; i < VFRAMES; i++) av_frame_free(&P.frames[i]);
    P.fcount = P.fhead = 0;
    P.rcount = P.rr = P.rw = 0; P.scount = P.shead = 0;
    P.state = PS_IDLE;
    free(P.s_video); free(P.s_audio); free(P.s_muxed); free(P.s_fallback); free(P.s_headers); free(P.s_ua); free(P.s_vcodec); free(P.s_lv); free(P.s_la);
    P.s_video = P.s_audio = P.s_muxed = P.s_fallback = P.s_headers = P.s_ua = P.s_vcodec = P.s_lv = P.s_la = NULL;
    P.opener_started = P.vthread_on = P.athread_on = false;
    pthread_mutex_unlock(&P.lock);
}

bool Player_open(const PlayerSource *src) {
    Player_close();
    pthread_mutex_lock(&P.lock);
    atomic_store(&P.quit, false);
    P.active = true;
    P.src = *src;
    P.s_video = sdup(src->video_url); P.s_audio = sdup(src->audio_url); P.s_muxed = sdup(src->muxed_url); P.s_fallback = sdup(src->fallback_url);
    P.s_headers = sdup(src->headers); P.s_ua = sdup(src->user_agent); P.s_vcodec = sdup(src->video_codec);
    P.s_lv = sdup(src->local_video); P.s_la = sdup(src->local_audio);
    P.src.video_url = P.s_video; P.src.audio_url = P.s_audio; P.src.muxed_url = P.s_muxed; P.src.fallback_url = P.s_fallback; P.src.headers = P.s_headers;
    P.src.user_agent = P.s_ua; P.src.video_codec = P.s_vcodec; P.src.local_video = P.s_lv; P.src.local_audio = P.s_la;
    q_init(&P.vq); q_init(&P.aq); q_init(&P.vh); q_init(&P.ah);
    P.serial++; P.dserial++; P.snap_pending = false;
    P.state = PS_LOADING; P.started = P.paused = P.ended = P.audio_eof_done = false; P.buffering = true;
    P.live = src->live; P.audio_only = src->audio_only;
    P.duration = 0; P.start = src->start; P.error[0] = 0;
    P.decoded = P.dropped = 0; P.av_offset_ms = 0; P.degrade = 0; P.hifps = P.resyncing = P.cap30 = false; P.boost = false; P.stat_at = 0;
    P.width = src->video_height * 16 / 9; P.height = src->video_height; P.fps = src->video_fps; P.kbps = src->video_kbps;
    P.vcodec[0] = P.acodec[0] = 0;
    P.preview = true;
    pthread_mutex_unlock(&P.lock);
    pthread_create(&P.opener, NULL, opener, NULL);
    P.opener_started = true;
    return true;
}

/* ---------- live video-track swap ----------
 * Quality changes and audio-only <-> video keep the audio running: the new video
 * track is opened and positioned in the background, then swapped in. */
typedef struct { char *url; long long size; bool local, audio_only; int height, fps; double kbps; } SwapReq;
static void close_demux(Demux *d) {
    if (d->fmt) avformat_close_input(&d->fmt);
    if (d->pb) { av_freep(&d->pb->buffer); avio_context_free(&d->pb); }
    chunk_free(d->cio);
    memset(d, 0, sizeof(*d));
}
static void stop_video_demux(void) {
    Demux *d = &P.dx[0];
    if (!d->fmt) return;
    d->stop = true;
    pthread_mutex_lock(&P.lock); pthread_cond_broadcast(&P.cond); pthread_mutex_unlock(&P.lock);
    if (d->started) pthread_join(d->thread, NULL);
    d->started = false;
}
static void park_video_locked(void) {
    P.vswitch = true;
    pthread_cond_broadcast(&P.cond);
    if (P.vthread_on) while (!P.vparked && !atomic_load(&P.quit)) pthread_cond_wait(&P.cond, &P.lock);
}
static void *switcher(void *u) {
    SwapReq *q = u;
    Uint32 t0 = SDL_GetTicks();
    if (q->audio_only) {
        stop_video_demux();
        pthread_mutex_lock(&P.lock);
        park_video_locked();
        close_demux(&P.dx[0]);
        q_flush(&P.vq); q_flush(&P.vh);
        for (int i = 0; i < P.fcount; i++) av_frame_unref(P.frames[(P.fhead + i) % VFRAMES]);
        P.fcount = 0;
        avcodec_free_context(&P.vdec);
        P.has_video = false; P.audio_only = true; P.vgen++;
        P.vswitch = false; pthread_cond_broadcast(&P.cond);
        pthread_mutex_unlock(&P.lock);
        Log_msg("video track off (audio only)");
        free(q->url); free(q); P.switch_done = true; return NULL;
    }
    Demux nd; memset(&nd, 0, sizeof(nd));
    pthread_mutex_lock(&P.lock); double pos = now_position_locked(); int serial0 = P.serial; pthread_mutex_unlock(&P.lock);
    int r = open_demux(&nd, q->url, q->size, !q->local, q->local);
    if (r >= 0 && nd.vidx < 0) r = AVERROR_STREAM_NOT_FOUND;
    AVCodecContext *dec = NULL;
    if (r >= 0) {
        for (unsigned k = 0; k < nd.fmt->nb_streams; k++) nd.fmt->streams[k]->discard = (int)k == nd.vidx ? AVDISCARD_DEFAULT : AVDISCARD_ALL;
        nd.aidx = -1;
        /* aim a little ahead: the clock keeps running while the new track starts */
        pthread_mutex_lock(&P.lock); pos = now_position_locked() + 0.4; pthread_mutex_unlock(&P.lock);
        int64_t ts = (int64_t)(pos * AV_TIME_BASE);
        if (avformat_seek_file(nd.fmt, -1, INT64_MIN, ts, ts, 0) < 0) av_seek_frame(nd.fmt, -1, ts, AVSEEK_FLAG_BACKWARD);
        r = open_decoder(nd.fmt->streams[nd.vidx], &dec, true);
    }
    if (r < 0 || atomic_load(&P.quit)) {
        Log_msg("video swap failed: %s", av_err2str(r));
        close_demux(&nd); avcodec_free_context(&dec);
        free(q->url); free(q); P.switch_done = true; return NULL;
    }
    stop_video_demux();
    pthread_mutex_lock(&P.lock);
    park_video_locked();
    close_demux(&P.dx[0]);
    P.dx[0] = nd;
    P.dx[0].fmt->interrupt_callback.opaque = &P.dx[0];
    q_flush(&P.vq); q_flush(&P.vh);
    for (int i = 0; i < P.fcount; i++) av_frame_unref(P.frames[(P.fhead + i) % VFRAMES]);
    P.fcount = 0;
    avcodec_free_context(&P.vdec);
    P.vdec = dec;
    AVStream *st = P.dx[0].fmt->streams[P.dx[0].vidx];
    P.vtb = st->time_base;
    P.fps = av_q2d(st->avg_frame_rate) > 0 ? av_q2d(st->avg_frame_rate) : q->fps;
    P.hifps = P.fps > 40 || (P.fps > 31 && st->codecpar->height >= 1000); P.cap30 = P.hifps && !P.full60; P.resyncing = false;
    snprintf(P.vcodec, sizeof(P.vcodec), "%s", avcodec_get_name(st->codecpar->codec_id));
    P.width = st->codecpar->width; P.height = st->codecpar->height; P.kbps = q->kbps;
    P.has_video = true; P.audio_only = false; P.degrade = 0; P.vgen++;
    if (P.serial != serial0) { P.dx[0].seek_to = P.drop_v; P.dx[0].seek_pending = true; }   /* the user seeked meanwhile */
    else P.drop_v = pos - 0.4;
    P.preview = P.paused;
    P.vswitch = false;
    pthread_cond_broadcast(&P.cond);
    pthread_mutex_unlock(&P.lock);
    pthread_create(&P.dx[0].thread, NULL, demux_thread, &P.dx[0]); P.dx[0].started = true;
    if (!P.vthread_on) { pthread_create(&P.vthread, NULL, video_thread, NULL); P.vthread_on = true; }
    Log_msg("video track swapped to %dp in %.2fs", P.height, (SDL_GetTicks() - t0) / 1000.0);
    free(q->url); free(q);
    P.switch_done = true;
    return NULL;
}
bool Player_switchVideo(const PlayerSource *src) {
    if (P.switcher_on) { pthread_join(P.switcher, NULL); P.switcher_on = false; }   /* one swap at a time */
    PlayerStatus st; Player_status(&st);
    pthread_mutex_lock(&P.lock);
    const char *cur_audio = P.s_audio ? P.s_audio : P.s_la;
    const char *new_audio = src->audio_url ? src->audio_url : src->local_audio;
    const char *new_video = src->video_url ? src->video_url : src->local_video;
    bool fast = P.active && P.started && !P.live && P.state != PS_ERROR && P.dx[1].fmt && P.dx[1].vidx < 0 &&
                !src->muxed_url && cur_audio && new_audio && !strcmp(cur_audio, new_audio) &&
                (src->audio_only || new_video) && !P.switcher_on;
    if (fast && !src->audio_only && P.has_video && P.s_video && src->video_url && !strcmp(P.s_video, src->video_url)) { pthread_mutex_unlock(&P.lock); return true; }
    if (fast) {
        SwapReq *q = calloc(1, sizeof(*q));
        q->audio_only = src->audio_only; q->url = src->audio_only ? NULL : strdup(new_video);
        q->size = src->video_size; q->local = !src->video_url; q->height = src->video_height; q->fps = src->video_fps; q->kbps = src->video_kbps;
        free(P.s_video); P.s_video = src->audio_only ? NULL : sdup(src->video_url); P.src.video_url = P.s_video;
        P.src.audio_only = src->audio_only;
        pthread_mutex_unlock(&P.lock);
        P.switch_done = false;
        P.switcher_on = !pthread_create(&P.switcher, NULL, switcher, q);
        if (!P.switcher_on) { free(q->url); free(q); }
        return P.switcher_on;
    }
    pthread_mutex_unlock(&P.lock);
    /* different audio or a muxed/live source: full reopen at the same spot */
    PlayerSource copy = *src;
    copy.start = st.live ? 0 : st.position;
    bool paused = st.state == PS_PAUSED;
    float speed = st.speed;
    bool ok = Player_open(&copy);
    pthread_mutex_lock(&P.lock); P.paused = paused; P.speed = speed; pthread_mutex_unlock(&P.lock);
    return ok;
}
void Player_setAvOffset(int ms) { pthread_mutex_lock(&P.lock); P.user_offset = ms / 1000.0; pthread_mutex_unlock(&P.lock); }
void Player_setFade(float f) { pthread_mutex_lock(&P.lock); P.fade = f < 0 ? 0 : f > 1 ? 1 : f; pthread_mutex_unlock(&P.lock); }
void Player_setFull60(bool on) { pthread_mutex_lock(&P.lock); P.full60 = on; if (P.hifps) P.cap30 = !on; pthread_mutex_unlock(&P.lock); }
void Player_setGain(float g) { pthread_mutex_lock(&P.lock); P.gain = g < 0 ? 0 : g > 1 ? 1 : g; pthread_mutex_unlock(&P.lock); }
double Player_latencyMs(void) { return P.latency * 1000; }
bool Player_switching(void) { return P.switcher_on && !P.switch_done; }

void Player_setPaused(bool paused) {
    pthread_mutex_lock(&P.lock);
    if (P.active && P.paused != paused) {
        P.clock_pts = now_position_locked() + (P.latency + P.user_offset) * P.speed;
        P.paused = paused;
        P.clock_ticks = SDL_GetTicks();
        if (paused) P.state = P.state == PS_PLAYING || P.state == PS_BUFFERING ? PS_PAUSED : P.state;
        else if (P.state == PS_PAUSED) P.state = P.started ? PS_PLAYING : PS_LOADING;
    }
    pthread_cond_broadcast(&P.cond);
    pthread_mutex_unlock(&P.lock);
}
void Player_togglePause(void) {
    pthread_mutex_lock(&P.lock); bool p = !P.paused; bool ended = P.ended; pthread_mutex_unlock(&P.lock);
    if (ended) { Player_seek(0); Player_setPaused(false); return; }
    Player_setPaused(p);
}

/* Seek inside what is already downloaded (ahead in the queues or behind in the
 * histories): no network, no reconnect. Lands on the keyframe at or before t. */
static QItem *c_at(PQueue *h, PQueue *q, int i) { return i < h->count ? q_at(h, i) : q_at(q, i - h->count); }
static void split_locked(PQueue *h, PQueue *q, int at, int serial) {
    PQueue nh, nq; q_init(&nh); q_init(&nq);
    int total = h->count + q->count;
    for (int i = 0; i < total; i++) {
        QItem *it = c_at(h, q, i);
        PQueue *dst = i < at ? &nh : &nq;
        if (dst->count == dst->cap) { av_packet_free(&it->pkt); continue; }
        QItem *d = &dst->items[(dst->head + dst->count++) % dst->cap];
        *d = *it; d->serial = serial; dst->bytes += it->pkt->size;
        if (it->ts > dst->last_ts || dst->count == 1) dst->last_ts = it->ts;
    }
    nq.eof = q->eof;
    free(h->items); free(q->items);
    *h = nh; *q = nq;
}
static bool buffer_seek_locked(double t, double pos, bool forward) {
    if (!P.started || P.live || !P.vh.items || !P.ah.items) return false;
    double target = t; int kidx = -1;
    if (P.has_video) {
        int total = P.vh.count + P.vq.count;
        if (!total || (!P.vq.eof && P.vq.last_ts < t + 1.0)) return false;
        double k = -1;
        for (int i = 0; i < total; i++) { QItem *it = c_at(&P.vh, &P.vq, i); if ((it->pkt->flags & AV_PKT_FLAG_KEY) && it->ts >= 0 && it->ts <= t + 0.001) { kidx = i; k = it->ts; } }
        if (kidx < 0 || k < t - 8) return false;
        if (!(forward && k <= pos + 0.5)) target = k;
    }
    int atotal = P.ah.count + P.aq.count, aidx = -1;
    if (!atotal || (!P.aq.eof && P.aq.last_ts < target + 1.0)) return false;
    for (int i = 0; i < atotal; i++) { double ts = c_at(&P.ah, &P.aq, i)->ts; if (ts >= 0 && ts <= target + 0.001) aidx = i; }
    if (aidx < 0) { if (c_at(&P.ah, &P.aq, 0)->ts > target + 0.05) return false; aidx = 0; }
    P.serial++;
    if (P.has_video) split_locked(&P.vh, &P.vq, kidx, P.serial);
    split_locked(&P.ah, &P.aq, aidx, P.serial);
    P.drop_v = P.drop_a = target;
    return true;
}
void Player_seek(double t) {
    pthread_mutex_lock(&P.lock);
    if (!P.active || P.live || P.state == PS_ERROR || P.state == PS_LOADING) { pthread_mutex_unlock(&P.lock); return; }
    if (P.duration > 0 && t > P.duration - 1) t = P.duration - 1;
    if (t < 0) t = 0;
    double pos = now_position_locked();
    bool forward = t > pos;
    Uint32 t0 = SDL_GetTicks();
    bool local = buffer_seek_locked(t, pos, forward);
    if (!local) {
        P.serial++; P.dserial++;
        q_flush(&P.vq); q_flush(&P.aq); q_flush(&P.vh); q_flush(&P.ah);
        P.drop_v = P.drop_a = t;
        bool two = P.ndx == 2 && P.has_video;      /* dx[0] video, dx[1] audio */
        P.snap_pending = P.has_video; P.snap_t = t; P.snap_pos = pos; P.snap_forward = forward;
        for (int i = 0; i < 2; i++) if (P.dx[i].fmt) {
            /* audio reaches a little earlier so it can start at the video keyframe */
            P.dx[i].seek_to = two && i == 1 ? fmax(0, t - 6.5) : t;
            P.dx[i].seek_pending = true;
        }
    }
    for (int i = 0; i < P.fcount; i++) av_frame_unref(P.frames[(P.fhead + i) % VFRAMES]);
    P.fcount = 0;
    P.rcount = P.rr = P.rw = 0; P.scount = P.shead = 0;
    P.clock_pts = P.next_pts = P.drop_v; P.clock_ticks = SDL_GetTicks();
    P.ended = false; P.audio_eof_done = false;
    P.buffering = true; P.preview = true; P.started = false;
    pthread_cond_broadcast(&P.cond);
    pthread_mutex_unlock(&P.lock);
    Log_msg("seek %.1f -> %.1f: %s (%u ms)", pos, t, local ? "from buffer" : "network", SDL_GetTicks() - t0);
}

void Player_setSpeed(float s) {
    pthread_mutex_lock(&P.lock);
    if (s < 0.5f) s = 0.5f; if (s > 2.0f) s = 2.0f;
    P.clock_pts = now_position_locked() + (P.latency + P.user_offset) * P.speed; P.clock_ticks = SDL_GetTicks();
    P.speed = s;
    pthread_mutex_unlock(&P.lock);
}

/* readiness / buffering state machine, called from the UI thread */
static void update_locked(void) {
    if (!P.active || P.state == PS_ERROR) return;
    double pos = now_position_locked();
    double vahead = P.has_video ? P.vq.last_ts - pos : 1e9, aahead = P.aq.last_ts - pos;
    bool veof = !P.has_video || P.vq.eof, aeof = P.aq.eof;
    double ahead = fmin(veof ? 1e9 : vahead, aeof ? 1e9 : aahead);
    bool pcm_ready = P.rcount >= OUT_RATE / 5 || (aeof && P.audio_eof_done);
    bool frame_ready = !P.has_video || P.fcount > 0 || veof;
    if (P.ended) { P.state = PS_ENDED; return; }
    if (P.buffering) {
        double need = P.started ? RESUME_SEC : START_SEC;
        if (P.live) need = P.started ? 4.0 : 2.0;
        if ((ahead >= need || (veof && aeof)) && pcm_ready && frame_ready) {
            if (!P.started) Log_msg("started after %.2fs buffered", ahead > 1e8 ? 0 : ahead);
            else Log_msg("resumed, %.1fs buffered", ahead > 1e8 ? 0 : ahead);
            P.buffering = false; P.started = true; P.clock_ticks = SDL_GetTicks();
            if (P.scount) P.clock_pts = P.seg[P.shead].pts;
        }
    }
    if (P.paused) P.state = PS_PAUSED;
    else if (!P.started) P.state = PS_LOADING;
    else P.state = P.buffering ? PS_BUFFERING : PS_PLAYING;
}

void Player_status(PlayerStatus *s) {
    memset(s, 0, sizeof(*s));
    pthread_mutex_lock(&P.lock);
    update_locked();
    s->state = P.active ? P.state : PS_IDLE;
    if (!P.active) { s->speed = P.speed; pthread_mutex_unlock(&P.lock); return; }
    s->position = now_position_locked();
    s->duration = P.duration;
    double vahead = P.has_video ? P.vq.last_ts - s->position : 1e9;
    double aahead = P.aq.last_ts - s->position;
    s->buffered = fmax(0, fmin(P.vq.eof || !P.has_video ? 1e9 : vahead, P.aq.eof ? 1e9 : aahead));
    if (s->buffered > 1e8) s->buffered = s->duration > 0 ? s->duration - s->position : 0;
    s->width = P.width; s->height = P.height; s->fps = P.fps;
    snprintf(s->vcodec, sizeof(s->vcodec), "%s", P.vcodec);
    snprintf(s->acodec, sizeof(s->acodec), "%s", P.acodec);
    s->video_kbps = P.kbps;
    s->decoded = P.decoded; s->dropped = P.dropped; s->av_offset_ms = P.av_offset_ms;
    s->live = P.live; s->audio_only = P.audio_only || !P.has_video; s->speed = P.speed;
    snprintf(s->error, sizeof(s->error), "%s", P.error);
    pthread_mutex_unlock(&P.lock);
    (void)q_first_ts;
}
bool Player_active(void) { pthread_mutex_lock(&P.lock); bool a = P.active; pthread_mutex_unlock(&P.lock); return a; }

void Player_releaseTexture(void) { if (frame_tex) SDL_DestroyTexture(frame_tex); frame_tex = NULL; tex_w = tex_h = 0; }

static bool upload(SDL_Renderer *r, AVFrame *f) {
    if (!frame_tex || tex_w != f->width || tex_h != f->height) {
        if (frame_tex) SDL_DestroyTexture(frame_tex);
        tex_yuv = true;
        frame_tex = SDL_CreateTexture(r, SDL_PIXELFORMAT_IYUV, SDL_TEXTUREACCESS_STREAMING, f->width, f->height);
        if (!frame_tex) { tex_yuv = false; frame_tex = SDL_CreateTexture(r, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, f->width, f->height); Log_msg("YUV texture unavailable, RGB path"); }
        if (!frame_tex) return false;
        SDL_SetTextureScaleMode(frame_tex, SDL_ScaleModeLinear);
        tex_w = f->width; tex_h = f->height;
    }
    bool direct = tex_yuv && (f->format == AV_PIX_FMT_YUV420P || f->format == AV_PIX_FMT_YUVJ420P);
    if (direct) return !SDL_UpdateYUVTexture(frame_tex, NULL, f->data[0], f->linesize[0], f->data[1], f->linesize[1], f->data[2], f->linesize[2]);
    enum AVPixelFormat dst = tex_yuv ? AV_PIX_FMT_YUV420P : AV_PIX_FMT_BGRA;
    P.sws = sws_getCachedContext(P.sws, f->width, f->height, f->format, f->width, f->height, dst, SWS_FAST_BILINEAR, NULL, NULL, NULL);
    if (!P.sws) return false;
    if (P.conv_w != f->width || P.conv_h != f->height) {
        av_freep(&P.conv[0]);
        av_image_alloc(P.conv, P.conv_ls, f->width, f->height, dst, 32);
        P.conv_w = f->width; P.conv_h = f->height;
    }
    sws_scale(P.sws, (const uint8_t *const *)f->data, f->linesize, 0, f->height, P.conv, P.conv_ls);
    if (tex_yuv) return !SDL_UpdateYUVTexture(frame_tex, NULL, P.conv[0], P.conv_ls[0], P.conv[1], P.conv_ls[1], P.conv[2], P.conv_ls[2]);
    return !SDL_UpdateTexture(frame_tex, NULL, P.conv[0], P.conv_ls[0]);
}

/* Is a new picture ready to show? Lets the UI skip redraws between video frames
 * (a 30 fps video needs 30 redraws a second, not 60). */
bool Player_frameDue(void) {
    bool due = false;
    pthread_mutex_lock(&P.lock);
    if (P.active && P.has_video && P.fcount > 0) {
        if (P.preview) due = true;
        else if (P.state == PS_PLAYING) {
            double clock = now_position_locked();
            due = P.fpts[P.fhead] <= clock + 0.004;
            if (due && P.cap30 && SDL_GetTicks() - P.last_show < 28) due = false;
        }
    }
    pthread_mutex_unlock(&P.lock);
    return due;
}
/* the decoder fell behind (or the video is 60 fps / 1080p): worth full CPU speed */
bool Player_wantsBoost(void) {
    pthread_mutex_lock(&P.lock);
    bool b = P.active && P.has_video && P.state != PS_PAUSED && P.state != PS_ENDED && (P.hifps || P.boost || P.degrade > 0 || P.height >= 1000);
    pthread_mutex_unlock(&P.lock);
    return b;
}
bool Player_present(SDL_Renderer *r, SDL_Texture **texture, int *w, int *h) {
    AVFrame *show = NULL;
    pthread_mutex_lock(&P.lock);
    update_locked();
    if (P.active && P.has_video) {
        double clock = now_position_locked();
        bool running = P.state == PS_PLAYING;
        while (P.fcount > 0) {
            int i = P.fhead;
            double pts = P.fpts[i];
            if (running && P.fcount > 1 && pts < clock - 0.06) {      /* late: drop */
                av_frame_unref(P.frames[i]); P.fhead = (P.fhead + 1) % VFRAMES; P.fcount--; P.dropped++; continue;
            }
            if ((running && pts <= clock + 0.004) || P.preview) {
                /* high frame rates: upload at most ~30 pictures a second, always the newest due one */
                if (P.cap30 && running && !P.preview && show == NULL && P.fcount > 1 && P.fpts[(i + 1) % VFRAMES] <= clock + 0.004) {
                    av_frame_unref(P.frames[i]); P.fhead = (P.fhead + 1) % VFRAMES; P.fcount--; P.stat_drop++; continue;
                }
                if (P.cap30 && running && !P.preview && SDL_GetTicks() - P.last_show < 28 && P.fcount > 1) break;
                if (show) av_frame_free(&show);
                show = av_frame_alloc(); av_frame_move_ref(show, P.frames[i]);
                P.av_offset_ms = (int)lround((pts - clock) * 1000);
                P.fhead = (P.fhead + 1) % VFRAMES; P.fcount--;
                if (P.preview) { P.preview = false; break; }
                continue;
            }
            break;
        }
        pthread_cond_broadcast(&P.cond);
    }
    pthread_mutex_unlock(&P.lock);
    if (show) {
        Uint32 u0 = SDL_GetTicks();
        upload(r, show); av_frame_free(&show);
        Uint32 now = SDL_GetTicks();
        P.last_show = now; P.stat_frames++; P.stat_upload_ms += now - u0;
        if (now - P.stat_at >= 10000) {
            if (P.stat_at) Log_msg("video: %.1f pictures/s shown, %u late-dropped, upload %.1f ms, decoded %u dropped %u, A/V %+d ms",
                                   P.stat_frames * 1000.0 / (now - P.stat_at), P.stat_drop, P.stat_frames ? (double)P.stat_upload_ms / P.stat_frames : 0, P.decoded, P.dropped, P.av_offset_ms);
            P.stat_at = now; P.stat_frames = 0; P.stat_drop = 0; P.stat_upload_ms = 0;
        }
    }
    if (texture) *texture = frame_tex;
    if (w) *w = tex_w;
    if (h) *h = tex_h;
    return frame_tex != NULL;
}
