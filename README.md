# Timers

Countdown, stopwatch-with-laps, and repeat interval timers for **Pebble Time 2**,
with multiple live timers, saved presets, and “Say a timer” voice entry.

**Free · Pebble Time 2 (`emery`) · current PebbleOS · not compatible with Round 2 or older Pebbles**

**[Get Timers, see screenshots, and install it from the rePebble store](https://apps.repebble.com/5673ae9a96914ac297bfec85)**

![Timers on Pebble Time 2: home with multiple live timers, a countdown, a stopwatch with laps, a repeat timer, and voice entry](assets/banner.png)

## A note from the human (this note, like the app, was written by an AI)

Depending on a project's characteristics I decide how involved to be, from carefully reviewing every
line to a single-request vibe-code, and everywhere in between.

For Timers I was near the hands-off end: I set the product, design, and technical direction but did
not review the code. It was built by AI agents, Claude and Codex, working to that direction and
reviewing each other's work.

## What it does

- Runs several timers at once, with a home screen that keeps their live times visible.
- Creates countdowns, stopwatch sessions with laps, and repeat intervals.
- Saves presets for timers you use often.
- Lets you say a timer, review what the watch heard, and confirm or edit it before it starts.
- Keeps recent laps on the stopwatch screen and completed rounds on the repeat screen.
- Keeps timer deadlines while the app is closed and re-alerts, with a snooze option, when one
  finishes.

Timers is for Pebble Time 2 owners who want countdowns, stopwatch sessions, and intervals in one
place, whether that is for cooking, workout rests, focus blocks, or other everyday timing. It is
designed for short, glanceable watch interactions: the current time stays prominent, relevant
button actions remain visible, holds show progress before committing, and the layout adapts from a
single large timer to a compact multi-timer list.

## Build and develop

Timers is a native C app built with Pebble SDK 4.17. Clone the repository with its submodule, then
use the setup script to install and verify the local toolchain:

```sh
git clone --recurse-submodules https://github.com/Persitz/pebble-timers.git
cd pebble-timers
./app/scripts/setup.sh
```

Build the watch app:

```sh
cd app
pebble build
```

The resulting bundle is written to `app/build/app.pbw`. From `app/`, the main local checks are:

```sh
npm run test:c
npm run size:c
../harness/bin/emu-verify.sh --build --out emulator-runs/screenshot.png
```

The host tests exercise the app's platform-independent C logic. The public CI covers those tests,
the watch build, and the binary-size gate; the emulator command above is the documented manual
smoke test. `harness/` is a git submodule used for emulator testing.

Built and tested with [pebble-testpilot](https://github.com/Persitz/pebble-testpilot).

## Source, contributions, and license

Timers is open source under the [Apache License 2.0](LICENSE). Third-party notices are in
[NOTICE](NOTICE), with the JetBrains Mono license alongside the font at
[`app/resources/fonts/OFL.txt`](app/resources/fonts/OFL.txt).

Issues and patches are welcome. See [CONTRIBUTING.md](CONTRIBUTING.md) for how public contributions
are handled.
