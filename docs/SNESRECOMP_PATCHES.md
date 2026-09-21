# snesrecomp integration

| Revision | Commit |
| --- | --- |
| Integration on [craigshaw/snesrecomp](https://github.com/craigshaw/snesrecomp/tree/codex/fzero-upstream-20260914) | `da4a541713fd28702dd8c11c87a2c0fd69fcce45` |
| Upstream base | `4d42cab33d02a8ce3dd5626ca2f2a7b91cedb628` |

The ordered files under `patches/snesrecomp/` reproduce the integration
from that base. Keep generic changes in the dependency and game-specific host
code here.

Recovery patches use LF line endings, enforced by `.gitattributes`, so
`git apply --cached` sees the same payload on Windows and POSIX hosts.

## Patch series

| Patch | Purpose |
| --- | --- |
| 0001 | Recognise long-call trampolines in Python and Rust analysis and emission. |
| 0002 | Preserve RDNMI open-bus bits and resume parked polls at the hardware read. |
| 0003 | Give the active interpreter ownership of mixed-tier return continuations. |
| 0004 | Classify dispatch helpers at exact entry M/X widths and retract disagreements. |
| 0005 | Classify tier-2 evidence and honour final per-address entry-width overrides. |
| 0006 | Support the macOS system Bash, linker and context API in the C test harness. |
| 0007 | Restrict the low-WRAM dynamic polling policy to S-DD1 cartridges. |
| 0008 | Preserve eight-byte PPU priority-buffer alignment with MSVC, GCC and Clang; test C and C++ on Windows, Linux and macOS. |
| 0009 | PS Vita host support: runner build, `getenv` shim, APU on a second core, PPU optimisations, and FZERO_DIAG probes. Applied on top of the pinned integration. |

The previous HDMA ownership patch is retired. F-Zero now uses upstream's
`snes_set_hdma_beam_enabled` interface around its per-line HDMA/render walk
and restores the prior setting. The runtime patches preserve upstream's
interpreter stack-pop behavior and explicit tier-2 entry widths.

Patch 0005's constructed regression fixture includes a real ROM call pair.
It was reviewed and accepted unchanged. The publication audit covers these
added patches and commits, not inherited upstream content or history.

## Verify or recover

```sh
git submodule update --init
sh tools/apply_snesrecomp_patches.sh
```

The submodule is pinned at the published integration commit. At that
revision the script applies 0009 and accepts a repeated run. At the documented
base, it applies all nine in order. It refuses an unexpected revision, and a
patch that no longer applies because of local edits.

## Update the dependency

1. Start from a clean base in an isolated checkout and apply the ordered stack.
2. Preserve logical commits and synthetic tests. Run from the dependency root:

   ```sh
   python3 tests/v2/run_tests.py
   python3 -m pytest -p no:cacheprovider tests/test_tier2_ingest.py
   bash tests/run_c_tests.sh
   python3 - <<'PYTEST'
   import runpy, sys
   sys.path.insert(0, "recompiler")
   test = runpy.run_path("tests/v2/test_dispatch_helper_entry_width.py")
   test["test_x16_call_does_not_accept_x8_misalignment_as_dispatch_helper"]()
   PYTEST
   cargo test --manifest-path recompiler-rs/Cargo.toml
   ```

3. Follow the [build instructions](../README.md#build-and-install) to regenerate and
   rebuild the VPK. Exercise the affected route. Verify recovery,
   repeated application, and dirty-checkout refusal.
4. Publish the integration before updating the parent pin. Update the recovery
   patches, verifier, and this document together.

When upstream accepts a change, remove only its superseded recovery patch and
revalidate the remaining stack. The script itself is covered by
`uvx pytest tests/test_snesrecomp_patches.py`. Never edit the pinned dependency ad hoc.
