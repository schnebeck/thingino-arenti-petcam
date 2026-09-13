#!/usr/bin/env python3
import sys
from elftools.elf.elffile import ELFFile

def main(path):
    with open(path, 'rb') as f:
        elf = ELFFile(f)
        for sec in elf.iter_sections():
            if sec.name in ('.rodata', '.data', '.rodata.str1.4', '.rodata.str1.1'):
                data = sec.data()
                print(f"--- section {sec.name} size={len(data)} addr={sec['sh_addr']:#x} ---")

        # search .rodata for stride-0x78 structures containing an ASCII prefix
        for secname in ('.rodata', '.data'):
            sec = elf.get_section_by_name(secname)
            if sec is None:
                continue
            data = sec.data()
            n = len(data)
            # look at every 0x78-aligned-ish window for printable ascii run at start
            for off in range(0, max(0, n - 0x78), 4):
                chunk = data[off:off+12]
                # check first bytes look like ascii name then nul padding
                printable = 0
                for b in chunk:
                    if 32 <= b < 127:
                        printable += 1
                    else:
                        break
                if printable >= 3:
                    # crude heuristic: print candidate
                    name = chunk[:printable].decode('ascii', 'replace')
                    print(f"{secname}+0x{off:x}: name-like='{name}'")

if __name__ == '__main__':
    main(sys.argv[1])
