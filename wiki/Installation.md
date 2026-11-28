# Installation

Three paths, in order of preference.

## Requirements

| Component | Requirement | Applies to |
|-----------|-------------|------------|
| GPU | NVIDIA RTX 20-series or newer for VSR; frame interpolation needs **Ada/Blackwell** for the default VFG backend (the frozen RIFE source-build backend needs Ampere+ with FP16 Tensor Cores) | all |
| Driver | 570+ (with CUDA; Wayland works out of the box — DRM modeset is enabled by default on modern drivers) | all |
| VFX SDK | NVIDIA VFX SDK **1.3** (`nvidia-vfx` 0.2.0.0 wheel) — provides VSR, VFG **and** TrueHDR; `install.sh` downloads and unpacks it from NVIDIA's official index (`pypi.nvidia.com`), never into a Python environment | all |
| Qt | 6.12+ (Quick, QuickControls, Vulkan) | source build / standard tarball (no prebuilt full package — build it with `--variant=full`) |
| C++ compiler | GCC 13+ (C++20) | source build |
| Build | meson, ninja, CUDA Toolkit (`/opt/cuda`); TensorRT (`trtexec`) only to build the frozen RIFE backend | source build |

Third-party SDKs (NvVFX headers/runtime, MDI icon font, mpv source) are **not** bundled in
the repository — see
[docs/third-party-setup.md](https://github.com/zhangmq/vsr-player/blob/master/docs/third-party-setup.md)
to prepare `third_party/` (needed for source builds).

## Option A (recommended): build from source

Build on your own machine: the resulting binaries **match your system libraries by
construction**, so a system upgrade is fixed by re-running the same command — no
"stale soname → won't start" class of breakage (which this project hit for real when
libcdio/libbluray were upgraded).

```bash
git clone https://github.com/zhangmq/vsr-player && cd vsr-player
# 1) prepare third_party/ per docs/third-party-setup.md
#    (NvVFX runtime, MDI font, mpv source; RIFE ONNX only for -Dfruc_backend=rife)
./scripts/build-from-source.sh
```

One command does it all: dependency self-check → build libmpv + client → install to
`~/.local` (no sudo) → record a dependency snapshot. The first run fetches the VFX runtime
from **NVIDIA's official index** (~0.6 GB wheel, ~1.1 GB once extracted); add `--no-vfx`
to skip if you already have it, plus `--no-install` / `--jobs N`.

**After a system upgrade** (Qt/FFmpeg/media-library soname changes break stale binaries):

```bash
./scripts/check-deps.sh              # shows drift (build-time snapshot vs current system libs)
./scripts/build-from-source.sh       # rebuild with one command
```

## Option B: prebuilt tarball

For when you would rather not compile, or your distro lacks FFmpeg 9.x development packages.

| | standard (the only distributed prebuilt package) | full (source build only — `--variant=full`) |
|---|---|---|
| What it ships | **thin (~7 MB)**: `vsr-player`, `mpv-vsr`, `libmpv.so.2`, the two Wayland helper modules (`libvsr-color-feedback`, `libvsr-idle-wayland`), the two patched Qt 6.12.0 libraries (used only when the system Qt is still 6.12.0 — Arch/CachyOS are on 6.12.1 now), `deps.manifest` (machine-readable list of what the system must provide), icon font, translations, licenses, desktop assets, `COMPONENTS.md` | the same **+ everything that has actually broken on distro upgrades**: FFmpeg ×7, CUDA runtime, Qt6 (libraries / platform plugins / QML modules), the optical-media stack, libplacebo + its shader stack, libmujs, libsndio |
| Comes from the system | **everything else**: FFmpeg 9.x (`libav*`, `NEEDED` de-versioned and checked at install time), CUDA driver/runtime, Vulkan loader, Wayland/X11 client libs, the audio stack (PipeWire/Pulse/ALSA/JACK), the media stack (libcdio/libdvd/libbluray), libplacebo, libass, libarchive, …, plus **Qt ≥ 6.12** | only the ABI that cannot be carried: glibc/C++ runtime, NVIDIA driver, Vulkan loader, DRM/GBM, X11/Wayland client libs, the audio server libraries |
| VFX runtime | **never bundled** (NVIDIA's SLA forbids redistribution) — `install.sh` downloads it from NVIDIA's index, or you place it in `~/.local/lib/vsr-player/` yourself | same |
| For | rolling-release distros (Arch/CachyOS/Fedora…) with FFmpeg 9.x and Qt ≥ 6.12 | distros with Qt < 6.12, or when you want a self-contained Qt |

`standard` is thin by contract: system libraries stay on the system, and the package ships
`deps.manifest` — one line per required soname plus what needs it. `install.sh` verifies the
manifest and **refuses to install when something is missing**, printing the missing soname
and its owner instead of leaving you with a bare loader error. `full` flips the default and
carries every library that has actually broken on distro upgrades; only the system ABI
(glibc, NVIDIA driver, Vulkan loader, DRM/GBM, display/audio client libraries) stays
outside. FFmpeg is the one dependency whose `NEEDED` entries are **de-versioned**
(`libavcodec.so` without the soname suffix) so a distro version is not pinned; the installer
records and checks the major version:

- `lib/vsr-player/VSR-FFMPEG-MAJOR` records the FFmpeg major version the package was built
  against; the installer compares it with the target system and warns on mismatch.
- A missing soname is reported explicitly (with a compatibility-symlink fallback that is
  validated with `ldd -r` and rolled back if symbols are missing).
- `ldd` resolving `libmpv.so.2` to a system mpv instead of the bundled one is a hard error —
  that combination "runs" but silently loses every AI stage.
- `COMPONENTS.md` (generated at package time from the artifacts) lists what the package
  carries, what it expects from the system, and the mpv requirement per library.

What that means in practice: a `standard` tarball never breaks because a disc/placebo/sndio
library changed during `pacman -Syu`, but it still wants a reasonably current distro for
FFmpeg and Qt. If your FFmpeg/Qt is older — or you want zero exposure to system media
libraries — build from source (identical trade-off, compiled locally) or build the full
variant.

```bash
tar -xJf vsr-player-<ver>-linux-x86_64.tar.xz
./install.sh                         # installs to ~/.local, no sudo
```

Check your Qt with `pkg-config --modversion Qt6Quick`: ≥ 6.12 → standard; otherwise build
the full variant yourself.

**Building without an NVIDIA GPU** — `bash scripts/build-from-source.sh --no-rtx`
(`-Dno_rtx=true` for the mpv build) leaves the CUDA/VFX part out entirely, so the binary
starts on a machine without an N card. The AI features are absent by construction; this is
meant for testing the player shell and for packaging checks. The floor is 6.12 because the package is built with **Qt
6.12.0**, and Qt 6 binary compatibility is **one-way** (a package built with 6.x runs on
6.x and newer 6.y, not on older ones).

**The optional `-rife` package is discontinued** (as of 0.4.0): the RIFE backend is frozen
and no longer shipped as a prebuilt tarball. It remains available as a source-build option —
`./scripts/build-from-source.sh --fruc-backend=rife`, or
`./scripts/build_release.sh --fruc-backend=rife` to package it yourself — which matters for
Ampere and older GPUs, since the default VFG backend requires Ada/Blackwell.

The installer also sets up **desktop integration**: a **VSR Player** entry appears in the
application menu/launcher (pinnable to the taskbar, usable as the default handler for
video/audio files). The package carries the application icon in two forms — a scalable SVG
and a 512 px PNG — installed into `~/.local/share/icons/hicolor/{scalable,512x512}/apps/`,
which is what the launcher, taskbar and file manager show. `data/vsr-player.svg` is the
single source: `scripts/render-icon.sh` regenerates the PNG from it, and that same PNG is the
one embedded in the **About** dialog, so the two never diverge.

- The **VFX runtime** (~0.6 GB wheel, ~1.1 GB extracted) is **not** bundled (NVIDIA's SLA
  forbids redistribution): `install.sh` downloads and extracts it from **NVIDIA's official
  index `pypi.nvidia.com`** (plain curl, no pip, no system changes), or you can place it in
  `~/.local/lib/vsr-player/` manually.
- Packaging uses "explicit core set + auto-discovered media stack + **closure check at
  package time**": any dependency that is neither bundled nor on the system allowlist fails
  the build, so a broken tarball cannot ship silently.
- Both variants also rely on ordinary distro components: the Vulkan loader, Wayland/X11
  client libraries, and mpv/FFmpeg optional deps (libass, libplacebo, libmujs, liblcms2,
  libwebp, libopenjp2, the 1394 capture libs, …) — all validated by the closure check.

## Option C: manual build (development/debugging)

1. **Prepare `third_party/`** — NvVFX SDK headers/runtime, MDI icon font, mpv source,
   CUDA 12 archive; RIFE ONNX asset only for `-Dfruc_backend=rife`: follow
   [docs/third-party-setup.md](https://github.com/zhangmq/vsr-player/blob/master/docs/third-party-setup.md).
2. **Build and run:**

```bash
./scripts/build_mpv.sh          # merge src/mpv overlay → third_party/mpv, build libmpv + filters
ninja -C build                  # build the Qt client
./build/src/client/vsr-player <video-or-directory>
```

Notes / gotchas:

- **mpv patch scheme**: `third_party/mpv` is the pristine base; `src/mpv/` is the overlay
  (mirrors the mpv tree, only modified files). After editing anything under `src/mpv/` you
  **must** re-run `./scripts/build_mpv.sh` — `build/mpv` is a *merged copy*, and running
  `ninja` on it alone silently keeps the stale copy.
- **`build_mpv.sh` output is grep-filtered** — a failed compile can be hidden. Confirm the
  build finished by checking for the final "Done" line.
- **Don't `cd build` and then use `./build/...`** — relative paths break. Stay at the
  repository root or use absolute paths.
- **After system library upgrades** (FFmpeg, Qt, TensorRT, libcdio, … — pacman/apt) stale
  binaries link the old sonames and fail to start: run `./scripts/build-from-source.sh`, or
  manually `./scripts/build_mpv.sh` + `ninja -C build`. `./scripts/check-deps.sh` compares
  against the build-time snapshot and tells you whether a rebuild is needed.
- **RIFE engine** (frozen backend, source builds only): built with the system `trtexec` via
  `bash tests/fruc/build_rife_full_engine.sh` (dynamic-shape FP16, `--hardware-compat on`
  for cross-architecture). Requires the RIFE ONNX asset in `third_party/rife/`.
- **Development install**: `./scripts/install.sh` works from a repository checkout too (dev
  mode — picks up the build tree automatically, no tarball needed). Add `--dev` to force the
  dev tree when a stale `build/client-dist` exists.
- **Distributable build**: `./scripts/build_release.sh [--variant=standard|full]
  [--fruc-backend=vfg|rife]` — release build + `$ORIGIN`-relative RPATH + dependency
  collection + closure check + tarball. Only `--variant=standard` is distributed;
  `--variant=full` (bundles Qt) exists for Qt < 6.12 users to build themselves.
  `build_mpv.sh`/client builds accept `MPV_BUILD_DIR`, `BUILDTYPE`, `DIST_RPATH` env
  overrides (used by the release scripts).

## Version compatibility

| Component | Binding | If mismatched |
|-----------|---------|---------------|
| **VFX SDK ↔ driver** | The latest NVIDIA-index wheel may require a newer driver; an old driver + new VFX → VSR fails to load | Upgrade the driver, or pin an older VFX version (below) |
| **RIFE engine ↔ TensorRT** (frozen backend, source builds only) | Engine files embed the exact TRT version that built them — deserialization fails on version mismatch (verified in both directions) | Rebuild the engine with your system TRT (`bash tests/fruc/build_rife_full_engine.sh`), or install a matching TRT. The VFX SDK's own TRT 10 libs coexist with RIFE's TRT 11 in one process (RTLD_LOCAL isolation). Not applicable to the default VFG backend |
| **Qt** | Hard requirement ≥ 6.12 for the source build and the standard tarball | Upgrade the system Qt, or build the full variant yourself (`--variant=full`) |
| **GPU** | VSR needs RTX 20+; the default VFG interpolation backend needs **Ada/Blackwell**, the frozen RIFE source-build backend needs Ampere+ | Older GPUs: VSR works, interpolation is unavailable (passthrough) |
| **Driver** | 570+ for VFX | Upgrade, or pin an older VFX wheel |

### Pinning a VFX SDK version

`install.sh` never forces a version onto your system — everything lives in
`~/.local/lib/vsr-player/`, and replacing the VFX files there is the supported way to switch.

```bash
# 1) list available versions (wheels live on NVIDIA's index, not pypi.org)
curl -s https://pypi.nvidia.com/nvidia-vfx/ | grep -o 'nvidia_vfx-[0-9.]*-py3-none-manylinux[^"#]*' | sort -u

# 2) download a specific version's wheel (pip download only fetches; no install)
pip download --no-deps --dest /tmp/vfx nvidia-vfx==<version> --index-url https://pypi.nvidia.com

# 3) extract into the app's lib dir + the sibling dir the VFX libs' RPATH expects
cd /tmp/vfx && unzip -o nvidia_vfx-*.whl
cp nvvfx/libs/* ~/.local/lib/vsr-player/
mkdir -p ~/.local/nvidia_vfx.libs && cp nvidia_vfx.libs/* ~/.local/nvidia_vfx.libs/
```

`wheel ≥ 0.2.0.0` also ships `nvidia_vfx.libs/` (`libcudart-<hash>.so.*`); the VFX
libraries carry `RPATH $ORIGIN/../../nvidia_vfx.libs`, so that directory must land next to
the app's lib dir (as above) or the VFX chain fails to load.
