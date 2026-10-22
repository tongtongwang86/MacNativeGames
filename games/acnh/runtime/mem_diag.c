/* Diagnostics (Mac, TOTK_INPUT_FILE lines; input.c): find, read and change bytes in the running game's memory, used to
   locate live game state such as the island's item layer (docs/LAN-COOP-DESIGN.md section 9).
     memfind <hex bytes>         every writable region, at most 32 matches, on a background thread
     peek <hex address> <len>    hex dump, at most 256 bytes
     poke <hex address> <hex>    write the bytes */
#include <TargetConditionals.h>
#if !TARGET_OS_IPHONE
#include "runtime.h"
#include "mach_vm_compat.h"
#include <mach/mach.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int parse_hex(const char *s, uint8_t *out, size_t max) {
    size_t n = 0;
    while (*s && n < max) {
        while (*s == ' ') s++;
        if (!s[0] || !s[1] || s[0] == '\n') break;
        unsigned v; if (sscanf(s, "%2x", &v) != 1) break;
        out[n++] = (uint8_t)v; s += 2;
    }
    return (int)n;
}

typedef struct { uint8_t pat[256]; int n; } FindJob;
static void *find_main(void *arg) {
    FindJob *job = arg;
    mach_vm_address_t a = 0; unsigned found = 0; uint64_t scanned = 0;
    enum { CHUNK = 16 << 20 };
    uint8_t *buf = malloc(CHUNK + 256);
    if (!buf) { free(job); return NULL; }
    for (;;) {
        mach_vm_size_t size = 0; vm_region_basic_info_data_64_t info; mach_msg_type_number_t count = VM_REGION_BASIC_INFO_COUNT_64; mach_port_t obj = 0;
        if (mach_vm_region(mach_task_self(), &a, &size, VM_REGION_BASIC_INFO_64, (vm_region_info_t)&info, &count, &obj) != KERN_SUCCESS) break;
        if ((info.protection & (VM_PROT_READ | VM_PROT_WRITE)) == (VM_PROT_READ | VM_PROT_WRITE)) {
            for (mach_vm_address_t p = a; p < a + size && found < 32; p += CHUNK) {
                mach_vm_size_t want = a + size - p < (mach_vm_size_t)CHUNK + job->n ? a + size - p : (mach_vm_size_t)CHUNK + job->n, got = 0;
                if (mach_vm_read_overwrite(mach_task_self(), p, want, (mach_vm_address_t)(uintptr_t)buf, &got) != KERN_SUCCESS) continue;
                scanned += got;
                for (uint8_t *h = buf; h + job->n <= buf + got && found < 32; h++) {
                    h = memmem(h, (size_t)(buf + got - h), job->pat, (size_t)job->n);
                    if (!h) break;
                    if ((uint64_t)(h - buf) < CHUNK) { fprintf(stderr, "[mem] match at %#llx\n", (unsigned long long)(p + (mach_vm_address_t)(h - buf))); found++; }
                }
            }
        }
        a += size;
    }
    fprintf(stderr, "[mem] memfind done: %u match(es), %llu MiB scanned\n", found, (unsigned long long)(scanned >> 20));
    free(buf); free(job);
    return NULL;
}

int runtime_mem_command(char *line) {
    if (!strncmp(line, "memfind ", 8)) {
        FindJob *job = calloc(1, sizeof *job);
        if (!job) return 1;
        job->n = parse_hex(line + 8, job->pat, sizeof job->pat);
        if (job->n < 4) { fprintf(stderr, "[mem] memfind needs at least 4 bytes\n"); free(job); return 1; }
        pthread_t t; if (pthread_create(&t, NULL, find_main, job)) { free(job); return 1; }
        pthread_detach(t); return 1;
    }
    if (!strncmp(line, "peek ", 5)) {
        unsigned long long addr; unsigned len;
        if (sscanf(line + 5, "%llx %u", &addr, &len) != 2 || !len || len > 256) return 1;
        uint8_t b[256]; mach_vm_size_t got = 0;
        if (mach_vm_read_overwrite(mach_task_self(), addr, len, (mach_vm_address_t)(uintptr_t)b, &got) != KERN_SUCCESS) { fprintf(stderr, "[mem] peek %#llx: not readable\n", addr); return 1; }
        for (unsigned i = 0; i < got; i += 16) {
            char row[80]; int o = snprintf(row, sizeof row, "[mem] %#llx:", addr + i);
            for (unsigned k = i; k < i + 16 && k < got; k++) o += snprintf(row + o, sizeof row - o, " %02x", b[k]);
            fprintf(stderr, "%s\n", row);
        }
        return 1;
    }
    if (!strncmp(line, "poke ", 5)) {
        unsigned long long addr; int used = 0;
        if (sscanf(line + 5, "%llx %n", &addr, &used) != 1) return 1;
        uint8_t b[256]; int n = parse_hex(line + 5 + used, b, sizeof b);
        if (n <= 0) return 1;
        if (mach_vm_write(mach_task_self(), addr, (vm_offset_t)(uintptr_t)b, (mach_msg_type_number_t)n) != KERN_SUCCESS) fprintf(stderr, "[mem] poke %#llx failed\n", addr);
        else fprintf(stderr, "[mem] poked %d byte(s) at %#llx\n", n, addr);
        return 1;
    }
    return 0;
}
#else
int runtime_mem_command(char *line) { (void)line; return 0; }
#endif
