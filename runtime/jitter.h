/* Sub-pixel camera jitter for temporal upscaling (docs/handoff/temporal-upscale.md).
 *
 * A temporal upscaler builds a sharp picture out of several small ones, and for that each frame has to sample the
 * scene at slightly different places inside its pixels. The world camera's frustum is shifted by a fraction of a
 * pixel, a different fraction each frame, and the frame carries the shift it was drawn with.
 *
 * The shift goes into the frustum the game is about to use (runtime/d3d9_bridge.cpp calls jitter_apply from the
 * camera's update), so everything drawn through that camera moves together: the world, the first-person pass
 * and the scene depth. Screen-space passes and the interface go through no camera and stay where they are.
 *
 * Portable C with no game in it; tests/temporal/run.py checks it on any machine. Called on the game's main
 * thread, except jitter_configure, which may come from another one. */
#ifndef JITTER_H
#define JITTER_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

/* Jitter over a sequence of `phases` positions (Halton 2,3), each within half a pixel; 0 turns it off.
 * MetalFX suggests 8 x (output size / input size) squared. Takes effect at the next frame. */
void jitter_configure(int phases);
/* For tests: the same shift every frame, in pixels (may be whole pixels, to see it). Turns the sequence off. */
void jitter_fixed(float x, float y);
/* For tests: multiplies the sequence, so the shifts are whole pixels that can be measured in a screenshot. */
void jitter_test_scale(float scale);
int jitter_on(void);

/* This frame's shift in pixels: the picture moves this far right (x) and down (y). */
void jitter_pixels(float out[2]);

/* The frustum (tangents left, right, top, bottom) a camera is about to render width x height pixels with. Gives it
 * this frame's shift and returns 1 if that changed it. A frustum this function shifted earlier (the game keeps
 * it from frame to frame, and copies it between cameras and back) is first put back as the game set it, so
 * shifts never add up; with jitter off, or a width or height of 0 (the caller wants no shift this frame: a menu
 * is up), that is all that happens. */
int jitter_apply(float frustum[4], uint32_t width, uint32_t height);

/* Undoes jitter_apply on a frustum read back from the game: if it is one that was shifted, restores it as the
 * game set it, writes the shift in pixels and returns 1. Otherwise writes 0, 0 and returns 0. */
int jitter_remove(float frustum[4], float pixels[2]);

/* A frame has been presented: the next one gets the next position. */
void jitter_next_frame(void);

/* Position `index` (from 0) of a sequence of `phases`: x and y from -0.5 to 0.5. */
void jitter_sequence(int index, int phases, float out[2]);

#ifdef __cplusplus
}
#endif
#endif
