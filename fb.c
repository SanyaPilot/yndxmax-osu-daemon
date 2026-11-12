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

struct fb_info {
    int fd;
    uint8_t px_bytes;
    char *buf;
    size_t buf_len;
};

static struct fb_fix_screeninfo finfo;
static struct fb_var_screeninfo vinfo;
static struct fb_info fb_info = { 0 };

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

#define px_offset(x, y) (fb_info.buf + (y * finfo.line_length + x * fb_info.px_bytes))

void set_pixel(uint8_t x, uint8_t y, uint8_t val) {
    memset(px_offset(x, y), val, fb_info.px_bytes);
}

void fb_fill(uint8_t val) {
    memset(fb_info.buf, val, fb_info.buf_len);
}

void fb_refresh() {
    vinfo.activate = FB_ACTIVATE_NOW | FB_ACTIVATE_FORCE;
    ioctl(fb_info.fd, FBIOPAN_DISPLAY, &vinfo);
}

void draw_line_h(uint8_t x1, uint8_t x2, uint8_t y, uint8_t val) {
    memset(px_offset(x1, y), val, (x2 - x1 + 1) * fb_info.px_bytes);
}

void draw_line_v(uint8_t x, uint8_t y1, uint8_t y2, uint8_t val) {
    for (uint8_t i = y1; i <= y2; i++)
        set_pixel(x, i, val);
}

void draw_char(const struct bitmap_font *font, char sym, uint8_t x, uint8_t y) {
    if (sym < font->startchar)
        return;

    size_t start_pos = (sym - font->startchar) * font->height;
    for (uint8_t i = 0; i < font->height && y + i < vinfo.yres; i++) {
        uint8_t font_row = font->data[start_pos + i];
        for (uint8_t j = 0; j < font->width && x + j < vinfo.xres; j++) {
            set_pixel(x + j, y + i, font_row & (1 << (7 - j)) ? 255 : 0);
        }
    }
}

void draw_string(const struct bitmap_font *font, char *str, uint8_t x, uint8_t y) {
    for (uint8_t i = 0; i < strlen(str) && x < vinfo.xres; i++) {
        draw_char(font, str[i], x, y);
        x += font->width;
    }
}

static const uint8_t fade_step = 255 / (FADE_ANIM_DURATION / FADE_ANIM_DELAY);
static const struct timespec fade_sleep_ts = { .tv_nsec = FADE_ANIM_DELAY * 1000000};

void fb_fade_out() {
    uint8_t val = 255;
    do {
        val = val >= fade_step ? (val - fade_step) : 0;
        for (size_t i = 0; i < fb_info.buf_len; i++) {
            fb_info.buf[i] = fb_info.buf[i] > 0 ? val : 0;
        }
        fb_refresh();
        nanosleep(&fade_sleep_ts, NULL);
    } while (val > 0);
}

void fb_fade_in() {
    uint8_t val = 0;
    do {
        val = (255 - val) >= fade_step ? (val + fade_step) : 255;
        for (size_t i = 0; i < fb_info.buf_len; i++) {
            fb_info.buf[i] = fb_info.buf[i] > 0 ? val : 0;
        }
        fb_refresh();
        nanosleep(&fade_sleep_ts, NULL);
    } while (val < 255);
}

int fb_init() {
    int fbfd = open(FRAMEBUFFER, O_RDWR);
    if (fbfd < 0) {
        perror("Failed to open framebuffer");
        return errno;
    }

    ioctl(fbfd, FBIOGET_FSCREENINFO, &finfo);
    ioctl(fbfd, FBIOGET_VSCREENINFO, &vinfo);

    int fb_width = vinfo.xres;
    int fb_height = vinfo.yres;
    int fb_bpp = vinfo.bits_per_pixel;

    printf("%d x %d @ %d; grayscale: %d; visual: %d\n", fb_width, fb_height, fb_bpp, vinfo.grayscale, finfo.visual);

    fb_info.fd = fbfd;
    fb_info.px_bytes = fb_bpp / 8;
    fb_info.buf_len = fb_height * finfo.line_length;
    fb_info.buf = mmap(0, fb_info.buf_len,
            PROT_READ | PROT_WRITE, MAP_SHARED, fbfd, (off_t)0);
    if (fb_info.buf == MAP_FAILED) {
        perror("Failed to mmap the framebuffer");
        close(fbfd);
        return errno;
    }
    memset(fb_info.buf, 0, fb_info.buf_len);

    return 0;
}

void fb_close() {
    if (fb_info.buf > 0)
        munmap(fb_info.buf, fb_info.buf_len);
    close(fb_info.fd);
}

