# PPSSPP-spruce



CI-built PPSSPP binaries for [SpruceOS](https://github.com/spruceUI/spruceOS), targeting per-device vendor toolchains for maximum performance on handheld devices.

## Builds

| Binary | Devices | SoC / GPU | Toolchain |
|--------|---------|-----------|-----------|
| `PPSSPPSDL.64` | Universal ARM64 | aarch64 | Ubuntu Focal GCC 9.4 | 
| `PPSSPPSDL_TrimUI` | Brick, SmartPro (TSP), MagicX Mini Zero 28 | A133 / A133P / PowerVR GE8300 | Ubuntu Focal GCC 9.4 + SmartPro SDK |
| `PPSSPPSDL_SmartProS` | SmartPro S (TSPS) | A523 / Mali G57 | Buildroot GCC 10.3 |
| `PPSSPPSDL_Flip` | Flip | RK3566 / Mali G52 | Steward-Fu GCC 13.3 |
| `PPSSPPSDL_h700` | Anbernic RG XX (BaseOS) | Allwinner H700 / Mali G31 | Ubuntu Focal GCC 9.4 |
| `PPSSPPSDL_Pixel2` | GKD Pixel 2 | RK3326 / Mali G31 | Ubuntu Noble Clang 18 |
| `PPSSPPSDL_A30` | Miyoo A30 | A33 / Mali 400 | Steward-Fu GCC 13.2 |

All builds are triggered via **GitHub Actions > workflow_dispatch**. Binaries are uploaded to the [`latest` release](https://github.com/spruceUI/PPSSPP-spruce/releases/tag/latest).

## Patches

Patches in `patches/common/` are applied to all builds. Device-specific patches go in `patches/<device>/`.

### Common patches

| Patch | What it does |
|-------|-------------|
| `fullscreen.py` | `SDL_WINDOW_FULLSCREEN_DESKTOP` → `SDL_WINDOW_FULLSCREEN` (required for DRM/KMS) |
| `hide-cursor.py` | Unconditionally hide mouse cursor |
| `no-mute-secondary.py` | Remove auto-mute when PPSSPP_ID > 1 |

### PowerVR build: display rotation and in-game menu (MagicX Mini Zero 28)

The MagicX Mini Zero 28 has the Brick/TSP's A133P and GE8300, and its MOSS firmware ships the Smart Pro's SDL2, so it runs `PPSSPPSDL_TrimUI`. Its 640x480 panel is mounted portrait, so the PVR build also carries the following. None of it does anything unless a launcher sets `DISPLAY_ROTATION` or `EMU_OVERLAY_JSON`, so the Brick and TSP behave as before.

| Patch | What it does |
|-------|-------------|
| `a30/display-rotation.py` | Also applied to the PVR build: rotates PPSSPP's output by `DISPLAY_ROTATION` |
| `zero28/rotated-backbuffer-rects.py` | Maps backbuffer viewport/scissor rects correctly for every rotation. `display-rotation.py` swaps x/y, which only matches `DISPLAY_ROTATION=270`; at 90 it clipped the menus to the middle of the screen |
| `zero28/emu-overlay.py` | Builds in a MinUI/NextUI-style in-game menu (`overlay/`): Continue, Save/Load State with screenshots, Options, the PPSSPP menu, Quit |

`overlay/` is the emulator overlay and PPSSPP integration from [mohammadsyuhada/nx-redux](https://github.com/mohammadsyuhada/nx-redux) v1.1.1 (GPL-3.0), with [cJSON](https://github.com/DaveGamble/cJSON) (MIT). The first commit vendors it unmodified. Our changes: the display rotation (`EMU_OVERLAY_ROTATE`), a configurable pad (`EMU_PAD`, in [josegonzalez/minui-n64-pak](https://github.com/josegonzalez/minui-n64-pak)'s format), a 2x layout for 640x480, and an optional entry for PPSSPP's own menu (`EMU_OVERLAY_HOST_MENU`). Built in, it makes the PVR binary GPL-3.0 as a whole; PPSSPP itself is GPL-2.0-or-later. [arniebradfo/minui-psp](https://github.com/arniebradfo/minui-psp/tree/magicx-zero28) (`magicx-zero28`) is the MinUI pak that sets it all up on the Zero 28.

This Zero 28 work was written by Claude (Anthropic's Claude Opus 5.5) in Claude Code, and tested on a Zero 28 by [arniebradfo](https://github.com/arniebradfo). It builds on the Miyoo A30 rotation patch already here, on nx-redux's overlay, and on [ben16w/minui-psp](https://github.com/ben16w/minui-psp).

### Assets path

Assets (fonts, UI images, flash0 firmware files) are found **relative to the binary** at `<binary_dir>/assets/`. This is not patched — it uses PPSSPP's built-in exe-relative discovery via `/proc/self/exe`. No changes needed as long as the `assets/` folder sits next to the binary.

## Vendor SDK tarballs

The PVR and TSPS builds require proprietary vendor SDKs stored in the [`sdk-toolchains` release](https://github.com/spruceUI/PPSSPP-spruce/releases/tag/sdk-toolchains). Flip and A30 toolchains are public and downloaded automatically during Docker build.

## PPSSPP version

All builds currently target **v1.20.3**. To change, pass `ppsspp_version` when triggering the workflow (e.g., `v1.21.0`).
