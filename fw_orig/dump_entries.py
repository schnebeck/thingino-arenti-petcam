#!/usr/bin/env python3
from elftools.elf.elffile import ELFFile
import struct

PATH = 'strnio.ko'
TABLE_FILE_OFF = 0xfad0
ENTRY_SIZE = 0x78
COUNT = 0x22

LABELS = [
    ("0x0c", "LED0"),
    ("0x0e", "LED1"),
    ("0x10", "IR-LED"),
    ("0x12", "IRCUT-A"),
    ("0x14", "IRCUT-B"),
    ("0x16", "?16"),
    ("0x18", "LED-status/aux"),
    ("0x1a", "STEP1-A"),
    ("0x1c", "STEP1-B"),
    ("0x1e", "STEP1-C"),
    ("0x20", "STEP1-D"),
    ("0x22", "STEP2-A"),
    ("0x24", "STEP2-B"),
    ("0x26", "STEP2-C"),
    ("0x28", "STEP2-D"),
    ("0x2a", "?2a"),
    ("0x2c", "?2c"),
    ("0x2e", "?2e"),
    ("0x30", "DCMOTOR-EN"),
    ("0x32", "DCMOTOR-SLOW-IRQ"),
    ("0x34", "DCMOTOR-STOP-IRQ"),
    ("0x36", "?36"),
    ("0x38", "?38"),
    ("0x3a", "?3a"),
    ("0x3c", "?3c"),
    ("0x3e", "?3e"),
    ("0x40", "?40"),
    ("0x42", "?42"),
    ("0x44", "?44"),
    ("0x46", "?46"),
    ("0x48", "?48"),
    ("0x4a", "?4a"),
    ("0x4c", "?4c"),
    ("0x4e", "?4e"),
    ("0x50", "?50"),
]

with open(PATH, 'rb') as f:
    raw = f.read()

for i in range(COUNT):
    off = TABLE_FILE_OFF + i * ENTRY_SIZE
    entry = raw[off:off+ENTRY_SIZE]
    name_bytes = entry[0:12]
    name = name_bytes.split(b'\x00')[0].decode('latin1')
    if not name.strip():
        continue
    print(f"=== entry {i}: name='{name}' (raw name bytes: {name_bytes.hex()}) ===")
    for rel_off_str, label in LABELS:
        rel_off = int(rel_off_str, 16)
        if rel_off + 2 > ENTRY_SIZE:
            continue
        val = struct.unpack_from('<H', entry, rel_off)[0]
        pin = val & 0xff
        flags = (val >> 8) & 0xff
        print(f"    +{rel_off_str:5s} {label:18s} pin={pin:3d} (0x{pin:02x})  flags=0x{flags:02x}")
    print()
