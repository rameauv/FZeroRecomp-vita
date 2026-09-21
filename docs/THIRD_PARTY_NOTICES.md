# Third-party notices

The VPK is built from this repository's code, the recompiler runtime from
[snesrecomp](../snesrecomp/LICENSE) (see its
[third-party notices](../snesrecomp/THIRD_PARTY_ATTRIBUTION.md)), and these
libraries linked from VitaSDK. Each keeps its own licence:

| Library | Use | Licence |
| --- | --- | --- |
| [SDL2](https://github.com/libsdl-org/SDL) (Vita port) | Video, audio and events | zlib |
| [debugnet](https://github.com/psxdev/debugnet) | Network logging | See upstream |
| [VitaSDK](https://github.com/vitasdk) runtime and stubs | System libraries | See upstream |

Parts of the SDL2 audio, input and main loop setup follow patterns from
[sm64-vita](https://github.com/bythos14/sm64-vita).
