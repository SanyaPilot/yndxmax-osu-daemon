#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <sys/param.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <linux/input.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <pthread.h>
#include <time.h>

#include "fb.h"
#include "config.h"

#define FRAME_TIME_NS (1000000000L / TARGET_FPS)
#define FRAME_TIME_MS (1000 / TARGET_FPS)

enum display_state {
    DISPLAY_IDLE,
    DISPLAY_TIME,
    DISPLAY_SPM
};

static struct {
    time_t accumulated;
    struct timespec last_start;
    bool running;
} beatmap_timer = { 0 };

static uint8_t display_state = DISPLAY_IDLE;
static pthread_mutex_t ring_event_mutex = PTHREAD_MUTEX_INITIALIZER;
static uint ring_event_count = 0;

struct input_thread_args {
    int fd;
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

static void draw_timer(time_t time, uint8_t y) {
    if (time > 0) {
        uint8_t minutes = time / 60;
        uint8_t seconds = time % 60;

        if (minutes >= 10) {
            draw_char(&digits_font, '0' + minutes / 10, 0, y);
        }
        draw_char(&digits_font, '0' + minutes % 10, digits_font.width, y);
        draw_char(&digits_font, '0' + seconds / 10, digits_font.width * 2 + 2, y);
        draw_char(&digits_font, '0' + seconds % 10, digits_font.width * 3 + 2, y);
    } else {
        draw_string(&digits_font, "--", 0, y);
        draw_string(&digits_font, "--", digits_font.width * 2 + 2, y);
    }

    if (time % 2 == 0 || time < 0) {
        set_pixel(digits_font.width * 2, y + 2, 255);
        set_pixel(digits_font.width * 2, y + 6, 255);
    }
}

void *display_thread(void *data) {
    draw_string(&spleen_font, "osu", 0, 0);
    fb_refresh();

    sleep(2);

    struct timespec cur_time, next_frame;
    clock_gettime(CLOCK_MONOTONIC, &next_frame);

    char str[4] = { 0 };

    // Time variables
    uint8_t digit_x = (25 - digits_font.width * 3 - 2) / 2;
    uint8_t digit_y = (16 - digits_font.height) / 2;
    uint8_t div_y = digit_y + 2;
    uint8_t minutes, seconds;
    time_t elapsed;

    // SPM variables
    uint8_t spm_x = (25 - digits_font.width * 3) / 2;
    uint spm, spm_capped;
    while (1) {
        fb_fill(0);
        switch (display_state) {
            case DISPLAY_IDLE:
                draw_timer(-1, digit_y);
                break;

            case DISPLAY_TIME:
                elapsed = beatmap_timer.accumulated;
                if (beatmap_timer.running)
                    elapsed += next_frame.tv_sec - beatmap_timer.last_start.tv_sec;
                draw_timer(elapsed, digit_y);
                break;

            case DISPLAY_SPM:
                pthread_mutex_lock(&ring_event_mutex);
                spm = ((double)ring_event_count / FULL_SPIN_TICKS / (double)FRAME_TIME_MS) * 1000 * 60;
                spm_capped = MIN(477, spm);
                ring_event_count = 0;
                pthread_mutex_unlock(&ring_event_mutex);
                sprintf(str, "%03d", spm_capped);
                draw_string(&digits_font, str, spm_x, digit_y);
                break;
        }
        fb_refresh();

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

void *input_read_thread(void *data) {
    struct input_thread_args *args = data;
    struct input_event event;
    char buf[32];
    uint16_t prev_code = 0;

    while (read(args->fd, &event, sizeof(event)) > 0) {
        printf("Read event! [%ld] type: %d code: %d val: %d\n",
                event.time.tv_sec, event.type, event.code, event.value);
        if (event.type == EV_KEY && event.value == 0) {
            pthread_mutex_lock(&ring_event_mutex);
            if (prev_code != event.code)
                ring_event_count = 0;
            ring_event_count++;
            pthread_mutex_unlock(&ring_event_mutex);
            prev_code = event.code;
            sprintf(buf, "evt:ring:%d", event.code);
            if (sendto(args->sockfd, buf, strlen(buf), 0,
                        (const struct sockaddr *)args->cliaddr, *args->cli_len) < 0) {
                perror("sendto failed!");
                break;
            }
        }
    }
}

int main() {
    int err;
    if ((err = fb_init()) < 0)
        return err;

    pthread_t d_thread;
    pthread_create(&d_thread, NULL, display_thread, NULL);

    struct sockaddr_in servaddr;
    int sockfd;
    if ((sockfd = socket(AF_INET, SOCK_DGRAM, 0)) < 0) {
        perror("Failed to create a socket");
        fb_close();
        return errno;
    }

    memset(&servaddr, 0, sizeof(servaddr));
    servaddr.sin_addr.s_addr = INADDR_ANY;
    servaddr.sin_port = htons(PORT);
    servaddr.sin_family = AF_INET;
    if (bind(sockfd, (struct sockaddr*)&servaddr, sizeof(servaddr))) {
        perror("Failed to bind socket");
        fb_close();
        close(sockfd);
        return errno;
    }

    printf("Listening on port %d...\n", PORT);

    int fd = open(EVENT_PATH, O_RDONLY);
    if (fd < 0) {
        perror("Failed to open event device");
        fb_close();
        close(sockfd);
        return errno;
    }

    socklen_t *cli_len = malloc(sizeof(socklen_t));
    *cli_len = sizeof(struct sockaddr_in);
    struct sockaddr_in *cliaddr = malloc(*cli_len);
    memset(cliaddr, 0, *cli_len);

    pthread_t input_thread;
    struct input_thread_args *input_args = malloc(sizeof(struct input_thread_args));
    input_args->fd = fd;
    input_args->sockfd = sockfd;
    input_args->cliaddr = cliaddr;
    input_args->cli_len = cli_len;
    pthread_create(&input_thread, NULL, input_read_thread, input_args);

    char buf[BUF_SIZE];
    while (1) {
        ssize_t n = recvfrom(sockfd, &buf, BUF_SIZE - 1, 0,
            (struct sockaddr *)cliaddr, cli_len);
        if (n < 0) {
            perror("recvfrom failed");
            fb_close();
            close(fd);
            close(sockfd);
            return errno;
        }
        buf[n] = 0;
        printf("msg: %s\n", buf);
        if (strcmp(buf, "display:time") == 0) {
            printf("Settings screen to TIME mode!\n");
            display_state = DISPLAY_TIME;
        } else if (strcmp(buf, "display:spm") == 0) {
            printf("Settings screen to SPM mode!\n");
            display_state = DISPLAY_SPM;
        } else if (strcmp(buf, "action:start") == 0) {
            printf("Beatmap started\n");
            beatmap_timer.accumulated = 0;
            clock_gettime(CLOCK_MONOTONIC, &beatmap_timer.last_start);
            beatmap_timer.running = true;
            display_state = DISPLAY_TIME;
        } else if (strcmp(buf, "action:stop") == 0) {
            printf("Beatmap stopped\n");
            beatmap_timer.running = false;
            display_state = DISPLAY_IDLE;
        } else if (strcmp(buf, "action:pause") == 0) {
            printf("Beatmap paused\n");
            struct timespec now;
            clock_gettime(CLOCK_MONOTONIC, &now);
            beatmap_timer.accumulated += now.tv_sec - beatmap_timer.last_start.tv_sec;
            beatmap_timer.running = false;
        } else if (strcmp(buf, "action:resume") == 0) {
            printf("Beatmap resumed\n");
            clock_gettime(CLOCK_MONOTONIC, &beatmap_timer.last_start);
            beatmap_timer.running = true;
        }
    }

    close(fd);
    close(sockfd);
    printf("exit!\n");
    return 0;
}
