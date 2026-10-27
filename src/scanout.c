/* NXT Retro Tube — direct panel output for the TrimUI Smart Pro S (stock firmware).
 * The firmware's SDL there cannot open the panel (its KMSDRM driver reports "not available",
 * there is no usable GLES), so SDL ends up on the invisible "offscreen" driver. Here the app
 * draws with SDL's CPU renderer into a plain memory surface and this file puts each finished
 * frame on the panel itself:
 *   1. KMS: two "dumb" buffers on /dev/dri/card*, page flips on vsync (no libdrm/gbm needed)
 *   2. fbdev: /dev/fb0 (pan between two halves if the driver allows, else single buffer)
 * Also logs what the device offers (DRM connectors, fb0, GPU libraries) for future fixes. */
#include "app.h"
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <poll.h>
#include <dirent.h>
#include <dlfcn.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <drm/drm.h>
#include <drm/drm_mode.h>
#include <linux/fb.h>

static struct {
    int kind;                    /* 0 none, 1 KMS, 2 fbdev */
    int fd;
    int w, h;                    /* panel mode */
    int lw, lh;                  /* what the app draws (landscape) */
    int rot;                     /* 0, 90, 270: panel mounted in portrait */
    /* KMS */
    uint32_t crtc, conn, fb[2], handle[2], pitch[2];
    uint64_t size[2];
    uint8_t *map[2];
    int back; bool flip_ok, flip_pending;
    struct drm_mode_modeinfo mode;
    struct drm_mode_crtc saved; bool have_saved;
    /* fbdev */
    uint8_t *fbmem; size_t fblen; int fbpitch, fbbpp, fbpages, fbpage;
    struct fb_var_screeninfo var;
    Uint32 last;
    SDL_Surface *surf;
} SO = {.fd = -1};

static int xioctl(int fd, unsigned long req, void *arg) {
    int r; do r = ioctl(fd, req, arg); while (r == -1 && (errno == EINTR || errno == EAGAIN));
    return r;
}

/* ---------- diagnostics ---------- */
static void diag_lib(const char *name) {
    void *h = dlopen(name, RTLD_LAZY | RTLD_LOCAL);
    Log_msg("display diag: %s %s", name, h ? "loads" : dlerror());
    if (h) dlclose(h);
}
void Scanout_diag(void) {
    static bool done; if (done) return; done = true;
    Dl_info di; if (dladdr((void *)SDL_Init, &di) && di.dli_fname) Log_msg("display diag: SDL from %s", di.dli_fname);
    diag_lib("libdrm.so.2"); diag_lib("libgbm.so.1"); diag_lib("libEGL.so.1"); diag_lib("libGLESv2.so.2"); diag_lib("libmali.so");
    DIR *d = opendir("/dev/dri"); struct dirent *e; char list[300] = "";
    while (d && (e = readdir(d))) if (e->d_name[0] != '.') { size_t l = strlen(list); snprintf(list + l, sizeof(list) - l, "%s ", e->d_name); }
    if (d) closedir(d);
    Log_msg("display diag: /dev/dri: %s", list[0] ? list : "(none)");
    list[0] = 0; d = opendir("/dev");
    while (d && (e = readdir(d))) if (!strncmp(e->d_name, "fb", 2) || !strncmp(e->d_name, "disp", 4) || !strncmp(e->d_name, "mali", 4)) { size_t l = strlen(list); snprintf(list + l, sizeof(list) - l, "%s ", e->d_name); }
    if (d) closedir(d);
    Log_msg("display diag: /dev: %s", list[0] ? list : "(no fb/disp/mali)");
    /* which GPU / display libraries the firmware menu itself uses */
    d = opendir("/proc");
    while (d && (e = readdir(d))) {
        char *end; long pid = strtol(e->d_name, &end, 10);
        if (*end || pid <= 1 || pid == getpid()) continue;
        char p[64], comm[40] = ""; snprintf(p, sizeof(p), "/proc/%ld/comm", pid);
        FILE *f = fopen(p, "r"); if (!f) continue; if (fgets(comm, sizeof(comm), f)) comm[strcspn(comm, "\n")] = 0; fclose(f);
        if (strcasecmp(comm, "MainUI") && strncasecmp(comm, "trimui", 6) && strcasecmp(comm, "launcher")) continue;
        snprintf(p, sizeof(p), "/proc/%ld/maps", pid);
        f = fopen(p, "r"); if (!f) continue;
        char line[512], libs[700] = "";
        while (fgets(line, sizeof(line), f)) {
            char *so = strrchr(line, '/'); if (!so) continue;
            so[strcspn(so, "\n")] = 0;
            if (!strcasestr(so, "sdl") && !strcasestr(so, "gbm") && !strcasestr(so, "egl") && !strcasestr(so, "gles") && !strcasestr(so, "mali") && !strcasestr(so, "drm")) continue;
            char *full = strchr(line, '/'); if (strstr(libs, full)) continue;
            size_t l = strlen(libs); snprintf(libs + l, sizeof(libs) - l, "%s ", full);
        }
        fclose(f);
        Log_msg("display diag: %s(%ld) uses %s", comm, pid, libs[0] ? libs : "(no SDL/GPU libraries)");
    }
    if (d) closedir(d);
}

/* ---------- KMS ---------- */
static bool kms_open_card(const char *dev) {
    int fd = open(dev, O_RDWR | O_CLOEXEC);
    if (fd < 0) { Log_msg("scanout: %s: %s", dev, strerror(errno)); return false; }
    struct drm_mode_card_res res = {0};
    if (xioctl(fd, DRM_IOCTL_MODE_GETRESOURCES, &res) || !res.count_connectors || !res.count_crtcs) {
        Log_msg("scanout: %s: no display resources (%s)", dev, strerror(errno)); close(fd); return false;
    }
    uint32_t conns[16], crtcs[16], encs[16], fbs[16];
    if (res.count_connectors > 16) res.count_connectors = 16;
    if (res.count_crtcs > 16) res.count_crtcs = 16;
    if (res.count_encoders > 16) res.count_encoders = 16;
    if (res.count_fbs > 16) res.count_fbs = 16;
    res.connector_id_ptr = (uintptr_t)conns; res.crtc_id_ptr = (uintptr_t)crtcs;
    res.encoder_id_ptr = (uintptr_t)encs; res.fb_id_ptr = (uintptr_t)fbs;
    if (xioctl(fd, DRM_IOCTL_MODE_GETRESOURCES, &res)) { close(fd); return false; }
    Log_msg("scanout: %s: %u connectors, %u encoders, %u crtcs", dev, res.count_connectors, res.count_encoders, res.count_crtcs);
    for (uint32_t i = 0; i < res.count_connectors && i < 16; i++) {
        struct drm_mode_get_connector c = {.connector_id = conns[i]};
        if (xioctl(fd, DRM_IOCTL_MODE_GETCONNECTOR, &c)) continue;
        uint32_t nm = c.count_modes, ne = c.count_encoders;
        struct drm_mode_modeinfo modes[32]; uint32_t cencs[16];
        if (nm > 32) nm = 32;
        if (ne > 16) ne = 16;
        struct drm_mode_get_connector c2 = {.connector_id = conns[i], .count_modes = nm, .modes_ptr = (uintptr_t)modes,
                                            .count_encoders = ne, .encoders_ptr = (uintptr_t)cencs};
        if (xioctl(fd, DRM_IOCTL_MODE_GETCONNECTOR, &c2)) continue;
        Log_msg("scanout: connector %u type %u status %u, %u modes%s", conns[i], c2.connector_type, c2.connection, c2.count_modes,
                c2.count_modes ? "" : " (none)");
        if (c2.connection == 2 || !c2.count_modes) continue;          /* 1 connected, 2 disconnected, 3 unknown */
        int pick = 0;
        for (uint32_t m = 0; m < nm && m < c2.count_modes; m++) if (modes[m].type & DRM_MODE_TYPE_PREFERRED) { pick = (int)m; break; }
        SO.mode = modes[pick];
        /* CRTC: the one the encoder drives now, else the first it may use */
        uint32_t crtc = 0;
        uint32_t encids[17]; int nenc = 0;
        if (c2.encoder_id) encids[nenc++] = c2.encoder_id;
        for (uint32_t k = 0; k < ne && k < c2.count_encoders; k++) encids[nenc++] = cencs[k];
        for (int k = 0; k < nenc && !crtc; k++) {
            struct drm_mode_get_encoder en = {.encoder_id = encids[k]};
            if (xioctl(fd, DRM_IOCTL_MODE_GETENCODER, &en)) continue;
            if (en.crtc_id) { crtc = en.crtc_id; break; }
            for (uint32_t j = 0; j < res.count_crtcs; j++) if (en.possible_crtcs & (1u << j)) { crtc = crtcs[j]; break; }
        }
        if (!crtc) { Log_msg("scanout: connector %u: no CRTC", conns[i]); continue; }
        SO.fd = fd; SO.crtc = crtc; SO.conn = conns[i];
        SO.w = SO.mode.hdisplay; SO.h = SO.mode.vdisplay;
        Log_msg("scanout: %s connector %u crtc %u mode %s %dx%d@%u", dev, SO.conn, SO.crtc, SO.mode.name, SO.w, SO.h, SO.mode.vrefresh);
        return true;
    }
    close(fd);
    return false;
}
static void kms_free_buffers(void) {
    for (int i = 0; i < 2; i++) {
        if (SO.map[i]) munmap(SO.map[i], SO.size[i]);
        if (SO.fb[i]) { uint32_t id = SO.fb[i]; xioctl(SO.fd, DRM_IOCTL_MODE_RMFB, &id); }
        if (SO.handle[i]) { struct drm_mode_destroy_dumb dd = {.handle = SO.handle[i]}; xioctl(SO.fd, DRM_IOCTL_MODE_DESTROY_DUMB, &dd); }
        SO.map[i] = NULL; SO.fb[i] = SO.handle[i] = 0;
    }
}
static bool kms_open(void) {
    char dev[32];
    bool found = false;
    for (int n = 0; n < 8 && !found; n++) {
        snprintf(dev, sizeof(dev), "/dev/dri/card%d", n);
        if (access(dev, F_OK)) continue;
        found = kms_open_card(dev);
    }
    if (!found) return false;
    if (xioctl(SO.fd, DRM_IOCTL_SET_MASTER, NULL)) Log_msg("scanout: master: %s (trying anyway)", strerror(errno));
    struct drm_mode_crtc cur = {.crtc_id = SO.crtc};
    if (!xioctl(SO.fd, DRM_IOCTL_MODE_GETCRTC, &cur)) { SO.saved = cur; SO.have_saved = true; }
    for (int i = 0; i < 2; i++) {
        struct drm_mode_create_dumb cd = {.width = (uint32_t)SO.w, .height = (uint32_t)SO.h, .bpp = 32};
        if (xioctl(SO.fd, DRM_IOCTL_MODE_CREATE_DUMB, &cd)) { Log_msg("scanout: dumb buffer: %s", strerror(errno)); goto fail; }
        SO.handle[i] = cd.handle; SO.pitch[i] = cd.pitch; SO.size[i] = cd.size;
        struct drm_mode_fb_cmd fc = {.width = (uint32_t)SO.w, .height = (uint32_t)SO.h, .pitch = cd.pitch, .bpp = 32, .depth = 24, .handle = cd.handle};
        if (xioctl(SO.fd, DRM_IOCTL_MODE_ADDFB, &fc)) { Log_msg("scanout: addfb: %s", strerror(errno)); goto fail; }
        SO.fb[i] = fc.fb_id;
        struct drm_mode_map_dumb md = {.handle = cd.handle};
        if (xioctl(SO.fd, DRM_IOCTL_MODE_MAP_DUMB, &md)) { Log_msg("scanout: map: %s", strerror(errno)); goto fail; }
        SO.map[i] = mmap(NULL, cd.size, PROT_READ | PROT_WRITE, MAP_SHARED, SO.fd, (off_t)md.offset);
        if (SO.map[i] == MAP_FAILED) { SO.map[i] = NULL; Log_msg("scanout: mmap: %s", strerror(errno)); goto fail; }
        memset(SO.map[i], 0, cd.size);
    }
    struct drm_mode_crtc sc = {.crtc_id = SO.crtc, .fb_id = SO.fb[0], .set_connectors_ptr = (uintptr_t)&SO.conn,
                               .count_connectors = 1, .mode = SO.mode, .mode_valid = 1};
    if (xioctl(SO.fd, DRM_IOCTL_MODE_SETCRTC, &sc)) { Log_msg("scanout: setcrtc: %s", strerror(errno)); goto fail; }
    SO.back = 1; SO.flip_ok = true; SO.flip_pending = false;
    SO.kind = 1;
    return true;
fail:
    kms_free_buffers();
    xioctl(SO.fd, DRM_IOCTL_DROP_MASTER, NULL);
    close(SO.fd); SO.fd = -1;
    return false;
}
static void kms_wait_flip(int ms) {
    if (!SO.flip_pending) return;
    struct pollfd p = {.fd = SO.fd, .events = POLLIN};
    if (poll(&p, 1, ms) > 0) {
        char buf[256];
        ssize_t n = read(SO.fd, buf, sizeof(buf));
        (void)n;
    }
    SO.flip_pending = false;
}

/* ---------- fbdev ---------- */
static bool fb_open(void) {
    const char *fake = getenv("NXT_TUBE_FAKE_FB");   /* QA: "file:WxH:bpp" stands in for /dev/fb0 */
    if (fake) {
        char path[600]; int w = 0, h = 0, bpp = 32;
        if (sscanf(fake, "%599[^:]:%dx%d:%d", path, &w, &h, &bpp) < 3) return false;
        int fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0644);
        if (fd < 0 || ftruncate(fd, (off_t)w * h * (bpp / 8))) return false;
        SO.fblen = (size_t)w * h * (bpp / 8);
        SO.fbmem = mmap(NULL, SO.fblen, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        if (SO.fbmem == MAP_FAILED) { SO.fbmem = NULL; close(fd); return false; }
        memset(&SO.var, 0, sizeof(SO.var)); SO.var.xres = SO.var.xres_virtual = (uint32_t)w; SO.var.yres = SO.var.yres_virtual = (uint32_t)h;
        SO.fd = fd; SO.w = w; SO.h = h; SO.fbpitch = w * (bpp / 8); SO.fbbpp = bpp; SO.fbpages = 1; SO.fbpage = 0; SO.kind = 2;
        return true;
    }
    int fd = open("/dev/fb0", O_RDWR | O_CLOEXEC);
    if (fd < 0) { Log_msg("scanout: /dev/fb0: %s", strerror(errno)); return false; }
    struct fb_fix_screeninfo fix;
    if (xioctl(fd, FBIOGET_VSCREENINFO, &SO.var) || xioctl(fd, FBIOGET_FSCREENINFO, &fix)) { close(fd); return false; }
    Log_msg("scanout: fb0 %ux%u (virtual %ux%u) %u bpp, line %u", SO.var.xres, SO.var.yres, SO.var.xres_virtual, SO.var.yres_virtual, SO.var.bits_per_pixel, fix.line_length);
    if (SO.var.bits_per_pixel != 32 && SO.var.bits_per_pixel != 16) { close(fd); return false; }
    SO.fblen = fix.smem_len;
    SO.fbmem = mmap(NULL, SO.fblen, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (SO.fbmem == MAP_FAILED) { SO.fbmem = NULL; Log_msg("scanout: fb0 mmap: %s", strerror(errno)); close(fd); return false; }
    SO.fd = fd; SO.w = (int)SO.var.xres; SO.h = (int)SO.var.yres;
    SO.fbpitch = (int)fix.line_length; SO.fbbpp = (int)SO.var.bits_per_pixel;
    SO.fbpages = SO.var.yres_virtual >= 2 * SO.var.yres && SO.fblen >= (size_t)SO.fbpitch * SO.var.yres * 2 ? 2 : 1;
    SO.fbpage = 0;
    SO.kind = 2;
    return true;
}

/* ---------- copy one frame (with rotation for a portrait panel) ---------- */
static void put_frame(uint8_t *dst, int pitch, int bpp) {
    SDL_Surface *s = SO.surf;
    const uint8_t *src = s->pixels;
    if (!SO.rot) {
        int w = SO.lw < SO.w ? SO.lw : SO.w, h = SO.lh < SO.h ? SO.lh : SO.h;
        if (bpp == 32) for (int y = 0; y < h; y++) memcpy(dst + (size_t)y * pitch, src + (size_t)y * s->pitch, (size_t)w * 4);
        else for (int y = 0; y < h; y++) {
            const uint32_t *in = (const uint32_t *)(src + (size_t)y * s->pitch); uint16_t *out = (uint16_t *)(dst + (size_t)y * pitch);
            for (int x = 0; x < w; x++) { uint32_t c = in[x]; out[x] = (uint16_t)(((c >> 8) & 0xF800) | ((c >> 5) & 0x07E0) | ((c >> 3) & 0x001F)); }
        }
        return;
    }
    /* portrait panel (w < h): the app's row y becomes a column; rotate in 32x32 tiles (cache friendly) */
    int W = SO.lw, H = SO.lh;
    for (int ty = 0; ty < H; ty += 32) for (int tx = 0; tx < W; tx += 32)
        for (int y = ty; y < ty + 32 && y < H; y++) {
            const uint32_t *in = (const uint32_t *)(src + (size_t)y * s->pitch);
            for (int x = tx; x < tx + 32 && x < W; x++) {
                int px, py;
                if (SO.rot == 90) { px = H - 1 - y; py = x; } else { px = y; py = W - 1 - x; }
                if (px >= SO.w || py >= SO.h) continue;
                if (bpp == 32) ((uint32_t *)(dst + (size_t)py * pitch))[px] = in[x];
                else { uint32_t c = in[x]; ((uint16_t *)(dst + (size_t)py * pitch))[px] = (uint16_t)(((c >> 8) & 0xF800) | ((c >> 5) & 0x07E0) | ((c >> 3) & 0x001F)); }
            }
        }
}

/* ---------- public ---------- */
bool Scanout_open(int *lw, int *lh) {
    Scanout_close();
    Scanout_diag();
    if (!kms_open() && !fb_open()) { Log_msg("scanout: no way to reach the panel"); return false; }
    SO.rot = 0; SO.lw = SO.w; SO.lh = SO.h;
    if (SO.h > SO.w) {      /* panel mounted in portrait: draw landscape and turn it */
        const char *r = getenv("NXT_TUBE_ROTATE");
        SO.rot = r && atoi(r) == 270 ? 270 : 90;
        SO.lw = SO.h; SO.lh = SO.w;
    }
    SO.surf = SDL_CreateRGBSurfaceWithFormat(0, SO.lw, SO.lh, 32, SDL_PIXELFORMAT_XRGB8888);
    if (!SO.surf) { Log_msg("scanout: surface: %s", SDL_GetError()); Scanout_close(); return false; }
    SDL_FillRect(SO.surf, NULL, 0);
    *lw = SO.lw; *lh = SO.lh;
    Log_msg("scanout: %s, panel %dx%d, app draws %dx%d%s", SO.kind == 1 ? "KMS (page flip on vsync)" : SO.fbpages > 1 ? "fbdev (2 pages)" : "fbdev",
            SO.w, SO.h, SO.lw, SO.lh, SO.rot ? (SO.rot == 90 ? ", turned 90°" : ", turned 270°") : "");
    SO.last = SDL_GetTicks();
    return true;
}
SDL_Surface *Scanout_surface(void) { return SO.surf; }
bool Scanout_active(void) { return SO.kind != 0; }
void Scanout_present(void) {
    if (!SO.kind || !SO.surf) return;
    if (SO.kind == 1) {
        kms_wait_flip(50);                                   /* the back buffer is free once the last flip happened */
        int b = SO.flip_ok ? SO.back : 0;
        put_frame(SO.map[b], (int)SO.pitch[b], 32);
        if (SO.flip_ok) {
            struct drm_mode_crtc_page_flip pf = {.crtc_id = SO.crtc, .fb_id = SO.fb[b], .flags = DRM_MODE_PAGE_FLIP_EVENT};
            if (xioctl(SO.fd, DRM_IOCTL_MODE_PAGE_FLIP, &pf)) {
                Log_msg("scanout: page flip: %s, drawing to one buffer", strerror(errno));
                SO.flip_ok = false;
                struct drm_mode_crtc sc = {.crtc_id = SO.crtc, .fb_id = SO.fb[0], .set_connectors_ptr = (uintptr_t)&SO.conn,
                                           .count_connectors = 1, .mode = SO.mode, .mode_valid = 1};
                xioctl(SO.fd, DRM_IOCTL_MODE_SETCRTC, &sc);
                put_frame(SO.map[0], (int)SO.pitch[0], 32);
            } else { SO.flip_pending = true; SO.back ^= 1; }
        }
    } else {
        int page = SO.fbpages > 1 ? SO.fbpage ^ 1 : 0;
        put_frame(SO.fbmem + (size_t)page * SO.fbpitch * SO.var.yres, SO.fbpitch, SO.fbbpp);
        if (SO.fbpages > 1) {
            SO.var.yoffset = (uint32_t)(page * (int)SO.var.yres);
            if (!xioctl(SO.fd, FBIOPAN_DISPLAY, &SO.var)) SO.fbpage = page; else SO.fbpages = 1;
        }
        int dummy = 0;
        if (xioctl(SO.fd, FBIO_WAITFORVSYNC, &dummy)) {
            /* no vsync from this driver: keep to ~60 frames a second so the CPU stays cool */
            Uint32 now = SDL_GetTicks();
            if (now - SO.last < 16) SDL_Delay(16 - (now - SO.last));
        }
    }
    if (!SO.flip_ok && SO.kind == 1) { Uint32 now = SDL_GetTicks(); if (now - SO.last < 16) SDL_Delay(16 - (now - SO.last)); }
    SO.last = SDL_GetTicks();
}
void Scanout_close(void) {
    if (SO.kind == 1) {
        kms_wait_flip(60);
        if (SO.have_saved && SO.saved.fb_id) {
            struct drm_mode_crtc sc = SO.saved; sc.set_connectors_ptr = (uintptr_t)&SO.conn; sc.count_connectors = 1;
            xioctl(SO.fd, DRM_IOCTL_MODE_SETCRTC, &sc);
        }
        kms_free_buffers();
        xioctl(SO.fd, DRM_IOCTL_DROP_MASTER, NULL);
    } else if (SO.kind == 2) {
        if (SO.fbmem) { if (!getenv("NXT_TUBE_FAKE_FB")) memset(SO.fbmem, 0, SO.fblen); munmap(SO.fbmem, SO.fblen); }
        SO.var.yoffset = 0; xioctl(SO.fd, FBIOPAN_DISPLAY, &SO.var);
        SO.fbmem = NULL;
    }
    if (SO.fd >= 0) close(SO.fd);
    if (SO.surf) SDL_FreeSurface(SO.surf);
    SO.surf = NULL; SO.fd = -1; SO.kind = 0; SO.have_saved = false;
}
