#include <errno.h>
#include <fcntl.h>
#include <linux/fb.h>
#include <stddef.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include "fb.h"
#include "config.h"

struct fb_info screen = { 0 };
struct fb_info ring = { 0 };

static const uint8_t spleen_8x16[] = {
    #include "spleen-8x16.h"
};

const struct bitmap_font spleen_font = {
    .width = 8,
    .height = 16,
    .startchar = 32,
    .data = spleen_8x16
};

static const uint8_t digits[] = {
    #include "digits-font.h"
};

const struct bitmap_font digits_font = {
    .width = 6,
    .height = 9,
    .startchar = 45,
    .data = digits
};

#define px_offset(fb, x, y) (fb->buf + (y * fb->finfo.line_length + x * fb->px_bytes))

void set_pixel(struct fb_info *fb, uint8_t x, uint8_t y, uint8_t val) {
    memset(px_offset(fb, x, y), val, fb->px_bytes);
}

void fb_fill(struct fb_info *fb, uint8_t val) {
    memset(fb->buf, val, fb->buf_len);
}

void fb_refresh(struct fb_info *fb) {
    fb->vinfo.activate = FB_ACTIVATE_NOW | FB_ACTIVATE_FORCE;
    ioctl(fb->fd, FBIOPAN_DISPLAY, &fb->vinfo);
}

void draw_line_h(struct fb_info *fb, uint8_t x1, uint8_t x2, uint8_t y, uint8_t val) {
    memset(px_offset(fb, x1, y), val, (x2 - x1 + 1) * fb->px_bytes);
}

void draw_line_v(struct fb_info *fb, uint8_t x, uint8_t y1, uint8_t y2, uint8_t val) {
    for (uint8_t i = y1; i <= y2; i++)
        set_pixel(fb, x, i, val);
}

void draw_char(struct fb_info *fb, const struct bitmap_font *font, char sym, uint8_t x, uint8_t y) {
    if (sym < font->startchar)
        return;

    size_t start_pos = (sym - font->startchar) * font->height;
    for (uint8_t i = 0; i < font->height && y + i < fb->vinfo.yres; i++) {
        uint8_t font_row = font->data[start_pos + i];
        for (uint8_t j = 0; j < font->width && x + j < fb->vinfo.xres; j++) {
            set_pixel(fb, x + j, y + i, font_row & (1 << (7 - j)) ? 255 : 0);
        }
    }
}

void draw_string(struct fb_info *fb, const struct bitmap_font *font, char *str, uint8_t x, uint8_t y) {
    for (uint8_t i = 0; i < strlen(str) && x < fb->vinfo.xres; i++) {
        draw_char(fb, font, str[i], x, y);
        x += font->width;
    }
}

static const uint8_t fade_step = 255 / (FADE_ANIM_DURATION / FADE_ANIM_DELAY);
static const struct timespec fade_sleep_ts = { .tv_nsec = FADE_ANIM_DELAY * 1000000};

static inline void fb_fade_step(struct fb_info *fb, uint8_t val) {
    for (size_t i = 0; i < fb->buf_len; i++) {
        fb->buf[i] = fb->buf[i] > 0 ? val : 0;
    }
    fb_refresh(fb);
}

void fb_fade_out(struct fb_info *fb) {
    uint8_t val = 255;
    do {
        val = val >= fade_step ? (val - fade_step) : 0;
        fb_fade_step(fb, val);
        nanosleep(&fade_sleep_ts, NULL);
    } while (val > 0);
}

void fb_fade_out_all() {
    uint8_t val = 255;
    do {
        val = val >= fade_step ? (val - fade_step) : 0;
        fb_fade_step(&screen, val);
        fb_fade_step(&ring, val);
        nanosleep(&fade_sleep_ts, NULL);
    } while (val > 0);
}

void fb_fade_in(struct fb_info *fb) {
    uint8_t val = 0;
    do {
        val = (255 - val) >= fade_step ? (val + fade_step) : 255;
        fb_fade_step(fb, val);
        nanosleep(&fade_sleep_ts, NULL);
    } while (val < 255);
}

static int fb_init(struct fb_info *fb, char *path) {
    int fbfd = open(path, O_RDWR);
    if (fbfd < 0) {
        perror("Failed to open framebuffer");
        return errno;
    }

    ioctl(fbfd, FBIOGET_FSCREENINFO, &fb->finfo);
    ioctl(fbfd, FBIOGET_VSCREENINFO, &fb->vinfo);

    fb->fd = fbfd;
    fb->px_bytes = fb->vinfo.bits_per_pixel / 8;
    fb->buf_len = fb->vinfo.yres * fb->finfo.line_length;
    fb->buf = mmap(0, fb->buf_len,
            PROT_READ | PROT_WRITE, MAP_SHARED, fbfd, (off_t)0);
    if (fb->buf == MAP_FAILED) {
        perror("Failed to mmap the framebuffer");
        close(fbfd);
        return errno;
    }
    memset(fb->buf, 0, fb->buf_len);

    return 0;
}

int fb_init_all() {
    int ret = fb_init(&screen, SCREEN_FRAMEBUFFER);
    if (ret)
        return ret;

    ret = fb_init(&ring, RING_FRAMEBUFFER);
    return ret;
}

static void fb_close(struct fb_info *fb) {
    if (fb->buf > 0)
        munmap(fb->buf, fb->buf_len);
    close(fb->fd);
}

void fb_close_all() {
    fb_close(&screen);
    fb_close(&ring);
}

