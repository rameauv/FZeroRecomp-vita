#!/usr/bin/env sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
DEPENDENCY="$ROOT/snesrecomp"
PATCH_DIR="$ROOT/patches/snesrecomp"
# Upstream base. Patches 0001-0008 turn it into the integration commit.
UPSTREAM_BASE=4d42cab33d02a8ce3dd5626ca2f2a7b91cedb628
# The integration commit, published on craigshaw/snesrecomp. This is the
# submodule pin. Patches 0009 and later add the PS Vita runner on top of it.
INTEGRATION=da4a541713fd28702dd8c11c87a2c0fd69fcce45
# The local commit that patches 0009 and later were exported from. A checkout
# that descends from it already contains every patch.
VITA_REVISION=cb76cc21fc2e3b9854ce949d3e9d89f420f49e11

if [ ! -d "$DEPENDENCY/.git" ] && [ ! -f "$DEPENDENCY/.git" ]; then
  printf '%s\n' 'snesrecomp submodule is not initialised.' >&2
  printf '%s\n' 'Run: git submodule update --init' >&2
  exit 1
fi

head=$(git -C "$DEPENDENCY" rev-parse HEAD)

if git -C "$DEPENDENCY" merge-base --is-ancestor \
       "$VITA_REVISION" "$head" 2>/dev/null; then
  if [ -n "$(git -C "$DEPENDENCY" status --porcelain)" ]; then
    printf '%s\n' 'The snesrecomp checkout has local edits; review them first.' >&2
    exit 1
  fi
  printf '%s\n' 'All patches are included in the snesrecomp checkout.'
  exit 0
fi

case $head in
  "$INTEGRATION")   first=0009 ;;
  "$UPSTREAM_BASE") first=0001 ;;
  *)
    printf 'unexpected snesrecomp revision: %s\nexpected: %s\n' \
      "$head" "$INTEGRATION" >&2
    exit 1 ;;
esac

PATCHES=$(for p in "$PATCH_DIR"/*.patch; do
  n=$(basename "$p" | cut -c1-4)
  [ "$n" \< "$first" ] || printf '%s\n' "$p"
done)

# Build the expected final tree in a separate index, so that the checkout can
# be compared with it without changing the real index. Later patches revise
# earlier hunks, so per-patch reverse checks cannot identify a full stack.
EXPECTED_INDEX=$(mktemp "${TMPDIR:-/tmp}/fzero-snesrecomp.XXXXXX")
trap 'rm -f "$EXPECTED_INDEX"' EXIT HUP INT TERM
rm -f "$EXPECTED_INDEX"
GIT_INDEX_FILE="$EXPECTED_INDEX" git -C "$DEPENDENCY" read-tree "$head"
for p in $PATCHES; do
  GIT_INDEX_FILE="$EXPECTED_INDEX" git -C "$DEPENDENCY" apply --cached --whitespace=nowarn "$p"
done

matches_expected() {
  GIT_INDEX_FILE="$EXPECTED_INDEX" git -C "$DEPENDENCY" update-index -q --refresh >/dev/null 2>&1 || true
  GIT_INDEX_FILE="$EXPECTED_INDEX" git -C "$DEPENDENCY" diff --quiet
}

if matches_expected; then
  printf '%s\n' 'All patches are already applied.'
  exit 0
fi

if [ -n "$(git -C "$DEPENDENCY" status --porcelain)" ]; then
  printf '%s\n' 'The snesrecomp checkout has edits that are not the patch stack.' >&2
  exit 1
fi

for p in $PATCHES; do
  git -C "$DEPENDENCY" apply --whitespace=nowarn "$p"
  printf 'applied: %s\n' "$(basename "$p")"
done

if ! matches_expected; then
  printf '%s\n' 'The patched checkout differs from the expected patch stack.' >&2
  exit 1
fi
