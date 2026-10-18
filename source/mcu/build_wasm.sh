#!/usr/bin/env bash
# Builds the games engine for the site's preview: cloud/public/kgame.wasm.
# Needs clang with the wasm32 target and wasm-ld (macOS: brew install lld).
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
osd="$here/extra/components/osd"
PATH="/opt/homebrew/opt/lld/bin:$PATH"
clang --target=wasm32 -std=c11 -O2 -nostdlib -ffreestanding -fno-builtin -Wall -Wextra -Wno-unused-parameter -I "$osd" \
  -Wl,--no-entry -Wl,--export-memory -Wl,--strip-all -Wl,-z,stack-size=131072 \
  -o "$here/../../cloud/public/kgame.wasm" "$osd/kortamod_game.c" "$here/kortamod_game_wasm.c"
ls -l "$here/../../cloud/public/kgame.wasm"
