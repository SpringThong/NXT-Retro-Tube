/* Subtitles: YouTube's own caption tracks, translated by YouTube (&tlang=) into the
 * viewer's language. "Auto" shows them only when the video is in another language.
 * Tracks come with the stream (direct player API or the yt-dlp helper); the json3
 * file is fetched directly, or through yt-dlp (browser TLS) if YouTube refuses us.
 * Languages written in other scripts need a font pack (Noto, SIL OFL), downloaded
 * once into the data folder and checked with SHA-256.
 * SPDX-License-Identifier: GPL-3.0-only */
#include "app.h"
#include "parson.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <sys/stat.h>
#include <libavformat/avio.h>
#include <libavutil/dict.h>
#include <libavutil/error.h>
#include <mbedtls/sha256.h>

const SubLang SUB_LANGS[] = {
    {"vi", "Tiếng Việt", 0}, {"en", "English", 0}, {"es", "Español", 0}, {"fr", "Français", 0}, {"de", "Deutsch", 0},
    {"pt", "Português", 0}, {"it", "Italiano", 0}, {"id", "Bahasa Indonesia", 0},
    {"ko", "Tiếng Hàn", 1}, {"ja", "Tiếng Nhật", 2}, {"zh-Hans", "Tiếng Trung", 3}, {"th", "Tiếng Thái", 4},
};
const int SUB_NLANGS = sizeof(SUB_LANGS) / sizeof(SUB_LANGS[0]);

/* ---------- font packs (google/fonts, pinned commit) ---------- */
#define FONTS_COMMIT "e7db6ca603fd4862010383ff17ffd6cc4ffac1c0"
typedef struct { const char *name, *file, *path; long size; const char *sha; } Pack;
static const Pack PACKS[] = {
    {NULL, NULL, NULL, 0, NULL},
    {"Hàn", "NotoSansKR.ttf", "ofl/notosanskr/NotoSansKR%5Bwght%5D.ttf", 10414588, "194018e6b2b293a7964f037b25c0249ce1418bc9ab3c971060a03aa57861e252"},
    {"Nhật", "NotoSansJP.ttf", "ofl/notosansjp/NotoSansJP%5Bwght%5D.ttf", 9589900, "c2f3b4d463500a2ddcd3849cded1fceeb9fd6d1c32e6cbecd568453ba50fc68f"},
    {"Trung", "NotoSansSC.ttf", "ofl/notosanssc/NotoSansSC%5Bwght%5D.ttf", 17772300, "a3041811a78c361b1de50f953c805e0244951c21c5bd412f7232ef0d899af0da"},
    {"Thái", "NotoSansThai.ttf", "ofl/notosansthai/NotoSansThai%5Bwdth,wght%5D.ttf", 218652, "5a1c559bb539583c8a1fd99d1c5b9491e5e14478c9cd2bd0970d5c3096cc9ef8"},
};
#define NPACKS 5
static volatile int pack_progress[NPACKS] = {-1, -1, -1, -1, -1};
static bool pack_ok[NPACKS];

static void pack_file(int p, char *out, size_t n) { snprintf(out, n, "%s/fonts/%s", paths.data, PACKS[p].file); }
const char *Subs_packName(int p) { return p > 0 && p < NPACKS ? PACKS[p].name : ""; }
int Subs_packMB(int p) { return p > 0 && p < NPACKS ? (int)((PACKS[p].size + 524288) / 1048576) : 0; }
int Subs_packFor(const char *code) {
    for (int i = 0; i < SUB_NLANGS; i++) if (!strcasecmp(SUB_LANGS[i].code, code)) return SUB_LANGS[i].pack;
    /* an original track in another script */
    if (!strncasecmp(code, "ko", 2)) return 1;
    if (!strncasecmp(code, "ja", 2)) return 2;
    if (!strncasecmp(code, "zh", 2)) return 3;
    if (!strncasecmp(code, "th", 2)) return 4;
    return 0;
}
bool Subs_needsPack(const char *code) { return Subs_packFor(code) > 0; }
bool Subs_packReady(int p) {
    if (p <= 0) return true;
    if (p >= NPACKS) return false;
    if (pack_ok[p]) return true;
    char f[1300]; pack_file(p, f, sizeof(f));
    struct stat st;
    pack_ok[p] = !stat(f, &st) && st.st_size == PACKS[p].size;   /* verified with SHA-256 when it was saved */
    return pack_ok[p];
}
int Subs_packProgress(int p) { return p > 0 && p < NPACKS ? pack_progress[p] : -1; }
static void *pack_thread(void *arg) {
    int p = (int)(intptr_t)arg;
    char dir[1200], f[1300], tmp[1320], url[512], ca[1200];
    snprintf(dir, sizeof(dir), "%s/fonts", paths.data); mkdir(dir, 0755);
    pack_file(p, f, sizeof(f)); snprintf(tmp, sizeof(tmp), "%s.part", f);
    const char *base = getenv("NXT_TUBE_FONTS_BASE");   /* QA: a local mirror */
    snprintf(url, sizeof(url), "%s/%s", base ? base : "https://raw.githubusercontent.com/google/fonts/" FONTS_COMMIT, PACKS[p].path);
    snprintf(ca, sizeof(ca), "%s/res/cacert.pem", paths.app);
    bool ok = false;
    for (int attempt = 0; attempt < 2 && !ok; attempt++) {
        AVDictionary *o = NULL;
        av_dict_set(&o, "ca_file", ca, 0);
        av_dict_set(&o, "tls_verify", attempt ? "0" : "1", 0);      /* the SHA-256 check guards the file either way */
        av_dict_set(&o, "timeout", "15000000", 0); av_dict_set(&o, "rw_timeout", "15000000", 0);
        av_dict_set(&o, "user_agent", "NXT-Retro-Tube", 0);
        AVIOContext *io = NULL;
        int r = avio_open2(&io, url, AVIO_FLAG_READ, NULL, &o);
        av_dict_free(&o);
        if (r < 0) { Log_msg("font pack %s: %s", PACKS[p].file, av_err2str(r)); continue; }
        FILE *out = fopen(tmp, "wb");
        if (!out) { avio_closep(&io); break; }
        mbedtls_sha256_context sh; mbedtls_sha256_init(&sh); mbedtls_sha256_starts(&sh, 0);
        static unsigned char buf[65536]; long got = 0;
        for (;;) {
            int n = avio_read(io, buf, sizeof(buf));
            if (n <= 0) break;
            fwrite(buf, 1, n, out); mbedtls_sha256_update(&sh, buf, n); got += n;
            pack_progress[p] = (int)(got * 100 / PACKS[p].size); if (pack_progress[p] > 99) pack_progress[p] = 99;
        }
        avio_closep(&io); fclose(out);
        unsigned char d[32]; char hex[65]; mbedtls_sha256_finish(&sh, d); mbedtls_sha256_free(&sh);
        for (int i = 0; i < 32; i++) sprintf(hex + i * 2, "%02x", d[i]);
        ok = got == PACKS[p].size && !strcmp(hex, PACKS[p].sha);
        if (ok) rename(tmp, f); else { unlink(tmp); Log_msg("font pack %s: %ld bytes, checksum %s", PACKS[p].file, got, ok ? "ok" : "mismatch"); }
    }
    pack_ok[p] = false;
    pack_progress[p] = ok ? -1 : 101;
    Log_msg("font pack %s %s", PACKS[p].file, ok ? "installed" : "failed");
    return NULL;
}
bool Subs_packDownload(int p) {
    if (p <= 0 || p >= NPACKS || (pack_progress[p] >= 0 && pack_progress[p] <= 100)) return false;
    pack_progress[p] = 0;
    pthread_t t;
    if (pthread_create(&t, NULL, pack_thread, (void *)(intptr_t)p)) { pack_progress[p] = 101; return false; }
    pthread_detach(t);
    return true;
}
const char *Subs_fontFor(const char *code, long *index) {
    static char f[1300];
    int p = Subs_packFor(code);
    if (!p || !Subs_packReady(p)) return NULL;
    pack_file(p, f, sizeof(f));
    if (index) *index = 5L << 16;            /* named instance 5 = Medium (the variable fonts default to Thin) */
    return f;
}

/* ---------- cues ---------- */
typedef struct { int start, end; char *text; } Cue;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static Cue *cues; static int ncues;
static int gen;                 /* bumped on every open/close: stale fetches are dropped */
static char status[160], shown_lang[24];

static void free_cues(Cue *c, int n) { for (int i = 0; i < n; i++) free(c[i].text); free(c); }
static int parse_json3(const char *text, Cue **out) {
    JSON_Value *root = json_parse_string(text);
    JSON_Array *ev = json_object_get_array(json_value_get_object(root), "events");
    int n = 0, cap = 0; Cue *c = NULL;
    for (size_t i = 0; ev && i < json_array_get_count(ev); i++) {
        JSON_Object *e = json_array_get_object(ev, i);
        JSON_Array *segs = json_object_get_array(e, "segs");
        if (!segs) continue;
        char line[1024]; size_t k = 0;
        for (size_t s = 0; s < json_array_get_count(segs) && k + 1 < sizeof(line); s++) {
            const char *u = json_object_get_string(json_array_get_object(segs, s), "utf8");
            for (; u && *u && k + 1 < sizeof(line); u++) line[k++] = (*u == '\n' || *u == '\r') ? ' ' : *u;
        }
        line[k] = 0;
        while (k && line[k - 1] == ' ') line[--k] = 0;
        char *t = line; while (*t == ' ') t++;
        if (!*t) continue;
        int st = (int)json_object_get_number(e, "tStartMs"), du = (int)json_object_get_number(e, "dDurationMs");
        if (du <= 0) du = 2500;
        if (n == cap) { cap = cap ? cap * 2 : 256; Cue *nc = realloc(c, cap * sizeof(Cue)); if (!nc) break; c = nc; }
        c[n++] = (Cue){st, st + du, strdup(t)};
    }
    json_value_free(root);
    *out = c;
    return n;
}

typedef struct { int gen; char *url; char lang[24]; char label[128]; } FetchReq;
static void *fetch_thread(void *arg) {
    FetchReq *r = arg;
    char err[160] = "";
    char *text = Innertube_get(r->url, err, sizeof(err));
    Cue *c = NULL; int n = text ? parse_json3(text, &c) : 0;
    free(text);
    if (!n) {
        /* YouTube may only hand captions to a browser-like TLS client: ask yt-dlp */
        free_cues(c, 0); c = NULL;
        Log_msg("subs: direct fetch gave nothing (%s), trying yt-dlp", err[0] ? err : "empty");
        int job = Yt_fetch(r->url);
        for (int i = 0; job && i < 600; i++) {
            JobState s = Yt_state(job);
            if (s == JOB_DONE) { char *t = Yt_takeText(job); if (t) { n = parse_json3(t, &c); free(t); } job = 0; break; }
            if (s == JOB_FAILED) { Yt_forget(job); job = 0; break; }
            pthread_mutex_lock(&lock); bool stale = r->gen != gen; pthread_mutex_unlock(&lock);
            if (stale) { Yt_forget(job); job = 0; break; }
            usleep(100000);
        }
        if (job) Yt_forget(job);
    }
    pthread_mutex_lock(&lock);
    if (r->gen == gen) {
        free_cues(cues, ncues); cues = c; ncues = n; c = NULL; n = 0;
        if (ncues) { snprintf(status, sizeof(status), "%s", r->label); snprintf(shown_lang, sizeof(shown_lang), "%s", r->lang); }
        else snprintf(status, sizeof(status), "%s", T("Không tải được phụ đề"));
        Log_msg("subs: %d lines (%s)", ncues, r->label);
    }
    pthread_mutex_unlock(&lock);
    free_cues(c, n);
    free(r->url); free(r);
    return NULL;
}

static bool same_lang(const char *a, const char *b) {
    size_t la = strcspn(a, "-_"), lb = strcspn(b, "-_");
    return la == lb && !strncasecmp(a, b, la);
}
static const char *lang_name(const char *code) {
    for (int i = 0; i < SUB_NLANGS; i++) if (same_lang(SUB_LANGS[i].code, code)) return T(SUB_LANGS[i].name);
    return code;
}
const char *Subs_target(void) {
    static char t[24];
    pthread_mutex_lock(&lock); snprintf(t, sizeof(t), "%s", shown_lang); pthread_mutex_unlock(&lock);
    return t;
}
static const char *viewer_lang(void) { return S.sub_lang[0] ? S.sub_lang : S.lang == 1 ? "en" : "vi"; }

/* the track URL without fmt= and tlang= (we set both) */
static void url_without(const char *in, char *out, size_t n) {
    size_t k = 0;
    const char *q = strchr(in, '?');
    if (!q) { snprintf(out, n, "%s", in); return; }
    k = (size_t)(q - in) + 1; if (k >= n) k = n - 1;
    memcpy(out, in, k); out[k] = 0;
    bool first = true;
    for (const char *p = q + 1; *p;) {
        size_t len = strcspn(p, "&");
        if (len && strncmp(p, "fmt=", 4) && strncmp(p, "tlang=", 6)) {
            size_t l = strlen(out);
            snprintf(out + l, n - l, "%s%.*s", first ? "" : "&", (int)len, p); first = false;
        }
        p += len; if (*p) p++;
    }
    size_t l = strlen(out);
    if (l && (out[l - 1] == '?' || out[l - 1] == '&')) out[--l] = 0;
}
void Subs_close(void) {
    pthread_mutex_lock(&lock);
    gen++; free_cues(cues, ncues); cues = NULL; ncues = 0; status[0] = 0; shown_lang[0] = 0;
    pthread_mutex_unlock(&lock);
}
void Subs_open(const StreamInfo *s, int override) {
    Subs_close();
    int mode = override == 3 ? 2 : override ? 1 : S.sub_mode;         /* 0 auto, 1 on, 2 off */
    if (mode == 2) { snprintf(status, sizeof(status), "%s", T("Phụ đề: tắt")); return; }
    if (!s || !s->ncaps) { snprintf(status, sizeof(status), "%s", T("Video không có phụ đề")); return; }
    const char *target = viewer_lang();
    /* the video's language: its speech-recognition track, else the first manual one */
    const CaptionTrack *asr = NULL, *manual_target = NULL, *first = NULL;
    for (int i = 0; i < s->ncaps; i++) {
        const CaptionTrack *t = &s->caps[i];
        if (!first) first = t;
        if (t->asr && !asr) asr = t;
        if (!t->asr && same_lang(t->lang, target) && !manual_target) manual_target = t;
    }
    const char *vlang = asr ? asr->lang : first->lang;
    const CaptionTrack *src = NULL; bool translate = false; const char *show = target;
    if (override == 2) { src = asr ? asr : first; show = src->lang; }
    else if (manual_target) { src = manual_target; if (mode == 0 && same_lang(vlang, target)) src = NULL; }
    else if (same_lang(vlang, target)) { if (mode == 1) src = asr ? asr : first; }
    else {
        for (int i = 0; i < s->ncaps && !src; i++) if (!s->caps[i].asr && same_lang(s->caps[i].lang, vlang) && s->caps[i].translatable) src = &s->caps[i];
        if (!src && asr && asr->translatable) src = asr;
        for (int i = 0; i < s->ncaps && !src; i++) if (s->caps[i].translatable) src = &s->caps[i];
        translate = src != NULL;
    }
    if (!src) { snprintf(status, sizeof(status), "%s", same_lang(vlang, target) ? T("Phụ đề: không cần (cùng ngôn ngữ)") : T("Video không có phụ đề dịch được")); return; }
    int pack = Subs_packFor(show);
    if (pack && !Subs_packReady(pack)) { snprintf(status, sizeof(status), T("Cần tải gói chữ %s trong Cài đặt"), Subs_packName(pack)); return; }
    FetchReq *r = calloc(1, sizeof(*r));
    size_t ul = strlen(src->url) + 64;
    r->url = malloc(ul);
    url_without(src->url, r->url, ul);
    size_t l = strlen(r->url);
    snprintf(r->url + l, ul - l, "%sfmt=json3%s%s", strchr(r->url, '?') ? "&" : "?", translate ? "&tlang=" : "", translate ? show : "");
    snprintf(r->lang, sizeof(r->lang), "%s", show);
    if (translate) snprintf(r->label, sizeof(r->label), T("Phụ đề: %s (dịch từ %s)"), lang_name(show), lang_name(vlang));
    else snprintf(r->label, sizeof(r->label), T("Phụ đề: %s%s"), lang_name(show), src->asr ? T(" (tự động)") : "");
    pthread_mutex_lock(&lock); r->gen = gen; snprintf(status, sizeof(status), "%s", T("Đang tải phụ đề…")); pthread_mutex_unlock(&lock);
    pthread_t t;
    if (pthread_create(&t, NULL, fetch_thread, r)) { free(r->url); free(r); return; }
    pthread_detach(t);
}
const char *Subs_text(double pos) {
    static char out[1024];
    int ms = (int)(pos * 1000);
    out[0] = 0;
    pthread_mutex_lock(&lock);
    /* the last two lines on screen at this moment (speech-recognition tracks roll) */
    int found[2] = {-1, -1}, nf = 0;
    for (int i = 0; i < ncues; i++) {
        if (cues[i].start > ms) break;
        if (ms < cues[i].end) { if (nf == 2) { found[0] = found[1]; found[1] = i; } else found[nf++] = i; }
    }
    for (int k = 0; k < nf; k++) {
        size_t l = strlen(out);
        snprintf(out + l, sizeof(out) - l, "%s%s", l ? " " : "", cues[found[k]].text);
    }
    pthread_mutex_unlock(&lock);
    return out[0] ? out : NULL;
}
const char *Subs_status(void) {
    static char s[160];
    pthread_mutex_lock(&lock); snprintf(s, sizeof(s), "%s", status); pthread_mutex_unlock(&lock);
    return s;
}
void Subs_init(void) {}
void Subs_quit(void) { Subs_close(); }
