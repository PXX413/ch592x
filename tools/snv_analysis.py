# -*- coding: utf-8 -*-
"""在链接后的 ELF 中定位 SNV 写回调的调用点，分析传入的 addr 形态"""
from elftools.elf.elffile import ELFFile
import sys

ELF = r"C:\Users\a\Desktop\单片机开发\ch592x\obj\CH592X.elf"

with open(ELF, "rb") as f:
    elf = ELFFile(f)
    symtab = elf.get_section_by_name(".symtab")
    syms = []
    for s in symtab.iter_symbols():
        n = s.name
        if not n:
            continue
        low = n.lower()
        if ("snv" in low) or ("bond" in low and "mgr" not in low) or ("nv" == low[-2:]) or ("flash" in low):
            syms.append((s["st_value"], s["st_size"], n))

    syms.sort()
    print("== SNV/Flash 相关符号 ==")
    for addr, size, name in syms:
        if 0x20000 < addr or addr < 0x70000:  # 代码段
            print(f"  0x{addr:08x}  size={size:5d}  {name}")

    # Lib_Write_Flash 本体
    print("\n== 关键回调符号 ==")
    for s in symtab.iter_symbols():
        if s.name in ("Lib_Write_Flash", "Lib_Write_Flash_592A", "Lib_Read_Flash", "tmos_snv_read", "tmos_snv_write"):
            print(f"  0x{s['st_value']:08x}  size={s['st_size']:5d}  {s.name}")
