# Development

## Repository layout

```
src/mpv/            patch overlay for mpv (only modified files; mirrors the mpv tree)
src/client/         Qt 6 + QML frontend (MpvController, PlayerViewModel, StereoController, RPC)
third_party/mpv     pristine mpv source (not modified by hand)
scripts/            build / install / release / translation scripts
tests/              probes and stress tests (VSR/VFG timing, 3D conversion, fullscreen stress)
docs/               engineering notes and measurements
wiki/               source for the GitHub wiki (see wiki/README.md)
```

## mpv patch scheme

`third_party/mpv` is the pristine base; `src/mpv/` is the overlay. `scripts/build_mpv.sh`
merges them into `build/mpv` (or `build/mpv-dist` for releases) and builds libmpv.

**After editing anything under `src/mpv/` you must re-run `./scripts/build_mpv.sh`.**
`build/mpv` is a *merged copy*: running `ninja -C build` alone silently keeps the stale
copy, which looks like "the filter is not in the chain". The same applies to the client:
`ninja -C build` does not rebuild libmpv.

Custom filters live in `src/mpv/video/filter/`: `vf_hwup`, `vf_vsr`, `vf_truehdr`, `vf_vfg`,
`vf_rot`, `vf_eye` (plus the frozen `vf_rife`).

## Build commands

| Command | Purpose |
|---|---|
| `./scripts/build_mpv.sh` | merge overlay + build libmpv and filters |
| `ninja -C build` | build the Qt client |
| `./scripts/build-from-source.sh` | end-to-end: deps check → build → install → dependency snapshot |
| `./scripts/build_release.sh [--variant=standard\|full] [--fruc-backend=vfg\|rife]` | release build + dist RPATH + dependency collection + closure check + tarball |
| `./scripts/install.sh [--dev] [--no-vfx] [--no-rtx]` | install to `~/.local` (dev mode reads the build tree; `--dev` forces it; `--no-vfx/--no-rtx` keep the 900 MB VFX runtime out) |
| `./scripts/check-deps.sh` | compare build-time dependency snapshot with the current system |
| `./update-translations.sh` / `--check` | refresh `.ts` from sources / verify completeness |

`--no-rtx` (or `-Dno_rtx=true` for the mpv build) also disables the CUDA/VFX half of the
filter chain, which is how the player is smoke-tested on a machine without an NVIDIA GPU.

Gotchas worth knowing:

- `build_mpv.sh` filters its output — a failed compile can be hidden; look for the final
  "Done" line.
- `install.sh` replaces files atomically (temp file + rename) so installing while the player
  runs cannot crash the running instance.
- Dev builds have a build-tree RPATH; for an installed binary that must load the bundled
  libraries, use the release path (`build_release.sh` → `install.sh`), not the dev tree.

## Tests and probes

| Path | What it does |
|---|---|
| `tests/vfg/vfg_timing_probe.c` | pure NvVFX VFG timing, no player (landscape vs portrait paths) |
| `tests/fs_stress.sh` | fullscreen toggling stress (device-loss regressions) |
| `tests/hotkeys/test_hotkeys.py` | key map, popup hand-off, menu state machine, single-eye branch — drives a real player through the JSON IPC and injects keys |
| `tests/hotkeys/test_stereo_keys.py` | real 3D session with a second screen as the glasses: key routing, help overlay, both windows, exit/restore |
| `tests/hotkeys/test_thumbnails.py` | hover thumbnails: size and DPR, network streams, first-hover replay, retry cooldown, menu toggle |
| `tests/fruc/build_rife_full_engine.sh` | builds the frozen RIFE TensorRT engine with the system `trtexec` |
| `tests/ff2mpv/*.c` | libmpv option/terminal probes used while wiring the browser-integration wrapper |
| `tools/mvc2sbs` | offline MVC → full-width SBS conversion |

Shortcuts have a single source of truth: `src/client/Keymap.cpp` (one table) executed by
`KeyRouter`. QML never declares a `Shortcut` or compares `Qt.Key_*` for app actions — the
context menu and the 3D picker resolve physical keys through the same table
(`keymap::findMenu` → `keys.menuAction`), so a binding exists exactly once. `check_keys.py`
(also run by `scripts/precommit.sh`) enforces this and rejects duplicate bindings,
unimplemented actions and shortcuts leaking back into QML.

The `tests/hotkeys` scripts take over the window, keyboard and audio for their duration, so
run them when nothing else is playing.

Many behaviours are verifiable without looking at the screen, through the JSON IPC
(`get-playlist`, `get-video-size`, `get-stereo`, `get-source-format`, `set-eye`, `set-rotate`,
`grab-window`, `vf-command`). This is the preferred way to check geometry, filter state and
playlist contents.

## Debugging tips

- Logs go to stderr; raise verbosity with `-- --msg-level all=v` (frame-level: `all=dbg`).
  `--benchmark` is headless and quiet (`all=no`).
- The OSD (`Tab`) reports the actual output format, VSR factor and cost, interpolation state
  and the selected decoder — often enough to tell "passthrough" from "working".
- GUI thread notes: never call synchronous mpv APIs from the GUI thread while media is
  playing (the paint thread also serves DR image allocation); use the `*Async` variants or
  reflections via observers. This is documented at length in the engineering notes.

## Design records (`docs/`)

| Document | Subject |
|---|---|
| `docs/hdr-pipeline-investigation.md` | 10-bit chain, HDR10 output, colour management measurements |
| `docs/seekbar-thumbnail-research.md` | hover-thumbnail design: geometry, DPR, network-stream frame extraction |
| `docs/vfx-sdk-1.3-investigation.md` | VFX SDK 1.3 capabilities and API findings |
| `docs/vfx-truehdr-investigation.md` | SDR→HDR10 upconversion parameters and behaviour |
| `docs/mvc-3d-open-source.md` | MVC decoding landscape / why it is not done in the player |
| `docs/ff2mpv-support-research.md` | browser integration notes |
| `docs/trt-engine-investigation.md` | TensorRT engine compatibility for the frozen RIFE backend |
| `docs/fruc-distribution-design.md` | interpolation backends and packaging |
| `docs/mpv-reference-index.md` | mpv source reference index used while patching |

## Contributing

Bug reports are most useful with: GPU model, driver version, `vsr-player --benchmark` output
or a log at `all=v`, and the exact source (resolution, fps, HDR/3D packing). The project is
intentionally agent-friendly: the code, the mpv overlay and the design records are written so
that an AI coding agent can follow the pipeline and locate issues quickly.
