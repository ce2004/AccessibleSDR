# Working on AccessibleSDR (guide for AI agents and contributors)

## What this is
A receive-only, screen-reader-first SDR app for Windows (x64 and ARM64), built for NVDA users: every action must be keyboard-driven and spoken; nothing may depend on seeing the screen. It supports RTL-SDR dongles (librtlsdr, rtl-sdr-blog fork) and SDRplay RSPs (SDRplay API, loaded at run time).

## Layout
- `src/tools/tuner.c`: **the whole app**, a single Win32 C file. Sections in order: presets and categories, speech (NVDA controller client), sound effects (synthesised chimes), audio output (waveOut), HD Radio (libnrsc5), wide-FM stereo plus RDS, narrow modes (AM, NFM, SSB, CW), spectrum, squelch and scanner, Explore (channel-map survey), decoder children (multimon-ng, dsd-neo, rtl_433, AIS-catcher, acarsdec, decode_ft8), ADS-B (in-app), trunk following (rtl_tcp server on 1234 for dsd-neo), driver installer, updater, the radio layer (`rad_*` wrappers, RTL and SDRplay), the window and keys, and WinMain.
- `src/tools/{wfm,play,iqcap}.c`: small developer tools.
- `src/aoshim/`: a libao replacement so the nrsc5 CLI builds on Windows.
- `patches/<project>.patch` plus `patches/sources.txt`: our changes to upstream projects and the exact commits they apply to.
- `presets.txt` (universal channel plans, no city-specific data), `zipcodes.txt` and `counties.txt` (US Census) ship with the app.
- `env.cmd`: the shared build settings (`ARCH=arm64` (default) or `x64`). `build-*.cmd`: one per component. `build-all.cmd`, `make-release.cmd`.
- `.github/workflows/release.yml`: a tag `v*` builds both architectures and attaches the zips to the release.

## Build
1. `fetch-sources.cmd`: clones upstream projects into `src/<name>` at pinned commits and applies the patches, downloads llvm-mingw and the SDRplay headers, and builds the vcpkg libraries.
2. `set ARCH=x64` (or arm64), then `build-all.cmd`. Output goes to `bin\` (arm64) or `bin-x64\`.
3. `make-release.cmd` writes `release\AccessibleSDR-<ver>-win-<arch>.zip`.

After editing anything under `src/<upstream>/`, run `python tools\export-patches.py` and commit the updated patch. Never commit the upstream clones or the SDRplay headers (their licence forbids it).

## Test switches (tuner.exe)
Test runs start minimized, so they never take the keyboard from someone using the app. Their logs go next to `presets.txt`.
- `--listen <MHz> <WFM|AM|NFM|USB|LSB|CW> <seconds>`: tune and listen. Writes `rds_test.log` and `glide_test.log` (audio underruns and drops).
- `--glide <MHz> <mode> <dir> <seconds>`: simulate holding an arrow key.
- `--explore <MHz> <seconds>`: press X in that frequency's category. Results go to `found.txt`.
- `--scantest <MHz> <stops> <squelch>`, `--trunk <MHz> <seconds> [dmr colour code]`.
- `--sounds` (play every sound effect), `--zip <text>`, `--driver-check`, `--install-driver` (elevated).
- Set `SDR_NO_UPDATE=1` to skip the update check.

## Project rules (keep these)
- **Receive only.** Nothing may transmit.
- **Every test must be audible** on the default audio device. No silent decodes to files. Volume defaults to **30%**.
- **The listener always chooses:** never auto-switch stereo to mono or analog to HD. Announce what's available instead.
- **Holding an arrow key glides at one constant speed** (Ctrl is 10x, Shift is 0.1x). No acceleration. It must sound analog: no gaps or lag.
- **X (Explore) must be very fast** and use real channel maps (FM 87.9-107.9 every 200 kHz, AM 540-1700 every 10 kHz, or a category's official channels). It must never pick bleed, mirror images or random noise.
- **Nothing location-specific** in shipped data. Location comes from Z (ZIP code) or Windows Location.
- **Sound effects use the soft chime** voice (`g_sfx_style = 2`).
- Status and messages are spoken through NVDA with `say()`. Keep wording short and plain.

## Releasing
Bump `TUNER_VERSION` in `tuner.c`, commit, then tag and push (`git tag v1.2.3 && git push --tags`). CI builds both zips. Installed copies update themselves on their next start.
