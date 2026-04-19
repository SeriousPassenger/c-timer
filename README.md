# c-timer
Minimal Linux-first CLI timer written in C.

## Build

```sh
make
```

## Usage

```sh
./ctimer HH:MM:SS [--repeat] [--sound] [--notify]
./ctimer help
./ctimer --help
```

Example commands:

```sh
./ctimer 00:30:00
./ctimer 00:05:00 --sound --notify
./ctimer 00:00:10 --repeat
```

## Features

- Strict `HH:MM:SS` parsing with non-zero durations only.
- Single-line countdown with a simple ASCII progress bar.
- Optional repeat mode: `Enter` restarts, `q` exits.
- Optional best-effort desktop notifications with `notify-send`. (<code>sudo apt install libnotify-bin</code> for Debian based systems.)
- Optional bundled alert sound played with `paplay`, falling back to `aplay`.

## Notes

- v1 is implemented and verified for Linux.
- `--repeat` requires an interactive stdin TTY.
- Sound and desktop notifications are optional. Missing tools do not stop the timer; the app prints a warning and exits normally.
