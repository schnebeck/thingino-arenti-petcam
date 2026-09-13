# thingino-arenti-petcam

Decloudification of an Arenti PetCam (Ingenic T31X, SC301IoT sensor,
RTL8731BU WiFi — Jienuo JN-107AR-E-WIFI reference design) using
[thingino-firmware](https://thingino.com/): the vendor's cloud-dependent
stock firmware is replaced with an open, self-hosted firmware image that
never needs the manufacturer's servers, while keeping every feature that
made the hardware worth buying — PTZ, treat dispenser, two-way audio,
motion/audio alerts — and adding a few the stock firmware didn't have.

## What's here

This repo covers the whole project, not just the firmware:

- **`firmware/`** — a git submodule pointing at
  [schnebeck/thingino-firmware](https://github.com/schnebeck/thingino-firmware)
  (fork of [themactep/thingino-firmware](https://github.com/themactep/thingino-firmware),
  branch `arenti_petcam`). This is where the actual camera profile,
  custom packages, and build system live.
- **`GPIO_TABLE.md`**, **`fw_orig/`** — GPIO pin mapping and other
  findings from reverse-engineering the vendor's stock firmware (our own
  analysis scripts and notes only — the vendor's own binaries and
  datasheet are not redistributed here, see Licensing below).
- **`stepper/`**, **`audio-alarm/`** — standalone prototyping work for
  the pan motor control and audio-alarm detection that later became the
  `petcam-tools` / `petcam-audio-alarm` packages in `firmware/`.
- **`avanti_cam_manager.py`**, **`onvif_test.py`** — small Python tools
  for talking to the camera directly (ONVIF PTZ control, RTSP streaming)
  independent of the firmware itself.

## What was built

On top of stock thingino, this camera profile (`arenti_petcam`) adds:

- **PTZ pan control**, calibrated for this specific gearbox/mount
  (step count, home position) via `petcam-tools`' motor scripts.
- **Treat dispenser** control (`dispense-treat-cycle`, stepper-driven).
- **Audio-triggered recording** (`petcam-audio-alarm`): detects loud
  sounds via the mic feed and starts/extends a recording, independent of
  the motion detector.
- **Sound button playback**: upload a clip through the web UI, play it
  through the camera's speaker; non-native formats are transcoded with a
  minimal on-device `ffmpeg` build.
- **Off-site backup over VPN**: recent clips, sound uploads, and
  emergency snapshots are pushed to a VPN-reachable helper server the
  moment the connection is available — see
  `firmware/package/petcam-tools/files/backup-server-setup/README.md`
  for the server-side setup (dedicated unprivileged service account,
  works with any VPN backend and any server, not tied to this project's
  own deployment).
- **Emergency snapshot fast-path**: on the very first motion/audio
  trigger, a few high-res stills are pushed off-site immediately,
  independent of and not waiting on the video recording pipeline — the
  scenario this covers is the camera being carried off before a full
  clip could ever finish.
- **Approximate WiFi-based geolocation** (via the community-run
  [beaconDB](https://beacondb.net/) API, reached through the same VPN
  helper server so the camera itself never needs direct internet
  access), opt-in and disabled by default.
- **Automatic TLS certificate provisioning** from a small private
  internal CA over the VPN, so the web UI gets a browser-trusted
  padlock instead of a self-signed warning.
- Assorted WebRTC/UI fixes upstream didn't have yet at the time (a
  boot-time race that could leave WebRTC bound to the wrong local IP
  after a reboot with multiple network interfaces, an Opus audio payload
  type mismatch, clearer live-view naming).
- Unused feature packages (WireGuard, MQTT, Home Assistant integration)
  disabled at build time to keep the image lean.

## Relationship to thingino master

`firmware/` tracks
[themactep/thingino-firmware](https://github.com/themactep/thingino-firmware)'s
`master` directly (currently based on `fd9d04d`) with all of the above as
local commits on the `arenti_petcam` branch — not a hard fork with
diverging history. Concretely, that's:

- **Two new packages**, following thingino's own package conventions:
  [`petcam-tools`](package/petcam-tools/) (motors, treat dispenser,
  sound buttons, off-site backup, geolocation, VPN-connect detection) and
  [`petcam-audio-alarm`](package/petcam-audio-alarm/) (audio-triggered
  recording).
- **One new package override**,
  [`thingino-openvpn`](package/thingino-openvpn/): an `openvpn-override.mk`
  layering thingino-style web UI integration and config-from-JSON onto
  the stock Buildroot `openvpn` package — the same pattern thingino
  itself already uses for `thingino-wireguard-tools` over stock
  `wireguard-tools`, applied here to a package upstream doesn't wrap yet.
- **One new camera profile**,
  [`configs/cameras/arenti_petcam/`](configs/cameras/arenti_petcam/) —
  defconfig, kernel fragment, GPIO map, and default `thingino.json`
  specific to this camera's sensor/gearbox/wiring.
- **Three patches to `thingino-raptor`** (upstream's own streamer
  package, vendored via a pinned commit rather than forked): an Opus
  audio payload-type mismatch, a recording clip storage cap, and a
  boot-time race where WebRTC could bind to the wrong local IP on a
  device with more than one network interface up at boot.
- **Small, targeted edits** to a handful of other existing packages
  (`thingino-motors`, `thingino-sysupgrade`, `thingino-uhttpd`,
  `thingino-webui`, a few others) — see `git log`/`git diff
  master...arenti_petcam` in `firmware/` for the exact list; nothing
  there rewrites an existing package's own logic wholesale, only adds or
  adjusts what this build needed.
- **Two lines** added to the top-level `Config.in` to register the two
  new packages' menu entries.

## Building it yourself

```sh
git clone --recurse-submodules https://github.com/schnebeck/thingino-arenti-petcam.git
cd thingino-arenti-petcam/firmware
CAMERA=arenti_petcam make defconfig
```

**Known gotcha**: the shared `configs/fragments/core.fragment` force-enables
WireGuard regardless of the camera profile's own settings, so `make
defconfig` re-enables it every time it runs. If you don't want it (this
profile doesn't), fix it up once after `defconfig` and use
`olddefconfig` (not `defconfig`) afterward:

```sh
sed -i \
  -e 's/^BR2_PACKAGE_THINGINO_VPN_WIREGUARD=y/# BR2_PACKAGE_THINGINO_VPN_WIREGUARD is not set/' \
  -e 's/^BR2_PACKAGE_WIREGUARD_TOOLS=y/# BR2_PACKAGE_WIREGUARD_TOOLS is not set/' \
  -e 's/^BR2_PACKAGE_WIREGUARD_LINUX_COMPAT=y/# BR2_PACKAGE_WIREGUARD_LINUX_COMPAT is not set/' \
  .config
CAMERA=arenti_petcam make olddefconfig
```

Then build:

```sh
CAMERA=arenti_petcam make fast
```

The resulting image is at
`output/master/arenti_petcam-*/images/thingino-arenti_petcam.bin` — flash
it with thingino's own `sysupgrade` (pass `-x` to skip its self-update
step if you specifically want the exact script version that shipped in
this build, since it otherwise always pulls the latest version from
upstream `themactep/thingino-firmware` at invocation time regardless of
what's in the image).

## Licensing

This repo (and thingino-firmware itself) is MIT licensed — see
[`firmware/LICENSE`](https://github.com/schnebeck/thingino-firmware/blob/arenti_petcam/LICENSE).
The built firmware image is a combined work that also includes GPL-2.0
components (Linux kernel, BusyBox, OpenVPN, and others via Buildroot) —
their own licenses and source-availability obligations apply to
*distributing the built image*, independent of this repo's own license.

`fw_orig/` deliberately excludes the vendor's own firmware binaries,
extracted kernel modules, and datasheet PDF (all proprietary/copyrighted
material not ours to redistribute) — only our own analysis scripts and
notes are tracked here.
