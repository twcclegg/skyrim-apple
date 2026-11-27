/* Skyrim Apple port: comfort defaults for immersive play, given to the game as INI values.
 *
 * In the headset the picture follows the head, so anything that turns or smears the picture without the player
 * asking is felt, not just seen. Three things the 2011 game does by itself can be switched off by settings it
 * already has (all three are in the 1.9.32 executable, Skyrim.ini collection, read once at INI load):
 *
 *   VATS:bVATSDisable=1             no VATS playback camera: the kill-cam that follows an arrow or a spell, and the
 *                                   one a melee kill move switches to (the only reader gates both)
 *   Camera:bDisableAutoVanityMode=1 no orbiting camera after two minutes without input
 *   Imagespace:bDoRadialBlur=0      no radial blur pass (hits, staggers, shouts)
 *
 * They are added to LB_INI (runtime/skyrim_shims.c), behind anything the player set there, so the player's INI
 * files are never touched and each one is logged as "ini: LB_INI sets ..." when the game reads it.
 *
 * When: LB_COMFORT_INI unset, only when the launcher said this launch plays in the immersive space
 * (skyrim_comfort_ini_immersive, Play Immersive on Apple Vision Pro); windowed play and every other platform read
 * exactly what they did before. LB_COMFORT_INI=0: never. LB_COMFORT_INI=1: on any launch, any platform (for
 * checking the three settings on a desktop build). One setting is opted out by naming it in LB_INI with the value
 * wanted, for example LB_INI=VATS:bVATSDisable=0.
 *
 * Checks, without the game: python3 tests/comfort_ini/run.py */
#ifndef COMFORT_INI_H
#define COMFORT_INI_H
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif

/* As many "Section:key=value" entries as LB_INI holds (INI_OVERRIDES in skyrim_shims.c). */
#define COMFORT_INI_MAX_ENTRIES 32

/* Whether the defaults apply: setting is LB_COMFORT_INI's value or NULL. */
int comfort_ini_wanted(const char *setting, int immersive_launch);

/* Writes to out the LB_INI value with the comfort defaults added after the user's own entries (user may be NULL).
 * A default is left out when the user already names its Section:key (any case), and when LB_INI's entry limit is
 * reached. Returns how many defaults were added, or -1 when out is too small (out is then an empty string and
 * the user's value should be used as it is). */
int comfort_ini_merge(const char *user, char *out, size_t out_size);

/* The LB_INI value the runtime should parse: user itself (possibly NULL) when the defaults don't apply or add
 * nothing, else a merged copy that stays valid for the life of the process. Reads LB_COMFORT_INI and the launch
 * flag, and says on stderr what it did. Call it once. */
const char *comfort_ini_effective(const char *user);

/* From the launcher, before the game starts: this launch plays in the immersive space (1) or not (0). */
void skyrim_comfort_ini_immersive(int on);

#ifdef __cplusplus
}
#endif
#endif
