# Blu-ray and Playlists

## Blu-ray ISO (2D titles)

A `.iso` (or a BDMV directory) is treated like a folder or an m3u container: mpv expands it
into its titles and the player lists them, but **nothing is played automatically** — you pick
the title you want.

- The expansion happens inside mpv's playlist demuxer (`parse_bluray`), so the playlist
  entries are mpv's own `bd://mpls/<n>//<image>` URLs with a title name attached.
- Entries show as `Title 6 · 1:18:30` — the index plus the duration.
- AACS-protected discs are enumerated but cannot be decrypted without keys; the player
  reports what libbluray says instead of failing silently.
- Both a mounted disc directory and a plain `.iso` file work; the image does not have to be
  mounted.

**3D Blu-ray (MVC) is not supported** — it would need software decoding of two dependent
views and changes the whole GPU pipeline. Convert the disc to full-width SBS offline first,
then play that with "3D glasses · side by side · full". The conversion scripts are public:
<https://gist.github.com/zhangmq/cb457cb5263f2101b7adf36d81b46e31> (also described in
[docs/handover-3d-conversion.md](https://github.com/zhangmq/vsr-player/blob/master/docs/handover-3d-conversion.md)).

## Playlist persistence

The playlist is stored as a standard **m3u8** file:

```
~/.local/share/vsr-player/last-playlist.m3u8
```

```m3u
#EXTM3U
#VSR-CURRENT:3
#EXTINF:-1,Title 1 · 0:00:00
bd://mpls/00004//home/user/disc.iso
#EXTINF:-1,Title 6 · 1:18:30
bd://mpls/00003//home/user/disc.iso
/home/user/films/other.mkv
```

- The file is rewritten atomically (`QSaveFile`) every time the playlist changes.
- `#EXTINF` carries the entry name, which is how names survive a restart: playback is
  restored through mpv's own playlist parser (`loadlist`), so the entry titles come from the
  m3u file exactly like they come from an ISO expansion — the player keeps no name table of
  its own.
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
