#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <sys/param.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/poll.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <linux/input.h>
#include <netinet/in.h>
#include <pthread.h>
#include <time.h>

#include "fb.h"
#include "config.h"

#define FRAME_TIME_NS (1000000000L / TARGET_FPS)
#define FRAME_TIME_MS (1000 / TARGET_FPS)

#define MSG_DELIM ":"

enum display_state {
    DISPLAY_IDLE,
    DISPLAY_TIME,
    DISPLAY_SPM
};

static uint8_t display_state = DISPLAY_IDLE;

static pthread_mutex_t ring_event_mutex = PTHREAD_MUTEX_INITIALIZER;
static uint ring_event_count = 0;

static pthread_mutex_t beatmap_timer_mutex = PTHREAD_MUTEX_INITIALIZER;
static struct {
    double accumulated;
    struct timespec last_start;
    bool running;
} beatmap_timer = { 0 };

struct input_thread_args {
    int btn_fd;
    int ring_fd;
    int sockfd;
    struct sockaddr_in *cliaddr;
    socklen_t *cli_len;
};

static inline void timespec_add_ns(struct timespec *ts, long ns) {
    ts->tv_nsec += ns;
    while (ts->tv_nsec >= 1000000000L) {
        ts->tv_sec += 1;
        ts->tv_nsec -= 1000000000L;
    }
}

static inline bool timespec_less(const struct timespec *a, const struct timespec *b) {
    if (a->tv_sec < b->tv_sec) return true;
    if (a->tv_sec > b->tv_sec) return false;
    return a->tv_nsec < b->tv_nsec;
}

static inline double timespec_diff(const struct timespec *a, const struct timespec *b) {
    return (a->tv_sec - b->tv_sec) + (a->tv_nsec - b->tv_nsec) / 1e9;
}

static void draw_timer(int time, bool enabled, uint8_t y) {
    if (enabled) {
        bool negative = time < 0;
        time = abs(time);

        uint8_t minutes = time / 60;
        uint8_t seconds = time % 60;

        if (negative) {
            draw_char(&screen, &digits_font, '-', 0, y);
        } else if (minutes >= 10) {  // else if just in case we have -10 min offset... don't handle this
            draw_char(&screen, &digits_font, '0' + minutes / 10, 0, y);
        }
        draw_char(&screen, &digits_font, '0' + minutes % 10, digits_font.width, y);
        draw_char(&screen, &digits_font, '0' + seconds / 10, digits_font.width * 2 + 2, y);
        draw_char(&screen, &digits_font, '0' + seconds % 10, digits_font.width * 3 + 2, y);
    } else {
        draw_string(&screen, &digits_font, "--", 0, y);
        draw_string(&screen, &digits_font, "--", digits_font.width * 2 + 2, y);
    }

    if (time % 2 == 0 || !enabled) {
        set_pixel(&screen, digits_font.width * 2, y + 2, 255);
        set_pixel(&screen, digits_font.width * 2, y + 6, 255);
    }
}

static void draw_frame(uint8_t digit_x, uint8_t digit_y, uint8_t spm_x, struct timespec *next_frame) {
    char str[4] = { 0 };
    uint spm, spm_capped;
    double elapsed;

    switch (display_state) {
        case DISPLAY_IDLE:
            draw_timer(0, false, digit_y);
            break;

        case DISPLAY_TIME:
            pthread_mutex_lock(&beatmap_timer_mutex);
            elapsed = beatmap_timer.accumulated;
            if (beatmap_timer.running)
                elapsed += timespec_diff(next_frame, &beatmap_timer.last_start);
            pthread_mutex_unlock(&beatmap_timer_mutex);
            draw_timer(elapsed, true, digit_y);
            break;

        case DISPLAY_SPM:
            pthread_mutex_lock(&ring_event_mutex);
            spm = ((double)ring_event_count / FULL_SPIN_TICKS / (double)FRAME_TIME_MS) * 1000 * 60;
            spm_capped = MIN(477, spm);
            ring_event_count = 0;
            pthread_mutex_unlock(&ring_event_mutex);
            sprintf(str, "%03d", spm_capped);
            draw_string(&screen, &digits_font, str, spm_x, digit_y);
            break;
    }
}

static void *display_thread(void *data) {
    draw_string(&screen, &spleen_font, "osu", 0, 0);
    fb_refresh(&screen);

    sleep(2);

    struct timespec cur_time, next_frame;
    clock_gettime(CLOCK_MONOTONIC, &next_frame);

    uint8_t prev_state = display_state;

    uint8_t digit_x = (25 - digits_font.width * 3 - 2) / 2;
    uint8_t digit_y = (16 - digits_font.height) / 2;
    uint8_t spm_x = (25 - digits_font.width * 3) / 2;

    while (1) {
        if (display_state != prev_state) {
            fb_fade_out(&screen);
            draw_frame(digit_x, digit_y, spm_x, &next_frame);
            fb_fade_in(&screen);
        } else {
            fb_fill(&screen, 0);
            draw_frame(digit_x, digit_y, spm_x, &next_frame);
            fb_refresh(&screen);
        }

        prev_state = display_state;

        timespec_add_ns(&next_frame, FRAME_TIME_NS);
        clock_gettime(CLOCK_MONOTONIC, &cur_time);
        if (timespec_less(&next_frame, &cur_time)) {
            // Drop frame
            next_frame = cur_time;
            timespec_add_ns(&next_frame, FRAME_TIME_NS);
        }
        clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next_frame, NULL);
    }
}

static void *input_read_thread(void *data) {
    struct input_thread_args *args = data;
    struct input_event event;
    char buf[32];
    uint16_t prev_code = 0;
    struct pollfd fds[] = {
        { .fd = args->ring_fd, .events = POLLIN },
        { .fd = args->btn_fd, .events = POLLIN }
    };

    while (poll(fds, 2, -1) > 0) {
        if (fds[0].revents & POLLIN) {
            if (read(args->ring_fd, &event, sizeof(event)) > 0) {
                printf("Ring event! [%ld] type: %d code: %d val: %d\n",
                        event.time.tv_sec, event.type, event.code, event.value);

                if (event.type == EV_KEY && event.value == 0) {
                    pthread_mutex_lock(&ring_event_mutex);
                    if (prev_code != event.code)
                        ring_event_count = 0;
                    ring_event_count++;
                    pthread_mutex_unlock(&ring_event_mutex);
                    prev_code = event.code;
                    sprintf(buf, "ring:%s", event.code == KEY_PAGEUP ? "cw" : "ccw");
                    if (sendto(args->sockfd, buf, strlen(buf), 0,
                                (const struct sockaddr *)args->cliaddr, *args->cli_len) < 0) {
                        perror("sendto failed");
                    }
                }
            } else {
                perror("Ring read failed");
            }
        }

        if (fds[1].revents & POLLIN) {
            if (read(args->btn_fd, &event, sizeof(event)) > 0) {
                printf("Button event! [%ld] type: %d code: %d val: %d\n",
                        event.time.tv_sec, event.type, event.code, event.value);

                if (event.code == KEY_VOICECOMMAND) {
                    sprintf(buf, "btn1:%s", event.value ? "dn" : "up");
                } else if (event.code == KEY_MICMUTE) {
                    strcpy(buf, "btn2");
                }
                if (sendto(args->sockfd, buf, strlen(buf), 0,
                            (const struct sockaddr *)args->cliaddr, *args->cli_len) < 0) {
                    perror("sendto failed");
                }
            } else {
                perror("Button read failed");
            }
        }
    }
    perror("poll failed");
}

int main() {
    int ret = fb_init_all();
    if (ret)
        return ret;

    pthread_t d_thread;
    pthread_create(&d_thread, NULL, display_thread, NULL);

    struct sockaddr_in servaddr;
    int sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (sockfd < 0) {
        perror("Failed to create a socket");
        fb_close_all();
        return errno;
    }

    memset(&servaddr, 0, sizeof(servaddr));
    servaddr.sin_addr.s_addr = INADDR_ANY;
    servaddr.sin_port = htons(PORT);
    servaddr.sin_family = AF_INET;
    if (bind(sockfd, (struct sockaddr*)&servaddr, sizeof(servaddr))) {
        perror("Failed to bind socket");
        fb_close_all();
        close(sockfd);
        return errno;
    }

    printf("Listening on port %d...\n", PORT);

    int btn_fd = open(BTN_EVENT_PATH, O_RDONLY);
    int ring_fd = open(RING_EVENT_PATH, O_RDONLY);
    if (btn_fd < 0 || ring_fd < 0) {
        perror("Failed to open event device");
        fb_close_all();
        close(sockfd);
        return errno;
    }

    socklen_t *cli_len = malloc(sizeof(socklen_t));
    *cli_len = sizeof(struct sockaddr_in);
    struct sockaddr_in *cliaddr = malloc(*cli_len);
    memset(cliaddr, 0, *cli_len);

    pthread_t input_thread;
    struct input_thread_args *input_args = malloc(sizeof(struct input_thread_args));
    input_args->btn_fd = btn_fd;
    input_args->ring_fd = ring_fd;
    input_args->sockfd = sockfd;
    input_args->cliaddr = cliaddr;
    input_args->cli_len = cli_len;
    pthread_create(&input_thread, NULL, input_read_thread, input_args);

    char buf[BUF_SIZE];
    char *strtok_save;
    while (1) {
        ssize_t n = recvfrom(sockfd, &buf, BUF_SIZE - 1, 0,
            (struct sockaddr *)cliaddr, cli_len);
        if (n < 0) {
            perror("recvfrom failed");
            ret = errno;
            break;
        }
        buf[n] = 0;
        printf("msg: %s\n", buf);
        char *token = strtok_r(buf, MSG_DELIM, &strtok_save);
        if (strcmp(token, "ping") == 0) {
            strcpy(buf, "pong");
            if (sendto(sockfd, buf, strlen(buf), 0,
                        (const struct sockaddr *)cliaddr, *cli_len) < 0) {
                perror("sendto failed");
                ret = errno;
                break;
            }
        } else if (strcmp(token, "display") == 0) {
            token = strtok_r(NULL, MSG_DELIM, &strtok_save);
            if (token == NULL)
                continue;

            if (strcmp(token, "time") == 0) {
                printf("Settings screen to TIME mode!\n");
                display_state = DISPLAY_TIME;
            } else if (strcmp(token, "spm") == 0) {
                printf("Settings screen to SPM mode!\n");
                display_state = DISPLAY_SPM;
            }
        } else if (strcmp(token, "action") == 0) {
            token = strtok_r(NULL, MSG_DELIM, &strtok_save);
            if (token == NULL)
                continue;

            pthread_mutex_lock(&beatmap_timer_mutex);
            if (strcmp(token, "start") == 0) {
                token = strtok_r(NULL, MSG_DELIM, &strtok_save);
                if (token == NULL) {
                    pthread_mutex_unlock(&beatmap_timer_mutex);
                    continue;
                }
                double offset = atof(token);
                printf("Beatmap started, offset: %f\n", offset);
                beatmap_timer.accumulated = offset;
                clock_gettime(CLOCK_MONOTONIC, &beatmap_timer.last_start);
                beatmap_timer.running = true;
                display_state = DISPLAY_TIME;
            } else if (strcmp(token, "stop") == 0) {
                printf("Beatmap stopped\n");
                beatmap_timer.running = false;
                display_state = DISPLAY_IDLE;
            } else if (strcmp(token, "pause") == 0) {
                printf("Beatmap paused\n");
                struct timespec now;
                clock_gettime(CLOCK_MONOTONIC, &now);
                beatmap_timer.accumulated += timespec_diff(&now, &beatmap_timer.last_start);
                beatmap_timer.running = false;
            } else if (strcmp(token, "resume") == 0) {
                printf("Beatmap resumed\n");
                clock_gettime(CLOCK_MONOTONIC, &beatmap_timer.last_start);
                beatmap_timer.running = true;
            }
            pthread_mutex_unlock(&beatmap_timer_mutex);
        }
    }

    fb_close_all();
    close(btn_fd);
    close(ring_fd);
    close(sockfd);
    return ret;
}
