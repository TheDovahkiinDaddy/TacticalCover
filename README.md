# Tactical Cover

Gears of War style third-person cover for Starfield: a native SFSE plugin.

Walk into a wall and you take cover. Low cover, tall cover, aim over or around it, blind fire, reload and switch weapons without leaving cover, and swap shoulders (LT + A on a controller, X on the keyboard).

- Download and full description: Nexus Mods (link on the release page)
- Game: Starfield 1.16.244 (Steam) only. On any other version the plugin installs nothing and says so in its log.
- Requires: Starfield Script Extender (SFSE) and Address Library for SFSE Plugins.

## Building

Requirements: Windows, Visual Studio 2022 (MSVC, C++23) and [xmake](https://xmake.io).

```
git clone --recurse-submodules https://github.com/TheDovahkiinDaddy/TacticalCover.git
cd TacticalCover
xmake f -m releasedbg -y
xmake build TacticalCover
```

The release target writes `TacticalCover.dll`; the player's settings are `data/release/TacticalCover.ini`. `xmake build CoverSF_tests` builds the engine-free tests in `tests/`.

## Layout

- `src/Core`: engine-free logic (cover state machine, sensor, camera slot, input chord), covered by `tests/`.
- `src/Engine`: everything that touches the game. Every engine address and offset lives in `src/Engine/Offsets.h` with its source. The `*Script.cpp` files are the automated test rig used during development; the release build refuses to run them.

## Licence

GPL-3.0 (see `LICENSE`), with the modding exception in `EXCEPTIONS`. Built on [CommonLibSF](https://github.com/libxse/commonlibsf).

The code was written with an AI assistant (Claude), directed, tested and played by the author.
