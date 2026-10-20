# LittleBigPlanet 3 (PS4) on Linux

A native Linux port of *LittleBigPlanet 3* for PlayStation 4. It runs the game's original PS4
executable directly on a Linux PC, the way Wine plus DXVK runs a Windows game:

- the game's x86-64 code runs on your CPU as it is, with no emulation;
- a small runtime stands in for the PS4 system libraries the game calls;
- the game's graphics are translated to Vulkan by a renderer derived from
  [shadPS4](https://github.com/shadps4-emu/shadPS4).

It is built on [bbport](https://github.com/deadinside28/bloodborne_pc), the native Bloodborne port
by deadinside28, and keeps its loader, runtime and renderer.

**No game files are included, and none are linked here.** You need your own copy of the game,
dumped from your own PS4. This project is not affiliated with Sony Interactive Entertainment, Media
Molecule or Sumo Digital.

**Status: early work in progress, not playable yet.** The game boots, shows the brightness
calibration screen, reports that its online servers are unreachable, and goes on offline to the
Pod, the Pod Computer and the Prologue map. The Introduction plays to its end, lit and colored, at
the game's 30 FPS. The Legend of Bunkum plays its story to the end, and the game goes on by
itself to Needlepoint Peaks, the first level with gameplay, which can be walked through. Nothing
later has been reached yet. It has been tested on one desktop (NVIDIA RTX 4070 SUPER) and, for
correctness only, on an Intel iGPU.

## Requirements

- Linux on an x86-64 PC and a GPU with Vulkan 1.3. NVIDIA is the tested setup. AMD has not been
  tried.
- A dump of **LittleBigPlanet 3, European release CUSA00063, version 01.00**, with no update merged
  into it. The game folder holds `eboot.bin`, `sce_module/` and `sce_sys/`. Other releases have not
  been tried.

## Getting the game files

You need a jailbroken PS4 and your own copy of the game, on disc or bought digitally. Dump it with
a PS4 dumper of your choice, or extract its package file with a recent PKG extractor. You should
end up with a folder named `CUSA00063` that contains `eboot.bin`.

Do not ask for game files in this project's issues. They will be closed.

## Build and run

There is no AppImage or launcher for this game yet. You need GCC, CMake, Ninja, Python 3,
glslang, SDL3, the Vulkan headers and the libraries listed in `shell.nix`. With
[Nix](https://nixos.org) they all come from `shell.nix`.

```bash
git clone --recursive <this repository> lbp3-port && cd lbp3-port
bash build.sh
BB_GAME_DIR=/path/to/CUSA00063 bash run.sh
```

`run.sh` recognises the game by its title ID and applies what it needs. `BB_USER_NAME=Name` sets
the name the game shows for your profile.

## Known issues

- The first start after installing (or after an update of the port clears the shader cache) can
  stop at the first level load with a GPU timeout from the game itself: a few hundred shaders
  compile at once and the game's watchdog gives up. Start it again; the second start uses the
  cache and gets through.
- Only the Prologue up to the start of Needlepoint Peaks has been played, with a scripted
  controller. Later levels, Create mode and saving progress have not been tested.
- Online play does not work. The game's servers are closed; the port tells the game they are
  unreachable and it goes on offline.
- Many system functions the game imports are not implemented yet and return 0 (`BB_STUB_IMPORTS`).
  Most are online, dialog and content search functions.
- Sound has not been checked in detail.
- The in-game overlay (Insert) and the bundled launcher use bbport's Bloodborne layout. Most of
  their options do nothing in this game.

## Environment variables

You don't need to set these. The port sets them for this game, and each one can be overridden.

| Variable | Default for this game | What it does |
|---|---|---|
| `BB_GAME_DIR` | | The game folder |
| `BB_FPS` | `30` | The game's frame rate, as on PS4 |
| `BB_IMAGE_BASE` | `0x80000000` | Loads the executable below 4 GiB, where the game's job code expects it |
| `BB_STUB_IMPORTS` | `1` | Missing system functions return 0 instead of stopping the game |
| `BB_NP_SIGNED_IN` | `1` | Signed in to PSN with the game's servers unreachable |
| `BB_LOCAL_UDP` | `1` | Local network sockets for the game's networking code |
| `BB_SAVE_LOG` | | `1` writes the log to `logs/` in the data folder |
| `BB_SKIP_GAME_CHECK` | | `1` starts a game folder the check rejects |

## Credits and licenses

This port is licensed under the **GNU GPL v2 or later** ([LICENSE](LICENSE)).

- [bbport](https://github.com/deadinside28/bloodborne_pc) by deadinside28 and its contributors:
  the loader, runtime, renderer and packaging this port is built on. bbport's own documentation is
  in [docs/bloodborne](docs/bloodborne/README.md).
- [shadPS4](https://github.com/shadps4-emu/shadPS4): the video core and shader recompiler the
  renderer is derived from (GPL-2.0-or-later), with sirit and half.
- AMD FidelityFX Super Resolution 3.1 through [FSR-Vulkan](https://github.com/FireBurn/FSR-Vulkan)
  by FireBurn (MIT) and the AMD FidelityFX SDK (MIT).
- [LibAtrac9](https://github.com/Thealexbarney/LibAtrac9) (MIT) and
  [Dear ImGui](https://github.com/ocornut/imgui) (MIT).

LittleBigPlanet is a trademark of Sony Interactive Entertainment. AMD and FidelityFX are trademarks
of Advanced Micro Devices.
