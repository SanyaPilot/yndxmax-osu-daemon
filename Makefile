TOOLCHAIN := $(NDK)/toolchains/llvm/prebuilt/linux-x86_64
TARGET=armv7a-linux-androideabi28

CC := $(TOOLCHAIN)/bin/clang

.PHONY: all

all: yndxmax_osu_daemon

yndxmax_osu_daemon: main.c fb.c config.h fb.h
	$(CC) --target=$(TARGET) main.c fb.c -o yndxmax_osu_daemon

