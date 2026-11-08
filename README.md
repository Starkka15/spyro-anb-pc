# The Legend of Spyro: A New Beginning — PC port

A native Windows version of the original Xbox game, made by static
recompilation with [xboxrecomp](https://github.com/sp00nznet/xboxrecomp): the
Xbox program is translated to C ahead of time and runs directly, not in an
emulator.

> **About this port.** This port isn't mine to gatekeep. It's a basic starting
> point to build on (features, fixes, polish), not a finished or maintained product. If
> you feel up to it, feel free to continue the work; forks and pull requests are welcome.

**This repository contains none of the game.** You need your own copy of The
Legend of Spyro: A New Beginning for the original Xbox (title VU-074,
version 1). Setup copies the game's files from a folder, a disc drive, or an
Xbox disc image (.iso, full or trimmed) and checks the version first.

> **Early release.** Only the first level has been played through. Expect bugs
> later in the game; reports are welcome in Issues (set `Log = 1` in
> `spyro.ini` and attach `spyro.log`, or `crash.txt` if it crashed).

## Playing

Download `SpyroANB-PC-<version>-Setup.exe` from
[Releases](../../releases), run it, and point it at your copy of the game.
Windows 10 or 11 (64-bit) with a DirectX 11 graphics card.

- Keyboard and mouse or any XInput controller; on-screen button prompts follow
  the device you used last. Keys can be changed in `mkb.ini`.
- The launcher sets fullscreen, window size, render scale, widescreen, VSync
  and skipping the logo movies.
- Controllers can be plugged in and out at any time; unplugging the one in use
  pauses the game.

See [installer/README.txt](installer/README.txt) for the full player notes.

## Building

Needs Visual Studio 2022 (C, x64 tools), CMake 3.20+, Python 3 with Capstone,
and your own copy of the game.

1. Clone the runtime branch beside this repository:
   `git clone -b spyro-anb-1.0.0 https://github.com/Starkka15/xboxrecomp ../xboxrecomp`
2. Put the game's files in `game_files/` (with `default.xbe`).
3. Generate the code from your XBE, from the xboxrecomp folder:

   ```
   py -3 -m tools.disasm   <port>/game_files/default.xbe --seed-functions <port>/tools/seeds.json --force
   py -3 -m tools.func_id  <port>/game_files/default.xbe
   py -3 -m tools.abi_analysis <port>/game_files/default.xbe
   py -3 -m tools.recomp   <port>/game_files/default.xbe --all --split 250 --gen-dir <port>/src/recomp/gen
   ```

4. `cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release` then
   `cmake --build build`.
5. Optional: `ISCC installer\spyro.iss` (Inno Setup 6) builds Setup.

The generated code is the game's code translated, so it is never committed:
`src/recomp/gen/` stays local.

## Credits and licenses

The Legend of Spyro: A New Beginning was developed by Krome Studios and
published by Sierra Entertainment; Spyro is a trademark of its owner. This
port is not affiliated with or endorsed by any of them.

The port's own code is GPL-3.0 (see LICENSE). The xboxrecomp runtime is MIT
and includes components from xemu under the LGPL 2.1; see
[installer/LICENSES.txt](installer/LICENSES.txt).
