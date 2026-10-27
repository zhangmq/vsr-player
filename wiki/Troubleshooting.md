# Troubleshooting

## Multiple GPUs (iGPU + dGPU, or two NVIDIA cards): black picture / no AI / no hardware decoding

The whole AI chain is **CUDA output → zero-copy Vulkan import**, so the **CUDA device and the
Vulkan device must be the same card**. Both symptoms below come from a cross-GPU mismatch:

- the picture goes **black** after enabling upscaling/interpolation (audio and controls keep
  working), or
- the log reports that AI processing is unavailable for the Vulkan device (no CUDA device
  with the same UUID) — that is the **explicit degraded mode**: no black screen, but neither
  AI nor hardware decoding.

**First check which card the player picked** (startup log):

```
[vsr] GPU: <device name> uuid=<first 4 bytes>…
[vsr] AI processing available: <same card> ↔ CUDA <same card>      # matched
```

**Make Vulkan and CUDA point at the same card** (both are needed; take the full `uuid` from
`nvidia-smi -L` or `vulkaninfo --summary`):

```bash
VK_DRIVER_FILES=/usr/share/vulkan/icd.d/nvidia_icd.json \
CUDA_VISIBLE_DEVICES=GPU-xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx \
vsr-player <video>
```

⚠️ With two NVIDIA cards there is no reliable per-device Vulkan environment variable on many
loaders (`VK_LOADER_DEVICE_SELECT` is ineffective in practice); take the `GPU:` line from the
log as the source of truth and pin `CUDA_VISIBLE_DEVICES` to that same card. `vf_hwup`
already selects its CUDA device by the Vulkan UUID (`cuda-uuid` option), so the
software-decode path cannot pick the wrong card either; if no match is found it logs a
warning and falls back to device 0.

## Won't start: `error while loading shared libraries: libXXX.so.NN`

**Cause**: a system library was upgraded and its **soname changed** (measured examples:
libcdio `.so.19 → .so.21`, libbluray `.so.3 → .so.4`), while the already-built binary still
links the old name. This is not an application bug — it is a binary/system-library version
mismatch that affects essentially every native Linux program. The whole point of the
source-build path is to make it disappear.

**Diagnose**:

```bash
./scripts/check-deps.sh
```

- `❌ … missing: libXXX.so.NN` → that soname no longer exists on the system (fatal — rebuild
  or switch packages)
- `⚠ libXXX was replaced since the build` → the library file was upgraded in place (a rebuild
  is advisable)
- Neither → something else; when filing an issue include
  `ldd ~/.local/bin/vsr-player | grep "not found"` plus the `check-deps.sh` output

**What to do, by install method**:

| Install method | Action |
|---|---|
| **Source build** | Re-run `./scripts/build-from-source.sh` — one command rebuilds binaries that match your current system libraries. **This is the main reason the source path is recommended** |
| **standard tarball** (thin package; a self-built full variant additionally bundles Qt) | It ships only `libmpv` and the two Wayland helper modules — FFmpeg, the media stack, CUDA, Vulkan and Qt all come from your system, so a soname bump hits it directly. In order: ① download a **newer tarball** ② switch to a source build ③ temporary workaround below |

**Temporary workaround (not for the long term)**: create compatibility symlinks for the
missing soname — **only safe when the old and new libraries are ABI-compatible**:

```bash
mkdir -p ~/.local/lib/compat
ln -sf /usr/lib/libcdio.so.21 ~/.local/lib/compat/libcdio.so.19   # use the library from the error
LD_LIBRARY_PATH=~/.local/lib/compat vsr-player <video-or-directory>
```

⚠️ A soname major bump usually means the ABI changed. The feature paths this project does not
use (CDDA, Blu-ray menus) are low risk, but **do not treat this as a permanent fix** — the
real fix is a rebuild (source path) or a newer package.

**Don't**: downgrade a single library on a rolling-release distro (partial upgrade) — it
creates more mismatches than it solves.

## Upscaling has no effect (playback looks fine, but nothing is upscaled)

When the VFX runtime is missing or fails to load, `vf_vsr` **passes through silently** (video
still plays, no error) — so "it looks fine" does not mean VSR is working. Check:

- The startup log for `VSR: nvVFX libraries loaded`; if it is absent, VFX never loaded
- `ls ~/.local/lib/vsr-player/libNVCVImage.so` and whether the **unversioned** symlinks
  (`libnppc.so`, `libcudnn.so`, …) are present — `vsr_proc.c` dlopens unversioned names, and
  a missing symlink breaks the whole VFX chain
- Re-fetch: run `./scripts/install.sh` (downloads from NVIDIA's official index), or place the
  files manually per [third-party setup](https://github.com/zhangmq/vsr-player/blob/master/docs/third-party-setup.md)
- An outdated driver also makes VFX fail to load: the **VFX version ↔ driver** pair must match
  (see [Installation](Installation))

## Frame interpolation inactive

Which backend a build uses is visible in the startup filter status line: `vfg-status:` for
VFG (default), `fruc-status:` for RIFE. The default **VFG** backend needs Ada/Blackwell; on
older GPUs the filter stays inactive.

**RIFE backend only** (`--fruc-backend=rife` source builds): a RIFE engine embeds the **exact
TensorRT version that built it**; after a TRT upgrade the old engine is refused at
deserialization (interpolation silently degrades to passthrough). Rebuild it with your
current system TRT:

```bash
bash tests/fruc/build_rife_full_engine.sh
```

## First run appears frozen for 10–15 s (engine kernel cache)

The **first** run in a HOME that has never run CUDA compute has to compile the
super-resolution engine kernels, so the picture takes 10–15 s to appear. The startup log says
so:

```
first run on this HOME: engine kernel cache is empty (<path>) — the first super-resolution
engine load compiles kernels and can take 10-15 s (one-time; later loads ~0.15 s)
```

- One-time cost per HOME (`$HOME/.nv/ComputeCache`); later engine loads take about 0.15 s.
- The cache is **independent of size/multiplier**: after one warm run, every other
  scale/multiplier is fast too.
- A **reboot does not** invalidate it. Whether a **major driver upgrade** does is **not
  verified**.
- This is a notice only — there is no pre-warm: the first frame still waits 10–15 s.

## 3D / single-eye oddities

- **Picture does not fill the glasses screen**: the OSD/log line `stereo map:` reports the
  per-eye viewport (`eye=`) and the actual content size (`content=`); a smaller `content` than
  `eye` means black bars, i.e. the source aspect does not match the eye aspect.
- **Eyes are swapped**: `K` (in 3D mode) swaps them; the setting is per session.
- **Wrong packing**: set **Source layout** to the actual packing (SBS/TAB, full/half). A TAB
  source interpreted as SBS looks squashed and doubled.
- **Aspect looks wrong in 2D output**: the value in the aspect popup is the *per-eye* aspect
  while in 3D mode; press `A` or set `no` to fall back to the source aspect.
