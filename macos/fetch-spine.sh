#!/bin/zsh
# macos/fetch-spine.sh -- fetch the spine-cpp 4.2 runtime into macos/spine/.
#
# The runtime is third-party (Esoteric Software) and requires a Spine licence
# of your own; see ../NOTICE.md and macos/spine/README.md. It is never
# committed: .gitignore excludes macos/spine/include and macos/spine/src.
#
# Usage: ./fetch-spine.sh [branch]   (branch defaults to 4.2)
#
# Pinned: upstream 1.3.0 vendors spine-cpp at 4.2 commit e7dc1435 (see
# shittim-logon-1.3.0-src/NOTICE.txt). The pin keeps the mac runtime byte-
# identical to the Windows build's; pass a branch name explicitly to override.

set -euo pipefail
cd "$(dirname "$0")"

BRANCH="${1:-4.2}"
COMMIT="e7dc1435fa4a0083ab431f1b28e083c14a1f5c68"
TMP=$(mktemp -d)
cleanup() { rm -rf "$TMP"; }
trap cleanup EXIT

git clone --branch "$BRANCH" --filter=blob:none --sparse \
    https://github.com/EsotericSoftware/spine-runtimes.git "$TMP/sr"
cd "$TMP/sr"
git sparse-checkout set spine-cpp
if [ "$BRANCH" = "4.2" ]; then
    git checkout -q "$COMMIT"
fi

DEST="$(pwd)/spine"
mkdir -p "$DEST"
cp -R spine-cpp/spine-cpp/include "$DEST/"
cp -R spine-cpp/spine-cpp/src "$DEST/"
echo "spine-cpp ($BRANCH) -> $DEST"
echo "headers: $(find "$DEST/include" -name '*.h' | wc -l | tr -d ' ')  sources: $(find "$DEST/src" -name '*.cpp' | wc -l | tr -d ' ')"
