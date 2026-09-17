# reference/ -- upstream 1.3.0 overlay files kept for the macOS port

Copied verbatim from `shittim-logon-1.3.0-src/overlay/src/` (the upstream
Windows source, 2026-09) so that the macOS port can consult them after the
full source tree is removed. They are Windows-side code and are **not built**
here; the portable parts of the same upstream release live in `adapter/`.

| file | what the macOS port takes from it |
| ---- | --------------------------------- |
| `arrival.h` | 27 lines, no Windows coupling. The wake trigger: input must be *held* for 0.35 s before "somebody arrived" fires -- a pointer crossing the screen is not an arrival. Ported candidate for Engine's wake detection. |
| `gpu_scene.h` | How the D3D11 overlay consumes `scene.h` (passes, tints, the fx/drift layers). The reference for replacing the Metal sink's reverse-engineered white flash and particle layer with the official `fx_layer.h`/`drift.h` behaviour. |
| `clock.h` + `localeclock.h` | The clock patch drawn over the room (D2D/DWrite). Layout constants and the locale logic behind `force_24_hour`; the macOS clock was matched to the reference recording by eye and can be re-derived from these. |

Not copied: the audio subsystem (`bgm.h`, `audiodev.h`, ...), the credential
watchers (`authwatch.h`, `etwauth.h`), the DComp/stage plumbing, and the
installer/config UI -- none of it is on the macOS roadmap.
