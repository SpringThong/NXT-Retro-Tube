/* Vietnamese Telex input: raw keystrokes -> UTF-8 text.
 * aa â, aw ă, ee ê, oo ô, ow ơ, uw/w ư, uow ươ, dd đ; s f r x j tones, z clears.
 * SPDX-License-Identifier: GPL-3.0-only */
#include <stdbool.h>
#include <stddef.h>
#include <string.h>
#include <stdint.h>

enum { V_A, V_AW, V_AA, V_E, V_EE, V_I, V_O, V_OO, V_OW, V_U, V_UW, V_Y, NV };
static const char *const TONED[NV][6] = {
    {"a", "á", "à", "ả", "ã", "ạ"}, {"ă", "ắ", "ằ", "ẳ", "ẵ", "ặ"}, {"â", "ấ", "ầ", "ẩ", "ẫ", "ậ"},
    {"e", "é", "è", "ẻ", "ẽ", "ẹ"}, {"ê", "ế", "ề", "ể", "ễ", "ệ"}, {"i", "í", "ì", "ỉ", "ĩ", "ị"},
    {"o", "ó", "ò", "ỏ", "õ", "ọ"}, {"ô", "ố", "ồ", "ổ", "ỗ", "ộ"}, {"ơ", "ớ", "ờ", "ở", "ỡ", "ợ"},
    {"u", "ú", "ù", "ủ", "ũ", "ụ"}, {"ư", "ứ", "ừ", "ử", "ữ", "ự"}, {"y", "ý", "ỳ", "ỷ", "ỹ", "ỵ"}};
/* letters: ASCII for consonants, 0x100+V_x for vowels, 'D' for đ */
#define VW(v) (0x100 + (v))
typedef struct { int c[48]; int n; int tone; char raw[48]; int nraw; bool literal; } Syl;

static int vowel_of(int ch) { return ch >= 0x100 ? ch - 0x100 : -1; }
static int plain_vowel(char c) {
    switch (c) { case 'a': return V_A; case 'e': return V_E; case 'i': return V_I; case 'o': return V_O; case 'u': return V_U; case 'y': return V_Y; }
    return -1;
}
/* indices of the vowel nucleus, skipping the u of "qu" and the i of "gi" */
static int nucleus(const Syl *s, int *first) {
    int start = 0;
    if (s->n >= 2 && s->c[0] == 'q' && vowel_of(s->c[1]) == V_U) start = 2;
    if (s->n >= 3 && s->c[0] == 'g' && vowel_of(s->c[1]) == V_I && vowel_of(s->c[2]) >= 0) start = 2;
    int i = start;
    while (i < s->n && vowel_of(s->c[i]) < 0) i++;
    *first = i;
    int len = 0;
    while (i + len < s->n && vowel_of(s->c[i + len]) >= 0) len++;
    if (!len && start == 2 && s->c[0] == 'g') { *first = 1; return 1; }   /* "gì" */
    if (!len && start == 2 && s->c[0] == 'q') { *first = 1; return 1; }
    return len;
}
static int tone_pos(const Syl *s) {
    int f, len = nucleus(s, &f);
    if (!len) return -1;
    for (int i = f; i < f + len; i++) if (vowel_of(s->c[i]) == V_OW) return i;
    for (int i = f; i < f + len; i++) { int v = vowel_of(s->c[i]); if (v == V_AW || v == V_AA || v == V_EE || v == V_OO || v == V_UW) return i; }
    if (len == 1) return f;
    if (len >= 3) return f + 1;
    return f + len < s->n ? f + 1 : f;   /* closed syllable: second vowel; open: first (hòa, thủy) */
}
static void apply(Syl *s, char c) {
    int f, len = nucleus(s, &f);
    int last_v = -1;
    for (int i = s->n - 1; i >= 0; i--) if (vowel_of(s->c[i]) >= 0) { last_v = i; break; }
    int tone = c == 's' ? 1 : c == 'f' ? 2 : c == 'r' ? 3 : c == 'x' ? 4 : c == 'j' ? 5 : 0;
    if (tone && len) {
        if (s->tone == tone) { s->tone = 0; s->c[s->n++] = c; s->literal = true; }   /* "ss" types a plain s */
        else s->tone = tone;
        return;
    }
    if (c == 'z' && len && s->tone) { s->tone = 0; return; }
    if (c == 'd' && s->n && (s->c[s->n - 1] == 'd' || s->c[s->n - 1] == 'D')) {
        if (s->c[s->n - 1] == 'd') s->c[s->n - 1] = 'D'; else { s->c[s->n - 1] = 'd'; s->c[s->n++] = 'd'; }
        return;
    }
    if (c == 'w') {
        for (int i = 0; i + 1 < s->n; i++)
            if (vowel_of(s->c[i]) == V_U && vowel_of(s->c[i + 1]) == V_O) { s->c[i] = VW(V_UW); s->c[i + 1] = VW(V_OW); return; }
        if (last_v >= 0) {
            int v = vowel_of(s->c[last_v]);
            if (v == V_A && last_v > 0 && vowel_of(s->c[last_v - 1]) == V_U && !(s->n >= 2 && s->c[0] == 'q')) { s->c[last_v - 1] = VW(V_UW); return; }   /* "muaw" = mưa */
            if (v == V_A) { s->c[last_v] = VW(V_AW); return; }
            if (v == V_O) { s->c[last_v] = VW(V_OW); return; }
            if (v == V_U) { s->c[last_v] = VW(V_UW); return; }
            if (v == V_AW) { s->c[last_v] = VW(V_A); s->c[s->n++] = 'w'; return; }
            if (v == V_OW) { s->c[last_v] = VW(V_O); s->c[s->n++] = 'w'; return; }
            if (v == V_UW) { s->c[last_v] = VW(V_U); s->c[s->n++] = 'w'; return; }
            s->c[s->n++] = 'w'; return;
        }
        s->c[s->n++] = VW(V_UW);   /* w alone = ư */
        return;
    }
    int pv = plain_vowel(c);
    if ((c == 'a' || c == 'e' || c == 'o') && last_v >= 0) {
        int v = vowel_of(s->c[last_v]);
        int hat = c == 'a' ? V_AA : c == 'e' ? V_EE : V_OO;
        if (v == pv || (c == 'a' && v == V_AW) || (c == 'o' && v == V_OW)) { s->c[last_v] = VW(hat); return; }
        if (v == hat) { s->c[last_v] = VW(pv); s->c[s->n++] = VW(pv); return; }
    }
    s->c[s->n++] = pv >= 0 ? VW(pv) : c;
}
/* consonants* vowels+ (c|ch|m|n|ng|nh|p|t)? — anything else is a foreign word, typed as-is */
static bool valid_shape(const Syl *s) {
    int i = 0;
    while (i < s->n && vowel_of(s->c[i]) < 0) i++;
    if (i == s->n) return true;
    while (i < s->n && vowel_of(s->c[i]) >= 0) i++;
    char fin[4] = {0}; int k = 0;
    for (; i < s->n; i++) { if (vowel_of(s->c[i]) >= 0 || k >= 2) return false; fin[k++] = (char)s->c[i]; }
    static const char *ok[] = {"", "c", "ch", "m", "n", "ng", "nh", "p", "t"};
    for (unsigned j = 0; j < sizeof(ok) / sizeof(ok[0]); j++) if (!strcmp(fin, ok[j])) return true;
    return false;
}
static size_t emit(const Syl *s, char *out, size_t cap) {
    if (!s->literal && !valid_shape(s)) {
        size_t l = (size_t)s->nraw < cap - 1 ? (size_t)s->nraw : cap - 1;
        memcpy(out, s->raw, l); out[l] = 0; return l;
    }
    int tp = s->tone ? tone_pos(s) : -1;
    size_t k = 0;
    for (int i = 0; i < s->n; i++) {
        const char *t; char one[2] = {0, 0};
        int v = vowel_of(s->c[i]);
        if (v >= 0) t = TONED[v][i == tp ? s->tone : 0];
        else if (s->c[i] == 'D') t = "đ";
        else { one[0] = (char)s->c[i]; t = one; }
        size_t l = strlen(t);
        if (k + l + 1 >= cap) break;
        memcpy(out + k, t, l); k += l;
    }
    out[k] = 0;
    return k;
}
void Telex_convert(const char *raw, char *out, size_t cap) {
    size_t k = 0; Syl s; memset(&s, 0, sizeof(s));
    for (const char *p = raw;; p++) {
        char c = *p;
        bool letter = c >= 'a' && c <= 'z';
        if (letter && s.n < 40) { s.raw[s.nraw++] = c; apply(&s, c); continue; }
        if (s.n) { k += emit(&s, out + k, cap - k); memset(&s, 0, sizeof(s)); }
        if (!c) break;
        if (k + 2 < cap) out[k++] = c;
    }
    out[k < cap ? k : cap - 1] = 0;
}
#ifdef TELEX_TEST
#include <stdio.h>
int main(void) {
    const char *t[][2] = {{"vieetj nam", "việt nam"}, {"nguowif", "người"}, {"dduwowcj", "được"}, {"hoaf", "hòa"}, {"thuyr", "thủy"},
        {"quas", "quá"}, {"gif", "gì"}, {"tieengs", "tiếng"}, {"loffi muaw ddeem", "lofi mưa đêm"}, {"nhacj", "nhạc"},
        {"chuyeenj", "chuyện"}, {"khoong", "không"}, {"hayx", "hãy"}, {"w", "ư"}, {"sown tungf", "sơn tùng"}, {"mtp", "mtp"},
        {"phim hoatj hinhf", "phim hoạt hình"}, {"toanf", "toàn"}, {"giaf", "già"}, {"quoocs", "quốc"}, {"tuaanf", "tuần"},
        {"ass", "as"}, {"bawts", "bắt"}, {"youtube", "youtube"}, {"2024 remix", "2024 remix"}, {"cuar", "của"}, {"muaf", "mùa"}};
    int bad = 0;
    for (unsigned i = 0; i < sizeof(t) / sizeof(t[0]); i++) {
        char o[128]; Telex_convert(t[i][0], o, sizeof(o));
        if (strcmp(o, t[i][1])) { printf("FAIL %s -> %s (want %s)\n", t[i][0], o, t[i][1]); bad++; }
    }
    printf("%d failures\n", bad);
    return bad != 0;
}
#endif
