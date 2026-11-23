# ani3ds

ani-cli for the **New 3DS**: search anime, pick an episode and stream it, with subtitles, entirely on the console (no PC or server needed). Video is decoded by the New 3DS hardware decoder (MVD), audio by ffmpeg on the CPU and played through the DSP.

## Features
- Search, episode lists, watch history ("continue")
- Sub / dub, subtitles (VTT/SRT) drawn on the video
- HLS streaming up to 720p (pick 480p or 720p as the maximum), 25 fps
- Episode menu after playback: next, replay, previous, choose episode
- Bottom screen: playback status and a frame counter (frames drawn / new pictures / decoded per second)

## Controls
**Home:** A continue, X search, Y remove from history, SELECT sub/dub, R subtitles on/off, L max quality (480p/720p)

**Playback:** A pause, B stop, Left/Right 10 s, L/R 1 min, Y subtitles, X next episode

## Requirements
- New 3DS / New 2DS XL (the MVD hardware decoder does not exist on the old models)
- Install as **CIA** (a .3dsx cannot use the `mvd:STD` service)
- Wi-Fi with roughly 2-3 Mbit/s for 720p

## Install with FBI
In FBI, open **Remote Install → Scan QR Code** and scan this code. It always installs the CIA from the latest release.

<img src="3ds/fbi-qr.png" alt="QR code: latest ani3ds.cia" width="200">

Or download `ani3ds.cia` from [Releases](https://github.com/affemitwaffe091/ani3ds/releases/latest) and install it manually.

## Limits
- 1080p-only streams cannot play (the MVD system module crashes on 1080p); on the current source many episodes after the first are 1080p-only
- The app refuses streams larger than 1920x720

## Files on the SD card (`sdmc:/3ds/ani3ds/`)
- `history.txt`, `config.txt` (mode, subtitles, `maxq=`), `log.txt`
- `debug` (empty file): verbose log incl. frame fingerprints for testing
- `nol2` (empty file): run with the New 3DS L2 cache off

## Building
Needs devkitARM + libctru, the 3DS portlibs (curl, mbedtls, zlib), makerom (Project_CTR), bannertool and a CA bundle; the paths are set at the top of the `Makefile`.

ffmpeg 5.1 for the 3DS: copy `deps/ffmpeg/pbnet.c` into `libavformat/` of the ffmpeg source and run `deps/build-ffmpeg.sh` (adjust its paths). It adds a curl-backed http/https protocol (`pbnet`).

```
make            # build/ani3ds.cia and build/ani3ds.3dsx
make linux-test # host build of the scraper test (build/t)
```

## Layout
- `src/` app (UI, scraper, network, subtitles, overlay)
- `vp/` video player (ffmpeg demux, MVD video, NDSP audio, A/V sync)
- `3ds/` RSF, icon, banner
- `test/` test and measurement scripts (fingerprint comparison against a PC decode)
- `dev/patches/` the scripted edits made during development (history only)

## Credits
- [ani-cli](https://github.com/pystardust/ani-cli) for the scraping flow
- FourthTube for the MVD setup and RSF
- [Video player for 3DS](https://github.com/Core-2-Extreme/Video_player_for_3DS) (Core-2-Extreme) for the MVD output handling that makes H.264 with B-frames play smoothly
