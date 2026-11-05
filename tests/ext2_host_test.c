// ext2_host_test.c - the ext2/ext3 driver on the host, checked by e2fsck
//
// A file system is made on a disk image in memory, changed through the
// driver, written to a file, and Linux's e2fsck must find nothing wrong.
// Also: a crash after the journal commit (replayed by us and by e2fsck),
// names found in any case, a full disk, and volumes made by Linux's mke2fs.
// Runs in the build image (make test-ext2), which has e2fsprogs.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "drivers/ext2.h"
#include "drivers/rtc.h"

void *kmalloc(uint64_t size) { return malloc(size ? size : 1); }
void kfree(void *p) { free(p); }
void kstrncpy(char *d, const char *s, size_t n) {
    if (!n) return;
    strncpy(d, s, n - 1);
    d[n - 1] = 0;
}
int rtc_read(rtc_time_t *t) {
    *t = (rtc_time_t){ 2026, 11, 5, 12, 0, 0 };
    return 0;
}

static uint8_t *disk;
static uint64_t disk_size;
static int failures;
static const char *IMG = "/tmp/ext2_host_test.img";

static int mem_read(void *ctx, uint64_t lba, uint32_t n, void *buf) {
    (void)ctx;
    if ((lba + n) * 512 > disk_size) return -1;
    memcpy(buf, disk + lba * 512, (size_t)n * 512);
    return 0;
}

static int mem_write(void *ctx, uint64_t lba, uint32_t n, const void *buf) {
    (void)ctx;
    if ((lba + n) * 512 > disk_size) return -1;
    memcpy(disk + lba * 512, buf, (size_t)n * 512);
    return 0;
}

static blockdev_t dev(void) {
    blockdev_t d;
    memset(&d, 0, sizeof(d));
    d.read = mem_read;
    d.write = mem_write;
    d.sectors = disk_size / 512;
    d.writable = 1;
    return d;
}

#define CHECK(c, ...) do { if (!(c)) { printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); failures++; } } while (0)

static void save(const char *path) {
    FILE *f = fopen(path, "wb");
    fwrite(disk, 1, disk_size, f);
    fclose(f);
}

static void load(const char *path) {
    FILE *f = fopen(path, "rb");
    fseek(f, 0, SEEK_END);
    disk_size = (uint64_t)ftell(f);
    fseek(f, 0, SEEK_SET);
    free(disk);
    disk = malloc(disk_size);
    if (fread(disk, 1, disk_size, f) != disk_size) printf("short read\n");
    fclose(f);
}

/* e2fsck's verdict on the image as it is now: its exit code. */
static int fsck(const char *opts, const char *what) {
    save(IMG);
    char cmd[256];
    snprintf(cmd, sizeof(cmd), "e2fsck %s %s > /tmp/fsck.out 2>&1", opts, IMG);
    int r = system(cmd);
    r = WEXITSTATUS(r);
    if (r != 0 && strcmp(opts, "-fy")) {
        printf("e2fsck %s after %s says (exit %d):\n", opts, what, r);
        system("cat /tmp/fsck.out");
    }
    return r;
}

static void pattern(uint8_t *b, uint32_t n, uint32_t seed) {
    for (uint32_t i = 0; i < n; i++) b[i] = (uint8_t)(i * 31 + seed + (i >> 12));
}

static int write_file(ext2vol_t *v, uint32_t dir, const char *name, uint32_t size, uint32_t seed) {
    ext2ent_t f;
    int r = ext2_lookup(v, dir, name, &f);
    if (r == FATV_NOT_FOUND) r = ext2_create(v, dir, name, EXT2_S_IFREG | 0644, 1000, 1000, &f);
    if (r) return r;
    uint8_t *b = malloc(size + 1);
    pattern(b, size, seed);
    r = ext2_write(v, &f, b, size);
    free(b);
    return r;
}

static int check_file(ext2vol_t *v, uint32_t dir, const char *name, uint32_t size, uint32_t seed) {
    ext2ent_t f;
    if (ext2_lookup(v, dir, name, &f) != 0) return -1;
    if (f.size != size) return -2;
    uint8_t *b = malloc(size + 1), *want = malloc(size + 1);
    pattern(want, size, seed);
    int ok = 1;
    for (uint32_t off = 0; off < size;) {                    /* in odd pieces, as programs read */
        uint32_t n = 7777 + off % 5000;
        if (n > size - off) n = size - off;
        int got = ext2_read_at(v, &f, off, b + off, n);
        if (got != (int)n) { ok = 0; break; }
        off += n;
    }
    if (ok && memcmp(b, want, size)) ok = 0;
    free(b);
    free(want);
    return ok ? 0 : -3;
}

static uint32_t mkdir_at(ext2vol_t *v, uint32_t dir, const char *name, uint16_t mode) {
    ext2ent_t d;
    int r = ext2_create(v, dir, name, EXT2_S_IFDIR | mode, 0, 0, &d);
    CHECK(r == 0, "mkdir %s: %d", name, r);
    return d.ino;
}

static void test_basics(void) {
    printf("basics\n");
    disk_size = 64ull << 20;
    free(disk);
    disk = calloc(1, disk_size);
    blockdev_t d = dev();
    CHECK(ext2_format(&d, "TERMOS DATA") == 0, "format");
    CHECK(fsck("-fn", "format") == 0, "fresh file system");

    ext2vol_t v;
    CHECK(ext2_open(&v, &d) == 0, "open");
    CHECK(!strcmp(v.label, "TERMOS DATA"), "label %s", v.label);
    uint32_t home = mkdir_at(&v, EXT2_ROOT_INO, "HOME", 0755);
    uint32_t alice = mkdir_at(&v, home, "ALICE", 0700);
    uint32_t sizes[] = { 0, 1, 4095, 4096, 4097, 50000, 5u << 20, 300000 };
    char name[64];
    for (int i = 0; i < 8; i++) {
        snprintf(name, sizeof(name), "file%d.bin", i);
        CHECK(write_file(&v, alice, name, sizes[i], (uint32_t)i) == 0, "write %s", name);
    }
    for (int i = 0; i < 8; i++) {
        snprintf(name, sizeof(name), "file%d.bin", i);
        CHECK(check_file(&v, alice, name, sizes[i], (uint32_t)i) == 0, "read %s", name);
    }
    /* Rewritten smaller and bigger: the old blocks are freed. */
    CHECK(write_file(&v, alice, "file6.bin", 1234, 66) == 0, "shrink");
    CHECK(check_file(&v, alice, "file6.bin", 1234, 66) == 0, "shrunk");
    CHECK(write_file(&v, alice, "file1.bin", 2u << 20, 11) == 0, "grow");
    CHECK(check_file(&v, alice, "file1.bin", 2u << 20, 11) == 0, "grown");

    /* Names in any case; the spelling is kept. */
    ext2ent_t f;
    CHECK(ext2_lookup(&v, alice, "FILE2.BIN", &f) == 0 && !strcmp(f.name, "file2.bin"), "case: %s", f.name);
    CHECK(ext2_create(&v, alice, "File2.Bin", EXT2_S_IFREG | 0644, 0, 0, 0) == FATV_EXISTS, "taken in another case");
    CHECK(ext2_lookup(&v, EXT2_ROOT_INO, "home", &f) == 0 && f.ino == home, "home");

    /* A folder of many names: it grows past one block; half go, half come back. */
    uint32_t many = mkdir_at(&v, alice, "Many files with long names", 0755);
    for (int i = 0; i < 400; i++) {
        snprintf(name, sizeof(name), "a rather long file name number %d.txt", i);
        CHECK(write_file(&v, many, name, (uint32_t)(i % 7) * 100, (uint32_t)i) == 0, "many %d", i);
    }
    for (int i = 0; i < 400; i += 2) {
        snprintf(name, sizeof(name), "a rather long file name number %d.txt", i);
        CHECK(ext2_lookup(&v, many, name, &f) == 0 && ext2_delete(&v, &f) == 0, "delete %d", i);
    }
    for (int i = 0; i < 100; i++) {
        snprintf(name, sizeof(name), "n%d", i);
        CHECK(write_file(&v, many, name, 10, (uint32_t)i) == 0, "again %d", i);
    }
    fat32_file_info_t *list = malloc(1000 * sizeof(*list));
    int n = ext2_list(&v, many, list, 1000);
    CHECK(n == 300, "listed %d", n);
    free(list);
    for (int i = 1; i < 400; i += 2) {
        snprintf(name, sizeof(name), "a rather long file name number %d.txt", i);
        CHECK(check_file(&v, many, name, (uint32_t)(i % 7) * 100, (uint32_t)i) == 0, "kept %d", i);
    }

    /* Permissions, renames, folders that are not empty. */
    CHECK(ext2_lookup(&v, alice, "file3.bin", &f) == 0, "file3");
    CHECK(ext2_setattr(&v, &f, 0600, 1001, 1001) == 0 && f.uid == 1001 && (f.mode & 07777) == 0600, "chmod");
    CHECK(ext2_rename(&v, &f, home, "moved.bin") == 0, "rename");
    CHECK(check_file(&v, home, "moved.bin", 4096, 3) == 0, "renamed");
    CHECK(ext2_lookup(&v, alice, "file3.bin", &f) == FATV_NOT_FOUND, "gone from the old folder");
    CHECK(ext2_lookup(&v, home, "moved.bin", &f) == 0 && ext2_rename(&v, &f, home, "MOVED.BIN") == 0, "case rename");
    CHECK(ext2_lookup(&v, home, "moved.bin", &f) == 0 && !strcmp(f.name, "MOVED.BIN"), "new spelling");
    CHECK(ext2_lookup(&v, alice, "file4.bin", &f) == 0 && ext2_rename(&v, &f, home, "MOVED.BIN") == 0, "replace");
    CHECK(check_file(&v, home, "moved.bin", 4097, 4) == 0, "replaced");
    ext2ent_t m;
    CHECK(ext2_lookup(&v, alice, "Many files with long names", &m) == 0, "many");
    CHECK(ext2_delete(&v, &m) == FATV_NOT_EMPTY, "not empty");
    CHECK(ext2_rename(&v, &m, home, "Many") == 0, "move a folder");
    ext2ent_t h;
    CHECK(ext2_stat(&v, home, &h) == 0, "stat");
    CHECK(ext2_lookup(&v, home, "Many", &m) == 0 && ext2_rename(&v, &m, many, "inside") == FATV_BAD_NAME,
          "into itself");
    uint32_t tmp = mkdir_at(&v, EXT2_ROOT_INO, "TMP", 01777);
    CHECK(ext2_lookup(&v, EXT2_ROOT_INO, "TMP", &f) == 0 && (f.mode & 07777) == 01777, "sticky %o", f.mode);
    CHECK(ext2_delete(&v, &f) == 0, "rmdir");
    (void)tmp;
    ext2_close(&v);
    CHECK(fsck("-fn", "changes") == 0, "after the changes");

    CHECK(ext2_open(&v, &d) == 0, "open again");
    CHECK(check_file(&v, home, "moved.bin", 4097, 4) == 0, "after a remount");
    ext2_close(&v);
}

static void test_crash(void) {
    printf("crash after the journal commit\n");
    blockdev_t d = dev();
    ext2vol_t v;
    CHECK(ext2_open(&v, &d) == 0, "open");
    ext2ent_t f;
    CHECK(ext2_lookup(&v, EXT2_ROOT_INO, "HOME", &f) == 0, "home");
    uint32_t home = f.ino;
    CHECK(write_file(&v, home, "after the crash.txt", 10, 1) == 0, "made");
    v.crash_after_commit = 1;
    int r = write_file(&v, home, "after the crash.txt", 100000, 77);
    CHECK(r != 0, "the change stopped: %d", r);
    /* The power goes: the journal holds the change (close writes nothing now). */
    save("/tmp/ext2_crashed.img");
    ext2_close(&v);
    int fr = fsck("-fy", "the crash");
    CHECK(fr == 0 || fr == 1, "e2fsck -fy replays it (exit %d)", fr);
    CHECK(fsck("-fn", "e2fsck's replay") == 0, "clean after e2fsck's replay");
    ext2vol_t w;
    CHECK(ext2_open(&w, &d) == 0, "open what e2fsck replayed");
    CHECK(check_file(&w, home, "after the crash.txt", 100000, 77) == 0, "there after e2fsck's replay");
    ext2_close(&w);

    load("/tmp/ext2_crashed.img");                          /* now our own replay */
    d = dev();
    CHECK(ext2_open(&w, &d) == 0, "open with a full journal");
    CHECK(check_file(&w, home, "after the crash.txt", 100000, 77) == 0, "there after our replay");
    ext2_close(&w);
    CHECK(fsck("-fn", "our replay") == 0, "clean after our replay");
}

static void test_full(void) {
    printf("full disk\n");
    disk_size = 16ull << 20;
    free(disk);
    disk = calloc(1, disk_size);
    blockdev_t d = dev();
    CHECK(ext2_format(&d, "SMALL") == 0, "format");
    ext2vol_t v;
    CHECK(ext2_open(&v, &d) == 0, "open");
    int r = 0, i;
    char name[32];
    for (i = 0; i < 100 && r == 0; i++) {
        snprintf(name, sizeof(name), "big%d", i);
        r = write_file(&v, EXT2_ROOT_INO, name, 1u << 20, (uint32_t)i);
    }
    CHECK(r == FATV_FULL, "full after %d files: %d", i, r);
    snprintf(name, sizeof(name), "big%d", i - 1);
    ext2ent_t f;
    CHECK(ext2_lookup(&v, EXT2_ROOT_INO, name, &f) == 0 && f.size == 0, "the failed one is empty");
    CHECK(check_file(&v, EXT2_ROOT_INO, "big0", 1u << 20, 0) == 0, "the first is whole");
    ext2_close(&v);
    CHECK(fsck("-fn", "a full disk") == 0, "full disk");
}

static void test_linux(const char *opts) {
    printf("made by mke2fs %s\n", opts);
    char cmd[256];
    snprintf(cmd, sizeof(cmd), "rm -f %s; mke2fs -q -F %s %s 32M >/dev/null 2>&1", IMG, opts, IMG);
    CHECK(system(cmd) == 0, "mke2fs");
    load(IMG);
    blockdev_t d = dev();
    ext2vol_t v;
    CHECK(ext2_open(&v, &d) == 0, "open");
    uint32_t dir = mkdir_at(&v, EXT2_ROOT_INO, "Docs", 0755);
    char name[32];
    for (int i = 0; i < 120; i++) {
        snprintf(name, sizeof(name), "note %d", i);
        CHECK(write_file(&v, dir, name, (uint32_t)i * 300, (uint32_t)i) == 0, "write");
    }
    for (int i = 0; i < 120; i += 3) {
        ext2ent_t f;
        snprintf(name, sizeof(name), "note %d", i);
        CHECK(ext2_lookup(&v, dir, name, &f) == 0 && ext2_delete(&v, &f) == 0, "delete");
    }
    ext2_close(&v);
    CHECK(fsck("-fn", opts) == 0, "mke2fs %s", opts);
}

int main(void) {
    test_basics();
    test_crash();
    test_full();
    test_linux("-t ext3 -b 4096 -I 128");
    test_linux("-t ext3 -b 1024 -I 256");
    test_linux("-t ext2 -b 2048");
    if (failures) {
        printf("%d FAILED\n", failures);
        return 1;
    }
    printf("ext2: all passed\n");
    return 0;
}
