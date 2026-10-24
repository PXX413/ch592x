# -*- coding: gbk -*-
"""解析并校验 USB 复合(HID+CDC)描述符的一致性"""
import re, os

ROOT = r'C:/Users/a/Desktop/单片机开发/ch592x/keyboard'

def load(name):
    raw = open(os.path.join(ROOT, name), 'rb').read()
    for enc in ('utf-8', 'gbk'):
        try:
            return raw.decode(enc)
        except Exception:
            pass
    return raw.decode('utf-8', errors='replace')

# ---------- 收集宏 ----------
MAC = {}
for f in ['usb_desc.h', 'usb_desc_cdc.h', 'usb_kbd.h', 'usb_kbd_port.h']:
    if not os.path.exists(os.path.join(ROOT, f)):
        continue
    t = load(f)
    for m in re.finditer(r'#define\s+(\w+)\s+([0-9A-Fa-fx]+u?)\s*(?:/\*.*?\*/)?\s*$', t, re.M):
        MAC.setdefault(m.group(1), m.group(2).rstrip('u'))

def val(tok):
    tok = tok.strip().rstrip(',')
    if tok in MAC:
        return int(MAC[tok], 0)
    if re.fullmatch(r'0x[0-9A-Fa-f]+u?', tok):
        return int(tok.rstrip('u'), 16)
    if re.fullmatch(r'\d+u?', tok):
        return int(tok.rstrip('u'))
    m = re.fullmatch(r'\(uint8_t\)\((\w+)\s*&\s*0xFFu?\)', tok)
    if m:
        return val(m.group(1)) & 0xFF
    m = re.fullmatch(r'\(uint8_t\)\(\((\w+)\s*>>\s*8\)\s*&\s*0xFFu?\)', tok)
    if m:
        return (val(m.group(1)) >> 8) & 0xFF
    return None

# ---------- 取数组 ----------
def get_array(src, name):
    i = src.find(name + '[')
    i = src.find('{', i)
    depth = 0
    for j in range(i, len(src)):
        if src[j] == '{': depth += 1
        elif src[j] == '}':
            depth -= 1
            if depth == 0:
                break
    body = src[i+1:j]
    body = re.sub(r'/\*.*?\*/', '', body, flags=re.S)
    out, bad = [], []
    for tok in re.split(r',(?![^(]*\))', body):
        tok = tok.strip()
        if not tok: continue
        v = val(tok)
        if v is None: bad.append(tok[:40])
        else: out.append(v)
    return out, bad

cdc = load('usb_desc_cdc.c')
print('=== 复合配置描述符 (UsbDescCdc_Config) ===')
cfg, bad = get_array(cdc, 'UsbDescCdc_Config')
print('数组长度 =', len(cfg), ' 无法解析的项:', bad if bad else '无')
T = {'CONFIG':0x02,'IFACE':0x04,'ENDPOINT':0x05,'HID':0x21,'IAD':0x0B,
     'CS_IFACE':0x24,'CS_ENDP':0x25,'DEVICE':0x01,'REPORT':0x22}
NAME = {v:k for k,v in T.items()}
print()
print('%-4s %-6s %-16s %s' % ('off','len','type','内容'))
o, ifaces, eps, iad = 0, [], [], 0
while o < len(cfg):
    ln = cfg[o]
    if ln == 0:
        print('  !! bLength=0 @ %d' % o); break
    ty = cfg[o+1] if o+1 < len(cfg) else -1
    nm = NAME.get(ty, '0x%02X' % ty)
    extra = ''
    if ty == 0x02 and o+8 < len(cfg):
        total = cfg[o+2] | (cfg[o+3] << 8)
        extra = 'wTotalLength=%d bNumInterfaces=%d cfgVal=%d bmAttr=0x%02X' % (
            total, cfg[o+4], cfg[o+5], cfg[o+7])
        wtotal = total
        nif_decl = cfg[o+4]
    if ty == 0x04:
        ifaces.append(cfg[o+2])
        extra = 'iface=%d alt=%d nEP=%d class=0x%02X sub=0x%02X proto=0x%02X' % (
            cfg[o+2], cfg[o+3], cfg[o+4], cfg[o+5], cfg[o+6], cfg[o+7])
    if ty == 0x05:
        eps.append((cfg[o+2], cfg[o+4] | (cfg[o+5] << 8)))
        extra = 'addr=0x%02X attr=0x%02X maxPkt=%d' % (cfg[o+2], cfg[o+3], cfg[o+4] | (cfg[o+5] << 8))
    if ty == 0x0B:
        iad += 1
    print('%-4d %-6d %-16s %s' % (o, ln, nm, extra))
    o += ln

print()
print('--- 一致性检查 ---')
print('  数组长度 %d  vs  wTotalLength %d  -> %s' % (
    len(cfg), wtotal, '一致' if len(cfg) == wtotal else '★不一致!'))
print('  声明接口数 %d  vs  实际接口描述符 %d 个 -> %s' % (
    nif_decl, len(ifaces), '一致' if nif_decl == len(ifaces) else '★不一致!'))
print('  接口号:', ifaces)
print('  端点:', [(hex(a), m) for a, m in eps])
print('  IAD(0x0B) 个数: %d  %s' % (iad, '<- CDC 双接口必须有 IAD!' if iad == 0 and len(ifaces) >= 2 else ''))

print()
print('=== 设备描述符 ===')
for nm in ['UsbDescCdc_Device', 'UsbDescCdc_Dev', 'UsbDesc_Device']:
    if nm in cdc:
        d, bad2 = get_array(cdc, nm)
        print(nm, ':', d[:18])
        if len(d) >= 8:
            print('  bMaxPacketSize0 = %d' % d[7])
            print('  bDeviceClass/Sub/Proto = 0x%02X / 0x%02X / 0x%02X' % (d[4], d[5], d[6]))
        break
else:
    print('未在 usb_desc_cdc.c 里找到设备描述符，看它 include 了谁')
    for m in re.finditer(r'#include\s+"(\w+\.h)"', cdc):
        print('  include', m.group(1))
