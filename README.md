# F-Zero Recomp for PS Vita

Prototype of a static recompilation of *F-Zero (USA)* for the PlayStation Vita made over the weekend with AI.

## Build and install
Requires VitaSDK, CMake 3.20+, Ninja, Python 3.11+ and Git.
1. Build the VPK.
```bash
git clone https://github.com/rameauv/FZeroRecomp-vita.git
cd FZeroRecomp-vita
git submodule update --init --recursive
export VITASDK=/path/to/vitasdk

# Emit generated/ from your own ROM. The script checks the ROM's SHA-256,
# applies the snesrecomp patches, and pins the analysis backend this build is
# known to match. Omit the argument to use "F-Zero (USA).sfc" beside this file.
sh tools/regenerate.sh /path/to/"F-Zero (USA).sfc"

sh build.sh
```
2. Install `fzero_recomp.vpk` with VitaShell.
3. Create `ux0:/data/fzero_recomp/` and put your ROM there as `fzero.sfc`.

## Controls

| Vita | SNES | In F-Zero |
| --- | --- | --- |
| ✕ | B | Accelerate |
| □ | Y | Brake |
| ○ | A | — |
| △ | X | — |
| L / R | L / R | Air brakes |
| D-pad or left stick | D-pad | Steer |

Hold Start + Select for about a second to save and quit.

See [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) for how the port works.

## ROM requirements

Generation needs a **headerless F-Zero (USA)** cartridge dump:

| Property | Value |
| --- | --- |
| Size | 524,288 bytes |
| Mapping | LoROM |
| SHA-256 | `bf16c3c867c58e2ab061c70de9295b6930d63f29f81cc986f5ecae03e0ad18d2` |

## Credits and licence

Built on [snesrecomp](https://github.com/craigshaw/snesrecomp) and the
projects it credits. See [third-party notices](docs/THIRD_PARTY_NOTICES.md).

Original code in this repository uses the
[PolyForm Noncommercial License 1.0.0](LICENSE).

Required Notice: Copyright (c) 2026 Craig Shaw

Third-party code retains its respective licences. The ROM, the generated game
code, and the game artwork are not covered by the project licence. The
gameplay screenshots illustrate the project; the artwork shown belongs to its
respective rights holders. This project is not affiliated with or endorsed by
Nintendo. *F-Zero* and its related names and assets belong to their respective
owners.

## Development disclosure

AI coding assistants have contributed to the runtime, tooling, testing, and documentation under the maintainer's direction and review.
