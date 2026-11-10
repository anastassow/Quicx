#include "platform.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "pool_config.h"
#include "admission.h"

/* the [allocator] on/off keys and the apool feature each one switches */
static const struct {
    const char* key;
    uint32_t    flag;
    const char* name;  /* as pool_config_describe prints it */
} SWITCHES[] = {
    { "reserve",       APOOL_F_RESERVE, "reserve"       },
    { "hardening",     APOOL_F_HARDEN,  "hardening"     },
    { "purge",         APOOL_F_PURGE,   "purge"         },
    { "fullest_first", APOOL_F_FULLEST, "fullest-first" },
};

#define SWITCH_COUNT (sizeof(SWITCHES) / sizeof(SWITCHES[0]))

void pool_config_defaults(PoolConfig* pc) {
    pc->pool_size = DEFAULT_POOL_SIZE;
#ifdef QUICX_POOL_FLAGS   /* test builds only: -DQUICX_POOL_FLAGS=APOOL_F_ALL turns every feature on */
    pc->flags = QUICX_POOL_FLAGS;
#else
    pc->flags = APOOL_F_DEFAULT;
#endif
    pc->warm_pages    = POOL_AUTO;
    pc->reserve_pages = POOL_AUTO;
    pc->admission     = ADMISSION_DEFAULT;
}

/* on / off, yes / no, true / false, 1 / 0 */
static int parse_switch(const char* v, int* on) {
    static const char* yes[] = { "on", "yes", "true", "1" };
    static const char* no[]  = { "off", "no", "false", "0" };
    for (size_t i = 0; i < 4; i++) {
        if (strcasecmp(v, yes[i]) == 0) { *on = 1; return 0; }
        if (strcasecmp(v, no[i]) == 0)  { *on = 0; return 0; }
    }
    return -1;
}

/* a byte count with an optional K, M or G suffix, 1024-based (KiB, MiB, GiB also accepted) */
static int parse_size(const char* v, size_t* out) {
    if (!isdigit((unsigned char)*v)) return -1;

    char* end;
    errno = 0;
    unsigned long long n = strtoull(v, &end, 10);
    if (errno) return -1;

    while (isspace((unsigned char)*end)) end++;

    unsigned long long mul = 1;
    if (*end) {
        switch (toupper((unsigned char)*end)) {
            case 'K': mul = 1ull << 10; break;
            case 'M': mul = 1ull << 20; break;
            case 'G': mul = 1ull << 30; break;
            default:  return -1;
        }
        end++;
        if (strcasecmp(end, "iB") == 0) end += 2;
        if (*end) return -1;
    }

    if (n == 0 || n > SIZE_MAX / mul) return -1;
    *out = (size_t)(n * mul);
    return 0;
}

/* auto, or a whole number of pages above 0 */
static int parse_pages(const char* v, uint32_t* out) {
    if (strcasecmp(v, "auto") == 0) { *out = POOL_AUTO; return 0; }
    if (!isdigit((unsigned char)*v)) return -1;

    char* end;
    errno = 0;
    unsigned long n = strtoul(v, &end, 10);
    if (errno || *end || n == 0 || n > UINT32_MAX) return -1;
    *out = (uint32_t)n;
    return 0;
}

PoolKeyResult pool_config_set(PoolConfig* pc, const char* key, const char* value,
                              char* err, size_t errlen) {
    if (strcmp(key, "pool_size") == 0) {
        if (parse_size(value, &pc->pool_size) != 0) {
            snprintf(err, errlen, "pool_size must be a byte count such as 8388608 or 8M, not '%s'", value);
            return POOL_KEY_BAD;
        }
        return POOL_KEY_OK;
    }

    for (size_t i = 0; i < SWITCH_COUNT; i++) {
        if (strcmp(key, SWITCHES[i].key) != 0) continue;
        int on;
        if (parse_switch(value, &on) != 0) {
            snprintf(err, errlen, "%s must be on or off, not '%s'", key, value);
            return POOL_KEY_BAD;
        }
        if (on) pc->flags |= SWITCHES[i].flag;
        else    pc->flags &= ~SWITCHES[i].flag;
        return POOL_KEY_OK;
    }

    if (strcmp(key, "admission") == 0) {
        if      (strcmp(value, "refuse") == 0) pc->admission = ADMISSION_REFUSE;
        else if (strcmp(value, "oldest") == 0) pc->admission = ADMISSION_OLDEST;
        else if (strcmp(value, "value") == 0)  pc->admission = ADMISSION_VALUE;
        else {
            snprintf(err, errlen, "admission must be refuse, oldest or value, not '%s'", value);
            return POOL_KEY_BAD;
        }
        return POOL_KEY_OK;
    }

    uint32_t* pages = strcmp(key, "warm_pages") == 0    ? &pc->warm_pages
                    : strcmp(key, "reserve_pages") == 0 ? &pc->reserve_pages
                    : NULL;
    if (pages) {
        if (parse_pages(value, pages) != 0) {
            snprintf(err, errlen, "%s must be auto or a page count above 0, not '%s'", key, value);
            return POOL_KEY_BAD;
        }
        return POOL_KEY_OK;
    }

    return POOL_KEY_UNKNOWN;
}

int pool_config_check(const PoolConfig* pc, char* err, size_t errlen) {
    if (pc->pool_size == 0) {
        snprintf(err, errlen, "pool_size must be greater than 0");
        return -1;
    }
    if (pc->reserve_pages != POOL_AUTO && !(pc->flags & APOOL_F_RESERVE)) {
        snprintf(err, errlen, "reserve_pages is set but reserve is off");
        return -1;
    }
    if (pc->warm_pages != POOL_AUTO && !(pc->flags & APOOL_F_PURGE)) {
        snprintf(err, errlen, "warm_pages is set but purge is off (without purge every empty page stays warm)");
        return -1;
    }
    return 0;
}

int pool_config_apply(const PoolConfig* pc, Apool* pool, PoolRole role) {
    pool->flags = pc->flags;
    if (role != POOL_TASKS) return 0;

    if (pc->warm_pages != POOL_AUTO) {
        if (pc->warm_pages > pool->num_pages) return -1;
        pool->warm = pc->warm_pages;
    }
    if (pc->reserve_pages != POOL_AUTO) {
        if (pc->reserve_pages >= pool->num_pages) return -1;
        pool->reserve = pc->reserve_pages;
    }
    return 0;
}

void pool_config_describe(const PoolConfig* pc, char* out, size_t n) {
    size_t used = 0;
    out[0] = '\0';
    for (size_t i = 0; i < SWITCH_COUNT && used < n; i++) {
        if (!(pc->flags & SWITCHES[i].flag)) continue;
        used += (size_t)snprintf(out + used, n - used, "%s%s", used ? ", " : "", SWITCHES[i].name);
    }
    if (used == 0) used = (size_t)snprintf(out, n, "no optional features");
    static const char* modes[] = { "refuse", "oldest", "value" };
    if (used < n && pc->admission <= ADMISSION_VALUE)
        snprintf(out + used, n - used, "; when full: %s", modes[pc->admission]);
}
