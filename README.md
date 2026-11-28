# ani3ds

ani-cli for the **New 3DS**: search anime, pick an episode and stream it, with subtitles, entirely on the console (no PC or server needed). Video is decoded by the New 3DS hardware decoder (MVD) and scaled to the screen by the GPU, audio is decoded by ffmpeg on the CPU and played through the DSP.

Uses the same source as ani-cli 5.2 (miruro).

## Features
- Search, episode lists, watch history ("continue")
- Sub / dub, subtitles (VTT/SRT/ASS) drawn on the video
- HLS streaming up to 720p (pick 480p or 720p as the maximum), 25 fps, also with a separate audio track
- Clean picture: the GPU scales the video to the screen with smoothing (no line patterns in dark scenes)
- Picture and sound stay in sync: if the decoder can't keep up (720p with very detailed 3D animation), it leaves out single frames that nothing else depends on instead of falling behind
- Checks the servers of an episode and plays one that has a stream up to the chosen maximum; if it fails, the next one (continuing at the same spot)
- Closing the lid pauses playback (buttons are ignored while it is closed); press A to continue after opening. If the console sleeps, playback reopens at the same spot after waking up
- Uses the free RAM as buffer (about 65 MB: minutes of read-ahead, and jumps back or forward inside it play without downloading again)
- Settings by touch on the bottom screen: sub/dub, subtitles, max quality, top screen 400 or 800 px wide, bottom screen off
- 800 px mode: the New 3DS wide mode, twice the pixels across
- Episode menu after playback: next, replay, previous, choose episode
- Bottom screen: playback status and a frame counter (frames drawn / new pictures / decoded per second)

## Controls
**Home:** A continue, X search, Y remove from history; settings: tap them on the bottom screen

**Playback:** A pause, B stop, Left/Right 10 s, L/R 1 min, Y subtitles, X next episode; tap subtitles / screen width (reopens at the same spot) / bottom screen

**Everywhere:** SELECT turns the bottom screen off and on

## Requirements
- New 3DS / New 2DS XL (the MVD hardware decoder does not exist on the old models)
- Install as **CIA** (a .3dsx cannot use the `mvd:STD` service)
- Wi-Fi with roughly 2-3 Mbit/s for 720p
- 480p plays smoothly everywhere; 720p is smooth for most anime, heavy 3D CG shows may leave out the odd frame (more so in 800 px mode)

## Install with FBI
In FBI, open **Remote Install → Scan QR Code** and scan this code. It always installs the CIA from the latest release.

<img src="3ds/fbi-qr.png" alt="QR code: latest ani3ds.cia" width="200">

Or download `ani3ds.cia` from [Releases](https://github.com/affemitwaffe091/ani3ds/releases/latest) and install it manually.

## Limits
- 1080p-only streams cannot play (the MVD system module crashes on 1080p); servers that only have 1080p are skipped
- The app refuses streams larger than 1920x720

## Files on the SD card (`sdmc:/3ds/ani3ds/`)
- `history.txt` (keyed on AniList ids like ani-cli; entries from 0.1.x are converted when you open them), `config.txt` (mode, subtitles, `maxq=`, `wide=`), `log.txt`
- `debug` (empty file): verbose log incl. frame fingerprints for testing
- `nol2` (empty file): run with the New 3DS L2 cache off

## Building
Needs devkitARM + libctru, citro2d/citro3d, the 3DS portlibs (curl, mbedtls, zlib), makerom (Project_CTR), bannertool and a CA bundle; the paths are set at the top of the `Makefile`.

ffmpeg 5.1 for the 3DS: copy `deps/ffmpeg/pbnet.c` into `libavformat/` of the ffmpeg source and run `deps/build-ffmpeg.sh` (adjust its paths). It adds a curl-backed http/https protocol (`pbnet`).

```
make            # build/ani3ds.cia and build/ani3ds.3dsx
make linux-test # host build of the scraper test (build/t)
```

## Layout
- `src/` app (UI, scraper, network, subtitles, overlay); `js.c` reads the data miruro's pages embed
- `vp/` video player (ffmpeg demux, MVD video, GPU display with citro2d, NDSP audio, A/V sync)
- `3ds/` RSF, icon, banner
- `test/` test and measurement scripts (fingerprint comparison against a PC decode)
- `dev/patches/` the scripted edits made during development (history only)

## Credits
- [ani-cli](https://github.com/pystardust/ani-cli) for the scraping flow
- [FourthTube](https://github.com/erievs/FourthTube) for the MVD setup and RSF
- [Video player for 3DS](https://github.com/Core-2-Extreme/Video_player_for_3DS) (Core-2-Extreme) for the MVD output handling that makes H.264 with B-frames play smoothly, and the GPU display (full-size decode, scaled by the GPU)

## License
ani3ds is free software: you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, either version 3 of the License, or (at your option) any later version. See [LICENSE](LICENSE).

It builds on GPL-3.0 projects (ani-cli, FourthTube, Video player for 3DS), so changed versions and forks have to stay under the GPL too, with their source available.

The CIA includes these libraries under their own licenses:
- [FFmpeg](https://ffmpeg.org) 5.1 (LGPL 2.1 or later), built without GPL-only parts; `deps/` has the build script and the added `pbnet` protocol
- [libctru](https://github.com/devkitPro/libctru) (zlib License)
- [curl](https://curl.se) (curl License)
- [Mbed TLS](https://github.com/Mbed-TLS/mbedtls) (Apache 2.0)
- [zlib](https://zlib.net) (zlib License)
- the CA certificate bundle from [NetSurf](https://www.netsurf-browser.org), based on Mozilla's CA list (MPL 2.0)
