/* Read-only game RomFS for ACNH, backed by the extracted romfs folder (hactool output of base + 2.0.6).
   ACNH_ROMFS overrides the folder (default extracted/romfs). Every path is resolved relative to that folder with the
   shared validation (no "..", no absolute paths, no backslashes) and opened with O_NOFOLLOW, so a guest path can't
   leave it. TOTK's verified-image backend (runtime-index.bin + romfs.bin, overlays) is not used here. */
#include "runtime.h"
#include "fs_host.h"
#include "romfs_path.h"
#include <fcntl.h>
#include <pthread.h>
#include <sys/stat.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define NOT_FOUND 0x202u
#define UNSUPPORTED 0x6402u
#define INVALID 0xe402u
#define NH 65536
#define HANDLE_BASE 0x100000000ULL

static int root_fd = -1;
static char root_path[2048];
static char mount_name[64];
static int handle_fd[NH];
static uint64_t handle_size[NH];
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;

static void need(int c, const char *m) { if (!c) runtime_fail(m); }

void runtime_fs_init(void) {
    if (root_fd >= 0) return;
    const char *dir = getenv("ACNH_ROMFS");
    if (!dir || !*dir) dir = "extracted/romfs";
    if (strlen(dir) >= sizeof root_path) runtime_fail("RomFS folder path too long");
    strcpy(root_path, dir);
    root_fd = open(dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (root_fd < 0) { fprintf(stderr, "[acnh] RomFS folder %s missing (set ACNH_ROMFS)\n", dir); runtime_fail("RomFS folder missing"); }
    struct stat st;
    need(fstatat(root_fd, "Shader", &st, 0) == 0 && S_ISDIR(st.st_mode) &&
         fstatat(root_fd, "System", &st, 0) == 0 && S_ISDIR(st.st_mode), "RomFS folder is not ACNH's (no Shader/ and System/)");
    fprintf(stderr, "[acnh] read-only RomFS: folder %s\n", dir);
}

static const char *rom_path(const char *path) { return romfs_path_relative(mount_name, path); }

static unsigned mount_rom(const char *name, void *cache, uint64_t size) {
    (void)cache; (void)size;
    runtime_fs_init();
    if (!name || !romfs_path_valid_relative(name) || strlen(name) >= sizeof(mount_name) || strchr(name, '/') || strchr(name, ':'))
        return INVALID;
    if (*mount_name && strcmp(name, mount_name)) runtime_fail("second RomFS mount not implemented");
    strcpy(mount_name, name);
    return 0;
}
static unsigned cache_size(uint64_t *out) { if (!out) return INVALID; *out = 0; return 0; }

/* TOTK_FS_LOG=1: log every open with the time since start. */
static void fs_log(const char *what, const char *path) {
    static int on = -1; static uint64_t t0;
    if (on < 0) { on = getenv("TOTK_FS_LOG") != NULL; t0 = runtime_now_ns(); }
    if (on) fprintf(stderr, "[totk-fs] %8.3f %s %s\n", (runtime_now_ns() - t0) / 1e9, what, path);
}
void runtime_fs_log(const char *what, const char *path) { fs_log(what, path); }

unsigned runtime_romfs_open(uint64_t *out, const char *path, int mode) {
    fs_log("rom", path ? path : "(null)");
    if (!out || mode != 1) return UNSUPPORTED;
    const char *p = rom_path(path);
    if (!p) return NOT_FOUND;
    int fd = openat(root_fd, p, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) return NOT_FOUND;
    struct stat st;
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) { close(fd); return NOT_FOUND; }
    pthread_mutex_lock(&lock);
    for (unsigned j = 1; j < NH; j++)
        if (!handle_fd[j]) {
            handle_fd[j] = fd + 1; handle_size[j] = (uint64_t)st.st_size;
            *out = HANDLE_BASE + j;
            pthread_mutex_unlock(&lock);
            return 0;
        }
    pthread_mutex_unlock(&lock);
    runtime_fail("file handle table full");
    return INVALID;
}

static int handle_get(uint64_t h, int *fd, uint64_t *size) {
    if (h <= HANDLE_BASE || h >= HANDLE_BASE + NH) return 0;
    pthread_mutex_lock(&lock);
    int f = handle_fd[h - HANDLE_BASE]; uint64_t s = handle_size[h - HANDLE_BASE];
    pthread_mutex_unlock(&lock);
    if (!f) return 0;
    *fd = f - 1; *size = s;
    return 1;
}

unsigned runtime_romfs_read(uint64_t *out, uint64_t h, int64_t offset, void *buffer, uint64_t size) {
    int fd; uint64_t fsize;
    if (!out || offset < 0 || !handle_get(h, &fd, &fsize)) return INVALID;
    if ((uint64_t)offset > fsize || (!buffer && size)) return INVALID;
    uint64_t n = size, done = 0;
    if (n > fsize - (uint64_t)offset) n = fsize - (uint64_t)offset;
    dm_guest_write_prepare(buffer, (size_t)n);
    while (done < n) {
        size_t chunk = (size_t)(n - done);
        if (chunk > 8 << 20) chunk = 8 << 20;
        ssize_t r = pread(fd, (uint8_t *)buffer + done, chunk, (off_t)((uint64_t)offset + done));
        if (r <= 0) runtime_fail("RomFS read failure");
        done += (uint64_t)r;
    }
    *out = done;
    return 0;
}
unsigned runtime_romfs_size(int64_t *out, uint64_t h) {
    int fd; uint64_t size;
    if (!out || !handle_get(h, &fd, &size)) return INVALID;
    *out = (int64_t)size;
    return 0;
}
void runtime_romfs_close(uint64_t h) {
    int fd; uint64_t size;
    if (!handle_get(h, &fd, &size)) runtime_fail("close invalid file handle");
    pthread_mutex_lock(&lock);
    handle_fd[h - HANDLE_BASE] = 0; handle_size[h - HANDLE_BASE] = 0;
    pthread_mutex_unlock(&lock);
    close(fd);
}
unsigned runtime_romfs_entry_type(int *out, const char *path) {
    if (!out) return INVALID;
    const char *p = rom_path(path);
    if (!p) {   /* the mount root itself ("rom:/") */
        size_t n = strlen(mount_name);
        if (n && path && !strncmp(path, mount_name, n) && !strcmp(path + n, ":/")) { *out = 0; return 0; }
        return NOT_FOUND;
    }
    struct stat st;
    if (fstatat(root_fd, p, &st, AT_SYMLINK_NOFOLLOW) != 0) return NOT_FOUND;
    if (S_ISREG(st.st_mode)) { *out = 1; return 0; }
    if (S_ISDIR(st.st_mode)) { *out = 0; return 0; }
    return NOT_FOUND;
}
/* The host folder behind a RomFS directory, for fs_host.c's directory listing. NOT_FOUND unless it is a directory. */
unsigned runtime_romfs_host_directory(const char *path, char *out, size_t size) {
    int type = -1;
    unsigned r = runtime_romfs_entry_type(&type, path);
    if (r) return r;
    if (type != 0) return NOT_FOUND;
    const char *p = rom_path(path);
    if (snprintf(out, size, "%s/%s", root_path, p ? p : "") >= (int)size) return INVALID;
    return 0;
}
int runtime_romfs_owns(const char *path) { size_t n = strlen(mount_name); return n && path && !strncmp(path, mount_name, n) && path[n] == ':'; }
int runtime_romfs_handle(uint64_t h) { return h > HANDLE_BASE && h < HANDLE_BASE + NH; }
unsigned runtime_romfs_read_options(uint64_t *out, uint64_t h, int64_t off, void *buf, uint64_t size) { return runtime_romfs_read(out, h, off, buf, size); }
void *runtime_fs_override(const char *name) {
    static const struct { const char *name; void *fn; } map[] = {
        {"_ZN2nn2fs8MountRomEPKcPvm", mount_rom}, {"_ZN2nn2fs22QueryMountRomCacheSizeEPm", cache_size}};
    for (size_t i = 0; i < sizeof(map) / sizeof(map[0]); i++) if (!strcmp(name, map[i].name)) return map[i].fn;
    return runtime_fs_host_override(name);
}
void runtime_fs_probe(void) {
    runtime_fs_init();
    need(mount_rom("rom", NULL, 0) == 0, "mount RomFS");
    int type = -1; uint64_t h = 0;
    need(!runtime_romfs_entry_type(&type, "rom:/Shader") && type == 0, "probe folder type");
    need(!runtime_romfs_entry_type(&type, "rom:/") && type == 0, "probe root type");
    need(runtime_romfs_open(&h, "rom:/../main", 1) == NOT_FOUND, "reject traversal");
    need(runtime_romfs_open(&h, "rom:/absent", 2) == UNSUPPORTED, "reject write open");
    need(runtime_romfs_open(&h, "rom:/Shader", 1) == NOT_FOUND, "reject opening a folder");
    printf("{\"status\":\"pass\",\"scope\":\"directory RomFS mount, types, traversal and write rejection\"}\n");
}
