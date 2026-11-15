/* Files you can take away: downloaded videos / music copied out of the app's private folder as
 * "<Title>.mp4", ".m4a", ".opus" or ".mp3" into "NXT Tube" at the top of the SD card, plus the
 * Google Takeout subscriptions import. Remux only (no re-encode) except MP3 (shine, fixed point).
 * SPDX-License-Identifier: GPL-3.0-only */
#include "app.h"
#include "shine/layer3.h"
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswresample/swresample.h>
#include <libavutil/opt.h>
#include <libavutil/channel_layout.h>
#include <sys/stat.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

const char *Export_dir(void) {
    static char d[1200];
    if (d[0]) return d;
    const char *env = getenv("NXT_TUBE_EXPORT_DIR");
    if (env && *env) { snprintf(d, sizeof(d), "%s", env); return d; }
    /* the top of the card that holds the app's data: /mnt/SDCARD/…  -> /mnt/SDCARD/NXT Tube,
     * /roms/…, /userdata/… -> /roms/NXT Tube */
    const char *p = paths.data; int keep = !strncmp(p, "/mnt/", 5) || !strncmp(p, "/media/", 7) ? 2 : 1, seen = 0;
    size_t k = 0;
    for (; p[k]; k++) if (p[k] == '/' && k > 0 && ++seen == keep) break;
    snprintf(d, sizeof(d), "%.*s/NXT Tube", (int)k, p);
    return d;
}
static void ensure_dir(void) { mkdir(Export_dir(), 0755); }

/* a name FAT/exFAT accept: no \/:*?"<>| or control chars, no trailing dots/spaces, ≤ 120 bytes on a UTF-8 boundary */
static void safe_name(const char *title, const char *id, char *out, size_t n) {
    char b[200]; size_t k = 0;
    for (const unsigned char *p = (const unsigned char *)(title && *title ? title : id); *p && k + 1 < sizeof(b); p++) {
        unsigned char c = *p;
        if (c < 32 || strchr("\\/:*?\"<>|", c)) c = c == ':' ? '-' : ' ';
        if (c == ' ' && k && b[k - 1] == ' ') continue;
        b[k++] = c;
    }
    b[k] = 0;
    if (k > 120) { k = 120; while (k && ((unsigned char)b[k] & 0xC0) == 0x80) k--; b[k] = 0; }
    while (k && (b[k - 1] == ' ' || b[k - 1] == '.')) b[--k] = 0;
    char *s = b; while (*s == ' ' || *s == '.') s++;
    snprintf(out, n, "%s", *s ? s : id);
}
static void out_path(const VideoItem *v, const char *ext, char *out, size_t n) {
    char name[200]; safe_name(v->title, v->id, name, sizeof(name));
    snprintf(out, n, "%s/%s.%s", Export_dir(), name, ext);
}
static bool exists(const char *p) { struct stat st; return !stat(p, &st) && st.st_size > 0; }

/* copy the packets of the chosen streams into a new container (no decoding) */
static bool remux(const char *in_v, const char *in_a, const char *out, const char *fmt, const VideoItem *v, char *msg, size_t n) {
    AVFormatContext *ic[2] = {0}, *oc = NULL; int map[2] = {-1, -1}; bool ok = false;
    const char *ins[2] = {in_v, in_a};
    char tmp[1300]; snprintf(tmp, sizeof(tmp), "%s.part", out);
    for (int i = 0; i < 2; i++) {
        if (!ins[i]) continue;
        if (avformat_open_input(&ic[i], ins[i], NULL, NULL) < 0 || avformat_find_stream_info(ic[i], NULL) < 0) { snprintf(msg, n, "%s", T("Không đọc được file đã tải")); goto done; }
    }
    if (avformat_alloc_output_context2(&oc, NULL, fmt, tmp) < 0 || !oc) { snprintf(msg, n, "%s", T("Bản này chưa hỗ trợ định dạng đó")); goto done; }
    for (int i = 0; i < 2; i++) {
        if (!ic[i]) continue;
        /* video from the first file (if wanted), audio from the second (or the first when it is the only one) */
        enum AVMediaType want = (i == 0 && in_a) ? AVMEDIA_TYPE_VIDEO : AVMEDIA_TYPE_AUDIO;
        if (i == 0 && !in_a && in_v && !strcmp(fmt, "mp4")) want = AVMEDIA_TYPE_VIDEO;
        int si = av_find_best_stream(ic[i], want, -1, -1, NULL, 0);
        if (si < 0) continue;
        AVStream *os = avformat_new_stream(oc, NULL);
        if (!os || avcodec_parameters_copy(os->codecpar, ic[i]->streams[si]->codecpar) < 0) goto done;
        os->codecpar->codec_tag = 0; os->time_base = ic[i]->streams[si]->time_base;
        map[i] = si;
    }
    if (map[0] < 0 && map[1] < 0) { snprintf(msg, n, "%s", T("File đã tải không có hình / tiếng")); goto done; }
    av_dict_set(&oc->metadata, "title", v->title, 0);
    av_dict_set(&oc->metadata, "artist", v->channel, 0);
    char cm[80]; snprintf(cm, sizeof(cm), "https://youtu.be/%s", v->id); av_dict_set(&oc->metadata, "comment", cm, 0);
    if (avio_open(&oc->pb, tmp, AVIO_FLAG_WRITE) < 0) { snprintf(msg, n, T("Không ghi được vào %s"), Export_dir()); goto done; }
    AVDictionary *mo = NULL; if (!strcmp(fmt, "mp4") || !strcmp(fmt, "ipod")) av_dict_set(&mo, "movflags", "+faststart", 0);
    if (avformat_write_header(oc, &mo) < 0) { av_dict_free(&mo); snprintf(msg, n, "%s", T("Không tạo được file")); goto done; }
    av_dict_free(&mo);
    AVPacket *pkt = av_packet_alloc();
    int ostream = 0; bool fail = false;
    for (int i = 0; i < 2 && !fail; i++) {
        if (map[i] < 0) continue;
        AVStream *is = ic[i]->streams[map[i]], *os = oc->streams[ostream];
        while (av_read_frame(ic[i], pkt) >= 0) {
            if (pkt->stream_index != map[i]) { av_packet_unref(pkt); continue; }
            av_packet_rescale_ts(pkt, is->time_base, os->time_base);
            pkt->stream_index = ostream; pkt->pos = -1;
            if (av_interleaved_write_frame(oc, pkt) < 0) { fail = true; av_packet_unref(pkt); break; }
        }
        ostream++;
    }
    av_packet_free(&pkt);
    if (av_write_trailer(oc) < 0) fail = true;
    avio_closep(&oc->pb);
    ok = !fail && !rename(tmp, out);
    if (!ok) snprintf(msg, n, "%s", T("Ghi file lỗi (thẻ nhớ đầy?)"));
done:
    if (oc) { if (oc->pb) avio_closep(&oc->pb); avformat_free_context(oc); }
    for (int i = 0; i < 2; i++) if (ic[i]) avformat_close_input(&ic[i]);
    if (!ok) unlink(tmp);
    return ok;
}

/* AAC/Opus -> MP3 128 kbps (shine: fixed point, fine on the A53) with an ID3v2.3 tag */
static void id3_frame(FILE *f, const char *id, const char *text) {
    size_t l = strlen(text) + 1;
    unsigned char h[10] = {id[0], id[1], id[2], id[3], (unsigned char)(l >> 24), (unsigned char)(l >> 16), (unsigned char)(l >> 8), (unsigned char)l, 0, 0};
    fwrite(h, 1, 10, f); fputc(3, f); /* UTF-8 (ID3v2.4 encoding byte; most players accept it in 2.3 too) */ fwrite(text, 1, l - 1, f);
}
static bool to_mp3(const char *in, const char *out, const VideoItem *v, char *msg, size_t n) {
    AVFormatContext *ic = NULL; AVCodecContext *dc = NULL; SwrContext *sw = NULL; shine_t sh = NULL; FILE *f = NULL;
    bool ok = false; char tmp[1300]; snprintf(tmp, sizeof(tmp), "%s.part", out);
    if (avformat_open_input(&ic, in, NULL, NULL) < 0 || avformat_find_stream_info(ic, NULL) < 0) { snprintf(msg, n, "%s", T("Không đọc được file đã tải")); goto done; }
    const AVCodec *c = NULL;
    int si = av_find_best_stream(ic, AVMEDIA_TYPE_AUDIO, -1, -1, &c, 0);
    if (si < 0 || !c) { snprintf(msg, n, "%s", T("File đã tải không có tiếng")); goto done; }
    dc = avcodec_alloc_context3(c);
    avcodec_parameters_to_context(dc, ic->streams[si]->codecpar);
    if (avcodec_open2(dc, c, NULL) < 0) goto done;
    int rate = dc->sample_rate == 48000 ? 48000 : 44100;
    AVChannelLayout stereo = AV_CHANNEL_LAYOUT_STEREO;
    if (swr_alloc_set_opts2(&sw, &stereo, AV_SAMPLE_FMT_S16, rate, &dc->ch_layout, dc->sample_fmt, dc->sample_rate, 0, NULL) < 0 || swr_init(sw) < 0) goto done;
    shine_config_t cfg; memset(&cfg, 0, sizeof(cfg));
    shine_set_config_mpeg_defaults(&cfg.mpeg);
    cfg.wave.channels = PCM_STEREO; cfg.wave.samplerate = rate; cfg.mpeg.mode = JOINT_STEREO; cfg.mpeg.bitr = 128;
    if (shine_check_config(rate, 128) < 0 || !(sh = shine_initialise(&cfg))) goto done;
    int spp = shine_samples_per_pass(sh);
    f = fopen(tmp, "wb");
    if (!f) { snprintf(msg, n, T("Không ghi được vào %s"), Export_dir()); goto done; }
    /* ID3v2.3 header with title / artist / comment-ish URL; size patched after */
    unsigned char hdr[10] = {'I', 'D', '3', 3, 0, 0, 0, 0, 0, 0}; fwrite(hdr, 1, 10, f);
    id3_frame(f, "TIT2", v->title); id3_frame(f, "TPE1", v->channel);
    char url[64]; snprintf(url, sizeof(url), "https://youtu.be/%s", v->id); id3_frame(f, "TIT3", url);
    long tag = ftell(f) - 10;
    fseek(f, 6, SEEK_SET);
    unsigned char sz[4] = {(unsigned char)((tag >> 21) & 127), (unsigned char)((tag >> 14) & 127), (unsigned char)((tag >> 7) & 127), (unsigned char)(tag & 127)};
    fwrite(sz, 1, 4, f); fseek(f, 0, SEEK_END);
    int16_t *pcm = malloc(sizeof(int16_t) * 2 * (spp + 8192)); int have = 0;
    AVPacket *pkt = av_packet_alloc(); AVFrame *fr = av_frame_alloc();
    bool fail = !pcm || !pkt || !fr;
    for (int eof = 0; !fail && eof < 2; ) {
        if (!eof) {
            if (av_read_frame(ic, pkt) < 0) { eof = 1; avcodec_send_packet(dc, NULL); }
            else { if (pkt->stream_index == si) avcodec_send_packet(dc, pkt); av_packet_unref(pkt); }
        }
        for (;;) {
            int r = avcodec_receive_frame(dc, fr);
            if (r == AVERROR_EOF) { eof = 2; break; }
            if (r < 0) { if (eof == 1) eof = 2; break; }
            int room = spp + 8192 - have;
            uint8_t *dst[1] = {(uint8_t *)(pcm + have * 2)};
            int got = swr_convert(sw, dst, room, (const uint8_t **)fr->extended_data, fr->nb_samples);
            if (got > 0) have += got;
            av_frame_unref(fr);
            while (have >= spp) {
                int written = 0; unsigned char *d = shine_encode_buffer_interleaved(sh, pcm, &written);
                if (written && fwrite(d, 1, written, f) != (size_t)written) { fail = true; break; }
                have -= spp; memmove(pcm, pcm + spp * 2, sizeof(int16_t) * 2 * have);
            }
        }
    }
    if (!fail && have > 0) {   /* last partial block: pad with silence */
        memset(pcm + have * 2, 0, sizeof(int16_t) * 2 * (spp - have));
        int written = 0; unsigned char *d = shine_encode_buffer_interleaved(sh, pcm, &written); if (written) fwrite(d, 1, written, f);
    }
    if (!fail) { int written = 0; unsigned char *d = shine_flush(sh, &written); if (written) fwrite(d, 1, written, f); }
    free(pcm); av_packet_free(&pkt); av_frame_free(&fr);
    if (fclose(f)) fail = true;
    f = NULL;
    ok = !fail && !rename(tmp, out);
    if (!ok) snprintf(msg, n, "%s", T("Ghi file lỗi (thẻ nhớ đầy?)"));
done:
    if (f) fclose(f);
    if (sh) shine_close(sh);
    swr_free(&sw); avcodec_free_context(&dc); if (ic) avformat_close_input(&ic);
    if (!ok) unlink(tmp);
    return ok;
}

bool Export_video(const VideoItem *v, char *msg, size_t n) {
    ensure_dir();
    char out[1400]; out_path(v, "mp4", out, sizeof(out));
    if (exists(out)) { snprintf(msg, n, T("Đã có: %s"), out); return true; }
    char vv[1300], aa[1300];
    snprintf(vv, sizeof(vv), "%s", Store_downloadPath(v->id, false)); snprintf(aa, sizeof(aa), "%s", Store_downloadPath(v->id, true));
    if (!exists(vv)) return Export_audio(v, DL_M4A, aa, msg, n);   /* a music download */
    bool ok = remux(vv, exists(aa) ? aa : NULL, out, "mp4", v, msg, n);
    if (ok) snprintf(msg, n, T("Đã lưu %s"), out);
    Log_msg("export %s -> %s: %s", v->id, out, ok ? "ok" : msg);
    return ok;
}
bool Export_audio(const VideoItem *v, int kind, const char *src, char *msg, size_t n) {
    ensure_dir();
    const char *ext = kind == DL_OPUS ? "opus" : kind == DL_MP3 ? "mp3" : "m4a";
    char out[1400]; out_path(v, ext, out, sizeof(out));
    if (exists(out)) { snprintf(msg, n, T("Đã có: %s"), out); return true; }
    bool ok = kind == DL_MP3 ? to_mp3(src, out, v, msg, n) : remux(NULL, src, out, kind == DL_OPUS ? "opus" : "ipod", v, msg, n);
    if (ok) snprintf(msg, n, T("Đã lưu %s"), out);
    Log_msg("export %s (%s) -> %s: %s", v->id, ext, out, ok ? "ok" : msg);
    return ok;
}
int Export_all(char *msg, size_t n) {
    const VideoList *d = Store_downloads();
    int done = 0, fail = 0; char m[400] = "";
    for (int i = 0; i < d->count; i++) { if (Export_video(&d->items[i], m, sizeof(m))) done++; else fail++; }
    if (!d->count) snprintf(msg, n, "%s", T("Chưa có video / nhạc nào đã tải"));
    else if (fail) snprintf(msg, n, T("Đã xuất %d file vào %s · %d lỗi: %s"), done, Export_dir(), fail, m);
    else snprintf(msg, n, T("Đã xuất %d file vào %s"), done, Export_dir());
    return done;
}

/* Google Takeout: YouTube and YouTube Music → subscriptions/subscriptions.csv
 * "Channel Id,Channel Url,Channel Title" (header in the account's language) */
int Import_takeout(char *msg, size_t n) {
    const char *cands[] = {"subscriptions.csv", "Subscriptions.csv", "đăng ký.csv", "subscriptions/subscriptions.csv"};
    char p[1400]; FILE *f = NULL;
    for (int i = 0; i < 4 && !f; i++) { snprintf(p, sizeof(p), "%s/%s", Export_dir(), cands[i]); f = fopen(p, "r"); }
    for (int i = 0; i < 4 && !f; i++) { snprintf(p, sizeof(p), "%s/%s", paths.data, cands[i]); f = fopen(p, "r"); }
    if (!f) { ensure_dir(); snprintf(msg, n, T("Không thấy subscriptions.csv trong %s"), Export_dir()); return -1; }
    char line[1024]; int added = 0, total = 0;
    while (fgets(line, sizeof(line), f)) {
        /* first field: a UC… channel id; the third: the title (may be quoted, may contain commas) */
        char *a = line; if ((unsigned char)a[0] == 0xEF) a += 3;   /* BOM */
        char *c1 = strchr(a, ','); if (!c1) continue;
        *c1 = 0;
        if (strncmp(a, "UC", 2) || strlen(a) != 24) continue;
        char *c2 = strchr(c1 + 1, ','); if (!c2) continue;
        char *t = c2 + 1; size_t l = strlen(t);
        while (l && (t[l - 1] == '\n' || t[l - 1] == '\r')) t[--l] = 0;
        if (*t == '"') { t++; l = strlen(t); if (l && t[l - 1] == '"') t[--l] = 0; }
        char title[128]; size_t k = 0;
        for (const char *q = t; *q && k + 1 < sizeof(title); q++) { if (q[0] == '"' && q[1] == '"') q++; title[k++] = *q; }
        title[k] = 0;
        total++;
        if (Store_isChannel(a)) continue;
        VideoItem v; memset(&v, 0, sizeof(v));
        snprintf(v.channel_id, sizeof(v.channel_id), "%s", a);
        snprintf(v.channel, sizeof(v.channel), "%s", title);
        snprintf(v.title, sizeof(v.title), "%s", title);
        if (Store_toggleChannel(&v)) added++;
    }
    fclose(f);
    Store_save();
    if (!total) snprintf(msg, n, "%s", T("File không có kênh nào (cần subscriptions.csv của Takeout)"));
    else snprintf(msg, n, T("Đã thêm %d kênh mới (%d kênh trong file)"), added, total);
    Log_msg("takeout import %s: %d/%d", p, added, total);
    return added;
}
