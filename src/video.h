// Display output: OpenGL / OpenGL ES with filter shaders, or plain SDL rendering as a fallback.
#pragma once
#include <SDL2/SDL.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum { FILTER_SHARP, FILTER_SMOOTH, FILTER_SCANLINES, FILTER_CRT, NFILTERS };

// Create the window. Uses OpenGL or OpenGL ES when available.
SDL_Window *video_open(const char *title, int w, int h);
void video_set_vsync(bool on);
// game: 320x240 0x00RRGGBB. overlay: 320x240 0xAARRGGBB drawn on top, unfiltered (may be NULL).
void video_present(const uint32_t *game, const uint32_t *overlay, int filter, int scale_mode);
// Fast path for 15-bit arcade frames converted to RGB565 (half the upload bandwidth on handheld GPUs).
void video_present_565(const uint16_t *game565, const uint32_t *overlay, int filter, int scale_mode);
const char *video_backend(void);
bool video_is_gles(void);
// Load a libretro .glslp preset (NULL or "" = back to the built-in filter).
bool video_set_shader(const char *path, char *err, size_t errlen);
bool video_shaders_supported(void);
void video_close(void);
