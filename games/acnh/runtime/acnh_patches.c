#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>
#include <strings.h>
#include <sys/stat.h>
/* ACNH game-code patches: applied to main after relocation, before its text is sealed. Each site checks the original
   instruction first (aspect_patch.h checks that it still produces a float ratio). */
#include "game_patches.h"
#include "aspect_patch.h"
#include "game_version.h"
#include "coop.h"

/* Camera aspect ratio (aspect_patch.h: the picture follows the window's shape, key 4 toggles 16:9).
   ACNH uses sead's PerspectiveProjection (2.0.6: default ctor main+0x271f5e0, ctor(near, far, fovy, aspect) main+0x271f6f0;
   aspect at +0xb0, TOTK's layout). Unlike TOTK, most of its projections are made on the fly (the ctor runs about 850
   times a second, reached through function pointers), and the cameras take their aspect from a viewport rectangle; so
   the ratio is not changed where it is produced but where sead reads it: the perspective matrix, the near-plane size and
   getAspect, which the game's culling and screen picking use (3.0.0: main+0x37e3e28, +0x37e3ef4, +0x37e3e0c; 2.0.6 had the
   same code at main+0x271f8a0 / +0x271f970 / +0x271f870, with the matrix's registers allocated differently).
   Only the game's own screen ratio is replaced: a stored aspect of exactly 16:9 (0x3fe38e39, what 1920/1080 and
   1280/720 give) becomes the window's, while projections with other ratios (the 3D model views in menus, render
   targets) keep theirs. ACNH draws its UI through such a 16:9 perspective too, so the UI follows the same rule: wider
   than 16:9, the view widens and the UI ends up in the middle 16:9 of the window; narrower (16:10, 4:3), the view keeps
   its width and grows taller (the near-plane height, +0xac, scaled by (16/9) / ratio), so neither the world's sides
   nor the UI are cut off.
   Each read branches to an 80-byte stub in main's text padding:
       <the read>                     ldp s<h>, s<d> / ldr s<d> that loads the aspect into s<d>
       stp x10, x11, [sp, #-16]!
       adrp x10, <data page>;  ldr w11, [x10, #60]    the 16:9 bits (written by the host)
       fmov w10, s<d>;  sub w11, w11, w10;  cbnz w11, out
       adrp x10, <data page>;  ldr w11, [x10, #52];  ldr w10, [x10, #56]    vertical scale, ratio (0 = off)
       cbz w10, out
       cbz w11, set;  fmov s<d>, w11;  fmul s<h>, s<h>, s<d>     (ldp sites only: the height scaled)
   set: fmov s<d>, w10
   out: ldp x10, x11, [sp], #16;  b <after the read>
   Only x10 / x11 (restored) and no flags are touched. iPhone / iPad: written by the iOS export, checked on the device. */
typedef struct { uint64_t offset; uint32_t original; unsigned reg; int height; const char *what; } AspectRead;   /* height: s<h>, -1 none */
static const AspectRead aspect_reads[] = {   /* ACNH 3.0.0 */
    {0x37e3e34, 0x2d558801, 2, 1, "perspective matrix: ldp s1, s2, [x0, #0xac]"},
    {0x37e3f3c, 0x2d558c02, 3, 2, "near-plane size: ldp s2, s3, [x0, #0xac]"},
    {0x37e3e0c, 0xbd40b000, 0, -1, "getAspect: ldr s0, [x0, #0xb0]"},
};
#define ASPECT_READ_STUB 80
#define ASPECT_VSCALE_SLOT 52   /* data page: the vertical scale (float, 0 = none) */
#define ASPECT_16_9_SLOT 60   /* data page: the 16:9 float bits the stubs compare with */
#define ASPECT_16_9_BITS 0x3fe38e39u

static uint32_t adrp_x10(uint64_t at, uint64_t page_off) {
    int64_t pages = (int64_t)(page_off >> 12) - (int64_t)(at >> 12);   /* the module base is page aligned */
    if (pages < -(1 << 20) || pages >= (1 << 20)) runtime_fail("aspect patch: data page out of ADRP range");
    uint32_t imm = (uint32_t)pages & 0x1fffff;
    return 0x90000000u | ((imm & 3) << 29) | ((imm >> 2) << 5) | 10;
}
static void aspect_install_reads(Module *m, uint64_t stub_off, uint64_t data_off) {
    const unsigned n = sizeof aspect_reads / sizeof *aspect_reads;
    g_aspect_wide = aspect_requested();
    if (stub_off < m->text_end || stub_off + ASPECT_READ_STUB * n > m->ro_off) runtime_fail("aspect patch: no room in main's text padding");
    if (data_off + 64 > m->size) runtime_fail("aspect patch: no data page after main");
    for (unsigned i = 0; i < n; i++) {
        const AspectRead *r = &aspect_reads[i]; uint64_t s = stub_off + ASPECT_READ_STUB * i;
        uint32_t stub[20] = {0}; unsigned k = 0, d = r->reg;
        stub[k++] = r->original;
        stub[k++] = 0xa9bf2fea;                                                          /* stp x10, x11, [sp, #-16]! */
        stub[k] = adrp_x10(s + 4 * k, data_off); k++;
        stub[k++] = 0xb9400000u | ((ASPECT_16_9_SLOT / 4) << 10) | (10 << 5) | 11;     /* ldr w11, [x10, #60] */
        stub[k++] = 0x1e260000u | (d << 5) | 10;                                         /* fmov w10, s<d> */
        stub[k++] = 0x4b0a016b;                                                          /* sub w11, w11, w10 */
        unsigned cbnz_at = k++;
        stub[k] = adrp_x10(s + 4 * k, data_off); k++;
        stub[k++] = 0xb9400000u | ((ASPECT_VSCALE_SLOT / 4) << 10) | (10 << 5) | 11;   /* ldr w11, [x10, #52] */
        stub[k++] = 0xb9400000u | ((ASPECT_DATA_SLOT / 4) << 10) | (10 << 5) | 10;     /* ldr w10, [x10, #56] */
        unsigned cbz_at = k++;
        if (r->height >= 0) {
            stub[k++] = 0x34000000u | (3 << 5) | 11;                                     /* cbz w11, set */
            stub[k++] = 0x1e270000u | (11 << 5) | d;                                     /* fmov s<d>, w11 */
            stub[k++] = 0x1e200800u | (d << 16) | ((unsigned)r->height << 5) | (unsigned)r->height;   /* fmul s<h>, s<h>, s<d> */
        }
        stub[k++] = 0x1e270140u | d;                                                     /* set: fmov s<d>, w10 */
        unsigned out = k;
        stub[k++] = 0xa8c12fea;                                                          /* out: ldp x10, x11, [sp], #16 */
        stub[k] = aspect_branch(s + 4 * k, r->offset + 4); k++;
        stub[cbnz_at] = 0x35000000u | ((out - cbnz_at) << 5) | 11;                       /* cbnz w11, out */
        stub[cbz_at] = 0x34000000u | ((out - cbz_at) << 5) | 10;                         /* cbz w10, out */
        uint32_t jump = aspect_branch(r->offset, s), have; memcpy(&have, m->base + r->offset, 4);
#if TARGET_OS_IPHONE
        if (have == r->original) { fprintf(stderr, "[acnh] aspect: not in this build's code (16:9 only)\n"); return; }
        if (have != jump || memcmp(m->base + s, stub, sizeof stub)) runtime_fail("aspect patch: embedded stub does not match");
#else
        if (have != r->original) {
            fprintf(stderr, "[acnh] aspect main+%#llx: found %08x, expected %08x (%s)\n", (unsigned long long)r->offset, have, r->original, r->what);
            runtime_fail("aspect patch: the camera code does not match ACNH 3.0.0");
        }
        for (unsigned b = 0; b < ASPECT_READ_STUB; b++) if (m->base[s + b]) runtime_fail("aspect patch: text padding in use");
        memcpy(m->base + s, stub, sizeof stub); memcpy(m->base + r->offset, &jump, 4);
#endif
    }
    *(volatile uint32_t *)(void *)(m->base + data_off + ASPECT_16_9_SLOT) = ASPECT_16_9_BITS;
    g_aspect_vslot = (_Atomic(uint32_t) *)(void *)(m->base + data_off + ASPECT_VSCALE_SLOT);
    g_aspect_slot = (_Atomic(uint32_t) *)(void *)(m->base + data_off + ASPECT_DATA_SLOT);
    atomic_store(&g_aspect_on, !aspect_starts_off()); aspect_apply();
    char fixed[32]; snprintf(fixed, sizeof fixed, "%.4f", g_aspect_wide);
    fprintf(stderr, "[acnh] aspect: key 4 switches 16:9 / %s (TOTK_ASPECT), starts %s; %u projection read(s), stubs +%#llx\n",
            g_aspect_wide ? fixed : "the window's shape", atomic_load(&g_aspect_on) ? "on" : "at 16:9", n, (unsigned long long)stub_off);
}
#define ASPECT_STUBS_END(m) ((((m)->text_end + 15) & ~15ULL) + ASPECT_READ_STUB * (sizeof aspect_reads / sizeof *aspect_reads))

/* 60 FPS at the normal game speed: the community "ACNH 60 FPS + SpeedFix" mod (tmouh, github.com/tmouh/acnh-60fps,
   CC BY-NC 4.0; not in this repository: scripts/speedfix.py fetches one pinned commit into tooling/acnh-60fps and builds
   its RomFS overlay). ACNH steps its world once per frame with amounts tuned for 30 updates a second and has no global
   speed setting, so the mod rescales 6,417 words of main (speeds x1/2, accelerations x1/4, update counts x2, ...) and
   1,544 parameters in RomFS. Its patch list (speedfix_main_300.pchtxt) names main's build ID ("@nsobid-"), and its
   "<offset> <4 bytes>" lines are main+offset ("@flag offset_shift 0x100" is the step to NSO file offsets, which a loader
   of the memory image does not take). It is applied when the folder has it (ACNH_SPEEDFIX_DIR, default
   tooling/acnh-60fps; ACNH_SPEEDFIX=0 leaves it out): the build ID must be main's and every word must lie in main's code,
   read-only data or data. Checked on 3.0.0 against this port: none of the 6,417 words is an instruction the rewriter
   changed, a relocated pointer or an aspect site, and none of the mod's code uses x18, system registers or svc.
   The mod's SDK "enabler" presents a frame at every vblank inside NVN, which runs in the host here: the host presents
   one game frame per 60 Hz tick instead (DM_PRESENT_INTERVAL=display). Below 60 FPS the game slows down in step, as
   with the mod. iPhone / iPad: the code words were applied by the iOS export (checked here); data words are written. */
static int speedfix_apply(Module *m) {
    const char *off = getenv("ACNH_SPEEDFIX"); if (off && !strcmp(off, "0")) { fprintf(stderr, "[acnh] 60 FPS SpeedFix off (ACNH_SPEEDFIX=0)\n"); return 0; }
    const char *dir = getenv("ACNH_SPEEDFIX_DIR"); if (!dir || !*dir) dir = "tooling/acnh-60fps";
    char path[2048]; snprintf(path, sizeof path, "%s/speedfix_main_300.pchtxt", dir);
    FILE *f = fopen(path, "r");
    if (!f) { fprintf(stderr, "[acnh] 30 FPS (no SpeedFix in %s: python3 scripts/speedfix.py)\n", dir); return 0; }
    char line[512]; int id_ok = 0, shift_ok = 0, enabled = 0; unsigned words = 0, code = 0;
    while (fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = 0;
        if (!strncmp(line, "@nsobid-", 8)) { id_ok = !strncasecmp(line + 8, ACNH_MAIN_BUILD_ID, strlen(ACNH_MAIN_BUILD_ID)); if (!id_ok) break; continue; }
        if (!strcmp(line, "@flag offset_shift 0x100")) { shift_ok = 1; continue; }
        if (!strcmp(line, "@enabled")) { enabled = 1; continue; }
        if (!strcmp(line, "@disabled")) { enabled = 0; continue; }
        if (!strcmp(line, "@stop")) break;
        if (line[0] == '@' || line[0] == '/' || !line[0]) continue;
        unsigned long long o; char hex[16]; int used = 0;
        if (sscanf(line, "%8llx %8[0-9A-Fa-f]%n", &o, hex, &used) != 2 || strlen(hex) != 8 || (line[used] && line[used] != ' '))
            { fprintf(stderr, "[acnh] SpeedFix: unreadable line \"%s\"\n", line); runtime_fail("SpeedFix patch list is not the expected format"); }
        if (!enabled) continue;
        if (!id_ok || !shift_ok) runtime_fail("SpeedFix patch list: no matching @nsobid / offset_shift 0x100 before its words");
        if (o + 4 > m->file_size || (o >= m->text_end && o < m->ro_off)) runtime_fail("SpeedFix patch outside main's code and data");
        uint8_t w[4]; for (int i = 0; i < 4; i++) { unsigned b; sscanf(hex + 2 * i, "%2x", &b); w[i] = (uint8_t)b; }
        if (o < m->text_end) {
            code++;
#if TARGET_OS_IPHONE
            if (memcmp(m->base + o, w, 4)) { fprintf(stderr, "[acnh] SpeedFix main+%#llx not in this build's code\n", o); runtime_fail("SpeedFix: the app was exported without it"); }
            words++; continue;
#endif
        }
        memcpy(m->base + o, w, 4); words++;
    }
    fclose(f);
    if (!id_ok) { fprintf(stderr, "[acnh] SpeedFix %s is for another main (build ID); 30 FPS\n", path); return 0; }
    if (!words) runtime_fail("SpeedFix patch list has no words");
    char ov[2100]; snprintf(ov, sizeof ov, "%s/romfs", dir);
    struct stat st; if (stat(ov, &st) != 0 || !S_ISDIR(st.st_mode)) runtime_fail("SpeedFix: its RomFS files are missing (python3 scripts/speedfix.py)");
    setenv("ACNH_ROMFS_OVERLAY", ov, 0);
    setenv("DM_PRESENT_INTERVAL", "display", 0);   /* one game frame per 60 Hz tick: the enabler */
    fprintf(stderr, "[acnh] 60 FPS SpeedFix: %u words (%u code, %u data) from %s, RomFS overlay %s\n", words, code, words - code, path, ov);
    return 1;
}

/* ACNH_SITE_PROBE=<hex>,<hex>,... (Mac, diagnostics): counts how often each main+<hex> instruction runs and logs the
   counts every 5 s. Each site branches to a stub in the text padding after the aspect stubs that bumps a counter on the
   data page after main (+64 + 4 * i), runs the instruction (it must not be PC-relative) and branches back. */
#define PROBE_MAX 24
static volatile uint32_t *probe_counts; static uint64_t probe_sites[PROBE_MAX]; static unsigned probe_n;
static void *probe_log(void *unused) {
    (void)unused;
    for (;;) {
        sleep(5); char line[1024]; int n = snprintf(line, sizeof line, "[acnh] site probe:");
        for (unsigned i = 0; i < probe_n; i++) n += snprintf(line + n, sizeof line - n, " +%#llx=%u", (unsigned long long)probe_sites[i], probe_counts[i]);
        fprintf(stderr, "%s\n", line);
    }
    return NULL;
}
static void site_probe(Module *m, uint64_t stub_off, uint64_t data_off) {
    const char *e = getenv("ACNH_SITE_PROBE"); if (!e || !*e) return;
    for (const char *p = e; *p && probe_n < PROBE_MAX; ) { char *end; probe_sites[probe_n++] = strtoull(p, &end, 16); p = *end == ',' ? end + 1 : end; if (end == p && *p) break; }
    uint64_t page_end = (m->text_end + 0x3fff) & ~0x3fffULL;
    for (unsigned i = 0; i < probe_n; i++) {
        uint64_t s = stub_off + 32 * i, site = probe_sites[i];
        if (s + 32 > page_end) runtime_fail("ACNH_SITE_PROBE: no room in the text padding");
        for (unsigned b = 0; b < 32; b++) if (m->base[s + b]) runtime_fail("ACNH_SITE_PROBE: text padding in use");
        uint32_t ins; memcpy(&ins, m->base + site, 4);
        int64_t pages = (int64_t)(data_off >> 12) - (int64_t)((s + 4) >> 12); uint32_t imm = (uint32_t)pages & 0x1fffff;
        uint32_t slot = (64 + 4 * i) / 4;
        uint32_t stub[8] = {0xa9bf2fea,                                                   /* stp x10, x11, [sp, #-16]! */
                            0x90000000u | ((imm & 3) << 29) | ((imm >> 2) << 5) | 10,    /* adrp x10, <data page> */
                            0xb940014bu | (slot << 10),                                   /* ldr w11, [x10, #slot] */
                            0x1100056b,                                                   /* add w11, w11, #1 */
                            0xb900014bu | (slot << 10),                                   /* str w11, [x10, #slot] */
                            0xa8c12fea,                                                   /* ldp x10, x11, [sp], #16 */
                            ins, aspect_branch(s + 28, site + 4)};
        memcpy(m->base + s, stub, sizeof stub);
        uint32_t jump = aspect_branch(site, s); memcpy(m->base + site, &jump, 4);
        /* ACNH_TRACE_SITE=<hex> (one of the probe sites): the site becomes brk #0x7a; loader.c's signal handler prints
           the guest call chain and x0-x3 (first 12 hits), then resumes in this stub (which runs the instruction). */
        const char *t = getenv("ACNH_TRACE_SITE");
        if (t && strtoull(t, NULL, 16) == site) {
            extern uint64_t runtime_trace_site_pc, runtime_trace_site_stub;
            runtime_trace_site_pc = (uintptr_t)m->base + site; runtime_trace_site_stub = (uintptr_t)m->base + s;
            uint32_t brk = 0xd4200f40; memcpy(m->base + site, &brk, 4);
            fprintf(stderr, "[acnh] trace site main+%#llx armed\n", (unsigned long long)site);
        }
    }
    probe_counts = (volatile uint32_t *)(void *)(m->base + data_off + 64);
    pthread_t t; pthread_create(&t, NULL, probe_log, NULL); pthread_detach(t);
    fprintf(stderr, "[acnh] site probe on %u instruction(s)\n", probe_n);
}

/* Sizes (3.0.0). ACNH has its own size table, main+0x4075708: {w, h} floats for 540p, 720p, 900p, 1080p, 1440p and 2160p.
   main+0x1527acc (a virtual function, run at start and when the operation mode changes) picks entry 3 docked and 1
   handheld and stores the index twice:
     main+0x1527af4  csinc w9, w9, wzr, ne        (index = docked ? 3 : 1)
     main+0x1527af8  str   w9, [x8, #0x2e0]       the scene's render size: the 3D scene is drawn at it, then scaled to the
                                                 canvas (the function also writes that scale, size / canvas, for the renderer)
     main+0x1527afc  str   w9, [x10, #0x2e4]      the output: the canvas the UI and the scene are composed in, the window crop
   Each of the two can be replaced through a 4-byte slot on the data page after main (index + 1, 0 = the game's own); the
   instruction branches to a stub in main's text padding (after the aspect stubs) that loads it. x12 is free there (a leaf
   function); x8, x10 and x11 are live.
     output (Mac):  csinc w9, w9, wzr, ne; adrp x12, <data>; ldr w12, [x12, #OUTPUT_SLOT]; cbz w12, 1f; sub w9, w12, #1; 1: b +0x1527af8
     render:        adrp x12, <data>; ldr w12, [x12, #RENDER_SLOT]; cbnz w12, 1f; add w12, w9, #1; 1: sub w12, w12, #1;
                    str w12, [x8, #0x2e0]; b +0x1527afc
   The output stub keeps w9, so the render size follows the output unless the render slot says otherwise. */
#define SIZE_TABLE 0x4075708u
#define OUTPUT_SITE 0x1527af4u
#define RENDER_SITE 0x1527af8u
#define OUTPUT_SLOT 120
#define RENDER_SLOT 124
#define SIZE_STUBS 64   /* bytes after the aspect stubs: the render stub (+0), the output stub (+32) */
static _Atomic(uint32_t) *output_slot;   /* set once the output patch is in (Mac) */
static const unsigned game_sizes[6][2] = {{960, 540}, {1280, 720}, {1600, 900}, {1920, 1080}, {2560, 1440}, {3840, 2160}};
static uint32_t adrp_x12(uint64_t at, uint64_t page_off) { return (adrp_x10(at, page_off) & ~31u) | 12; }

/* Render resolution (TOTK_RENDER_RES=<w>x<h>, the iPhone / iPad Settings sheet's Render resolution; read at start): the
   3D scene's size, the UI keeps the output's. A size of the table is its index; another (480p) replaces entry 0 (540p,
   which no output mode uses) and takes index 0. The stub is always in main (harmless with the slot at 0), so the iOS
   export embeds it; the device checks it and writes the slot. Unset = the game's own. */
static void render_install(Module *m, uint64_t s, uint64_t data_off) {
    uint32_t stub[7] = {adrp_x12(s, data_off), 0xb9400000u | ((RENDER_SLOT / 4) << 10) | (12 << 5) | 12, 0x35000000u | (2 << 5) | 12,
                        0x1100052cu /* add w12, w9, #1 */, 0x5100058cu /* sub w12, w12, #1 */, 0xb902e10cu /* str w12, [x8, #0x2e0] */,
                        aspect_branch(s + 24, RENDER_SITE + 4)};
    uint32_t jump = aspect_branch(RENDER_SITE, s), have; memcpy(&have, m->base + RENDER_SITE, 4);
#if TARGET_OS_IPHONE
    if (have == 0xb902e109u) { fprintf(stderr, "[acnh] render resolution: the game's own (not in this build's code)\n"); return; }
    if (have != jump || memcmp(m->base + s, stub, sizeof stub)) runtime_fail("render resolution: embedded stub does not match");
#else
    if (have != 0xb902e109u) runtime_fail("render resolution patch: main does not match 3.0.0 (main+0x1527af8)");
    for (unsigned b = 0; b < sizeof stub; b++) if (m->base[s + b]) runtime_fail("render resolution patch: text padding in use");
    memcpy(m->base + s, stub, sizeof stub); memcpy(m->base + RENDER_SITE, &jump, 4);
#endif
    const char *e = getenv("TOTK_RENDER_RES"); unsigned w, h; char rest;
    if (!e || !*e || !strcmp(e, "0")) return;
    if (sscanf(e, "%ux%u%c", &w, &h, &rest) != 2 || w < 64 || h < 64 || w > 3840 || h > 2160) runtime_fail("TOTK_RENDER_RES must be <width>x<height>");
    int index = -1;
    for (int i = 0; i < 6; i++) if (game_sizes[i][0] == w && game_sizes[i][1] == h) index = i;
    if (index < 0) { float v[2] = {(float)w, (float)h}; memcpy(m->base + SIZE_TABLE, v, sizeof v); index = 0; }
    *(volatile uint32_t *)(void *)(m->base + data_off + RENDER_SLOT) = (uint32_t)index + 1;
    fprintf(stderr, "[acnh] render resolution: scene %ux%u (size index %d), stub +%#llx\n", w, h, index, (unsigned long long)s);
}

/* Output size (Mac: key 3 and the Settings window's Picture, while the game runs). The output index is the game's own
   switch between docked 1080p and handheld 720p, so it works for every size of the table up to the memory made for it.
   The host writes the slot and posts "operation mode changed" (oe message 30; the mode stays docked): the game runs
   main+0x1527acc again and lays out its canvas, UI and crop at the new size (checked 1080p -> 720p -> 900p -> 1080p).
   Sizes above 1080p need room made at start, once, for the largest one (OUTPUT_W x OUTPUT_H):
     main+0x5cdcc0  mov x8, #0x780; movk x8, #0x438, lsl #32   the framework's display size: the swapchain (3 textures,
                                                               +18.5 MiB at 1440p, from root heap "cSys")
     main+0x404c638 0xa8df000                                  cSys, the first of ACNH's six root heaps (cSys, cGfx, cRes,
                                                               cSnd, cScene, then cOthers = the rest); 10 MiB were left: +48 MiB
     main+0x5c9b90  mov w10, #0x4c00000                        gsys's two render-target pools (76 MiB each, from root heap
                                                               "cGfx"): 128 MiB, else a 1440p target gets no memory (null
                                                               texture, main+0x3847ca0)
     main+0x404c640 0x9900000                                  cGfx: + what the two pools grow by
   The application pool grows by what this takes (runtime_output_res_extra_bytes, services.c), so cOthers, which holds
   AppStaticHeap (2016 MiB), keeps its size. 4K ran out of render-target memory even with 448 MiB pools: 1440p is the top.
   ACNH_OUTPUT_RES=0 keeps the game's own 1080p (Picture: 1080p / MetalFX). Not on iOS and not in the iOS export. */
#define OUTPUT_W 2560u
#define OUTPUT_H 1440u
#define CSYS_EXTRA (48ull << 20)
#define RT_POOL (128u << 20)
#define RT_POOL_GAME (76u << 20)
#define CGFX_EXTRA (2ull * (RT_POOL - RT_POOL_GAME))
static int output_enabled(void) {
    static int v = -1;
    if (v < 0) { const char *e = getenv("ACNH_OUTPUT_RES"); v = !TARGET_OS_IPHONE && !getenv("TOTK_EXPORT_IOS") && !(e && !strcmp(e, "0")); }
    return v;
}
int runtime_output_res(unsigned *w, unsigned *h) { if (!output_enabled()) return 0; *w = OUTPUT_W; *h = OUTPUT_H; return 1; }
uint64_t runtime_output_res_extra_bytes(void) { return output_enabled() ? CSYS_EXTRA + CGFX_EXTRA : 0; }
#if !TARGET_OS_IPHONE
static void output_install(Module *m, uint64_t s, uint64_t data_off) {
    if (!output_enabled()) { fprintf(stderr, "[acnh] output: the game's own 1920x1080 (ACNH_OUTPUT_RES=0 or iOS export)\n"); return; }
    uint32_t w[4]; uint64_t roots[2];
    memcpy(w, m->base + OUTPUT_SITE, 4); memcpy(w + 1, m->base + 0x5cdcc0, 8); memcpy(w + 3, m->base + 0x5c9b90, 4); memcpy(roots, m->base + 0x404c638, 16);
    if (w[0] != 0x1a9f1529u || w[1] != 0xd280f008u || w[2] != 0xf2c08708u || w[3] != 0x52a0980au || roots[0] != 0xa8df000 || roots[1] != 0x9900000)
        runtime_fail("output patch: main does not match 3.0.0");
    uint32_t stub[6] = {0x1a9f1529u, adrp_x12(s + 4, data_off), 0xb9400000u | ((OUTPUT_SLOT / 4) << 10) | (12 << 5) | 12,
                        0x34000000u | (2 << 5) | 12, 0x51000589u /* sub w9, w12, #1 */, aspect_branch(s + 20, OUTPUT_SITE + 4)};
    for (unsigned b = 0; b < sizeof stub; b++) if (m->base[s + b]) runtime_fail("output patch: text padding in use");
    memcpy(m->base + s, stub, sizeof stub);
    uint32_t jump = aspect_branch(OUTPUT_SITE, s); memcpy(m->base + OUTPUT_SITE, &jump, 4);
    uint32_t swap[2] = {0xd2800008u | (OUTPUT_W << 5), 0xf2c00008u | (OUTPUT_H << 5)}; memcpy(m->base + 0x5cdcc0, swap, 8);
    uint32_t pool = 0x52a0000au | ((RT_POOL >> 16) << 5); memcpy(m->base + 0x5c9b90, &pool, 4);
    roots[0] += CSYS_EXTRA; roots[1] += CGFX_EXTRA; memcpy(m->base + 0x404c638, roots, 16);
    output_slot = (_Atomic(uint32_t) *)(void *)(m->base + data_off + OUTPUT_SLOT);
    fprintf(stderr, "[acnh] output: swapchain %ux%u, starts at the game's 1920x1080; key 3 / Picture switch sizes up to it; stub +%#llx\n",
            OUTPUT_W, OUTPUT_H, (unsigned long long)s);
}
#endif

/* Local play editing (docs/LAN-COOP-DESIGN.md, ACNH_MP_EDIT). Each row is checked against the 3.0.0 word first.
   "furniture": main+0x21cc50c decides whether a furniture operation is allowed (8 callers, indoor and outdoor): 1 =
   someone else's home during a session, 2 = a special state, 3 / 4 = another player is near or in the room
   (SYS_Telepathy 005 "It's rude to redecorate when you have company!"), 0 = allowed. */
static const struct { uint64_t off; uint32_t original, patched; const char *group; } mp_edit_sites[] = {
    /* Only 3 and 4 (another player near or in the room) are lifted. 1 stays: a visitor who could grab and turn the
       host's furniture crashed (main+0x3847ca0 reads an object only the island's owner has), so that needs more work. */
    /* All of it (0 = allowed), for host and visitor. A visitor holding the owner's furniture allocates a half-resolution
       "ExtendGBuffer" render target (main+0x1458fd0 -> 0x38472d0 -> pool search main+0x3865630); with the game's own
       render-target pools that search fails and main+0x3847ca0 uses the null result (crash), so this needs the output
       patch's larger pools (the default on the Mac; ACNH_OUTPUT_RES=0 turns them off). Turning or picking up the
       owner's furniture still does not work for a visitor (LAN-COOP-DESIGN.md 8.10). */
    {0x21cc50c, 0xa9bc7bfd, 0x52800000, "furniture"},   /* stp x29, x30, [sp, #-0x40]!  ->  mov w0, #0 */
    {0x21cc510, 0xf9000bf7, 0xd65f03c0, "furniture"},   /* str x23, [sp, #0x10]         ->  ret */
    /* main+0x3353540: may an item from the pockets be placed here? Returns 0 or a reason the pocket menu turns into a
       message: 0x10 indoors with 2+ players in the session (SYS_Telepathy 005 "It's rude to redecorate when you have
       company!"), 0x11 outdoors (012 "There's company over right now"). Found with the "watch" diagnostic on the pocket
       menu's message field (written at main+0x3073c84). The three branches below skip only the player-count tests. */
    {0x3353818, 0x54001183, 0x1400008c, "furniture"},   /* b.lo +0x230 (outdoors, < 2 players: allowed) -> b */
    {0x3353960, 0x54000122, 0x14000025, "furniture"},   /* b.hs 0x3353984 (2+ players: indoor block)    -> b 0x33539f4 */
    {0x3353980, 0x540003a9, 0x1400001d, "furniture"},   /* b.ls 0x33539f4 (<= 1 player: allowed)        -> b */
    /* main+0x307cf70 builds the pocket item submenu; on another island it leaves out "Place Item" (CmnSubWin 0111)
       after asking main+0x2567d40, "is this player on their own island?". These three calls answer yes (this menu only). */
    {0x307d9ec, 0x97d3a8d5, 0x52800020, "furniture"},   /* bl IsInMyVillage -> mov w0, #1 */
    {0x307e5a0, 0x97d3a5e8, 0x52800020, "furniture"},   /* bl IsInMyVillage -> mov w0, #1 */
    {0x307e918, 0x97d3a50a, 0x52800020, "furniture"},   /* bl IsInMyVillage -> mov w0, #1 */
    /* Visitor turns, pushes, pulls and Y pick-ups of the owner's furniture (LAN-COOP-DESIGN.md 8.14, 8.15). Each
       furniture action first starts a job of the field-action system (*(main+0x50a8970): a visitor's job asks the
       host, which grants unless the furniture is locked). Two IsInMyVillage(player) tests stop a visitor's copy from
       starting the job at all; both are true on the island's own copy, so these change nothing for the host.
       main+0x1412c8c (placement check, via main+0x1375040; used by turn / push / pull): on a map with flag bit 3 it
       requires IsInMyVillage. main+0x140ea70 (may this item be picked up; used by Y): not in my village and a
       furniture-class item answers no. */
    {0x14130ec, 0x36002160, 0xd503201f, "visitor-turn"},   /* tbz w0, #0, 0x1413518 (not my village: fail) -> nop */
    {0x140efbc, 0x37000080, 0x14000004, "visitor-turn"},   /* tbnz w0, #0, 0x140efcc (my village: go on) -> b */
    /* A pushed item's move state (item state 5: enter main+0x13ab3a0, run main+0x13ab740) follows the pushing player
       while that player is in a pull / push / turn state. When the pusher is not (yet) in one: with item+0xab0 set it
       waits, giving up after 8 frames of the item's state; with it clear it ends the move at once (the item jumps). The
       flag was set only for a pusher that is unknown or not a resident. On the other copies the replayed pusher enters
       the push state about 0.25 s after the move starts, so: always set the flag, and wait up to 60 frames (2 s). With
       the push-anim stub (pusher filled in) the item then slides on every copy (LAN-COOP-DESIGN.md 8.16). */
    {0x13ab684, 0x54000500, 0x14000026, "push-anim"},   /* b.eq 0x13ab724 (skip setting the flag) -> b 0x13ab71c (set it) */
    {0x13aba04, 0x7100211f, 0x7100f11f, "push-anim"},   /* cmp w8, #8 -> cmp w8, #60 */
    /* Houses (group "house"; LAN-COOP-DESIGN.md 8.17). Found by two read-only agents from the decompiled code.
       main+0x2813cd8 is "is this the player's own house" (house interior stage, room's house index == the player's); the
       two rules already patched for outdoors ask it right after IsInMyVillage, and main+0x32f82c0 "this house belongs to
       someone else" stops placing from pockets. Each is true / false as needed on the owner's copy, so only a visitor
       changes. The d-pad selector main+0x22676e4 drops decorating mode (action 0xef) and, probably, storage (0xf0) unless
       this copy is the session's host (station +0x473 == host station +0x472); main+0x2df5480 has another such test.
       The company refusals in main+0x2df6328 (codes 4 and 0x87) are lifted below, together with the fix for the freeze
       they used to cause. */
    {0x141317c, 0x36001ce0, 0xd503201f, "house"},   /* placement rule: tbz (not my house: fail) -> nop */
    {0x140f050, 0x37000480, 0x14000024, "house"},   /* pick-up rule: tbnz (my house: go on) -> b */
    {0x33543f8, 0x36000060, 0x14000003, "house"},   /* pocket placement: tbz (someone else's house: code 0x11) -> b */
    {0x2267da8, 0x54000a21, 0xd503201f, "house"},   /* d-pad 0xf0: b.ne (not the host: drop) -> nop */
    {0x2267d28, 0x54000e21, 0xd503201f, "house"},   /* d-pad 0xef decorating: b.ne (not the host: drop) -> nop */
    {0x2df5a1c, 0x54000100, 0x14000008, "house"},   /* main+0x2df5480: b.eq (the host: go on) -> b */
    /* The light (d-pad up, command 0x15, main+0x2df5480): a copy that is not the session's host branches at
       main+0x2df59d8 straight to IsInMyHouse and main+0x256b250 == 1, and stops at either. */
    {0x2df5a28, 0x36000f60, 0xd503201f, "house"},   /* tbz (not own house: stop) -> nop */
    {0x2df5a38, 0x54000ee0, 0xd503201f, "house"},   /* b.eq (main+0x256b250 == 1: stop) -> nop */
    /* Y indoors: the Y decision main+0x232583c gives up when main+0x23f3d30(player) is false, which in a room is
       main+0x15e638c = house interior and "own house" (main+0x2813cd8). */
    {0x23f3f40, 0x360001e0, 0xd503201f, "house"},   /* tbz w0, #0, 0x23f3f7c (not own house: no) -> nop (then b: yes) */
    /* Placing from pockets indoors: "You can't use this item right now" (SYS_Telepathy 404, code 0x28) is the default
       code of an item with no actions; the pocket submenu builder main+0x307cf70 adds the place entries only for a
       player in their own house (main+0x2813cd8). */
    {0x307dc50, 0x360005a0, 0xd503201f, "house"},   /* tbz w0, #0, 0x307dd04 (not own house: no place entries) -> nop */
    /* The main island's house d-pad goes through the selector's generic part (main+0x2267eec on; the room branch
       main+0x2267b10 is the Slumber Island house only), as actions 0x45 / 0x3d / 0x3c / 4 of main+0x2df6328. Action 0x3d
       (storage) asks IsInMyHouse then main+0x256b250 >= 2. Action 0x45 (decorating mode: main+0x2dfb4f8, own house on own
       island) and the company refusals are opened. Entering decorating mode used to freeze the other player: the enter
       function of cInteriorMode's VisibleOff state (main+0x23081f0) hides every other player (model+0x5f02 = 1, then
       vtable+0x4e8(model, 0)), and it runs on the other copy too, for the replayed player, so that copy hid and froze
       its own player. Skipping the loop keeps everyone visible and moving. */
    {0x2df6d28, 0x3607d000, 0xd503201f, "house"},   /* 0x3d: tbz (not own house: stop) -> nop */
    {0x2df6d38, 0x54ffcf83, 0xd503201f, "house"},   /* 0x3d: b.lo (main+0x256b250 < 2: stop) -> nop */
    {0x2df6b48, 0x540003e3, 0x1400001f, "house"},   /* company refusal (code 4): b.lo -> b 0x2df6bc4 */
    {0x2df6bb8, 0x54000063, 0x14000003, "house"},   /* company refusal (code 0x87, rude to redecorate): b.lo -> b */
    {0x2df6d60, 0x3707ea00, 0x17ffff50, "house"},   /* 0x45: tbnz (own house on own island: go on) -> b */
    {0x2308480, 0x540005a0, 0x1400002d, "house"},   /* VisibleOff enter: b.eq (loop over other players) -> b past it */
    /* Nook Shopping with company (group "mail"; LAN-COOP-DESIGN.md 8.20). The order check main+0x2b75620 reads the
       session *(main+0x4ab1760) +0x80: below 2 (no session) it runs the offline checks at main+0x2b75908; in a session
       every path refuses: DIALOG_Msg 1122 ("visiting another player's island") on a copy that is not the host, 1121
       (company) on the host with 2 or more players, else 1123 ("the airport gate is open"). The offline checks now run
       in a session too, and the visitor's order goes to the host's copy (order_install, coop_edit.c). */
    {0x2b757b4, 0x54000aa3, 0x14000055, "mail"},   /* b.lo 0x2b75908 (no session: offline checks) -> b */
    /* The mailbox's A press: its action component asks vtable +0x328 = main+0x1352130 "is this player the post's
       owner" (resident slot main+0x24819f0, then its id == component+0x588) before any state or script runs; a player
       with no resident slot (a visitor) gets 0 at main+0x135219c. Residents still go through the owner compare. */
    {0x135219c, 0x2a1f03e0, 0x52800020, "mail"},   /* mov w0, wzr (no resident slot: not the owner) -> mov w0, #1 */
    /* The hotel (group "hotel"; LAN-COOP-DESIGN.md 8.22). The lobby stairs (main+0x221a7f0, map 0x3ee08254) let a player
       up only when main+0x1cbec9c(session) is false and the session has fewer than 2 players, then if the island flag
       HotelIsCompletedEditTutorialAnyone is set; otherwise System_HotelRoomVisit ProhibitEnter ("I shouldn't enter
       without permission" / "I should be socializing"). Both company checks are dropped. */
    {0x221b028, 0x370001a0, 0xd503201f, "hotel"},  /* tbnz (session check: no) -> nop */
    {0x221b03c, 0x54000108, 0xd503201f, "hotel"},  /* b.hi (2 or more players: no) -> nop */
    /* The job's end reads save file 1, the per-player Happy Home Paradise save, through main+0x3675e00; in a session the
       slot holds the other players' buffer (slot 0) instead and file 1 is not loaded. The read now returns (to the
       function's epilogue) when file 1 is not ready, instead of reading through a null. */
    {0x3675e54, 0x54000121, 0x54000881, "hotel"},  /* b.ne 0x3675e78 (null) -> b.ne 0x3675f64 (return) */
    /* The same two in the pull's item state 6 (enter main+0x13abd30, run main+0x13ac0d0). */
    {0x13ac014, 0x54000500, 0x14000026, "push-anim"},   /* b.eq 0x13ac0b4 -> b 0x13ac0ac (set the flag) */
    {0x13ac394, 0x7100211f, 0x7100f11f, "push-anim"},   /* cmp w8, #8 -> cmp w8, #60 */
};
static void mp_edit_apply(Module *m) {
    const char *e = getenv("ACNH_MP_EDIT"); if (!e || !*e || !strcmp(e, "0")) return;
    unsigned n = 0;
    for (size_t i = 0; i < sizeof mp_edit_sites / sizeof *mp_edit_sites; i++) {
        if (strcmp(e, "full") && !strstr(e, mp_edit_sites[i].group)) continue;
        uint32_t w; memcpy(&w, m->base + mp_edit_sites[i].off, 4);
        if (w != mp_edit_sites[i].original) runtime_fail("ACNH_MP_EDIT: a site's original word differs (another game version?)");
        memcpy(m->base + mp_edit_sites[i].off, &mp_edit_sites[i].patched, 4); n++;
    }
    fprintf(stderr, "[acnh] local play editing (%s): %u word(s) patched\n", e, n);
}

/* Museum donations by a visitor (ACNH_MP_EDIT group "museum"; docs/LAN-COOP-DESIGN.md 8.12). The donation action
   (EventFlowActionOwlDonationDone, main+0xafb0f0) records each item with main+0x26bdd90(museum record, item, donor),
   where donor = main+0x24819f0(player) is the donor's resident slot on this island, or -1 for anyone else (a
   visitor), and -1 records nothing. Its only call, main+0xafb53c, goes through this stub: with a negative donor and a
   credit in the data page's slot (the host's slot + 1, kept by coop_edit.c while visiting; 0 otherwise) it records
   with that credit and also puts the 8-byte item in a 16-entry ring on the data page, which coop_edit.c sends to the
   host, whose copy records it in the island that is saved. x3, x4, x16 and x17 are free at a call.
     tbz w2, #31, 1f; adrp x16, <data>; ldr w17, [x16, #CREDIT]; cbz w17, 1f; sub w2, w17, #1
     ldr w17, [x16, #HEAD]; and w3, w17, #15; add x3, x16, x3, lsl #3; ldr x4, [x1]; str x4, [x3, #RING]
     dmb ishst; add w17, w17, #1; str w17, [x16, #HEAD]; 1: b main+0x26bdd90
   Mac only for now, like the site probes it sits after. */
#if !TARGET_OS_IPHONE
#define MUSEUM_SITE 0x0afb53cu
#define MUSEUM_RECORD 0x26bdd90u
#define MUSEUM_STUB (SIZE_STUBS + 32 * 24)   /* after the site probes' stubs */
#define MUSEUM_CREDIT 0x200
#define MUSEUM_HEAD 0x204
#define MUSEUM_RING 0x210
static void museum_install(Module *m, uint64_t s, uint64_t data_off) {
    const char *e = getenv("ACNH_MP_EDIT");
    if (!e || (strcmp(e, "full") && !strstr(e, "museum"))) return;
    uint32_t call; memcpy(&call, m->base + MUSEUM_SITE, 4);
    if (call != aspect_branch(MUSEUM_SITE, MUSEUM_RECORD) + 0x80000000u) runtime_fail("ACNH_MP_EDIT museum: the donation call differs (another game version?)");
    if (s + 64 > m->ro_off) runtime_fail("ACNH_MP_EDIT museum: no room in main's text padding");
    for (unsigned b = 0; b < 64; b++) if (m->base[s + b]) runtime_fail("ACNH_MP_EDIT museum: text padding in use");
    if (data_off + MUSEUM_RING + 16 * 8 > m->size) runtime_fail("ACNH_MP_EDIT museum: no data page after main");
    uint32_t stub[14] = {
        0x36000000u | 31u << 19 | 13u << 5 | 2,                        /* tbz w2, #31, 1f */
        (adrp_x10(s + 4, data_off) & ~31u) | 16,                       /* adrp x16, <data> */
        0xb9400000u | (MUSEUM_CREDIT / 4) << 10 | 16 << 5 | 17,        /* ldr w17, [x16, #CREDIT] */
        0x34000000u | 10u << 5 | 17,                                   /* cbz w17, 1f */
        0x51000400u | 17 << 5 | 2,                                     /* sub w2, w17, #1 */
        0xb9400000u | (MUSEUM_HEAD / 4) << 10 | 16 << 5 | 17,          /* ldr w17, [x16, #HEAD] */
        0x12000c00u | 17 << 5 | 3,                                     /* and w3, w17, #15 */
        0x8b000000u | 3 << 16 | 3 << 10 | 16 << 5 | 3,                 /* add x3, x16, x3, lsl #3 */
        0xf9400000u | 1 << 5 | 4,                                      /* ldr x4, [x1] */
        0xf9000000u | (MUSEUM_RING / 8) << 10 | 3 << 5 | 4,            /* str x4, [x3, #RING] */
        0xd5033abfu,                                                   /* dmb ishst */
        0x11000400u | 17 << 5 | 17,                                    /* add w17, w17, #1 */
        0xb9000000u | (MUSEUM_HEAD / 4) << 10 | 16 << 5 | 17,          /* str w17, [x16, #HEAD] */
        aspect_branch(s + 52, MUSEUM_RECORD)};                         /* 1: b main+0x26bdd90 */
    memcpy(m->base + s, stub, sizeof stub);
    uint32_t bl = aspect_branch(MUSEUM_SITE, s) + 0x80000000u; memcpy(m->base + MUSEUM_SITE, &bl, 4);
    coop_edit_set_queue((volatile uint32_t *)(void *)(m->base + data_off + MUSEUM_CREDIT), (volatile uint64_t *)(void *)(m->base + data_off + MUSEUM_RING));
    fprintf(stderr, "[acnh] local play editing: visitor museum donations go to the host\n");
}

/* Pushed / pulled furniture slides on every copy (ACNH_MP_EDIT group "push-anim"; LAN-COOP-DESIGN.md 8.16). A push
   gives the item a move (main+0x13773a4(item, cells, &player, data, 0): target at item+0x8c0, pushing player at
   item+0x848, item state 5), and while it runs the item follows the pushing player's per-frame movement
   (main+0x13ab740: main+0x244c554(player) "in pull / push / turn state", main+0x241b698 that player's movement, both
   from the per-player network data, which every copy has). The player byte comes from the action data's +0x68, which
   only the copy that started the push has; elsewhere it is 0xff, so the item does not follow and jumps at the end.
   This stub on main+0x13773a4's first instruction calls coop_fill_pusher(&player) (coop_edit.c), which fills a missing
   player with the one now pushing, pulling or turning. The pull's setter, main+0x1377a28 (item state 6), has the same
   arguments and first instruction and gets the same stub.
     stp x29, x30, [sp, #-0x40]!; stp x0, x1, [sp, #0x10]; stp x2, x3, [sp, #0x20]; str x4, [sp, #0x30]; mov x0, x2
     adrp x16, <data>; ldr x16, [x16, #PUSH_HOOK]; blr x16; ldr x4, [sp, #0x30]; ldp x2, x3, [sp, #0x20]
     ldp x0, x1, [sp, #0x10]; ldp x29, x30, [sp], #0x40; <the site's instruction>; b <site + 4> */
static const uint64_t push_sites[] = {0x13773a4u, 0x1377a28u};   /* push setter, pull setter */
#define PUSH_SITE_WORD 0xd10243ffu   /* sub sp, sp, #0x90 */
#define PUSH_STUB (MUSEUM_STUB + 64)   /* 64 bytes per site */
#define PUSH_HOOK 0x300
static void push_install(Module *m, uint64_t s, uint64_t data_off) {
    const char *e = getenv("ACNH_MP_EDIT");
    if (!e || (strcmp(e, "full") && !strstr(e, "push-anim"))) return;
    if (data_off + PUSH_HOOK + 8 > m->size) runtime_fail("ACNH_MP_EDIT push-anim: no data page after main");
    *(uint64_t *)(void *)(m->base + data_off + PUSH_HOOK) = (uint64_t)(uintptr_t)coop_fill_pusher;
    for (size_t k = 0; k < sizeof push_sites / sizeof *push_sites; k++, s += 64) {
    uint64_t site = push_sites[k];
    uint32_t w; memcpy(&w, m->base + site, 4);
    if (w != PUSH_SITE_WORD) runtime_fail("ACNH_MP_EDIT push-anim: the item move function differs (another game version?)");
    if (s + 64 > m->ro_off) runtime_fail("ACNH_MP_EDIT push-anim: no room in main's text padding");
    for (unsigned b = 0; b < 64; b++) if (m->base[s + b]) runtime_fail("ACNH_MP_EDIT push-anim: text padding in use");
    uint32_t stub[14] = {
        0xa9bc7bfdu,                                                   /* stp x29, x30, [sp, #-0x40]! */
        0xa90107e0u,                                                   /* stp x0, x1, [sp, #0x10] */
        0xa9020fe2u,                                                   /* stp x2, x3, [sp, #0x20] */
        0xf9001be4u,                                                   /* str x4, [sp, #0x30] */
        0xaa0203e0u,                                                   /* mov x0, x2 */
        (adrp_x10(s + 20, data_off) & ~31u) | 16,                      /* adrp x16, <data> */
        0xf9400000u | (PUSH_HOOK / 8) << 10 | 16 << 5 | 16,            /* ldr x16, [x16, #PUSH_HOOK] */
        0xd63f0200u,                                                   /* blr x16 */
        0xf9401be4u,                                                   /* ldr x4, [sp, #0x30] */
        0xa9420fe2u,                                                   /* ldp x2, x3, [sp, #0x20] */
        0xa94107e0u,                                                   /* ldp x0, x1, [sp, #0x10] */
        0xa8c47bfdu,                                                   /* ldp x29, x30, [sp], #0x40 */
        PUSH_SITE_WORD,
        aspect_branch(s + 52, site + 4)};
    memcpy(m->base + s, stub, sizeof stub);
    uint32_t b = aspect_branch(site, s); memcpy(m->base + site, &b, 4);
    }
    fprintf(stderr, "[acnh] local play editing: pushed furniture slides on every copy\n");
}

/* Decorating mode in company (ACNH_MP_EDIT group "decor-sync"; LAN-COOP-DESIGN.md 8.18): stubs on the room item
   layer's two writes (main+0x1950560 put a record, main+0x19508a0 clear a tile) hand their arguments to coop_edit.c,
   which sends the ones the decorating editor makes; two more on the place ops (0, 5) log their packets. Each stub
   keeps x0..x8, calls the hook with x0..x3 and the return address, then runs the site's first instruction (all four
   are `sub sp, sp, #n`) and goes on:
     stp x29, x30, [sp, #-0x60]!; stp x0, x1, [sp, #0x10]; stp x2, x3, [sp, #0x20]; stp x4, x5, [sp, #0x30]
     stp x6, x7, [sp, #0x40]; str x8, [sp, #0x50]; mov x4, x30; adrp x16, <data>; ldr x16, [x16, #slot]; blr x16
     ldr x8, [sp, #0x50]; ldp x6, x7, [sp, #0x40]; ldp x4, x5, [sp, #0x30]; ldp x2, x3, [sp, #0x20]
     ldp x0, x1, [sp, #0x10]; ldp x29, x30, [sp], #0x60; <the site's instruction>; b <site + 4> */
static const struct { uint64_t site; uint32_t word; void *hook; } decor_sites[] = {
    {0x1950560u, 0xd10143ffu, (void *)coop_room_set},      /* sub sp, sp, #0x50 */
    {0x19508a0u, 0xd10103ffu, (void *)coop_room_clear},    /* sub sp, sp, #0x40 */
    {0x21da650u, 0xd10303ffu, (void *)coop_room_op_seen},  /* op 0, sub sp, sp, #0xc0 */
    {0x21dc0e8u, 0xd10383ffu, (void *)coop_room_op_seen},  /* op 5, sub sp, sp, #0xe0 */
};
#define DECOR_STUB (PUSH_STUB + 128)   /* 80 bytes per site */
#define DECOR_HOOK 0x308
static void precall_stub(Module *m, uint64_t s, uint64_t data_off, uint64_t site, uint32_t word, uint64_t slot, void *hook, const char *group) {
    char why[160]; const char *bad = NULL;
    uint32_t w; memcpy(&w, m->base + site, 4);
    if (data_off + slot + 8 > m->size) bad = "no data page after main";
    else if (w != word) bad = "a hooked function differs (another game version?)";
    else if (s + 80 > m->ro_off) bad = "no room in main's text padding";
    else for (unsigned b = 0; b < 80; b++) if (m->base[s + b]) { bad = "text padding in use"; break; }
    if (bad) { snprintf(why, sizeof why, "ACNH_MP_EDIT %s: %s (main+%#llx)", group, bad, (unsigned long long)site); runtime_fail(why); }
    *(uint64_t *)(void *)(m->base + data_off + slot) = (uint64_t)(uintptr_t)hook;
    uint32_t stub[20] = {
        0xa9ba7bfdu,                                                   /* stp x29, x30, [sp, #-0x60]! */
        0xa90107e0u,                                                   /* stp x0, x1, [sp, #0x10] */
        0xa9020fe2u,                                                   /* stp x2, x3, [sp, #0x20] */
        0xa90317e4u,                                                   /* stp x4, x5, [sp, #0x30] */
        0xa9041fe6u,                                                   /* stp x6, x7, [sp, #0x40] */
        0xf9002be8u,                                                   /* str x8, [sp, #0x50] */
        0xaa1e03e4u,                                                   /* mov x4, x30 */
        (adrp_x10(s + 28, data_off) & ~31u) | 16,                      /* adrp x16, <data> */
        0xf9400000u | (uint32_t)(slot / 8) << 10 | 16 << 5 | 16,       /* ldr x16, [x16, #slot] */
        0xd63f0200u,                                                   /* blr x16 */
        0xf9402be8u,                                                   /* ldr x8, [sp, #0x50] */
        0xa9441fe6u,                                                   /* ldp x6, x7, [sp, #0x40] */
        0xa94317e4u,                                                   /* ldp x4, x5, [sp, #0x30] */
        0xa9420fe2u,                                                   /* ldp x2, x3, [sp, #0x20] */
        0xa94107e0u,                                                   /* ldp x0, x1, [sp, #0x10] */
        0xa8c67bfdu,                                                   /* ldp x29, x30, [sp], #0x60 */
        w,
        aspect_branch(s + 68, site + 4),
        0, 0};
    memcpy(m->base + s, stub, sizeof stub);
    uint32_t b = aspect_branch(site, s); memcpy(m->base + site, &b, 4);
}
static void decor_install(Module *m, uint64_t s, uint64_t data_off) {
    const char *e = getenv("ACNH_MP_EDIT");
    if (!e || (strcmp(e, "full") && !strstr(e, "decor-sync"))) return;
    for (size_t k = 0; k < sizeof decor_sites / sizeof *decor_sites; k++, s += 80)
        precall_stub(m, s, data_off, decor_sites[k].site, decor_sites[k].word, DECOR_HOOK + 8 * k, decor_sites[k].hook, "decor-sync");
    fprintf(stderr, "[acnh] local play editing: decorating-mode changes go to the other copies\n");
}

/* Nook Shopping orders (group "mail"): the order's record call main+0x2b7e064 `bl main+0x1a49ab0(&item, &date, &player)`
   (it puts the order in the ordering player's personal data, delivered as mail the next day) goes to coop_order_record
   (coop_edit.c), which records it as before outside a session and on the host, and on a visitor's copy sends it to the
   host instead. The stub: adrp x16, <data>; ldr x16, [x16, #ORDER_HOOK]; br x16 (the bl's return address stays). */
#define ORDER_SITE 0x2b7e064u
#define ORDER_RECORD 0x1a49ab0u
#define ORDER_STUB (DECOR_STUB + 80 * 4)
#define ORDER_HOOK 0x328
static void order_install(Module *m, uint64_t s, uint64_t data_off) {
    const char *e = getenv("ACNH_MP_EDIT");
    if (!e || (strcmp(e, "full") && !strstr(e, "mail"))) return;
    uint32_t w; memcpy(&w, m->base + ORDER_SITE, 4);
    if (w != aspect_branch(ORDER_SITE, ORDER_RECORD) + 0x80000000u) runtime_fail("ACNH_MP_EDIT mail: the order's record call differs (another game version?)");
    if (s + 16 > m->ro_off) runtime_fail("ACNH_MP_EDIT mail: no room in main's text padding");
    for (unsigned b = 0; b < 16; b++) if (m->base[s + b]) runtime_fail("ACNH_MP_EDIT mail: text padding in use");
    if (data_off + ORDER_HOOK + 8 > m->size) runtime_fail("ACNH_MP_EDIT mail: no data page after main");
    *(uint64_t *)(void *)(m->base + data_off + ORDER_HOOK) = (uint64_t)(uintptr_t)coop_order_record;
    uint32_t stub[3] = {(adrp_x10(s, data_off) & ~31u) | 16, 0xf9400000u | (ORDER_HOOK / 8) << 10 | 16 << 5 | 16, 0xd61f0200u};
    memcpy(m->base + s, stub, sizeof stub);
    uint32_t bl = aspect_branch(ORDER_SITE, s) + 0x80000000u; memcpy(m->base + ORDER_SITE, &bl, 4);
    fprintf(stderr, "[acnh] local play editing: a visitor's Nook Shopping orders go to the host's mailbox\n");
}

/* The host's mailbox for a visitor (group "mail"): main+0x25f2a00(save) picks the postbox every mail user reads (the
   local player's). Its first instruction branches to coop_postbox (coop_edit.c), which calls the original through
   the trampoline after the stub (the displaced instruction, then b main+0x25f2a04) and may return the host's postbox
   instead. Stub: adrp x16, <data>; ldr x16, [x16, #POSTBOX_HOOK]; br x16; trampoline: stp x29, x30, [sp, #-0x20]!;
   b main+0x25f2a04. */
#define POSTBOX_SITE 0x25f2a00u
#define POSTBOX_WORD 0xa9be7bfdu   /* stp x29, x30, [sp, #-0x20]! */
#define POSTBOX_STUB (ORDER_STUB + 16)
#define POSTBOX_HOOK 0x330
uint64_t coop_postbox_original;   /* coop_edit.c calls the original through this */
/* A visitor follows the host's decorating job (group "hotel"; LAN-COOP-DESIGN.md 8.22): main+0x15bf0b0(hotel manager)
   prepares the room just before going to it, main+0x15c7d94(manager) ends the job ("cEditEnd"); coop_edit.c tells the
   visitors. */
static void hotel_install(Module *m, uint64_t s, uint64_t data_off) {
    const char *e = getenv("ACNH_MP_EDIT");
    if (!e || (strcmp(e, "full") && !strstr(e, "hotel"))) return;
    precall_stub(m, s + 16, data_off, 0x15bf0b0u, 0xd10283ffu, 0x350, (void *)coop_hotel_go, "hotel");    /* sub sp, sp, #0xa0 */
    precall_stub(m, s + 96, data_off, 0x15c7d94u, 0xd10183ffu, 0x358, (void *)coop_hotel_end, "hotel");   /* sub sp, sp, #0x60 */
}
static void postbox_install(Module *m, uint64_t s, uint64_t data_off) {
    const char *e = getenv("ACNH_MP_EDIT");
    if (!e || (strcmp(e, "full") && !strstr(e, "mail"))) return;
    uint32_t w; memcpy(&w, m->base + POSTBOX_SITE, 4);
    if (w != POSTBOX_WORD) runtime_fail("ACNH_MP_EDIT mail: the postbox accessor differs (another game version?)");
    if (s + 32 > m->ro_off) runtime_fail("ACNH_MP_EDIT mail: no room in main's text padding");
    for (unsigned b = 0; b < 32; b++) if (m->base[s + b]) runtime_fail("ACNH_MP_EDIT mail: text padding in use");
    if (data_off + POSTBOX_HOOK + 8 > m->size) runtime_fail("ACNH_MP_EDIT mail: no data page after main");
    *(uint64_t *)(void *)(m->base + data_off + POSTBOX_HOOK) = (uint64_t)(uintptr_t)coop_postbox;
    uint32_t stub[5] = {(adrp_x10(s, data_off) & ~31u) | 16, 0xf9400000u | (POSTBOX_HOOK / 8) << 10 | 16 << 5 | 16, 0xd61f0200u,
                        POSTBOX_WORD, aspect_branch(s + 16, POSTBOX_SITE + 4)};
    memcpy(m->base + s, stub, sizeof stub);
    coop_postbox_original = (uint64_t)(uintptr_t)(m->base + s + 12);
    uint32_t b = aspect_branch(POSTBOX_SITE, s); memcpy(m->base + POSTBOX_SITE, &b, 4);
    fprintf(stderr, "[acnh] local play editing: a visitor opens the host's mailbox\n");
    /* Only while that mailbox is open: the mailbox screen's start (EventFlowActionUIPostCreate, exec main+0xd28250) and
       the end of the mailbox action (enter of the player state cFtrMailboxEnd, main+0x22a60d0) tell coop_edit.c. */
    precall_stub(m, s + 32, data_off, 0xd28250u, 0xf9400408u, 0x338, (void *)coop_post_open, "mail");    /* ldr x8, [x0, #8] */
    precall_stub(m, s + 112, data_off, 0x22a60d0u, 0xa9be7bfdu, 0x340, (void *)coop_post_close, "mail");   /* stp x29, x30, [sp, #-0x20]! */
}
#endif

void runtime_game_patches(Module *m) {
    /* Frame pacing: ACNH advances its simulation once per frame and asks for present interval 2, i.e. every second
       60 Hz vblank = 30 fps on the console. The graphics layer otherwise paces by the Mac display's refresh rate
       (TOTK steps its game time by real time), which ran ACNH at 60 fps on a 120 Hz display: twice the speed. A 60 Hz
       pacing base keeps the game's own interval meaning. DM_PRESENT_HZ set by the user still wins. */
    setenv("DM_PRESENT_HZ", "60", 0);
    fprintf(stderr, "[acnh] main at %p\n", (void *)m->base);   /* for debugger offsets (main+0x...) */
    speedfix_apply(m);   /* before the aspect stubs, which check their own words */
    mp_edit_apply(m);
    /* Stubs go to main's text padding: 432 zero bytes between text_end and read-only data (ro_off). */
    aspect_install_reads(m, (m->text_end + 15) & ~15ULL, (m->file_size + 0x3fff) & ~0x3fffULL);
    render_install(m, ASPECT_STUBS_END(m), (m->file_size + 0x3fff) & ~0x3fffULL);
#if !TARGET_OS_IPHONE
    output_install(m, ASPECT_STUBS_END(m) + 32, (m->file_size + 0x3fff) & ~0x3fffULL);
    site_probe(m, ASPECT_STUBS_END(m) + SIZE_STUBS, (m->file_size + 0x3fff) & ~0x3fffULL);
    museum_install(m, ASPECT_STUBS_END(m) + MUSEUM_STUB, (m->file_size + 0x3fff) & ~0x3fffULL);
    push_install(m, ASPECT_STUBS_END(m) + PUSH_STUB, (m->file_size + 0x3fff) & ~0x3fffULL);
    decor_install(m, ASPECT_STUBS_END(m) + DECOR_STUB, (m->file_size + 0x3fff) & ~0x3fffULL);
    order_install(m, ASPECT_STUBS_END(m) + ORDER_STUB, (m->file_size + 0x3fff) & ~0x3fffULL);
    postbox_install(m, ASPECT_STUBS_END(m) + POSTBOX_STUB, (m->file_size + 0x3fff) & ~0x3fffULL);
    hotel_install(m, ASPECT_STUBS_END(m) + POSTBOX_STUB + 32 + 160, (m->file_size + 0x3fff) & ~0x3fffULL);
#endif
}

/* TOTK's frame-step patch has no ACNH counterpart. */
void runtime_frame_step_pin(double ms) { (void)ms; }
void runtime_frame_step_update(uint64_t now_ns) { (void)now_ns; }
/* Picture modes (key 3, the Settings window's Picture), in TOTK's order so that a saved choice means the same: a size of
   the game's table (output_install), shown as it is (the present scales it to the window) or, at 1080p, upscaled to the
   window by MetalFX (the renderer's output stage, which runs only with MetalFX upscaling on, hence both flags). Without the
   output patch only the first two. */
extern void dm_gfx_set_flag(const char *name, int value);
extern int mfxup_output_enabled(void);
typedef struct { int index, mfx; const char *name; } OutputMode;   /* index: the game's size table */
static const OutputMode output_modes[] = {{3, 0, "1080p"}, {3, 1, "MetalFX"}, {4, 0, "1440p"}, {1, 0, "720p"}, {2, 0, "900p"}};
#define OUTPUT_NMODE (int)(sizeof output_modes / sizeof *output_modes)
static int output_index(void) { uint32_t v = output_slot ? atomic_load(output_slot) : 0; return v ? (int)v - 1 : 3; }
int runtime_output_mode_count(void) { return output_enabled() ? OUTPUT_NMODE : 2; }   /* known before the patch is in */
const char *runtime_output_mode_name(int k) { return k >= 0 && k < runtime_output_mode_count() ? output_modes[k].name : ""; }
int runtime_output_mode_get(void) {
    int index = output_index(), mfx = mfxup_output_enabled() != 0;
    for (int k = 0; k < runtime_output_mode_count(); k++) if (output_modes[k].index == index && output_modes[k].mfx == mfx) return k;
    for (int k = 0; k < runtime_output_mode_count(); k++) if (output_modes[k].index == index) return k;
    return -1;
}
int runtime_output_mode_set(int k) {
    if (k < 0 || k >= runtime_output_mode_count()) return 0;
    const OutputMode *o = &output_modes[k];
    if (output_slot && output_index() != o->index) { atomic_store(output_slot, (uint32_t)o->index + 1); runtime_oe_push_message(30); }
    dm_gfx_set_flag("mfx_output", o->mfx); dm_gfx_set_flag("mfx_upscale", o->mfx);
    fprintf(stderr, "[acnh] picture: %s\n", o->name);
    return 1;
}
int runtime_output_mode_cycle(void) { return runtime_output_mode_set((runtime_output_mode_get() + 1) % runtime_output_mode_count()); }
uint64_t runtime_output_size_current(void) { int i = output_index(); return game_sizes[i][0] | ((uint64_t)game_sizes[i][1] << 32); }
