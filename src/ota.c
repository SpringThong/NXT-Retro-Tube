/* OTA updates from the author's GitHub releases.
 *
 * The newest release carries ota.json (what is current for every ROM / device) and
 * ota.json.sig, an ECDSA P-256 signature made with the author's private key. The app
 * only trusts a manifest whose signature matches the public key below, and only installs
 * a file whose size and SHA-256 match the manifest, so a wrong clock (no RTC: TLS dates
 * fail) or a hostile network cannot slip anything in.
 *
 * Components, each updated only when its version changed:
 *   app-<channel>  the program, its resources and the yt-dlp plugin (a few MB)
 *   ytdlp          the yt-dlp runtime (YouTube changes: the part that needs updates most)
 *   deno           the JavaScript runtime used for "made for kids" videos
 * The channel is the ROM this build is for plus the device it actually runs on.
 *
 * Install: download -> verify -> unpack next to the app (.ota/stage) -> move the old
 * files to .ota/backup and the new ones in place (renames on the same card) -> restart.
 * launch.sh counts starts while .ota/pending exists and puts the backup back if the new
 * version fails to come up twice; the app clears the marker once it has run for 20 s.
 * SPDX-License-Identifier: GPL-3.0-only */
#include "app.h"
#include "parson.h"
#include <dirent.h>
#include <stdarg.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <zlib.h>
#include <libavformat/avio.h>
#include <libavutil/dict.h>
#include <libavutil/error.h>
#include <mbedtls/sha256.h>
#include <mbedtls/pk.h>
#include <mbedtls/md.h>
#include <psa/crypto.h>

static const char OTA_PUBKEY[] =
    "-----BEGIN PUBLIC KEY-----\n"
    "MFkwEwYHKoZIzj0CAQYIKoZIzj0DAQcDQgAE6HWYrPaK1dXVnVcjJSWNVF0tzttp\n"
    "ILx5BDdNR2t4a6wW1EiYPPTPxEVDotAzfRemqKvjITMGdB1YkAoL7k7JMg==\n"
    "-----END PUBLIC KEY-----\n";
#define OTA_REPO_DEFAULT "SpringThong/NXT-Retro-Tube"

typedef struct { char name[48], version[40], url[512], sha[65]; long size; bool need; } Comp;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static volatile int state = OTA_IDLE;
static volatile int progress;                /* 0..100 while downloading */
static char msg[200], latest[40], notes[600];
static Comp comps[3]; static int ncomps;
static long total_bytes;
static bool busy, pending_cleared;
static Uint32 started_at, next_auto;

/* ---------- where am I ---------- */
static bool read_line(const char *p, char *out, size_t n) {
    FILE *f = fopen(p, "r"); if (!f) return false;
    bool ok = fgets(out, (int)n, f) != NULL; fclose(f);
    if (ok) { size_t l = strlen(out); while (l && (out[l - 1] == '\n' || out[l - 1] == '\r' || out[l - 1] == ' ')) out[--l] = 0; }
    return ok && *out;
}
const char *Ota_device(void) {
    static char dev[64];
    if (dev[0]) return dev;
    /* the device tree names the SoC: Allwinner A523 (sun55i) = Smart Pro S, A133 (sun50i) = Brick / Smart Pro */
    char compat[512] = ""; FILE *f = fopen("/proc/device-tree/compatible", "rb");
    if (f) { size_t n = fread(compat, 1, sizeof(compat) - 1, f); fclose(f); for (size_t i = 0; i < n; i++) if (!compat[i]) compat[i] = ' '; compat[n] = 0; }
    const char *d = getenv("DEVICE");
    if (strstr(compat, "sun55i")) snprintf(dev, sizeof(dev), "TrimUI Smart Pro S");
    else if (strstr(compat, "rk3326") || strstr(compat, "rockchip")) snprintf(dev, sizeof(dev), "R36S / RK3326");
    else if (d && !strcmp(d, "brick")) snprintf(dev, sizeof(dev), "TrimUI Brick");
    else if (d && !strcmp(d, "brickpro")) snprintf(dev, sizeof(dev), "TrimUI Brick Pro");
    else if (d && !strcmp(d, "smartpro")) snprintf(dev, sizeof(dev), "TrimUI Smart Pro");
    else if (strstr(compat, "sun50i")) snprintf(dev, sizeof(dev), "TrimUI (A133)");
    else { char m[64]; if (read_line("/proc/device-tree/model", m, sizeof(m))) snprintf(dev, sizeof(dev), "%s", m); else snprintf(dev, sizeof(dev), "?"); }
    return dev;
}
const char *Ota_channel(void) {
    static char ch[32];
    if (ch[0]) return ch;
#if defined(NXT_TUBE_ROM_MUOS)
    snprintf(ch, sizeof(ch), "muos");
#elif defined(NXT_TUBE_ROM_KNULLI)
    snprintf(ch, sizeof(ch), "knulli");
#elif defined(NXT_TUBE_ROM_DARKOS)
    snprintf(ch, sizeof(ch), "darkos");
#elif defined(NXT_TUBE_ROM_STOCK)
    /* the right build for the hardware, even if the other one was copied by mistake */
    snprintf(ch, sizeof(ch), "%s", strstr(Ota_device(), "Smart Pro S") ? "stock-sps" : "stock");
#elif defined(NXT_TUBE_ROM_NEXTUI)
    snprintf(ch, sizeof(ch), "nextui");
#else
    snprintf(ch, sizeof(ch), "nxredux");
#endif
    return ch;
}
static void comp_version(const char *name, char *out, size_t n) {
    char p[1300];
    if (!strncmp(name, "app-", 4)) {
        /* an app built for another channel (installed on the wrong device) counts as outdated */
        snprintf(out, n, "%s", strcmp(name + 4, Ota_channel()) ? "0" : APP_VERSION);
#if defined(NXT_TUBE_ROM_STOCK)
        bool sps_build =
#ifdef NXT_TUBE_DEV_TG5050
            true;
#else
            false;
#endif
        if (sps_build != !strcmp(Ota_channel(), "stock-sps")) snprintf(out, n, "0");
#endif
        return;
    }
    if (!strcmp(name, "ytdlp")) snprintf(p, sizeof(p), "%s/yt-dlp-runtime/.nxt-version", paths.bin);
    else snprintf(p, sizeof(p), "%s/.deno-version", paths.bin);
    if (!read_line(p, out, n)) snprintf(out, n, "0");
}
/* "2.0.1" > "2.0" ; "2026.10.21" > "2026.9.3" */
static int vercmp(const char *a, const char *b) {
    while (*a || *b) {
        long x = strtol(a, (char **)&a, 10), y = strtol(b, (char **)&b, 10);
        if (x != y) return x < y ? -1 : 1;
        while (*a && (*a < '0' || *a > '9')) a++;
        while (*b && (*b < '0' || *b > '9')) b++;
    }
    return 0;
}

/* ---------- downloads ---------- */
static void manifest_base(char *out, size_t n) {
    const char *b = getenv("NXT_TUBE_OTA_BASE");     /* QA: a local server */
    if (b && *b) { snprintf(out, n, "%s", b); return; }
    char repo[200] = OTA_REPO_DEFAULT, p[1300];
    snprintf(p, sizeof(p), "%s/res/ota-repo.txt", paths.app);
    char r[200]; if (read_line(p, r, sizeof(r)) && strchr(r, '/')) snprintf(repo, sizeof(repo), "%s", r);
    snprintf(out, n, "https://github.com/%s/releases/latest/download", repo);
}
static AVIOContext *open_url(const char *url, bool verify) {
    char ca[1200]; snprintf(ca, sizeof(ca), "%s/res/cacert.pem", paths.app);
    AVDictionary *o = NULL;
    av_dict_set(&o, "ca_file", ca, 0);
    av_dict_set(&o, "tls_verify", verify ? "1" : "0", 0);   /* either way the signature / SHA-256 decide */
    av_dict_set(&o, "timeout", "20000000", 0); av_dict_set(&o, "rw_timeout", "20000000", 0);
    av_dict_set(&o, "user_agent", "NXT-Retro-Tube/" APP_VERSION, 0);
    AVIOContext *io = NULL;
    int r = avio_open2(&io, url, AVIO_FLAG_READ, NULL, &o);
    av_dict_free(&o);
    if (r < 0) { Log_msg("ota: %s: %s", url, av_err2str(r)); return NULL; }
    return io;
}
static char *fetch_small(const char *url, size_t max, size_t *len) {
    for (int attempt = 0; attempt < 2; attempt++) {
        AVIOContext *io = open_url(url, attempt == 0);
        if (!io) continue;
        char *buf = malloc(max + 1); size_t got = 0;
        while (buf && got < max) { int n = avio_read(io, (unsigned char *)buf + got, (int)(max - got)); if (n <= 0) break; got += n; }
        avio_closep(&io);
        if (buf && got > 0) { buf[got] = 0; *len = got; return buf; }
        free(buf);
    }
    return NULL;
}
static bool sig_ok(const char *data, size_t len, const unsigned char *sig, size_t siglen) {
#if defined(MBEDTLS_PSA_CRYPTO_C)
    psa_crypto_init();
#endif
    unsigned char hash[32];
    if (mbedtls_sha256((const unsigned char *)data, len, hash, 0)) return false;
    mbedtls_pk_context pk; mbedtls_pk_init(&pk);
    bool ok = !mbedtls_pk_parse_public_key(&pk, (const unsigned char *)OTA_PUBKEY, sizeof(OTA_PUBKEY)) &&
              !mbedtls_pk_verify(&pk, MBEDTLS_MD_SHA256, hash, sizeof(hash), sig, siglen);
    mbedtls_pk_free(&pk);
    return ok;
}
static void set_state(int s, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void set_state(int s, const char *fmt, ...) {
    pthread_mutex_lock(&lock);
    state = s;
    va_list ap; va_start(ap, fmt); vsnprintf(msg, sizeof(msg), fmt, ap); va_end(ap);
    pthread_mutex_unlock(&lock);
    Log_msg("ota: %s", msg);
}

/* ---------- check ---------- */
static void state_file(char *p, size_t n) { snprintf(p, n, "%s/ota-state.txt", paths.data); }
static void *check_thread(void *arg) {
    bool manual = arg != NULL;
    char base[600], url[700];
    manifest_base(base, sizeof(base));
    size_t mlen = 0, slen = 0;
    snprintf(url, sizeof(url), "%s/ota.json", base);
    char *man = fetch_small(url, 256 * 1024, &mlen);
    snprintf(url, sizeof(url), "%s/ota.json.sig", base);
    char *sig = man ? fetch_small(url, 512, &slen) : NULL;
    {   char p[1300]; state_file(p, sizeof(p)); FILE *f = fopen(p, "w"); if (f) { fprintf(f, "%lld\n", (long long)time(NULL)); fclose(f); } }
    if (!man || !sig) { set_state(OTA_ERROR, "%s", T("Không kiểm tra được (cần Wi-Fi)")); goto out; }
    if (!sig_ok(man, mlen, (unsigned char *)sig, slen)) { set_state(OTA_ERROR, "%s", T("Bản cập nhật không có chữ ký hợp lệ · bỏ qua")); goto out; }
    JSON_Value *root = json_parse_string(man);
    JSON_Object *o = json_value_get_object(root), *cs = json_object_get_object(o, "components");
    const char *ver = json_object_get_string(o, "version");
    if (!o || !cs || !ver) { json_value_free(root); set_state(OTA_ERROR, "%s", T("Tệp cập nhật lỗi")); goto out; }
    pthread_mutex_lock(&lock);
    snprintf(latest, sizeof(latest), "%s", ver);
    const char *nt = json_object_get_string(o, S.lang == 1 ? "notes_en" : "notes_vi");
    snprintf(notes, sizeof(notes), "%s", nt ? nt : "");
    ncomps = 0; total_bytes = 0;
    char app_name[48]; snprintf(app_name, sizeof(app_name), "app-%s", Ota_channel());
    const char *names[3] = {app_name, "ytdlp", "deno"};
    for (int i = 0; i < 3; i++) {
        JSON_Object *c = json_object_get_object(cs, names[i]);
        if (!c) continue;
        Comp *k = &comps[ncomps];
        memset(k, 0, sizeof(*k));
        snprintf(k->name, sizeof(k->name), "%s", names[i]);
        snprintf(k->version, sizeof(k->version), "%s", json_object_get_string(c, "version") ? json_object_get_string(c, "version") : "");
        snprintf(k->url, sizeof(k->url), "%s", json_object_get_string(c, "url") ? json_object_get_string(c, "url") : "");
        snprintf(k->sha, sizeof(k->sha), "%s", json_object_get_string(c, "sha256") ? json_object_get_string(c, "sha256") : "");
        k->size = (long)json_object_get_number(c, "size");
        char have[40]; comp_version(names[i], have, sizeof(have));
        /* the app only moves forward; runtimes follow whatever the release ships */
        k->need = k->url[0] && strlen(k->sha) == 64 && k->size > 0 &&
                  (i == 0 ? vercmp(k->version, have) > 0 : strcmp(k->version, have) != 0);
        if (k->need) total_bytes += k->size;
        ncomps++;
    }
    pthread_mutex_unlock(&lock);
    json_value_free(root);
    if (total_bytes > 0) set_state(OTA_AVAILABLE, T("Có bản %s · %.1f MB"), latest, total_bytes / 1048576.0);
    else set_state(OTA_UPTODATE, T("Đang dùng bản mới nhất (%s)"), APP_VERSION);
    (void)manual;
out:
    free(man); free(sig);
    pthread_mutex_lock(&lock); busy = false; pthread_mutex_unlock(&lock);
    return NULL;
}
bool Ota_check(bool manual) {
    pthread_mutex_lock(&lock);
    if (busy) { pthread_mutex_unlock(&lock); return false; }
    busy = true; state = OTA_CHECKING; snprintf(msg, sizeof(msg), "%s", T("Đang kiểm tra cập nhật…"));
    pthread_mutex_unlock(&lock);
    pthread_t t;
    if (pthread_create(&t, NULL, check_thread, manual ? (void *)1 : NULL)) { busy = false; return false; }
    pthread_detach(t);
    return true;
}

/* ---------- unzip (stored / deflate, enough for our own packages) ---------- */
static unsigned rd16(const unsigned char *p) { return p[0] | p[1] << 8; }
static unsigned long rd32(const unsigned char *p) { return p[0] | p[1] << 8 | (unsigned long)p[2] << 16 | (unsigned long)p[3] << 24; }
static bool mkdirs(const char *path) {
    char tmp[1400]; snprintf(tmp, sizeof(tmp), "%s", path);
    for (char *p = tmp + 1; *p; p++) if (*p == '/') { *p = 0; mkdir(tmp, 0755); *p = '/'; }
    return !mkdir(tmp, 0755) || errno == EEXIST;
}
static bool unzip_to(const char *zip, const char *dest) {
    FILE *f = fopen(zip, "rb"); if (!f) return false;
    bool ok = false;
    unsigned char *cd = NULL;
    fseek(f, 0, SEEK_END); long fsz = ftell(f);
    long tail = fsz < 65557 ? fsz : 65557;
    unsigned char *t = malloc(tail);
    if (!t) goto done;
    fseek(f, fsz - tail, SEEK_SET);
    if (fread(t, 1, tail, f) != (size_t)tail) { free(t); goto done; }
    long eocd = -1;
    for (long i = tail - 22; i >= 0; i--) if (rd32(t + i) == 0x06054b50) { eocd = i; break; }
    if (eocd < 0) { free(t); goto done; }
    unsigned entries = rd16(t + eocd + 10); unsigned long cdsize = rd32(t + eocd + 12), cdoff = rd32(t + eocd + 16);
    free(t);
    cd = malloc(cdsize); if (!cd) goto done;
    fseek(f, (long)cdoff, SEEK_SET);
    if (fread(cd, 1, cdsize, f) != cdsize) goto done;
    unsigned char *p = cd;
    for (unsigned e = 0; e < entries; e++) {
        if (p + 46 > cd + cdsize || rd32(p) != 0x02014b50) goto done;
        unsigned method = rd16(p + 10); unsigned long csize = rd32(p + 20), usize = rd32(p + 24);
        unsigned nlen = rd16(p + 28), xlen = rd16(p + 30), clen = rd16(p + 32);
        unsigned long attr = rd32(p + 38), loff = rd32(p + 42);
        char name[600]; if (nlen >= sizeof(name)) goto done;
        memcpy(name, p + 46, nlen); name[nlen] = 0;
        p += 46 + nlen + xlen + clen;
        if (strstr(name, "..") || name[0] == '/') goto done;           /* never outside dest */
        char out[1400]; snprintf(out, sizeof(out), "%s/%s", dest, name);
        unsigned mode = (attr >> 16) & 0777;
        if (nlen && name[nlen - 1] == '/') { mkdirs(out); continue; }
        if (((attr >> 16) & 0170000) == 0120000) continue;             /* no symlinks in our packages */
        char *sl = strrchr(out, '/'); if (sl) { *sl = 0; mkdirs(out); *sl = '/'; }
        unsigned char lh[30]; fseek(f, (long)loff, SEEK_SET);
        if (fread(lh, 1, 30, f) != 30 || rd32(lh) != 0x04034b50) goto done;
        fseek(f, (long)loff + 30 + rd16(lh + 26) + rd16(lh + 28), SEEK_SET);
        FILE *o = fopen(out, "wb"); if (!o) goto done;
        static unsigned char in[65536], buf[65536];
        unsigned long left = csize, wrote = 0; bool good = true;
        if (method == 0) {
            while (left && good) { size_t n = fread(in, 1, left > sizeof(in) ? sizeof(in) : left, f); if (!n) good = false; else { fwrite(in, 1, n, o); left -= n; wrote += n; } }
        } else if (method == 8) {
            z_stream z; memset(&z, 0, sizeof(z));
            if (inflateInit2(&z, -MAX_WBITS) != Z_OK) good = false;
            int zr = Z_OK;
            while (good && zr != Z_STREAM_END) {
                if (!z.avail_in && left) { size_t n = fread(in, 1, left > sizeof(in) ? sizeof(in) : left, f); if (!n) { good = false; break; } z.next_in = in; z.avail_in = (uInt)n; left -= n; }
                z.next_out = buf; z.avail_out = sizeof(buf);
                zr = inflate(&z, Z_NO_FLUSH);
                if (zr != Z_OK && zr != Z_STREAM_END) { good = false; break; }
                size_t n = sizeof(buf) - z.avail_out; fwrite(buf, 1, n, o); wrote += n;
                if (zr != Z_STREAM_END && !z.avail_in && !left && n == 0) { good = false; break; }
            }
            inflateEnd(&z);
        } else good = false;
        if (fclose(o) || !good || wrote != usize) goto done;
        if (mode) chmod(out, mode);
    }
    ok = true;
done:
    free(cd); fclose(f);
    return ok;
}

/* ---------- apply ---------- */
static int rm_tree(const char *p) {
    struct stat st; if (lstat(p, &st)) return 0;
    if (S_ISDIR(st.st_mode)) {
        DIR *d = opendir(p); struct dirent *e;
        while (d && (e = readdir(d))) {
            if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
            char c[1400]; snprintf(c, sizeof(c), "%s/%s", p, e->d_name); rm_tree(c);
        }
        if (d) closedir(d);
        return rmdir(p);
    }
    return unlink(p);
}
/* move every file of src (relative path rel) over dst, keeping the old one in bak */
static bool merge(const char *src, const char *dst, const char *bak, int depth) {
    DIR *d = opendir(src); if (!d) return false;
    struct dirent *e; bool ok = true;
    while ((e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..") || !strcmp(e->d_name, "component.json")) continue;
        char s[1400], t[1400], b[1400];
        snprintf(s, sizeof(s), "%s/%s", src, e->d_name); snprintf(t, sizeof(t), "%s/%s", dst, e->d_name); snprintf(b, sizeof(b), "%s/%s", bak, e->d_name);
        struct stat st; if (lstat(s, &st)) continue;
        /* whole runtime directories are swapped, so no stale module survives */
        bool swap = S_ISDIR(st.st_mode) && depth == 0 && !strcmp(e->d_name, "yt-dlp-runtime");
        if (S_ISDIR(st.st_mode) && !swap) { mkdir(t, 0755); mkdir(b, 0755); ok = merge(s, t, b, depth + 1) && ok; continue; }
        struct stat tt;
        if (!lstat(t, &tt)) { rm_tree(b); if (rename(t, b)) { ok = false; continue; } }
        if (rename(s, t)) { ok = false; rename(b, t); }
    }
    closedir(d);
    return ok;
}
static void ota_dir(char *out, size_t n, const char *sub) { snprintf(out, n, "%s/.ota%s%s", paths.app, sub ? "/" : "", sub ? sub : ""); }
static bool download_comp(Comp *k, const char *zip, long done_before) {
    for (int attempt = 0; attempt < 2; attempt++) {
        AVIOContext *io = open_url(k->url, attempt == 0);
        if (!io) continue;
        char part[1400]; snprintf(part, sizeof(part), "%s.part", zip);
        FILE *o = fopen(part, "wb");
        if (!o) { avio_closep(&io); return false; }
        mbedtls_sha256_context sh; mbedtls_sha256_init(&sh); mbedtls_sha256_starts(&sh, 0);
        static unsigned char buf[65536]; long got = 0;
        for (;;) {
            int n = avio_read(io, buf, sizeof(buf));
            if (n <= 0) break;
            if (fwrite(buf, 1, n, o) != (size_t)n) { got = -1; break; }
            mbedtls_sha256_update(&sh, buf, n); got += n;
            progress = (int)((done_before + got) * 100 / (total_bytes > 0 ? total_bytes : 1)); if (progress > 99) progress = 99;
            if (got > k->size) break;
        }
        avio_closep(&io);
        bool wr = !fclose(o);
        unsigned char d[32]; char hex[65]; mbedtls_sha256_finish(&sh, d); mbedtls_sha256_free(&sh);
        for (int i = 0; i < 32; i++) sprintf(hex + i * 2, "%02x", d[i]);
        if (wr && got == k->size && !strcmp(hex, k->sha)) return !rename(part, zip);
        unlink(part);
        Log_msg("ota: %s: %ld of %ld bytes, checksum %s", k->name, got, k->size, strcmp(hex, k->sha) ? "wrong" : "ok");
    }
    return false;
}
static void *install_thread(void *arg) {
    (void)arg;
    char top[1300], stage[1300], bak[1300], dl[1300];
    ota_dir(top, sizeof(top), NULL); mkdir(top, 0755);
    ota_dir(stage, sizeof(stage), "stage"); ota_dir(bak, sizeof(bak), "backup"); ota_dir(dl, sizeof(dl), "download");
    rm_tree(stage); mkdir(stage, 0755); mkdir(dl, 0755);
    long done = 0; bool ok = true;
    Comp list[3]; int n;
    pthread_mutex_lock(&lock); n = ncomps; memcpy(list, comps, sizeof(list)); pthread_mutex_unlock(&lock);
    /* 1. fetch and check everything first: nothing changes unless all of it arrived intact */
    for (int i = 0; i < n && ok; i++) {
        if (!list[i].need) continue;
        char zip[1400]; snprintf(zip, sizeof(zip), "%s/%s.zip", dl, list[i].name);
        set_state(OTA_DOWNLOADING, T("Đang tải %s…"), list[i].name);
        ok = download_comp(&list[i], zip, done);
        done += list[i].size;
        if (!ok) { set_state(OTA_ERROR, T("Tải %s lỗi · thử lại sau"), list[i].name); break; }
        char sd[1400]; snprintf(sd, sizeof(sd), "%s/%s", stage, list[i].name);
        mkdir(sd, 0755);
        if (!unzip_to(zip, sd)) { ok = false; set_state(OTA_ERROR, T("Giải nén %s lỗi (thẻ nhớ đầy?)"), list[i].name); }
        unlink(zip);
    }
    /* 2. swap them in; the old files go to .ota/backup for launch.sh's rollback */
    if (ok) {
        set_state(OTA_DOWNLOADING, "%s", T("Đang cài…"));
        rm_tree(bak); mkdir(bak, 0755);
        for (int i = 0; i < n; i++) {
            if (!list[i].need) continue;
            char sd[1400], bd[1400];
            snprintf(sd, sizeof(sd), "%s/%s", stage, list[i].name);
            bool app = !strncmp(list[i].name, "app-", 4);
            snprintf(bd, sizeof(bd), "%s/%s", bak, app ? "app" : "bin"); mkdir(bd, 0755);
            if (!merge(sd, app ? paths.app : paths.bin, bd, 0)) ok = false;
            char p[1300]; snprintf(p, sizeof(p), "%s/deno", paths.bin); chmod(p, 0755);   /* FAT cards ignore it */
        }
        char p[1400]; snprintf(p, sizeof(p), "%s/pending", top);
        FILE *f = fopen(p, "w"); if (f) { fprintf(f, "%s\n", latest); fclose(f); }
        snprintf(p, sizeof(p), "%s/tries", top); unlink(p);
        sync();
        if (ok) set_state(OTA_READY, T("Đã cập nhật lên %s · khởi động lại để dùng"), latest);
        else set_state(OTA_READY, "%s", T("Cập nhật xong một phần · khởi động lại app"));
    }
    rm_tree(stage);
    pthread_mutex_lock(&lock); busy = false; pthread_mutex_unlock(&lock);
    return NULL;
}
bool Ota_install(void) {
    pthread_mutex_lock(&lock);
    if (busy || state != OTA_AVAILABLE) { pthread_mutex_unlock(&lock); return false; }
    busy = true; progress = 0; state = OTA_DOWNLOADING;
    pthread_mutex_unlock(&lock);
    pthread_t t;
    if (pthread_create(&t, NULL, install_thread, NULL)) { busy = false; return false; }
    pthread_detach(t);
    return true;
}

/* ---------- life cycle ---------- */
int Ota_state(void) { return state; }
int Ota_progress(void) { return progress; }
long Ota_bytes(void) { return total_bytes; }
const char *Ota_latest(void) { return latest; }
const char *Ota_notes(void) { static char n[600]; pthread_mutex_lock(&lock); snprintf(n, sizeof(n), "%s", notes); pthread_mutex_unlock(&lock); return n; }
const char *Ota_message(void) { static char m[200]; pthread_mutex_lock(&lock); snprintf(m, sizeof(m), "%s", msg); pthread_mutex_unlock(&lock); return m; }
void Ota_init(void) {
    started_at = SDL_GetTicks();
    /* first run after an update: say so in the log; the marker is cleared after 20 s */
    char p[1300]; ota_dir(p, sizeof(p), "pending");
    if (!access(p, F_OK)) Log_msg("ota: first start of an updated version");
    else pending_cleared = true;
    /* automatic check at most once a day, 30 s after start (the network is up by then) */
    char sf[1300]; state_file(sf, sizeof(sf)); char line[40]; long long last = 0;
    if (read_line(sf, line, sizeof(line))) last = atoll(line);
    long long now = time(NULL);
    next_auto = started_at + 30000;
    if (last > 1600000000LL && now > last && now - last < 20 * 3600) next_auto = 0;   /* checked recently */
}
void Ota_tick(Uint32 now) {
    if (!pending_cleared && now - started_at > 20000) {
        /* this version runs: the backup is no longer needed */
        char p[1300]; ota_dir(p, sizeof(p), "pending"); unlink(p);
        ota_dir(p, sizeof(p), "tries"); unlink(p);
        ota_dir(p, sizeof(p), "backup"); rm_tree(p);
        pending_cleared = true;
        Log_msg("ota: update confirmed");
    }
    if (next_auto && S.ota_auto && (Sint32)(now - next_auto) >= 0) { next_auto = 0; Ota_check(false); }
}
