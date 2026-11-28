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

- **Bottom bar** — progress/seek, play/pause, previous/next, stop, volume, quality (scale,
  VSR quality, denoise, interpolation, SDR→HDR10), hardware/software decode toggle, speed,
  aspect ratio, loop mode, fullscreen, playlist.
- **Progress bar hover** — a thumbnail of that position appears above the pointer, with the
  time on a black strip at its bottom and, when the source has chapters, the chapter name on
  a half-transparent strip at the top. It works for local files and for network streams: the
  player reuses the address and request headers it has already resolved, so hovering does
  not run a second extraction (the first networked thumbnail is fetched on the first hover).
  If no thumbnail can be produced, the bubble falls back to the time plus "Thumbnail
  unavailable" and the next hover retries after a short cooldown. The panel is 192 logical
  pixels wide and rendered at the display scale, so it stays sharp on HiDPI screens. Switch
  it off in the menu (**Interface → Thumbnails**); hovering then shows the time only.
- **Right-click menu** (`Menu`, `Shift+F10` or `F10`) — groups, in this order: **File**
  (open file / folder / URL, delete the current file), **Playlist** (add file / folder,
  remove the current entry), **Preferences → Interface** (thumbnails, hide unavailable menu
  items, show the 3D menu block), **Tracks** (video track and network stream, audio, subtitles including loading an
  external file, subtitle delay and audio delay), **Picture processing** (deinterlace,
  rotation), **3D**, **About**, **Quit**. The 3D block is described on
  [3D and Single-Eye](3D-and-Single-Eye); **About** shows the icon, program name, version,
  licence and a clickable repository link. Keyboard: `↑`/`↓` move (skipping separators and
  group headers, wrapping around), `Home`/`End` jump to the first/last entry, `→` opens a
  submenu, `←` closes it, `Enter` activates, `Esc` steps back (delay control → submenu →
  menu). When the menu is taller than the window it scrolls (▲/▼ triangles, the mouse wheel,
  or keyboard navigation which keeps the current row visible) instead of being cropped.
- **Playlist panel** (`P`) — current entry is centred when the panel opens; drag files onto
  the panel to append them without interrupting playback.
- **OSD** (`Tab`, or `h` in 2D) — `Source` (resolution/codec/fps/frames plus the
  content-side HDR state), `Output` (viewport, render fps and the **actual** output mode:
  `HDR10 · BT.2020/PQ · display 380nit`), `VSR`, `TrueHDR`, `FRUC`, decoder, speed, GPU.
  The three VFX rows read `<setting> | <what the filter reports>`: `mode=active cost=…` while
  the stage runs (VSR also reports the effective scale and fps; interpolation the source and
  output rate), `mode=passthrough reason=…` when it deliberately idles (e.g. `reason=max-fps`),
  and just `off` when the stage is switched off.
- **Aspect ratio** (`A` / `Shift+A`, or the bottom-bar popup) — cycles the same list mpv
  ships with: `16:9` → `4:3` → `2.35:1` → `no`. Use it when a source has a wrong or missing
  display aspect; in 3D mode the value is interpreted as **per-eye** aspect.

## Keyboard shortcuts

| Key | Action |
|-----|--------|
| `Space` | Play / pause |
| `←` / `→` | Seek ∓5 s (`Shift` + arrow = ±10 s) |
| `↑` / `↓` | Volume ±5 % |
| `Ctrl+←` / `Ctrl+→` | Previous / next playlist entry |
| `,` / `.` | Step one frame back / forward |
| `A` / `Shift+A` | Cycle aspect ratio forward / backward |
| `V` | Subtitles on / off |
| `z` / `Shift+Z` | Subtitle delay −50 / +50 ms |
| `Ctrl+Alt+Z` | Reset subtitle delay (`Ctrl+0` is now the upscaling "auto") |
| `Ctrl++` / `Ctrl+-` | Audio delay +50 / −50 ms |
| `Ctrl+Alt+0` | Reset audio delay |
| `S` | Screenshot |
| `Tab` (2D: also `h`) | Toggle the OSD overlay |
| `P` | Toggle the playlist panel |
| `F` | Toggle fullscreen |
| `[` / `]` / `\` | Speed 0.5× / 2× / 1× |
| `Delete` / `Shift+Delete` | Remove the current entry from the playlist / move the file to trash |
| `Ctrl+O` / `Ctrl+Shift+O` / `Ctrl+L` | Open file / folder / URL |
| `Menu` / `Shift+F10` / `F10` | Open the right-click menu |
| `Ctrl+1/2/3/4` / `Ctrl+0` | Upscaling: off / 2× / 3× / 4× / auto |
| `Ctrl+Shift+1/2/3/4` / `Ctrl+Shift+0` | Interpolation: off / 2× / 3× / 4× / cycle the output cap (48→60→120) |
| `Ctrl+H` | SDR→HDR10 upconversion on / off |
| `Ctrl+Q` | Quit |
| `Esc` | Leave fullscreen / close the playlist (it does not stop playback) |
| `H` | Toggle the hotkey help overlay (3D presentation) |
| `J` / `K` / `L` | 3D or single-eye mode: layout · eye order (or eye) · full/half |

While the right-click menu is open, the arrow keys, `Home`/`End`, `Enter` and `Esc` drive the
menu instead of playback; the subtitle/audio delay keys keep working so a delay can be tuned
while watching the result. In a 3D session `Esc` is deliberately ignored on the glasses
(fullscreen would move the window back to the focused desktop); leave 3D from the menu.

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

Commands, grouped the way they are used:

- **Playback** — `play`, `pause`, `stop`, `seek`, `open <path>`, `fullscreen`,
  `resize <w> <h>`, `playlist-play`, `clear-playlist`, `remove-current`, `delete-current`,
  `quit`.
- **Processing** — `set-vsr` / `get-vsr`, `set-fruc`, `set-fruc-quality`,
  `set-fruc-max-fps`, `set-deinterlace` / `get-deinterlace`, `set-rotate` / `get-rotate`,
  and `vf-command <filter> <cmd> [arg]` to poke a filter at runtime.
- **Inspection** — `get-playlist` (mpv-side `title`/`filename` plus the panel's display
  names), `get-video-size` (post-filter frame size), `get-source-format`, `get-media-phase`,
  `get-chapters`, `get-stereo`, `get-eye`, `get-screens`, `get-menu-state`,
  `get-control-state`, `get-focus`, `get-keys`, `grab-window <path>` (writes a PNG of the
  player window).
- **Thumbnails** — `preview <out.png> <seconds>` (renders one hover thumbnail and waits for
  it), `get-preview-state`, `get-preview-width`, `set-preview-width`,
  `set-preview-thumbnails`.
- **UI test hooks** — `toggle-osd`, `open-menu`, `menu-key`, `key`, `key-log`,
  `clear-key-log`, `debug-hover`, `warp-pointer`, `nudge-volume`, `set-menu-hide-invalid`.
- **Generic** — `get_property` / `set_property`, and `command` for a raw mpv command
  (`{"command":["command","loadfile","<path>","replace"]}`).

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
