/* NXT Retro Tube — Google account sign-in (YouTube TV device sign-in) and the signed-in feeds.
 *
 * Sign-in works like the YouTube app on a TV: the app asks YouTube for a short code, you open
 * google.com/device on a phone, type the code and allow access. The app never sees the password.
 * What it keeps (data/account.json): a refresh token (to get short-lived access tokens), the
 * account name, the device id. Signing out deletes the file.
 *
 * With an access token the app asks YouTube's TV client (TVHTML5) for: the personal home feed
 * (FEwhat_to_watch), the subscriptions feed (FEsubscriptions), liked videos (VLLL),
 * Watch later (VLWL) and history (FEhistory). Their items are TV "tiles"; the walker below also
 * understands the web shapes (videoRenderer, gridVideoRenderer, lockupViewModel).
 * Playback itself does not use the account.
 * SPDX-License-Identifier: GPL-3.0-only */
#include "app.h"
#include "parson.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdarg.h>
#include <sys/stat.h>

char *Innertube_http(const char *url, const char *body, const char *headers, const char *ua, char *err, size_t en);

/* the YouTube TV app's OAuth client (also found in its base.js, see discover_client) */
#define TV_CLIENT_ID "861556708454-d6dlm3lh05idd8npek18k6be8ba3oc68.apps.googleusercontent.com"
#define TV_CLIENT_SECRET "SboVhoG9s0rNafixCSGGKXAT"
#define TV_SCOPE "http://gdata.youtube.com https://www.googleapis.com/auth/youtube-paid-content"
#define TV_NAME "TVHTML5"
#define TV_NAME_ID "7"
#define TV_VERSION "7.20260901.15.00"
#define TV_UA "Mozilla/5.0 (Linux armeabi-v7a; Android 7.1.2; Fire OS 6.0) Cobalt/22.lts.3.306369-gold (unlike Gecko) v8/8.8.278.8-jit gles Starboard/13, Amazon_ATV_mediatek8695_2019/NS6294 (Amazon, AFTMM, Wireless) com.amazon.firetv.youtube/22.3.r2.v66.0"

static const char *base(void) { const char *b = getenv("NXT_TUBE_INNERTUBE_BASE"); return b ? b : "https://www.youtube.com"; }

static pthread_mutex_t alock = PTHREAD_MUTEX_INITIALIZER;
static struct {
    char refresh[512], access[2048], name[128], handle[96], device[48];
    char client_id[128], client_secret[64];
    long long access_until;                 /* unix seconds */
    /* sign-in in progress */
    int login;                              /* ACC_* */
    char code[32], url[160], device_code[512], msg[200];
    long long code_until;
    bool cancel;
} AC;

static void acc_path(char *p, size_t n) { snprintf(p, n, "%s/account.json", paths.data); }
static void save(void) {
    char p[1100]; acc_path(p, sizeof(p));
    if (!AC.refresh[0]) { unlink(p); return; }
    JSON_Value *v = json_value_init_object(); JSON_Object *o = json_value_get_object(v);
    json_object_set_string(o, "refresh_token", AC.refresh);
    json_object_set_string(o, "name", AC.name);
    json_object_set_string(o, "handle", AC.handle);
    json_object_set_string(o, "device_id", AC.device);
    json_object_set_string(o, "client_id", AC.client_id);
    json_object_set_string(o, "client_secret", AC.client_secret);
    char tmp[1120]; snprintf(tmp, sizeof(tmp), "%s.tmp", p);
    if (json_serialize_to_file(v, tmp) == JSONSuccess) { chmod(tmp, 0600); rename(tmp, p); }
    json_value_free(v);
}
static void new_device_id(void) {
    unsigned char r[16]; int fd = open("/dev/urandom", O_RDONLY);
    if (fd < 0 || read(fd, r, 16) != 16) { srand((unsigned)time(NULL) ^ (unsigned)getpid()); for (int i = 0; i < 16; i++) r[i] = (unsigned char)rand(); }
    if (fd >= 0) close(fd);
    r[6] = (r[6] & 0x0F) | 0x40; r[8] = (r[8] & 0x3F) | 0x80;
    snprintf(AC.device, sizeof(AC.device), "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
             r[0], r[1], r[2], r[3], r[4], r[5], r[6], r[7], r[8], r[9], r[10], r[11], r[12], r[13], r[14], r[15]);
}
void Account_init(void) {
    char p[1100]; acc_path(p, sizeof(p));
    JSON_Value *v = json_parse_file(p);
    JSON_Object *o = json_value_get_object(v);
    #define GETS(k, dst) do { const char *s_ = o ? json_object_get_string(o, k) : NULL; snprintf(dst, sizeof(dst), "%s", s_ ? s_ : ""); } while (0)
    GETS("refresh_token", AC.refresh); GETS("name", AC.name); GETS("handle", AC.handle); GETS("device_id", AC.device);
    GETS("client_id", AC.client_id); GETS("client_secret", AC.client_secret);
    #undef GETS
    json_value_free(v);
    if (!AC.device[0]) new_device_id();
    if (!AC.client_id[0]) { snprintf(AC.client_id, sizeof(AC.client_id), "%s", TV_CLIENT_ID); snprintf(AC.client_secret, sizeof(AC.client_secret), "%s", TV_CLIENT_SECRET); }
    if (AC.refresh[0]) Log_msg("account: signed in as %s", AC.name[0] ? AC.name : "?");
}
bool Account_signedIn(void) { pthread_mutex_lock(&alock); bool r = AC.refresh[0] != 0; pthread_mutex_unlock(&alock); return r; }
const char *Account_name(void) { return AC.name[0] ? AC.name : AC.handle; }
void Account_logout(void) {
    pthread_mutex_lock(&alock);
    AC.refresh[0] = AC.access[0] = AC.name[0] = AC.handle[0] = 0; AC.access_until = 0;
    new_device_id();
    save();
    pthread_mutex_unlock(&alock);
    Log_msg("account: signed out");
}

/* ---------- HTTP helpers ---------- */
#define SAMSUNG_UA "Mozilla/5.0 (SMART-TV; Linux; Tizen 2.4.0) AppleWebKit/538.1 (KHTML, like Gecko) Version/2.4.0 TV Safari/538.1"
static JSON_Value *post_json_ua(const char *url, const char *body, const char *extra_headers, const char *ua, char *err, size_t en) {
    char h[3000];
    snprintf(h, sizeof(h), "Content-Type: application/json\r\nOrigin: https://www.youtube.com\r\nReferer: https://www.youtube.com/tv\r\n%s", extra_headers ? extra_headers : "");
    char *r = Innertube_http(url, body, h, ua, err, en);
    if (!r) return NULL;
    JSON_Value *v = json_parse_string(r);
    free(r);
    if (!v) snprintf(err, en, "bad JSON");
    return v;
}
static JSON_Value *post_json(const char *url, const char *body, const char *extra_headers, char *err, size_t en) { return post_json_ua(url, body, extra_headers, TV_UA, err, en); }
/* signatureTimestamp of the current YouTube player: the TV player call answers "please reload
 * this page" without it (and then carries no watch-history URLs). Read once from the player JS
 * whose version iframe_api names, kept in sts.txt until the version changes (checked once a day). */
static int sts_get(void) {
    static int cached; static time_t checked;
    char path[1300]; snprintf(path, sizeof(path), "%s/sts2.txt", paths.data);
    char hash[64] = ""; int sts = 0; long when = 0;
    FILE *f = fopen(path, "r");
    if (f) { if (fscanf(f, "%63s %d %ld", hash, &sts, &when) != 3) sts = 0; fclose(f); }
    time_t now = time(NULL);
    if (cached && now - checked < 3600) return cached;
    if (sts > 0 && now - when < 86400) { cached = sts; checked = now; return sts; }
    char err[200] = "", url[400];
    snprintf(url, sizeof(url), "%s/iframe_api", base());
    char *api = Innertube_http(url, NULL, "Referer: https://www.youtube.com/\r\n", TV_UA, err, sizeof(err));
    char h[64] = "";
    const char *p = api ? strstr(api, "player\\/") : NULL;
    if (p) { p += 8; size_t k = 0; while (*p && *p != '\\' && *p != '/' && k + 1 < sizeof(h)) h[k++] = *p++; h[k] = 0; }
    free(api);
    if (!h[0]) { Log_msg("account: player version not found (%s)", err); checked = now; cached = sts; return sts; }
    if (sts > 0 && !strcmp(h, hash)) { cached = sts; checked = now; f = fopen(path, "w"); if (f) { fprintf(f, "%s %d %ld\n", h, sts, (long)now); fclose(f); } return sts; }
    static const char *js[2] = {"player_ias.vflset/en_US/base.js", "tv-player-ias.vflset/tv-player-ias.js"};
    int got = 0;
    for (int i = 0; i < 2 && !got; i++) {
        snprintf(url, sizeof(url), "%s/s/player/%s/%s", base(), h, js[i]);
        char *body = Innertube_http(url, NULL, "Referer: https://www.youtube.com/\r\n", TV_UA, err, sizeof(err));
        if (!body) continue;
        const char *q = strstr(body, "signatureTimestamp:");
        if (q) got = atoi(q + 19);
        if (!got && (q = strstr(body, ",sts:"))) got = atoi(q + 5);
        free(body);
    }
    if (got > 0) {
        f = fopen(path, "w"); if (f) { fprintf(f, "%s %d %ld\n", h, got, (long)now); fclose(f); }
        Log_msg("account: player %s, signatureTimestamp %d", h, got);
        sts = got;
    } else Log_msg("account: signatureTimestamp not found in player %s (%s)", h, err);
    cached = sts; checked = now;
    return sts;
}
/* the TV app's current clientVersion, from youtube.com/tv itself: the player call answers
 * "please reload this page" to a version it no longer takes (feeds still accept older ones) */
static const char *tv_live_version(void) {
    static char ver[48]; static time_t checked;
    time_t now = time(NULL);
    if (ver[0] && now - checked < 6 * 3600) return ver;
    char path[1300]; snprintf(path, sizeof(path), "%s/tvver.txt", paths.data);
    long when = 0; char saved[48] = "";
    FILE *f = fopen(path, "r");
    if (f) { if (fscanf(f, "%47s %ld", saved, &when) != 2) saved[0] = 0; fclose(f); }
    if (saved[0] && now - when < 86400) { snprintf(ver, sizeof(ver), "%s", saved); checked = now; return ver; }
    char err[200] = "", url[300]; snprintf(url, sizeof(url), "%s/tv", base());
    char *page = Innertube_http(url, NULL, "Referer: https://www.youtube.com/\r\n", TV_UA, err, sizeof(err));
    const char *keys[3] = {"\"INNERTUBE_CLIENT_VERSION\":\"", "\"clientVersion\":\"7.", "INNERTUBE_CONTEXT_CLIENT_VERSION\":\""};
    char found[48] = "";
    for (int i = 0; page && i < 3 && !found[0]; i++) {
        const char *q = strstr(page, keys[i]);
        if (!q) continue;
        q += strlen(keys[i]); if (i == 1) q -= 2;
        size_t k = 0; while (*q && *q != '"' && k + 1 < sizeof(found)) found[k++] = *q++;
        found[k] = 0;
        if (strncmp(found, "7.", 2)) found[0] = 0;
    }
    free(page);
    if (found[0]) {
        snprintf(ver, sizeof(ver), "%s", found);
        f = fopen(path, "w"); if (f) { fprintf(f, "%s %ld\n", found, (long)now); fclose(f); }
        Log_msg("account: TV app version %s", found);
    } else { Log_msg("account: TV app version not found (%s)", err[0] ? err : "page"); snprintf(ver, sizeof(ver), "%s", saved[0] ? saved : TV_VERSION); }
    checked = now;
    return ver;
}
/* the TV app's client id / secret from its own base.js, if the built-in ones are refused */
static bool discover_client(void) {
    char err[200], url[300];
    snprintf(url, sizeof(url), "%s/tv", base());
    char *page = Innertube_http(url, NULL, "Referer: https://www.youtube.com/\r\n", TV_UA, err, sizeof(err));
    if (!page) { Log_msg("account: TV page: %s", err); return false; }
    bool ok = false;
    for (char *p = strstr(page, "src=\""); p && !ok; p = strstr(p + 5, "src=\"")) {
        char src[600]; size_t k = 0; const char *q = p + 5;
        while (*q && *q != '"' && k + 1 < sizeof(src)) src[k++] = *q++;
        src[k] = 0;
        if (!strstr(src, ".js") || (!strstr(src, "base") && !strstr(src, "kabuki") && !strstr(src, "main"))) continue;
        char full[800]; snprintf(full, sizeof(full), "%s%s", !strncmp(src, "http", 4) ? "" : base(), src);
        char *js = Innertube_http(full, NULL, "Referer: https://www.youtube.com/tv\r\n", TV_UA, err, sizeof(err));
        if (!js) continue;
        /* clientId:"<id>.apps.googleusercontent.com",xx:"<secret>" — the first one is the TV's */
        for (char *c = strstr(js, "clientId:\""); c; c = strstr(c + 10, "clientId:\"")) {
            char id[128]; k = 0; const char *s = c + 10;
            while (*s && *s != '"' && k + 1 < sizeof(id)) id[k++] = *s++;
            id[k] = 0;
            if (!strstr(id, ".apps.googleusercontent.com") || *s != '"') continue;
            s++; if (*s == ',') s++; if (*s == '\n') s++;
            while (*s && *s != ':' && *s != '"' && *s != ',') s++;
            if (*s != ':' || s[1] != '"') continue;
            s += 2;
            char sec[64]; k = 0;
            while (*s && *s != '"' && k + 1 < sizeof(sec)) sec[k++] = *s++;
            sec[k] = 0;
            if (k < 10) continue;
            pthread_mutex_lock(&alock);
            snprintf(AC.client_id, sizeof(AC.client_id), "%s", id); snprintf(AC.client_secret, sizeof(AC.client_secret), "%s", sec);
            pthread_mutex_unlock(&alock);
            Log_msg("account: client id from %s", src);
            ok = true; break;
        }
        free(js);
    }
    free(page);
    return ok;
}

/* ---------- access token ---------- */
static bool refresh_access(char *err, size_t en) {
    char body[1200], url[300];
    pthread_mutex_lock(&alock);
    snprintf(body, sizeof(body), "{\"refresh_token\":\"%s\",\"client_id\":\"%s\",\"client_secret\":\"%s\",\"grant_type\":\"refresh_token\"}", AC.refresh, AC.client_id, AC.client_secret);
    pthread_mutex_unlock(&alock);
    snprintf(url, sizeof(url), "%s/o/oauth2/token", base());
    JSON_Value *v = post_json(url, body, NULL, err, en);
    JSON_Object *o = json_value_get_object(v);
    const char *tok = o ? json_object_get_string(o, "access_token") : NULL;
    const char *e = o ? json_object_get_string(o, "error") : NULL;
    bool ok = tok && *tok;
    if (ok) {
        pthread_mutex_lock(&alock);
        snprintf(AC.access, sizeof(AC.access), "%s", tok);
        double ex = json_object_get_number(o, "expires_in"); if (ex < 60) ex = 3600;
        AC.access_until = (long long)time(NULL) + (long long)ex - 120;
        pthread_mutex_unlock(&alock);
    } else if (e) snprintf(err, en, "%s", e);
    json_value_free(v);
    if (!ok) Log_msg("account: token refresh failed: %s", err);
    return ok;
}
/* "Authorization: Bearer …\r\n" (refreshed when it is about to expire) */
static bool bearer(char *h, size_t n, bool force, char *err, size_t en) {
    pthread_mutex_lock(&alock);
    bool have = AC.refresh[0], fresh = AC.access[0] && (long long)time(NULL) < AC.access_until;
    pthread_mutex_unlock(&alock);
    if (!have) { snprintf(err, en, "not signed in"); return false; }
    if ((force || !fresh) && !refresh_access(err, en)) return false;
    pthread_mutex_lock(&alock);
    snprintf(h, n, "Authorization: Bearer %s\r\n", AC.access);
    pthread_mutex_unlock(&alock);
    return true;
}
static char *tv_body(const char *extra) {
    static const char *fmt = "{\"context\":{\"client\":{\"clientName\":\"" TV_NAME "\",\"clientVersion\":\"" TV_VERSION "\",\"hl\":\"%s\",\"gl\":\"VN\","
                             "\"tvAppInfo\":{\"appQuality\":\"TV_APP_QUALITY_FULL_ANIMATION\",\"zylonLeftNav\":true},\"webpSupport\":false,\"animatedWebpSupport\":false},"
                             "\"user\":{\"enableSafetyMode\":false}},%s}";
    size_t n = strlen(fmt) + strlen(extra) + 16;
    char *b = malloc(n);
    if (b) snprintf(b, n, fmt, S.lang ? "en" : "vi", extra);
    return b;
}
/* an InnerTube call as the signed-in TV app (one retry with a new token on 401) */
static JSON_Value *tv_call(const char *endpoint, const char *extra, char *err, size_t en) {
    char *body = tv_body(extra); if (!body) return NULL;
    char url[300]; snprintf(url, sizeof(url), "%s/youtubei/v1/%s?prettyPrint=false", base(), endpoint);
    JSON_Value *v = NULL;
    for (int attempt = 0; attempt < 2 && !v; attempt++) {
        char auth[2200], h[3300];
        if (!bearer(auth, sizeof(auth), attempt > 0, err, en)) break;
        snprintf(h, sizeof(h), "%sX-YouTube-Client-Name: " TV_NAME_ID "\r\nX-YouTube-Client-Version: " TV_VERSION "\r\n", auth);
        const char *vis = !strcmp(endpoint, "player") ? Innertube_visitor() : NULL;   /* the player wants the visitor too */
        if (vis && *vis) { size_t l = strlen(h); snprintf(h + l, sizeof(h) - l, "X-Goog-Visitor-Id: %s\r\n", vis); }
        v = post_json(url, body, h, err, en);
        if (!v && !strstr(err, "401") && !strstr(err, "Unauthorized")) break;
    }
    free(body);
    return v;
}

/* the same call as another TV client (embedded TV player), for the history URLs */
static JSON_Value *tv_call_as(const char *name, const char *ver, const char *id, const char *endpoint, const char *extra, char *err, size_t en) {
    size_t n = strlen(extra) + 800; char *body = malloc(n); if (!body) return NULL;
    bool novis = !strcmp(name, "TVHTML5_SAMSUNG_ACC");   /* signed-in call only: no anonymous visitor id next to the account token */
    bool samsung = novis || !strcmp(name, "TVHTML5_SAMSUNG");
    if (samsung)   /* the Tizen TV profile: YouTube still gives it a full player answer */
        snprintf(body, n, "{\"context\":{\"client\":{\"clientName\":\"TVHTML5\",\"clientVersion\":\"%s\",\"hl\":\"%s\",\"gl\":\"VN\","
                 "\"deviceMake\":\"Samsung\",\"deviceModel\":\"UKS9800\",\"osName\":\"Tizen\",\"osVersion\":\"2.4.0\",\"userAgent\":\"" SAMSUNG_UA "\"},"
                 "\"user\":{\"enableSafetyMode\":false}},%s}", ver, S.lang ? "en" : "vi", extra);
    else if (!strcmp(name, "TVHTML5"))
        snprintf(body, n, "{\"context\":{\"client\":{\"clientName\":\"TVHTML5\",\"clientVersion\":\"%s\",\"hl\":\"%s\",\"gl\":\"VN\","
                 "\"tvAppInfo\":{\"appQuality\":\"TV_APP_QUALITY_FULL_ANIMATION\",\"zylonLeftNav\":true},\"webpSupport\":false,\"animatedWebpSupport\":false},"
                 "\"user\":{\"enableSafetyMode\":false}},%s}", ver, S.lang ? "en" : "vi", extra);
    else
        snprintf(body, n, "{\"context\":{\"client\":{\"clientName\":\"%s\",\"clientVersion\":\"%s\",\"hl\":\"%s\",\"gl\":\"VN\"},"
                 "\"thirdParty\":{\"embedUrl\":\"https://www.youtube.com/\"}},%s}", name, ver, S.lang ? "en" : "vi", extra);
    char url[300]; snprintf(url, sizeof(url), "%s/youtubei/v1/%s?prettyPrint=false", base(), endpoint);
    JSON_Value *v = NULL;
    char auth[2200], h[3300];
    if (bearer(auth, sizeof(auth), false, err, en)) {
        snprintf(h, sizeof(h), "%sX-YouTube-Client-Name: %s\r\nX-YouTube-Client-Version: %s\r\n", auth, id, ver);
        const char *vis = novis ? NULL : Innertube_visitor();
        if (vis && *vis) { size_t l = strlen(h); snprintf(h + l, sizeof(h) - l, "X-Goog-Visitor-Id: %s\r\n", vis); }
        v = post_json_ua(url, body, h, samsung ? SAMSUNG_UA : TV_UA, err, en);
    }
    free(body);
    return v;
}
/* ---------- account name ---------- */
static const char *text_of(JSON_Object *t, char *buf, size_t n) {
    buf[0] = 0;
    if (!t) return buf;
    const char *s = json_object_get_string(t, "simpleText");
    if (!s) s = json_object_get_string(t, "content");
    if (s) { snprintf(buf, n, "%s", s); return buf; }
    JSON_Array *runs = json_object_get_array(t, "runs");
    for (size_t i = 0; runs && i < json_array_get_count(runs); i++) {
        const char *r = json_object_get_string(json_array_get_object(runs, i), "text");
        if (r) { size_t l = strlen(buf); snprintf(buf + l, n - l, "%s", r); }
    }
    return buf;
}
static void find_account(JSON_Value *v, int depth) {
    if (!v || depth > 30 || AC.name[0]) return;
    JSON_Object *o = json_value_get_object(v);
    if (o) {
        JSON_Object *ai = json_object_get_object(o, "accountItem");
        if (ai) {
            char b[160];
            pthread_mutex_lock(&alock);
            snprintf(AC.name, sizeof(AC.name), "%s", text_of(json_object_get_object(ai, "accountName"), b, sizeof(b)));
            snprintf(AC.handle, sizeof(AC.handle), "%s", text_of(json_object_get_object(ai, "channelHandle"), b, sizeof(b)));
            pthread_mutex_unlock(&alock);
            return;
        }
        for (size_t i = 0; i < json_object_get_count(o); i++) find_account(json_object_get_value_at(o, i), depth + 1);
        return;
    }
    JSON_Array *a = json_value_get_array(v);
    for (size_t i = 0; a && i < json_array_get_count(a); i++) find_account(json_array_get_value(a, i), depth + 1);
}
static void fetch_name(void) {
    char err[200];
    JSON_Value *v = tv_call("account/accounts_list", "\"accountReadMask\":{\"returnOwner\":true,\"returnBrandAccounts\":true,\"returnPersonaAccounts\":false}", err, sizeof(err));
    if (!v) { Log_msg("account: accounts_list: %s", err); return; }
    AC.name[0] = 0; find_account(v, 0);
    json_value_free(v);
    Log_msg("account: name %s %s", AC.name, AC.handle);
}

/* ---------- sign-in (device code) ---------- */
static void login_msg(int state, const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    pthread_mutex_lock(&alock);
    AC.login = state; vsnprintf(AC.msg, sizeof(AC.msg), fmt, ap);
    pthread_mutex_unlock(&alock);
    va_end(ap);
}
static bool request_code(char *err, size_t en) {
    char body[700], url[300];
    pthread_mutex_lock(&alock);
    snprintf(body, sizeof(body), "{\"client_id\":\"%s\",\"device_id\":\"%s\",\"model_name\":\"ytlr::\",\"scope\":\"" TV_SCOPE "\"}", AC.client_id, AC.device);
    pthread_mutex_unlock(&alock);
    snprintf(url, sizeof(url), "%s/o/oauth2/device/code", base());
    JSON_Value *v = post_json(url, body, NULL, err, en);
    JSON_Object *o = json_value_get_object(v);
    const char *dc = o ? json_object_get_string(o, "device_code") : NULL, *uc = o ? json_object_get_string(o, "user_code") : NULL;
    const char *vu = o ? json_object_get_string(o, "verification_url") : NULL;
    bool ok = dc && uc;
    if (ok) {
        pthread_mutex_lock(&alock);
        snprintf(AC.device_code, sizeof(AC.device_code), "%s", dc);
        snprintf(AC.code, sizeof(AC.code), "%s", uc);
        snprintf(AC.url, sizeof(AC.url), "%s", vu && *vu ? vu : "https://www.google.com/device");
        double ex = json_object_get_number(o, "expires_in"); if (ex < 60) ex = 1800;
        AC.code_until = (long long)time(NULL) + (long long)ex;
        pthread_mutex_unlock(&alock);
    } else if (o && json_object_get_string(o, "error")) snprintf(err, en, "%s", json_object_get_string(o, "error"));
    json_value_free(v);
    return ok;
}
static void *login_thread(void *arg) {
    (void)arg;
    char err[200] = "";
    login_msg(ACC_WORKING, "%s", T("Đang lấy mã đăng nhập…"));
    bool ok = request_code(err, sizeof(err));
    if (!ok && (strstr(err, "invalid_client") || strstr(err, "401") || strstr(err, "Unauthorized") || strstr(err, "400"))) {
        Log_msg("account: built-in client refused (%s), reading the TV app's", err);
        if (discover_client()) ok = request_code(err, sizeof(err));
    }
    if (!ok) { Log_msg("account: device code: %s", err); login_msg(ACC_ERROR, T("Không lấy được mã đăng nhập (%s)"), err); return NULL; }
    Log_msg("account: code %s at %s", AC.code, AC.url);
    login_msg(ACC_SHOW, "%s", T("Đang chờ bạn xác nhận trên điện thoại…"));
    int interval = 5;
    char body[1400], url[300];
    pthread_mutex_lock(&alock);
    snprintf(body, sizeof(body), "{\"code\":\"%s\",\"client_id\":\"%s\",\"client_secret\":\"%s\",\"grant_type\":\"http://oauth.net/grant_type/device/1.0\"}", AC.device_code, AC.client_id, AC.client_secret);
    pthread_mutex_unlock(&alock);
    snprintf(url, sizeof(url), "%s/o/oauth2/token", base());
    for (;;) {
        for (int i = 0; i < interval * 10; i++) { if (AC.cancel) { login_msg(ACC_IDLE, "%s", ""); return NULL; } usleep(100000); }
        if ((long long)time(NULL) > AC.code_until) { login_msg(ACC_ERROR, "%s", T("Mã đã hết hạn · bấm A để lấy mã mới")); return NULL; }
        JSON_Value *v = post_json(url, body, NULL, err, sizeof(err));
        JSON_Object *o = json_value_get_object(v);
        char ebuf[64] = ""; const char *e = NULL;
        if (o && json_object_get_string(o, "error")) { snprintf(ebuf, sizeof(ebuf), "%s", json_object_get_string(o, "error")); e = ebuf; }
        const char *rt = o ? json_object_get_string(o, "refresh_token") : NULL, *at = o ? json_object_get_string(o, "access_token") : NULL;
        if (rt && at) {
            pthread_mutex_lock(&alock);
            snprintf(AC.refresh, sizeof(AC.refresh), "%s", rt);
            snprintf(AC.access, sizeof(AC.access), "%s", at);
            double ex = json_object_get_number(o, "expires_in"); if (ex < 60) ex = 3600;
            AC.access_until = (long long)time(NULL) + (long long)ex - 120;
            pthread_mutex_unlock(&alock);
            json_value_free(v);
            login_msg(ACC_WORKING, "%s", T("Đã xác nhận · đang lấy tên tài khoản…"));
            fetch_name();
            pthread_mutex_lock(&alock); save(); pthread_mutex_unlock(&alock);
            Log_msg("account: signed in as %s", AC.name);
            login_msg(ACC_DONE, "%s", "");
            return NULL;
        }
        json_value_free(v);
        if (e && !strcmp(e, "authorization_pending")) continue;
        if (e && !strcmp(e, "slow_down")) { interval += 3; continue; }
        if (!v && (strstr(err, "428") || strstr(err, "4XX"))) continue;          /* pending, answered as an HTTP error */
        if (e && (!strcmp(e, "access_denied") || !strcmp(e, "expired_token"))) {
            login_msg(ACC_ERROR, "%s", !strcmp(e, "access_denied") ? T("Bạn đã từ chối trên điện thoại") : T("Mã đã hết hạn · bấm A để lấy mã mới"));
            return NULL;
        }
        if (!v) { Log_msg("account: token poll: %s", err); interval = interval < 15 ? interval + 2 : interval; continue; }   /* network: keep trying until the code expires */
        Log_msg("account: token poll: %s", e ? e : "?");
        login_msg(ACC_ERROR, T("Đăng nhập lỗi (%s)"), e ? e : "?");
        return NULL;
    }
}
void Account_startLogin(void) {
    pthread_mutex_lock(&alock);
    if (AC.login == ACC_WORKING || AC.login == ACC_SHOW) { pthread_mutex_unlock(&alock); return; }
    AC.cancel = false; AC.login = ACC_WORKING; AC.code[0] = AC.msg[0] = 0;
    pthread_mutex_unlock(&alock);
    pthread_t t;
    if (pthread_create(&t, NULL, login_thread, NULL) == 0) pthread_detach(t);
    else login_msg(ACC_ERROR, "%s", "thread");
}
void Account_cancelLogin(void) { AC.cancel = true; }
int Account_loginState(char *code, size_t cn, char *url, size_t un, char *msg, size_t mn, int *secs_left) {
    pthread_mutex_lock(&alock);
    int s = AC.login;
    snprintf(code, cn, "%s", AC.code); snprintf(url, un, "%s", AC.url); snprintf(msg, mn, "%s", AC.msg);
    *secs_left = AC.code_until ? (int)(AC.code_until - (long long)time(NULL)) : 0;
    if (s == ACC_DONE) AC.login = ACC_IDLE;          /* reported once */
    pthread_mutex_unlock(&alock);
    return s;
}

/* ---------- feeds ---------- */
static void tile_lines(JSON_Object *meta, char *l0, size_t n0, char *l1, size_t n1) {
    l0[0] = l1[0] = 0;
    JSON_Array *lines = json_object_get_array(meta, "lines");
    for (size_t i = 0; lines && i < json_array_get_count(lines) && i < 2; i++) {
        JSON_Array *items = json_object_dotget_array(json_array_get_object(lines, i), "lineRenderer.items");
        char *dst = i ? l1 : l0; size_t dn = i ? n1 : n0;
        for (size_t k = 0; items && k < json_array_get_count(items); k++) {
            char b[200]; text_of(json_object_dotget_object(json_array_get_object(items, k), "lineItemRenderer.text"), b, sizeof(b));
            if (!b[0]) continue;
            size_t l = strlen(dst); snprintf(dst + l, dn - l, "%s", b);
        }
    }
}
static long long count_of(const char *t) {
    /* "1.2M views", "1,2 Tr lượt xem", "12K views", "345 views" */
    if (!t) return 0;
    double v = 0; const char *p = t; while (*p && !(*p >= '0' && *p <= '9')) p++;
    if (!*p) return 0;
    char num[32]; size_t k = 0;
    while (*p && ((*p >= '0' && *p <= '9') || *p == '.' || *p == ',') && k + 1 < sizeof(num)) { num[k++] = *p == ',' ? '.' : *p; p++; }
    num[k] = 0; v = atof(num);
    while (*p == ' ') p++;
    if (*p == 'K' || *p == 'k' || !strncmp(p, "N ", 2) || !strncmp(p, "N\xc2\xa0", 3)) v *= 1e3;
    else if (*p == 'M' || !strncmp(p, "Tr", 2)) v *= 1e6;
    else if (*p == 'B' || !strncmp(p, "Tỷ", 4)) v *= 1e9;
    else { /* plain number with thousand separators */ long long n = 0; for (const char *q = t; *q && *q != ' '; q++) if (*q >= '0' && *q <= '9') n = n * 10 + (*q - '0'); return n; }
    return (long long)v;
}
static int secs_of(const char *t) {
    int total = 0, part = 0; bool any = false;
    for (; t && *t; t++) { if (*t >= '0' && *t <= '9') { part = part * 10 + (*t - '0'); any = true; } else if (*t == ':') { total = total * 60 + part; part = 0; } else if (any) break; }
    return any ? total * 60 + part : 0;
}
/* "continue watching": how far the account got in a video, from the red bar YouTube draws on its thumbnail
 * (TV/web: thumbnailOverlayResumePlaybackRenderer.percentDurationWatched, new UI: progress bar startPercent) */
static double pct_of(JSON_Value *v, int depth) {
    JSON_Object *o = json_value_get_object(v);
    if (depth > 10) return 0;
    if (o) {
        if (json_object_has_value_of_type(o, "percentDurationWatched", JSONNumber)) return json_object_get_number(o, "percentDurationWatched");
        if (json_object_has_value_of_type(o, "startPercent", JSONNumber)) return json_object_get_number(o, "startPercent");
        for (size_t i = 0; i < json_object_get_count(o); i++) {
            const char *k = json_object_get_name(o, i);
            /* only look where overlays live: skip titles, menus and endpoints */
            if (!strstr(k, "verlay") && !strstr(k, "header") && !strstr(k, "Header") && !strstr(k, "contentImage") && !strstr(k, "thumbnail") &&
                !strstr(k, "Renderer") && !strstr(k, "ViewModel") && !strstr(k, "progress") && !strstr(k, "Progress")) continue;
            double p = pct_of(json_object_get_value_at(o, i), depth + 1);
            if (p > 0) return p;
        }
        return 0;
    }
    JSON_Array *a = json_value_get_array(v);
    for (size_t i = 0; a && i < json_array_get_count(a); i++) { double p = pct_of(json_array_get_value(a, i), depth + 1); if (p > 0) return p; }
    return 0;
}
#define RESUME_N 400
static struct { char id[12]; float f; } resume[RESUME_N];
static int resume_next;
static pthread_mutex_t rlock = PTHREAD_MUTEX_INITIALIZER;
static void note_resume(const char *id, double pct) {
    if (!id || strlen(id) != 11 || pct <= 0) return;
    pthread_mutex_lock(&rlock);
    int k = -1;
    for (int i = 0; i < RESUME_N; i++) if (!strcmp(resume[i].id, id)) { k = i; break; }
    if (k < 0) { k = resume_next; resume_next = (resume_next + 1) % RESUME_N; snprintf(resume[k].id, sizeof(resume[k].id), "%s", id); }
    resume[k].f = (float)(pct / 100.0);
    pthread_mutex_unlock(&rlock);
}
double Account_resumeSeconds(const char *id, double duration) {
    if (!id || duration <= 0) return 0;
    float f = 0;
    pthread_mutex_lock(&rlock);
    for (int i = 0; i < RESUME_N; i++) if (!strcmp(resume[i].id, id)) { f = resume[i].f; break; }
    pthread_mutex_unlock(&rlock);
    double p = f * duration;
    if (f <= 0 || f >= 0.97 || p < 15 || p > duration - 20) return 0;
    return p - 3;   /* a little before, so you catch the sentence you stopped in */
}
static double cur_pct;   /* set by take_* right before add_item (feed walk runs under flock) */
static void add_item(VideoList *out, const char *id, const char *title, const char *channel, const char *cid, const char *dur, const char *views, bool live) {
    if (!id || strlen(id) != 11 || !title || !*title) return;
    for (int i = 0; i < out->count; i++) if (!strcmp(out->items[i].id, id)) return;
    VideoItem it; memset(&it, 0, sizeof(it));
    snprintf(it.id, sizeof(it.id), "%s", id); snprintf(it.title, sizeof(it.title), "%s", title);
    snprintf(it.channel, sizeof(it.channel), "%s", channel ? channel : "");
    snprintf(it.channel_id, sizeof(it.channel_id), "%s", cid ? cid : "");
    it.duration = live ? 0 : secs_of(dur);
    it.views = count_of(views);
    it.live = live;
    if (cur_pct > 0 && !live) { it.watched = (float)(cur_pct / 100.0); note_resume(id, cur_pct); }
    VideoList_add(out, &it);
}
static void take_tile(JSON_Object *t, VideoList *out) {
    const char *style = json_object_get_string(t, "style"), *ctype = json_object_get_string(t, "contentType");
    if (style && strstr(style, "SHORTS")) return;
    if (ctype && !strstr(ctype, "VIDEO")) return;
    const char *id = json_object_dotget_string(t, "onSelectCommand.watchEndpoint.videoId");
    if (!id) id = json_object_get_string(t, "contentId");
    JSON_Object *meta = json_object_dotget_object(t, "metadata.tileMetadataRenderer");
    char title[400], l0[200], l1[200], dur[40] = "";
    text_of(meta ? json_object_get_object(meta, "title") : NULL, title, sizeof(title));
    if (meta) tile_lines(meta, l0, sizeof(l0), l1, sizeof(l1)); else l0[0] = l1[0] = 0;
    bool live = false;
    JSON_Array *ov = json_object_dotget_array(t, "header.tileHeaderRenderer.thumbnailOverlays");
    for (size_t i = 0; ov && i < json_array_get_count(ov); i++) {
        JSON_Object *ts = json_object_dotget_object(json_array_get_object(ov, i), "thumbnailOverlayTimeStatusRenderer");
        if (!ts) continue;
        const char *st = json_object_get_string(ts, "style");
        if (st && strstr(st, "LIVE")) live = true;
        if (st && strstr(st, "SHORTS")) return;
        text_of(json_object_get_object(ts, "text"), dur, sizeof(dur));
    }
    /* line 1 is the channel, line 2 "views • when" (when both exist) */
    cur_pct = pct_of(json_object_get_value(t, "header"), 0);
    add_item(out, id, title, l1[0] ? l0 : "", NULL, dur, l1[0] ? l1 : l0, live);
}
static void take_web(JSON_Object *v, VideoList *out) {
    char title[400], ch[200], dur[40], views[80];
    text_of(json_object_get_object(v, "title"), title, sizeof(title));
    JSON_Object *owner = json_object_get_object(v, "ownerText");
    if (!owner) owner = json_object_get_object(v, "shortBylineText");
    if (!owner) owner = json_object_get_object(v, "longBylineText");
    text_of(owner, ch, sizeof(ch));
    const char *cid = owner ? json_object_dotget_string(json_array_get_object(json_object_get_array(owner, "runs"), 0), "navigationEndpoint.browseEndpoint.browseId") : NULL;
    text_of(json_object_get_object(v, "lengthText"), dur, sizeof(dur));
    text_of(json_object_get_object(v, "viewCountText"), views, sizeof(views));
    bool live = !dur[0] && (strstr(views, "watching") || strstr(views, "đang xem"));
    cur_pct = pct_of(json_object_get_value(v, "thumbnailOverlays"), 0);
    add_item(out, json_object_get_string(v, "videoId"), title, ch, cid, dur, views, live);
}
static void take_lockup(JSON_Object *l, VideoList *out) {
    const char *ct = json_object_get_string(l, "contentType");
    if (ct && !strstr(ct, "VIDEO")) return;
    JSON_Object *m = json_object_dotget_object(l, "metadata.lockupMetadataViewModel");
    const char *title = m ? json_object_dotget_string(m, "title.content") : NULL;
    char ch[200] = "", views[120] = "", dur[40] = "";
    JSON_Array *rows = m ? json_object_dotget_array(m, "metadata.contentMetadataViewModel.metadataRows") : NULL;
    for (size_t r = 0; rows && r < json_array_get_count(rows) && r < 2; r++) {
        JSON_Array *parts = json_object_get_array(json_array_get_object(rows, r), "metadataParts");
        const char *s = parts ? json_object_dotget_string(json_array_get_object(parts, 0), "text.content") : NULL;
        if (s) snprintf(r ? views : ch, r ? sizeof(views) : sizeof(ch), "%s", s);
    }
    JSON_Array *ovl = json_object_dotget_array(l, "contentImage.thumbnailViewModel.overlays");
    bool live = false;
    for (size_t i = 0; ovl && i < json_array_get_count(ovl); i++) {
        JSON_Array *b = json_object_dotget_array(json_array_get_object(ovl, i), "thumbnailOverlayBadgeViewModel.thumbnailBadges");
        JSON_Object *bv = b ? json_object_get_object(json_array_get_object(b, 0), "thumbnailBadgeViewModel") : NULL;
        const char *t = bv ? json_object_get_string(bv, "text") : NULL, *st = bv ? json_object_get_string(bv, "badgeStyle") : NULL;
        if (t) snprintf(dur, sizeof(dur), "%s", t);
        if (st && strstr(st, "LIVE")) live = true;
    }
    cur_pct = pct_of(json_object_get_value(l, "contentImage"), 0);
    add_item(out, json_object_get_string(l, "contentId"), title, ch, NULL, dur, views, live);
}
/* the list's own "next page" token wins over a shelf's (horizontal) one */
static bool list_key(const char *k) {
    return k && (strstr(k, "sectionList") || strstr(k, "gridRenderer") || strstr(k, "gridContinuation") || strstr(k, "SurfaceContent") ||
                 strstr(k, "richGrid") || strstr(k, "playlistVideoList") || strstr(k, "itemSection"));
}
static void walk(JSON_Value *v, const char *key, VideoList *out, char *tok, size_t tn, int *tprio, int depth) {
    if (!v || depth > 60) return;
    JSON_Object *o = json_value_get_object(v);
    if (o) {
        JSON_Object *x;
        if ((x = json_object_get_object(o, "tileRenderer"))) { take_tile(x, out); return; }
        if ((x = json_object_get_object(o, "videoRenderer")) || (x = json_object_get_object(o, "gridVideoRenderer")) ||
            (x = json_object_get_object(o, "compactVideoRenderer")) || (x = json_object_get_object(o, "playlistVideoRenderer"))) { take_web(x, out); return; }
        if ((x = json_object_get_object(o, "lockupViewModel"))) { take_lockup(x, out); return; }
        /* continuation tokens: TV ("continuations":[{"nextContinuationData":…}]) and web (continuationItemRenderer) */
        JSON_Array *cs = json_object_get_array(o, "continuations");
        for (size_t i = 0; cs && i < json_array_get_count(cs); i++) {
            JSON_Object *ce = json_array_get_object(cs, i);
            const char *t = json_object_dotget_string(ce, "nextContinuationData.continuation");
            if (!t) t = json_object_dotget_string(ce, "reloadContinuationData.continuation");
            int pr = list_key(key) ? 2 : 1;
            if (t && pr > *tprio) { snprintf(tok, tn, "%s", t); *tprio = pr; }
        }
        x = json_object_get_object(o, "continuationItemRenderer");
        if (x) {
            const char *t = json_object_dotget_string(x, "continuationEndpoint.continuationCommand.token");
            if (t && 1 > *tprio) { snprintf(tok, tn, "%s", t); *tprio = 1; }
            return;
        }
        for (size_t i = 0; i < json_object_get_count(o); i++) walk(json_object_get_value_at(o, i), json_object_get_name(o, i), out, tok, tn, tprio, depth + 1);
        return;
    }
    JSON_Array *a = json_value_get_array(v);
    for (size_t i = 0; a && i < json_array_get_count(a); i++) walk(json_array_get_value(a, i), key, out, tok, tn, tprio, depth + 1);
}
typedef struct { char id[64]; VideoList items; char next[2048]; bool done; Uint32 used; int dry; } FeedCache;
static FeedCache fc[5];
static pthread_mutex_t flock = PTHREAD_MUTEX_INITIALIZER;
bool Account_feed(const char *browse, int start, int count, VideoList *out, char *err, size_t en) {
    if (!Account_signedIn()) { snprintf(err, en, "not signed in"); return false; }
    pthread_mutex_lock(&flock);
    FeedCache *c = NULL, *old = &fc[0];
    for (int i = 0; i < 5; i++) { if (!strcmp(fc[i].id, browse)) c = &fc[i]; if (fc[i].used < old->used) old = &fc[i]; }
    if (!c || start == 0) {
        if (!c) c = old;
        VideoList_clear(&c->items); memset(c, 0, sizeof(*c));
        snprintf(c->id, sizeof(c->id), "%s", browse);
    }
    c->used = SDL_GetTicks();
    bool ok = true;
    /* YouTube's TV pages often repeat videos already shown (or are all Shorts): keep following the
     * token through up to 4 such pages in a row before calling the feed finished */
    for (int pages = 0; c->items.count < start + count && !c->done && pages < 10; pages++) {
        char extra[2300];
        if (c->next[0]) snprintf(extra, sizeof(extra), "\"continuation\":\"%s\"", c->next);
        else snprintf(extra, sizeof(extra), "\"browseId\":\"%s\"", browse);
        JSON_Value *v = tv_call("browse", extra, err, en);
        if (!v) { ok = c->items.count > start; break; }
        int before = c->items.count, prio = 0;
        char tok[2048] = "";
        walk(v, NULL, &c->items, tok, sizeof(tok), &prio, 0);
        json_value_free(v);
        bool same = tok[0] && !strcmp(tok, c->next);
        c->dry = c->items.count == before ? c->dry + 1 : 0;
        if (!tok[0] || same || c->dry >= 4) {
            c->done = true;
            Log_msg("account feed %s ends after %d videos: %s", browse, c->items.count, !tok[0] ? "no next-page token" : same ? "same token again" : "4 pages without new videos");
        }
        snprintf(c->next, sizeof(c->next), "%s", tok);
    }
    for (int i = start; i < c->items.count && i < start + count; i++) VideoList_add(out, &c->items.items[i]);
    if (ok && !out->count && !c->done) { snprintf(err, en, "empty"); ok = false; c->done = true; }
    if (ok && !c->items.count) snprintf(err, en, "no videos");
    pthread_mutex_unlock(&flock);
    return ok && (out->count > 0 || c->done);
}

/* ---------- 2.4.1: things you do here reach your Google account ----------
 * like / subscribe through the TV app's own calls; watch history and "continue watching" by the
 * same playback / watch-time pings the TV app sends (videostats URLs from its player answer). */
static pthread_mutex_t wlock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t wwake = PTHREAD_COND_INITIALIZER;
static bool w_running;
static struct { int kind; char id[48]; double pos, dur; bool paused, on; } wq[16];
static int wq_n;
static char w_vid[16], w_cpn[20], w_play[2048], w_time[2048];
static double w_last_et;
static char fb_play[1400], fb_time[1400];
enum { WJ_START = 1, WJ_TIME, WJ_LIKE, WJ_SUB, WJ_LSTATE, WJ_CHANS };
static int ls_sub = -1;   /* is the account subscribed to that video's channel: -1 not known, 0, 1 (handed out once) */
static char ls_id[16]; static int ls_val = -1;   /* like state of a video as the account sees it */
static int find_bool(JSON_Value *v, const char *key, int depth) {   /* -1 when the key is nowhere */
    if (depth > 30) return -1;
    JSON_Object *o = json_value_get_object(v);
    if (o) {
        JSON_Value *x = json_object_get_value(o, key);
        if (x && json_value_get_type(x) == JSONBoolean) return json_value_get_boolean(x);
        for (size_t i = 0; i < json_object_get_count(o); i++) { int r = find_bool(json_object_get_value_at(o, i), key, depth + 1); if (r >= 0) return r; }
        return -1;
    }
    JSON_Array *a = json_value_get_array(v);
    for (size_t i = 0; a && i < json_array_get_count(a); i++) { int r = find_bool(json_array_get_value(a, i), key, depth + 1); if (r >= 0) return r; }
    return -1;
}
static const char *find_str(JSON_Value *v, const char *key, int depth) {
    if (depth > 30) return NULL;
    JSON_Object *o = json_value_get_object(v);
    if (o) {
        const char *x = json_object_get_string(o, key);
        if (x) return x;
        for (size_t i = 0; i < json_object_get_count(o); i++) { const char *r = find_str(json_object_get_value_at(o, i), key, depth + 1); if (r) return r; }
        return NULL;
    }
    JSON_Array *a = json_value_get_array(v);
    for (size_t i = 0; a && i < json_array_get_count(a); i++) { const char *r = find_str(json_array_get_value(a, i), key, depth + 1); if (r) return r; }
    return NULL;
}
static void new_cpn(void) {
    static const char *A64 = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    for (int i = 0; i < 16; i++) w_cpn[i] = A64[rand() % 64];
    w_cpn[16] = 0;
}
/* set (replace) one query parameter of a tracking URL */
static bool w_samsung; static unsigned w_t0;
static void set_q(char *url, size_t n, const char *key, const char *val) {
    char pat[40]; size_t kl = strlen(key);
    for (int d = 0; d < 2; d++) {
        snprintf(pat, sizeof(pat), "%c%s=", d ? '&' : '?', key);
        for (char *s; (s = strstr(url, pat)); ) {
            char *e = strchr(s + 1 + kl, '&');
            if (e) memmove(s + 1, e + 1, strlen(e + 1) + 1); else s[*s == '?' ? 1 : 0] = 0;
        }
    }
    size_t l = strlen(url);
    snprintf(url + l, n - l, "%s%s=%s", !strchr(url, '?') ? "?" : (url[l - 1] == '?' || url[l - 1] == '&') ? "" : "&", key, val);
}
static void ping(const char *base_url, const char *extra) {
    if (!base_url[0]) return;
    char err[200] = "", auth[2200], url[3000], kv[400];
    if (!bearer(auth, sizeof(auth), false, err, sizeof(err))) return;
    snprintf(url, sizeof(url), "%s", base_url);
    set_q(url, sizeof(url), "ver", "2"); set_q(url, sizeof(url), "cpn", w_cpn);
    if (w_samsung) {   /* what the TV app itself adds to every report */
        set_q(url, sizeof(url), "c", "TVHTML5"); set_q(url, sizeof(url), "cver", "5.20260707"); set_q(url, sizeof(url), "cplayer", "UNIPLAYER");
        set_q(url, sizeof(url), "cbrand", "Samsung"); set_q(url, sizeof(url), "cmodel", "UKS9800"); set_q(url, sizeof(url), "cos", "Tizen"); set_q(url, sizeof(url), "cosver", "2.4.0");
        set_q(url, sizeof(url), "cplatform", "TV"); set_q(url, sizeof(url), "hl", S.lang ? "en_US" : "vi_VN"); set_q(url, sizeof(url), "cr", "VN");
        set_q(url, sizeof(url), "volume", "100"); set_q(url, sizeof(url), "muted", "0"); set_q(url, sizeof(url), "fmt", "136"); set_q(url, sizeof(url), "afmt", "140");
        char rt[24]; snprintf(rt, sizeof(rt), "%.3f", (SDL_GetTicks() - w_t0) / 1000.0); set_q(url, sizeof(url), "rt", rt); set_q(url, sizeof(url), "lact", "1500");
    } else set_q(url, sizeof(url), "el", "detailpage");   /* without it the view is filed as a Short */
    snprintf(kv, sizeof(kv), "%s", extra);         /* "&a=1&b=2": each one replaces what the URL had */
    for (char *t = strtok(kv, "&"); t; t = strtok(NULL, "&")) { char *eq = strchr(t, '='); if (!eq) continue; *eq = 0; set_q(url, sizeof(url), t, eq + 1); }
    char h[2600]; snprintf(h, sizeof(h), "%sOrigin: https://www.youtube.com\r\nReferer: https://www.youtube.com/tv\r\n", auth);
    char *r = Innertube_http(url, NULL, h, w_samsung ? SAMSUNG_UA : TV_UA, err, sizeof(err));
    if (!r && err[0] && !strstr(err, "204") && !strstr(err, "empty reply")) Log_msg("account ping: %s", err);
    free(r);
}
static int w_pings;

/* ---------- the account's subscribed channels (so channels subscribed on a phone show up here) ---------- */
static VideoItem ch_found[400]; static int ch_n; static bool ch_ready;
/* channel pictures: "channel id -> picture address", learnt from the account's channel list, kept in data/avatars.txt */
#define AV_MAX 1500
static struct { char cid[28]; char url[230]; char name[72]; } av[AV_MAX]; static int av_n, av_next; static bool av_loaded, av_dirty;
static pthread_mutex_t av_lock = PTHREAD_MUTEX_INITIALIZER;
static void av_load_locked(void) {
    if (av_loaded) return;
    av_loaded = true;
    char path[1200]; snprintf(path, sizeof(path), "%s/avatars.txt", paths.data);
    FILE *f = fopen(path, "r"); if (!f) return;
    char line[400];
    while (av_n < AV_MAX && fgets(line, sizeof(line), f)) {
        char c[28], u[230]; int used = 0;
        if (sscanf(line, "%27s %229s%n", c, u, &used) == 2 && strlen(c) == 24) {
            snprintf(av[av_n].cid, 28, "%s", c); snprintf(av[av_n].url, 230, "%s", u);
            char *nm = line + used; while (*nm == ' ' || *nm == '\t') nm++;
            nm[strcspn(nm, "\r\n")] = 0; snprintf(av[av_n].name, sizeof(av[0].name), "%s", nm);
            av_n++;
        }
    }
    fclose(f);
}
static void av_set(const char *cid, const char *url, const char *name) {   /* caller holds av_lock */
    if (!url || strlen(url) > 200 || strlen(url) < 12) return;
    char full[230]; snprintf(full, sizeof(full), "%s%s", !strncmp(url, "//", 2) ? "https:" : "", url);
    char *eq = strrchr(full, '=');           /* "=s88-c-k-…": ask for 160 px, as JPEG */
    if (eq && eq[1] == 's') snprintf(eq, sizeof(full) - (eq - full), "=s160-c-k-rj");
    for (int i = 0; i < av_n; i++) if (!strcmp(av[i].cid, cid)) {
        if (strcmp(av[i].url, full)) { snprintf(av[i].url, 230, "%s", full); av_dirty = true; }
        if (name && *name && strcmp(av[i].name, name)) { snprintf(av[i].name, sizeof(av[0].name), "%s", name); av_dirty = true; }
        return;
    }
    int k = av_n < AV_MAX ? av_n++ : (av_next = (av_next + 1) % AV_MAX);   /* full: the oldest ones make room */
    snprintf(av[k].cid, 28, "%s", cid); snprintf(av[k].url, 230, "%s", full); snprintf(av[k].name, sizeof(av[0].name), "%s", name ? name : ""); av_dirty = true;
}
static void av_save_locked(void) {
    char path[1200], tmp[1210]; snprintf(path, sizeof(path), "%s/avatars.txt", paths.data); snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    FILE *f = fopen(tmp, "w"); if (!f) return;
    for (int i = 0; i < av_n; i++) fprintf(f, "%s %s %s\n", av[i].cid, av[i].url, av[i].name);
    if (!fclose(f)) rename(tmp, path);
    av_dirty = false;
}
void Account_avatarSet(const char *cid, const char *url, const char *name) {
    if (!cid || strlen(cid) != 24 || !url) return;
    pthread_mutex_lock(&av_lock); av_load_locked(); av_set(cid, url, name); pthread_mutex_unlock(&av_lock);
}
int Account_avatarCount(void) { pthread_mutex_lock(&av_lock); av_load_locked(); int n = av_n; pthread_mutex_unlock(&av_lock); return n; }
void Account_avatarFlush(void) {
    pthread_mutex_lock(&av_lock); if (av_dirty) av_save_locked(); pthread_mutex_unlock(&av_lock);
}
/* key = "@" + the last 14 letters of a channel id (thumbnail names are short). The picture address, if known. */
bool Account_avatarUrl(const char *key, char *out, size_t n) {
    bool ok = false;
    if (!key || key[0] != '@' || strlen(key) != 15) return false;
    pthread_mutex_lock(&av_lock); av_load_locked();
    for (int i = 0; i < av_n && !ok; i++) if (!strcmp(av[i].cid + 10, key + 1)) { snprintf(out, n, "%s", av[i].url); ok = true; }
    pthread_mutex_unlock(&av_lock);
    return ok;
}
/* true + the thumbnail key when this channel's picture is known: by its id, else by its exact name
 * (lists that come without channel ids still get the picture) */
bool Account_avatarKey(const char *cid, const char *name, char *key16) {
    bool ok = false;
    pthread_mutex_lock(&av_lock); av_load_locked();
    if (cid && strlen(cid) == 24) for (int i = 0; i < av_n && !ok; i++) if (!strcmp(av[i].cid, cid)) { snprintf(key16, 16, "@%s", av[i].cid + 10); ok = true; }
    if (!ok && name && *name) for (int i = 0; i < av_n && !ok; i++) if (av[i].name[0] && !strcmp(av[i].name, name)) { snprintf(key16, 16, "@%s", av[i].cid + 10); ok = true; }
    pthread_mutex_unlock(&av_lock);
    return ok;
}
static const char *find_pic(JSON_Value *v, int depth) {   /* a yt3 / ggpht picture address anywhere inside */
    if (depth > 12) return NULL;
    const char *s = json_value_get_string(v);
    if (s) return (strstr(s, "yt3.") || strstr(s, "ggpht.com") || strstr(s, "googleusercontent.com")) ? s : NULL;
    JSON_Object *o = json_value_get_object(v);
    if (o) { for (size_t i = 0; i < json_object_get_count(o); i++) { const char *r = find_pic(json_object_get_value_at(o, i), depth + 1); if (r) return r; } return NULL; }
    JSON_Array *a = json_value_get_array(v);
    for (size_t i = 0; a && i < json_array_get_count(a); i++) { const char *r = find_pic(json_array_get_value(a, i), depth + 1); if (r) return r; }
    return NULL;
}
static const char *ch_title(JSON_Object *o) {
    const char *t = json_object_dotget_string(o, "title.simpleText");
    if (!t) t = json_object_dotget_string(json_array_get_object(json_object_dotget_array(o, "title.runs"), 0), "text");
    if (!t) t = find_str(json_object_get_wrapping_value(o), "simpleText", 0);
    if (!t) t = find_str(json_object_get_wrapping_value(o), "text", 0);
    return t;
}
static int ch_pics;
static void ch_walk(JSON_Value *v, VideoItem *out, int *n, int max, int depth) {
    if (depth > 40) return;
    JSON_Object *o = json_value_get_object(v);
    if (o) {
        static const char *eps[] = {"navigationEndpoint", "onSelectCommand", "endpoint", "command", "onClickCommand"};
        for (int e = 0; e < 5; e++) {
            JSON_Object *ep = json_object_get_object(o, eps[e]);
            const char *id = ep ? json_object_dotget_string(ep, "browseEndpoint.browseId") : NULL;
            if (!id || strncmp(id, "UC", 2) || strlen(id) != 24) continue;
            if (find_str(v, "videoId", 0)) break;            /* a video tile's "go to channel": its text is the video's */
            const char *t = ch_title(o);
            bool dup = false; for (int i = 0; i < *n; i++) if (!strcmp(out[i].channel_id, id)) dup = true;
            if (t && *t && !dup && *n < max) {
                memset(&out[*n], 0, sizeof(out[0]));
                snprintf(out[*n].channel_id, sizeof(out[0].channel_id), "%s", id);
                snprintf(out[*n].channel, sizeof(out[0].channel), "%s", t); snprintf(out[*n].title, sizeof(out[0].title), "%s", t);
                (*n)++;
                const char *pic = find_pic(v, 0);
                if (pic) { pthread_mutex_lock(&av_lock); av_load_locked(); av_set(id, pic, t); ch_pics++; pthread_mutex_unlock(&av_lock); }
            }
            return;
        }
        for (size_t i = 0; i < json_object_get_count(o); i++) ch_walk(json_object_get_value_at(o, i), out, n, max, depth + 1);
        return;
    }
    JSON_Array *a = json_value_get_array(v);
    for (size_t i = 0; a && i < json_array_get_count(a); i++) ch_walk(json_array_get_value(a, i), out, n, max, depth + 1);
}
static void ch_ids(JSON_Value *v, char *out, size_t n, int *k, int depth) {   /* which pages an answer links to (for the log) */
    if (depth > 40 || *k >= 8) return;
    JSON_Object *o = json_value_get_object(v);
    if (o) {
        const char *b = json_object_get_string(o, "browseId");
        if (b && !strstr(out, b) && strlen(out) + strlen(b) + 2 < n) { strcat(out, b); strcat(out, " "); (*k)++; }
        for (size_t i = 0; i < json_object_get_count(o); i++) ch_ids(json_object_get_value_at(o, i), out, n, k, depth + 1);
        return;
    }
    JSON_Array *a = json_value_get_array(v);
    for (size_t i = 0; a && i < json_array_get_count(a); i++) ch_ids(json_array_get_value(a, i), out, n, k, depth + 1);
}
static void ch_fetch(void) {
    static VideoItem tmp[400]; int n = 0; char err[200] = "";
    static const char *pages[] = {"FEchannels", "FEsubscriptions"};
    for (int p = 0; p < 2 && !n; p++) {
        char extra[80]; snprintf(extra, sizeof(extra), "\"browseId\":\"%s\"", pages[p]);
        JSON_Value *v = tv_call("browse", extra, err, sizeof(err));
        ch_pics = 0;
        if (v) ch_walk(v, tmp, &n, 400, 0);
        Log_msg("account channels %s: %s, %d channels, %d pictures%s%s", pages[p], v ? "ok" : err, n, ch_pics, n ? ", first: " : "", n ? tmp[0].channel : "");
        if (ch_pics) { pthread_mutex_lock(&av_lock); av_save_locked(); pthread_mutex_unlock(&av_lock); }
        if (v && !n) { char ids[400] = ""; int k = 0; ch_ids(v, ids, sizeof(ids), &k, 0); Log_msg("account channels %s links to: %s", pages[p], ids[0] ? ids : "(nothing)"); }
        json_value_free(v);
    }
    pthread_mutex_lock(&wlock);
    memcpy(ch_found, tmp, sizeof(tmp[0]) * n); ch_n = n; ch_ready = n > 0;
    pthread_mutex_unlock(&wlock);
}
static void do_job(int kind, const char *id, double pos, double dur, bool paused, bool on) {
    char err[300] = "", extra[480];
    if (kind == WJ_START) {
        /* the TV app's own player answer for this account carries the history URLs; when YouTube
         * answers it without them (it may ask the TV player for more checks), the ones from the
         * normal stream answer are used: the pings count for whoever sends the account token */
        const char *live = tv_live_version();
        const char *clients[5][3] = {{"TVHTML5_SAMSUNG", "5.20260707", TV_NAME_ID}, {"TVHTML5_SAMSUNG_ACC", "5.20260707", TV_NAME_ID}, {"TVHTML5", live, TV_NAME_ID}, {"TVHTML5", TV_VERSION, TV_NAME_ID}, {"TVHTML5_SIMPLY_EMBEDDED_PLAYER", "2.0", "85"}};
        static int good = -1;   /* the client that answered last time is asked first */
        snprintf(w_vid, sizeof(w_vid), "%s", id); new_cpn(); w_last_et = 0; w_play[0] = w_time[0] = 0; w_samsung = false;
        int sts0 = sts_get();
        Log_msg("account history %s: signatureTimestamp %d", id, sts0);
        for (int k = -1; k < 5 && !w_play[0]; k++) {
            int c = k < 0 ? good : k;
            if (c < 0 || (k >= 0 && c == good)) continue;
            if (c == 3 && !strcmp(live, TV_VERSION)) continue;
            char stsp[48] = ""; if (sts0 > 0) snprintf(stsp, sizeof(stsp), ",\"signatureTimestamp\":%d", sts0);
            snprintf(extra, sizeof(extra), "\"videoId\":\"%s\",\"contentCheckOk\":true,\"racyCheckOk\":true,\"playbackContext\":{\"contentPlaybackContext\":{\"html5Preference\":\"HTML5_PREF_WANTS\"%s}}", id, stsp);
            err[0] = 0;
            JSON_Value *v = c == 3 ? tv_call("player", extra, err, sizeof(err)) : tv_call_as(clients[c][0], clients[c][1], clients[c][2], "player", extra, err, sizeof(err));
            JSON_Object *o = json_value_get_object(v);
            const char *p = json_object_dotget_string(o, "playbackTracking.videostatsPlaybackUrl.baseUrl");
            const char *t = json_object_dotget_string(o, "playbackTracking.videostatsWatchtimeUrl.baseUrl");
            if (p) { snprintf(w_play, sizeof(w_play), "%s", p); good = c; w_samsung = c <= 1; w_t0 = SDL_GetTicks(); Log_msg("account history %s: tracking from %s %s", id, clients[c][0], clients[c][1]); }
            if (t) snprintf(w_time, sizeof(w_time), "%s", t);
            if (!p) Log_msg("account history %s: %s %s player has no tracking (%s %s)", id, clients[c][0], clients[c][1],
                            v ? json_object_dotget_string(o, "playabilityStatus.status") ? json_object_dotget_string(o, "playabilityStatus.status") : "?" : "error",
                            v ? (json_object_dotget_string(o, "playabilityStatus.reason") ? json_object_dotget_string(o, "playabilityStatus.reason") : "") : err);
            json_value_free(v);
        }
        if (!w_play[0] && fb_play[0]) { snprintf(w_play, sizeof(w_play), "%s", fb_play); snprintf(w_time, sizeof(w_time), "%s", fb_time); Log_msg("account history %s: using the stream's tracking URLs", id); }
        if (!w_play[0]) { Log_msg("account history %s: no tracking URLs at all", id); return; }
        snprintf(extra, sizeof(extra), "&cmt=%.3f&len=%.3f", pos, dur);
        ping(w_play, extra);
        w_pings = 0;
        {   /* which parameters YouTube put in the tracking address (names only) */
            char keys[300] = ""; size_t kn = 0;
            for (const char *q = strchr(w_play, '?'); q && *q && kn + 40 < sizeof(keys); ) { const char *k = q + 1, *e = strchr(k, '='); if (!e) break; kn += snprintf(keys + kn, sizeof(keys) - kn, "%.*s ", (int)(e - k > 20 ? 20 : e - k), k); q = strchr(e, '&'); }
            Log_msg("account history: %s marked watched (%.40s… keys: %s)", id, w_play, keys);
        }
    } else if (kind == WJ_TIME) {
        if (strcmp(id, w_vid) || !w_time[0]) return;
        double st = w_last_et < pos ? w_last_et : pos;
        snprintf(extra, sizeof(extra), "&cmt=%.3f&st=%.3f&et=%.3f&len=%.3f&state=%s&final=0", pos, st, pos, dur, paused ? "paused" : "playing");
        ping(w_time, extra);
        w_last_et = pos;
    } else if (kind == WJ_LIKE) {
        snprintf(extra, sizeof(extra), "\"target\":{\"videoId\":\"%s\"}", id);
        JSON_Value *v = tv_call(on ? "like/like" : "like/removelike", extra, err, sizeof(err));
        Log_msg("account like %s %s: %s", id, on ? "on" : "off", v ? "ok" : err);
        json_value_free(v);
    } else if (kind == WJ_LSTATE) {
        snprintf(extra, sizeof(extra), "\"videoId\":\"%s\"", id);
        JSON_Value *v = tv_call("next", extra, err, sizeof(err));
        const char *st = v ? find_str(v, "likeStatus", 0) : NULL;
        pthread_mutex_lock(&wlock);
        int sub = v ? find_bool(v, "subscribed", 0) : -1;
        if (!strcmp(ls_id, id)) { ls_val = st ? (!strcmp(st, "LIKE") ? 1 : 0) : -2; ls_sub = sub; }
        pthread_mutex_unlock(&wlock);
        Log_msg("account like state %s: %s, channel %s", id, st ? st : (v ? "not in answer" : err), sub == 1 ? "subscribed" : sub == 0 ? "not subscribed" : "unknown");
        json_value_free(v);
    } else if (kind == WJ_CHANS) {
        ch_fetch();
    } else if (kind == WJ_SUB) {
        snprintf(extra, sizeof(extra), "\"channelIds\":[\"%s\"],\"params\":\"EgIIAhgA\"", id);
        JSON_Value *v = tv_call(on ? "subscription/subscribe" : "subscription/unsubscribe", extra, err, sizeof(err));
        Log_msg("account %ssubscribe %s: %s", on ? "" : "un", id, v ? "ok" : err);
        json_value_free(v);
    }
}
static void *w_thread(void *u) {
    (void)u;
    pthread_mutex_lock(&wlock);
    for (;;) {
        while (!wq_n) pthread_cond_wait(&wwake, &wlock);
        typeof(wq[0]) j = wq[0];
        memmove(&wq[0], &wq[1], (wq_n - 1) * sizeof(wq[0])); wq_n--;
        pthread_mutex_unlock(&wlock);
        do_job(j.kind, j.id, j.pos, j.dur, j.paused, j.on);
        pthread_mutex_lock(&wlock);
    }
    return NULL;
}
static void queue_job(int kind, const char *id, double pos, double dur, bool paused, bool on) {
    if (!Account_signedIn() || !id || !*id) return;
    pthread_mutex_lock(&wlock);
    if (!w_running) { pthread_t t; w_running = pthread_create(&t, NULL, w_thread, NULL) == 0; if (w_running) pthread_detach(t); }
    /* a newer watch-time ping replaces one still waiting */
    if (kind == WJ_TIME) for (int i = 0; i < wq_n; i++) if (wq[i].kind == WJ_TIME) { memmove(&wq[i], &wq[i + 1], (wq_n - i - 1) * sizeof(wq[0])); wq_n--; break; }
    if (wq_n < 16) { wq[wq_n].kind = kind; snprintf(wq[wq_n].id, sizeof(wq[0].id), "%s", id); wq[wq_n].pos = pos; wq[wq_n].dur = dur; wq[wq_n].paused = paused; wq[wq_n].on = on; wq_n++; }
    pthread_cond_signal(&wwake);
    pthread_mutex_unlock(&wlock);
}
void Account_watchStart(const char *id, double pos, double dur, const char *track_pb, const char *track_wt) {
    pthread_mutex_lock(&wlock);
    snprintf(fb_play, sizeof(fb_play), "%s", track_pb ? track_pb : ""); snprintf(fb_time, sizeof(fb_time), "%s", track_wt ? track_wt : "");
    pthread_mutex_unlock(&wlock);
    queue_job(WJ_START, id, pos, dur, false, true);
}
void Account_watchTime(const char *id, double pos, double dur, bool paused) { queue_job(WJ_TIME, id, pos, dur, paused, true); }
void Account_like(const char *id, bool on) { queue_job(WJ_LIKE, id, 0, 0, false, on); }
void Account_channelsFetch(void) { queue_job(WJ_CHANS, "channels", 0, 0, false, false); }
int Account_channelsTake(VideoItem *out, int max) {
    pthread_mutex_lock(&wlock);
    int n = 0;
    if (ch_ready) { n = ch_n < max ? ch_n : max; memcpy(out, ch_found, sizeof(out[0]) * n); ch_ready = false; }
    pthread_mutex_unlock(&wlock);
    return n;
}
void Account_subscribe(const char *channel_id, bool on) { queue_job(WJ_SUB, channel_id, 0, 0, false, on); }
void Account_likeStateFetch(const char *id) {
    pthread_mutex_lock(&wlock); snprintf(ls_id, sizeof(ls_id), "%s", id ? id : ""); ls_val = -1; ls_sub = -1; pthread_mutex_unlock(&wlock);
    queue_job(WJ_LSTATE, id, 0, 0, false, true);
}
int Account_likeState(const char *id) {   /* -1 not known yet, -2 unknown, 0 not liked, 1 liked */
    pthread_mutex_lock(&wlock); int v = id && !strcmp(id, ls_id) ? ls_val : -2; pthread_mutex_unlock(&wlock);
    return v;
}
int Account_subStateTake(const char *id) {   /* 1 / 0 once when known for this video, else -1 */
    pthread_mutex_lock(&wlock); int v = id && !strcmp(id, ls_id) ? ls_sub : -1; if (v >= 0) ls_sub = -1; pthread_mutex_unlock(&wlock);
    return v;
}
void Account_likeSet(const char *id, bool on) { pthread_mutex_lock(&wlock); if (id && !strcmp(id, ls_id)) ls_val = on; pthread_mutex_unlock(&wlock); }
