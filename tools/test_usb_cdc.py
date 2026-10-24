#!/usr/bin/env python3
# -*- coding: gbk -*-
"""
usb_cdc 传输层逻辑仿真测试

把 usb_cdc.c 的核心逻辑（发送队列 + 跨包分片发送 + 协议解析串联）
用 Python 复刻一遍，验证：

  T1  单帧应答：入队 -> 一次取出，内容一致
  T2  跨包分片：应答 71 字节，调用者缓冲只有 16 字节
       -> 需要 5 次才能取完（16*4 + 7），拼回来必须和原帧一模一样
  T3  队列积压 + 队满丢最旧：连续入 12 帧（容量 8），
       取出来应该是最后 8 帧，且丢帧计数 = 4
  T4  半包输入：把一帧拆成 5 段分 5 次喂 UsbCdc_OnDataOut，
       只能产出 1 帧应答，不能多也不能少
  T5  粘包输入：3 帧一次性灌进去，必须产出 3 帧应答且顺序正确
"""

import struct

# ---------------------------------------------------------------- 协议常量
SOF0, SOF1 = 0xA5, 0x5A
CRC8_POLY = 0x07
MAX_PAYLOAD = 64
MAX_FRAME = 7 + MAX_PAYLOAD      # 71

TXQ_FRAMES = 8

# ---------------------------------------------------------------- CRC8
def crc8(data: bytes) -> int:
    crc = 0
    for b in data:
        crc ^= b
        for _ in range(8):
            if crc & 0x80:
                crc = ((crc << 1) ^ CRC8_POLY) & 0xFF
            else:
                crc = (crc << 1) & 0xFF
    return crc

def build_frame(cmd: int, seq: int, payload: bytes) -> bytes:
    body = bytes([SOF0, SOF1, cmd, seq]) + struct.pack('<H', len(payload)) + payload
    return body + bytes([crc8(body)])

# ---------------------------------------------------------------- 解析器（对应 Kmap_ParserPush）
class Parser:
    def __init__(self):
        self.buf = bytearray()
        self.frames_ok = 0
        self.frames_bad = 0

    def push(self, byte: int):
        """返回 (cmd, seq, payload) 或 None"""
        # --- 找帧头 ---
        if len(self.buf) < 2:
            self.buf.append(byte)
            if len(self.buf) == 1 and self.buf[0] != SOF0:
                self.buf.clear()
            elif len(self.buf) == 2 and self.buf[1] != SOF1:
                # 第二个字节不是 SOF1：把当前字节留下重新当 SOF0 试
                self.buf = bytearray([byte]) if byte == SOF0 else bytearray()
            if len(self.buf) < 5:
                return None
            # 落到下面继续走

        # 已在收集态
        if len(self.buf) == 2 and self.buf[0] == SOF0 and self.buf[1] == SOF1:
            self.buf.append(byte)
        elif len(self.buf) >= 2 and self.buf[0] == SOF0 and self.buf[1] == SOF1:
            self.buf.append(byte)
        else:
            # 不在收集态，重新起头
            self.buf = bytearray([byte]) if byte == SOF0 else bytearray()
            return None

        if len(self.buf) < 6:
            return None

        plen = self.buf[4] | (self.buf[5] << 8)
        if plen > MAX_PAYLOAD:
            # 长度非法，丢头一个字节重新同步
            self.buf = self.buf[1:]
            self.frames_bad += 1
            return None

        total = 6 + plen + 1
        if len(self.buf) < total:
            return None

        frame = bytes(self.buf[:total])
        self.buf = self.buf[total:]

        if crc8(frame[:-1]) != frame[-1]:
            self.frames_bad += 1
            return None

        self.frames_ok += 1
        return (frame[2], frame[3], frame[6:6+plen])


# ---------------------------------------------------------------- usb_cdc.c 逻辑复刻
class UsbCdc:
    def __init__(self):
        self.parser = Parser()
        self.txq = [None] * TXQ_FRAMES
        self.txq_head = 0
        self.txq_tail = 0
        self.txq_count = 0
        self.tx_cur = b''
        self.tx_cur_off = 0
        self.tx_frames = 0
        self.tx_drops = 0
        self.rx_bytes = 0
        self.replies = []          # 记录业务层产出的应答（替代 KmapServer）

    # ---- 队列 ----
    def q_push(self, frame: bytes):
        if not frame:
            return
        if self.txq_count >= TXQ_FRAMES:
            self.txq_head = (self.txq_head + 1) % TXQ_FRAMES
            self.txq_count -= 1
            self.tx_drops += 1
        self.txq[self.txq_tail] = frame
        self.txq_tail = (self.txq_tail + 1) % TXQ_FRAMES
        self.txq_count += 1

    # ---- 收 ----
    def on_data_out(self, buf: bytes):
        self.rx_bytes += len(buf)
        for b in buf:
            fr = self.parser.push(b)
            if fr is None:
                continue
            cmd, seq, payload = fr
            # 业务层：这里简单做一个"把 payload 长度 + 100 原样回"的假应答
            reply_payload = bytes([len(payload) & 0xFF])
            reply = build_frame(cmd | 0x80, seq, reply_payload)
            self.replies.append((cmd, seq, payload))
            self.q_push(reply)

    # ---- 发 ----
    def poll_tx(self, cap: int):
        if self.tx_cur_off < len(self.tx_cur):
            n = min(len(self.tx_cur) - self.tx_cur_off, cap)
            out = self.tx_cur[self.tx_cur_off:self.tx_cur_off + n]
            self.tx_cur_off += n
            if self.tx_cur_off >= len(self.tx_cur):
                self.tx_cur_off = 0
                self.tx_cur = b''
                self.tx_frames += 1
            return out

        if self.txq_count == 0:
            return b''

        self.tx_cur = self.txq[self.txq_head]
        self.tx_cur_off = 0
        self.txq_head = (self.txq_head + 1) % TXQ_FRAMES
        self.txq_count -= 1

        n = min(len(self.tx_cur) - self.tx_cur_off, cap)
        out = self.tx_cur[self.tx_cur_off:self.tx_cur_off + n]
        self.tx_cur_off += n
        if self.tx_cur_off >= len(self.tx_cur):
            self.tx_cur_off = 0
            self.tx_cur = b''
            self.tx_frames += 1
        return out

    def tx_pending(self):
        return self.txq_count + (1 if self.tx_cur_off < len(self.tx_cur) else 0)


# ================================================================ 测试
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
    print("T1 单帧应答：入队 -> 一次取出")
    d = UsbCdc()
    req = build_frame(0x01, 0x00, b'')          # PING, 无负载
    d.on_data_out(req)
    check("请求被解析", len(d.replies) == 1, f"replies={d.replies}")
    out = b''
    while True:
        p = d.poll_tx(64)
        if not p:
            break
        out += p
    check("应答取回", len(out) > 0, f"len={len(out)}")
    # 应答应是 PONG(0x81)
    check("应答命令码", out[2] == 0x81, f"cmd={hex(out[2])}")
    check("CRC 正确", crc8(out[:-1]) == out[-1])
    check("待发清零", d.tx_pending() == 0, f"pending={d.tx_pending()}")

def t2():
    print("T2 跨包分片：71 字节应答 / 16 字节缓冲")
    d = UsbCdc()
    # 造一个 64 字节负载的请求，应答也是 71 字节
    big_payload = bytes(range(64))
    req = build_frame(0x12, 0x07, big_payload)
    d.on_data_out(req)

    # 手工往队列塞一个 71 字节的"应答"来精确测分片
    d2 = UsbCdc()
    full = build_frame(0x92, 0x07, bytes(64))
    check("构造的整帧 71 字节", len(full) == 71, f"len={len(full)}")
    d2.q_push(full)

    out = b''
    rounds = 0
    while True:
        p = d2.poll_tx(16)
        rounds += 1
        if not p:
            break
        out += p
        if rounds > 20:
            break
    check("拼回内容一致", out == full, f"out_len={len(out)} full_len={len(full)}")
    # 71 / 16 = 4 满包 + 1 个 7 字节包 = 5 次，第 6 次返回空
    check("分片次数 = 5", rounds == 6, f"rounds={rounds}（含最后一次空取）")
    check("分片后帧计数 = 1", d2.tx_frames == 1, f"tx_frames={d2.tx_frames}")
    check("待发清零", d2.tx_pending() == 0)

def t3():
    print("T3 队列积压 + 队满丢最旧")
    d = UsbCdc()
    for i in range(12):
        d.q_push(build_frame(0x10 + (i & 0x0F), i, bytes([i])))
    check("丢帧计数 = 4", d.tx_drops == 4, f"drops={d.tx_drops}")
    check("队列里 8 帧", d.txq_count == 8, f"count={d.txq_count}")

    seqs = []
    while True:
        p = d.poll_tx(64)
        if not p:
            break
        seqs.append(p[3])
    check("取出 8 帧", len(seqs) == 8, f"len={len(seqs)}")
    check("保留的是最后 8 帧 (seq 4..11)", seqs == list(range(4, 12)), f"seqs={seqs}")

def t4():
    print("T4 半包输入：一帧拆成 5 段")
    d = UsbCdc()
    req = build_frame(0x11, 0x22, bytes([0, 1, 0x06, 0x01]))
    n = len(req)
    cuts = [0, 3, 5, 6, n]        # 切成 4 段（+末段）
    segs = [req[cuts[i]:cuts[i+1]] for i in range(len(cuts) - 1)]
    check("切成 4 段", len(segs) == 4, f"segs={len(segs)}")
    for s in segs:
        d.on_data_out(s)
    check("只产出 1 帧", len(d.replies) == 1, f"replies={len(d.replies)}")
    if d.replies:
        cmd, seq, pl = d.replies[0]
        check("命令码正确", cmd == 0x11, f"cmd={hex(cmd)}")
        check("序号正确", seq == 0x22, f"seq={seq}")
        check("负载正确", pl == bytes([0, 1, 0x06, 0x01]), f"pl={pl.hex()}")

def t5():
    print("T5 粘包输入：3 帧一次性灌入")
    d = UsbCdc()
    frames = b''.join(
        build_frame(0x10 + i, i, bytes([i, i + 1])) for i in range(3)
    )
    d.on_data_out(frames)
    check("产出 3 帧", len(d.replies) == 3, f"len={len(d.replies)}")
    seqs = [r[1] for r in d.replies]
    check("顺序正确 0,1,2", seqs == [0, 1, 2], f"seqs={seqs}")
    check("队列里 3 个应答", d.txq_count == 3, f"count={d.txq_count}")

def t6():
    print("T6 脏数据抗性：帧头间塞垃圾 + CRC 破坏")
    d = UsbCdc()
    junk = bytes([0x00, 0x11, 0xA5, 0x00, 0xFF, 0x5A, 0x5A])   # 含假帧头
    good = build_frame(0x02, 0x05, bytes([0xAA]))
    bad = bytearray(build_frame(0x02, 0x06, bytes([0xBB])))
    bad[-1] ^= 0xFF                                            # 破坏 CRC
    d.on_data_out(junk + good + bytes(bad) + good)
    check("只认 2 帧有效", len(d.replies) == 2, f"replies={len(d.replies)}")
    seqs = [r[1] for r in d.replies]
    check("有效帧都是 seq=5", seqs == [5, 5], f"seqs={seqs}")
    check("解析器记了 1 个坏帧", d.parser.frames_bad == 1, f"bad={d.parser.frames_bad}")

def t7():
    print("T7 大量随机分片鲁棒性（模拟串口随机切分）")
    import random
    random.seed(20261010)
    d = UsbCdc()
    frames = [build_frame(0x10, i, bytes([i] * (i % 20))) for i in range(30)]
    stream = b''.join(frames)
    # 随机切分成 1~9 字节的小块
    pos = 0
    while pos < len(stream):
        n = random.randint(1, 9)
        d.on_data_out(stream[pos:pos + n])
        pos += n
    check("30 帧全部解析", len(d.replies) == 30, f"got={len(d.replies)}")
    check("序号 0..29", [r[1] for r in d.replies] == list(range(30)))
    check("字节计数正确", d.rx_bytes == len(stream), f"{d.rx_bytes} vs {len(stream)}")
    # 全部取完
    total_out = 0
    while True:
        p = d.poll_tx(64)
        if not p:
            break
        total_out += len(p)
    check("应答全部取出", total_out > 0 and d.tx_pending() == 0, f"out={total_out}")


if __name__ == '__main__':
    print("=" * 62)
    print(" usb_cdc 传输层逻辑仿真测试")
    print("=" * 62)
    for t in (t1, t2, t3, t4, t5, t6, t7):
        t()
        print()
    print("=" * 62)
    print(f" 结果：PASS={PASS}  FAIL={FAIL}")
    print("=" * 62)
    raise SystemExit(1 if FAIL else 0)
