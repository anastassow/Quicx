/*
test_config — the config file and the PoolConfig type

  values:  every [memory] / [allocator] key, good and bad values
  apply:   the switches and page counts reach a real pool
  files:   a full file with sections and comments; an old PMAD-era file;
           the repository's quicx.conf gives exactly the built-in defaults;
           files quicx must refuse to start on (checked in a child process)

build: make test_config        run: ./test_config [path/to/quicx.conf]
*/
#include "platform.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "config.h"

static int passed, failed;

static void check(const char* name, int ok) {
    printf("  %s: %s\n", ok ? "PASS" : "FAIL", name);
    if (ok) passed++; else failed++;
}

static PoolKeyResult set(PoolConfig* pc, const char* k, const char* v) {
    char err[160];
    return pool_config_set(pc, k, v, err, sizeof(err));
}

/* writes text to a temporary file and returns its path */
static const char* write_tmp(const char* text) {
    static char path[64];
    snprintf(path, sizeof(path), "/tmp/quicx_test_config_%d.conf", (int)getpid());
    FILE* f = fopen(path, "w");
    if (!f) { perror("fopen"); exit(2); }
    fputs(text, f);
    fclose(f);
    return path;
}

/* 1 if config_load refuses the file: the child exits with status 1 */
static int refused(const char* text) {
    const char* path = write_tmp(text);
    fflush(stdout);
    pid_t pid = fork();
    if (pid == 0) {
        freopen("/dev/null", "w", stderr);
        freopen("/dev/null", "w", stdout);
        Config c;
        config_defaults(&c);
        config_load(&c, path);
        _exit(0);
    }
    int status = 0;
    waitpid(pid, &status, 0);
    unlink(path);
    return WIFEXITED(status) && WEXITSTATUS(status) == 1;
}

static void test_defaults(void) {
    printf("--- defaults ---\n");
    Config c;
    config_defaults(&c);
    check("port 16381", c.port == 16381);
    check("pool_size 8 MiB", c.pool.pool_size == 8u * 1024 * 1024);
    check("flags are APOOL_F_DEFAULT", c.pool.flags == APOOL_F_DEFAULT);
    check("warm and reserve pages automatic", c.pool.warm_pages == POOL_AUTO && c.pool.reserve_pages == POOL_AUTO);
    char d[96];
    pool_config_describe(&c.pool, d, sizeof(d));
    check("described as 'reserve, hardening, purge; when full: value'",
          strcmp(d, "reserve, hardening, purge; when full: value") == 0);
    check("admission defaults to value", pc.admission == ADMISSION_VALUE);
}

static void test_values(void) {
    printf("--- values ---\n");
    PoolConfig pc;
    pool_config_defaults(&pc);
    check("pool_size 8M",         set(&pc, "pool_size", "8M") == POOL_KEY_OK && pc.pool_size == 8u << 20);
    check("pool_size 64MiB",      set(&pc, "pool_size", "64MiB") == POOL_KEY_OK && pc.pool_size == 64u << 20);
    check("pool_size 1048576",    set(&pc, "pool_size", "1048576") == POOL_KEY_OK && pc.pool_size == 1048576);
    check("pool_size 8MB refused (1000 or 1024?)", set(&pc, "pool_size", "8MB") == POOL_KEY_BAD);
    check("pool_size 0 refused",  set(&pc, "pool_size", "0") == POOL_KEY_BAD);
    check("pool_size -5 refused", set(&pc, "pool_size", "-5") == POOL_KEY_BAD);

    check("reserve off",          set(&pc, "reserve", "off") == POOL_KEY_OK && !(pc.flags & APOOL_F_RESERVE));
    check("reserve on",           set(&pc, "reserve", "on") == POOL_KEY_OK && (pc.flags & APOOL_F_RESERVE));
    check("hardening no",         set(&pc, "hardening", "no") == POOL_KEY_OK && !(pc.flags & APOOL_F_HARDEN));
    check("purge false",          set(&pc, "purge", "false") == POOL_KEY_OK && !(pc.flags & APOOL_F_PURGE));
    check("fullest_first 1",      set(&pc, "fullest_first", "1") == POOL_KEY_OK && (pc.flags & APOOL_F_FULLEST));
    check("purge maybe refused",  set(&pc, "purge", "maybe") == POOL_KEY_BAD);
    check("admission refuse", set(&pc, "admission", "refuse") == POOL_KEY_OK && pc.admission == ADMISSION_REFUSE);
    check("admission oldest", set(&pc, "admission", "oldest") == POOL_KEY_OK && pc.admission == ADMISSION_OLDEST);
    check("admission value",  set(&pc, "admission", "value")  == POOL_KEY_OK && pc.admission == ADMISSION_VALUE);
    check("admission newest refused", set(&pc, "admission", "newest") == POOL_KEY_BAD);

    check("warm_pages 100",       set(&pc, "warm_pages", "100") == POOL_KEY_OK && pc.warm_pages == 100);
    check("warm_pages auto",      set(&pc, "warm_pages", "auto") == POOL_KEY_OK && pc.warm_pages == POOL_AUTO);
    check("reserve_pages 0 refused", set(&pc, "reserve_pages", "0") == POOL_KEY_BAD);
    check("reserve_pages 7x refused", set(&pc, "reserve_pages", "7x") == POOL_KEY_BAD);
    check("unknown key left alone", set(&pc, "colour", "blue") == POOL_KEY_UNKNOWN);

    char err[160];
    PoolConfig q;
    pool_config_defaults(&q);
    q.flags &= ~APOOL_F_RESERVE;
    q.reserve_pages = 5;
    check("reserve_pages without reserve refused", pool_config_check(&q, err, sizeof(err)) != 0);
    pool_config_defaults(&q);
    q.flags &= ~APOOL_F_PURGE;
    q.warm_pages = 5;
    check("warm_pages without purge refused", pool_config_check(&q, err, sizeof(err)) != 0);
}

static void test_apply(void) {
    printf("--- apply to a real pool ---\n");
    PoolConfig pc;
    pool_config_defaults(&pc);
    pc.flags |= APOOL_F_FULLEST;
    pc.warm_pages = 100;
    pc.reserve_pages = 20;

    Apool p;
    check("init", apool_init(&p, 8u * 1024 * 1024) == APOOL_OK);
    uint32_t auto_warm = p.warm, auto_reserve = p.reserve;
    check("task pool takes flags and page counts",
          pool_config_apply(&pc, &p, POOL_TASKS) == 0 && p.flags == (APOOL_F_DEFAULT | APOOL_F_FULLEST) &&
          p.warm == 100 && p.reserve == 20);
    apool_destroy(&p);

    apool_init(&p, 8u * 1024 * 1024);
    check("control pool takes flags, keeps its own page counts",
          pool_config_apply(&pc, &p, POOL_CONTROL) == 0 && p.flags == (APOOL_F_DEFAULT | APOOL_F_FULLEST) &&
          p.warm == auto_warm && p.reserve == auto_reserve);
    apool_destroy(&p);

    apool_init(&p, 8u * 1024 * 1024);
    pc.warm_pages = p.num_pages + 1;
    check("more warm pages than the pool has refused", pool_config_apply(&pc, &p, POOL_TASKS) != 0);
    apool_destroy(&p);
}

static void test_files(const char* repo_conf) {
    printf("--- files ---\n");
    Config c;
    config_defaults(&c);
    const char* path = write_tmp(
        "# every section\n"
        "[server]\n"
        "port = 17000   # inline comment\n"
        "\n"
        "[memory]\n"
        "pool_size = 16M\n"
        "\n"
        "[allocator]\n"
        "reserve       = on\n"
        "hardening     = off\n"
        "purge         = on\n"
        "fullest_first = on\n"
        "warm_pages    = 32\n"
        "reserve_pages = auto\n");
    config_load(&c, path);
    unlink(path);
    check("port from [server]", c.port == 17000);
    check("pool_size from [memory]", c.pool.pool_size == 16u << 20);
    check("switches from [allocator]",
          c.pool.flags == (APOOL_F_RESERVE | APOOL_F_PURGE | APOOL_F_FULLEST));
    check("warm_pages 32, reserve automatic", c.pool.warm_pages == 32 && c.pool.reserve_pages == POOL_AUTO);

    /* the PMAD-era layout: pool_size under [allocator], class lines ignored */
    config_defaults(&c);
    path = write_tmp("[server]\nport = 16381\n[allocator]\npool_size = 1048576\nclass = 32,10\nclass = 64,90\n");
    config_load(&c, path);
    unlink(path);
    check("old file still loads", c.pool.pool_size == 1048576 && c.pool.flags == APOOL_F_DEFAULT);

    check("unknown section refused",      refused("[network]\nport = 1\n"));
    check("unknown key refused",          refused("[allocator]\npurj = on\n"));
    check("port in [allocator] refused",  refused("[allocator]\nport = 16381\n"));
    check("bad value refused",            refused("[allocator]\npurge = sometimes\n"));
    check("line without = refused",       refused("[allocator]\npurge on\n"));
    check("port 70000 refused",           refused("[server]\nport = 70000\n"));

    FILE* f = fopen(repo_conf, "r");
    if (!f) {
        printf("  (skipped: %s not found; pass its path as the first argument)\n", repo_conf);
        return;
    }
    fclose(f);
    Config want, got;
    config_defaults(&want);
    config_defaults(&got);
    got.port = 1;                          /* so the file has to set it */
    got.pool.pool_size = 1;
    got.pool.flags = 0;
    config_load(&got, repo_conf);
    check("quicx.conf gives exactly the built-in defaults",
          got.port == want.port && got.pool.pool_size == want.pool.pool_size &&
          got.pool.flags == want.pool.flags && got.pool.warm_pages == want.pool.warm_pages &&
          got.pool.reserve_pages == want.pool.reserve_pages);
}

int main(int argc, char* argv[]) {
    test_defaults();
    test_values();
    test_apply();
    test_files(argc > 1 ? argv[1] : "quicx.conf");
    printf("\npassed: %d  failed: %d\n", passed, failed);
    return failed ? 1 : 0;
}
