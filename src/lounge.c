/* Play from a phone (2.4.1): the handheld acts as a YouTube "screen" (like a smart TV), so the YouTube
 * app on a phone can pair with it ("Watch on TV" → "Link with TV code") and send videos, play / pause,
 * seek, next. This is YouTube's "lounge" protocol (the one TVs use): a pairing code, then a long-poll
 * channel that delivers the phone's commands; the screen reports what it plays back.
 * SPDX-License-Identifier: GPL-3.0-only */
#include "app.h"
#include "parson.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define L_APP "lb-v4"
#define L_UA "Mozilla/5.0 (X11; Linux aarch64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/126.0 Safari/537.36"
static const char *lbase(void) { const char *b = getenv("NXT_TUBE_LOUNGE_BASE"); return b ? b : "https://www.youtube.com"; }

static pthread_mutex_t llock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t lwake = PTHREAD_COND_INITIALIZER;
static bool enabled, running, want_code;
static char screen_name[64] = "NXT Retro Tube", screen_id[160], token[1024], dev_id[40];
static char sid[128], gsid[128]; static int rid = 1000, aid = -1, ofs;
static char code[32], code_msg[200]; static int code_state;
static LoungeCmd cmds[16]; static int ncmds;
static char remotes[64]; static int nremotes;
static char rep_body[2048]; static bool rep_pending;

static void urlenc(const char *s, char *o, size_t n) {
    size_t k = 0;
    for (const unsigned char *p = (const unsigned char *)s; *p && k + 4 < n; p++) {
        if ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') || strchr("-_.~", *p)) o[k++] = *p;
        else k += snprintf(o + k, n - k, "%%%02X", *p);
    }
    o[k] = 0;
}
static void zx(char *o) { static const char *a = "abcdefghijklmnopqrstuvwxyz0123456789"; for (int i = 0; i < 12; i++) o[i] = a[rand() % 36]; o[12] = 0; }
static void l_path(char *p, size_t n) { snprintf(p, n, "%s/lounge.json", paths.data); }
static void l_load(void) {
    char p[1300]; l_path(p, sizeof(p));
    JSON_Value *v = json_parse_file(p); JSON_Object *o = json_value_get_object(v);
    const char *a = json_object_get_string(o, "screen_id"), *d = json_object_get_string(o, "device_id");
    if (a) snprintf(screen_id, sizeof(screen_id), "%s", a);
    if (d) snprintf(dev_id, sizeof(dev_id), "%s", d);
    json_value_free(v);
    if (!dev_id[0]) {
        static const char *h = "0123456789abcdef";
        for (int i = 0; i < 36; i++) dev_id[i] = (i == 8 || i == 13 || i == 18 || i == 23) ? '-' : h[rand() % 16];
        dev_id[36] = 0;
    }
}
static void l_save(void) {
    JSON_Value *v = json_value_init_object(); JSON_Object *o = json_value_get_object(v);
    json_object_set_string(o, "screen_id", screen_id); json_object_set_string(o, "device_id", dev_id);
    char p[1300]; l_path(p, sizeof(p)); json_serialize_to_file(v, p); json_value_free(v);
}
static char *req(const char *url, const char *body, int timeout, char *err, size_t en) {
    const char *h = body ? "Content-Type: application/x-www-form-urlencoded\r\nOrigin: https://www.youtube.com\r\n" : "Origin: https://www.youtube.com\r\n";
    return Innertube_httpLong(url, body, h, L_UA, timeout, err, en);
}
/* screen id (kept), a fresh lounge token every start */
static bool l_identity(char *err, size_t en) {
    char url[600];
    if (!screen_id[0]) {
        snprintf(url, sizeof(url), "%s/api/lounge/pairing/generate_screen_id", lbase());
        char *r = req(url, NULL, 15, err, en);
        if (!r) return false;
        char *e = r + strlen(r); while (e > r && (e[-1] == '\n' || e[-1] == '\r' || e[-1] == ' ')) *--e = 0;
        snprintf(screen_id, sizeof(screen_id), "%s", r); free(r);
        if (!screen_id[0] || strchr(screen_id, '<')) { screen_id[0] = 0; snprintf(err, en, "no screen id"); return false; }
        l_save();
    }
    snprintf(url, sizeof(url), "%s/api/lounge/pairing/get_lounge_token_batch", lbase());
    char body[300]; snprintf(body, sizeof(body), "screen_ids=%s", screen_id);
    char *r = req(url, body, 15, err, en);
    if (!r) return false;
    JSON_Value *v = json_parse_string(r); free(r);
    const char *t = json_object_get_string(json_array_get_object(json_object_get_array(json_value_get_object(v), "screens"), 0), "loungeToken");
    if (t) snprintf(token, sizeof(token), "%s", t);
    json_value_free(v);
    if (!t) { snprintf(err, en, "no lounge token"); return false; }
    return true;
}
static void l_code(void) {
    char err[200] = "", url[400], body[2400], n[200], t[2000];
    urlenc(screen_name, n, sizeof(n)); urlenc(token, t, sizeof(t));
    snprintf(url, sizeof(url), "%s/api/lounge/pairing/get_pairing_code?ctx=pair", lbase());
    snprintf(body, sizeof(body), "access_type=permanent&app=" L_APP "&lounge_token=%s&screen_id=%s&screen_name=%s&device_id=%s", t, screen_id, n, dev_id);
    char *r = req(url, body, 15, err, sizeof(err));
    pthread_mutex_lock(&llock);
    if (r) {
        char d[20] = ""; int k = 0;
        for (char *p = r; *p && k < 12; p++) if (*p >= '0' && *p <= '9') d[k++] = *p;
        d[k] = 0;
        if (k == 12) { snprintf(code, sizeof(code), "%.3s %.3s %.3s %.3s", d, d + 3, d + 6, d + 9); code_state = 1; }
        else { code_state = -1; snprintf(code_msg, sizeof(code_msg), "%s", T("YouTube không cho mã ghép nối")); }
    } else { code_state = -1; snprintf(code_msg, sizeof(code_msg), T("Không lấy được mã: %s"), err); }
    pthread_mutex_unlock(&llock);
    free(r);
}
static void bind_url(char *u, size_t n, const char *rest) {
    char nm[200], tk[2000]; urlenc(screen_name, nm, sizeof(nm)); urlenc(token, tk, sizeof(tk));
    snprintf(u, n, "%s/api/lounge/bc/bind?device=LOUNGE_SCREEN&id=%s&obfuscatedGaiaId=&name=%s&app=" L_APP "&theme=cl&capabilities=dsp%%2Cmic%%2Cdpa&cst=m&mdx-version=2&loungeIdToken=%s&VER=8&v=2&t=1%s",
             lbase(), dev_id, nm, tk, rest);
}
/* the bind channel's body: "<len>\n[[n,[name,args...]],...]" chunks */
static const char *json_end(const char *p) {
    int depth = 0; bool str = false;
    for (; *p; p++) {
        if (str) { if (*p == '\\' && p[1]) p++; else if (*p == '"') str = false; continue; }
        if (*p == '"') str = true;
        else if (*p == '[' || *p == '{') depth++;
        else if (*p == ']' || *p == '}') { if (--depth == 0) return p + 1; }
    }
    return NULL;
}
static const char *arg(JSON_Object *o, const char *k, char *buf, size_t n) {
    JSON_Value *v = json_object_get_value(o, k);
    if (json_value_get_type(v) == JSONString) snprintf(buf, n, "%s", json_value_get_string(v));
    else if (json_value_get_type(v) == JSONNumber) snprintf(buf, n, "%g", json_value_get_number(v));
    else buf[0] = 0;
    return buf;
}
static void push(const LoungeCmd *c) { pthread_mutex_lock(&llock); if (ncmds < 16) cmds[ncmds++] = *c; pthread_mutex_unlock(&llock); }
static void queue_report(const char *body);
static char last_state_body[2048];
static void handle(const char *name, JSON_Object *a) {
    LoungeCmd c; memset(&c, 0, sizeof(c)); char b[2048];
    if (!strcmp(name, "c") || !strcmp(name, "S") || !strcmp(name, "noop")) return;
    Log_msg("lounge: %s", name);
    if (!strcmp(name, "setPlaylist") || !strcmp(name, "setVideo")) {
        c.kind = LC_PLAYLIST;
        snprintf(c.video, sizeof(c.video), "%s", arg(a, "videoId", b, sizeof(b)));
        snprintf(c.ids, sizeof(c.ids), "%s", arg(a, "videoIds", b, sizeof(b)));
        c.index = atoi(arg(a, "currentIndex", b, sizeof(b)));
        c.time = atof(arg(a, "currentTime", b, sizeof(b)));
        if (c.video[0]) push(&c);
    }
    else if (!strcmp(name, "play")) { c.kind = LC_PLAY; push(&c); }
    else if (!strcmp(name, "pause")) { c.kind = LC_PAUSE; push(&c); }
    else if (!strcmp(name, "seekTo")) { c.kind = LC_SEEK; c.time = atof(arg(a, "newTime", b, sizeof(b))); push(&c); }
    else if (!strcmp(name, "next")) { c.kind = LC_NEXT; push(&c); }
    else if (!strcmp(name, "previous")) { c.kind = LC_PREV; push(&c); }
    else if (!strcmp(name, "stopVideo")) { c.kind = LC_STOP; push(&c); }
    else if (!strcmp(name, "setVolume")) { c.kind = LC_VOLUME; c.volume = atoi(arg(a, "volume", b, sizeof(b))); push(&c); }
    else if (!strcmp(name, "remoteConnected") || !strcmp(name, "loungeStatus")) {
        if (!strcmp(name, "remoteConnected")) {
            c.kind = LC_CONNECTED; snprintf(c.who, sizeof(c.who), "%s", arg(a, "name", b, sizeof(b)));
            pthread_mutex_lock(&llock); nremotes++; snprintf(remotes, sizeof(remotes), "%s", c.who); pthread_mutex_unlock(&llock);
            push(&c);
        }
        if (last_state_body[0]) queue_report(last_state_body);
    }
    else if (!strcmp(name, "remoteDisconnected")) { c.kind = LC_DISCONNECTED; pthread_mutex_lock(&llock); if (nremotes) nremotes--; pthread_mutex_unlock(&llock); push(&c); }
    else if (!strcmp(name, "getNowPlaying") || !strcmp(name, "getVolume")) { if (last_state_body[0]) queue_report(last_state_body); }
}
static void parse_chunks(const char *r) {
    for (const char *p = r; p && *p; ) {
        while (*p && *p != '[') p++;
        if (!*p) break;
        const char *e = json_end(p); if (!e) break;
        char *chunk = strndup(p, e - p);
        JSON_Value *v = json_parse_string(chunk); free(chunk);
        JSON_Array *outer = json_value_get_array(v);
        for (size_t i = 0; outer && i < json_array_get_count(outer); i++) {
            JSON_Array *ev = json_array_get_array(outer, i);
            int n = (int)json_array_get_number(ev, 0);
            if (n > aid) aid = n;
            JSON_Array *body = json_array_get_array(ev, 1);
            const char *name = json_array_get_string(body, 0);
            if (!name) continue;
            if (!strcmp(name, "c")) snprintf(sid, sizeof(sid), "%s", json_array_get_string(body, 1) ? json_array_get_string(body, 1) : "");
            else if (!strcmp(name, "S")) snprintf(gsid, sizeof(gsid), "%s", json_array_get_string(body, 1) ? json_array_get_string(body, 1) : "");
            handle(name, json_array_get_object(body, 1));
        }
        json_value_free(v);
        p = e;
    }
}
static bool l_bind(char *err, size_t en) {
    char u[4000], rest[200], z[16]; zx(z);
    sid[0] = gsid[0] = 0; aid = -1; ofs = 0;
    snprintf(rest, sizeof(rest), "&RID=%d&AID=-1&CVER=1&zx=%s", rid++, z);
    bind_url(u, sizeof(u), rest);
    char *r = req(u, "count=0", 15, err, en);
    if (!r) return false;
    parse_chunks(r); free(r);
    if (!sid[0] || !gsid[0]) { snprintf(err, en, "bind: no session"); return false; }
    Log_msg("lounge: connected as \"%s\"", screen_name);
    return true;
}
/* reports go out from their own thread: the main one never waits on the network */
static pthread_cond_t rwake = PTHREAD_COND_INITIALIZER;
static void queue_report(const char *body) {
    pthread_mutex_lock(&llock);
    snprintf(rep_body, sizeof(rep_body), "%s", body); rep_pending = true;
    pthread_cond_signal(&rwake);
    pthread_mutex_unlock(&llock);
}
static void *report_thread(void *u) {
    (void)u;
    pthread_mutex_lock(&llock);
    for (;;) {
        while (!rep_pending) pthread_cond_wait(&rwake, &llock);
        char body[2400]; snprintf(body, sizeof(body), "count=2&ofs=%d&%s", ofs, rep_body); ofs += 2; rep_pending = false;
        char s1[128], g1[128]; snprintf(s1, sizeof(s1), "%s", sid); snprintf(g1, sizeof(g1), "%s", gsid);
        int r1 = rid++, a1 = aid;
        pthread_mutex_unlock(&llock);
        if (s1[0]) {
            char u[4000], rest[400], z[16], err[200] = ""; zx(z);
            snprintf(rest, sizeof(rest), "&RID=%d&SID=%s&AID=%d&gsessionid=%s&zx=%s", r1, s1, a1, g1, z);
            bind_url(u, sizeof(u), rest);
            char *r = req(u, body, 10, err, sizeof(err));
            free(r);
        }
        pthread_mutex_lock(&llock);
    }
    return NULL;
}
static void *lounge_thread(void *u) {
    (void)u;
    srand((unsigned)time(NULL) ^ (unsigned)getpid());
    l_load();
    { pthread_t t; if (!pthread_create(&t, NULL, report_thread, NULL)) pthread_detach(t); }
    int backoff = 2;
    for (;;) {
        pthread_mutex_lock(&llock);
        while (!enabled) pthread_cond_wait(&lwake, &llock);
        pthread_mutex_unlock(&llock);
        char err[300] = "";
        if (!l_identity(err, sizeof(err))) {
            Log_msg("lounge: %s", err);
            pthread_mutex_lock(&llock); if (want_code) { want_code = false; code_state = -1; snprintf(code_msg, sizeof(code_msg), T("Không kết nối được YouTube: %s"), err); } pthread_mutex_unlock(&llock);
            sleep(backoff); if (backoff < 60) backoff *= 2; continue;
        }
        if (!l_bind(err, sizeof(err))) { Log_msg("lounge: %s", err); sleep(backoff); if (backoff < 60) backoff *= 2; continue; }
        backoff = 2;
        time_t token_at = time(NULL);
        while (enabled) {
            pthread_mutex_lock(&llock); bool wc = want_code; want_code = false; pthread_mutex_unlock(&llock);
            if (wc) l_code();
            char url[4000], rest[400], z[16]; zx(z);
            snprintf(rest, sizeof(rest), "&RID=rpc&SID=%s&CI=1&AID=%d&gsessionid=%s&TYPE=xmlhttp&zx=%s", sid, aid, gsid, z);
            bind_url(url, sizeof(url), rest);
            err[0] = 0;
            char *r = req(url, NULL, 30, err, sizeof(err));
            if (r) { parse_chunks(r); free(r); continue; }
            /* a silent minute just times out: poll again; a dead session: bind again */
            if (strstr(err, "400") || strstr(err, "404") || strstr(err, "410") || strstr(err, "Bad Request") || strstr(err, "Not Found")) { Log_msg("lounge: session ended (%s), reconnecting", err); break; }
            if (time(NULL) - token_at > 12 * 3600) break;   /* fresh token now and then */
            if (!strstr(err, "timed out") && !strstr(err, "Timer") && !strstr(err, "empty")) sleep(3);
        }
    }
    return NULL;
}
void Lounge_enable(bool on, const char *name) {
    pthread_mutex_lock(&llock);
    enabled = on;
    if (name && *name) snprintf(screen_name, sizeof(screen_name), "%s", name);
    if (on && !running) { pthread_t t; running = pthread_create(&t, NULL, lounge_thread, NULL) == 0; if (running) pthread_detach(t); }
    pthread_cond_broadcast(&lwake);
    pthread_mutex_unlock(&llock);
}
bool Lounge_running(void) { return enabled; }
void Lounge_requestCode(void) {
    pthread_mutex_lock(&llock); want_code = true; code_state = 0; code[0] = 0; code_msg[0] = 0; pthread_mutex_unlock(&llock);
    Lounge_enable(true, NULL);
}
int Lounge_code(char *out, size_t n, char *msg, size_t mn) {
    pthread_mutex_lock(&llock);
    int s = code_state; snprintf(out, n, "%s", code); snprintf(msg, mn, "%s", code_msg);
    pthread_mutex_unlock(&llock);
    return s;
}
bool Lounge_take(LoungeCmd *c) {
    pthread_mutex_lock(&llock);
    bool r = ncmds > 0;
    if (r) { *c = cmds[0]; memmove(&cmds[0], &cmds[1], (ncmds - 1) * sizeof(cmds[0])); ncmds--; }
    pthread_mutex_unlock(&llock);
    return r;
}
void Lounge_report(const char *video_id, int state, double pos, double dur, int volume) {
    if (!enabled) return;
    char b[2048];
    snprintf(b, sizeof(b),
             "req0__sc=nowPlaying&req0_videoId=%s&req0_currentTime=%.3f&req0_duration=%.3f&req0_state=%d&req0_listId=&req0_currentIndex=0"
             "&req1__sc=onStateChange&req1_currentTime=%.3f&req1_duration=%.3f&req1_state=%d&req1_loadedTime=%.3f&req1_cpn=nxtretrotube00001",
             video_id ? video_id : "", pos, dur, state, pos, dur, state, dur);
    (void)volume;
    pthread_mutex_lock(&llock); snprintf(last_state_body, sizeof(last_state_body), "%s", b); pthread_mutex_unlock(&llock);
    queue_report(b);
}
int Lounge_remotes(char *who, size_t n) { pthread_mutex_lock(&llock); int r = nremotes; snprintf(who, n, "%s", remotes); pthread_mutex_unlock(&llock); return r; }
