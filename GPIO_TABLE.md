# PetCam GPIO/IO Map

Board: Ingenic T31X, PCB variant `P1T_T3_V10` (decoded from the original firmware).
Thingino camera identity: SC301IoT sensor, RTL8731BU WiFi, Ethernet
(board config: `jienuo_jn107arewifi_t31x_..._eth+rtl8731bu` as base, sensor corrected to sc301iot).

Pin notation: Ingenic GPIO banks PA=0-31, PB=32-63, PC=64-95 (no PD bank on this SoC).

## Motors

| Function | Pin (dec.) | Ingenic GPIO | Status |
|---|---|---|---|
| Agitator stepper A | 46 | PB14 | live verified |
| Agitator stepper B | 40 | PB8 | live verified |
| Agitator stepper C | 10 | PA10 | live verified |
| Agitator stepper D | 11 | PA11 | live verified |
| DC dispenser enable (active-low) | 57 | PB25 | live verified |
| DC dispenser SLOW-IRQ (vendor name) | 54 | PB22 | live verified. Per original-firmware disassembly (`strnio.ko`), the handler only latches a timestamp and never touches the motor. `dispense-treat-cycle` only uses this pin for the initial "clear the cam" level-poll, not as a stop signal. |
| DC dispenser STOP-IRQ (vendor name) | 51 | PB19 | live verified. Vendor table: DCMOTOR-STOP-IRQ, confirmed via disassembly (`request_threaded_irq` in `strnio.ko`) and used live via `gpio-wait` (falling edge) as the actual stop signal. Reproducible false trigger ~80-100ms after motor start, hence the minimum-elapsed-time filter (300ms) in `dispense-treat-cycle`. |
| Pan stepper A | 45 | PB13 | confirmed (by user) |
| Pan stepper B | 41 | PB9 | confirmed |
| Pan stepper C | 42 | PB10 | confirmed |
| Pan stepper D | 43 | PB11 | confirmed |

Agitator phase pins (46/40/10/11), like pin 57, are explicitly held low at
boot via `gpio.conf`/`S05gpio` (no hardware pull; a floating pin otherwise
gets pulled into an active state by the motor driver circuit).

Control: `/usr/sbin/stepper` (half-step, 8-phase A→AB→B→BC→C→CD→D→DA),
`/usr/sbin/dispense-treat-cycle` (full dispense cycle), `/usr/sbin/gpio-wait`
(edge-triggered wait for the STOP-IRQ via `poll()`, instead of busy-polling).
Source: `package/petcam-tools/files/` in the Buildroot tree, built into the
rootfs, no longer in the fragile overlay.

**Pan (Thingino-native `motors`/`motors-daemon`, GUI joystick):** working and
survives reboot. Kernel module `motor.ko` (from the `ingenic-sdk` package,
`CONFIG_INGENIC_MOTOR=y`) has a hard dependency on `tcu_alloc.ko` (provides
`tcu_alloc_claim/release/owner` for TCU channel reservation) — `modules.dep`
must know this dependency, otherwise `insmod motor.ko` fails with "unknown
symbol". `modprobe motor ...` resolves this automatically, plain `insmod`
does NOT (load `tcu_alloc.ko` separately first). `steps_pan=20000` (user
estimate, not hardware-calibrated). Tilt axis unused (`gpio_tilt=""`,
`steps_tilt=0`) — needed a fix in `S59motor`, see
`package/thingino-motors/files/S59motor` (`set_motor_phases` otherwise
aborts the whole script on an empty `gpio_tilt`).

⚠️ **Never `rmmod motor` while `motors-daemon` is running** — causes a
kernel fault ("Fixing recursive fault but reboot is needed!"), the daemon
hangs unrecoverably in `D` state. Always stop it first
(`/etc/init.d/S59motor stop`, and verify the process is actually gone)
before unloading the module. If it happens anyway, only a plain `reboot`
helps (no reflash needed — root is OverlayFS from `squashfs`(ro) +
`jffs2`(rw), all manually pushed files survive it).

## Camera / Misc

| Function | Pin (dec.) | Ingenic GPIO | Status |
|---|---|---|---|
| LED red | 49 | PB17 | physically verified (active-high) |
| LED blue | 50 | PB18 | physically verified (active-high) |
| IR night-vision LED | 60 | PB28 | decoded from firmware |
| IR-CUT filter A | 53 | PB21 | decoded from firmware |
| IR-CUT filter B | 52 | PB20 | decoded from firmware |
| Sensor reset | 18 | PA18 | held active by Thingino |
| SD card detect | 38 | PB6 | physically verified, active-low (0=card in, 1=out). Not derivable from the vendor dump, only reliable via hardware measurement. |
| Physical button | 62 | PB30 | physically verified, active-low (0=pressed, 1=released). No internal pull needed — an external pull-up on the button board pulls it to 3.3V. Not derivable from the vendor dump. |

Also a red power mini-LED, hardwired (no GPIO, no software access).

## Button

The original firmware reads pin 62 via `strnio.ko`, board table offset
`+0x3e` (PCB variant `P1T_T3_V10`/entry 29), slot 1 of the
`pps_board_get_io_events` function (up to 8 configurable digital inputs,
4×1ms debounced).

## ⚠️ Dangerous pin

**Pin 39 (PB7) — NEVER export/touch as GPIO.** Exporting it as input
hangs/crashes the device instantly, with no auto-recovery — needs a manual
power cycle. Likely an active SPI-flash control line (chip-select/clock/WP#),
not a normal GPIO.

## WiFi / SD power

- WiFi: no switching GPIO on this board — `pin=0` across the entire vendor
  board table. The `wlanPwr` ioctl exists generically in `strnio.ko`, but
  its handler doesn't call any GPIO function (a software no-op on this
  variant).
- SD card power: handled in the original firmware via a kernel ioctl
  (`0x80044d0e`) on a char device, not via sysfs GPIO.

## Reverse-engineering tools

Scripts to reproduce/extend the decoding live in
`~/Documents/Projects/petcam/fw_orig/`:
- `dump_entries2.py` — lists all 34 PCB variants from `strnio.ko` with their GPIO values
- `find_gpio_calls.py`, `disasm_func.py` — MIPS disassembly helpers (capstone/pyelftools)
