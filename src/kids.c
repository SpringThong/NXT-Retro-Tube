/* Kids mode (parental controls): PIN, YouTube Restricted Mode, approved channels,
 * blocked words, daily time limit, viewing hours, eye-break reminder, and a
 * viewing report for the parent. Stored in kids.json next to settings.json.
 * SPDX-License-Identifier: GPL-3.0-only */
#include "app.h"
#include "parson.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <ctype.h>

Kids K;
static bool dirty;
static char path[1200];

/* ---------- text folding: lower case, Vietnamese accents removed ---------- */
static const char *VN_FROM[] = {
    "à","á","ạ","ả","ã","â","ầ","ấ","ậ","ẩ","ẫ","ă","ằ","ắ","ặ","ẳ","ẵ","À","Á","Ạ","Ả","Ã","Â","Ầ","Ấ","Ậ","Ẩ","Ẫ","Ă","Ằ","Ắ","Ặ","Ẳ","Ẵ",
    "è","é","ẹ","ẻ","ẽ","ê","ề","ế","ệ","ể","ễ","È","É","Ẹ","Ẻ","Ẽ","Ê","Ề","Ế","Ệ","Ể","Ễ",
    "ì","í","ị","ỉ","ĩ","Ì","Í","Ị","Ỉ","Ĩ",
    "ò","ó","ọ","ỏ","õ","ô","ồ","ố","ộ","ổ","ỗ","ơ","ờ","ớ","ợ","ở","ỡ","Ò","Ó","Ọ","Ỏ","Õ","Ô","Ồ","Ố","Ộ","Ổ","Ỗ","Ơ","Ờ","Ớ","Ợ","Ở","Ỡ",
    "ù","ú","ụ","ủ","ũ","ư","ừ","ứ","ự","ử","ữ","Ù","Ú","Ụ","Ủ","Ũ","Ư","Ừ","Ứ","Ự","Ử","Ữ",
    "ỳ","ý","ỵ","ỷ","ỹ","Ỳ","Ý","Ỵ","Ỷ","Ỹ","đ","Đ", NULL};
static const char VN_TO[] =
    "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa" "eeeeeeeeeeeeeeeeeeeeee" "iiiiiiiiii"
    "oooooooooooooooooooooooooooooooooo" "uuuuuuuuuuuuuuuuuuuuuu" "yyyyyyyyyy" "dd";
void Kids_fold(const char *in, char *out, size_t n) {
    size_t k = 0;
    while (*in && k + 1 < n) {
        unsigned char c = (unsigned char)*in;
        if (c < 0x80) { out[k++] = (char)tolower(c); in++; continue; }
        bool done = false;
        for (int i = 0; VN_FROM[i]; i++) {
            size_t l = strlen(VN_FROM[i]);
            if (!strncmp(in, VN_FROM[i], l)) { out[k++] = VN_TO[i]; in += l; done = true; break; }
        }
        if (done) continue;
        int len = (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 1;
        for (int i = 0; i < len && *in && k + 1 < n; i++) out[k++] = *in++;
    }
    out[k] = 0;
}

/* ---------- PIN ---------- */
static unsigned long pin_hash(const char *pin) {
    unsigned long h = 1469598103UL;            /* FNV-1a with a fixed salt: not plain text in kids.json */
    const char *salt = "nxt-retro-tube-kids:";
    for (const char *p = salt; *p; p++) { h ^= (unsigned char)*p; h *= 16777619UL; }
    for (const char *p = pin; *p; p++) { h ^= (unsigned char)*p; h *= 16777619UL; }
    return h & 0xffffffffUL;
}
bool Kids_hasPin(void) { return K.pin_set; }
bool Kids_checkPin(const char *pin) { return K.pin_set && pin_hash(pin) == K.pin; }
void Kids_setPin(const char *pin) { K.pin = pin_hash(pin); K.pin_set = true; dirty = true; Kids_save(); }

/* ---------- days ---------- */
static void today(char *out, size_t n) {
    time_t t = time(NULL); struct tm tm; localtime_r(&t, &tm);
    strftime(out, n, "%Y-%m-%d", &tm);
}
static int hour_now(void) { time_t t = time(NULL); struct tm tm; localtime_r(&t, &tm); return tm.tm_hour; }
static KidsDay *day_slot(void) {
    char d[12]; today(d, sizeof(d));
    for (int i = 0; i < K.ndays; i++) if (!strcmp(K.days[i].day, d)) return &K.days[i];
    /* new day: newest first, keep 14 */
    if (K.ndays < KIDS_DAYS) K.ndays++;
    memmove(&K.days[1], &K.days[0], sizeof(K.days[0]) * (K.ndays - 1));
    memset(&K.days[0], 0, sizeof(K.days[0]));
    snprintf(K.days[0].day, sizeof(K.days[0].day), "%s", d);
    dirty = true;
    return &K.days[0];
}
int Kids_usedToday(void) { return day_slot()->secs; }
int Kids_extraToday(void) { return day_slot()->extra_min; }
void Kids_grantExtra(int minutes) { day_slot()->extra_min += minutes; dirty = true; Kids_save(); }
int Kids_leftToday(void) {
    if (!K.daily_min) return -1;
    int left = (K.daily_min + day_slot()->extra_min) * 60 - day_slot()->secs;
    return left < 0 ? 0 : left;
}
bool Kids_outsideHours(void) {
    if (K.from_h == K.to_h || (K.from_h == 0 && K.to_h == 24)) return false;
    int h = hour_now();
    if (K.from_h < K.to_h) return h < K.from_h || h >= K.to_h;
    return h < K.from_h && h >= K.to_h;           /* e.g. 18 -> 8 overnight window */
}
/* 0 free to watch, 1 daily time used up, 2 outside the allowed hours */
int Kids_lockReason(void) {
    if (!K.enabled) return 0;
    if (Kids_outsideHours() && !day_slot()->hours_waived) return 2;
    if (K.daily_min && Kids_leftToday() == 0) return 1;
    return 0;
}
void Kids_waiveHoursToday(void) { day_slot()->hours_waived = true; dirty = true; Kids_save(); }

/* called once a second while a video (or its sound) plays */
void Kids_tick(const VideoItem *v) {
    if (!K.enabled) return;
    KidsDay *d = day_slot();
    d->secs++; dirty = true;
    K.since_break++;
    if (v && v->id[0]) {
        KidsLog *l = K.nlog ? &K.log[0] : NULL;
        if (!l || strcmp(l->id, v->id) || strcmp(l->day, d->day)) {
            if (K.nlog < KIDS_LOG) K.nlog++;
            memmove(&K.log[1], &K.log[0], sizeof(K.log[0]) * (K.nlog - 1));
            l = &K.log[0]; memset(l, 0, sizeof(*l));
            snprintf(l->day, sizeof(l->day), "%s", d->day);
            snprintf(l->id, sizeof(l->id), "%s", v->id);
            snprintf(l->title, sizeof(l->title), "%s", v->title);
            snprintf(l->channel, sizeof(l->channel), "%s", v->channel);
            time_t t = time(NULL); struct tm tm; localtime_r(&t, &tm);
            snprintf(l->at, sizeof(l->at), "%02d:%02d", tm.tm_hour, tm.tm_min);
        }
        l->secs++;
    }
    if (d->secs % 30 == 0) Kids_save();
}
bool Kids_breakDue(void) { return K.enabled && K.break_min > 0 && K.since_break >= K.break_min * 60; }
void Kids_breakDone(void) { K.since_break = 0; }

/* ---------- content rules ---------- */
bool Kids_approved(const char *channel_id) {
    for (int i = 0; i < K.napproved; i++) if (channel_id && !strcmp(K.approved[i].id, channel_id)) return true;
    return false;
}
bool Kids_toggleApproved(const char *channel_id, const char *name) {
    for (int i = 0; i < K.napproved; i++) if (!strcmp(K.approved[i].id, channel_id)) {
        memmove(&K.approved[i], &K.approved[i + 1], sizeof(K.approved[0]) * (K.napproved - i - 1)); K.napproved--;
        dirty = true; Kids_save(); return false;
    }
    if (K.napproved >= KIDS_CHANNELS || !channel_id[0]) return false;
    snprintf(K.approved[K.napproved].id, sizeof(K.approved[0].id), "%s", channel_id);
    snprintf(K.approved[K.napproved].name, sizeof(K.approved[0].name), "%s", name && *name ? name : channel_id);
    K.napproved++; dirty = true; Kids_save();
    return true;
}
bool Kids_addWord(const char *w) {
    char f[64]; Kids_fold(w, f, sizeof(f));
    char *s = f; while (*s == ' ') s++;
    size_t l = strlen(s); while (l && s[l - 1] == ' ') s[--l] = 0;
    if (!*s || K.nwords >= KIDS_WORDS) return false;
    for (int i = 0; i < K.nwords; i++) if (!strcmp(K.words[i], s)) return false;
    snprintf(K.words[K.nwords++], sizeof(K.words[0]), "%s", s);
    dirty = true; Kids_save();
    return true;
}
void Kids_removeWord(int i) {
    if (i < 0 || i >= K.nwords) return;
    memmove(&K.words[i], &K.words[i + 1], sizeof(K.words[0]) * (K.nwords - i - 1)); K.nwords--;
    dirty = true; Kids_save();
}
void Kids_removeApproved(int i) {
    if (i < 0 || i >= K.napproved) return;
    memmove(&K.approved[i], &K.approved[i + 1], sizeof(K.approved[0]) * (K.napproved - i - 1)); K.napproved--;
    dirty = true; Kids_save();
}
static bool has_word(const char *text) {
    if (!K.nwords || !text) return false;
    char f[600]; Kids_fold(text, f, sizeof(f));
    for (int i = 0; i < K.nwords; i++) if (strstr(f, K.words[i])) return true;
    return false;
}
bool Kids_queryBlocked(const char *q) { return K.enabled && has_word(q); }
bool Kids_allowed(const VideoItem *v) {
    if (!K.enabled) return true;
    if (has_word(v->title) || has_word(v->channel)) return false;
    if (K.approved_only && !Kids_approved(v->channel_id)) return false;
    return true;
}
int Kids_filter(VideoList *l) {
    if (!K.enabled || !l) return 0;
    int w = 0, removed = 0;
    for (int i = 0; i < l->count; i++) {
        if (Kids_allowed(&l->items[i])) { if (w != i) l->items[w] = l->items[i]; w++; }
        else removed++;
    }
    l->count = w;
    return removed;
}

/* ---------- storage ---------- */
void Kids_load(void) {
    memset(&K, 0, sizeof(K));
    K.daily_min = 60; K.from_h = 0; K.to_h = 24; K.break_min = 30;   /* hours: all day until the parent sets them (the clock may be off) */
    snprintf(path, sizeof(path), "%s/kids.json", paths.data);
    JSON_Value *root = json_parse_file(path);
    JSON_Object *o = json_value_get_object(root);
    if (o) {
        K.enabled = json_object_get_boolean(o, "enabled") == 1;
        if (json_object_has_value(o, "pin")) { K.pin = (unsigned long)json_object_get_number(o, "pin"); K.pin_set = true; }
        if (json_object_has_value(o, "daily_min")) K.daily_min = (int)json_object_get_number(o, "daily_min");
        if (json_object_has_value(o, "from_h")) K.from_h = (int)json_object_get_number(o, "from_h");
        if (json_object_has_value(o, "to_h")) K.to_h = (int)json_object_get_number(o, "to_h");
        if (json_object_has_value(o, "break_min")) K.break_min = (int)json_object_get_number(o, "break_min");
        K.approved_only = json_object_get_boolean(o, "approved_only") == 1;
        JSON_Array *a = json_object_get_array(o, "approved");
        for (size_t i = 0; a && i < json_array_get_count(a) && K.napproved < KIDS_CHANNELS; i++) {
            JSON_Object *c = json_array_get_object(a, i);
            const char *id = json_object_get_string(c, "id"), *nm = json_object_get_string(c, "name");
            if (!id) continue;
            snprintf(K.approved[K.napproved].id, sizeof(K.approved[0].id), "%s", id);
            snprintf(K.approved[K.napproved].name, sizeof(K.approved[0].name), "%s", nm ? nm : id);
            K.napproved++;
        }
        a = json_object_get_array(o, "words");
        for (size_t i = 0; a && i < json_array_get_count(a) && K.nwords < KIDS_WORDS; i++) {
            const char *w = json_array_get_string(a, i); if (w) snprintf(K.words[K.nwords++], sizeof(K.words[0]), "%s", w);
        }
        a = json_object_get_array(o, "days");
        for (size_t i = 0; a && i < json_array_get_count(a) && K.ndays < KIDS_DAYS; i++) {
            JSON_Object *d = json_array_get_object(a, i);
            const char *day = json_object_get_string(d, "day"); if (!day) continue;
            KidsDay *s = &K.days[K.ndays++];
            snprintf(s->day, sizeof(s->day), "%s", day);
            s->secs = (int)json_object_get_number(d, "secs");
            s->extra_min = (int)json_object_get_number(d, "extra_min");
            s->hours_waived = json_object_get_boolean(d, "hours_waived") == 1;
        }
        a = json_object_get_array(o, "log");
        for (size_t i = 0; a && i < json_array_get_count(a) && K.nlog < KIDS_LOG; i++) {
            JSON_Object *e = json_array_get_object(a, i);
            KidsLog *l = &K.log[K.nlog++];
            snprintf(l->day, sizeof(l->day), "%s", json_object_get_string(e, "day") ? json_object_get_string(e, "day") : "");
            snprintf(l->at, sizeof(l->at), "%s", json_object_get_string(e, "at") ? json_object_get_string(e, "at") : "");
            snprintf(l->id, sizeof(l->id), "%s", json_object_get_string(e, "id") ? json_object_get_string(e, "id") : "");
            snprintf(l->title, sizeof(l->title), "%s", json_object_get_string(e, "title") ? json_object_get_string(e, "title") : "");
            snprintf(l->channel, sizeof(l->channel), "%s", json_object_get_string(e, "channel") ? json_object_get_string(e, "channel") : "");
            l->secs = (int)json_object_get_number(e, "secs");
        }
    }
    json_value_free(root);
    if (K.enabled && !K.pin_set) K.enabled = false;       /* never locked without a way back in */
    Log_msg("kids mode %s, limit %d min/day, hours %d-%d, break %d min, %d approved channels, %d blocked words",
            K.enabled ? "on" : "off", K.daily_min, K.from_h, K.to_h, K.break_min, K.napproved, K.nwords);
}
void Kids_save(void) {
    if (!dirty || !path[0]) return;
    JSON_Value *root = json_value_init_object(); JSON_Object *o = json_value_get_object(root);
    json_object_set_boolean(o, "enabled", K.enabled);
    if (K.pin_set) json_object_set_number(o, "pin", (double)K.pin);
    json_object_set_number(o, "daily_min", K.daily_min);
    json_object_set_number(o, "from_h", K.from_h);
    json_object_set_number(o, "to_h", K.to_h);
    json_object_set_number(o, "break_min", K.break_min);
    json_object_set_boolean(o, "approved_only", K.approved_only);
    JSON_Value *a = json_value_init_array();
    for (int i = 0; i < K.napproved; i++) {
        JSON_Value *c = json_value_init_object();
        json_object_set_string(json_value_get_object(c), "id", K.approved[i].id);
        json_object_set_string(json_value_get_object(c), "name", K.approved[i].name);
        json_array_append_value(json_value_get_array(a), c);
    }
    json_object_set_value(o, "approved", a);
    a = json_value_init_array();
    for (int i = 0; i < K.nwords; i++) json_array_append_string(json_value_get_array(a), K.words[i]);
    json_object_set_value(o, "words", a);
    a = json_value_init_array();
    for (int i = 0; i < K.ndays; i++) {
        JSON_Value *d = json_value_init_object(); JSON_Object *dd = json_value_get_object(d);
        json_object_set_string(dd, "day", K.days[i].day);
        json_object_set_number(dd, "secs", K.days[i].secs);
        json_object_set_number(dd, "extra_min", K.days[i].extra_min);
        json_object_set_boolean(dd, "hours_waived", K.days[i].hours_waived);
        json_array_append_value(json_value_get_array(a), d);
    }
    json_object_set_value(o, "days", a);
    a = json_value_init_array();
    for (int i = 0; i < K.nlog; i++) {
        JSON_Value *e = json_value_init_object(); JSON_Object *ee = json_value_get_object(e);
        json_object_set_string(ee, "day", K.log[i].day); json_object_set_string(ee, "at", K.log[i].at);
        json_object_set_string(ee, "id", K.log[i].id); json_object_set_string(ee, "title", K.log[i].title);
        json_object_set_string(ee, "channel", K.log[i].channel); json_object_set_number(ee, "secs", K.log[i].secs);
        json_array_append_value(json_value_get_array(a), e);
    }
    json_object_set_value(o, "log", a);
    char tmp[1300]; snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    if (json_serialize_to_file_pretty(root, tmp) == JSONSuccess) rename(tmp, path);
    json_value_free(root);
    dirty = false;
}
void Kids_changed(void) { dirty = true; Kids_save(); }
void Kids_clearReport(void) { K.nlog = 0; dirty = true; Kids_save(); }   /* the daily totals stay: they drive the time limit */
