/* Comfort defaults for immersive play, merged into LB_INI. See comfort_ini.h. */
#include "comfort_ini.h"
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

static const char *const g_defaults[] = {
    "VATS:bVATSDisable=1",
    "Camera:bDisableAutoVanityMode=1",
    "Imagespace:bDoRadialBlur=0",
};
#define DEFAULTS ((int)(sizeof g_defaults / sizeof g_defaults[0]))

static atomic_int g_immersive_launch;

void skyrim_comfort_ini_immersive(int on) { atomic_store(&g_immersive_launch, on != 0); }

int comfort_ini_wanted(const char *setting, int immersive_launch) {
    if (setting && *setting) return atoi(setting) != 0;
    return immersive_launch != 0;
}

/* The next entry of an LB_INI value, split exactly as skyrim_shims.c splits it: entries end at ';', spaces before
 * one are skipped, and one without "Section:key=" in that order is passed over. Returns 0 at the end; *valid says
 * whether the entry counts, with the section at [e, colon) and the key at (colon, eq). */
static int next_entry(const char **at, const char **e_out, const char **colon_out, const char **eq_out, int *valid) {
    const char *e = *at;
    if (!e || !*e) return 0;
    while (*e == ' ') e++;
    const char *end = strchr(e, ';'), *colon = strchr(e, ':'), *eq = strchr(e, '=');
    if (!end) end = e + strlen(e);
    *valid = colon && eq && colon < eq && eq < end;
    *e_out = e;
    *colon_out = colon;
    *eq_out = eq;
    *at = *end ? end + 1 : end;
    return 1;
}

/* Does list set the Section:key that entry ("Section:key=value") sets? count gets its number of valid entries. */
static int names_key(const char *list, const char *entry, int *count) {
    const char *want_colon = strchr(entry, ':'), *want_eq = strchr(entry, '=');
    size_t sec = (size_t)(want_colon - entry), key = (size_t)(want_eq - want_colon - 1);
    const char *at = list, *e, *colon, *eq;
    int valid, found = 0;
    *count = 0;
    while (next_entry(&at, &e, &colon, &eq, &valid)) {
        if (!valid) continue;
        (*count)++;
        if ((size_t)(colon - e) == sec && (size_t)(eq - colon - 1) == key &&
            !strncasecmp(e, entry, sec) && !strncasecmp(colon + 1, want_colon + 1, key))
            found = 1;
    }
    return found;
}

int comfort_ini_merge(const char *user, char *out, size_t out_size) {
    if (!user) user = "";
    size_t len = strlen(user);
    if (!out_size || len + 1 > out_size) {
        if (out_size) out[0] = 0;
        return -1;
    }
    memcpy(out, user, len + 1);
    int added = 0;
    for (int i = 0; i < DEFAULTS; i++) {
        int entries;
        /* Checked against the user's entries only: the defaults never repeat one another. */
        if (names_key(user, g_defaults[i], &entries)) continue;
        if (entries + added >= COMFORT_INI_MAX_ENTRIES) break;
        int separator = len > 0 && out[len - 1] != ';';
        size_t need = strlen(g_defaults[i]) + (size_t)separator;
        if (len + need + 1 > out_size) {
            out[0] = 0;
            return -1;
        }
        if (separator) out[len++] = ';';
        memcpy(out + len, g_defaults[i], need - (size_t)separator + 1);
        len += need - (size_t)separator;
        added++;
    }
    return added;
}

const char *comfort_ini_effective(const char *user) {
    static char merged[4096];
    const char *setting = getenv("LB_COMFORT_INI");
    int immersive = atomic_load(&g_immersive_launch);
    if (!comfort_ini_wanted(setting, immersive)) {
        if (immersive) fprintf(stderr, "ini: comfort defaults off (LB_COMFORT_INI=%s)\n", setting ? setting : "");
        return user;
    }
    int added = comfort_ini_merge(user, merged, sizeof merged);
    if (added < 0) {
        fprintf(stderr, "ini: comfort defaults not added: LB_INI is too long\n");
        return user;
    }
    fprintf(stderr, "ini: comfort defaults for %s: %d of %d added, LB_INI=%s\n",
            immersive ? "immersive play" : "LB_COMFORT_INI", added, DEFAULTS, merged);
    return added ? merged : user;
}
