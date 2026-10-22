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
    /* "visitor-grab": all of it, so a visitor can also hold and turn the owner's furniture. Holding it allocates a
       half-resolution "ExtendGBuffer" render target (main+0x1458fd0 -> 0x38472d0 -> pool search main+0x3865630); with the
       game's own pool sizes the visitor's search fails and main+0x3847ca0 uses the null result (crash). Under test with
       the larger pools of the output patch. */
    {0x21cc50c, 0xa9bc7bfd, 0x52800000, "visitor-grab"},   /* stp x29, x30, [sp, #-0x40]!  ->  mov w0, #0 */
    {0x21cc510, 0xf9000bf7, 0xd65f03c0, "visitor-grab"},   /* str x23, [sp, #0x10]         ->  ret */
    {0x21cc714, 0x52800060, 0x52800000, "furniture"},   /* mov w0, #3 -> mov w0, #0 */
    {0x21cc724, 0x52800080, 0x52800000, "furniture"},   /* mov w0, #4 -> mov w0, #0 */
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
