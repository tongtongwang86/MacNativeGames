/* SaveSync implementation. Layers, bottom to top (ARCHITECTURE.md has the whole picture):
     1. sets      - a folder's files as {relative path: bytes}, their hash, writing and backing them up
     2. store     - where the cloud copy is (iOS ubiquity container, the Mac's ~/Library/Mobile Documents) and reading
                    and writing it: files/ first, manifest.json last, so a half-arrived copy never hashes to its manifest
     3. decision  - savesync_decide: a pure function of what both sides hold and what they last agreed on
     4. scheduler - one serial queue: the first sync, uploads a few seconds after each commit, flushes
   Nothing here is about a particular game; the port's savesync_config says where its saves are. */
#import <Foundation/Foundation.h>
#include <CommonCrypto/CommonDigest.h>
#include <TargetConditionals.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "savesync.h"

#define ROOT_FOLDER @"SaveSync"
#define FORMAT 1

static savesync_config cfg;
static NSString *tag, *title, *version, *save_dir, *state_dir, *container, *cloud_dir;   /* nil cloud_dir: off */
static NSArray<NSString *> *excluded;   /* cfg.exclude, copied */
static dispatch_queue_t queue;
static uint64_t push_generation;
static _Atomic uint64_t uploads_asked, uploads_done;   /* savesync_busy: commits asked for an upload that has not run yet */

/* A path under the save folder that is not a save (savesync_config.exclude). */
static BOOL is_excluded(NSString *rel) {
    for (NSString *x in excluded) if ([x hasSuffix:@"/"] ? [rel hasPrefix:x] || [[rel stringByAppendingString:@"/"] isEqualToString:x] : [rel isEqualToString:x]) return YES;
    return NO;
}

static void say(NSString *s) { fprintf(stderr, "%s iCloud saves: %s\n", tag.UTF8String ?: "[savesync]", s.UTF8String); }

/* ======================= 1. sets ======================= */
typedef NSDictionary<NSString *, NSData *> Set;
/* Every regular file under dir, without hidden files (.DS_Store, iCloud placeholders) and, with only_saves, without the
   paths savesync_config.exclude names; nil when one cannot be read. */
static Set *read_set(NSString *dir, BOOL only_saves) {
    NSMutableDictionary *set = [NSMutableDictionary dictionary];
    NSDirectoryEnumerator *e = [NSFileManager.defaultManager enumeratorAtPath:dir];
    for (NSString *rel in e) {
        BOOL folder = [e.fileAttributes[NSFileType] isEqual:NSFileTypeDirectory];
        if ([rel.lastPathComponent hasPrefix:@"."] || (only_saves && is_excluded(folder ? [rel stringByAppendingString:@"/"] : rel))) { if (folder) [e skipDescendants]; continue; }
        if (![e.fileAttributes[NSFileType] isEqual:NSFileTypeRegular]) continue;
        NSData *d = [NSData dataWithContentsOfFile:[dir stringByAppendingPathComponent:rel] options:0 error:nil];
        if (!d) return nil;
        set[rel] = d;
    }
    return set;
}
/* SHA-256 over (path NUL, size, bytes) of every file in path order: equal sets, equal hashes, on any device. */
static NSString *hash_set(Set *set) {
    CC_SHA256_CTX c; CC_SHA256_Init(&c);
    for (NSString *rel in [set.allKeys sortedArrayUsingSelector:@selector(compare:)]) {
        const char *p = rel.UTF8String; CC_SHA256_Update(&c, p, (CC_LONG)strlen(p) + 1);
        uint64_t n = set[rel].length; CC_SHA256_Update(&c, &n, sizeof n);
        CC_SHA256_Update(&c, set[rel].bytes, (CC_LONG)set[rel].length);
    }
    unsigned char md[CC_SHA256_DIGEST_LENGTH]; CC_SHA256_Final(md, &c);
    NSMutableString *s = [NSMutableString string]; for (int i = 0; i < CC_SHA256_DIGEST_LENGTH; i++) [s appendFormat:@"%02x", md[i]];
    return s;
}
static NSDate *newest_change(NSString *dir) {
    NSDate *newest = NSDate.distantPast; NSDirectoryEnumerator *e = [NSFileManager.defaultManager enumeratorAtPath:dir];
    for (NSString *rel in e) {
        BOOL folder = [e.fileAttributes[NSFileType] isEqual:NSFileTypeDirectory];
        if (is_excluded(folder ? [rel stringByAppendingString:@"/"] : rel)) { if (folder) [e skipDescendants]; continue; }   /* a cache rewritten at start is no save */
        NSDate *m = e.fileAttributes[NSFileModificationDate]; if (m && [m compare:newest] == NSOrderedDescending) newest = m;
    }
    return newest;
}
/* Makes dir hold exactly set: unchanged files are not rewritten, the rest atomically, stale files and folders go
   (in the save folder, the excluded paths stay: they are not saves). */
static BOOL write_set(NSString *dir, Set *set) {
    BOOL keep_excluded = [dir isEqualToString:save_dir];
    NSFileManager *fm = NSFileManager.defaultManager;
    [fm createDirectoryAtPath:dir withIntermediateDirectories:YES attributes:nil error:nil];
    for (NSString *rel in [fm enumeratorAtPath:dir].allObjects.reverseObjectEnumerator) {   /* deepest first */
        if ([rel.lastPathComponent hasPrefix:@"."] || set[rel]) continue;
        NSString *p = [dir stringByAppendingPathComponent:rel]; BOOL folder = NO; [fm fileExistsAtPath:p isDirectory:&folder];
        if (keep_excluded && is_excluded(folder ? [rel stringByAppendingString:@"/"] : rel)) continue;   /* not a save: left as it is */
        if (!folder || [fm contentsOfDirectoryAtPath:p error:nil].count == 0) [fm removeItemAtPath:p error:nil];
    }
    for (NSString *rel in set) {
        NSString *p = [dir stringByAppendingPathComponent:rel];
        [fm createDirectoryAtPath:p.stringByDeletingLastPathComponent withIntermediateDirectories:YES attributes:nil error:nil];
        if ([[NSData dataWithContentsOfFile:p] isEqualToData:set[rel]]) continue;
        NSError *err = nil;
        if (![set[rel] writeToFile:p options:NSDataWritingAtomic error:&err]) { say([NSString stringWithFormat:@"cannot write %@: %@", p, err.localizedDescription]); return NO; }
    }
    return YES;
}
static void backup(Set *set, NSString *why) {
    if (!set.count) return;
    NSFileManager *fm = NSFileManager.defaultManager;
    NSString *root = [state_dir stringByAppendingPathComponent:@"save-backups"];
    NSDateFormatter *f = [NSDateFormatter new]; f.dateFormat = @"yyyy-MM-dd-HHmmss"; f.locale = [NSLocale localeWithLocaleIdentifier:@"en_US_POSIX"];
    NSString *dir = [root stringByAppendingPathComponent:[NSString stringWithFormat:@"%@-%@", [f stringFromDate:NSDate.date], why]];
    if (write_set(dir, set)) say([NSString stringWithFormat:@"kept a copy in %@", dir]);
    NSArray *all = [[fm contentsOfDirectoryAtPath:root error:nil] sortedArrayUsingSelector:@selector(compare:)];
    for (NSUInteger i = 0; i + cfg.keep_backups < all.count; i++) [fm removeItemAtPath:[root stringByAppendingPathComponent:all[i]] error:nil];
}

/* ---- this device's state: the hash both sides last agreed on ---- */
static NSString *state_path(void) { return [state_dir stringByAppendingPathComponent:[NSString stringWithFormat:@"savesync-%@.json", title]]; }
static NSString *last_agreed(void) {
    NSData *d = [NSData dataWithContentsOfFile:state_path()];
    NSDictionary *j = d ? [NSJSONSerialization JSONObjectWithData:d options:0 error:nil] : nil;
    return [j isKindOfClass:NSDictionary.class] && [j[@"last"] isKindOfClass:NSString.class] ? j[@"last"] : nil;
}
static void set_last_agreed(NSString *hash) {
    [[NSJSONSerialization dataWithJSONObject:@{@"last": hash} options:0 error:nil] writeToFile:state_path() atomically:YES];
}

/* ======================= 2. store ======================= */
static NSString *files_dir(void) { return [cloud_dir stringByAppendingPathComponent:@"files"]; }
static NSString *manifest_path(void) { return [cloud_dir stringByAppendingPathComponent:@"manifest.json"]; }
#if TARGET_OS_IPHONE
/* iCloud may list a file before its contents are here: ask for it and wait (on a Mac, reading fetches it). */
static BOOL fetch(NSString *path, NSTimeInterval timeout) {
    NSURL *url = [NSURL fileURLWithPath:path]; NSFileManager *fm = NSFileManager.defaultManager;
    if (![fm startDownloadingUbiquitousItemAtURL:url error:nil]) return [fm fileExistsAtPath:path];
    for (NSDate *end = [NSDate dateWithTimeIntervalSinceNow:timeout]; end.timeIntervalSinceNow > 0; [NSThread sleepForTimeInterval:0.25]) {
        [url removeAllCachedResourceValues]; NSString *status = nil;
        [url getResourceValue:&status forKey:NSURLUbiquitousItemDownloadingStatusKey error:nil];
        if ([status isEqualToString:NSURLUbiquitousItemDownloadingStatusCurrent]) return YES;
    }
    return NO;
}
/* Whether iCloud knows a manifest, once its metadata for the container has arrived: a fresh install can start before
   it has and would otherwise take the cloud for empty. The query runs on the main run loop, so not from main. */
static BOOL manifest_known(void) {
    NSFileManager *fm = NSFileManager.defaultManager;
    if ([fm fileExistsAtPath:manifest_path()] || NSThread.isMainThread) return [fm fileExistsAtPath:manifest_path()];
    __block NSMetadataQuery *q; __block BOOL found = NO; __block id observer; dispatch_semaphore_t done = dispatch_semaphore_create(0);
    NSString *tail = [title stringByAppendingPathComponent:@"manifest.json"];
    dispatch_async(dispatch_get_main_queue(), ^{
        q = [NSMetadataQuery new]; q.searchScopes = @[NSMetadataQueryUbiquitousDocumentsScope];
        q.predicate = [NSPredicate predicateWithFormat:@"%K == %@", NSMetadataItemFSNameKey, @"manifest.json"];
        observer = [NSNotificationCenter.defaultCenter addObserverForName:NSMetadataQueryDidFinishGatheringNotification object:q queue:nil usingBlock:^(NSNotification *n) {
            (void)n; [q stopQuery];
            /* by folder, not full path: the container can be reported under /var or /private/var */
            for (NSUInteger i = 0; i < q.resultCount; i++) if ([[[q resultAtIndex:i] valueForAttribute:NSMetadataItemPathKey] hasSuffix:tail]) found = YES;
            [NSNotificationCenter.defaultCenter removeObserver:observer]; dispatch_semaphore_signal(done);
        }];
        if (![q startQuery]) dispatch_semaphore_signal(done);
    });
    if (dispatch_semaphore_wait(done, dispatch_time(DISPATCH_TIME_NOW, 20 * NSEC_PER_SEC))) say(@"iCloud did not answer in 20 s");
    return found || [fm fileExistsAtPath:manifest_path()];
}
#else
static BOOL fetch(NSString *path, NSTimeInterval timeout) { (void)timeout; return [NSFileManager.defaultManager fileExistsAtPath:path]; }
static BOOL manifest_known(void) { return [NSFileManager.defaultManager fileExistsAtPath:manifest_path()]; }
#endif
/* files: the saves (excluded paths left out), nil while the copy is still arriving; hash: theirs. A copy uploaded
   before a path was excluded lists it too: it is read to check the manifest's hash, then left out. */
typedef struct { NSDictionary *manifest; Set *files; NSString *hash; } Cloud;
static Cloud read_cloud(void) {
    Cloud c = {nil, nil, nil};
    if (!manifest_known() || !fetch(manifest_path(), 30)) return c;
    NSData *d = [NSData dataWithContentsOfFile:manifest_path()];
    NSDictionary *m = d ? [NSJSONSerialization JSONObjectWithData:d options:0 error:nil] : nil;
    if (![m isKindOfClass:NSDictionary.class] || ![m[@"hash"] isKindOfClass:NSString.class] || ![m[@"files"] isKindOfClass:NSArray.class]) return c;
    c.manifest = m;
    for (id rel in m[@"files"]) if (![rel isKindOfClass:NSString.class] || !fetch([files_dir() stringByAppendingPathComponent:rel], 60)) return c;
    Set *all = read_set(files_dir(), NO); NSMutableDictionary *listed = [NSMutableDictionary dictionary];
    for (NSString *rel in m[@"files"]) if (all[rel]) listed[rel] = all[rel];
    if (![hash_set(listed) isEqualToString:m[@"hash"]]) return c;
    NSMutableDictionary *saves = [NSMutableDictionary dictionary];
    for (NSString *rel in listed) if (!is_excluded(rel)) saves[rel] = listed[rel];
    c.files = saves; c.hash = hash_set(saves);
    return c;
}
static BOOL write_cloud(Set *set, NSString *hash) {
    __block BOOL ok = NO;
    void (^write)(void) = ^{
        if (!write_set(files_dir(), set)) return;
        uint64_t bytes = 0; for (NSString *rel in set) bytes += set[rel].length;
        NSDictionary *m = @{@"format": @FORMAT, @"title": title, @"hash": hash, @"files": [set.allKeys sortedArrayUsingSelector:@selector(compare:)],
                            @"bytes": @(bytes), @"version": version, @"time": @(NSDate.date.timeIntervalSince1970),
                            @"device": NSProcessInfo.processInfo.hostName ?: @"?"};
        ok = [[NSJSONSerialization dataWithJSONObject:m options:NSJSONWritingPrettyPrinted | NSJSONWritingSortedKeys error:nil] writeToFile:manifest_path() atomically:YES];
    };
#if TARGET_OS_IPHONE
    NSError *err = nil;   /* coordinated, so iCloud does not upload a half-written set */
    [[[NSFileCoordinator alloc] initWithFilePresenter:nil] coordinateWritingItemAtURL:[NSURL fileURLWithPath:cloud_dir] options:0 error:&err
                                                                         byAccessor:^(NSURL *u) { (void)u; write(); }];
    if (err) say([NSString stringWithFormat:@"cannot write the cloud copy: %@", err.localizedDescription]);
#else
    write();
#endif
    return ok;
}
/* SaveSync/<title> in the container. SAVESYNC_DIR names the folder itself (tests, other sync tools). */
static NSString *find_cloud_dir(void) {
    const char *off = getenv("SAVESYNC"); if (off && !strcmp(off, "0")) return nil;
    const char *env = getenv("SAVESYNC_DIR"); if (env && *env) return @(env);
    NSFileManager *fm = NSFileManager.defaultManager; NSString *rel = [[@"Documents" stringByAppendingPathComponent:ROOT_FOLDER] stringByAppendingPathComponent:title];
#if TARGET_OS_IPHONE
    NSURL *c = [fm URLForUbiquityContainerIdentifier:container];   /* nil without an iCloud entitlement or when signed out */
    if (!c) return nil;
    NSString *dir = [c.path stringByAppendingPathComponent:rel];
    [fm createDirectoryAtPath:[dir stringByAppendingPathComponent:@"files"] withIntermediateDirectories:YES attributes:nil error:nil];
    return dir;
#else
    /* No entitlement on the Mac: iCloud Drive keeps the account's containers in ~/Library/Mobile Documents, as
       iCloud~com~example~game for iCloud.com.example.game. */
    NSString *root = [NSHomeDirectory() stringByAppendingPathComponent:@"Library/Mobile Documents"];
    if (container) {
        NSString *name = [[container stringByReplacingOccurrencesOfString:@"." withString:@"~"] copy];
        NSString *top = [root stringByAppendingPathComponent:name];
        if (![fm fileExistsAtPath:top]) return nil;
        NSString *dir = [top stringByAppendingPathComponent:rel];
        [fm createDirectoryAtPath:[dir stringByAppendingPathComponent:@"files"] withIntermediateDirectories:YES attributes:nil error:nil];
        return dir;
    }
    NSMutableArray *found = [NSMutableArray array];
    for (NSString *name in [fm contentsOfDirectoryAtPath:root error:nil]) {
        NSString *dir = [[root stringByAppendingPathComponent:name] stringByAppendingPathComponent:rel]; BOOL folder = NO;
        if ([name hasPrefix:@"iCloud~"] && [fm fileExistsAtPath:dir isDirectory:&folder] && folder) [found addObject:dir];
    }
    if (found.count > 1) { say([NSString stringWithFormat:@"several containers hold %@; set SAVESYNC_CONTAINER to one: %@", rel, found]); return nil; }
    return found.firstObject;
#endif
}

/* ======================= 3. decision ======================= */
savesync_action savesync_decide(const savesync_facts *f, int *conflict) {
    if (conflict) *conflict = 0;
    if (!f->cloud_exists) return f->local_empty ? SAVESYNC_NOTHING : SAVESYNC_SEND;
    if (!f->cloud_complete) return SAVESYNC_WAIT;
    if (f->local_is_cloud) return SAVESYNC_IN_STEP;
    if (f->cloud_newer_version) return SAVESYNC_REFUSE_NEWER;
    if (f->local_empty) return SAVESYNC_TAKE;
    int local_changed = !f->local_is_last, cloud_changed = !f->cloud_is_last;
    if (cloud_changed && !local_changed) return SAVESYNC_TAKE;
    if (local_changed && !cloud_changed) return SAVESYNC_SEND;
    if (conflict) *conflict = 1;   /* both changed (or never agreed): the newer wins, the other is backed up */
    return f->cloud_newer_time ? SAVESYNC_TAKE : SAVESYNC_SEND;
}

/* ======================= 4. scheduler ======================= */
static int version_newer(NSString *cloud) { return [cloud compare:version options:NSNumericSearch] == NSOrderedDescending; }
/* One sync: at_start may take the cloud's saves (the game has not read its own yet); later only uploads, and only when
   this device alone changed them, so a save made on another device meanwhile is settled at the next start. */
static void sync_once(BOOL at_start) {
    Set *local = read_set(save_dir, YES); if (!local) { say(@"cannot read the local saves; not syncing"); return; }
    NSString *local_hash = hash_set(local), *last = last_agreed();
    /* The hash agreed on before a path was excluded covered it: this device's and the cloud's saves, hashed the old way
       too, still count as agreed then (no false conflict at the switch). */
    Set *local_all = excluded.count ? read_set(save_dir, NO) : local;
    NSString *local_old = local_all ? hash_set(local_all) : local_hash;
    if (!at_start && (!local.count || [local_hash isEqualToString:last])) return;   /* nothing new here */
    Cloud c = read_cloud(); NSString *cloud_hash = c.hash, *cloud_old = c.manifest[@"hash"];
    NSString *cloud_version = [c.manifest[@"version"] isKindOfClass:NSString.class] ? c.manifest[@"version"] : @"0";
    savesync_facts f = {
        .cloud_exists = c.manifest != nil, .cloud_complete = c.files != nil, .local_empty = local.count == 0,
        .local_is_cloud = [local_hash isEqualToString:cloud_hash], .local_is_last = [local_hash isEqualToString:last] || [local_old isEqualToString:last],
        .cloud_is_last = [cloud_hash isEqualToString:last] || [cloud_old isEqualToString:last], .cloud_newer_version = c.manifest && version_newer(cloud_version),
        .cloud_newer_time = [[NSDate dateWithTimeIntervalSince1970:[c.manifest[@"time"] doubleValue]] compare:newest_change(save_dir)] == NSOrderedDescending};
    int conflict; savesync_action a = savesync_decide(&f, &conflict);
    if (!at_start && (a != SAVESYNC_SEND || conflict)) {
        if (conflict) say(@"another device changed the cloud saves while playing here; settled at the next start");
        return;
    }
    switch (a) {
    case SAVESYNC_NOTHING: say(@"no saves here or in the cloud yet"); break;
    case SAVESYNC_IN_STEP: set_last_agreed(local_hash); say(@"in step with the cloud"); break;
    case SAVESYNC_WAIT: say(@"the cloud copy is still arriving; keeping this device's saves for now"); break;
    case SAVESYNC_REFUSE_NEWER: say([NSString stringWithFormat:@"the cloud save is from %@, newer than this game (%@); not taking it", cloud_version, version]); break;
    case SAVESYNC_TAKE:
        if (conflict) say([NSString stringWithFormat:@"both this device and %@ changed the saves; the cloud's is newer and wins", c.manifest[@"device"]]);
        backup(local, @"before-icloud");
        if (write_set(save_dir, c.files)) { set_last_agreed(cloud_hash); say([NSString stringWithFormat:@"took the saves from %@ (%lu files)", c.manifest[@"device"], (unsigned long)c.files.count]); }
        break;
    case SAVESYNC_SEND:
        if (conflict) { say([NSString stringWithFormat:@"both this device and %@ changed the saves; this device's is newer and wins", c.manifest[@"device"]]); backup(c.files, @"icloud-older"); }
        if (write_cloud(local, local_hash)) { set_last_agreed(local_hash); say([NSString stringWithFormat:@"sent %lu files", (unsigned long)local.count]); }
        break;
    }
}

const char *savesync_cloud_folder(void) {   /* kept: the folder does not change once found */
    static char *path; if (!path && cloud_dir) path = strdup(cloud_dir.fileSystemRepresentation); return path;
}
static void flush_at_exit(void) { savesync_flush(10); }
int savesync_start(const savesync_config *config) {
    @autoreleasepool {
        if (queue || !config || !config->title_id || !config->save_dir || !config->state_dir) return 0;
        cfg = *config; if (!cfg.keep_backups) cfg.keep_backups = 10;
        NSMutableArray *x = [NSMutableArray array];
        for (const char *const *p = config->exclude; p && *p; p++) [x addObject:@(*p)];
        excluded = x; cfg.exclude = NULL;   /* copied: the caller's list need not outlive this call */
        tag = @(config->log_tag ?: "[savesync]"); title = @(config->title_id); version = @(config->game_version ?: "0");
        save_dir = @(config->save_dir); state_dir = @(config->state_dir);
        const char *env_container = getenv("SAVESYNC_CONTAINER");
        container = env_container && *env_container ? @(env_container) : config->container_id ? @(config->container_id) : nil;
        NSString *dir = find_cloud_dir(); if (!dir) return 0;
        cloud_dir = dir; queue = dispatch_queue_create("savesync", DISPATCH_QUEUE_SERIAL);
        [NSFileManager.defaultManager createDirectoryAtPath:state_dir withIntermediateDirectories:YES attributes:nil error:nil];
        say([NSString stringWithFormat:@"syncing with %@", dir]);
        dispatch_sync(queue, ^{ @autoreleasepool { sync_once(YES); } });
        atexit(flush_at_exit);
        return 1;
    }
}
/* asked: the uploads_asked count this upload covers; when it has run, savesync_busy is 0 up to there */
static void push_later(uint64_t generation, uint64_t asked) {
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 3 * NSEC_PER_SEC), queue, ^{
        if (generation != push_generation) return;   /* a later save comes after this one */
        if (cfg.save_busy && cfg.save_busy()) { push_later(generation, asked); return; }
        @autoreleasepool { sync_once(NO); }
        atomic_store(&uploads_done, asked);
    });
}
void savesync_committed(void) {
    if (!queue) return;
    uint64_t asked = atomic_fetch_add(&uploads_asked, 1) + 1;   /* counted here, not on the queue: busy at once */
    dispatch_async(queue, ^{ push_later(++push_generation, asked); });
}
void savesync_flush(double timeout_s) {
    if (!queue) return;
    dispatch_semaphore_t done = dispatch_semaphore_create(0);
    uint64_t asked = atomic_load(&uploads_asked);
    dispatch_async(queue, ^{
        ++push_generation;   /* a pending delayed upload is covered by this one */
        if (!(cfg.save_busy && cfg.save_busy())) { @autoreleasepool { sync_once(NO); } atomic_store(&uploads_done, asked); }
        dispatch_semaphore_signal(done);
    });
    dispatch_semaphore_wait(done, dispatch_time(DISPATCH_TIME_NOW, (int64_t)(timeout_s * NSEC_PER_SEC)));
}
int savesync_busy(void) { return queue && atomic_load(&uploads_done) < atomic_load(&uploads_asked); }
