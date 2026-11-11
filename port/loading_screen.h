/*
 * The precompile gate and its 8-bit screen (loading_screen.cpp).
 */
#pragma once

/* Before the game starts: the vertex shaders built for this version, on a screen of its own, unless they are
 * already. Returns when the game can start (built, or the build failed and said so); quits if the window is closed. */
int gowa_precompile_gate(void);

/* The game's own window is up: the screen goes. Nothing when there is none. */
void gowa_loading_screen_close(void);

/* --loading-preview <folder>: the screen in a few states as BMP files. */
int gowa_loading_preview(const char* dir);
