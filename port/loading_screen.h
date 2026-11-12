/*
 * The precompile gate and its 8-bit screen (loading_screen.cpp).
 */
#pragma once

#include <stdint.h>

/* Before the game starts: the vertex shaders built for this version, on a screen of its own, unless they are
 * already. Returns when the game can start (built, or the build failed and said so); quits if the window is closed. */
int gowa_precompile_gate(void);

/* The game's own window is up: the screen goes. Nothing when there is none. */
void gowa_loading_screen_close(void);

/* --loading-preview <folder>: the screen in a few states as BMP files. */
int gowa_loading_preview(const char* dir);

/* The screen's scene at t_ms (Greek ruins at sunset, drawn by code), on its canvas of GOWA_SCENE_W x GOWA_SCENE_H
 * pixels, 0x00RRGGBB, top row first. The launcher's header shows it. */
enum { GOWA_SCENE_W = 640, GOWA_SCENE_H = 360 };
const uint32_t* gowa_scene_frame(uint64_t t_ms);

/* The screen's pixel font: seven rows of five dots for c, the top bit of each row's five the left dot; NULL for a
 * space or a character it has not got. */
const uint8_t* gowa_pixel_glyph(char c);
