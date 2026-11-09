#include "platform.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "config.h"

static char* trim(char* s) {
    while (isspace((unsigned char)*s)) s++;
    if (*s == 0) return s;

    char* end = s + strlen(s) - 1;
    while (end > s && isspace((unsigned char)*end)) end--;
    end[1] = '\0';
    return s;
}

/* prints "error: FILE:LINE: ..." and exits; quicx does not start on a config it cannot follow */
static void fail(const char* path, int line, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "error: %s:%d: ", path, line);
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
    va_end(ap);
    exit(1);
}

void config_defaults(Config* c) {
    c->port = DEFAULT_PORT;
    pool_config_defaults(&c->pool);
}

void config_validate(const Config* c) {
    if (c->port <= 0 || c->port > 65535) {
        fprintf(stderr, "error: port must be between 1 and 65535 (got %d)\n", c->port);
        exit(1);
    }

    char err[160];
    if (pool_config_check(&c->pool, err, sizeof(err)) != 0) {
        fprintf(stderr, "error: %s\n", err);
        exit(1);
    }
}

typedef enum { SEC_NONE, SEC_SERVER, SEC_MEMORY, SEC_ALLOCATOR } Section;

void config_load(Config* c, const char* path) {
    FILE* f = fopen(path, "r");
    if (!f) {
        fprintf(stderr, "error: cannot open config: %s\n", path);
        exit(1);
    }

    Section sec = SEC_NONE;
    int     lineno = 0, legacy_classes = 0;
    char    line[512];

    while (fgets(line, sizeof(line), f)) {
        lineno++;

        char* hash = strchr(line, '#');   /* comments run to the end of the line */
        if (hash) *hash = '\0';
        char* l = trim(line);
        if (l[0] == '\0') continue;

        if (l[0] == '[') {
            char* close = strchr(l, ']');
            if (!close || close[1] != '\0') fail(path, lineno, "a section looks like [server]");
            *close = '\0';
            char* name = trim(l + 1);
            if      (strcmp(name, "server") == 0)    sec = SEC_SERVER;
            else if (strcmp(name, "memory") == 0)    sec = SEC_MEMORY;
            else if (strcmp(name, "allocator") == 0) sec = SEC_ALLOCATOR;
            else fail(path, lineno, "unknown section [%s] (known: server, memory, allocator)", name);
            continue;
        }

        char* eq = strchr(l, '=');
        if (!eq) fail(path, lineno, "expected key = value");
        *eq = '\0';
        char* k = trim(l);
        char* v = trim(eq + 1);
        if (k[0] == '\0' || v[0] == '\0') fail(path, lineno, "expected key = value");

        if (strcmp(k, "port") == 0) {
            if (sec != SEC_NONE && sec != SEC_SERVER) fail(path, lineno, "port belongs in [server]");
            char* end;
            long  p = strtol(v, &end, 10);
            if (*end || p <= 0 || p > 65535) fail(path, lineno, "port must be 1-65535, not '%s'", v);
            c->port = (int)p;
            continue;
        }

        /* the old static allocator (PMAD) was laid out with class = size,percent lines */
        if (strcmp(k, "class") == 0) {
            legacy_classes++;
            continue;
        }

        if (sec == SEC_SERVER) fail(path, lineno, "unknown key '%s' in [server]", k);

        char          err[160];
        PoolKeyResult r = pool_config_set(&c->pool, k, v, err, sizeof(err));
        if (r == POOL_KEY_BAD)     fail(path, lineno, "%s", err);
        if (r == POOL_KEY_UNKNOWN) fail(path, lineno, "unknown key '%s'", k);
    }

    fclose(f);

    if (legacy_classes)
        printf("note: %d 'class' line%s ignored — they laid out the old static allocator; "
               "apool sizes itself\n", legacy_classes, legacy_classes == 1 ? "" : "s");

    config_validate(c);
    printf("config loaded: %s\n", path);
}
