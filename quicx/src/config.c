#include "platform.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#include "config.h"

static char* trim(char* s) {
    while (isspace((unsigned char)*s)) s++;
    if (*s == 0) return s;

    char* end = s + strlen(s) - 1;
    while (end > s && isspace((unsigned char)*end)) end--;
    end[1] = '\0';
    return s;
}

void config_defaults(Config* c) {
    c->port        = DEFAULT_PORT;
    c->pool_size   = 1048576;
    c->class_count = 6;

    c->sizes[0] = 32;   c->percentages[0] = 10;
    c->sizes[1] = 64;   c->percentages[1] = 25;
    c->sizes[2] = 128;  c->percentages[2] = 25;
    c->sizes[3] = 256;  c->percentages[3] = 20;
    c->sizes[4] = 512;  c->percentages[4] = 12;
    c->sizes[5] = 1024; c->percentages[5] =  8;
}

void config_validate(Config* c) {
    if (c->class_count == 0) {
        fprintf(stderr, "error: no allocator classes defined\n");
        exit(1);
    }

    int sum = 0;
    for (int i = 0; i < c->class_count; i++)
        sum += c->percentages[i];

    if (c->pool_size == 0) {
        fprintf(stderr, "error: pool_size must be greater than 0\n");
        exit(1);
    }

    if (sum != 100) {
        fprintf(stderr, "error: class percentages must sum to 100 (got %d)\n", sum);
        exit(1);
    }
}

void config_load(Config* c, const char* path) {
    FILE* f = fopen(path, "r");
    if (!f) {
        fprintf(stderr, "error: cannot open config: %s\n", path);
        exit(1);
    }

    c->class_count = 0;

    char line[512];

    while (fgets(line, sizeof(line), f)) {
        char* l = trim(line);

        if (l[0] == '#' || l[0] == '\0')
            continue;

        if (l[0] == '[')
            continue;

        char key[64], val[256];

        if (sscanf(l, "%63[^=]=%255[^\n]", key, val) != 2)
            continue;

        char* k = trim(key);
        char* v = trim(val);

        if (strcmp(k, "port") == 0) {
            c->port = atoi(v);
        }
        else if (strcmp(k, "pool_size") == 0) {
            c->pool_size = (size_t)atoll(v);
        }
        else if (strcmp(k, "class") == 0) {
            if (c->class_count >= MAX_PMAD_CLASSES) {
                fprintf(stderr, "error: too many classes (max %d)\n", MAX_PMAD_CLASSES);
                exit(1);
            }

            size_t size = 0, percent = 0;

            if (sscanf(v, "%zu,%zu", &size, &percent) != 2) {
                fprintf(stderr, "error: invalid class format: %s\n", v);
                exit(1);
            }

            c->sizes[c->class_count]       = size;
            c->percentages[c->class_count] = percent;
            c->class_count++;
        }
    }

    fclose(f);
    config_validate(c);

    printf("config loaded: %s\n", path);
}
