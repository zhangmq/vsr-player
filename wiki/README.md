# VSR Player wiki

Source of truth for the GitHub wiki. The pages in this directory are plain Markdown;
GitHub renders `# Title` as the page title and the file name (minus `.md`) as the page
name, so `3D-and-Single-Eye.md` becomes the page **3D and Single-Eye**.

## Publishing

The wiki is a separate git repository. One-time setup on GitHub:

**Settings → Features → check "Wikis"** (a repository cannot have a wiki until this is on).

Then publish (first time — clones the wiki repo and copies every page):

```bash
git clone https://github.com/zhangmq/vsr-player.wiki.git /tmp/vsr-wiki
cp wiki/*.md /tmp/vsr-wiki/            # wiki/README.md is publishing notes, skip it if you like
cd /tmp/vsr-wiki && git add -A && git commit -m "docs: sync wiki from repository" && git push
```

Subsequent updates: repeat the `cp` + commit + push. Keep this directory as the source
so the pages stay reviewable in normal pull requests.

## Pages

| Page | Contents |
|---|---|
| `Home.md` | Overview, feature list, navigation |
| `Installation.md` | Requirements, source build, tarball, manual build, packaging, version compatibility |
| `Usage.md` | Player controls, CLI options, shortcuts, JSON IPC, `mpv-vsr` CLI |
| `Video-Processing.md` | Filter chain, VSR / VFG / TrueHDR, HDR output, per-eye scaling, portrait interpolation note |
| `3D-and-Single-Eye.md` | Stereo output modes, source layouts, single-eye 2D output, menus and hotkeys |
| `Blu-ray-and-Playlists.md` | ISO handling, title naming, playlist snapshot file |
| `Troubleshooting.md` | Multi-GPU, soname breakage, silent passthrough, first-run stall |
| `Development.md` | mpv patch overlay, build scripts, tests, design records |
