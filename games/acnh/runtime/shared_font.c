/* nn::pl shared system fonts for ACNH. The console's system fonts are not part of the game, so in order of preference:
     1. ACNH_FONT=<file> (.ttf / .otf / .ttc face 0 / .bfttf) for every type;
     2. a dump from the player's own console in ACNH_SHARED_FONTS (default user-data/fonts):
        nintendo_udsg-r_std_003.bfttf (Standard) and nintendo_ext_003.bfttf (NintendoExt button glyphs);
     3. Hiragino Sans W4 (a Japanese gothic, close to the console's UD Shin Go), then Arial.
   .bfttf is the console's obfuscated TTF: an 8-byte header, then every u32 XORed with a fixed key. A .ttc collection
   gives its face 0 as a standalone sfnt (the game's font engine wants one). Ported from acnh-recomp runtime/hle/pl.c,
   where ACNH 2.0.6 ran on these substitutes. */
#include "runtime.h"
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct { const uint8_t *addr; uint64_t size; } Font;
static Font fonts[2];   /* [0] Standard (every type), [1] NintendoExt when a dump provides it */
static pthread_once_t once = PTHREAD_ONCE_INIT;

static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }
static void put32(uint8_t *p, uint32_t v) { p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v; }

static uint8_t *read_all(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) || (long)(*len = (size_t)ftell(f)) < 0 || fseek(f, 0, SEEK_SET)) { fclose(f); return NULL; }
    uint8_t *p = malloc(*len + 4);
    if (p && fread(p, 1, *len, f) != *len) { free(p); p = NULL; }
    fclose(f);
    return p;
}

/* .bfttf -> TTF in place: the new length, 0 if it isn't one */
static size_t bfttf_decode(uint8_t *d, size_t len) {
    static const uint32_t KEY = 0x49621806;
    if (len < 12) return 0;
    for (int swap = 0; swap < 2; swap++) {   /* the key's byte order differs between descriptions; accept both */
        uint32_t k = swap ? __builtin_bswap32(KEY) : KEY, w1, w2;
        memcpy(&w1, d + 4, 4); memcpy(&w2, d + 8, 4);
        uint32_t size = w1 ^ k, ver = w2 ^ k;
        if (!size || size > len - 8 || (ver != 0x00000100u && ver != 0x4F54544Fu)) continue;   /* sfnt 1.0 / OTTO */
        for (size_t i = 0; i + 4 <= size; i += 4) { uint32_t w; memcpy(&w, d + 8 + i, 4); w ^= k; memcpy(d + i, &w, 4); }
        return size;
    }
    return 0;
}

/* .ttc -> face 0 as its own sfnt (table data copied, offsets rewritten) */
static uint8_t *ttc_face0(const uint8_t *f, size_t len, size_t *out_len) {
    if (len < 16 || memcmp(f, "ttcf", 4)) return NULL;
    uint32_t o = be32(f + 12);
    if ((size_t)o + 12 > len) return NULL;
    uint32_t nt = (uint32_t)f[o + 4] << 8 | f[o + 5];
    if ((size_t)o + 12 + 16 * (size_t)nt > len) return NULL;
    size_t total = 12 + 16 * (size_t)nt;
    for (uint32_t i = 0; i < nt; i++) total += (be32(f + o + 12 + 16 * i + 12) + 3) & ~3u;
    uint8_t *out = calloc(1, total);
    if (!out) return NULL;
    memcpy(out, f + o, 12);
    size_t pos = 12 + 16 * (size_t)nt;
    for (uint32_t i = 0; i < nt; i++) {
        const uint8_t *e = f + o + 12 + 16 * i;
        uint32_t off = be32(e + 8), n = be32(e + 12);
        if ((size_t)off + n > len) { free(out); return NULL; }
        memcpy(out + 12 + 16 * i, e, 8);   /* tag, checksum */
        put32(out + 12 + 16 * i + 8, (uint32_t)pos);
        put32(out + 12 + 16 * i + 12, n);
        memcpy(out + pos, f + off, n);
        pos += (n + 3) & ~3u;
    }
    *out_len = total;
    return out;
}

static bool load_one(const char *path, Font *ft) {
    if (!path || !*path) return false;
    size_t len;
    uint8_t *d = read_all(path, &len);
    if (!d) return false;
    size_t n = bfttf_decode(d, len);
    if (n) len = n;
    else if (!memcmp(d, "ttcf", 4)) {
        size_t flen; uint8_t *face = ttc_face0(d, len, &flen);
        free(d);
        if (!face) { fprintf(stderr, "[acnh] %s: bad font collection\n", path); return false; }
        d = face; len = flen;
    }
    ft->addr = d; ft->size = len;
    fprintf(stderr, "[acnh] system font %s (%zu bytes)\n", path, len);
    return true;
}

static void load(void) {
    const char *dir = getenv("ACNH_SHARED_FONTS");
    if (!dir || !*dir) dir = "user-data/fonts";
    char std_dump[4096], ext_dump[4096];
    snprintf(std_dump, sizeof std_dump, "%s/nintendo_udsg-r_std_003.bfttf", dir);
    snprintf(ext_dump, sizeof ext_dump, "%s/nintendo_ext_003.bfttf", dir);
    const char *standard[] = {getenv("ACNH_FONT"), std_dump, "/System/Library/Fonts/ヒラギノ角ゴシック W4.ttc",
                              "/System/Library/Fonts/Supplemental/Arial.ttf"};
    for (size_t i = 0; i < sizeof standard / sizeof *standard && !fonts[0].addr; i++) load_one(standard[i], &fonts[0]);
    if (!fonts[0].addr) runtime_fail("no usable system font (set ACNH_FONT)");
    load_one(ext_dump, &fonts[1]);
}

/* SharedFontType: 0 Standard, 1 ChineseSimple, 2 ChineseSimpleExt, 3 ChineseTraditional, 4 Korean, 5 NintendoExt */
static const Font *font_for(int type) {
    if (type < 0 || type > 5) runtime_fail("invalid shared font type");
    pthread_once(&once, load);
    return type == 5 && fonts[1].addr ? &fonts[1] : &fonts[0];
}
static int load_state(int type) { font_for(type); return 1; }   /* SharedFontLoadState_Loaded */
static const void *address(int type) { return font_for(type)->addr; }
static size_t size(int type) { return (size_t)font_for(type)->size; }
static void request_load(int type) { font_for(type); }
/* bool GetSharedFont(int *count, SharedFontData *out, int max, LanguageCode): the fonts for a language in priority
   order; SharedFontData is {u32 type, const void *address, size_t size} (0x18 bytes). */
static bool shared_font(int32_t *count, uint8_t *out, int max, uint64_t language) {
    (void)language;
    if (!count || (!out && max > 0)) runtime_fail("GetSharedFont output pointer");
    static const uint32_t order[] = {0, 5};   /* Standard, then NintendoExt (button glyphs) */
    int n = 0;
    for (; n < 2 && n < max; n++) {
        const Font *f = font_for((int)order[n]);
        uint8_t *e = out + 0x18 * n;
        memset(e, 0, 0x18);
        memcpy(e, &order[n], 4); memcpy(e + 8, &f->addr, 8); memcpy(e + 0x10, &f->size, 8);
    }
    *count = 2;
    return true;
}
void *runtime_font_override(const char *name) {
    static const struct { const char *name; void *fn; } entries[] = {
        {"_ZN2nn2pl22GetSharedFontLoadStateENS0_14SharedFontTypeE", load_state},
        {"_ZN2nn2pl20GetSharedFontAddressENS0_14SharedFontTypeE", address},
        {"_ZN2nn2pl17GetSharedFontSizeENS0_14SharedFontTypeE", size},
        {"_ZN2nn2pl21RequestSharedFontLoadENS0_14SharedFontTypeE", request_load},
        {"_ZN2nn2pl13GetSharedFontEPiPNS0_14SharedFontDataEiNS_8settings12LanguageCodeE", shared_font}};
    for (unsigned i = 0; i < sizeof entries / sizeof *entries; i++) if (!strcmp(name, entries[i].name)) return entries[i].fn;
    return NULL;
}
