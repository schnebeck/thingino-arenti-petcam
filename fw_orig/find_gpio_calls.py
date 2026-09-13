#!/usr/bin/env python3
"""
Find calls to gpio_* kernel functions inside a MIPS .ko relocatable object
and disassemble the surrounding context to recover which GPIO pin number
(register $a0) is used, by resolving relocations against those symbols.
"""
import sys
from elftools.elf.elffile import ELFFile
from elftools.elf.relocation import RelocationSection
import capstone

TARGET_SYMS = {
    "gpio_request", "gpio_free", "gpio_direction_output", "gpio_direction_input",
    "gpio_set_value", "gpio_get_value", "__gpio_get_value", "__gpio_set_value",
    "gpio_to_irq", "request_irq", "devm_gpio_request", "devm_gpio_request_one",
    "gpio_request_one",
}

def nearest_local_symbol(symtab, section_idx, addr):
    best = None
    for sym in symtab.iter_symbols():
        if sym['st_shndx'] != section_idx:
            continue
        st_info = sym['st_info']
        if st_info.type not in ('STT_FUNC', 'STT_NOTYPE'):
            continue
        val = sym['st_value']
        if val <= addr and (best is None or val > best[0]):
            best = (val, sym.name)
    return best

def main(path):
    with open(path, 'rb') as f:
        elf = ELFFile(f)
        symtab = elf.get_section_by_name('.symtab')
        if symtab is None:
            print("no symtab"); return

        # map symbol index -> name for relocation resolution
        symbols = list(symtab.iter_symbols())

        text = elf.get_section_by_name('.text')
        text_idx = None
        for i in range(elf.num_sections()):
            if elf.get_section(i) == text:
                text_idx = i
                break
        text_data = text.data()
        text_addr_base = text['sh_addr']

        md = capstone.Cs(capstone.CS_ARCH_MIPS, capstone.CS_MODE_MIPS32 + capstone.CS_MODE_LITTLE_ENDIAN)
        md.detail = False

        # find relocation section for .text
        relsec = None
        for sec in elf.iter_sections():
            if isinstance(sec, RelocationSection) and sec.name in ('.rel.text', '.rela.text'):
                relsec = sec
                break
        if relsec is None:
            print("no .rel.text/.rela.text section found")
            print("Sections:", [s.name for s in elf.iter_sections()])
            return

        hits = []
        for rel in relsec.iter_relocations():
            sym_idx = rel['r_info_sym']
            sym = symbols[sym_idx]
            if sym.name in TARGET_SYMS:
                hits.append((rel['r_offset'], sym.name))

        hits.sort()
        print(f"Found {len(hits)} relocation references to target gpio/irq functions\n")

        # disassemble whole .text once, index by address
        insns = list(md.disasm(text_data, text_addr_base))
        by_addr = {ins.address: ins for ins in insns}
        addrs_sorted = sorted(by_addr.keys())

        for off, name in hits:
            call_addr = off  # address of the jal/relocation site (offset within .text, matches sh_addr+off since base likely 0)
            func = nearest_local_symbol(symtab, text_idx, call_addr)
            fname = func[1] if func else "?"
            print(f"=== call to {name} at .text+0x{call_addr:x} (in function ~{fname}) ===")
            # gather window of instructions: 12 before .. 4 after
            idx_list = [a for a in addrs_sorted if call_addr - 0x30 <= a <= call_addr + 0x10]
            for a in idx_list:
                ins = by_addr[a]
                marker = "  <-- CALL" if a == call_addr else ""
                print(f"    0x{a:06x}: {ins.mnemonic:8s} {ins.op_str}{marker}")
            print()

if __name__ == '__main__':
    main(sys.argv[1])
