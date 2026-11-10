#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="$SCRIPT_DIR/build"
CONFIG_DIR="${XDG_CONFIG_HOME:-${HOME:?HOME is not set}/.config}/hamzex"
MODEL_ZIP="${MODEL_ZIP:-/home/ceyro/Downloads/gawr_gura_player_model.zip}"
CS2_ADDON_DIR="${CS2_ADDON_DIR:-}"

die() {
  echo "[install] error: $*" >&2
  exit 1
}

command -v cmake >/dev/null 2>&1 || die "cmake is required"
command -v ninja >/dev/null 2>&1 || die "ninja is required"
command -v c++ >/dev/null 2>&1 || die "a C++ compiler is required"

[ -f "$SCRIPT_DIR/config.json" ] || die "config.json is missing from $SCRIPT_DIR"

echo "[install] configuring Hamzex"
cmake -S "$SCRIPT_DIR" -B "$BUILD_DIR" -G Ninja -DCMAKE_BUILD_TYPE=Release
echo "[install] building libHamzex.so"
cmake --build "$BUILD_DIR"

mkdir -p "$CONFIG_DIR"
if [ ! -e "$CONFIG_DIR/config.json" ]; then
  install -m 0644 "$SCRIPT_DIR/config.json" "$CONFIG_DIR/config.json"
  echo "[install] copied default config to $CONFIG_DIR/config.json"
else
  echo "[install] preserving existing $CONFIG_DIR/config.json"
fi

chmod 0755 "$SCRIPT_DIR/injekt.sh"
echo "[install] complete"
echo "[install] library: $BUILD_DIR/libHamzex.so"
if [ -f "$MODEL_ZIP" ]; then
  if ! command -v unzip >/dev/null 2>&1; then
    echo "[install] warning: unzip is required to install the custom model addon"
  else
    if [ -z "$CS2_ADDON_DIR" ]; then
      for candidate in \
        "$HOME/.local/share/Steam/steamapps/common/Counter-Strike 2/game/csgo_addons" \
        "$HOME/.steam/steam/steamapps/common/Counter-Strike 2/game/csgo_addons" \
        "$HOME/.local/share/Steam/steamapps/common/Counter-Strike Global Offensive/game/csgo_addons" \
        "$HOME/.steam/steam/steamapps/common/Counter-Strike Global Offensive/game/csgo_addons"; do
        if [ -d "${candidate%/csgo_addons}" ]; then
          CS2_ADDON_DIR="$candidate"
          break
        fi
      done
    fi
    if [ -n "$CS2_ADDON_DIR" ]; then
      ADDON_DIR="$CS2_ADDON_DIR/hamzex_gura"
      mkdir -p "$ADDON_DIR"
      unzip -q -o "$MODEL_ZIP" -d "$ADDON_DIR"
      if [ ! -f "$ADDON_DIR/addoninfo.txt" ]; then
        printf '%s\n' '"AddonInfo"' '{' '  "name" "hamzex_gura"' '  "SteamAppId" "730"' '  "version" "1"' '  "IsTemplate" "0"' '  "IsPlayable" "0"' '}' \
          > "$ADDON_DIR/addoninfo.txt"
      fi
      echo "[install] installed custom model addon: $ADDON_DIR"

      # Some retail Linux clients do not mount an arbitrary loose
      # csgo_addons/<name> search path for player resources. Install a second
      # copy in the normal client search path so SetModel can resolve the
      # compiled .vmdl_c and material files without requiring an addon pack.
      GAME_DIR="$(dirname "$CS2_ADDON_DIR")"
      BASE_CSGO_DIR="$GAME_DIR/csgo"
      mkdir -p "$BASE_CSGO_DIR"
      unzip -q -o "$MODEL_ZIP" -d "$BASE_CSGO_DIR"
      echo "[install] installed model fallback: $BASE_CSGO_DIR/characters/..."
    else
      echo "[install] warning: CS2 game/csgo_addons was not found; set CS2_ADDON_DIR to install the model"
    fi
  fi
else
  echo "[install] custom model archive not found: $MODEL_ZIP"
fi
echo "[install] next: sudo bash $SCRIPT_DIR/injekt.sh"
