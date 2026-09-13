#!/usr/bin/env python3
import sys
from elftools.elf.elffile import ELFFile
from elftools.elf.relocation import RelocationSection
import capstone

def main(path, func_name):
    with open(path, 'rb') as f:
        elf = ELFFile(f)
        symtab = elf.get_section_by_name('.symtab')
        symbols = list(symtab.iter_symbols())
        target = None
        for sym in symbols:
            if sym.name == func_name:
                target = sym
                break
        if target is None:
            print("symbol not found"); return
        start = target['st_value']
        size = target['st_size']
        end = start + size

        text = elf.get_section_by_name('.text')
        text_data = text.data()
        text_addr = text['sh_addr']

        # relocations for .text, indexed by offset
        relmap = {}
        for sec in elf.iter_sections():
            if isinstance(sec, RelocationSection) and sec.name in ('.rel.text', '.rela.text'):
                for rel in sec.iter_relocations():
                    sym = symbols[rel['r_info_sym']]
                    relmap[rel['r_offset']] = sym.name

        md = capstone.Cs(capstone.CS_ARCH_MIPS, capstone.CS_MODE_MIPS32 + capstone.CS_MODE_LITTLE_ENDIAN)
        chunk = text_data[start:end]
        print(f"=== {func_name} @ 0x{start:x}-0x{end:x} (size {size}) ===")
        for ins in md.disasm(chunk, text_addr + start):
            reloc = f"    ; RELOC -> {relmap[ins.address]}" if ins.address in relmap else ""
            print(f"  0x{ins.address:06x}: {ins.mnemonic:8s} {ins.op_str}{reloc}")

if __name__ == '__main__':
    main(sys.argv[1], sys.argv[2])
