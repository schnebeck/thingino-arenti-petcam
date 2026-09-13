#!/usr/bin/env python3
from elftools.elf.elffile import ELFFile
from elftools.elf.relocation import RelocationSection
import struct

PATH = 'strnio.ko'
TABLE_DATA_OFF = 0x70          # offset within .data section (virtual, section base 0)
ENTRY_SIZE = 0x78
COUNT = 0x22

LABELS = [
    ("0x0c", "LED0"), ("0x0e", "LED1"), ("0x10", "IR-LED"),
    ("0x12", "IRCUT-A"), ("0x14", "IRCUT-B"), ("0x16", "?16"),
    ("0x18", "LED-status/aux"),
    ("0x1a", "STEP1-A"), ("0x1c", "STEP1-B"), ("0x1e", "STEP1-C"), ("0x20", "STEP1-D"),
    ("0x22", "STEP2-A"), ("0x24", "STEP2-B"), ("0x26", "STEP2-C"), ("0x28", "STEP2-D"),
    ("0x2a", "?2a"), ("0x2c", "?2c"), ("0x2e", "?2e"),
    ("0x30", "DCMOTOR-EN"), ("0x32", "DCMOTOR-SLOW-IRQ"), ("0x34", "DCMOTOR-STOP-IRQ"),
    ("0x36", "?36"), ("0x38", "?38"), ("0x3a", "?3a"), ("0x3c", "?3c"),
    ("0x3e", "?3e"), ("0x40", "?40"), ("0x42", "?42"), ("0x44", "?44"),
    ("0x46", "?46"), ("0x48", "?48"), ("0x4a", "?4a"), ("0x4c", "?4c"),
    ("0x52", "?52"), ("0x54", "?54"), ("0x56", "?56"), ("0x58", "?58"),
    ("0x5a", "?5a"), ("0x5c", "?5c"), ("0x5e", "?5e"), ("0x60", "EXTERN-CTRL(cmd8)"),
    ("0x62", "?62"), ("0x64", "?64"), ("0x66", "?66"), ("0x68", "?68"),
    ("0x6a", "BUTTON/POWER-KEY"), ("0x6c", "?6c-flags2"), ("0x6e", "?6e"),
    ("0x70", "?70"), ("0x72", "?72"),
    ("0x4e", "?4e"), ("0x50", "?50"),
]

with open(PATH, 'rb') as f:
    elf = ELFFile(f)
    symtab = elf.get_section_by_name('.symtab')
    symbols = list(symtab.iter_symbols())
    data_sec = elf.get_section_by_name('.data')
    data_off_file = data_sec['sh_offset']
    raw = f.read(0) or None
    f.seek(0)
    filedata = f.read()

    # build reloc map for .rel.data: data_offset -> (target_section_name, addend/value)
    reldata = elf.get_section_by_name('.rel.data')
    relmap = {}
    for rel in reldata.iter_relocations():
        sym = symbols[rel['r_info_sym']]
        shndx = sym['st_shndx']
        target_sec = elf.get_section(shndx) if isinstance(shndx, int) else None
        relmap[rel['r_offset']] = (target_sec.name if target_sec else sym.name, sym['st_value'])

    def read_cstr_at(secname, base_value, addend_from_instr):
        sec = elf.get_section_by_name(secname)
        if sec is None:
            return None
        file_off = sec['sh_offset'] + base_value + addend_from_instr
        end = filedata.find(b'\x00', file_off)
        return filedata[file_off:end].decode('latin1', 'replace')

    for i in range(COUNT):
        entry_data_off = TABLE_DATA_OFF + i * ENTRY_SIZE
        entry_file_off = data_off_file + entry_data_off
        entry = filedata[entry_file_off:entry_file_off+ENTRY_SIZE]

        # name pointer field at +0x0 (4 bytes) - R_MIPS_32 reloc presumably
        name = "?"
        if entry_data_off in relmap:
            secname, symval = relmap[entry_data_off]
            addend = struct.unpack_from('<I', entry, 0)[0]  # embedded addend for REL
            name = read_cstr_at(secname, symval, addend) or "?"

        vals = {}
        for rel_off_str, label in LABELS:
            rel_off = int(rel_off_str, 16)
            val = struct.unpack_from('<H', entry, rel_off)[0]
            vals[label] = (val & 0xff, (val >> 8) & 0xff)

        has_step1 = vals["STEP1-A"][0] != 0
        has_step2 = vals["STEP2-A"][0] != 0
        has_dc = vals["DCMOTOR-EN"][0] != 0
        marker = ""
        if has_step1 and has_step2 and has_dc:
            marker = "  <<<< FULL MOTOR SET (2 stepper + DC) >>>>"
        elif has_step1 or has_step2 or has_dc:
            marker = "  (partial motor fields)"

        print(f"=== entry {i}: name='{name}'{marker} ===")
        for rel_off_str, label in LABELS:
            pin, flags = vals[label]
            if pin or flags:
                print(f"    +{rel_off_str:5s} {label:18s} pin={pin:3d} (0x{pin:02x})  flags=0x{flags:02x}")
        print()
