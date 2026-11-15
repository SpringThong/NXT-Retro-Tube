/* YouTube Music (2.4.1): song search and endless "radio" mixes, through music.youtube.com's own
 * web client (WEB_REMIX). Results are ordinary VideoItems, so they play like any video.
 * SPDX-License-Identifier: GPL-3.0-only */
#include "app.h"
#include "parson.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MUSIC_VER "1.20251110.01.00"
#define MUSIC_UA "Mozilla/5.0 (X11; Linux aarch64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/126.0 Safari/537.36"
static const char *mbase(void) { const char *b = getenv("NXT_TUBE_MUSIC_BASE"); return b ? b : "https://music.youtube.com"; }

static JSON_Value *remix(const char *endpoint, const char *extra, char *err, size_t en) {
    char body[3000], url[300], h[600];
    snprintf(body, sizeof(body), "{\"context\":{\"client\":{\"clientName\":\"WEB_REMIX\",\"clientVersion\":\"" MUSIC_VER "\",\"hl\":\"%s\",\"gl\":\"VN\"}},%s}", S.lang ? "en" : "vi", extra);
    snprintf(url, sizeof(url), "%s/youtubei/v1/%s?prettyPrint=false", mbase(), endpoint);
    snprintf(h, sizeof(h), "Content-Type: application/json\r\nOrigin: https://music.youtube.com\r\nReferer: https://music.youtube.com/\r\nX-YouTube-Client-Name: 67\r\nX-YouTube-Client-Version: " MUSIC_VER "\r\n");
    char *r = Innertube_http(url, body, h, MUSIC_UA, err, en);
    if (!r) return NULL;
    JSON_Value *v = json_parse_string(r);
    free(r);
    if (!v) snprintf(err, en, "bad JSON");
    return v;
}
static void runs_text(JSON_Object *t, char *out, size_t n, int which) {
    out[0] = 0;
    if (!t) return;
    const char *s = json_object_get_string(t, "simpleText");
    if (s) { snprintf(out, n, "%s", s); return; }
    JSON_Array *r = json_object_get_array(t, "runs");
    if (!r) return;
    if (which >= 0) { const char *x = json_object_get_string(json_array_get_object(r, which), "text"); snprintf(out, n, "%s", x ? x : ""); return; }
    size_t k = 0;
    for (size_t i = 0; i < json_array_get_count(r) && k + 1 < n; i++) { const char *x = json_object_get_string(json_array_get_object(r, i), "text"); if (x) k += snprintf(out + k, n - k, "%s", x); }
}
static int secs(const char *t) { int a = 0, b = 0, c = 0, n = sscanf(t, "%d:%d:%d", &a, &b, &c); return n == 3 ? a * 3600 + b * 60 + c : n == 2 ? a * 60 + b : 0; }
static bool has(const VideoList *l, const char *id) { for (int i = 0; i < l->count; i++) if (!strcmp(l->items[i].id, id)) return true; return false; }
static void add(VideoList *out, const char *id, const char *title, const char *artist, const char *dur) {
    if (!id || strlen(id) != 11 || has(out, id)) return;
    VideoItem it; memset(&it, 0, sizeof(it));
    snprintf(it.id, sizeof(it.id), "%s", id);
    snprintf(it.title, sizeof(it.title), "%s", title);
    snprintf(it.channel, sizeof(it.channel), "%s", artist);
    it.duration = secs(dur);
    VideoList_add(out, &it);
}
/* search result row: flexColumns[0] = title, [1] = "Artist • Album • 3:45" */
static void take_row(JSON_Object *r, VideoList *out) {
    const char *id = json_object_dotget_string(r, "playlistItemData.videoId");
    if (!id) id = json_object_dotget_string(r, "overlay.musicItemThumbnailOverlayRenderer.content.musicPlayButtonRenderer.playNavigationEndpoint.watchEndpoint.videoId");
    JSON_Array *fc = json_object_get_array(r, "flexColumns");
    if (!id || !fc) return;
    char title[200], line[300], artist[120] = "", dur[16] = "";
    runs_text(json_object_dotget_object(json_array_get_object(fc, 0), "musicResponsiveListItemFlexColumnRenderer.text"), title, sizeof(title), -1);
    JSON_Object *t1 = json_object_dotget_object(json_array_get_object(fc, 1), "musicResponsiveListItemFlexColumnRenderer.text");
    runs_text(t1, line, sizeof(line), -1);
    JSON_Array *runs = json_object_get_array(t1, "runs");
    if (runs && json_array_get_count(runs)) {
        const char *a = json_object_get_string(json_array_get_object(runs, 0), "text");
        /* songs: "Song • Artist • …" in some layouts; take the first run that is not a type word */
        for (size_t i = 0; i < json_array_get_count(runs); i++) {
            const char *x = json_object_get_string(json_array_get_object(runs, i), "text");
            if (!x || !strcmp(x, " • ") || !strcasecmp(x, "Song") || !strcasecmp(x, "Bài hát") || !strcasecmp(x, "Video")) continue;
            a = x; break;
        }
        snprintf(artist, sizeof(artist), "%s", a ? a : "");
        const char *last = json_object_get_string(json_array_get_object(runs, json_array_get_count(runs) - 1), "text");
        if (last && secs(last) > 0 && strchr(last, ':')) snprintf(dur, sizeof(dur), "%s", last);
    }
    add(out, id, title, artist, dur);
}
static void take_panel(JSON_Object *p, VideoList *out) {
    char title[200], artist[120], dur[16];
    runs_text(json_object_get_object(p, "title"), title, sizeof(title), -1);
    runs_text(json_object_get_object(p, "longBylineText"), artist, sizeof(artist), 0);
    if (!artist[0]) runs_text(json_object_get_object(p, "shortBylineText"), artist, sizeof(artist), 0);
    runs_text(json_object_get_object(p, "lengthText"), dur, sizeof(dur), -1);
    add(out, json_object_get_string(p, "videoId"), title, artist, dur);
}
static void walk(JSON_Value *v, VideoList *out, char *tok, size_t tn, int depth) {
    if (depth > 40) return;
    JSON_Object *o = json_value_get_object(v);
    if (o) {
        for (size_t i = 0; i < json_object_get_count(o); i++) {
            const char *k = json_object_get_name(o, i); JSON_Value *c = json_object_get_value_at(o, i);
            if (!strcmp(k, "musicResponsiveListItemRenderer")) take_row(json_value_get_object(c), out);
            else if (!strcmp(k, "playlistPanelVideoRenderer")) take_panel(json_value_get_object(c), out);
            else if (!strcmp(k, "nextContinuationData") && tok && !tok[0]) { const char *t = json_object_get_string(json_value_get_object(c), "continuation"); if (t) snprintf(tok, tn, "%s", t); }
            else walk(c, out, tok, tn, depth + 1);
        }
        return;
    }
    JSON_Array *a = json_value_get_array(v);
    for (size_t i = 0; a && i < json_array_get_count(a); i++) walk(json_array_get_value(a, i), out, tok, tn, depth + 1);
}

/* a little cache so paging (start/count) does not ask again */
typedef struct { char key[160]; VideoList items; char next[1024]; bool done; } MCache;
static MCache mc[3]; static int mc_next;
static pthread_mutex_t mlock = PTHREAD_MUTEX_INITIALIZER;
static bool feed(bool radio, const char *arg, int start, int count, VideoList *out, char *err, size_t en) {
    pthread_mutex_lock(&mlock);
    char key[160]; snprintf(key, sizeof(key), "%c%s", radio ? 'r' : 's', arg);
    MCache *c = NULL;
    for (int i = 0; i < 3; i++) if (!strcmp(mc[i].key, key)) c = &mc[i];
    if (!c || start == 0) {
        if (!c) { c = &mc[mc_next]; mc_next = (mc_next + 1) % 3; }
        VideoList_clear(&c->items); memset(c, 0, sizeof(*c)); snprintf(c->key, sizeof(c->key), "%s", key);
    }
    bool ok = true;
    for (int pages = 0; c->items.count < start + count && !c->done && pages < 4; pages++) {
        char extra[2400], q[300]; int before = c->items.count;
        if (c->next[0]) snprintf(extra, sizeof(extra), "\"continuation\":\"%s\"", c->next);
        else if (radio) snprintf(extra, sizeof(extra), "\"videoId\":\"%s\",\"playlistId\":\"RDAMVM%s\",\"isAudioOnly\":true,\"enablePersistentPlaylistPanel\":true", arg, arg);
        else {
            size_t k = 0; for (const char *p = arg; *p && k + 3 < sizeof(q); p++) { if (*p == '"' || *p == '\\') q[k++] = '\\'; q[k++] = *p; } q[k] = 0;
            snprintf(extra, sizeof(extra), "\"query\":\"%s\",\"params\":\"EgWKAQIIAWoKEAkQBRAKEAMQBA%%3D%%3D\"", q);   /* songs only */
        }
        pthread_mutex_unlock(&mlock);
        JSON_Value *v = remix(radio ? "next" : "search", extra, err, en);
        pthread_mutex_lock(&mlock);
        if (!v) { ok = c->items.count > start; c->done = true; break; }
        char tok[1024] = "";
        if (radio && c->items.count == 0) { /* the song itself first */ }
        walk(v, &c->items, tok, sizeof(tok), 0);
        json_value_free(v);
        if (!tok[0] || !strcmp(tok, c->next) || c->items.count == before) c->done = true;
        snprintf(c->next, sizeof(c->next), "%s", tok);
    }
    for (int i = start; i < c->items.count && i < start + count; i++) VideoList_add(out, &c->items.items[i]);
    if (ok && !out->count && start == 0) { snprintf(err, en, "%s", T("YouTube Music không trả về bài nào")); ok = false; }
    pthread_mutex_unlock(&mlock);
    return ok;
}
bool Music_search(const char *q, int start, int count, VideoList *out, char *err, size_t en) { return feed(false, q, start, count, out, err, en); }
bool Music_radio(const char *id, int start, int count, VideoList *out, char *err, size_t en) { return feed(true, id, start, count, out, err, en); }
