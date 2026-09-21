#!/usr/bin/env sh
# Regenerate generated/ from the ROM and config/.
#
# --analysis-backend python is not a preference: the native analyzer proves
# fewer exact entry widths, and the variants it cannot prove fall back to the
# interpreter. On this ROM native yields 661 AOT / 24 LLE where python yields
# 678 AOT / 9 LLE, and the python output is byte-identical to the PC host's
# generated tree. Interpreted and compiled bodies do not share a return/stack
# contract, so a build that interprets 15 functions the reference build
# compiles is not the same program. The default (auto) picks native when the
# release binary is present, so pin it.
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
ROM=${1:-"$ROOT/F-Zero (USA).sfc"}
EXPECTED=bf16c3c867c58e2ab061c70de9295b6930d63f29f81cc986f5ecae03e0ad18d2

[ -f "$ROM" ] || { printf 'ROM not found: %s\n' "$ROM" >&2; exit 1; }
actual=$(shasum -a 256 "$ROM" | cut -d' ' -f1)
[ "$actual" = "$EXPECTED" ] || {
  printf 'Unsupported ROM SHA-256: %s\nExpected: %s\n' "$actual" "$EXPECTED" >&2; exit 1; }

sh "$ROOT/tools/apply_snesrecomp_patches.sh"
python3 "$ROOT/snesrecomp/tools/v2_sync_funcs_h.py" \
    --cfg-dir "$ROOT/config" --out "$ROOT/config/funcs.h"
python3 "$ROOT/snesrecomp/tools/v2_emit.py" \
    --rom "$ROM" \
    --cfg-dir "$ROOT/config" \
    --out-dir "$ROOT/generated" \
    --cfg-roots \
    --analysis-backend python
