# Usage

## Playing something

```bash
vsr-player video.mkv                  # one file
vsr-player ~/Videos                   # directory → whole playlist (mpv expands it)
vsr-player a.mkv b.mkv                # positional args are playlist entries, like mpv
vsr-player --playlist list.m3u        # playlist file (`-` reads from stdin)
vsr-player                            # restore the last playlist, load nothing
```

**Directory loading is mpv's own expansion** (`--directory-mode`, default `lazy`): top-level
media files become entries, subdirectories stay as entries and expand when you click them.

**Launching with no arguments stays idle.** The playlist is restored (names included) but no
media is loaded — no decode, no VSR/FRUC engine work, no VRAM use. Pressing Play loads the
current entry and resumes at the last saved position through mpv's watch-later mechanism.
Passing a file on the command line — or an explicit `-- --pause=yes` — loads immediately,
following mpv semantics.

## On-screen controls

The UI auto-hides; it appears when the pointer enters the bottom hover zone (progress bar
plus a margin below it) and hides again when it leaves.

- **Bottom bar** — progress/seek, play/pause, previous/next, volume, quality (scale, VSR
  quality, denoise, interpolation, SDR→HDR10), hardware/software decode toggle, speed,
  aspect ratio, loop mode, fullscreen.
- **Right-click menu** — open file/folder, append to playlist, remove/delete the current
  entry, **Picture processing** (deinterlace, rotation), **3D**, quit. The 3D block is
  described on [3D and Single-Eye](3D-and-Single-Eye).
- **Playlist panel** (`P`) — current entry is centred when the panel opens; drag files onto
  the panel to append them without interrupting playback.
- **OSD** (`Tab`) — source/output resolution, the **actual** output mode (HDR10 or SDR),
  content HDR state, VSR factor and cost, interpolation status, decoder, speed, GPU.
- **Aspect ratio** (`A` / `Shift+A`, or the bottom-bar popup) — cycles the same list mpv
  ships with: `16:9` → `4:3` → `2.35:1` → `no`. Use it when a source has a wrong or missing
  display aspect; in 3D mode the value is interpreted as **per-eye** aspect.

## Keyboard shortcuts

| Key | Action |
|-----|--------|
| `Space` | Play / pause |
| `←` / `→` | Seek ±5 s (`Shift` + arrow = ±10 s) |
| `↑` / `↓` | Volume ±5 % |
| `Ctrl+←` / `Ctrl+→` | Previous / next playlist entry |
| `A` / `Shift+A` | Cycle aspect ratio forward / backward |
| `S` | Screenshot |
| `Tab` | Toggle OSD |
| `N` / `B` | Next / previous file |
| `[` / `]` / `\` | Speed 0.5× / 2× / 1× |
| `P` | Toggle playlist panel |
| `F` | Toggle fullscreen |
| `Esc` | Exit fullscreen / close the playlist |
| `Delete` / `Shift+Delete` | Remove current entry from the playlist / move the file to trash |
| `H` | Toggle the hotkey help overlay (3D presentation) |
| `J` / `K` / `L` | 3D or single-eye mode: layout · eye order (or eye) · full/half |

## Command-line options

| Option | Values | Default | Description |
|--------|--------|---------|-------------|
| `--scale` | `off`, `auto`, `2`, `3`, `4` | `auto` | Super-resolution scale |
| `--quality` | `low`, `medium`, `high`, `ultra` | `high` | Upscale quality |
| `--denoise` | `off`, `low`, `medium`, `high`, `ultra` | `off` | Denoise quality (applied at scale=1) |
| `--fruc` | `off`, `2`, `3`, `4` (default VFG backend); `off`, `40`, `48`, `60`, `2`, `3`, `4` (RIFE source build) | persisted | Frame interpolation: integer multiplier (VFG) or target fps 40/48/60 (RIFE) |
| `--fruc-quality` | `low`, `medium`, `high` | `medium` | VFG model quality (hot-switchable; ignored by RIFE) |
| `--fruc-order` | `pre`, `post` | `post` | VFG chain order: `post` = after VSR, `pre` = before VSR (RIFE is fixed) |
| `--fruc-max-fps` | `48`, `60`, `120` | `60` | Output frame-rate cap: source fps × multiplier above it → passthrough (ignored by RIFE) |
| `--no-hwaccel` | — | — | Disable NVDEC, use software decode |
| `--lang` | e.g. `en`, `zh_CN` | system locale | UI language |
| `--benchmark` | — | — | Headless throughput measurement (no UI, `all=no` logging) |
| `--vsync` | — | off | FIFO present (non-blocking by default; Wayland has no tearing) |
| `--no-rpc` | — | — | Disable the JSON IPC server |
| `--playlist` | `<file>`, `-` | — | Playlist file (m3u/plain text; blank lines and `#` comments skipped) |
| `--` | — | — | Everything after it is passed to mpv as options |

Options the app owns (`vo`, `vf`, `hwdec`, `msg-level`) must not be passed after `--`.

## JSON IPC

A Unix socket at `/tmp/vsr-player.sock`, one JSON object per line:

```bash
printf '{"command":["play"]}\n' | socat - UNIX-CONNECT:/tmp/vsr-player.sock
```

Commands: `play`, `pause`, `stop`, `seek`, `loadfile`, `set-vsr`, `get-vsr`, `quit`, plus a
raw `command` passthrough. Debug/inspection helpers used during development include
`get-playlist` (mpv-side `title`/`filename` plus the panel's display names),
`get-video-size` (post-filter frame size), `get-source-format`, `set-eye`, `set-rotate`,
`get-stereo`, `grab-window <path>` (writes a PNG of the player window) and `vf-command`.

## Standalone `mpv-vsr` CLI

The patched mpv binary (with `vf_hwup` + `vf_vsr` + `vf_truehdr` + `vf_vfg`, plus `vf_rife`
in RIFE builds) is shipped as `mpv-vsr`, usable as a plain mpv replacement:

```bash
mpv-vsr --hwdec=auto --vf=hwup,vsr:scale=2,truehdr:enable=yes:luminance=650,vfg:fps=2:mode=medium video.mkv
```

Chain order (left to right): `hwup` (SW→CUDA upload, enables the soft-decode path) → `vsr`
(upscaling) → `truehdr` (SDR→HDR10, off by default) → `vfg` (interpolation). Put `vfg`
before `vsr` for the `pre` order; RIFE builds use `hwup` → `rife` → `vsr` → `truehdr`. Use
`--hwdec=nvdec` for hardware decode (then `hwup` is a no-op passthrough).

| Option | Values | Description |
|--------|--------|-------------|
| `scale` (vsr) | `off`, `auto`, `2`, `3`, `4`, ratio (e.g. `4/3`) | Upscale factor; `auto` picks by viewport |
| `quality` (vsr) | `low`, `medium`, `high`, `ultra` | VSR inference quality |
| `denoise` (vsr) | `off`, `low`, `medium`, `high`, `ultra` | Denoise pass (works at scale=1) |
| `stereo-layout` / `stereo-half` (vsr) | `-1|0|1`, `0|1` | Per-eye scaling for 3D sources (see [3D and Single-Eye](3D-and-Single-Eye)) |
| `enable` (truehdr) | `yes`, `no` | SDR→HDR10 upconversion (HDR frames pass through) |
| `luminance` (truehdr) | 400…2000 | Target HDR peak in nits (default 650; outside the range the SDK silently ignores it) |
| `saturation` / `contrast` (truehdr) | 0…200 | Default 100 |
| `middle-gray` (truehdr) | 10…100 | Default 50 |
| `debanding-off` (truehdr) | `0`, `1` | `1` skips the debanding pass (diagnostic only; default `0` = on) |
| `fps` (vfg) | `off`, `2`, `3`, `4` | Interpolation multiplier |
| `mode` (vfg) | `low`, `medium`, `high` | VFG model quality (default `medium`) |
| `mode` / `layout` / `half` (eye) | `off|left|right`, `sbs|tab`, `0|1` | Single-eye extraction (see [3D and Single-Eye](3D-and-Single-Eye)) |
| `fps` (rife) | `off`, `auto`, integer 1..120 | Interpolation target fps (RIFE builds only); `auto` = 2×source |
| `scale` (rife) | `off`, `2`, `3`, `4` | Benchmark multiplier (measures the device ceiling; RIFE builds only) |

Everything except the RIFE options can also be changed at runtime through `vf-command`:

```bash
printf '{"command":["command","vf-command","truehdr","luminance","1000"]}\n' | socat - UNIX-CONNECT:/tmp/vsr-player.sock
```

`mpv-vsr-wrapper.py` (browser integration, ff2mpv-style) exports Chrome cookies, extracts
playlists via yt-dlp (YouTube/Bilibili/Niconico) and launches `mpv-vsr`. Personal-use tool,
not shipped in the release tarball.
