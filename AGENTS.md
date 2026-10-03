Working on AccessibleSDR: a guide for AI agents and contributors


What this is

A receive-only, screen-reader-first SDR app for Windows (x64 and ARM64), built for NVDA users. Every action must be keyboard-driven and spoken; nothing may depend on seeing the screen. It supports RTL-SDR dongles (librtlsdr, rtl-sdr-blog fork) and SDRplay RSPs (SDRplay API, loaded at run time).


Layout

- src/tools/tuner.c: the whole app, a single Win32 C file. Sections in order: presets and categories, speech (NVDA controller client), sound effects (synthesized chimes), audio output (waveOut), HD Radio (libnrsc5), wide FM stereo plus RDS, narrow modes (AM, NFM, SSB, CW), spectrum, squelch and scanner, Explore (channel-map survey), decoder child processes (multimon-ng, dsd-neo, rtl_433, AIS-catcher, acarsdec, decode_ft8), ADS-B (built in), trunk following (an rtl_tcp server on port 1234 for dsd-neo), the driver installer, the updater, the radio layer (rad_ wrapper functions for RTL-SDR and SDRplay), the window and keys, and WinMain.
- src/tools/wfm.c, play.c and iqcap.c: small developer tools.
- src/aoshim: a libao replacement so the nrsc5 command-line tool builds on Windows.
- patches/<project>.patch plus patches/sources.txt: our changes to upstream projects and the exact commits they apply to.
- presets.txt (universal channel plans, no city-specific data), zipcodes.txt and counties.txt (US Census) ship with the app.
- env.cmd: the shared build settings. ARCH=arm64 (the default) or ARCH=x64.
- build-<part>.cmd: one script per component. build-all.cmd builds them all; make-release.cmd packs the zip.
- .github/workflows/release.yml: pushing a tag that starts with v builds both architectures and attaches the zips to the release.


Build

1. fetch-sources.cmd clones the upstream projects into src/<name> at pinned commits and applies the patches, downloads llvm-mingw and the SDRplay headers, and builds the vcpkg libraries.
2. Set ARCH=x64 or ARCH=arm64, then run build-all.cmd. Output goes to bin (arm64) or bin-x64.
3. make-release.cmd writes release/AccessibleSDR-<version>-win-<arch>.zip.

After editing anything under src/<upstream project>, run python tools/export-patches.py and commit the updated patch. Never commit the upstream clones or the SDRplay headers (their license forbids it).


Test switches for tuner.exe

Test runs start minimized, so they never take the keyboard from someone using the app. Their logs go next to presets.txt.

- --listen <MHz> <WFM, AM, NFM, USB, LSB or CW> <seconds>: tune and listen. Writes rds_test.log and glide_test.log (audio underruns and drops).
- --glide <MHz> <mode> <direction> <seconds>: simulate holding an arrow key.
- --explore <MHz> <seconds>: press X in that frequency's category. Results go to found.txt.
- --scantest <MHz> <stops> <squelch>, and --trunk <MHz> <seconds> [DMR color code].
- --outdevs writes outdevs.log, the output devices as the O picker lists them. With SDR_TEST_OUTSWITCH=<device number>, a --listen run switches to that device 4 seconds in.
- --sounds plays every sound effect. Also --zip <text>, --driver-check, and --install-driver (needs administrator rights).
- Set the environment variable SDR_NO_UPDATE=1 to skip the update check.


Project rules (keep these)

- Receive only. Nothing may transmit.
- Every test must be audible on the default audio device. No silent decodes to files. Volume defaults to 30 percent.
- The listener always chooses: never switch stereo to mono or analog to HD automatically. Announce what's available instead.
- Holding an arrow key glides at one constant speed (Control is 10 times faster, Shift 10 times slower). No acceleration. It must sound analog: no gaps or lag.
- X (Explore) must be very fast and use real channel maps (FM 87.9 to 107.9 MHz every 200 kHz, AM 540 to 1700 kHz every 10 kHz, or a category's official channels). It must never pick bleed, mirror images or random noise.
- Nothing location-specific in shipped data. Location comes from Z (ZIP code) or Windows Location.
- Sound effects use the soft chime voice (g_sfx_style = 2).
- Documentation is plain text: no Markdown symbols (backticks, asterisks, pound signs), because screen readers read them aloud.
- Status and messages are spoken through NVDA with say(). Keep wording short and plain.


Releasing

Change TUNER_VERSION in tuner.c, commit, then tag and push: git tag v1.2.3, then git push --tags. GitHub builds both zips. Installed copies update themselves on their next start.
