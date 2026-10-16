#!/usr/bin/env bash
set -euo pipefail

# Requires ESP-IDF v5.3, activated with its export.sh script.
if ! command -v idf.py >/dev/null; then
  echo 'Activate ESP-IDF v5.3 before running this script.' >&2
  exit 1
fi
if [[ "$(idf.py --version)" != 'ESP-IDF v5.3' ]]; then
  echo 'This firmware requires the pinned ESP-IDF v5.3 release.' >&2
  exit 1
fi
script_dir="$(cd "$(dirname "$0")" && pwd)"
target="${1:-$script_dir/build-tree}"
git clone --branch v4.2 --depth 1 https://github.com/ModRetro/oss-chromatic-console-mcu.git "$target"
if [[ "$(git -C "$target" rev-parse HEAD)" != 6c94613da72420f6a364e0024681401bd69184ba ]]; then
  echo 'The upstream v4.2 tag changed; refusing to build.' >&2
  exit 1
fi
git -C "$target" apply "$script_dir/changes.patch"
cp "$script_dir"/extra/components/osd/kortamod_* "$target/components/osd/"
python "$script_dir/optimize_images.py" "$target/components/images"
cd "$target"
idf.py build
echo "Built $target/build/mcu_fw.bin"
