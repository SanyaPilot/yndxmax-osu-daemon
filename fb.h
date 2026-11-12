#pragma once

#include <stdint.h>

struct bitmap_font {
    const uint8_t width;
    const uint8_t height;
    const uint8_t startchar;
    const uint8_t *data;
};

void set_pixel(uint8_t x, uint8_t y, uint8_t val);
void draw_line_h(uint8_t x1, uint8_t x2, uint8_t y, uint8_t val);
void draw_line_v(uint8_t x, uint8_t y1, uint8_t y2, uint8_t val);
void draw_char(const struct bitmap_font *font, char sym, uint8_t x, uint8_t y);
void draw_string(const struct bitmap_font *font, char *str, uint8_t x, uint8_t y);

int fb_init();
void fb_close();
void fb_refresh();
void fb_fill(uint8_t val);
void fb_fade_out();
void fb_fade_in();

extern const struct bitmap_font spleen_font;
extern const struct bitmap_font digits_font;

