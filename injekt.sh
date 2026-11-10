#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
SO_PATH="${SO_PATH:-$SCRIPT_DIR/build/libHamzex.so}"

die() {
  echo "[injekt] error: $*" >&2
  exit 1
}

[ "${EUID:-$(id -u)}" -eq 0 ] || die "run as root so gdb can attach to CS2"
command -v gdb >/dev/null 2>&1 || die "gdb is required"
command -v pidof >/dev/null 2>&1 || die "pidof is required"

[ -f "$SO_PATH" ] || die "shared library not found: $SO_PATH (run install.sh first)"
SO_PATH="$(realpath -- "$SO_PATH")"

PID=""
for candidate in $(pidof cs2 2>/dev/null || true); do
  PID="$candidate"
  break
done
[ -n "$PID" ] || die "CS2 is not running"

[ -f "/proc/$PID/root$SO_PATH" ] || die "CS2 cannot see $SO_PATH"

case "$SO_PATH" in
  *'"'*|*$'\n'*|*$'\r'*) die "SO_PATH contains unsupported characters" ;;
esac

echo "[injekt] attaching to CS2 pid $PID"
gdb -q -batch -nx -nh -p "$PID" \
  -ex "set pagination off" \
  -ex "call (void*)dlopen(\"$SO_PATH\", 2)" \
  -ex detach \
  -ex quit

if grep -Fq "$SO_PATH" "/proc/$PID/maps" 2>/dev/null; then
  echo "[injekt] loaded: $SO_PATH"
else
  die "the library was not found in the CS2 memory map"
fi
