#pragma once

/* Foveating the game's own render (docs/handoff/foveated-render.md). Prototype; off unless LB_GAME_FOVEATION=1.
 *
 * The game keeps believing its back buffer is screen_width x screen_height. Render targets and depth buffers
 * of that size are created at physical_width x physical_height instead, scene draws into them are rasterized
 * through a rate map, and whoever shows the frame (the immersive compositor) undoes the map.
 *
 * The map itself is a Metal object, so the app makes it and hands it over here: DXVK asks the provider once
 * per swap-chain (re)creation, before any back-buffer-sized target exists. */

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct skyrim_foveation_map {
  uint32_t screen_width;     /* in: the game's back-buffer size */
  uint32_t screen_height;
  uint32_t physical_width;   /* out: the map's physicalSize, i.e. the size the targets get */
  uint32_t physical_height;
  void*    mtl_rate_map;     /* out: id<MTLRasterizationRateMap>, kept alive by the provider until the next
                                call or exit. NULL: no map, a plain uniform scale (plumbing tests only) */
} skyrim_foveation_map;

/* Fills the out fields for the given screen size. Returns non-zero to foveate, zero to leave the game alone. */
typedef int (*skyrim_foveation_provider_fn)(void* user, skyrim_foveation_map* map);

void skyrim_foveation_set_provider(skyrim_foveation_provider_fn fn, void* user);

/* The map in use (what the provider last returned), for the side that undoes it. Returns zero when off. */
int skyrim_foveation_current(skyrim_foveation_map* map);

/* What the last presented frame did, to see how the game's passes were sorted (also logged every 60 frames with
 * LB_GAME_FOVEATION_LOG=1). Returns zero when off. */
typedef struct skyrim_foveation_counts {
  uint32_t mapped;          /* draws into foveated targets through the map */
  uint32_t unmapped;        /* full-screen quads run in warped space, without the map */
  uint32_t hazards;         /* scene draws that sample a warped texture: wrong texels, not handled yet */
  uint32_t tainted;         /* quads that carried a warped picture into an ordinary target */
  uint32_t mixed_targets;   /* framebuffers mixing a foveated and an ordinary attachment */
  uint32_t partial_clears;  /* clears of part of a foveated target (placed by plain proportion) */
  uint32_t partial_copies;  /* StretchRect of part of a foveated surface (same) */
  uint32_t mode_switches;   /* mapped <-> unmapped changes; each restarts the render pass */
} skyrim_foveation_counts;

int skyrim_foveation_last_frame(skyrim_foveation_counts* counts);

#ifdef __cplusplus
}
#endif
