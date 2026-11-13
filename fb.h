#pragma once

#include <stdint.h>
#include <sys/types.h>
#include <linux/fb.h>

struct fb_info {
    int fd;
    uint8_t px_bytes;
    char *buf;
    size_t buf_len;
    struct fb_fix_screeninfo finfo;
    struct fb_var_screeninfo vinfo;
};

struct bitmap_font {
    const uint8_t width;
    const uint8_t height;
    const uint8_t startchar;
    const uint8_t *data;
};

void set_pixel(struct fb_info *fb, uint8_t x, uint8_t y, uint8_t val);
void draw_line_h(struct fb_info *fb, uint8_t x1, uint8_t x2, uint8_t y, uint8_t val);
void draw_line_v(struct fb_info *fb, uint8_t x, uint8_t y1, uint8_t y2, uint8_t val);
void draw_char(struct fb_info *fb, const struct bitmap_font *font, char sym, uint8_t x, uint8_t y);
void draw_string(struct fb_info *fb, const struct bitmap_font *font, char *str, uint8_t x, uint8_t y);

int fb_init_all();
void fb_close_all();
void fb_refresh(struct fb_info *fb);
void fb_fill(struct fb_info *fb, uint8_t val);
void fb_fade_out(struct fb_info *fb);
void fb_fade_out_all();
void fb_fade_in(struct fb_info *fb);

extern const struct bitmap_font spleen_font;
extern const struct bitmap_font digits_font;
extern struct fb_info screen;
extern struct fb_info ring;

