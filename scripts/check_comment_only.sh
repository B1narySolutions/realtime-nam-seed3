#!/bin/bash
# Checks that a change touches comments only: both firmware images, built from
# BASE and from the working tree, must be byte-identical. The .bin files carry
# no debug information, so moved line numbers don't count, but any change to
# code, constants or flags does.
#
# Usage: scripts/check_comment_only.sh [BASE]   (default origin/main)
# Needs make install and make download-models, like make build.
set -euo pipefail
cd "$(dirname "$0")/.."
root="$PWD"
base="${1:-origin/main}"
base_dir="$root/build/comment-only-base"

# Check out BASE beside this tree, sharing the pinned libraries and models.
git rev-parse --verify --quiet "$base^{commit}" >/dev/null || { echo "Unknown base: $base" >&2; exit 1; }
rm -rf "$base_dir"
git worktree prune
git worktree add --detach --quiet "$base_dir" "$base"
trap 'git -C "$root" worktree remove --force "$base_dir" 2>/dev/null || rm -rf "$base_dir"' EXIT
ln -s "$root/libs" "$base_dir/libs"
mkdir -p "$base_dir/models"
ln -s "$root/models/local" "$base_dir/models/local"

# Build both firmware images at BASE and here, showing a log only on failure.
# libDaisy is already built.
quiet() {
    local log="$root/build/comment-only.log"
    "$@" >"$log" 2>&1 || { cat "$log" >&2; echo "Build failed: $*" >&2; exit 1; }
}
quiet make -C "$root" build hwtest-build
quiet make -C "$base_dir" -f make/firmware.mk all
if [[ -f "$base_dir/make/hwtest.mk" ]]; then
    quiet make -C "$base_dir" -f make/hwtest.mk all
fi

# Compare.
status=0
for image in build/passthrough.bin build/hwtest-fw/hwtest.bin; do
    if [[ ! -f "$base_dir/$image" ]]; then
        echo "$image: not built at $base; skipped."
    elif cmp -s "$root/$image" "$base_dir/$image"; then
        echo "$image: identical to $base."
    else
        echo "$image: differs from $base, so this change is not comment-only." >&2
        status=1
    fi
done
exit $status
