# PetCam GPIO/IO-Zuordnung

Board: Ingenic T31X, PCB-Variante `P1T_T3_V10` (aus Original-Firmware dekodiert).
Thingino-Kamera-Identität: SC301IoT Sensor, RTL8731BU WLAN, Ethernet
(Board-Config: `jienuo_jn107arewifi_t31x_..._eth+rtl8731bu` als Basis, Sensor korrigiert auf sc301iot).

Pin-Notation: Ingenic GPIO-Banken PA=0-31, PB=32-63, PC=64-95 (keine PD-Bank vorhanden).

## Motoren

| Funktion | Pin (dez.) | Ingenic-GPIO | Status |
|---|---|---|---|
| Rührwerk-Stepper (Agitator) A | 46 | PB14 | live verifiziert |
| Rührwerk-Stepper B | 40 | PB8 | live verifiziert |
| Rührwerk-Stepper C | 10 | PA10 | live verifiziert |
| Rührwerk-Stepper D | 11 | PA11 | live verifiziert |
| DC-Auswurf Enable (active-low) | 57 | PB25 | live verifiziert |
| DC-Auswurf SLOW-IRQ (Vendor-Name) | 54 | PB22 | live verifiziert. Laut Original-Firmware-Disassembly (`strnio.ko`) latcht der Handler nur einen Timestamp, steuert den Motor nicht an. `dispense-treat-cycle` nutzt den Pin nur zum initialen "Nocke freifahren" (Pegel-Polling), nicht als Stop-Signal. |
| DC-Auswurf STOP-IRQ (Vendor-Name) | 51 | PB19 | live verifiziert. Vendor-Table: DCMOTOR-STOP-IRQ, bestätigt per Disassembly (`request_threaded_irq` in `strnio.ko`) und live per `gpio-wait` (fallende Flanke) als tatsächliches Stop-Signal genutzt. Reproduzierbares Fehlsignal ~80–100ms nach Motorstart, daher Mindestlaufzeit-Filter (300ms) in `dispense-treat-cycle`. |
| Pan-Stepper A | 45 | PB13 | bestätigt (laut User) |
| Pan-Stepper B | 41 | PB9 | bestätigt |
| Pan-Stepper C | 42 | PB10 | bestätigt |
| Pan-Stepper D | 43 | PB11 | bestätigt |

Rührwerk-Phasenpins (46/40/10/11) werden wie Pin 57 über `gpio.conf`/`S05gpio`
beim Boot explizit auf low gehalten (kein Hardware-Pull, floatender Pin wird
von der Motortreiberschaltung sonst in einen aktiven Zustand gezogen).

Ansteuerung: `/usr/sbin/stepper` (Halbschritt, 8-Phasen A→AB→B→BC→C→CD→D→DA),
`/usr/sbin/dispense-treat-cycle` (kompletter Auswurf-Zyklus), `/usr/sbin/gpio-wait`
(edge-getriggertes Warten auf die STOP-IRQ per `poll()`, statt Busy-Polling).
Quellcode: `package/petcam-tools/files/` im Buildroot-Tree, fest im Rootfs, nicht
mehr im fragilen Overlay.

**Pan (Thingino-native `motors`/`motors-daemon`, GUI-Joystick):** funktionsfähig
und bootfest. Kernelmodul
`motor.ko` (aus `ingenic-sdk`-Paket, `CONFIG_INGENIC_MOTOR=y`) hat eine
harte Abhängigkeit auf `tcu_alloc.ko` (liefert `tcu_alloc_claim/release/owner`
für die TCU-Kanal-Reservierung) — `modules.dep` muss diese Abhängigkeit
kennen, sonst schlägt `insmod motor.ko` mit "unknown symbol" fehl.
`modprobe motor ...` löst das automatisch, `insmod` direkt NICHT (erst
`tcu_alloc.ko` separat laden). `steps_pan=20000` (User-Schätzung, nicht
hardware-kalibriert). Tilt-Achse ungenutzt (`gpio_tilt=""`,
`steps_tilt=0`) — dafür war ein Fix in `S59motor` nötig, siehe
`package/thingino-motors/files/S59motor` (`set_motor_phases` bricht sonst
bei leerem `gpio_tilt` das ganze Skript ab).

⚠️ **Nie `rmmod motor` während `motors-daemon` läuft** — führt zu Kernel-
Fault ("Fixing recursive fault but reboot is needed!"), Daemon hängt
unrettbar in `D`-State. Immer erst `/etc/init.d/S59motor stop` (und
verifizieren, dass der Prozess wirklich weg ist), dann erst Modul
entladen. Im Ernstfall hilft nur ein normaler `reboot` (kein Reflash
nötig — Root ist OverlayFS aus `squashfs`(ro) + `jffs2`(rw), alle
manuell gepushten Dateien überleben das).

## Kamera / Sonstiges

| Funktion | Pin (dez.) | Ingenic-GPIO | Status |
|---|---|---|---|
| LED Rot | 49 | PB17 | physisch verifiziert (active-high) |
| LED Blau | 50 | PB18 | physisch verifiziert (active-high) |
| IR-Nachtsicht-LED | 60 | PB28 | aus Firmware dekodiert |
| IR-CUT Filter A | 53 | PB21 | aus Firmware dekodiert |
| IR-CUT Filter B | 52 | PB20 | aus Firmware dekodiert |
| Sensor-Reset | 18 | PA18 | von Thingino aktiv gehalten |
| SD-Karte Detect | 38 | PB6 | physisch verifiziert, active-low (0=Karte drin, 1=draußen). Nicht aus Vendor-Dump ableitbar, nur Hardwaremessung verlässlich. |
| Physische Taste | 62 | PB30 | physisch verifiziert, active-low (0=gedrückt, 1=los). Kein interner Pull nötig — externer Pull-Up auf dem Tasterboard zieht auf 3,3V. Nicht aus Vendor-Dump ableitbar. |

Zusätzlich eine rote Power-Mini-LED, fest verdrahtet (kein GPIO, kein Software-Zugriff).

## Taste

Original-Firmware liest Pin 62 über `strnio.ko`, Board-Tabelle Offset `+0x3e`
(PCB-Variante `P1T_T3_V10`/Eintrag 29), Slot 1 der Funktion
`pps_board_get_io_events` (bis zu 8 konfigurierbare digitale Eingänge,
4×1ms entprellt).

## ⚠️ Gefährlicher Pin

**Pin 39 (PB7) — NIEMALS als GPIO exportieren/anfassen.** Export als Input
hängt/crasht das Gerät sofort, ohne Auto-Recovery — manueller Stromreset
nötig. Vermutlich eine aktive SPI-Flash-Steuerleitung (Chip-Select/Clock/WP#),
kein normales GPIO.

## WLAN / SD-Power

- WLAN: keine Schalt-GPIO auf diesem Board — `pin=0` in der kompletten
  Vendor-Board-Tabelle. Der Ioctl-Befehl `wlanPwr` existiert generisch in
  `strnio.ko`, dessen Handler ruft aber keine GPIO-Funktion auf (Software-No-Op
  auf dieser Variante).
- SD-Karten-Power: läuft in der Original-Firmware über einen Kernel-ioctl
  (`0x80044d0e`) auf ein Char-Device, nicht über sysfs-GPIO.

## Reverse-Engineering-Werkzeuge

Skripte zum Nachvollziehen/Erweitern der Dekodierung liegen in
`~/Documents/Projects/petcam/fw_orig/`:
- `dump_entries2.py` — listet alle 34 PCB-Varianten aus `strnio.ko` mit GPIO-Werten
- `find_gpio_calls.py`, `disasm_func.py` — MIPS-Disassembly-Helfer (capstone/pyelftools)
