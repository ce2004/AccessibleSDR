AccessibleSDR

A screen-reader-first radio for RTL-SDR USB dongles (made with the RTL-SDR Blog V4) and SDRplay RSP radios. Everything is spoken through NVDA, and everything works from the keyboard. Receive only: it never transmits.

It plays FM (stereo, HD Radio HD1 to HD4, station names), AM, shortwave, aircraft, weather, ham, CB, FRS and GMRS, marine, railroad and more, glides across the dial like an analog radio, and decodes digital signals out loud: P25 (including trunked systems), DMR, NXDN, D-Star and Fusion voice, pagers, weather alerts, aircraft position and messages, ship positions, wireless sensors and FT8.


Getting started

1. Download the zip from the Releases page. There are two: win-x64 for most PCs, and win-arm64 for Snapdragon (Windows on ARM) PCs.
2. Unzip the folder anywhere, for example Documents.
3. Plug in the radio and run tuner.exe.
4. The first time, it asks to install the radio's driver. Say yes, then allow the Windows permission prompt. It uses Microsoft's own WinUSB driver.
5. Press Z and type your ZIP code, so distances to aircraft and ships are measured from you.
6. In FM radio, press X to find every station near you. Tab to AM radio and press X again.

The app updates itself: when it starts, it checks for a newer version, downloads it, and restarts. Volume starts at 30 percent.


SDRplay radios (RSP1, RSP1A, RSP1B, RSP2, RSPdx, RSPduo)

1. Install the SDRplay API from sdrplay.com (Downloads, then API for Windows). It's free, and it's SDRplay's own driver.
2. Plug in the RSP and run tuner.exe. It says SDRplay radio connected.

If both an RTL-SDR and an SDRplay radio are plugged in, the RTL-SDR is used. On an RSP1A or RSP1B, the broadcast AM and FM notch filter switches on automatically whenever you're tuned outside those bands, which keeps strong local stations from overloading the radio. On a Snapdragon (ARM) PC, if SDRplay doesn't offer an ARM version of the API, use the win-x64 download of this app; Windows runs it fine.


Keys

- Up, Down: tune. Tap for a small step, hold to glide at a steady speed.
- Shift with Up or Down: fine tuning, 10 times slower.
- Control with Up or Down: fast tuning, 10 times faster.
- Page Up, Page Down: in FM and AM, next or previous channel. In other bands, next or previous preset.
- Tab, Shift Tab: next or previous category.
- M, Shift M: receiver mode: wide FM, AM, narrow FM, upper sideband, lower sideband, CW.
- Enter: on FM, look for HD Radio. On HD, Up and Down choose HD1 to HD4.
- Escape: leave HD. In Found here, back to FM.
- N: song and station information (HD or RDS).
- Space: speak the frequency.
- Left, Right: volume.
- X: Explore. Finds every active signal in this category and saves them to Found here.
- S: scan for the next signal. Press again to continue.
- Shift S: squelch: off, low, medium, high.
- Shift Enter: save this frequency to Saved.
- Delete: remove a saved frequency.
- L: lock out this frequency from scanning. Shift L in Saved clears lockouts.
- T: follow a trunked radio system from its control channel.
- D: decoders on or off. Shift D: speaking decodes on or off.
- R: read the latest decodes, older each press.
- Z: set your location by ZIP code.
- Alt F4: close.


Files it keeps

Next to tuner.exe: saved.txt (your saved frequencies), found.txt (what Explore found), lockouts.txt, location.txt and decodes.txt (a log of everything decoded). Updates never touch these.


Building from source

Windows 10 or 11 (x64 or ARM64), Visual Studio Build Tools with the C++ tools for the target (x64 and/or ARM64), git and Python. Set ARCH=x64 or ARCH=arm64 first. ARM64 is the default; x64 builds go to the bin-x64 folder.

1. fetch-sources.cmd downloads the toolchain, libraries and upstream decoders at pinned versions and applies the patches in the patches folder.
2. build-all.cmd builds everything into the bin folder (or bin-x64).
3. make-release.cmd packs the release zip.

Pushing a version tag (for example v1.0.1) makes GitHub Actions build both versions and attach them to the release, which is where the app's updater looks.

The app itself is src/tools/tuner.c. It uses librtlsdr (rtl-sdr-blog), nrsc5, dsd-neo with mbelib-neo, multimon-ng, rtl_433, AIS-catcher, acarsdec and ft8_lib, each under its own license; those are GPL, and their source plus our changes are in this repository.
