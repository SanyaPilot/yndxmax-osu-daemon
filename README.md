# yndxmax-osu-daemon

A small native daemon, which allows you to connect a rooted Yandex Station Max to the osu! game client.

## Why??

Because it's fun lol.

## Features

- Sends ring rotation and button (*not yet actually*) events to the game client
- Displays a beatmap timer and spins-per-minute on the built-in LED screen

All this works via raw UDP socket with an extremely simple "protocol".

## Building and usage example

You'll need an Android NDK installed for a cross-compilation and platform tools for an ADB usage.

```shell
export NDK=/path/to/the/NDK
make

# Connect station via USB or network, e.g.:
adb connect Yandex-Station-Max

adb push yndxmax_osu_daemon /data/local/tmp/
adb shell /data/local/tmp/yndxmax_osu_daemon
```

If daemon launched successfully, go grab:

- the [modded osu! client](https://github.com/SanyaPilot/osu-yandexstation), as well as
- the [modded osu-framework](https://github.com/SanyaPilot/osu-framework-yandexstation) to build it

Set your device IP address (if needed) in the game input devices settings, and all set!

Now you can *spin the spinners...* with a *spinner...* Have fun!

## Other used projects

- [spleen](https://github.com/fcambus/spleen) font
- [bdfe](https://github.com/achilikin/bdfe) - to convert .bdf font to the C header

#### LLM usage

LLMs were used for faster osu! codebase understanding and distinct questions.

All code was written and verified manually, using all this knowledge. It's not perfect, but I was enjoying the process.

