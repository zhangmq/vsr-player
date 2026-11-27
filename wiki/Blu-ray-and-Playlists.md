# Blu-ray and Playlists

## Blu-ray discs

A `.iso` file or a BDMV directory is **one playlist entry** — one mpv stream. The disc's
titles are **editions** of that stream, not separate entries:

- Right-click → **Title** lists them as `Title 2 · 1:48:26` (index and duration), plus a
  synthetic **Disc menu** entry when the disc has menus.
- **Boot into disc menu** (Preferences → Disc, on by default) decides whether opening the
  disc enters its own menu or starts the longest title directly.
- Picking a title or a chapter does not reload the file: mpv jumps inside the same stream,
  so the filter chain and the AI engines stay warm.
- Chapters live in right-click → **Chapter** and are read from the title that is playing.
- AACS-protected discs are enumerated but cannot be decrypted without keys; the player
  reports what libbluray says instead of failing silently.
- Both a mounted disc directory and a plain `.iso` file work; the image does not have to be
  mounted.
- A disc can be listed in a hand-written `.m3u` as a **file path**. `bd://mpls/...` URLs
  inside a local playlist are rejected by mpv (its origin check), so don't write them by
  hand.

**3D Blu-ray (MVC) is not supported** — it would need software decoding of two dependent
views and changes the whole GPU pipeline. Convert the disc to full-width SBS offline first,
then play that with "3D glasses · side by side · full". The conversion scripts are public:
<https://gist.github.com/zhangmq/cb457cb5263f2101b7adf36d81b46e31>.

## Disc menus

While a menu is on screen the control bar hides and input is forwarded to the disc:

| Input | Action |
|---|---|
| Arrow keys · `Enter` · `Esc` | Navigate · activate · back (mpv's `discnav`) |
| Mouse move / click | Mapped onto buttons by the disc itself |
| `Ctrl+M` | Root menu |
| `Ctrl+Shift+M` | Pop-up menu |

The menu picture is the disc's own graphics, composited by mpv into the video frame, so it
shows up in screenshots and in the normal render path.

## BD-J discs: what works and what does not

BD-J discs run their menu as a **Java applet** instead of HDMV bytecode. libbluray needs a
Java VM plus its own BD-J jar (`libbluray-j2se-<version>.jar`) for that; the player does not
ship one. When either is missing, mpv logs a warning and plays the disc **without menus** —
its titles are still listed in the Title panel.

Where the applet does run, three limitations are worth knowing. All of them are upstream mpv
behaviour, not player bugs:

- **The Title panel can stay on `Disc menu`.** A Java menu normally starts playback from its
  own copy of a playlist — e.g. `01001.mpls` next to the disc's `00001.mpls`, identical
  except for the playlist number — and those copies are not in the disc index's title list.
  libbluray then has no title to report, and mpv deliberately points the edition at the
  synthetic `Disc menu` entry ("the disc is playing something the title list does not
  contain"). The picture is fine; only the current-title label is unknown. Chapters keep
  working, because they come from the playlist that is actually playing.
- **Menu calls can be refused.** A disc can mask the "menu call" user operation; `Ctrl+M`
  and the Title → `Disc menu` row then do nothing at all. Some discs mask the pop-up menu as
  well, so there may be no way back to the root menu until the disc is reopened.
- **The applet decides the flow.** Chapter selection, feature start and "return to menu"
  run inside the Java menu; the player only forwards keys/mouse and draws the overlay. What
  the disc does not offer cannot be reached from the player.

If a disc's menu is unusable, turn Preferences → Disc → *Boot into disc menu* off: the disc
then starts its longest title directly, and the Title panel can still jump to any title
(mpv's `bd_select_title`, which the disc cannot mask).

## Playlist persistence

The playlist is stored as a standard **m3u8** file:

```
~/.local/share/vsr-player/last-playlist.m3u8
```

```m3u
#EXTM3U
#VSR-CURRENT:1
#EXTINF:-1,disc.iso
/home/user/disc.iso
#EXTINF:-1,film.mkv
/home/user/films/film.mkv
```

- The file is rewritten atomically (`QSaveFile`) every time the playlist changes.
- `#EXTINF` carries the entry name, which is how names survive a restart: playback is
  restored through mpv's own playlist parser (`loadlist`), so the entry titles come from the
  m3u file exactly like they come from any other playlist — the player keeps no name table
  of its own.
- A disc is a single entry, so nothing per-title is stored: its titles come back with the
  disc itself.
- `#VSR-CURRENT` is a custom comment (mpv skips it) holding the index of the entry that was
  selected, so restarting with no arguments highlights the right row.

Launching without arguments restores this list **without loading any media** — no decode, no
engine work, no VRAM use — until you press Play.

The older `playlist` / `playlistCurrent` keys in `~/.config/vsr-player/vsr-player.conf` are
only read as a fallback when the m3u8 file does not exist (upgrades from older versions);
they are no longer written.

## Removing and deleting entries

| Action | Key | Menu | Touches disk |
|---|---|---|---|
| Remove current entry from the playlist | `Delete` | Remove from playlist | no |
| Delete the current file | `Shift+Delete` | Delete file | yes — moves it to the trash (`gio trash`) |

Both then advance exactly as mpv's `playlist-remove` does: the next entry becomes current
(and keeps the pause state), or the player goes idle when nothing is left.

Deleting is only offered for real, writable local files — URLs and streams have no file to
remove. `gio trash` refuses files on tmpfs; in that case the playlist is left untouched and
an error is logged.
