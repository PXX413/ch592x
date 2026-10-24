#!/usr/bin/env python3
# -*- coding: gbk -*-
"""
网页 ←→ 固件 常量一致性交叉校验

网页端（keymap_editor.html 的 8.5 节 JS）和固件端（kmap_proto.h / kmap_server.h）
各有一份协议常量。两份必须逐字对齐，否则连上设备就"答非所问"。

本脚本直接从两边的源码里把常量抠出来做集合对比，不依赖任何手工维护的清单。
"""

import re
import sys
from pathlib import Path

# Windows 默认控制台是 GBK，脚本里有 ←→ 这类字符，
# 打印时会 UnicodeEncodeError。改成"编不出来的用 ? 代替"，不要崩。
try:
    sys.stdout.reconfigure(errors="replace")
except Exception:
    pass

ROOT = Path(__file__).resolve().parent.parent
HTML = ROOT / "tools" / "keymap_editor.html"
PROTO_H = ROOT / "keyboard" / "kmap_proto.h"
SERVER_H = ROOT / "keyboard" / "kmap_server.h"

PASS = 0
FAIL = 0


def check(name, cond, extra=""):
    global PASS, FAIL
    if cond:
        PASS += 1
        print(f"  [PASS] {name}")
    else:
        FAIL += 1
        print(f"  [FAIL] {name}" + (f"  {extra}" if extra else ""))


def parse_c_defines(text):
    """抓 #define NAME 0xNN / 0xNNu 形式的常量"""
    out = {}
    for m in re.finditer(r"#define\s+([A-Za-z_][A-Za-z0-9_]*)\s+(0x[0-9A-Fa-f]+|\d+)\s*u?\b", text):
        out[m.group(1)] = int(m.group(2), 0)
    return out


def parse_js_kp(text):
    """从 HTML 里抠出 KP 常量表（JS 对象字面量）"""
    i = text.find("const KP = {")
    if i < 0:
        return None
    depth = 0
    for j in range(i, len(text)):
        if text[j] == "{":
            depth += 1
        elif text[j] == "}":
            depth -= 1
            if depth == 0:
                return text[i:j + 1]
    return None


def js_obj_to_dict(src):
    """把 JS 的 { A: 1, B: { C: 2 } } 粗略解析成嵌套 dict"""
    def strip_comments(s):
        s = re.sub(r"/\*[\s\S]*?\*/", "", s)
        s = re.sub(r"//[^\n]*", "", s)
        return s

    src = strip_comments(src)
    src = src[src.find("{"):]

    def parse(s, pos):
        obj = {}
        assert s[pos] == "{"
        pos += 1
        while True:
            while pos < len(s) and s[pos] in " \t\r\n,":
                pos += 1
            if s[pos] == "}":
                return obj, pos + 1
            # key（可能是标识符，也可能是纯数字 / 引号字符串）
            if s[pos] in "\"'":
                q = s[pos]
                end = s.index(q, pos + 1)
                key = s[pos + 1:end]
                pos = end + 1
            else:
                m = re.match(r"[A-Za-z_][A-Za-z0-9_]*|0x[0-9A-Fa-f]+|\d+", s[pos:])
                if not m:
                    raise ValueError(f"unexpected key at {pos}: {s[pos:pos+40]!r}")
                key = m.group(0)
                pos += len(key)
            while pos < len(s) and s[pos] in " \t\r\n":
                pos += 1
            assert s[pos] == ":", f"expected ':' at {pos}: {s[pos:pos+30]!r}"
            pos += 1
            while pos < len(s) and s[pos] in " \t\r\n":
                pos += 1
            if s[pos] == "{":
                val, pos = parse(s, pos)
            else:
                if s[pos] in "\"'":
                    q = s[pos]
                    end = s.index(q, pos + 1)
                    val = s[pos + 1:end]
                    pos = end + 1
                else:
                    m = re.match(r"0x[0-9A-Fa-f]+|\d+", s[pos:])
                    val = int(m.group(0), 0)
                    pos += len(m.group(0))
            obj[key] = val
        return obj, pos

    obj, _ = parse(src, 0)
    return obj


print("=" * 62)
print(" 网页 ←→ 固件 协议常量一致性交叉校验")
print("=" * 62)

html = HTML.read_text(encoding="gbk", errors="replace")
proto_h = PROTO_H.read_text(encoding="gbk", errors="replace")
server_h = SERVER_H.read_text(encoding="gbk", errors="replace")

# ---- 1. 网页 KP ----
kp_src = parse_js_kp(html)
if kp_src is None:
    check("能从网页抠出 KP 常量表", False)
    sys.exit(1)
KP = js_obj_to_dict(kp_src)
check("能从网页抠出 KP 常量表", True)

C = parse_c_defines(proto_h)
S = parse_c_defines(server_h)

# ---- 2. 帧头 ----
print("\nT1 帧头 / 帧上限")
check("SOF0 = 0xA5", KP["SOF0"] == C["KMAP_SOF0"] == 0xA5,
      f'web={KP["SOF0"]:#x} fw={C["KMAP_SOF0"]:#x}')
check("SOF1 = 0x5A", KP["SOF1"] == C["KMAP_SOF1"] == 0x5A,
      f'web={KP["SOF1"]:#x} fw={C["KMAP_SOF1"]:#x}')
check("MAX_PAYLOAD 一致", KP["MAX_PAYLOAD"] == C["KMAP_MAX_PAYLOAD"],
      f'web={KP["MAX_PAYLOAD"]} fw={C["KMAP_MAX_PAYLOAD"]}')

# ---- 3. 命令码 ----
print("\nT2 命令码")
CMD_MAP = {
    "PING":          "KMAP_CMD_PING",
    "GET_INFO":      "KMAP_CMD_GET_INFO",
    "GET_MAP":       "KMAP_CMD_GET_MAP",
    "SET_KEY":       "KMAP_CMD_SET_KEY",
    "SET_GROUP":     "KMAP_CMD_SET_GROUP",
    "GET_NAME":      "KMAP_CMD_GET_NAME",
    "SET_NAME":      "KMAP_CMD_SET_NAME",
    "SET_GROUP_IDX": "KMAP_CMD_SET_GROUP_IDX",
    "SAVE_FLASH":    "KMAP_CMD_SAVE_FLASH",
    "LOAD_FLASH":    "KMAP_CMD_LOAD_FLASH",
    "RESET_DEFAULT": "KMAP_CMD_RESET_DEFAULT",
}
for js_name, c_name in CMD_MAP.items():
    if js_name not in KP["CMD"]:
        check(f"网页有 {js_name}", False)
        continue
    if c_name not in C:
        check(f"固件有 {c_name}", False)
        continue
    check(f"{js_name} = {C[c_name]:#04x}", KP["CMD"][js_name] == C[c_name],
          f'web={KP["CMD"][js_name]:#x} fw={C[c_name]:#x}')

# ---- 4. 应答码 ----
print("\nT3 应答码")
check("PONG = 0x81", KP["PONG"] == C["KMAP_CMD_PONG"],
      f'web={KP["PONG"]:#x} fw={C["KMAP_CMD_PONG"]:#x}')
check("ERROR = 0x7F", KP["ERROR"] == C["KMAP_CMD_ERROR"],
      f'web={KP["ERROR"]:#x} fw={C["KMAP_CMD_ERROR"]:#x}')

# ACK 规则：cmd | 0x80
print("\nT4 ACK 规则（所有命令 cmd|0x80）")
for js_name in CMD_MAP:
    v = KP["CMD"][js_name]
    check(f"{js_name}: {v:#04x} | 0x80 = {v | 0x80:#04x}", (v | 0x80) == (v + 0x80),
          "命令码高位已被占用，无法用 |0x80 表示 ACK")

# ---- 5. 错误码 ----
print("\nT5 错误码")
ERR_MAP = {
    "1": "KMAP_E_CRC",
    "2": "KMAP_E_LEN",
    "3": "KMAP_E_CMD",
    "4": "KMAP_E_ARG",
    "5": "KMAP_E_FLASH",
}
for js_key, c_name in ERR_MAP.items():
    if js_key not in KP["ERR_NAME"]:
        check(f"网页有错误码 {js_key}", False)
        continue
    check(f"错误码 {js_key} = {C[c_name]:#04x}", int(js_key) == C[c_name],
          f'web_key={js_key} fw={C[c_name]}')

# ---- 6. 尺寸 ----
print("\nT6 尺寸常量")
# 网页在 setName 时用 name.slice(0, 16) 截断，必须与固件 KMAP_NAME_MAX 相同
web_name_max = re.search(r"name\.slice\(0,\s*(\d+)\)", html)
check("KMAP_NAME_MAX 一致（= 16）",
      web_name_max is not None
      and int(web_name_max.group(1)) == int(C["KMAP_NAME_MAX"]) == 16,
      f'web={web_name_max.group(1) if web_name_max else "?"} fw={C["KMAP_NAME_MAX"]}')

# ---- 7. INFO 魔数 ----
print("\nT7 设备标识魔数")
if "KMAP_INFO_MAGIC" in S:
    check("INFO 魔数 = 0x4B42（'K''B'）", S["KMAP_INFO_MAGIC"] == 0x4B42,
          f'fw={S["KMAP_INFO_MAGIC"]:#x}')
else:
    check("固件定义了 KMAP_INFO_MAGIC", False)

# 网页校验 GET_INFO 应答时也应该用同一个魔数
check("网页用 0x4B42 校验设备标识", "0x4B42" in html)

# ---- 8. 组数 / 键数 ----
print("\nT8 矩阵尺寸")
km_h = (ROOT / "hardware" / "key_matrix.h").read_text(encoding="utf-8", errors="replace")
g = re.search(r"#define\s+KEY_MAP_GROUPS\s+(\d+)", km_h)
k = re.search(r"#define\s+KEY_MAP_KEYS\s+(\d+)", km_h)
check("KEY_MAP_GROUPS = 5", g and int(g.group(1)) == 5, f"fw={g.group(1) if g else '?'}")
check("KEY_MAP_KEYS = 9", k and int(k.group(1)) == 9, f"fw={k.group(1) if k else '?'}")
check("网页 GROUPS = 5", re.search(r"GROUPS\s*=\s*5", html) is not None)
check("网页 KEYS = 9", re.search(r"KEYS\s*=\s*9", html) is not None)

# ---- 9. HID 键值列表（固件 s_hid_usage[] ←→ 网页 HID_USAGE）----
print("")
print("T9 HID 键值列表（设备上能选的键值）")
ui_pages2 = (ROOT / "ui" / "ui_pages2.c").read_text(encoding="gbk", errors="replace")
i = ui_pages2.find("s_hid_usage[]")
j = ui_pages2.find("};", i)
fw_usage = []
if i >= 0 and j > i:
    fw_usage = [int(x, 16)
                for x in re.findall(r"0x([0-9A-Fa-f]{2})u", ui_pages2[i:j])]

i = html.find("const HID_USAGE = [")
j = html.find("];", i)
web_usage = []
if i >= 0 and j > i:
    web_usage = [int(x, 16)
                 for x in re.findall(r"0x([0-9A-Fa-f]{2})", html[i:j])]

check("能从 ui_pages2.c 抠出 s_hid_usage[]", len(fw_usage) > 0)
check("能从网页抠出 HID_USAGE", len(web_usage) > 0)
check("两边都是 83 项", len(fw_usage) == 83 and len(web_usage) == 83,
      "fw=" + str(len(fw_usage)) + " web=" + str(len(web_usage)))
diff = [n for n, (a, b) in enumerate(zip(fw_usage, web_usage)) if a != b]
check("两边内容与顺序逐项一致",
      (len(fw_usage) == len(web_usage)) and (not diff),
      "第一个不同位置=" + (str(diff[0]) if diff else "-"))
check("修饰键 0xE0~0xE7 都在列表里", all(u in fw_usage for u in range(0xE0, 0xE8)))
check("修饰键排在最前（进编辑页一转编码器就到）",
      fw_usage[:8] == list(range(0xE0, 0xE8)))
check("页面提示里的项数由 HID_USAGE.length 现算",
      'usagecount' in html and 'HID_USAGE.length' in html)

# ---- 10. HID 键值名字（固件 UiKbd_UsageName() ←→ 网页 USAGE_NAME）----
print("")
print("T10 HID 键值名字（两边必须一模一样）")
ui_kbd = (ROOT / "ui" / "ui_kbd.c").read_text(encoding="gbk", errors="replace")
i = ui_kbd.find("const char *UiKbd_UsageName(uint8_t usage)")
j = ui_kbd.find("default:", i)
k = ui_kbd.find("}", j)
fw_names = {}
if i >= 0 and j > i and k > j:
    for a, b in re.findall('0x([0-9A-Fa-f]{2})u: return "([^"]+)"', ui_kbd[i:k]):
        fw_names[int(a, 16)] = b

i = html.find("const USAGE_NAME = {")
j = html.find("};", i)
web_names = {}
if i >= 0 and j > i:
    for a, b in re.findall('0x([0-9A-Fa-f]{2}):"([^"]+)"', html[i:j]):
        web_names[int(a, 16)] = b

check("能从 ui_kbd.c 抠出 UiKbd_UsageName()", len(fw_names) > 0)
check("能从网页抠出 USAGE_NAME", len(web_names) > 0)
missing_fw = [hex(u) for u in fw_usage if u not in fw_names]
missing_web = [hex(u) for u in fw_usage if u not in web_names]
check("固件名字表覆盖列表里每个值", not missing_fw, "缺=" + ",".join(missing_fw))
check("网页名字表覆盖列表里每个值", not missing_web, "缺=" + ",".join(missing_web))
fw_list = [fw_names.get(u) for u in fw_usage]
web_list = [web_names.get(u) for u in fw_usage]
check("两边名字逐项一致", fw_list == web_list and all(fw_list),
      "fw=" + str(fw_list[:8]) + " web=" + str(web_list[:8]))

print("\n" + "=" * 62)
print(f" 结果：PASS={PASS}  FAIL={FAIL}")
print("=" * 62)
sys.exit(0 if FAIL == 0 else 1)
