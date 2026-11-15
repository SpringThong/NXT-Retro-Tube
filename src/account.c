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
static JSON_Value *post_json(const char *url, const char *body, const char *extra_headers, char *err, size_t en) {
    char h[3000];
    snprintf(h, sizeof(h), "Content-Type: application/json\r\nOrigin: https://www.youtube.com\r\nReferer: https://www.youtube.com/tv\r\n%s", extra_headers ? extra_headers : "");
    char *r = Innertube_http(url, body, h, TV_UA, err, en);
    if (!r) return NULL;
    JSON_Value *v = json_parse_string(r);
    free(r);
    if (!v) snprintf(err, en, "bad JSON");
    return v;
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
        char auth[2200], h[2400];
        if (!bearer(auth, sizeof(auth), attempt > 0, err, en)) break;
        snprintf(h, sizeof(h), "%sX-YouTube-Client-Name: " TV_NAME_ID "\r\nX-YouTube-Client-Version: " TV_VERSION "\r\n", auth);
        v = post_json(url, body, h, err, en);
        if (!v && !strstr(err, "401") && !strstr(err, "Unauthorized")) break;
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
            const char *t = json_object_dotget_string(json_array_get_object(cs, i), "nextContinuationData.continuation");
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
typedef struct { char id[64]; VideoList items; char next[2048]; bool done; Uint32 used; } FeedCache;
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
    for (int pages = 0; c->items.count < start + count && !c->done && pages < 5; pages++) {
        char extra[2300];
        if (c->next[0]) snprintf(extra, sizeof(extra), "\"continuation\":\"%s\"", c->next);
        else snprintf(extra, sizeof(extra), "\"browseId\":\"%s\"", browse);
        JSON_Value *v = tv_call("browse", extra, err, en);
        if (!v) { ok = c->items.count > start; break; }
        int before = c->items.count, prio = 0;
        char tok[2048] = "";
        walk(v, NULL, &c->items, tok, sizeof(tok), &prio, 0);
        json_value_free(v);
        if (!tok[0] || !strcmp(tok, c->next) || c->items.count == before) { if (!tok[0] || c->items.count == before) c->done = true; }
        snprintf(c->next, sizeof(c->next), "%s", tok);
    }
    for (int i = start; i < c->items.count && i < start + count; i++) VideoList_add(out, &c->items.items[i]);
    if (ok && !out->count && !c->done) { snprintf(err, en, "empty"); ok = false; }
    if (ok && !c->items.count) snprintf(err, en, "no videos");
    pthread_mutex_unlock(&flock);
    return ok && (out->count > 0 || c->done);
}
