# ESP32 Doom Clone

A Wolfenstein/Doom-style raycaster FPS that runs on the ESP32-2432S028R
"Cheap Yellow Display" (CYD) — a $10 ESP32 dev board with a built-in
320x240 ILI9341 LCD and a resistive touchscreen. Movement, turning, and
firing are all done via on-screen touch buttons; no extra hardware needed.

## Hardware

- **Board:** ESP32-2432S028R ("Cheap Yellow Display" / CYD)
- **Display:** ILI9341, 320x240, driven over VSPI via [TFT_eSPI](https://github.com/Bodmer/TFT_eSPI)
- **Touch:** XPT2046 resistive touch controller, bit-banged on its own bus
  (the CYD wires touch to different pins than the display, and a separate
  bus avoids contending with the display's SPI transactions)
- Pin assignments live in `platformio.ini` (`build_flags`) for the display
  and at the top of `src/main.cpp` for touch.

## Building and flashing

Requires [PlatformIO](https://platformio.org/) (`pio` on your `PATH`).

```
scripts/build.sh     # compile
scripts/upload.sh     # compile + flash over USB (default port /dev/ttyUSB0)
scripts/monitor.sh    # open the serial monitor
scripts/run.sh         # upload + monitor in one step
scripts/clean.sh       # remove build artifacts
```

`upload_port` / `monitor_port` are set to `/dev/ttyUSB0` in
`platformio.ini` — edit that if your board enumerates elsewhere.

## Playing

On first boot (and any time you hold the screen during the title flash)
the game runs a 3-point touch calibration and saves it to flash, so it
only needs to happen once per board.

Controls are on-screen touch zones, drawn over the game view:

- Left side: a D-pad (up/down to move, left/right to turn)
- Bottom right: a red **FIRE** button (auto-targets the nearest enemy
  within your view cone)

Goal: clear the enemies (`E`) on each of the 3 built-in levels. Health
regenerates ammo over time; walking into enemies costs health.

## Code structure

Everything lives in `src/main.cpp` (~770 lines), organized top to bottom
roughly as:

1. Touch driver (bit-banged XPT2046 reads)
2. Touch calibration (3-point, swap-aware, persisted via `Preferences`)
3. Map data (`LEVELS[]`, ASCII grids — `1`/`2`/`3` = wall types, `.` =
   floor, `P` = player start, `E` = enemy spawn) and level loading
4. Raycasting (`castRay`, `renderScene`) and billboard sprite rendering
   for enemies (`renderSprites`)
5. HUD, gun sprite, controls overlay, damage flash
6. Game logic: firing, enemy AI/damage, input handling, state machine
7. `setup()` / `loop()` — Arduino entry points

The frame buffer is rendered in horizontal bands (`gBandH`) rather than
one full-frame sprite, because the ESP32 (no PSRAM on this board) can't
reliably get one contiguous ~150KB allocation once the heap fragments;
`setup()` picks the largest band height that actually allocates.

## Development approach

This project was built with Claude Code, iterating directly against real
hardware rather than writing the whole thing up front from a spec:

1. **Scaffold first.** Get a minimal PlatformIO project building and
   flashing to the actual board before writing any game logic — display
   init, a solid-color fill, confirm the toolchain and wiring are right.
2. **One subsystem at a time.** Raycasting and rendering came first (so
   there was something to look at), then touch input and calibration,
   then enemies/combat, then HUD/polish — each step small enough to
   flash and verify on-device before moving on.
3. **Let hardware constraints drive design.** Decisions like the banded
   framebuffer (heap fragmentation) and the bit-banged touch bus
   (pin/SPI-contention issues) came from things that only showed up when
   running on real hardware, not from up-front planning — see the
   comments in `src/main.cpp` at each of those spots for the specific
   failure that motivated them.
4. **Fix forward from serial logs.** `Serial.printf` boot diagnostics
   (heap size, chosen band height, sprite pointer) were added early and
   used to debug allocation failures live via `scripts/monitor.sh`,
   rather than guessing at memory behavior.

If you're extending this yourself, the same loop works well: change one
thing, `scripts/run.sh`, watch it on the board, repeat.
