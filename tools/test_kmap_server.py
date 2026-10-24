#!/usr/bin/env python3
# -*- coding: gbk -*-
"""
kmap_server 业务层协议一致性测试

按 kmap_server.c 里每个命令的分支逻辑，在 Python 里复刻一遍，
验证「网页要发的字节」和「固件会回的字节」是否完全对得上。

覆盖：
  T1  PING / PONG
  T2  GET_INFO 字段布局
  T3  GET_MAP 单组 9 键
  T4  SET_KEY -> GET_MAP 往返
  T5  SET_GROUP 整组写入
  T6  GET_NAME / SET_NAME（含非法字符过滤）
  T7  SET_GROUP_IDX
  T8  SAVE_FLASH / LOAD_FLASH / RESET_DEFAULT
  T9  错误路径：未知命令 / 长度错 / 参数越界
"""

import struct

SOF0, SOF1 = 0xA5, 0x5A
CRC8_POLY = 0x07
MAX_PAYLOAD = 64
MAX_FRAME = 7 + MAX_PAYLOAD

GROUPS, KEYS = 5, 9
NAME_MAX = 16
NAME_SLOT = 17

# 命令
C_PING, C_GET_INFO = 0x01, 0x02
C_GET_MAP, C_SET_KEY, C_SET_GROUP = 0x10, 0x11, 0x12
C_GET_NAME, C_SET_NAME = 0x13, 0x14
C_SET_GROUP_IDX = 0x20
C_SAVE, C_LOAD, C_RESET = 0x30, 0x31, 0x32
C_PONG, C_ERROR = 0x81, 0x7F
ACK = lambda c: c | 0x80

INFO_MAGIC = 0x4B42
STORE_VER = 0x0001
FW_BUILD = 0x0001

E_OK, E_CRC, E_LEN, E_CMD, E_ARG, E_FLASH = 0, 1, 2, 3, 4, 5


def crc8(data):
    crc = 0
    for b in data:
        crc ^= b
        for _ in range(8):
            crc = ((crc << 1) ^ CRC8_POLY) & 0xFF if crc & 0x80 else (crc << 1) & 0xFF
    return crc


def build(cmd, seq, payload=b''):
    body = bytes([SOF0, SOF1, cmd, seq]) + struct.pack('<H', len(payload)) + payload
    return body + bytes([crc8(body)])


# ---------------------------------------------------------------- 设备侧状态（对 key_matrix + kmap_store）
class Device:
    def __init__(self):
        self.map = [
            [0x1E, 0x1F, 0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26],
            [0x29, 0x52, 0x2C, 0x50, 0x28, 0x4F, 0x80, 0x51, 0x81],
            [0x3A, 0x3B, 0x3C, 0x3D, 0x3E, 0x3F, 0x40, 0x41, 0x42],
            [0x59, 0x5A, 0x5B, 0x5C, 0x5D, 0x5E, 0x5F, 0x60, 0x61],
        ]
        self.default_map = [r[:] for r in self.map]
        self.group = 0
        self.default_group = 0
        self.names = ["123", "ARROW", "F1-F9", "NUM"]
        self.default_names = self.names[:]
        self.flash = None          # 模拟 Flash 记录（dict 或 None）
        self.rx_frames = 0
        self.err_frames = 0

    # ---- 应答构造 ----
    def _err(self, seq, code):
        self.err_frames += 1
        return build(C_ERROR, seq, bytes([code]))

    def _ack(self, cmd, seq, pay=b''):
        return build(ACK(cmd), seq, pay)

    def handle(self, cmd, seq, pay):
        self.rx_frames += 1

        if cmd == C_PING:
            return build(C_PONG, seq, struct.pack('<HBB', INFO_MAGIC, STORE_VER & 0xFF, 1))

        if cmd == C_GET_INFO:
            out = struct.pack('<H', INFO_MAGIC)
            out += bytes([1, 0, GROUPS, KEYS, self.group, 1 if self.flash else 0])
            out += struct.pack('<H', FW_BUILD)
            return self._ack(C_GET_INFO, seq, out)

        if cmd == C_GET_MAP:
            if len(pay) != 1:
                return self._err(seq, E_LEN)
            g = pay[0]
            if g >= GROUPS:
                return self._err(seq, E_ARG)
            out = b''.join(struct.pack('<H', v) for v in self.map[g])
            return self._ack(C_GET_MAP, seq, out)

        if cmd == C_SET_KEY:
            if len(pay) != 4:
                return self._err(seq, E_LEN)
            g, k = pay[0], pay[1]
            v = struct.unpack('<H', pay[2:4])[0]
            if g >= GROUPS or not (1 <= k <= KEYS):
                return self._err(seq, E_ARG)
            self.map[g][k - 1] = v
            return self._ack(C_SET_KEY, seq,
                             bytes([g, k]) + struct.pack('<H', self.map[g][k - 1]))

        if cmd == C_SET_GROUP:
            if len(pay) != 1 + KEYS * 2:
                return self._err(seq, E_LEN)
            g = pay[0]
            if g >= GROUPS:
                return self._err(seq, E_ARG)
            for i in range(KEYS):
                self.map[g][i] = struct.unpack('<H', pay[1 + i * 2:3 + i * 2])[0]
            return self._ack(C_SET_GROUP, seq)

        if cmd == C_GET_NAME:
            if len(pay) != 1:
                return self._err(seq, E_LEN)
            g = pay[0]
            if g >= GROUPS:
                return self._err(seq, E_ARG)
            out = self.names[g].encode('ascii', 'ignore')[:NAME_MAX]
            out = out + bytes(NAME_SLOT - len(out))
            return self._ack(C_GET_NAME, seq, out)

        if cmd == C_SET_NAME:
            if len(pay) < 1 or len(pay) > 1 + NAME_MAX:
                return self._err(seq, E_LEN)
            g = pay[0]
            if g >= GROUPS:
                return self._err(seq, E_ARG)
            # 过滤不可打印（0x20..0x7E）
            raw = pay[1:]
            clean = bytes(b for b in raw if 0x20 <= b < 0x7F)[:NAME_MAX]
            self.names[g] = clean.decode('ascii')
            out = clean + bytes(NAME_SLOT - len(clean))
            return self._ack(C_SET_NAME, seq, out)

        if cmd == C_SET_GROUP_IDX:
            if len(pay) != 1:
                return self._err(seq, E_LEN)
            if pay[0] >= GROUPS:
                return self._err(seq, E_ARG)
            self.group = pay[0]
            return self._ack(C_SET_GROUP_IDX, seq, bytes([self.group]))

        if cmd == C_SAVE:
            self.flash = {
                'group': self.group,
                'map': [r[:] for r in self.map],
                'names': self.names[:],
            }
            return self._ack(C_SAVE, seq)

        if cmd == C_LOAD:
            ok = 1 if self.flash else 0
            if self.flash:
                self.group = self.flash['group']
                self.map = [r[:] for r in self.flash['map']]
                self.names = self.flash['names'][:]
            return self._ack(C_LOAD, seq, bytes([ok]))

        if cmd == C_RESET:
            self.map = [r[:] for r in self.default_map]
            self.names = self.default_names[:]
            self.group = self.default_group
            self.flash = {
                'group': self.group,
                'map': [r[:] for r in self.map],
                'names': self.names[:],
            }
            return self._ack(C_RESET, seq)

        return self._err(seq, E_CMD)


# 解析应答
def parse_reply(frame):
    assert frame[0] == SOF0 and frame[1] == SOF1, "SOF 错"
    assert crc8(frame[:-1]) == frame[-1], "CRC 错"
    cmd, seq = frame[2], frame[3]
    ln = struct.unpack('<H', frame[4:6])[0]
    return cmd, seq, frame[6:6 + ln]


PASS, FAIL = 0, 0


def check(name, cond, extra=""):
    global PASS, FAIL
    if cond:
        PASS += 1
        print(f"  [PASS] {name}")
    else:
        FAIL += 1
        print(f"  [FAIL] {name}  {extra}")


def t1():
    print("T1 PING / PONG")
    d = Device()
    cmd, seq, pay = parse_reply(d.handle(C_PING, 0x11, b''))
    check("命令码 = PONG", cmd == C_PONG, f"cmd={hex(cmd)}")
    check("SEQ 回填", seq == 0x11, f"seq={seq}")
    check("负载 4 字节", len(pay) == 4, f"len={len(pay)}")
    magic = struct.unpack('<H', pay[:2])[0]
    check("标识 = 'KB'", magic == INFO_MAGIC, f"magic={hex(magic)}")


def t2():
    print("T2 GET_INFO")
    d = Device()
    cmd, seq, pay = parse_reply(d.handle(C_GET_INFO, 3, b''))
    check("命令码", cmd == ACK(C_GET_INFO))
    check("负载 10 字节", len(pay) == 10, f"len={len(pay)}")
    check("magic", struct.unpack('<H', pay[:2])[0] == INFO_MAGIC)
    check("ver_major=1", pay[2] == 1)
    check("ver_minor=0", pay[3] == 0)
    check("组数=5", pay[4] == GROUPS, f"groups={pay[4]}")
    check("键数=9", pay[5] == KEYS, f"keys={pay[5]}")
    check("当前组=0", pay[6] == 0)
    check("无 Flash 记录=0", pay[7] == 0, f"has={pay[7]}")
    check("fw_build", struct.unpack('<H', pay[8:10])[0] == FW_BUILD)


def t3():
    print("T3 GET_MAP")
    d = Device()
    cmd, seq, pay = parse_reply(d.handle(C_GET_MAP, 1, bytes([0])))
    check("命令码", cmd == ACK(C_GET_MAP))
    check("负载 18 字节", len(pay) == 18, f"len={len(pay)}")
    vals = [struct.unpack('<H', pay[i * 2:i * 2 + 2])[0] for i in range(9)]
    check("组0 = 数字 1~9", vals == [0x1E, 0x1F, 0x20, 0x21, 0x22,
                                    0x23, 0x24, 0x25, 0x26], f"vals={[hex(v) for v in vals]}")


def t4():
    print("T4 SET_KEY -> GET_MAP 往返")
    d = Device()
    # 给组 0 的 K1 设成 Ctrl+C = 0x0106
    pl = bytes([0, 1, 0x06, 0x01])
    cmd, seq, pay = parse_reply(d.handle(C_SET_KEY, 5, pl))
    check("命令码", cmd == ACK(C_SET_KEY))
    check("回读组号键号", pay[0] == 0 and pay[1] == 1, f"{pay[0]},{pay[1]}")
    check("回读值 = 0x0106", struct.unpack('<H', pay[2:4])[0] == 0x0106,
          f"v={hex(struct.unpack('<H', pay[2:4])[0])}")
    # 再读回来确认
    _, _, pay2 = parse_reply(d.handle(C_GET_MAP, 6, bytes([0])))
    v0 = struct.unpack('<H', pay2[0:2])[0]
    check("GET_MAP 里 K1 也是 0x0106", v0 == 0x0106, f"v={hex(v0)}")

    # 组合键全表再验一遍
    combos = [
        (0x0106, "Ctrl+C"), (0x0204, "Shift+A"), (0x043D, "Alt+F4"),
        (0x0329, "Ctrl+Shift+Esc"), (0x0A22, "Ctrl+Alt+K5"), (0x0808, "Win+E"),
        (0x1006, "右Ctrl+C"), (0x0707, "Ctrl+Shift+Alt+全0"),
    ]
    ok = True
    for v, nm in combos:
        d.handle(C_SET_KEY, 9, bytes([1, 3]) + struct.pack('<H', v))
        _, _, p = parse_reply(d.handle(C_GET_MAP, 10, bytes([1])))
        got = struct.unpack('<H', p[2 * 2:2 * 2 + 2])[0]
        if got != v:
            ok = False
            print(f"    {nm}: 期望 {hex(v)} 得到 {hex(got)}")
    check("8 组组合键往返全部一致", ok)


def t5():
    print("T5 SET_GROUP 整组写入")
    d = Device()
    vals = [0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0A, 0x0B, 0x0C]   # a~i
    pl = bytes([2]) + b''.join(struct.pack('<H', v) for v in vals)
    cmd, seq, pay = parse_reply(d.handle(C_SET_GROUP, 7, pl))
    check("命令码", cmd == ACK(C_SET_GROUP))
    check("负载空", len(pay) == 0)
    _, _, p = parse_reply(d.handle(C_GET_MAP, 8, bytes([2])))
    got = [struct.unpack('<H', p[i * 2:i * 2 + 2])[0] for i in range(9)]
    check("9 键全部写入", got == vals, f"got={[hex(v) for v in got]}")


def t6():
    print("T6 GET_NAME / SET_NAME")
    d = Device()
    cmd, seq, pay = parse_reply(d.handle(C_GET_NAME, 1, bytes([1])))
    check("命令码", cmd == ACK(C_GET_NAME))
    check("负载 17 字节", len(pay) == NAME_SLOT, f"len={len(pay)}")
    check("组1 名字 = ARROW", pay[:5] == b'ARROW', f"pay={pay[:8]}")

    # 改名（含不可打印字符，应被过滤）
    raw = b'MY\x01KEYS\x7F'          # 0x01 和 0x7F 应被丢掉 -> MYKEYS
    pl = bytes([1]) + raw
    _, _, pay2 = parse_reply(d.handle(C_SET_NAME, 2, pl))
    nm = pay2.split(b'\x00')[0]
    check("非法字符已过滤", nm == b'MYKEYS', f"nm={nm}")
    check("长度 17", len(pay2) == NAME_SLOT)

    # 超长名字（17 字节）应被截到 16
    long_raw = bytes([1]) + b'ABCDEFGHIJKLMNOPQRSTUVW'
    _, _, pay3 = parse_reply(d.handle(C_SET_NAME, 3, long_raw))
    # 请求长度 = 1+22 = 23 > 1+16，应报长度错
    check("超长名字报长度错", pay3[0] == E_LEN, f"代码={pay3[0]}")

    # 正好 16 字节
    ok_raw = bytes([1]) + b'ABCDEFGHIJKLMNOP'
    _, _, pay4 = parse_reply(d.handle(C_SET_NAME, 4, ok_raw))
    nm4 = pay4.split(b'\x00')[0]
    check("16 字节刚好收录", nm4 == b'ABCDEFGHIJKLMNOP', f"nm={nm4}")


def t7():
    print("T7 SET_GROUP_IDX")
    d = Device()
    cmd, seq, pay = parse_reply(d.handle(C_SET_GROUP_IDX, 1, bytes([3])))
    check("命令码", cmd == ACK(C_SET_GROUP_IDX))
    check("回读当前组 = 3", pay[0] == 3, f"g={pay[0]}")
    # 越界
    _, _, pay2 = parse_reply(d.handle(C_SET_GROUP_IDX, 2, bytes([9])))
    check("越界报参数错", pay2[0] == E_ARG, f"代码={pay2[0]}")


def t8():
    print("T8 SAVE / LOAD / RESET")
    d = Device()
    # 改点东西再存
    d.handle(C_SET_KEY, 1, bytes([0, 1, 0x06, 0x01]))     # K1 -> Ctrl+C
    d.handle(C_SET_GROUP_IDX, 2, bytes([2]))              # 切到组 2
    d.handle(C_SET_NAME, 3, bytes([0]) + b'CTRL')         # 组0 改名
    cmd, _, _ = parse_reply(d.handle(C_SAVE, 4, b''))
    check("SAVE 命令码", cmd == ACK(C_SAVE))

    # 破坏内存（模拟断电重启后 key_matrix 回到默认）
    d.map = [r[:] for r in d.default_map]
    d.names = d.default_names[:]
    d.group = 0
    check("破坏后 K1 = 默认", d.map[0][0] == 0x1E)

    # LOAD 应该恢复
    _, _, pay = parse_reply(d.handle(C_LOAD, 5, b''))
    check("LOAD 返回 1", pay[0] == 1, f"pay={pay}")
    check("K1 恢复 Ctrl+C", d.map[0][0] == 0x0106, f"v={hex(d.map[0][0])}")
    check("组名恢复 CTRL", d.names[0] == 'CTRL', f"nm={d.names[0]}")
    check("当前组恢复 2", d.group == 2, f"g={d.group}")

    # RESET 回默认
    d.handle(C_RESET, 6, b'')
    check("RESET 后 K1 回默认", d.map[0][0] == 0x1E, f"v={hex(d.map[0][0])}")
    check("RESET 后组名回默认", d.names[0] == '123', f"nm={d.names[0]}")

    # GET_INFO 的 has_record 位
    _, _, info = parse_reply(d.handle(C_GET_INFO, 7, b''))
    check("GET_INFO has_record = 1", info[7] == 1, f"has={info[7]}")


def t9():
    print("T9 错误路径")
    d = Device()
    # 未知命令
    _, _, pay = parse_reply(d.handle(0x55, 1, b''))
    check("未知命令 -> E_CMD", pay[0] == E_CMD, f"代码={pay[0]}")
    # 长度错
    _, _, pay = parse_reply(d.handle(C_SET_KEY, 2, bytes([0, 1, 0x06])))
    check("SET_KEY 长度错 -> E_LEN", pay[0] == E_LEN, f"代码={pay[0]}")
    # 参数越界
    _, _, pay = parse_reply(d.handle(C_SET_KEY, 3, bytes([9, 1, 0x06, 0x01])))
    check("组号越界 -> E_ARG", pay[0] == E_ARG, f"代码={pay[0]}")
    _, _, pay = parse_reply(d.handle(C_SET_KEY, 4, bytes([0, 0, 0x06, 0x01])))
    check("键号 0 -> E_ARG", pay[0] == E_ARG, f"代码={pay[0]}")
    _, _, pay = parse_reply(d.handle(C_SET_KEY, 5, bytes([0, 10, 0x06, 0x01])))
    check("键号 10 -> E_ARG", pay[0] == E_ARG, f"代码={pay[0]}")
    # GET_MAP 长度错
    _, _, pay = parse_reply(d.handle(C_GET_MAP, 6, b''))
    check("GET_MAP 空负载 -> E_LEN", pay[0] == E_LEN, f"代码={pay[0]}")
    # 错误应答的 SEQ 也要回填
    fr = d.handle(0x55, 0xAB, b'')
    check("错误应答 SEQ 回填", fr[3] == 0xAB, f"seq={fr[3]}")
    check("错误应答命令码 = 0x7F", fr[2] == C_ERROR, f"cmd={hex(fr[2])}")


if __name__ == '__main__':
    print("=" * 62)
    print(" kmap_server 业务层协议一致性测试")
    print("=" * 62)
    for t in (t1, t2, t3, t4, t5, t6, t7, t8, t9):
        t()
        print()
    print("=" * 62)
    print(f" 结果：PASS={PASS}  FAIL={FAIL}")
    print("=" * 62)
    raise SystemExit(1 if FAIL else 0)
