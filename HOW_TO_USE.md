# Hamzex: how to use

Hamzex is a Linux-only Counter-Strike 2 shared library. It changes the game
client and may result in a game ban. Use it only where you are authorized to
modify and test the process.

## Requirements

- Linux
- CMake 4.4 or newer
- Ninja
- GCC 14+ or another compiler with C++26 module support
- X11 development headers and libraries
- `gdb`
- `/dev/uinput` write access for bhop input

The project no longer includes a kernel module, TracerPid masking, or any
anti-debugging path. Injection is transparent and uses the normal `gdb`
`dlopen` call.

## Install and build

From the project directory:

```sh
bash install.sh
```

This configures and builds `build/libHamzex.so`. It creates
`$HOME/.config/hamzex/` and copies the default skin config only when one does
not already exist.

## Load the library

Start CS2 first, then run:

```sh
sudo bash injekt.sh
```

To load another build, provide an absolute path:

```sh
sudo SO_PATH=/absolute/path/to/libHamzex.so bash injekt.sh
```

The script checks that CS2 can see the library through its process namespace
and verifies that the library appears in `/proc/<pid>/maps` after loading.

## GUI

After loading, press `HOME` to show or hide the compact in-game child overlay.
It attaches to the focused CS2 window and does not create a separate desktop
or taskbar window. The Movement page contains bunny hop, third-person, Gura,
and sniper-crosshair controls.
The Skins page has one `Skins enabled` switch that disables the skin and knife
changer together. It lets you select a weapon or `knife`, choose `both`, `ct`,
or `t`, edit paint kit/seed/wear, and choose the CT/T knife subclass with the
arrow buttons. Skin Manager overrides are stored in:

```text
$HOME/.config/hamzex/skins.json
```

They are overlaid on the original `config.json` skin table and picked up by the
live changer during the next frame-stage update.
The Unsafe page contains no-flash, no-smoke, no-skybox, fullbright,
opaque-only rendering, smoke-color, and player glow-chams controls. Glow chams
use cached player entities to reduce flicker and restore their original values
when disabled. `Weapon glow` applies the same full-model glow to weapon entities
and viewmodels.

Feature settings are saved in:

```text
$HOME/.config/hamzex/features.json
```

Enable bhop in the GUI, then press `Mouse4` to toggle the bhop state on or off.
The toggle uses CS2's own input state, so holding or releasing the button does
not cause repeated toggles. A/D remain normal manual air-strafe input. Enable
third-person to activate it immediately, or press `V` to toggle it while the
option is enabled. It only writes chase-camera state while the local pawn is
alive and on a playing team, and restores the original observer fields when
disabled. Input injection is disabled if `/dev/uinput` cannot be opened.
Enable `Sniper crosshair` to force CS2's native crosshair cvars, including the
sniper width and scoped render flag. It is active only while the live active
weapon is an AWP, G3SG1, SCAR-20, or SSG 08; it is hidden for every other
weapon. A shaped child overlay is also drawn at the game-window center, so the
reticle does not depend on CS2's suppressed sniper HUD. The original cvar
values are restored when the feature is disabled.

## Custom Gura character

`install.sh` automatically installs
`/home/ceyro/Downloads/gawr_gura_player_model.zip` as the addon
`game/csgo_addons/hamzex_gura/` when a local CS2 installation is found. You can
override the paths with `MODEL_ZIP=/path/to/model.zip` and
`CS2_ADDON_DIR=/path/to/game/csgo_addons`.
For clients that do not mount loose addon folders, the installer also copies
the compiled resources into the normal `game/csgo/` search path.

In the Movement page, enable `Gura body + hands`. The body uses the supplied
player model and matching first-person arm model. The addon must be installed
before enabling the option; restart the current round after changing models.
The runtime uses the archive's compiled resources at
`characters/models/nozb1/gawr_gura_player_model/*.vmdl`; restart CS2 after
installing them so the resource system rebuilds its search paths. Retail CS2
may require the addon to be packed/mounted; `install.sh` also places the files
under `game/csgo/` as a loose-file fallback.

## Offset safety

There are no copied static offset tables in the feature code. At runtime:

1. the local controller is found with the current client signature;
2. the entity system is obtained from `GameResourceServiceClientV0`;
3. field offsets are read from the live `libschemasystem.so` schema;
4. the input button-state offset comes from `InputSystemVersion0`;
5. entity identity size is taken from the live schema;
6. features do nothing until required fields pass validation.

If the game updates and a signature or schema field cannot be resolved, the
feature remains disabled and `/tmp/hamzex.log` records the failure. Do not
replace offsets by hand; update the resolver signatures only after verifying
them against the current live binaries.

## Logs and troubleshooting

```sh
tail -f /tmp/hamzex.log
```

- `XOpenDisplay failed`: start the script from the logged-in graphical session
  or set the correct `DISPLAY`/Xauthority environment.
- `/dev/uinput unavailable`: grant the running user access to uinput; the GUI
  and non-input features can still load.
- `client offsets are not ready`: the game build is not supported by the
  current runtime signatures; do not force guessed offsets.
