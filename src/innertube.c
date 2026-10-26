/* Direct InnerTube player request (visionOS client: no JS player, no PO token).
 * One HTTPS POST instead of a yt-dlp round trip; the result is reshaped into
 * the same JSON yt-dlp produces, so parse_stream_obj() handles both.
 * Anything unusual (not OK, SABR-only, no URLs) returns NULL and the caller
 * falls back to yt-dlp.
 * SPDX-License-Identifier: GPL-3.0-only */
#include "app.h"
#include "parson.h"
#include <libavformat/avformat.h>
#include <libavutil/opt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <pthread.h>

#define VISIONOS_UA "Mozilla/5.0 (Macintosh; Intel Mac OS X 15_7_3) AppleWebKit/605.1.15 (KHTML, like Gecko) Version/26.0 Safari/605.1.15"

#define CHROME_UA "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/138.0.0.0 Safari/537.36"
static const char *base_url(void) { const char *b = getenv("NXT_TUBE_INNERTUBE_BASE"); return b ? b : "https://www.youtube.com"; }

static char *http_req(const char *url, const char *body, const char *headers, const char *ua, bool verify, char *err, size_t en) {
    AVDictionary *o = NULL;
    char *hex = NULL;
    if (body) {
        size_t bl = strlen(body);
        hex = malloc(bl * 2 + 1);
        if (!hex) return NULL;
        static const char *digits = "0123456789abcdef";
        for (size_t i = 0; i < bl; i++) { hex[i * 2] = digits[(unsigned char)body[i] >> 4]; hex[i * 2 + 1] = digits[(unsigned char)body[i] & 15]; }
        hex[bl * 2] = 0;
        av_dict_set(&o, "method", "POST", 0);
        av_dict_set(&o, "post_data", hex, 0);
    }
    if (headers) av_dict_set(&o, "headers", headers, 0);
    av_dict_set(&o, "user_agent", ua, 0);
    av_dict_set(&o, "ca_file", paths.cacert, 0);
    av_dict_set(&o, "tls_verify", verify ? "1" : "0", 0);
    av_dict_set(&o, "timeout", "10000000", 0);
    av_dict_set(&o, "rw_timeout", "10000000", 0);
    av_dict_set(&o, "icy", "0", 0);
    AVIOContext *io = NULL;
    int r = avio_open2(&io, url, AVIO_FLAG_READ, NULL, &o);
    av_dict_free(&o); free(hex);
    if (r < 0) { snprintf(err, en, "%s", av_err2str(r)); return NULL; }
    size_t cap = 256 << 10, used = 0; char *buf = malloc(cap);
    for (;;) {
        if (used + 65536 + 1 > cap) { if (cap >= (8u << 20)) break; cap *= 2; char *n = realloc(buf, cap); if (!n) break; buf = n; }
        int n = avio_read(io, (unsigned char *)buf + used, (int)(cap - used - 1));
        if (n <= 0) break;
        used += n;
    }
    avio_closep(&io);
    buf[used] = 0;
    if (!used) { free(buf); snprintf(err, en, "empty reply"); return NULL; }
    return buf;
}
static char *http_any(const char *url, const char *body, const char *headers, const char *ua, char *err, size_t en) {
    char *r = http_req(url, body, headers, ua, true, err, en);
    return r ? r : http_req(url, body, headers, ua, false, err, en);   /* wrong clock / CA */
}

/* plain GET (subtitles): a browser user agent, the full CA bundle not needed for youtube.com */
char *Innertube_get(const char *url, char *err, size_t en) {
    return http_any(url, NULL, "Origin: https://www.youtube.com\r\nReferer: https://www.youtube.com/\r\n", CHROME_UA, err, en);
}

/* ---------- visitor identity ----------
 * YouTube answers "Sign in to confirm you're not a bot" to API calls without a
 * visitor id. One page load gives it (and the web client version); it is kept
 * on the SD card and reused for days. */
static pthread_mutex_t vlock = PTHREAD_MUTEX_INITIALIZER;
static char visitor[512], web_version[48];
static long long visitor_time;
static void visitor_file(char *p, size_t n) { snprintf(p, n, "%s/visitor.txt", paths.cache); }
static void visitor_load(void) {
    if (visitor[0]) return;
    char p[1200]; visitor_file(p, sizeof(p));
    FILE *f = fopen(p, "r"); if (!f) return;
    char line[700];
    if (fgets(line, sizeof(line), f)) {
        long long t = 0; char v[512] = "", w[48] = "";
        if (sscanf(line, "%lld %511s %47s", &t, v, w) >= 2 && time(NULL) - t < 5 * 86400) {
            snprintf(visitor, sizeof(visitor), "%s", v); snprintf(web_version, sizeof(web_version), "%s", w); visitor_time = t;
        }
    }
    fclose(f);
}
static void visitor_store(void) {
    char p[1200], tmp[1300]; visitor_file(p, sizeof(p)); snprintf(tmp, sizeof(tmp), "%s.tmp", p);
    FILE *f = fopen(tmp, "w"); if (!f) return;
    fprintf(f, "%lld %s %s\n", (long long)time(NULL), visitor, web_version[0] ? web_version : "-");
    if (!fclose(f)) rename(tmp, p);
}
static bool grab(const char *page, const char *key, char *out, size_t n) {
    const char *k = strstr(page, key); if (!k) return false;
    k += strlen(key);
    size_t i = 0; while (k[i] && k[i] != '"' && i + 1 < n) { out[i] = k[i]; i++; }
    out[i] = 0; return i > 0;
}
/* Returns true when a visitor id is available (fetches the page once if needed). */
bool Innertube_warmup(bool force) {
    pthread_mutex_lock(&vlock);
    visitor_load();
    if (visitor[0] && !force) { pthread_mutex_unlock(&vlock); return true; }
    char err[160] = "", url[256]; snprintf(url, sizeof(url), "%s/?hl=en&gl=VN", base_url());
    Uint32 t0 = SDL_GetTicks();
    char *page = http_any(url, NULL, "Accept-Language: en-US,en;q=0.8\r\nCookie: CONSENT=YES+1; SOCS=CAI\r\n", CHROME_UA, err, sizeof(err));
    bool ok = false;
    if (page) {
        char v[512], w[48];
        if (grab(page, "\"VISITOR_DATA\":\"", v, sizeof(v)) || grab(page, "\"visitorData\":\"", v, sizeof(v))) {
            snprintf(visitor, sizeof(visitor), "%s", v); ok = true;
            if (grab(page, "\"INNERTUBE_CLIENT_VERSION\":\"", w, sizeof(w))) snprintf(web_version, sizeof(web_version), "%s", w);
            visitor_time = time(NULL); visitor_store();
        }
        free(page);
    }
    Log_msg("visitor id %s in %.2fs%s%s", ok ? "ready" : "unavailable", (SDL_GetTicks() - t0) / 1000.0, ok ? "" : ": ", ok ? "" : err);
    pthread_mutex_unlock(&vlock);
    return ok;
}
static void visitor_copy(char *v, size_t vn, char *w, size_t wn) {
    pthread_mutex_lock(&vlock); visitor_load();
    snprintf(v, vn, "%s", visitor); snprintf(w, wn, "%s", web_version[0] && strcmp(web_version, "-") ? web_version : "2.20260708.00.00");
    pthread_mutex_unlock(&vlock);
}
static void visitor_set(const char *v) {
    if (!v || !*v) return;
    pthread_mutex_lock(&vlock);
    if (strcmp(visitor, v)) { snprintf(visitor, sizeof(visitor), "%s", v); visitor_store(); }
    pthread_mutex_unlock(&vlock);
}
const char *Innertube_visitor(void) { static char v[512]; char w[48]; visitor_copy(v, sizeof(v), w, sizeof(w)); return v; }

/* 'video/mp4; codecs="avc1.4d401f"' -> kind "video", codec "avc1.4d401f" */
static void mime_parts(const char *mime, char *kind, size_t kn, char *codec, size_t cn) {
    *kind = *codec = 0;
    if (!mime) return;
    const char *slash = strchr(mime, '/');
    size_t k = slash ? (size_t)(slash - mime) : 0; if (k >= kn) k = kn - 1;
    memcpy(kind, mime, k); kind[k] = 0;
    const char *c = strstr(mime, "codecs=\"");
    if (!c) return;
    c += 8;
    size_t i = 0; while (c[i] && c[i] != '"' && c[i] != ',' && i + 1 < cn) { codec[i] = c[i]; i++; }
    codec[i] = 0;
}

static void add_format(JSON_Array *out, JSON_Object *f, bool muxed_list) {
    const char *url = json_object_get_string(f, "url");
    if (!url) return;                                   /* signatureCipher / SABR only */
    if (strstr(url, "&n=") || strstr(url, "?n=")) return;   /* needs the JS player: let yt-dlp handle it */
    char kind[16], codec[48];
    mime_parts(json_object_get_string(f, "mimeType"), kind, sizeof(kind), codec, sizeof(codec));
    JSON_Value *v = json_value_init_object(); JSON_Object *o = json_value_get_object(v);
    json_object_set_string(o, "url", url);
    json_object_set_string(o, "protocol", "https");
    char id[16]; snprintf(id, sizeof(id), "%d", (int)json_object_get_number(f, "itag"));
    json_object_set_string(o, "format_id", id);
    bool video = !strcmp(kind, "video"), audio = !strcmp(kind, "audio");
    if (muxed_list) {      /* progressive: "avc1.42001E, mp4a.40.2" */
        json_object_set_string(o, "vcodec", codec);
        json_object_set_string(o, "acodec", "mp4a.40.2");
    } else {
        json_object_set_string(o, "vcodec", video ? codec : "none");
        json_object_set_string(o, "acodec", audio ? codec : "none");
    }
    if (json_object_has_value(f, "height")) json_object_set_number(o, "height", json_object_get_number(f, "height"));
    if (json_object_has_value(f, "width")) json_object_set_number(o, "width", json_object_get_number(f, "width"));
    if (json_object_has_value(f, "fps")) json_object_set_number(o, "fps", json_object_get_number(f, "fps"));
    double br = json_object_get_number(f, "bitrate");
    if (br > 0) { json_object_set_number(o, "tbr", br / 1000.0); if (audio) json_object_set_number(o, "abr", br / 1000.0); }
    const char *len = json_object_get_string(f, "contentLength");
    if (len) json_object_set_number(o, "filesize", atof(len));
    JSON_Object *track = json_object_get_object(f, "audioTrack");
    if (track) {
        if (json_object_get_boolean(track, "audioIsDefault") == 1) json_object_set_string(o, "format_note", "original (default)");
        const char *tid = json_object_get_string(track, "id");
        if (tid) { char lang[16]; size_t i = 0; while (tid[i] && tid[i] != '.' && i + 1 < sizeof(lang)) { lang[i] = tid[i]; i++; } lang[i] = 0; json_object_set_string(o, "language", lang); }
    }
    JSON_Value *hv = json_value_init_object();
    json_object_set_string(json_value_get_object(hv), "User-Agent", VISIONOS_UA);
    json_object_set_value(o, "http_headers", hv);
    json_array_append_value(out, v);
}

/* Returns a yt-dlp-shaped info JSON (malloc'd) or NULL with err set. */
static JSON_Value *player_call(const char *video_id, const char *vis, char *err, size_t en) {
    char body[2048], headers[900], url[256];
    snprintf(body, sizeof(body),
        "{\"context\":{\"client\":{\"clientName\":\"VISIONOS\",\"clientVersion\":\"1.02\",\"deviceMake\":\"Apple\","
        "\"deviceModel\":\"RealityDevice17,1\",\"userAgent\":\"" VISIONOS_UA "\",\"osName\":\"visionOS\","
        "\"osVersion\":\"26.5.23O471\",\"hl\":\"en\",\"timeZone\":\"UTC\",\"utcOffsetMinutes\":0%s%s%s}},"
        "\"videoId\":\"%s\",\"playbackContext\":{\"contentPlaybackContext\":{\"html5Preference\":\"HTML5_PREF_WANTS\"}},"
        "\"contentCheckOk\":true,\"racyCheckOk\":true}", *vis ? ",\"visitorData\":\"" : "", vis, *vis ? "\"" : "", video_id);
    snprintf(headers, sizeof(headers), "Content-Type: application/json\r\nX-YouTube-Client-Name: 101\r\nX-YouTube-Client-Version: 1.02\r\n"
             "Origin: https://www.youtube.com\r\n%s%s%s", *vis ? "X-Goog-Visitor-Id: " : "", vis, *vis ? "\r\n" : "");
    const char *over = getenv("NXT_TUBE_INNERTUBE_URL");
    snprintf(url, sizeof(url), "%s", over ? over : "https://www.youtube.com/youtubei/v1/player?prettyPrint=false");
    char *reply = http_any(url, body, headers, VISIONOS_UA, err, en);
    if (!reply) return NULL;
    JSON_Value *root = json_parse_string(reply);
    free(reply);
    if (!json_value_get_object(root)) { json_value_free(root); snprintf(err, en, "bad JSON"); return NULL; }
    return root;
}
/* Returns a yt-dlp-shaped info JSON (malloc'd) or NULL with err set. */
char *Innertube_player(const char *video_id, char *err, size_t en) {
    char vis[512], wv[48]; visitor_copy(vis, sizeof(vis), wv, sizeof(wv));
    JSON_Value *root = player_call(video_id, vis, err, en);
    if (!root) return NULL;
    JSON_Object *r = json_value_get_object(root);
    const char *status = json_object_dotget_string(r, "playabilityStatus.status");
    if (status && !strcmp(status, "LOGIN_REQUIRED")) {
        /* bot check: get (or refresh) the visitor id and ask once more */
        const char *given = json_object_dotget_string(r, "responseContext.visitorData");
        bool retry = false;
        if (!vis[0] && given && *given) { visitor_set(given); retry = true; }
        else if (Innertube_warmup(true)) retry = true;
        if (retry) {
            visitor_copy(vis, sizeof(vis), wv, sizeof(wv));
            json_value_free(root);
            root = player_call(video_id, vis, err, en);
            if (!root) return NULL;
            r = json_value_get_object(root);
            status = json_object_dotget_string(r, "playabilityStatus.status");
        }
    }
    if (!status || strcmp(status, "OK")) {
        const char *why = json_object_dotget_string(r, "playabilityStatus.reason");
        snprintf(err, en, "%s: %s", status ? status : "no status", why ? why : "");
        json_value_free(root); return NULL;
    }
    JSON_Object *vd = json_object_get_object(r, "videoDetails");
    JSON_Object *sd = json_object_get_object(r, "streamingData");
    if (!vd || !sd) { json_value_free(root); snprintf(err, en, "no streamingData"); return NULL; }
    JSON_Value *ov = json_value_init_object(); JSON_Object *o = json_value_get_object(ov);
    json_object_set_string(o, "id", json_object_get_string(vd, "videoId") ? json_object_get_string(vd, "videoId") : video_id);
    if (json_object_get_string(vd, "title")) json_object_set_string(o, "title", json_object_get_string(vd, "title"));
    if (json_object_get_string(vd, "author")) json_object_set_string(o, "channel", json_object_get_string(vd, "author"));
    if (json_object_get_string(vd, "channelId")) json_object_set_string(o, "channel_id", json_object_get_string(vd, "channelId"));
    if (json_object_get_string(vd, "lengthSeconds")) json_object_set_number(o, "duration", atof(json_object_get_string(vd, "lengthSeconds")));
    if (json_object_get_string(vd, "viewCount")) json_object_set_number(o, "view_count", atof(json_object_get_string(vd, "viewCount")));
    if (json_object_get_string(vd, "shortDescription")) {
        char d[600]; snprintf(d, sizeof(d), "%s", json_object_get_string(vd, "shortDescription"));
        json_object_set_string(o, "description", d);
    }
    bool live = json_object_get_boolean(vd, "isLive") == 1;
    if (live) { json_object_set_boolean(o, "is_live", true); json_object_set_string(o, "live_status", "is_live"); }
    JSON_Value *fv = json_value_init_array(); JSON_Array *fa = json_value_get_array(fv);
    JSON_Array *adaptive = json_object_get_array(sd, "adaptiveFormats"), *muxed = json_object_get_array(sd, "formats");
    for (size_t i = 0; adaptive && i < json_array_get_count(adaptive); i++) add_format(fa, json_array_get_object(adaptive, i), false);
    for (size_t i = 0; muxed && i < json_array_get_count(muxed); i++) add_format(fa, json_array_get_object(muxed, i), true);
    const char *hls = json_object_get_string(sd, "hlsManifestUrl");
    if (hls) {
        JSON_Value *hv = json_value_init_object(); JSON_Object *h = json_value_get_object(hv);
        json_object_set_string(h, "url", hls); json_object_set_string(h, "protocol", "m3u8_native");
        json_object_set_string(h, "vcodec", "avc1"); json_object_set_string(h, "acodec", "mp4a.40.2");
        json_object_set_number(h, "height", 720); json_object_set_string(h, "format_id", "hls");
        json_array_append_value(fa, hv);
    }
    size_t usable = json_array_get_count(fa);
    json_object_set_value(o, "formats", fv);
    /* subtitle tracks in the video's own language(s); the app asks YouTube to translate them */
    JSON_Array *ct = json_object_dotget_array(r, "captions.playerCaptionsTracklistRenderer.captionTracks");
    if (ct && json_array_get_count(ct)) {
        JSON_Value *cv = json_value_init_array(); JSON_Array *ca = json_value_get_array(cv);
        for (size_t i = 0; i < json_array_get_count(ct) && i < 16; i++) {
            JSON_Object *t = json_array_get_object(ct, i);
            const char *base = json_object_get_string(t, "baseUrl"), *lang = json_object_get_string(t, "languageCode");
            if (!base || !lang) continue;
            char url[2048]; snprintf(url, sizeof(url), "%s%s", base[0] == '/' ? "https://www.youtube.com" : "", base);
            const char *name = json_object_dotget_string(t, "name.simpleText");
            if (!name) { JSON_Array *runs = json_object_dotget_array(t, "name.runs"); name = runs ? json_object_get_string(json_array_get_object(runs, 0), "text") : NULL; }
            const char *kind = json_object_get_string(t, "kind");
            JSON_Value *e = json_value_init_object(); JSON_Object *eo = json_value_get_object(e);
            json_object_set_string(eo, "lang", lang);
            json_object_set_string(eo, "name", name ? name : lang);
            json_object_set_string(eo, "url", url);
            json_object_set_boolean(eo, "asr", kind && !strcmp(kind, "asr"));
            json_object_set_boolean(eo, "translatable", json_object_get_boolean(t, "isTranslatable") != 0);
            json_array_append_value(ca, e);
        }
        json_object_set_value(o, "nxt_captions", cv);
    }
    json_value_free(root);
    if (!usable) { json_value_free(ov); snprintf(err, en, "no direct URLs"); return NULL; }
    char *text = json_serialize_to_string(ov);
    json_value_free(ov);
    return text;
}

/* ---------- search (WEB client, results pages of ~20) ---------- */
static long long digits_of(const char *t) {
    long long v = 0; bool any = false;
    for (; t && *t; t++) if (*t >= '0' && *t <= '9') { v = v * 10 + (*t - '0'); any = true; }
    return any ? v : 0;
}
static int duration_of(const char *t) {
    int total = 0, part = 0; bool any = false;
    for (; t && *t; t++) { if (*t >= '0' && *t <= '9') { part = part * 10 + (*t - '0'); any = true; } else if (*t == ':') { total = total * 60 + part; part = 0; } }
    return any ? total * 60 + part : 0;
}
static const char *run_text(JSON_Object *o, const char *key) {
    JSON_Object *t = json_object_get_object(o, key);
    if (!t) return NULL;
    const char *s = json_object_get_string(t, "simpleText");
    if (s) return s;
    JSON_Array *runs = json_object_get_array(t, "runs");
    return runs ? json_object_get_string(json_array_get_object(runs, 0), "text") : NULL;
}
static void take_video(JSON_Object *v, VideoList *out) {
    VideoItem it; memset(&it, 0, sizeof(it));
    const char *id = json_object_get_string(v, "videoId");
    if (!id || strlen(id) != 11) return;
    snprintf(it.id, sizeof(it.id), "%s", id);
    const char *title = run_text(v, "title"); snprintf(it.title, sizeof(it.title), "%s", title ? title : "");
    JSON_Object *owner = json_object_get_object(v, "ownerText"); if (!owner) owner = json_object_get_object(v, "longBylineText");
    JSON_Array *runs = owner ? json_object_get_array(owner, "runs") : NULL;
    JSON_Object *run0 = runs ? json_array_get_object(runs, 0) : NULL;
    if (run0) {
        snprintf(it.channel, sizeof(it.channel), "%s", json_object_get_string(run0, "text") ? json_object_get_string(run0, "text") : "");
        const char *cid = json_object_dotget_string(run0, "navigationEndpoint.browseEndpoint.browseId");
        if (cid) snprintf(it.channel_id, sizeof(it.channel_id), "%s", cid);
    }
    const char *len = run_text(v, "lengthText");
    it.duration = duration_of(len);
    const char *views = run_text(v, "viewCountText");
    it.views = digits_of(views);
    if (!len && views && (strstr(views, "watching") || strstr(views, "đang xem"))) it.live = true;
    JSON_Array *badges = json_object_get_array(v, "badges");
    for (size_t i = 0; badges && i < json_array_get_count(badges); i++) {
        const char *st = json_object_dotget_string(json_array_get_object(badges, i), "metadataBadgeRenderer.style");
        if (st && strstr(st, "LIVE")) it.live = true;
    }
    if (it.title[0]) VideoList_add(out, &it);
}
/* walks any InnerTube response: collects videoRenderer items and the next continuation token */
static void walk(JSON_Value *v, VideoList *out, char *token, size_t tn, int depth) {
    if (!v || depth > 40) return;
    JSON_Object *o = json_value_get_object(v);
    if (o) {
        JSON_Object *vr = json_object_get_object(o, "videoRenderer");
        if (vr) { take_video(vr, out); return; }
        JSON_Object *cir = json_object_get_object(o, "continuationItemRenderer");
        if (cir) {
            const char *t = json_object_dotget_string(cir, "continuationEndpoint.continuationCommand.token");
            if (t && !*token) snprintf(token, tn, "%s", t);
            return;
        }
        for (size_t i = 0; i < json_object_get_count(o); i++) walk(json_object_get_value_at(o, i), out, token, tn, depth + 1);
        return;
    }
    JSON_Array *a = json_value_get_array(v);
    for (size_t i = 0; a && i < json_array_get_count(a); i++) walk(json_array_get_value(a, i), out, token, tn, depth + 1);
}
/* YouTube Restricted Mode for searches (kids mode) */
static volatile bool safety;
void Innertube_setSafety(bool on) { safety = on; }
static bool search_page(const char *q, bool newest, const char *cont, VideoList *out, char *next, size_t nn, char *err, size_t en) {
    char vis[512], wv[48]; visitor_copy(vis, sizeof(vis), wv, sizeof(wv));
    JSON_Value *bv = json_value_init_object(); JSON_Object *b = json_value_get_object(bv);
    if (safety) json_object_dotset_boolean(b, "context.user.enableSafetyMode", 1);
    json_object_dotset_string(b, "context.client.clientName", "WEB");
    json_object_dotset_string(b, "context.client.clientVersion", wv);
    json_object_dotset_string(b, "context.client.hl", "en");
    json_object_dotset_string(b, "context.client.gl", "VN");
    if (vis[0]) json_object_dotset_string(b, "context.client.visitorData", vis);
    if (cont) json_object_set_string(b, "continuation", cont);
    else { json_object_set_string(b, "query", q); json_object_set_string(b, "params", newest ? "CAISAhAB" : "EgIQAQ%3D%3D"); }
    char *body = json_serialize_to_string(bv); json_value_free(bv);
    char headers[900], url[256];
    snprintf(headers, sizeof(headers), "Content-Type: application/json\r\nX-YouTube-Client-Name: 1\r\nX-YouTube-Client-Version: %s\r\n"
             "Origin: https://www.youtube.com\r\n%s%s%s", wv, *vis ? "X-Goog-Visitor-Id: " : "", vis, *vis ? "\r\n" : "");
    snprintf(url, sizeof(url), "%s/youtubei/v1/search?prettyPrint=false", base_url());
    char *reply = http_any(url, body, headers, CHROME_UA, err, en);
    json_free_serialized_string(body);
    if (!reply) return false;
    JSON_Value *root = json_parse_string(reply);
    free(reply);
    if (!root) { snprintf(err, en, "bad JSON"); return false; }
    *next = 0;
    int before = out->count;
    walk(root, out, next, nn, 0);
    json_value_free(root);
    if (out->count == before && !*next) { snprintf(err, en, "no results in reply"); return out->count > 0; }
    return true;
}
/* small per-query cache of what was already fetched, so "load more" only asks for new pages */
typedef struct { char q[300]; bool newest, safe; VideoList items; char next[1024]; bool done; Uint32 used; } SearchCache;
static SearchCache sc[6];
static pthread_mutex_t slock = PTHREAD_MUTEX_INITIALIZER;
bool Innertube_search(const char *q, bool newest, int start, int count, VideoList *out, char *err, size_t en) {
    pthread_mutex_lock(&slock);
    SearchCache *c = NULL, *old = &sc[0];
    for (int i = 0; i < 6; i++) { if (sc[i].q[0] && sc[i].newest == newest && sc[i].safe == safety && !strcmp(sc[i].q, q)) c = &sc[i]; if (sc[i].used < old->used) old = &sc[i]; }
    if (!c || start == 0) {           /* a fresh search starts over (results change over time) */
        if (!c) c = old;
        VideoList_clear(&c->items); memset(c, 0, sizeof(*c));
        snprintf(c->q, sizeof(c->q), "%s", q); c->newest = newest; c->safe = safety;
    }
    c->used = SDL_GetTicks();
    bool ok = true;
    for (int pages = 0; c->items.count < start + count && !c->done && pages < 6; pages++) {
        char next[1024] = "";
        if (!search_page(q, newest, c->items.count || c->next[0] ? c->next : NULL, &c->items, next, sizeof(next), err, en)) { ok = c->items.count > start; break; }
        snprintf(c->next, sizeof(c->next), "%s", next);
        if (!next[0]) c->done = true;
    }
    for (int i = start; i < c->items.count && i < start + count; i++) VideoList_add(out, &c->items.items[i]);
    pthread_mutex_unlock(&slock);
    return ok && (out->count > 0 || c->done);
}
