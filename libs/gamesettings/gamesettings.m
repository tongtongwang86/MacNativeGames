/* GameSettings implementation. Layers, bottom to top (ARCHITECTURE.md has the whole picture):
     1. keyboard - the Switch inputs, the default keys, the bindings and the per-key lookup the game reads each frame
     2. store    - Settings/keyboard.json (every game) and Settings/<title id>.json (one game), on this device only
     3. editing  - giving an input a key, clearing it, the defaults, key names
     4. drawing  - a Switch Pro Controller showing each key at its button (Core Graphics; Mac and iOS)
     5. windows  - the Settings window (Mac, ⌘,) and the Settings sheet (iPhone / iPad): Graphics, Controls, Saves */
#import <Foundation/Foundation.h>
#include <math.h>
#include <TargetConditionals.h>
#import <CoreText/CoreText.h>
#if TARGET_OS_IPHONE
#import <UIKit/UIKit.h>
#else
#import <AppKit/AppKit.h>
#endif
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include "gamesettings.h"

#define NONE 0xff
#define MAX_KEYS 3

/* ======================= 1. keyboard ======================= */
/* Layers 3 and 4 (the controller drawing, the windows) are after the store. */
enum { AX_NONE, AX_LUP, AX_LDOWN, AX_LLEFT, AX_LRIGHT, AX_RUP, AX_RDOWN, AX_RLEFT, AX_RRIGHT };
typedef struct { const char *id, *title; uint64_t bit; int axis; uint8_t defaults[MAX_KEYS]; } Input;
/* The default keys are the ports' original layout (US positions): WASD and the arrows for the sticks, J K I U for A B X Y. */
static const Input inputs[] = {
    {"ls_up", "Left stick up", 0, AX_LUP, {13, NONE, NONE}},           {"ls_down", "Left stick down", 0, AX_LDOWN, {1, NONE, NONE}},
    {"ls_left", "Left stick left", 0, AX_LLEFT, {0, NONE, NONE}},      {"ls_right", "Left stick right", 0, AX_LRIGHT, {2, NONE, NONE}},
    {"rs_up", "Right stick up", 0, AX_RUP, {126, NONE, NONE}},         {"rs_down", "Right stick down", 0, AX_RDOWN, {125, NONE, NONE}},
    {"rs_left", "Right stick left", 0, AX_RLEFT, {123, NONE, NONE}},   {"rs_right", "Right stick right", 0, AX_RRIGHT, {124, NONE, NONE}},
    {"a", "A", 1 << 0, AX_NONE, {38, NONE, NONE}},                     {"b", "B", 1 << 1, AX_NONE, {40, 49, NONE}},
    {"x", "X", 1 << 2, AX_NONE, {34, NONE, NONE}},                     {"y", "Y", 1 << 3, AX_NONE, {32, NONE, NONE}},
    {"l", "L", 1 << 6, AX_NONE, {12, NONE, NONE}},                     {"r", "R", 1 << 7, AX_NONE, {14, NONE, NONE}},
    {"zl", "ZL", 1 << 8, AX_NONE, {58, NONE, NONE}},                   {"zr", "ZR", 1 << 9, AX_NONE, {56, NONE, NONE}},
    {"lstick", "Left stick press", 1 << 4, AX_NONE, {6, NONE, NONE}},  {"rstick", "Right stick press", 1 << 5, AX_NONE, {8, NONE, NONE}},
    {"plus", "+ (menu)", 1 << 10, AX_NONE, {36, NONE, NONE}},          {"minus", "− (map)", 1 << 11, AX_NONE, {48, 53, NONE}},
    {"up", "D-pad up", 1 << 13, AX_NONE, {17, NONE, NONE}},            {"down", "D-pad down", 1 << 15, AX_NONE, {5, NONE, NONE}},
    {"left", "D-pad left", 1 << 12, AX_NONE, {3, NONE, NONE}},         {"right", "D-pad right", 1 << 14, AX_NONE, {4, NONE, NONE}},
};
#define NINPUT (sizeof inputs / sizeof *inputs)
static uint8_t bound[NINPUT][MAX_KEYS];
/* What each key does, rebuilt on every change and read by the game's input thread each frame. */
static _Atomic uint64_t key_bits[128];
static _Atomic uint32_t key_axes[128];   /* bit n: axis n */

static void rebuild_lookup(void) {
    uint64_t bits[128] = {0}; uint32_t axes[128] = {0};
    for (unsigned i = 0; i < NINPUT; i++) for (int k = 0; k < MAX_KEYS; k++) {
        uint8_t c = bound[i][k]; if (c >= 128) continue;
        bits[c] |= inputs[i].bit; if (inputs[i].axis) axes[c] |= 1u << inputs[i].axis;
    }
    for (int c = 0; c < 128; c++) { atomic_store(&key_bits[c], bits[c]); atomic_store(&key_axes[c], axes[c]); }
}
static void set_defaults(void) { for (unsigned i = 0; i < NINPUT; i++) memcpy(bound[i], inputs[i].defaults, MAX_KEYS); }
/* The default keys until gamesettings_start has loaded the player's (or for a port that never calls it). */
static _Atomic int lookup_ready;
static void ensure_lookup(void) { if (!atomic_load_explicit(&lookup_ready, memory_order_acquire)) { set_defaults(); rebuild_lookup(); atomic_store(&lookup_ready, 1); } }

uint64_t gamesettings_buttons(const unsigned char held[128]) {
    ensure_lookup();
    uint64_t b = 0; for (int c = 0; c < 128; c++) if (held[c]) b |= atomic_load_explicit(&key_bits[c], memory_order_relaxed);
    return b;
}
void gamesettings_sticks(const unsigned char held[128], int32_t *lx, int32_t *ly, int32_t *rx, int32_t *ry) {
    ensure_lookup();
    uint32_t a = 0; for (int c = 0; c < 128; c++) if (held[c]) a |= atomic_load_explicit(&key_axes[c], memory_order_relaxed);
#define ON(x) (!!(a & (1u << (x))))
    *lx = 32767 * (ON(AX_LRIGHT) - ON(AX_LLEFT)); *ly = 32767 * (ON(AX_LUP) - ON(AX_LDOWN));
    *rx = 32767 * (ON(AX_RRIGHT) - ON(AX_RLEFT)); *ry = 32767 * (ON(AX_RUP) - ON(AX_RDOWN));
#undef ON
}

/* US key names by position (the codes are positions; the label is what a US keyboard prints there). */
static NSString *key_name(uint8_t c) {
    static NSDictionary *names;
    static dispatch_once_t once; dispatch_once(&once, ^{
        names = @{@0:@"A",@1:@"S",@2:@"D",@3:@"F",@4:@"H",@5:@"G",@6:@"Z",@7:@"X",@8:@"C",@9:@"V",@11:@"B",@12:@"Q",@13:@"W",@14:@"E",
            @15:@"R",@16:@"Y",@17:@"T",@18:@"1",@19:@"2",@20:@"3",@21:@"4",@22:@"6",@23:@"5",@24:@"=",@25:@"9",@26:@"7",@27:@"-",@28:@"8",
            @29:@"0",@30:@"]",@31:@"O",@32:@"U",@33:@"[",@34:@"I",@35:@"P",@36:@"Return",@37:@"L",@38:@"J",@39:@"'",@40:@"K",@41:@";",
            @42:@"\\",@43:@",",@44:@"/",@45:@"N",@46:@"M",@47:@".",@48:@"Tab",@49:@"Space",@50:@"`",@51:@"Delete",@53:@"Esc",
            @56:@"Shift",@57:@"Caps Lock",@58:@"Option",@59:@"Control",@60:@"Right Shift",@61:@"Right Option",@62:@"Right Control",
            @63:@"fn",@96:@"F5",@97:@"F6",@98:@"F7",@99:@"F3",@100:@"F8",@101:@"F9",@103:@"F11",@109:@"F10",@111:@"F12",@118:@"F4",
            @120:@"F2",@122:@"F1",@115:@"Home",@116:@"Page Up",@117:@"Forward Delete",@119:@"End",@121:@"Page Down",
            @123:@"←",@124:@"→",@125:@"↓",@126:@"↑",@65:@"Keypad .",@67:@"Keypad *",@69:@"Keypad +",@75:@"Keypad /",@76:@"Enter",
            @78:@"Keypad -",@82:@"Keypad 0",@83:@"Keypad 1",@84:@"Keypad 2",@85:@"Keypad 3",@86:@"Keypad 4",@87:@"Keypad 5",
            @88:@"Keypad 6",@89:@"Keypad 7",@91:@"Keypad 8",@92:@"Keypad 9"};
    });
    return names[@(c)] ?: [NSString stringWithFormat:@"Key %u", c];
}

/* ======================= 2. store ======================= */
static const gamesettings_config *cfg;
static NSString *tag;
static NSDictionary *graphics_saved;   /* id -> choice index, applied by gamesettings_apply_graphics */
static void say(NSString *s) { fprintf(stderr, "%s settings: %s\n", tag.UTF8String ?: "[gamesettings]", s.UTF8String); }

static NSString *local_dir(void) {
    const char *env = getenv("GAMESETTINGS_LOCAL_DIR"); if (env && *env) return @(env);   /* tests */
#if TARGET_OS_IPHONE
    return [NSSearchPathForDirectoriesInDomains(NSDocumentDirectory, NSUserDomainMask, YES).firstObject stringByAppendingPathComponent:@"Settings"];
#else
    return [NSHomeDirectory() stringByAppendingPathComponent:@"Library/Application Support/MacNativeGames/Settings"];
#endif
}
static NSDictionary *read_json(NSString *path) {
    NSData *d = [NSData dataWithContentsOfFile:path];
    id j = d ? [NSJSONSerialization JSONObjectWithData:d options:0 error:nil] : nil;
    return [j isKindOfClass:NSDictionary.class] ? j : nil;
}
/* Settings stay on this device: each device has its own screen, controls and speed. */
static NSDictionary *load(NSString *name) { return read_json([local_dir() stringByAppendingPathComponent:name]); }
static void save(NSString *name, NSDictionary *body) {
    NSMutableDictionary *j = [body mutableCopy]; j[@"time"] = @(NSDate.date.timeIntervalSince1970); j[@"format"] = @1;
    NSData *d = [NSJSONSerialization dataWithJSONObject:j options:NSJSONWritingPrettyPrinted | NSJSONWritingSortedKeys error:nil];
    [NSFileManager.defaultManager createDirectoryAtPath:local_dir() withIntermediateDirectories:YES attributes:nil error:nil];
    [d writeToFile:[local_dir() stringByAppendingPathComponent:name] atomically:YES];
}
static NSString *graphics_name(void) { return [NSString stringWithFormat:@"%s.json", cfg->title_id]; }

static void load_keyboard(void) {
    set_defaults();
    NSDictionary *b = load(@"keyboard.json")[@"bindings"];
    if ([b isKindOfClass:NSDictionary.class]) for (unsigned i = 0; i < NINPUT; i++) {
        NSArray *codes = b[@(inputs[i].id)]; if (![codes isKindOfClass:NSArray.class]) continue;
        memset(bound[i], NONE, MAX_KEYS); int k = 0;
        for (id c in codes) if ([c isKindOfClass:NSNumber.class] && [c intValue] >= 0 && [c intValue] < 128 && k < MAX_KEYS) bound[i][k++] = (uint8_t)[c intValue];
    }
    rebuild_lookup(); atomic_store(&lookup_ready, 1);
}
static void save_keyboard(void) {
    NSMutableDictionary *b = [NSMutableDictionary dictionary];
    for (unsigned i = 0; i < NINPUT; i++) {
        NSMutableArray *codes = [NSMutableArray array];
        for (int k = 0; k < MAX_KEYS; k++) if (bound[i][k] < 128) [codes addObject:@(bound[i][k])];
        b[@(inputs[i].id)] = codes;
    }
    save(@"keyboard.json", @{@"bindings": b});
}
static void save_graphics(void) {
    if (!cfg) return;
    NSMutableDictionary *v = [graphics_saved mutableCopy] ?: [NSMutableDictionary dictionary];
    for (unsigned i = 0; i < cfg->ngraphics; i++) if (cfg->graphics[i].get) v[@(cfg->graphics[i].id)] = @(cfg->graphics[i].get());
    graphics_saved = v; save(graphics_name(), @{@"values": v});
}

void gamesettings_start(const gamesettings_config *config) {
    @autoreleasepool {
        if (cfg || !config) return;
        cfg = config; tag = @(config->log_tag ?: "[gamesettings]");
        load_keyboard();
        NSDictionary *v = load(graphics_name())[@"values"];
        graphics_saved = [v isKindOfClass:NSDictionary.class] ? v : @{};
        say([NSString stringWithFormat:@"keyboard and %lu graphics settings from %@", (unsigned long)graphics_saved.count, local_dir()]);
    }
}
void gamesettings_apply_graphics(void) {
    if (!cfg) return;
    for (unsigned i = 0; i < cfg->ngraphics; i++) {
        const gamesettings_choice *g = &cfg->graphics[i]; NSNumber *n = graphics_saved[@(g->id)];
        if (!n || !g->set || [n intValue] < 0 || [n unsignedIntValue] >= g->nchoices) continue;
        if (!g->get || g->get() != [n intValue]) g->set([n intValue]);
    }
}
void gamesettings_note_graphics_changed(void) {
    if (!cfg) return;
    dispatch_async(dispatch_get_main_queue(), ^{ @autoreleasepool { save_graphics(); } });
}

/* Where the game keeps its saves (the Saves tab). Set from any thread, read on the main thread. */
static NSString *save_local, *save_cloud;
void gamesettings_set_save_folders(const char *local, const char *cloud) {
    NSString *l = local ? @(local) : nil, *c = cloud ? @(cloud) : nil;
    dispatch_async(dispatch_get_main_queue(), ^{ save_local = l; save_cloud = c; });
}
static NSString *saves_note(void) {
    return @"Quit the game before moving or replacing save files. Whatever iCloud sync replaces is kept in save-backups next to the save folder.";
}

/* ======================= 3. editing (Mac and iOS) ======================= */
/* An input's keys packed at the front of its MAX_KEYS slots; returns how many it has. */
static int pack(unsigned j) {
    uint8_t packed[MAX_KEYS]; memset(packed, NONE, MAX_KEYS); int n = 0;
    for (int k = 0; k < MAX_KEYS; k++) if (bound[j][k] < 128) packed[n++] = bound[j][k];
    memcpy(bound[j], packed, MAX_KEYS); return n;
}
/* Puts the key in input i's slot (each input has MAX_KEYS); a slot past the input's keys is its next free one. A key
   does one thing: it leaves any other input or slot it was on. Saved and live at once. */
static void assign_key(unsigned i, uint8_t code, int slot) {
    if (i >= NINPUT || code >= 128 || slot < 0 || slot >= MAX_KEYS) return;
    if (bound[i][slot] == code) return;
    for (unsigned j = 0; j < NINPUT; j++) for (int k = 0; k < MAX_KEYS; k++) if (bound[j][k] == code) bound[j][k] = NONE;
    int n = pack(i);
    bound[i][slot < n ? slot : n] = code;
    for (unsigned j = 0; j < NINPUT; j++) pack(j);
    rebuild_lookup(); save_keyboard();
}
static void clear_slot(unsigned i, int slot) {
    if (i >= NINPUT || slot < 0 || slot >= MAX_KEYS) return;
    bound[i][slot] = NONE; pack(i); rebuild_lookup(); save_keyboard();
}
/* The slot a click on the controller drawing records: the input's first free one, else its first. */
static int free_slot(unsigned i) { for (int k = 0; k < MAX_KEYS; k++) if (bound[i][k] >= 128) return k; return 0; }
static void restore_defaults(void) { set_defaults(); rebuild_lookup(); save_keyboard(); }
static int bindable(int code) { return code >= 0 && code < 128 && code != 54 && code != 55 && code != 57; }   /* not Command, Caps Lock */
static NSString *slot_title(unsigned i, int k) { return bound[i][k] < 128 ? key_name(bound[i][k]) : @"–"; }
/* The compact name drawn on the controller. */
static NSString *short_name(uint8_t c) {
    static NSDictionary *s;
    static dispatch_once_t once; dispatch_once(&once, ^{
        s = @{@36:@"↩", @48:@"⇥", @49:@"space", @51:@"⌫", @53:@"esc", @56:@"⇧", @60:@"⇧R", @58:@"⌥", @61:@"⌥R", @59:@"⌃", @62:@"⌃R", @63:@"fn", @76:@"⌤",
              @115:@"home", @116:@"pg↑", @117:@"⌦", @119:@"end", @121:@"pg↓"};
    });
    NSString *n = s[@(c)] ?: key_name(c);
    if ([n hasPrefix:@"Keypad "]) n = [@"kp" stringByAppendingString:[n substringFromIndex:7]];
    /* U+FE0E: draw symbols such as ↩ as text; iOS would otherwise pick their emoji form */
    return n.length == 1 && [n characterAtIndex:0] >= 0x2000 ? [n stringByAppendingString:@"\uFE0E"] : n;
}
static NSString *keys_short(unsigned i) {
    NSMutableArray *n = [NSMutableArray array]; for (int k = 0; k < MAX_KEYS; k++) if (bound[i][k] < 128) [n addObject:short_name(bound[i][k])];
    if (!n.count) return @"—";
    return n.count == 1 ? n[0] : [NSString stringWithFormat:@"%@/%@", n[0], n[1]];
}
static int input_index(const char *id) { for (unsigned i = 0; i < NINPUT; i++) if (!strcmp(inputs[i].id, id)) return (int)i; return -1; }

/* iOS keys (UIKey.keyCode, USB HID usages) as the Mac key codes the bindings use. */
int gamesettings_mac_key_from_hid(long usage) {
    static const uint8_t letters[26] = {0, 11, 8, 2, 14, 3, 5, 4, 34, 38, 40, 37, 46, 45, 31, 35, 12, 15, 1, 17, 32, 9, 13, 7, 16, 6};
    static const uint8_t digits[10] = {18, 19, 20, 21, 23, 22, 26, 28, 25, 29};   /* 1 ... 9, 0 */
    static const uint8_t f[12] = {122, 120, 99, 118, 96, 97, 98, 100, 101, 109, 103, 111};
    if (usage >= 4 && usage <= 29) return letters[usage - 4];
    if (usage >= 30 && usage <= 39) return digits[usage - 30];
    if (usage >= 58 && usage <= 69) return f[usage - 58];
    switch (usage) {
    case 40: return 36; case 41: return 53; case 42: return 51; case 43: return 48; case 44: return 49; case 45: return 27;
    case 46: return 24; case 47: return 33; case 48: return 30; case 49: return 42; case 51: return 41; case 52: return 39;
    case 53: return 50; case 54: return 43; case 55: return 47; case 56: return 44; case 57: return 57;
    case 74: return 115; case 75: return 116; case 76: return 117; case 77: return 119; case 78: return 121;
    case 79: return 124; case 80: return 123; case 81: return 125; case 82: return 126; case 88: return 76;
    case 224: return 59; case 225: return 56; case 226: return 58; case 227: return 55;
    case 228: return 62; case 229: return 60; case 230: return 61; case 231: return 54;
    default: return -1;
    }
}

/* ======================= 4. the controller drawing (Mac and iOS) ======================= */
/* Ryujinx's Pro Controller (assets/Controller_ProCon.svg, embedded as a PDF by tools/make_procon_pdf.sh) in the middle
   of a 1650 x 1060 design space (y down), with a key box for every control in a column on its side, joined to the
   control by a line, as in Ryujinx's input settings. The same Core Graphics code draws it in the Mac window and the
   iPad sheet; a click or tap on a box or a control records a key for it. */
#include "procon_pdf.h"
#define DESIGN_W 1650.0
#define DESIGN_H 1060.0
#define IMAGE_X 300.0    /* the controller's 1050 x 1050 image starts here */
#define IMAGE_Y 5.0
#define BOX_W 270.0
#define BOX_H 58.0
/* A control: its name in the box, where it sits on the image (SVG coordinates) and how far a click on it reaches. */
typedef struct { const char *id, *mark; int right; CGFloat x, y, reach; } Control;
static const Control controls[] = {   /* each column top to bottom, as the controls sit */
    {"zl", "ZL", 0, 242, 168, 60},       {"l", "L", 0, 264, 279, 50},         {"minus", "−", 0, 401, 489, 26},
    {"ls_up", "L stick ↑", 0, 240, 525, 0}, {"lstick", "L stick press", 0, 240, 565, 32}, {"ls_left", "L stick ←", 0, 200, 565, 0},
    {"ls_right", "L stick →", 0, 280, 565, 0}, {"ls_down", "L stick ↓", 0, 240, 605, 0},
    {"up", "D-pad ↑", 0, 401, 655, 22},   {"left", "D-pad ←", 0, 351, 703, 22}, {"right", "D-pad →", 0, 451, 703, 22},
    {"down", "D-pad ↓", 0, 401, 751, 22},
    {"zr", "ZR", 1, 810, 168, 60},       {"r", "R", 1, 784, 279, 50},         {"plus", "+", 1, 650, 489, 26},
    {"x", "X", 1, 793, 494, 34},         {"y", "Y", 1, 724, 565, 34},         {"a", "A", 1, 863, 565, 34},
    {"b", "B", 1, 793, 635, 34},         {"rs_up", "R stick ↑", 1, 650, 663, 0}, {"rstick", "R stick press", 1, 650, 703, 32},
    {"rs_left", "R stick ←", 1, 610, 703, 0}, {"rs_right", "R stick →", 1, 690, 703, 0}, {"rs_down", "R stick ↓", 1, 650, 743, 0},
};
#define NCONTROL (sizeof controls / sizeof *controls)
static CGAffineTransform design_to(CGRect bounds) {
    CGFloat s = MIN(bounds.size.width / DESIGN_W, bounds.size.height / DESIGN_H);
    CGFloat ox = bounds.origin.x + (bounds.size.width - DESIGN_W * s) / 2, oy = bounds.origin.y + (bounds.size.height - DESIGN_H * s) / 2;
    return CGAffineTransformScale(CGAffineTransformMakeTranslation(ox, oy), s, s);
}
/* The i-th control's box in its column: 12 to a side, spread over the height. */
static CGRect box_rect(unsigned i) {
    unsigned row = 0; for (unsigned j = 0; j < i; j++) if (controls[j].right == controls[i].right) row++;
    CGFloat x = controls[i].right ? DESIGN_W - BOX_W - 10 : 10, y = 40 + row * ((DESIGN_H - 80 - BOX_H) / 11);
    return CGRectMake(x, y, BOX_W, BOX_H);
}
static CGPoint control_point(unsigned i) { return CGPointMake(IMAGE_X + controls[i].x, IMAGE_Y + controls[i].y); }
static int input_of_control(unsigned i) { return input_index(controls[i].id); }
/* The input under a point of a view showing the drawing in bounds, -1 for none: a key box, or the control itself. */
static int part_at(CGPoint pt, CGRect bounds) {
    CGPoint d = CGPointApplyAffineTransform(pt, CGAffineTransformInvert(design_to(bounds)));
    for (unsigned i = 0; i < NCONTROL; i++) if (CGRectContainsPoint(CGRectInset(box_rect(i), -4, -4), d)) return input_of_control(i);
    for (unsigned i = 0; i < NCONTROL; i++) { CGPoint c = control_point(i);
        if (controls[i].reach > 0 && hypot(d.x - c.x, d.y - c.y) < controls[i].reach) return input_of_control(i); }
    return -1;
}
static CGColorRef rgb(CGFloat r, CGFloat g, CGFloat b, CGFloat a) {
    CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB(); CGFloat v[4] = {r, g, b, a};
    CGColorRef c = CGColorCreate(cs, v); CGColorSpaceRelease(cs); return (CGColorRef)CFAutorelease(c);
}
/* Text in a y-down context: centred on x (align 0), starting at x (align -1) or ending at x (align 1), centred on y. */
static void draw_text(CGContextRef c, NSString *text, CGFloat x, CGFloat y, CGFloat size, BOOL bold, CGColorRef color, int align) {
    CTFontRef font = CTFontCreateUIFontForLanguage(bold ? kCTFontUIFontEmphasizedSystem : kCTFontUIFontSystem, size, NULL);
    NSAttributedString *s = [[NSAttributedString alloc] initWithString:text attributes:@{(id)kCTFontAttributeName: (__bridge id)font, (id)kCTForegroundColorAttributeName: (__bridge id)color}];
    CTLineRef line = CTLineCreateWithAttributedString((__bridge CFAttributedStringRef)s);
    CGFloat ascent, descent; double w = CTLineGetTypographicBounds(line, &ascent, &descent, NULL);
    CGFloat left = align < 0 ? x : align > 0 ? x - w : x - w / 2;
    CGContextSaveGState(c); CGContextSetTextMatrix(c, CGAffineTransformMakeScale(1, -1));
    CGContextSetTextPosition(c, left, y + (ascent - descent) / 2); CTLineDraw(line, c); CGContextRestoreGState(c);
    CFRelease(line); CFRelease(font);
}
static CGPDFPageRef procon_page(void) {
    static CGPDFDocumentRef doc; static dispatch_once_t once;
    dispatch_once(&once, ^{
        CGDataProviderRef d = CGDataProviderCreateWithData(NULL, procon_pdf, sizeof procon_pdf, NULL);
        doc = CGPDFDocumentCreateWithProvider(d); CGDataProviderRelease(d);
    });
    return doc ? CGPDFDocumentGetPage(doc, 1) : NULL;
}
static void draw_controller(CGContextRef c, CGRect bounds, int recording) {
    CGContextSaveGState(c); CGContextConcatCTM(c, design_to(bounds));
    CGColorRef box = rgb(.13, .13, .14, 1), edge = rgb(.36, .36, .4, 1), ink = rgb(1, 1, 1, 1), dim = rgb(.66, .66, .7, 1),
               hot = rgb(.04, .52, 1, 1), line = rgb(.55, .55, .6, .75), dot = rgb(1, 1, 1, .9);
    /* the controller (PDF, y up: flipped into this y-down space) */
    CGPDFPageRef page = procon_page();
    if (page) {
        CGRect media = CGPDFPageGetBoxRect(page, kCGPDFMediaBox);
        CGContextSaveGState(c); CGContextTranslateCTM(c, IMAGE_X, IMAGE_Y + 1050);
        CGContextScaleCTM(c, 1050 / media.size.width, -1050 / media.size.height); CGContextDrawPDFPage(c, page); CGContextRestoreGState(c);
    }
    /* lines from each box to its control */
    CGContextSetLineWidth(c, 2.5);
    for (unsigned i = 0; i < NCONTROL; i++) {
        CGRect r = box_rect(i); CGPoint p = control_point(i), from = CGPointMake(controls[i].right ? CGRectGetMinX(r) : CGRectGetMaxX(r), CGRectGetMidY(r));
        /* buttons: the line stops at the edge, so the image's own label stays readable; stick and d-pad points are exact */
        CGFloat stop = controls[i].reach >= 26 && strcmp(controls[i].id, "lstick") && strcmp(controls[i].id, "rstick") ? controls[i].reach - 4 : 0;
        CGFloat dx = from.x - p.x, dy = from.y - p.y, len = hypot(dx, dy);
        if (stop > 0 && len > stop) { p.x += dx / len * stop; p.y += dy / len * stop; }
        BOOL rec = input_of_control(i) == recording;
        CGContextSetStrokeColorWithColor(c, rec ? hot : line); CGContextMoveToPoint(c, from.x, from.y); CGContextAddLineToPoint(c, p.x, p.y); CGContextStrokePath(c);
        CGContextSetFillColorWithColor(c, rec ? hot : dot); CGContextFillEllipseInRect(c, CGRectMake(p.x - 5, p.y - 5, 10, 10));
    }
    /* the boxes: the control's name, then its keys */
    for (unsigned i = 0; i < NCONTROL; i++) {
        int in = input_of_control(i); CGRect r = box_rect(i); BOOL rec = in == recording;
        CGPathRef path = CGPathCreateWithRoundedRect(r, 14, 14, NULL);
        CGContextSetFillColorWithColor(c, rec ? hot : box); CGContextSetStrokeColorWithColor(c, rec ? hot : edge); CGContextSetLineWidth(c, 2);
        CGContextAddPath(c, path); CGContextDrawPath(c, kCGPathFillStroke); CGPathRelease(path);
        NSString *key = rec ? @"press a key" : keys_short((unsigned)in);
        draw_text(c, @(controls[i].mark), CGRectGetMinX(r) + 16, CGRectGetMidY(r), 25, NO, rec ? ink : dim, -1);
        draw_text(c, key, CGRectGetMaxX(r) - 16, CGRectGetMidY(r), rec ? 23 : 28, YES, bound[in][0] < 128 || rec ? ink : dim, 1);
    }
    CGContextRestoreGState(c);
}

/* ======================= 5. the Settings window (Mac) ======================= */
#if !TARGET_OS_IPHONE
@class GSController;
/* A scroll view's content laid out from the top, so lists open at their first row. */
@interface GSFlippedView : NSView @end
@implementation GSFlippedView
- (BOOL)isFlipped { return YES; }
@end
@interface GSDiagramView : NSView @property(weak) GSController *owner; @property int recording; @end

@interface GSController : NSObject <NSWindowDelegate>
@property(strong) NSWindow *window;
@property(strong) GSDiagramView *diagram;
@property(strong) NSMutableArray<NSButton *> *keyButtons;
@property(strong) NSMutableArray<NSPopUpButton *> *popups;
@property(strong) NSMutableArray<NSSwitch *> *switches;   /* the Off / On settings */
@property(strong) NSMutableArray<NSSlider *> *sliders;     /* settings without choice titles, with their value labels */
@property(strong) NSMutableArray<NSTextField *> *sliderLabels;
@property(strong) NSTextField *localPath, *cloudPath;
@property(strong) NSButton *cloudButton;
@property(strong) id monitor;
@property NSInteger recording, slot;   /* the input and slot being recorded, -1 none */
- (void)record:(NSInteger)i slot:(NSInteger)slot;
@end
static GSController *controller;

@implementation GSDiagramView
- (BOOL)isFlipped { return YES; }
- (void)drawRect:(NSRect)dirty { (void)dirty; draw_controller(NSGraphicsContext.currentContext.CGContext, self.bounds, self.recording); }
- (void)mouseDown:(NSEvent *)e {
    int i = part_at([self convertPoint:e.locationInWindow fromView:nil], self.bounds);
    if (i >= 0) [self.owner record:i slot:free_slot((unsigned)i)];
}
@end

@implementation GSController
- (void)refresh {
    for (NSButton *b in self.keyButtons) {   /* tag: input * MAX_KEYS + slot */
        unsigned i = (unsigned)b.tag / MAX_KEYS; int k = (int)(b.tag % MAX_KEYS);
        b.title = (NSInteger)i == self.recording && k == self.slot ? @"Press a key…" : slot_title(i, k);
    }
    for (NSSwitch *w in self.switches) { const gamesettings_choice *g = &cfg->graphics[w.tag]; if (g->get) w.state = g->get() ? NSControlStateValueOn : NSControlStateValueOff; }
    for (NSUInteger i = 0; i < self.sliders.count; i++) {
        NSSlider *s = self.sliders[i]; const gamesettings_choice *g = &cfg->graphics[s.tag];
        const int now = g->get ? g->get() : 0; s.integerValue = now; self.sliderLabels[i].stringValue = [NSString stringWithFormat:@"%d", now + 1];
    }
    for (NSPopUpButton *p in self.popups) {
        /* a setting's choices can change after the window was made (a game knows some only once its code is in) */
        const gamesettings_choice *g = &cfg->graphics[p.tag];
        BOOL same = p.numberOfItems == (NSInteger)g->nchoices;
        for (unsigned c = 0; same && c < g->nchoices; c++) same = [[p itemAtIndex:c].title isEqualToString:@(g->choices[c])];
        if (!same) { [p removeAllItems]; for (unsigned c = 0; c < g->nchoices; c++) [p addItemWithTitle:@(g->choices[c])]; }
        if (g->get) [p selectItemAtIndex:g->get()];
    }
    self.diagram.recording = (int)self.recording; self.diagram.needsDisplay = YES;
    self.localPath.stringValue = save_local ?: @"Known once the game has opened its saves.";
    self.cloudPath.stringValue = save_cloud ?: @"No iCloud copy (iCloud is off, or SaveSync has not found its folder).";
    self.cloudButton.enabled = save_cloud != nil;
}
- (void)stopRecording { if (self.monitor) [NSEvent removeMonitor:self.monitor]; self.monitor = nil; self.recording = -1; [self refresh]; }
/* The next key (modifiers too) goes to input i. ⌘-keys keep working (⌘W, ⌘Q, ⌘,). */
- (void)record:(NSInteger)i slot:(NSInteger)slot {
    [self stopRecording]; self.recording = i; self.slot = slot; [self refresh];
    __weak GSController *weak = self;
    self.monitor = [NSEvent addLocalMonitorForEventsMatchingMask:NSEventMaskKeyDown | NSEventMaskFlagsChanged handler:^NSEvent *(NSEvent *e) {
        GSController *me = weak; if (!me || me.recording < 0) return e;
        if (e.type == NSEventTypeKeyDown && (e.modifierFlags & NSEventModifierFlagCommand)) return e;
        if (e.type == NSEventTypeFlagsChanged && !(e.modifierFlags & NSEventModifierFlagDeviceIndependentFlagsMask & ~NSEventModifierFlagCapsLock)) return nil;   /* a release */
        if (!bindable(e.keyCode)) return nil;
        assign_key((unsigned)me.recording, (uint8_t)e.keyCode, (int)me.slot); [me stopRecording];
        return nil;
    }];
}
- (void)slotClicked:(NSButton *)b {
    if (b.tag / MAX_KEYS == self.recording && b.tag % MAX_KEYS == self.slot) { [self stopRecording]; return; }   /* again: cancel */
    [self record:b.tag / MAX_KEYS slot:b.tag % MAX_KEYS];
}
- (void)clearSlot:(NSMenuItem *)m { [self stopRecording]; clear_slot((unsigned)m.tag / MAX_KEYS, (int)(m.tag % MAX_KEYS)); [self refresh]; }
- (void)defaultsClicked:(id)s { (void)s; [self stopRecording]; restore_defaults(); [self refresh]; }
- (void)choiceChanged:(NSPopUpButton *)p {
    const gamesettings_choice *g = &cfg->graphics[p.tag];
    if (g->set) g->set((int)p.indexOfSelectedItem);
    save_graphics(); [self refresh];
}
- (void)switchChanged:(NSSwitch *)w {
    const gamesettings_choice *g = &cfg->graphics[w.tag];
    if (g->set) g->set(w.state == NSControlStateValueOn);
    save_graphics(); [self refresh];
}
- (void)sliderChanged:(NSSlider *)s {
    const gamesettings_choice *g = &cfg->graphics[s.tag];
    if (g->set) g->set((int)s.integerValue);
    save_graphics(); [self refresh];
}
- (void)windowWillClose:(NSNotification *)n { (void)n; [self stopRecording]; }
- (void)showLocal:(id)s { (void)s; if (save_local) [NSWorkspace.sharedWorkspace openURL:[NSURL fileURLWithPath:save_local isDirectory:YES]]; }
- (void)showCloud:(id)s { (void)s; if (save_cloud) [NSWorkspace.sharedWorkspace openURL:[NSURL fileURLWithPath:save_cloud isDirectory:YES]]; }
/* The look of System Settings: a heading, then a rounded box of rows, each with its name and note on the left and its
   control on the right; Off / On settings are switches. */
#define CONTENT_W 560
static NSTextField *hint_label(NSString *s) {
    NSTextField *t = [NSTextField wrappingLabelWithString:s]; t.textColor = NSColor.secondaryLabelColor;
    t.font = [NSFont systemFontOfSize:NSFont.smallSystemFontSize]; [t.widthAnchor constraintEqualToConstant:CONTENT_W].active = YES; return t;
}
static NSTextField *heading(NSString *s) {
    NSTextField *t = [NSTextField labelWithString:s]; t.font = [NSFont systemFontOfSize:NSFont.systemFontSize weight:NSFontWeightSemibold]; return t;
}
static NSView *top_aligned(NSView *content, BOOL scrolls) {
    NSView *doc = [GSFlippedView new]; doc.translatesAutoresizingMaskIntoConstraints = NO; content.translatesAutoresizingMaskIntoConstraints = NO;
    [doc addSubview:content];
    [NSLayoutConstraint activateConstraints:@[[content.topAnchor constraintEqualToAnchor:doc.topAnchor], [content.leadingAnchor constraintEqualToAnchor:doc.leadingAnchor],
        [content.trailingAnchor constraintLessThanOrEqualToAnchor:doc.trailingAnchor]]];
    if (!scrolls) return doc;
    [content.bottomAnchor constraintEqualToAnchor:doc.bottomAnchor].active = YES;
    NSScrollView *scroll = [NSScrollView new]; scroll.hasVerticalScroller = YES; scroll.drawsBackground = NO; scroll.documentView = doc;
    [doc.widthAnchor constraintEqualToAnchor:scroll.contentView.widthAnchor].active = YES;
    return scroll;
}
/* One row: name (and a note under it, smaller and grey) on the left, the control on the right. */
static NSView *setting_row(NSString *title, NSString *note, NSView *control) {
    NSView *row = [NSView new]; row.translatesAutoresizingMaskIntoConstraints = NO;
    NSTextField *name = [NSTextField labelWithString:title]; name.font = [NSFont systemFontOfSize:NSFont.systemFontSize];
    NSMutableArray *left = [NSMutableArray arrayWithObject:name];
    if (note.length) {
        NSTextField *n = [NSTextField wrappingLabelWithString:note]; n.textColor = NSColor.secondaryLabelColor;
        n.font = [NSFont systemFontOfSize:NSFont.smallSystemFontSize]; n.preferredMaxLayoutWidth = 330; [left addObject:n];
    }
    NSStackView *text = [NSStackView stackViewWithViews:left]; text.orientation = NSUserInterfaceLayoutOrientationVertical;
    text.alignment = NSLayoutAttributeLeading; text.spacing = 3; text.translatesAutoresizingMaskIntoConstraints = NO;
    control.translatesAutoresizingMaskIntoConstraints = NO;   /* as wide as its text, right-aligned */
    [control setContentHuggingPriority:NSLayoutPriorityDefaultHigh forOrientation:NSLayoutConstraintOrientationHorizontal];
    [row addSubview:text]; [row addSubview:control];
    [NSLayoutConstraint activateConstraints:@[
        [text.leadingAnchor constraintEqualToAnchor:row.leadingAnchor constant:14], [text.widthAnchor constraintLessThanOrEqualToConstant:340],
        [text.topAnchor constraintEqualToAnchor:row.topAnchor constant:10], [text.bottomAnchor constraintEqualToAnchor:row.bottomAnchor constant:-10],
        [control.trailingAnchor constraintEqualToAnchor:row.trailingAnchor constant:-14], [control.centerYAnchor constraintEqualToAnchor:row.centerYAnchor],
        [control.leadingAnchor constraintGreaterThanOrEqualToAnchor:text.trailingAnchor constant:16],
        [row.heightAnchor constraintGreaterThanOrEqualToConstant:40]]];
    return row;
}
/* Rows in a rounded box, a hairline between each two. */
static NSView *group_box(NSArray<NSView *> *rows) {
    NSBox *box = [NSBox new]; box.boxType = NSBoxCustom; box.titlePosition = NSNoTitle; box.cornerRadius = 10;
    box.borderWidth = 0; box.fillColor = [NSColor.labelColor colorWithAlphaComponent:0.05]; box.contentViewMargins = NSZeroSize;   /* no outline */
    box.translatesAutoresizingMaskIntoConstraints = NO; [box.widthAnchor constraintEqualToConstant:CONTENT_W].active = YES;
    NSMutableArray *views = [NSMutableArray array];
    for (NSView *r in rows) {
        if (views.count) { NSBox *line = [NSBox new]; line.boxType = NSBoxSeparator; [views addObject:line]; }
        [views addObject:r];
    }
    NSStackView *stack = [NSStackView stackViewWithViews:views]; stack.orientation = NSUserInterfaceLayoutOrientationVertical; stack.spacing = 0;
    stack.alignment = NSLayoutAttributeWidth; stack.translatesAutoresizingMaskIntoConstraints = NO;
    [box.contentView addSubview:stack];
    for (NSView *v in views) if ([v isKindOfClass:NSBox.class]) {   /* hairlines start under the text, as in System Settings */
        [v.leadingAnchor constraintEqualToAnchor:stack.leadingAnchor constant:14].active = YES; [v.trailingAnchor constraintEqualToAnchor:stack.trailingAnchor].active = YES; }
    [NSLayoutConstraint activateConstraints:@[[stack.topAnchor constraintEqualToAnchor:box.contentView.topAnchor], [stack.bottomAnchor constraintEqualToAnchor:box.contentView.bottomAnchor],
        [stack.leadingAnchor constraintEqualToAnchor:box.contentView.leadingAnchor], [stack.trailingAnchor constraintEqualToAnchor:box.contentView.trailingAnchor]]];
    return box;
}
static BOOL is_switch(const gamesettings_choice *g) {
    return g->choices && g->nchoices == 2 && !strcmp(g->choices[0], "Off") && !strcmp(g->choices[1], "On");
}
/* The game's rows for one tab (controls: the Controls tab's) as headings and boxes; empty when it has none. */
- (NSArray<NSView *> *)groups:(BOOL)controls {
    if (!self.popups) { self.popups = [NSMutableArray array]; self.switches = [NSMutableArray array]; self.sliders = [NSMutableArray array]; self.sliderLabels = [NSMutableArray array]; }
    NSMutableArray *out = [NSMutableArray array], *rows = [NSMutableArray array];
    for (unsigned i = 0; cfg && i < cfg->ngraphics; i++) {
        const gamesettings_choice *g = &cfg->graphics[i];
        if (!g->controls != !controls) continue;
        if (g->group && rows.count) { [out addObject:group_box(rows)]; rows = [NSMutableArray array]; }
        if (g->group) [out addObject:heading(@(g->group))];
        NSControl *c;
        if (!g->choices) {
            NSSlider *s = [NSSlider sliderWithValue:0 minValue:0 maxValue:g->nchoices - 1 target:self action:@selector(sliderChanged:)];
            s.tag = i; [s.widthAnchor constraintEqualToConstant:160].active = YES;
            NSTextField *label = [NSTextField labelWithString:@""]; label.alignment = NSTextAlignmentRight; label.font = [NSFont monospacedDigitSystemFontOfSize:NSFont.systemFontSize weight:NSFontWeightRegular];
            [label.widthAnchor constraintEqualToConstant:28].active = YES;
            [self.sliders addObject:s]; [self.sliderLabels addObject:label];
            [rows addObject:setting_row(@(g->title), g->note ? @(g->note) : nil, [NSStackView stackViewWithViews:@[s, label]])];
            continue;
        }
        if (is_switch(g)) { NSSwitch *w = [NSSwitch new]; w.target = self; w.action = @selector(switchChanged:); [self.switches addObject:w]; c = w; }
        else {
            NSPopUpButton *p = [NSPopUpButton new]; p.target = self; p.action = @selector(choiceChanged:);
            for (unsigned k = 0; k < g->nchoices; k++) [p addItemWithTitle:@(g->choices[k])];
            [self.popups addObject:p]; c = p;
        }
        c.tag = i;
        [rows addObject:setting_row(@(g->title), g->note ? @(g->note) : nil, c)];
    }
    if (rows.count) [out addObject:group_box(rows)];
    return out;
}
static NSStackView *page_stack(NSArray<NSView *> *views) {
    NSStackView *stack = [NSStackView stackViewWithViews:views];
    stack.orientation = NSUserInterfaceLayoutOrientationVertical; stack.alignment = NSLayoutAttributeLeading; stack.spacing = 8;
    stack.edgeInsets = NSEdgeInsetsMake(20, 30, 24, 30);
    for (NSView *v in views) if ([v isKindOfClass:NSBox.class]) [stack setCustomSpacing:22 afterView:v];   /* room before the next heading */
    return stack;
}
/* The game's control settings (mouse, controller type), then the keyboard bindings. */
- (NSView *)controlsTab {
    NSMutableArray<NSView *> *views = [NSMutableArray arrayWithArray:[self groups:YES]];
    [views addObject:heading(@"Keyboard")];
    self.diagram = [GSDiagramView new]; self.diagram.owner = self; self.diagram.recording = -1;
    [self.diagram.widthAnchor constraintEqualToConstant:540].active = YES; [self.diagram.heightAnchor constraintEqualToConstant:540 * DESIGN_H / DESIGN_W].active = YES;
    NSStackView *grid = [NSStackView new]; grid.orientation = NSUserInterfaceLayoutOrientationVertical; grid.alignment = NSLayoutAttributeLeading; grid.spacing = 4;
    self.keyButtons = [NSMutableArray array];
    for (unsigned i = 0; i < NINPUT; i++) {   /* MAX_KEYS slots per input: click one, press the key; right-click clears it */
        NSTextField *name = [NSTextField labelWithString:@(inputs[i].title)]; name.alignment = NSTextAlignmentRight;
        [name.widthAnchor constraintEqualToConstant:130].active = YES;
        NSMutableArray *row = [NSMutableArray arrayWithObject:name];
        for (int k = 0; k < MAX_KEYS; k++) {
            NSButton *slot = [NSButton buttonWithTitle:@"" target:self action:@selector(slotClicked:)]; slot.tag = i * MAX_KEYS + k;
            [slot.widthAnchor constraintEqualToConstant:118].active = YES;
            NSMenu *menu = [NSMenu new]; NSMenuItem *clear = [menu addItemWithTitle:@"Clear" action:@selector(clearSlot:) keyEquivalent:@""];
            clear.target = self; clear.tag = slot.tag; slot.menu = menu;
            [row addObject:slot]; [self.keyButtons addObject:slot];
        }
        NSStackView *line = [NSStackView stackViewWithViews:row]; line.spacing = 8; [grid addView:line inGravity:NSStackViewGravityTop];
    }
    NSButton *defaults = [NSButton buttonWithTitle:@"Restore Defaults" target:self action:@selector(defaultsClicked:)];
    [views addObjectsFromArray:@[hint_label(@"Each action has three slots: click one (or a button on the controller), then press the key. Right-click a slot to clear it. Used by every game on this Mac; key 1 stays the Settings key."), self.diagram, grid, defaults]];
    return top_aligned(page_stack(views), YES);
}
- (NSView *)graphicsTab {
    NSMutableArray<NSView *> *views = [NSMutableArray arrayWithArray:[self groups:NO]];
    [views addObject:hint_label(@"Settings stored locally, auto saved.")];
    return top_aligned(page_stack(views), YES);
}
static NSTextField *path_label(void) {
    NSTextField *t = [NSTextField labelWithString:@""]; t.selectable = YES; t.lineBreakMode = NSLineBreakByTruncatingMiddle;
    t.font = [NSFont monospacedSystemFontOfSize:NSFont.smallSystemFontSize weight:NSFontWeightRegular]; t.textColor = NSColor.secondaryLabelColor;
    [t setContentCompressionResistancePriority:NSLayoutPriorityDefaultLow forOrientation:NSLayoutConstraintOrientationHorizontal]; return t;
}
/* A row whose note is a folder path (filled in by refresh). */
static NSView *folder_row(NSString *title, NSTextField *path, NSButton *button) {
    NSView *row = setting_row(title, @" ", button);
    NSStackView *text = (NSStackView *)row.subviews.firstObject;
    [text removeView:text.views.lastObject]; [text addView:path inGravity:NSStackViewGravityTop];
    return row;
}
- (NSView *)savesTab {
    self.localPath = path_label(); self.cloudPath = path_label();
    NSButton *showLocal = [NSButton buttonWithTitle:@"Show in Finder" target:self action:@selector(showLocal:)];
    self.cloudButton = [NSButton buttonWithTitle:@"Show in Finder" target:self action:@selector(showCloud:)];
    NSArray *views = @[heading(@"Save folders"),
                       group_box(@[folder_row(@"On this Mac", self.localPath, showLocal), folder_row(@"In iCloud (Game Saves)", self.cloudPath, self.cloudButton)]),
                       hint_label(saves_note())];
    return top_aligned(page_stack(views), NO);
}
- (void)show {
    if (!self.window) {
        self.recording = -1;
        NSTabViewController *tabs = [NSTabViewController new]; tabs.tabStyle = NSTabViewControllerTabStyleToolbar;
        tabs.canPropagateSelectedChildViewControllerTitle = NO;
        NSArray *pages = @[@[@"Graphics", @"display", [self graphicsTab]], @[@"Controls", @"gamecontroller", [self controlsTab]],
                           @[@"Saves", @"externaldrive.badge.icloud", [self savesTab]]];
        for (NSArray *p in pages) {
            NSViewController *vc = [NSViewController new]; vc.view = p[2]; vc.title = p[0]; vc.preferredContentSize = NSMakeSize(CONTENT_W + 60, 720);
            NSTabViewItem *item = [NSTabViewItem tabViewItemWithViewController:vc]; item.label = p[0];
            item.image = [NSImage imageWithSystemSymbolName:p[1] accessibilityDescription:p[0]];
            [tabs addTabViewItem:item];
        }
        self.window = [NSWindow windowWithContentViewController:tabs];
        self.window.styleMask = NSWindowStyleMaskTitled | NSWindowStyleMaskClosable | NSWindowStyleMaskMiniaturizable | NSWindowStyleMaskResizable;
        self.window.toolbarStyle = NSWindowToolbarStylePreference;
        self.window.title = [NSString stringWithFormat:@"%s Settings", cfg && cfg->game_name ? cfg->game_name : "Game"];
        [self.window setContentSize:NSMakeSize(CONTENT_W + 60, 720)]; self.window.contentMinSize = NSMakeSize(CONTENT_W + 60, 360);
        self.window.releasedWhenClosed = NO; self.window.delegate = self; [self.window center];
    }
    [self refresh]; [self.window makeKeyAndOrderFront:nil]; [NSApp activateIgnoringOtherApps:YES];
}
- (void)showFromMenu:(id)s { (void)s; [self show]; }
@end

void gamesettings_show(void) {
    dispatch_async(dispatch_get_main_queue(), ^{ if (!controller) controller = [GSController new]; [controller show]; });
}
void gamesettings_add_menu_item(NSMenu *app_menu) {
    if (!controller) controller = [GSController new];
    NSMenuItem *item = [[NSMenuItem alloc] initWithTitle:@"Settings…" action:@selector(showFromMenu:) keyEquivalent:@","];
    item.keyEquivalentModifierMask = NSEventModifierFlagCommand; item.target = controller;
    [app_menu insertItem:item atIndex:0]; [app_menu insertItem:NSMenuItem.separatorItem atIndex:1];
}

/* ======================= 5. the Settings sheet (iPhone / iPad) ======================= */
#else
@class GSPhoneController;
@interface GSTouchDiagram : UIView @property(weak) GSPhoneController *owner; @property int recording; @end

/* Graphics, Controls and Saves in one sheet. Keys are recorded from a hardware keyboard (Magic Keyboard, Bluetooth). */
@interface GSPhoneController : UIViewController
@property(strong) NSMutableArray<UISlider *> *sliders;     /* settings without choice titles, with their value labels */
@property(strong) NSMutableArray<UILabel *> *sliderLabels;
@property(strong) UISegmentedControl *tabs;
@property(strong) UIScrollView *graphicsPage, *controlsPage, *savesPage;
@property(strong) UILabel *localPath, *cloudPath;
@property(strong) UIButton *cloudButton;
@property(strong) GSTouchDiagram *diagram;
@property(strong) NSMutableArray<UIButton *> *keyButtons, *choiceButtons;
@property(strong) NSMutableArray<UISwitch *> *switches;   /* the Off / On settings */
@property NSInteger recording, slot;   /* the input and slot being recorded, -1 none */
- (void)record:(NSInteger)i slot:(NSInteger)slot;
@end

@implementation GSTouchDiagram
- (void)drawRect:(CGRect)dirty { (void)dirty; draw_controller(UIGraphicsGetCurrentContext(), self.bounds, self.recording); }
- (void)touchesEnded:(NSSet<UITouch *> *)touches withEvent:(UIEvent *)event {
    (void)event; int i = part_at([touches.anyObject locationInView:self], self.bounds);
    if (i >= 0) [self.owner record:i slot:free_slot((unsigned)i)];
}
@end

@implementation GSPhoneController
- (BOOL)canBecomeFirstResponder { return YES; }
- (void)viewDidAppear:(BOOL)animated { [super viewDidAppear:animated]; [self becomeFirstResponder]; }
static UILabel *note_label(NSString *s) {
    UILabel *l = [UILabel new]; l.text = s; l.numberOfLines = 0; l.textColor = UIColor.secondaryLabelColor; l.font = [UIFont preferredFontForTextStyle:UIFontTextStyleFootnote]; return l;
}
static UIScrollView *page(NSArray<UIView *> *views) {
    UIStackView *stack = [[UIStackView alloc] initWithArrangedSubviews:views]; stack.axis = UILayoutConstraintAxisVertical; stack.spacing = 12;
    stack.alignment = UIStackViewAlignmentFill; stack.translatesAutoresizingMaskIntoConstraints = NO;
    for (UIView *v in views) if (v.layer.cornerRadius > 0) [stack setCustomSpacing:24 afterView:v];   /* room before the next heading */
    UIScrollView *scroll = [UIScrollView new]; scroll.translatesAutoresizingMaskIntoConstraints = NO; [scroll addSubview:stack];
    UILayoutGuide *c = scroll.contentLayoutGuide, *f = scroll.frameLayoutGuide;
    [NSLayoutConstraint activateConstraints:@[[stack.topAnchor constraintEqualToAnchor:c.topAnchor constant:16], [stack.bottomAnchor constraintEqualToAnchor:c.bottomAnchor constant:-16],
        [stack.leadingAnchor constraintEqualToAnchor:f.leadingAnchor constant:20], [stack.trailingAnchor constraintEqualToAnchor:f.trailingAnchor constant:-20]]];
    return scroll;
}
- (UIMenu *)menuFor:(unsigned)i {
    const gamesettings_choice *g = &cfg->graphics[i]; int now = g->get ? g->get() : -1; NSMutableArray *actions = [NSMutableArray array];
    __weak GSPhoneController *weak = self;
    for (unsigned c = 0; c < g->nchoices; c++) {
        UIAction *a = [UIAction actionWithTitle:@(g->choices[c]) image:nil identifier:nil handler:^(UIAction *x) { (void)x;
            if (g->set) g->set((int)c); save_graphics(); [weak refresh]; }];
        a.state = (int)c == now ? UIMenuElementStateOn : UIMenuElementStateOff; [actions addObject:a];
    }
    return [UIMenu menuWithChildren:actions];
}
/* As the Mac window: a heading, then a rounded box of rows, each with its name and note on the left and its control on
   the right; Off / On settings are switches. */
static UILabel *heading_label(NSString *s) {
    UILabel *h = [UILabel new]; h.text = s; h.font = [UIFont preferredFontForTextStyle:UIFontTextStyleHeadline]; return h;
}
static UIView *setting_row(NSString *title, UILabel *note, UIView *control) {
    UILabel *name = [UILabel new]; name.text = title; name.numberOfLines = 0; name.font = [UIFont preferredFontForTextStyle:UIFontTextStyleBody];
    UIStackView *text = [[UIStackView alloc] initWithArrangedSubviews:note ? @[name, note] : @[name]]; text.axis = UILayoutConstraintAxisVertical; text.spacing = 3;
    [text setContentCompressionResistancePriority:UILayoutPriorityDefaultLow forAxis:UILayoutConstraintAxisHorizontal];
    [control setContentHuggingPriority:UILayoutPriorityRequired forAxis:UILayoutConstraintAxisHorizontal];
    [control setContentCompressionResistancePriority:UILayoutPriorityRequired forAxis:UILayoutConstraintAxisHorizontal];
    UIStackView *r = [[UIStackView alloc] initWithArrangedSubviews:@[text, control]]; r.spacing = 16; r.alignment = UIStackViewAlignmentCenter;
    r.layoutMargins = UIEdgeInsetsMake(11, 16, 11, 16); r.layoutMarginsRelativeArrangement = YES;
    return r;
}
static UIView *group_box(NSArray<UIView *> *rows) {
    NSMutableArray *views = [NSMutableArray array];
    for (UIView *r in rows) {
        if (views.count) {   /* a hairline, starting under the text */
            UIView *line = [UIView new], *pad = [UIView new]; line.backgroundColor = UIColor.separatorColor; line.translatesAutoresizingMaskIntoConstraints = NO;
            [pad addSubview:line];
            [NSLayoutConstraint activateConstraints:@[[pad.heightAnchor constraintEqualToConstant:1.0 / UIScreen.mainScreen.scale],
                [line.topAnchor constraintEqualToAnchor:pad.topAnchor], [line.bottomAnchor constraintEqualToAnchor:pad.bottomAnchor],
                [line.leadingAnchor constraintEqualToAnchor:pad.leadingAnchor constant:16], [line.trailingAnchor constraintEqualToAnchor:pad.trailingAnchor]]];
            [views addObject:pad];
        }
        [views addObject:r];
    }
    UIStackView *box = [[UIStackView alloc] initWithArrangedSubviews:views]; box.axis = UILayoutConstraintAxisVertical;
    box.backgroundColor = UIColor.secondarySystemGroupedBackgroundColor; box.layer.cornerRadius = 10; box.layer.cornerCurve = kCACornerCurveContinuous; box.clipsToBounds = YES;
    return box;
}
static BOOL is_switch(const gamesettings_choice *g) {
    return g->choices && g->nchoices == 2 && !strcmp(g->choices[0], "Off") && !strcmp(g->choices[1], "On");
}
/* The game's rows for one tab (controls: the Controls tab's) as headings and boxes. */
- (NSArray<UIView *> *)groups:(BOOL)controls {
    NSMutableArray *out = [NSMutableArray array], *rows = [NSMutableArray array];
    for (unsigned i = 0; cfg && i < cfg->ngraphics; i++) {
        const gamesettings_choice *g = &cfg->graphics[i];
        if (!g->controls != !controls) continue;
        if (g->group && rows.count) { [out addObject:group_box(rows)]; rows = [NSMutableArray array]; }
        if (g->group) [out addObject:heading_label(@(g->group))];
        UIView *c;
        if (!g->choices) {
            UISlider *s = [UISlider new]; s.tag = i; s.minimumValue = 0; s.maximumValue = g->nchoices - 1; [s addTarget:self action:@selector(sliderChanged:) forControlEvents:UIControlEventValueChanged];
            [s.widthAnchor constraintEqualToConstant:130].active = YES;
            UILabel *label = [UILabel new]; label.textAlignment = NSTextAlignmentRight; label.font = [UIFont monospacedDigitSystemFontOfSize:UIFont.labelFontSize weight:UIFontWeightRegular];
            [label.widthAnchor constraintEqualToConstant:32].active = YES;
            [self.sliders addObject:s]; [self.sliderLabels addObject:label];
            UIStackView *both = [[UIStackView alloc] initWithArrangedSubviews:@[s, label]]; both.spacing = 8; both.alignment = UIStackViewAlignmentCenter; c = both;
        } else if (is_switch(g)) {
            UISwitch *w = [UISwitch new]; w.tag = i; [w addTarget:self action:@selector(switchChanged:) forControlEvents:UIControlEventValueChanged];
            [self.switches addObject:w]; c = w;
        } else {
            UIButton *b = [UIButton buttonWithConfiguration:UIButtonConfiguration.grayButtonConfiguration primaryAction:nil];
            b.showsMenuAsPrimaryAction = YES; b.changesSelectionAsPrimaryAction = NO; b.tag = i; [self.choiceButtons addObject:b]; c = b;
        }
        [rows addObject:setting_row(@(g->title), g->note ? note_label(@(g->note)) : nil, c)];
    }
    if (rows.count) [out addObject:group_box(rows)];
    return out;
}
- (void)switchChanged:(UISwitch *)w {
    const gamesettings_choice *g = &cfg->graphics[w.tag];
    if (g->set) g->set(w.on ? 1 : 0);
    save_graphics(); [self refresh];
}
- (void)sliderChanged:(UISlider *)s {
    const gamesettings_choice *g = &cfg->graphics[s.tag];
    if (g->set) g->set((int)lroundf(s.value));
    save_graphics(); [self refresh];
}
- (UIButton *)button:(NSString *)title action:(SEL)action tag:(NSInteger)tag {
    UIButtonConfiguration *c = UIButtonConfiguration.grayButtonConfiguration; c.title = title;
    UIButton *b = [UIButton buttonWithConfiguration:c primaryAction:nil]; b.tag = tag; [b addTarget:self action:action forControlEvents:UIControlEventTouchUpInside]; return b;
}
- (void)viewDidLoad {
    [super viewDidLoad];
    self.view.backgroundColor = UIColor.systemGroupedBackgroundColor; self.recording = -1;
    self.title = [NSString stringWithFormat:@"%s Settings", cfg && cfg->game_name ? cfg->game_name : "Game"];
    self.navigationItem.rightBarButtonItem = [[UIBarButtonItem alloc] initWithBarButtonSystemItem:UIBarButtonSystemItemDone target:self action:@selector(done)];
    /* Graphics */
    self.choiceButtons = [NSMutableArray array]; self.switches = [NSMutableArray array]; self.sliders = [NSMutableArray array]; self.sliderLabels = [NSMutableArray array];
    NSMutableArray *g = [NSMutableArray arrayWithArray:[self groups:NO]];
    [g addObject:note_label(@"Settings stored locally, auto saved.")];
    self.graphicsPage = page(g);
    /* Controls: the game's control rows (motion, controller) above, then the keyboard */
    NSMutableArray *k = [NSMutableArray arrayWithArray:[self groups:YES]];
    [k addObject:heading_label(@"Keyboard")];
    self.diagram = [GSTouchDiagram new]; self.diagram.owner = self; self.diagram.recording = -1; self.diagram.backgroundColor = UIColor.clearColor;
    [self.diagram.heightAnchor constraintEqualToAnchor:self.diagram.widthAnchor multiplier:DESIGN_H / DESIGN_W].active = YES;
    [k addObjectsFromArray:@[note_label(@"With a keyboard connected: each action has three slots. Tap one (or a button on the controller), then press the key; touch and hold a slot to clear it. Shared with every game on this device."), self.diagram]];
    self.keyButtons = [NSMutableArray array];
    for (unsigned i = 0; i < NINPUT; i++) {   /* MAX_KEYS slots per input: tap one, press the key; a long press clears it */
        UILabel *name = [UILabel new]; name.text = @(inputs[i].title); [name.widthAnchor constraintEqualToConstant:150].active = YES;
        NSMutableArray *slots = [NSMutableArray array];
        for (int s = 0; s < MAX_KEYS; s++) {
            NSInteger tag = i * MAX_KEYS + s;
            UIButton *b = [self button:@"" action:@selector(slotTapped:) tag:tag]; [self.keyButtons addObject:b]; [slots addObject:b];
            __weak GSPhoneController *weak = self;
            b.menu = [UIMenu menuWithChildren:@[[UIAction actionWithTitle:@"Clear" image:nil identifier:nil handler:^(UIAction *a) { (void)a;
                weak.recording = -1; clear_slot((unsigned)tag / MAX_KEYS, (int)(tag % MAX_KEYS)); [weak refresh]; }]]];
        }
        UIStackView *keys = [[UIStackView alloc] initWithArrangedSubviews:slots]; keys.spacing = 8; keys.distribution = UIStackViewDistributionFillEqually;
        UIStackView *r = [[UIStackView alloc] initWithArrangedSubviews:@[name, keys]]; r.spacing = 8; r.alignment = UIStackViewAlignmentCenter;
        [k addObject:r];
    }
    [k addObject:[self button:@"Restore Defaults" action:@selector(defaultsTapped) tag:0]];
    self.controlsPage = page(k);
    /* Saves: the game's folders, opened in the Files app */
    self.localPath = note_label(@""); self.cloudPath = note_label(@"");
    for (UILabel *p in @[self.localPath, self.cloudPath]) {
        p.font = [UIFont monospacedSystemFontOfSize:[UIFont preferredFontForTextStyle:UIFontTextStyleFootnote].pointSize weight:UIFontWeightRegular];
        p.numberOfLines = 2; p.lineBreakMode = NSLineBreakByTruncatingMiddle;
    }
    self.cloudButton = [self button:@"Open in Files" action:@selector(openCloud) tag:0];
    self.savesPage = page(@[heading_label(@"Save folders"),
        group_box(@[setting_row(@"On this device", self.localPath, [self button:@"Open in Files" action:@selector(openLocal) tag:0]),
                    setting_row(@"In iCloud (Game Saves)", self.cloudPath, self.cloudButton)]),
        note_label(saves_note())]);
    self.tabs = [[UISegmentedControl alloc] initWithItems:@[@"Graphics", @"Controls", @"Saves"]]; self.tabs.selectedSegmentIndex = 0;
    [self.tabs addTarget:self action:@selector(tabChanged) forControlEvents:UIControlEventValueChanged]; self.tabs.translatesAutoresizingMaskIntoConstraints = NO;
    for (UIView *v in @[self.tabs, self.graphicsPage, self.controlsPage, self.savesPage]) [self.view addSubview:v];
    UILayoutGuide *safe = self.view.safeAreaLayoutGuide;
    [NSLayoutConstraint activateConstraints:@[[self.tabs.topAnchor constraintEqualToAnchor:safe.topAnchor constant:12], [self.tabs.centerXAnchor constraintEqualToAnchor:safe.centerXAnchor]]];
    for (UIScrollView *p in @[self.graphicsPage, self.controlsPage, self.savesPage])
        [NSLayoutConstraint activateConstraints:@[[p.topAnchor constraintEqualToAnchor:self.tabs.bottomAnchor constant:8], [p.bottomAnchor constraintEqualToAnchor:self.view.bottomAnchor],
            [p.leadingAnchor constraintEqualToAnchor:safe.leadingAnchor], [p.trailingAnchor constraintEqualToAnchor:safe.trailingAnchor]]];
    [self tabChanged]; [self refresh];
}
- (void)tabChanged {
    NSInteger t = self.tabs.selectedSegmentIndex;
    self.graphicsPage.hidden = t != 0; self.controlsPage.hidden = t != 1; self.savesPage.hidden = t != 2; [self refresh];
}
/* The Files app at a folder of this app (its Documents are shared: UIFileSharingEnabled) or of iCloud Drive. */
static void open_in_files(NSString *path) {
    if (!path) return;
    NSString *escaped = [path stringByAddingPercentEncodingWithAllowedCharacters:NSCharacterSet.URLPathAllowedCharacterSet];
    [UIApplication.sharedApplication openURL:[NSURL URLWithString:[@"shareddocuments://" stringByAppendingString:escaped]] options:@{} completionHandler:nil];
}
- (void)openLocal { open_in_files(save_local); }
- (void)openCloud { open_in_files(save_cloud); }
- (void)refresh {
    for (UIButton *b in self.keyButtons) {   /* tag: input * MAX_KEYS + slot */
        unsigned i = (unsigned)b.tag / MAX_KEYS; int k = (int)(b.tag % MAX_KEYS); BOOL rec = (NSInteger)i == self.recording && k == self.slot;
        UIButtonConfiguration *c = b.configuration; c.title = rec ? @"Press a key…" : slot_title(i, k);
        c.baseBackgroundColor = rec ? UIColor.systemBlueColor : nil; b.configuration = c;
    }
    for (UISwitch *w in self.switches) { const gamesettings_choice *g = &cfg->graphics[w.tag]; if (g->get) w.on = g->get() != 0; }
    for (NSUInteger i = 0; i < self.sliders.count; i++) {
        UISlider *s = self.sliders[i]; const gamesettings_choice *g = &cfg->graphics[s.tag];
        const int now = g->get ? g->get() : 0; s.value = now; self.sliderLabels[i].text = [NSString stringWithFormat:@"%d", now + 1];
    }
    for (UIButton *b in self.choiceButtons) {
        const gamesettings_choice *g = &cfg->graphics[b.tag]; int now = g->get ? g->get() : -1;
        UIButtonConfiguration *c = b.configuration; c.title = now >= 0 && (unsigned)now < g->nchoices ? @(g->choices[now]) : @"—";
        b.configuration = c; b.menu = [self menuFor:(unsigned)b.tag];
    }
    self.diagram.recording = (int)self.recording; [self.diagram setNeedsDisplay];
    self.localPath.text = save_local ?: @"Known once the game has opened its saves.";
    self.cloudPath.text = save_cloud ?: @"No iCloud copy (iCloud is off, or SaveSync has not found its folder).";
    self.cloudButton.enabled = save_cloud != nil;
}
- (void)record:(NSInteger)i slot:(NSInteger)slot { self.recording = i; self.slot = slot; [self becomeFirstResponder]; [self refresh]; }
- (void)slotTapped:(UIButton *)b {
    if (b.tag / MAX_KEYS == self.recording && b.tag % MAX_KEYS == self.slot) { self.recording = -1; [self refresh]; return; }   /* again: cancel */
    [self record:b.tag / MAX_KEYS slot:b.tag % MAX_KEYS];
}
- (void)defaultsTapped { self.recording = -1; restore_defaults(); [self refresh]; }
- (void)done { [self dismissViewControllerAnimated:YES completion:nil]; }
- (void)pressesBegan:(NSSet<UIPress *> *)presses withEvent:(UIPressesEvent *)event {
    if (self.recording < 0) { [super pressesBegan:presses withEvent:event]; return; }
    for (UIPress *p in presses) {
        if (!p.key || (p.key.modifierFlags & UIKeyModifierCommand)) continue;
        int code = gamesettings_mac_key_from_hid(p.key.keyCode); if (!bindable(code)) continue;
        assign_key((unsigned)self.recording, (uint8_t)code, (int)self.slot); self.recording = -1; [self refresh]; return;
    }
    [super pressesBegan:presses withEvent:event];
}
@end

void gamesettings_show(void) {
    dispatch_async(dispatch_get_main_queue(), ^{
        UIWindow *key = nil;
        for (UIScene *s in UIApplication.sharedApplication.connectedScenes)
            if ([s isKindOfClass:UIWindowScene.class]) for (UIWindow *w in ((UIWindowScene *)s).windows) if (w.isKeyWindow || !key) key = w;
        UIViewController *top = key.rootViewController; while (top.presentedViewController) top = top.presentedViewController;
        if (!top || [top isKindOfClass:UINavigationController.class]) return;   /* already open */
        UINavigationController *nav = [[UINavigationController alloc] initWithRootViewController:[GSPhoneController new]];
        nav.modalPresentationStyle = UIModalPresentationFormSheet; [top presentViewController:nav animated:YES completion:nil];
    });
}
#endif
