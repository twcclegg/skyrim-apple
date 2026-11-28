/* Checks for runtime/comfort_ini.c: the comfort defaults for immersive play, merged into LB_INI. No game code or
 * data. The merged value is read back with the same splitting rule skyrim_shims.c uses (first match wins, any
 * case), so the checks are about what the game would be told. Each check prints a line; the exit status is the
 * number that failed. */
#include "comfort_ini.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

static int g_failed;
#define CHECK(ok, ...) do { \
    int ok_ = (ok); \
    printf("%s  ", ok_ ? "ok  " : "FAIL"); printf(__VA_ARGS__); printf("\n"); \
    if (!ok_) g_failed++; \
} while (0)

/* skyrim_shims.c ini_override, as a function of the LB_INI value: the value the game gets for [sec] key, or NULL.
 * *entries gets the number of entries it keeps. */
static const char *lookup(const char *e, const char *sec, const char *key, int *entries) {
    static struct { char sec[64], key[64], val[64]; } over[COMFORT_INI_MAX_ENTRIES];
    int n = 0;
    while (e && *e && n < COMFORT_INI_MAX_ENTRIES) {
        while (*e == ' ') e++;
        const char *end = strchr(e, ';'), *colon = strchr(e, ':'), *eq = strchr(e, '=');
        if (!end) end = e + strlen(e);
        if (colon && eq && colon < eq && eq < end) {
            snprintf(over[n].sec, 64, "%.*s", (int)(colon - e), e);
            snprintf(over[n].key, 64, "%.*s", (int)(eq - colon - 1), colon + 1);
            snprintf(over[n].val, 64, "%.*s", (int)(end - eq - 1), eq + 1);
            n++;
        }
        e = *end ? end + 1 : end;
    }
    if (entries) *entries = n;
    for (int i = 0; i < n; i++)
        if (!strcasecmp(over[i].sec, sec) && !strcasecmp(over[i].key, key)) return over[i].val;
    return NULL;
}

static int is(const char *value, const char *want) { return value && want ? !strcmp(value, want) : value == want; }

/* All three defaults as the game would read them from merged. */
static int all_three(const char *merged) {
    return is(lookup(merged, "VATS", "bVATSDisable", NULL), "1") &&
           is(lookup(merged, "Camera", "bDisableAutoVanityMode", NULL), "1") &&
           is(lookup(merged, "Imagespace", "bDoRadialBlur", NULL), "0");
}

int main(void) {
    char out[4096];
    int entries;

    /* When they apply. */
    CHECK(comfort_ini_wanted(NULL, 1) == 1, "unset, immersive launch: on");
    CHECK(comfort_ini_wanted(NULL, 0) == 0, "unset, windowed launch: off (windowed play is unchanged)");
    CHECK(comfort_ini_wanted("", 1) == 1 && comfort_ini_wanted("", 0) == 0, "empty counts as unset");
    CHECK(comfort_ini_wanted("0", 1) == 0, "LB_COMFORT_INI=0 opts out of an immersive launch");
    CHECK(comfort_ini_wanted("1", 0) == 1, "LB_COMFORT_INI=1 turns them on for any launch");

    /* No LB_INI at all. */
    CHECK(comfort_ini_merge(NULL, out, sizeof out) == 3 && all_three(out), "no LB_INI: all three added (%s)", out);
    CHECK(!strcmp(out, "VATS:bVATSDisable=1;Camera:bDisableAutoVanityMode=1;Imagespace:bDoRadialBlur=0"),
          "no LB_INI: exactly the three entries");
    CHECK(comfort_ini_merge("", out, sizeof out) == 3 && all_three(out), "empty LB_INI: all three added");

    /* The player's own entries stay first and untouched. */
    const char *user = "Display:iShadowMapResolution=1024; Grass:bAllowCreateGrass=0";
    CHECK(comfort_ini_merge(user, out, sizeof out) == 3 && all_three(out), "two other entries: all three added");
    CHECK(!strncmp(out, user, strlen(user)), "the user's text is kept as it was, in front");
    CHECK(is(lookup(out, "Display", "iShadowMapResolution", &entries), "1024") &&
          is(lookup(out, "grass", "ballowcreategrass", NULL), "0") && entries == 5,
          "the user's entries still read back, 5 entries in all");
    CHECK(comfort_ini_merge("Display:iBlurDeferredShadowMask=3;", out, sizeof out) == 3 && all_three(out) &&
          !strstr(out, ";;"), "a trailing ';' gets no second one (%s)", out);

    /* Opting out of one setting by naming it. */
    CHECK(comfort_ini_merge("VATS:bVATSDisable=0", out, sizeof out) == 2 &&
          is(lookup(out, "VATS", "bVATSDisable", NULL), "0") &&
          is(lookup(out, "Camera", "bDisableAutoVanityMode", NULL), "1") &&
          is(lookup(out, "Imagespace", "bDoRadialBlur", NULL), "0"), "kill-cam kept by the player, other two added");
    CHECK(comfort_ini_merge("vats:bvatsdisable=0; IMAGESPACE:BDORADIALBLUR=1", out, sizeof out) == 1 &&
          is(lookup(out, "VATS", "bVATSDisable", NULL), "0") && is(lookup(out, "Imagespace", "bDoRadialBlur", NULL), "1") &&
          is(lookup(out, "Camera", "bDisableAutoVanityMode", NULL), "1"), "names match in any case, after a space");
    CHECK(comfort_ini_merge("VATS:bVATSDisable=0;Camera:bDisableAutoVanityMode=0;Imagespace:bDoRadialBlur=1", out,
                            sizeof out) == 0 && !strcmp(out, "VATS:bVATSDisable=0;Camera:bDisableAutoVanityMode=0;Imagespace:bDoRadialBlur=1"),
          "all three set by the player: nothing added");
    /* Near misses are other settings. */
    CHECK(comfort_ini_merge("VATS:bVATSDisabled=0;VAT:bVATSDisable=0;Camera:bVATSDisable=0", out, sizeof out) == 3 &&
          all_three(out), "a longer key, a shorter section and another section are not the same setting");
    /* An entry the runtime passes over doesn't count as naming the setting. */
    CHECK(comfort_ini_merge("VATS bVATSDisable=0;junk", out, sizeof out) == 3 && all_three(out),
          "malformed entries are ignored, as the runtime ignores them");

    /* LB_INI holds 32 entries; the player's come first. */
    char many[2048] = "";
    for (int i = 0; i < 30; i++) snprintf(many + strlen(many), sizeof many - strlen(many), "%sS:k%d=1", i ? ";" : "", i);
    CHECK(comfort_ini_merge(many, out, sizeof out) == 2 && is(lookup(out, "VATS", "bVATSDisable", &entries), "1") &&
          is(lookup(out, "Camera", "bDisableAutoVanityMode", NULL), "1") &&
          lookup(out, "Imagespace", "bDoRadialBlur", NULL) == NULL && entries == 32 && is(lookup(out, "S", "k29", NULL), "1"),
          "30 user entries: two defaults fit, none of the user's is pushed out");
    strcat(many, ";S:k30=1;S:k31=1");
    CHECK(comfort_ini_merge(many, out, sizeof out) == 0 && !strcmp(out, many), "32 user entries: nothing added");

    /* A buffer that is too small is reported, never overrun. */
    char small[40];
    memset(small, 'x', sizeof small);
    CHECK(comfort_ini_merge(NULL, small, 30) == -1 && small[0] == 0 && small[30] == 'x', "30 bytes for 78: -1, empty, in bounds");
    CHECK(comfort_ini_merge("Display:iShadowMapResolution=1024", small, 20) == -1 && small[0] == 0, "user text alone too long: -1");
    CHECK(comfort_ini_merge(NULL, out, 79) == 3 && strlen(out) == 78, "an exact fit works");
    CHECK(comfort_ini_merge(NULL, out, 78) == -1, "one byte short does not");

    /* The whole path the runtime takes, through the environment and the launch flag. */
    unsetenv("LB_COMFORT_INI");
    skyrim_comfort_ini_immersive(0);
    user = "Display:fGamma=1.0";
    CHECK(comfort_ini_effective(user) == user, "windowed launch: the very same LB_INI string");
    CHECK(comfort_ini_effective(NULL) == NULL, "windowed launch without LB_INI: still none");
    skyrim_comfort_ini_immersive(1);
    const char *effective = comfort_ini_effective(user);
    CHECK(effective != user && all_three(effective) && is(lookup(effective, "Display", "fGamma", NULL), "1.0"),
          "immersive launch: defaults behind the user's entry");
    CHECK(all_three(comfort_ini_effective(NULL)), "immersive launch without LB_INI: the three defaults");
    setenv("LB_COMFORT_INI", "0", 1);
    CHECK(comfort_ini_effective(user) == user, "immersive launch with LB_COMFORT_INI=0: unchanged");
    skyrim_comfort_ini_immersive(0);
    setenv("LB_COMFORT_INI", "1", 1);
    CHECK(all_three(comfort_ini_effective(NULL)), "LB_COMFORT_INI=1 without an immersive launch: the three defaults");
    const char *own = "VATS:bVATSDisable=1;Camera:bDisableAutoVanityMode=1;Imagespace:bDoRadialBlur=0";
    CHECK(comfort_ini_effective(own) == own, "nothing to add: the user's string itself");

    printf("%d failed\n", g_failed);
    return g_failed;
}
