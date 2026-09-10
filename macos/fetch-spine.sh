#!/bin/zsh
# macos/fetch-spine.sh -- fetch the spine-cpp 4.2 runtime into macos/spine/.
#
# The runtime is third-party (Esoteric Software) and requires a Spine licence
# of your own; see ../NOTICE.md and macos/spine/README.md. It is never
# committed: .gitignore excludes macos/spine/include and macos/spine/src.
#
# Usage: ./fetch-spine.sh [branch]   (branch defaults to 4.2)

set -euo pipefail
cd "$(dirname "$0")"

BRANCH="${1:-4.2}"
TMP=$(mktemp -d)

cleanup() { rm -rf "$TMP"; }
trap cleanup EXIT

git clone --depth 1 --branch "$BRANCH" --filter=blob:none --sparse \
    https://github.com/EsotericSoftware/spine-runtimes.git "$TMP/sr"
cd "$TMP/sr"
git sparse-checkout set spine-cpp

DEST="$(pwd)/spine"
mkdir -p "$DEST"
cp -R spine-cpp/spine-cpp/include "$DEST/"
cp -R spine-cpp/spine-cpp/src "$DEST/"
echo "spine-cpp ($BRANCH) -> $DEST"
echo "headers: $(find "$DEST/include" -name '*.h' | wc -l | tr -d ' ')  sources: $(find "$DEST/src" -name '*.cpp' | wc -l | tr -d ' ')"
