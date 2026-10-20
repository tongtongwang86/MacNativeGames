/* Unified nn::fs host layer: RomFS (romfs.c) plus writable save, temporary and cache storage. */
#pragma once
#include <stddef.h>
#include <stdint.h>

void *runtime_fs_host_override(const char *name);

/* romfs.c */
int runtime_romfs_owns(const char *path);
int runtime_romfs_unmount(const char *name);   /* romfs.c: a RomFS / add-on content mount was unmounted (1) */
int runtime_romfs_handle(uint64_t h);
unsigned runtime_romfs_open(uint64_t *out, const char *path, int mode);
unsigned runtime_romfs_read(uint64_t *out, uint64_t h, int64_t offset, void *buffer, uint64_t size);
unsigned runtime_romfs_size(int64_t *out, uint64_t h);
void runtime_romfs_close(uint64_t h);
unsigned runtime_romfs_entry_type(int *out, const char *path);
unsigned runtime_romfs_host_directory(const char *path, char *out, size_t size);

/* account.c: the local user identity */
void runtime_account_uid(uint64_t out[2]);
