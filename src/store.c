/* Local library: history with resume points, watch later, followed
 * channels, downloads, recent searches and settings. JSON on the SD card.
 * SPDX-License-Identifier: GPL-3.0-only */
#include "app.h"
#include "parson.h"
#include <sys/stat.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static VideoList history, later, channels, downloads, searches;
static double hist_pos[256], hist_dur[256];
static char quality[16] = "auto";
static bool dirty;

static void item_to(JSON_Object *o, const VideoItem *v) {
    json_object_set_string(o, "id", v->id); json_object_set_string(o, "title", v->title);
    json_object_set_string(o, "channel", v->channel); json_object_set_string(o, "channel_id", v->channel_id);
    json_object_set_number(o, "duration", v->duration); json_object_set_number(o, "views", (double)v->views);
    json_object_set_boolean(o, "live", v->live);
}
static void item_from(JSON_Object *o, VideoItem *v) {
    memset(v, 0, sizeof(*v));
#define S(f, k) snprintf(v->f, sizeof(v->f), "%s", json_object_get_string(o, k) ? json_object_get_string(o, k) : "")
    S(id, "id"); S(title, "title"); S(channel, "channel"); S(channel_id, "channel_id");
#undef S
    v->duration = (int)json_object_get_number(o, "duration"); v->views = (long long)json_object_get_number(o, "views");
    v->live = json_object_get_boolean(o, "live") == 1;
}
static JSON_Value *list_to(const VideoList *l, bool with_pos) {
    JSON_Value *a = json_value_init_array();
    for (int i = 0; i < l->count; i++) {
        JSON_Value *v = json_value_init_object(); item_to(json_value_get_object(v), &l->items[i]);
        if (with_pos) { json_object_set_number(json_value_get_object(v), "position", hist_pos[i]); json_object_set_number(json_value_get_object(v), "length", hist_dur[i]); }
        json_array_append_value(json_value_get_array(a), v);
    }
    return a;
}
static void list_from(JSON_Array *a, VideoList *l, bool with_pos) {
    VideoList_clear(l);
    for (size_t i = 0; a && i < json_array_get_count(a); i++) {
        JSON_Object *o = json_array_get_object(a, i); if (!o) continue;
        VideoItem v; item_from(o, &v); if (!*v.id) continue;
        if (with_pos && l->count < 256) { hist_pos[l->count] = json_object_get_number(o, "position"); hist_dur[l->count] = json_object_get_number(o, "length"); }
        VideoList_add(l, &v);
    }
}
static bool file_ok(const char *id) { struct stat st; return !stat(Store_downloadPath(id, true), &st) && st.st_size > 0; }
static void store_path(char *out, size_t n) { snprintf(out, n, "%s/library.json", paths.data); }

void Store_init(void) {
    char p[1200]; store_path(p, sizeof(p));
    JSON_Value *root = json_parse_file(p);
    JSON_Object *o = json_value_get_object(root);
    if (o) {
        list_from(json_object_get_array(o, "history"), &history, true);
        list_from(json_object_get_array(o, "later"), &later, false);
        list_from(json_object_get_array(o, "channels"), &channels, false);
        list_from(json_object_get_array(o, "downloads"), &downloads, false);
        list_from(json_object_get_array(o, "searches"), &searches, false);
        const char *q = json_object_get_string(o, "quality"); if (q) snprintf(quality, sizeof(quality), "%s", q);
    }
    json_value_free(root);
    /* forget downloads whose files are gone */
    for (int i = downloads.count - 1; i >= 0; i--) if (!file_ok(downloads.items[i].id)) { memmove(&downloads.items[i], &downloads.items[i + 1], (downloads.count - i - 1) * sizeof(VideoItem)); downloads.count--; }
}
void Store_save(void) {
    if (!dirty) return;
    JSON_Value *root = json_value_init_object(); JSON_Object *o = json_value_get_object(root);
    json_object_set_value(o, "history", list_to(&history, true));
    json_object_set_value(o, "later", list_to(&later, false));
    json_object_set_value(o, "channels", list_to(&channels, false));
    json_object_set_value(o, "downloads", list_to(&downloads, false));
    json_object_set_value(o, "searches", list_to(&searches, false));
    json_object_set_string(o, "quality", quality);
    char p[1200], tmp[1260]; store_path(p, sizeof(p)); snprintf(tmp, sizeof(tmp), "%s.tmp", p);
    if (json_serialize_to_file(root, tmp) == JSONSuccess) { rename(tmp, p); dirty = false; }
    json_value_free(root);
}
static void insert_front(VideoList *l, const VideoItem *v, int max) {
    for (int i = 0; i < l->count; i++) if (!strcmp(l->items[i].id, v->id)) { memmove(&l->items[i], &l->items[i + 1], (l->count - i - 1) * sizeof(VideoItem)); l->count--; break; }
    VideoList_add(l, v);  /* appended; rotate to front */
    VideoItem last = l->items[l->count - 1];
    memmove(&l->items[1], &l->items[0], (l->count - 1) * sizeof(VideoItem));
    l->items[0] = last;
    if (l->count > max) l->count = max;
}
void Store_addHistory(const VideoItem *v, double position, double duration) {
    if (!v || !*v->id) return;
    int old = -1; for (int i = 0; i < history.count; i++) if (!strcmp(history.items[i].id, v->id)) old = i;
    double keep_pos[256], keep_dur[256]; int n = history.count < 256 ? history.count : 256;
    memcpy(keep_pos, hist_pos, sizeof(keep_pos)); memcpy(keep_dur, hist_dur, sizeof(keep_dur));
    insert_front(&history, v, 200);
    /* rebuild positions in the new order */
    int k = 1;
    for (int i = 0; i < n && k < 256; i++) { if (i == old) continue; hist_pos[k] = keep_pos[i]; hist_dur[k] = keep_dur[i]; k++; }
    hist_pos[0] = position; hist_dur[0] = duration;
    dirty = true;
}
double Store_resumePosition(const char *id) {
    for (int i = 0; i < history.count && i < 256; i++) if (!strcmp(history.items[i].id, id)) {
        double p = hist_pos[i], d = hist_dur[i];
        if (p < 15 || (d > 0 && p > d - 20)) return 0;
        return p;
    }
    return 0;
}
const VideoList *Store_history(void) { return &history; }
double Store_historyProgress(int i) { if (i < 0 || i >= 256 || i >= history.count || hist_dur[i] <= 0) return 0; double f = hist_pos[i] / hist_dur[i]; return f > 1 ? 1 : f < 0 ? 0 : f; }
const VideoList *Store_later(void) { return &later; }
bool Store_isLater(const char *id) { for (int i = 0; i < later.count; i++) if (!strcmp(later.items[i].id, id)) return true; return false; }
bool Store_toggleLater(const VideoItem *v) {
    for (int i = 0; i < later.count; i++) if (!strcmp(later.items[i].id, v->id)) { memmove(&later.items[i], &later.items[i + 1], (later.count - i - 1) * sizeof(VideoItem)); later.count--; dirty = true; return false; }
    insert_front(&later, v, 200); dirty = true; return true;
}
const VideoList *Store_channels(void) { return &channels; }
bool Store_isChannel(const char *cid) { if (!cid || !*cid) return false; for (int i = 0; i < channels.count; i++) if (!strcmp(channels.items[i].channel_id, cid)) return true; return false; }
bool Store_toggleChannel(const VideoItem *v) {
    if (!*v->channel_id) return false;
    for (int i = 0; i < channels.count; i++) if (!strcmp(channels.items[i].channel_id, v->channel_id)) { memmove(&channels.items[i], &channels.items[i + 1], (channels.count - i - 1) * sizeof(VideoItem)); channels.count--; dirty = true; return false; }
    VideoItem c = {0};
    snprintf(c.id, sizeof(c.id), "%.11s", v->channel_id + (strlen(v->channel_id) > 11 ? strlen(v->channel_id) - 11 : 0)); /* unique key */
    snprintf(c.title, sizeof(c.title), "%s", v->channel); snprintf(c.channel, sizeof(c.channel), "%s", v->channel);
    snprintf(c.channel_id, sizeof(c.channel_id), "%s", v->channel_id);
    insert_front(&channels, &c, 100); dirty = true; return true;
}
const VideoList *Store_downloads(void) { return &downloads; }
const char *Store_downloadPath(const char *id, bool audio) {
    static char p[2][1200];
    snprintf(p[audio], sizeof(p[audio]), "%s/downloads/%s.%s", paths.data, id, audio ? "m4a" : "mp4");
    return p[audio];
}
/* in-memory check: this is called for every card on every frame */
bool Store_hasDownload(const char *id) { for (int i = 0; i < downloads.count; i++) if (!strcmp(downloads.items[i].id, id)) return true; return false; }
void Store_addDownload(const VideoItem *v) { insert_front(&downloads, v, 500); dirty = true; Store_save(); }
const char *Store_quality(void) { return quality; }
void Store_setQuality(const char *q) { snprintf(quality, sizeof(quality), "%s", q); dirty = true; }
const VideoList *Store_recentSearches(void) { return &searches; }
void Store_addSearch(const char *q) {
    VideoItem v = {0};
    snprintf(v.title, sizeof(v.title), "%s", q);
    unsigned h = 5381; for (const char *c = q; *c; c++) h = h * 33 + (unsigned char)*c;
    snprintf(v.id, sizeof(v.id), "q%08x", h);
    insert_front(&searches, &v, 12); dirty = true;
}
bool Store_saveFeed(const char *key, const VideoList *l) {
    JSON_Value *root = json_value_init_object();
    json_object_set_value(json_value_get_object(root), "items", list_to(l, false));
    json_object_set_number(json_value_get_object(root), "time", (double)time(NULL));
    char p[1300], tmp[1360]; snprintf(p, sizeof(p), "%s/feed-%s.json", paths.cache, key); snprintf(tmp, sizeof(tmp), "%s.tmp", p);
    bool ok = json_serialize_to_file(root, tmp) == JSONSuccess && !rename(tmp, p);
    json_value_free(root); return ok;
}
bool Store_loadFeed(const char *key, VideoList *l, long long *age) {
    char p[1300]; snprintf(p, sizeof(p), "%s/feed-%s.json", paths.cache, key);
    JSON_Value *root = json_parse_file(p);
    JSON_Object *o = json_value_get_object(root);
    if (!o) { json_value_free(root); return false; }
    list_from(json_object_get_array(o, "items"), l, false);
    if (age) *age = (long long)time(NULL) - (long long)json_object_get_number(o, "time");
    json_value_free(root);
    return l->count > 0;
}

void Store_removeSearch(int i) {
    if (i < 0 || i >= searches.count) return;
    memmove(&searches.items[i], &searches.items[i + 1], (searches.count - i - 1) * sizeof(VideoItem)); searches.count--; dirty = true;
}
void Store_clearHistory(void) { history.count = 0; memset(hist_pos, 0, sizeof(hist_pos)); memset(hist_dur, 0, sizeof(hist_dur)); dirty = true; }
void Store_clearSearches(void) { searches.count = 0; dirty = true; }
/* channel names watched most recently (distinct), for recommendations */
int Store_recentChannels(char out[][128], int max) {
    int n = 0;
    for (int i = 0; i < history.count && n < max; i++) {
        const char *c = history.items[i].channel; if (!*c) continue;
        bool dup = false; for (int k = 0; k < n; k++) if (!strcmp(out[k], c)) dup = true;
        if (!dup) snprintf(out[n++], 128, "%s", c);
    }
    return n;
}
