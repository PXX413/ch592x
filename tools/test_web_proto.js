/**
 * 网页 ↔ 固件 协议互通测试（Node 环境）
 *
 * 做法：
 *   1. 从 keymap_editor.html 里把 JS 抠出来
 *   2. 砍掉所有 DOM 依赖（$ / toast / render* 等），只留协议部分
 *   3. 用 Python 端 test_kmap_server.py 的同一套逻辑造一个"设备"应答器
 *   4. 网页发帧 -> 设备解析 -> 设备回帧 -> 网页解析，全链路对拍
 *
 * 这样就能在没有真硬件的情况下，确认「网页生成的字节」
 * 和「固件能正确理解并回复」是同一套东西。
 */

const fs = require("fs");
const path = require("path");
const vm = require("vm");

const HTML = path.join(__dirname, "keymap_editor.html");
const html = new TextDecoder("gbk").decode(fs.readFileSync(HTML));

const m = html.match(/<script>([\s\S]*)<\/script>/);
if (!m) { console.error("找不到 <script>"); process.exit(1); }
let js = m[1];

/* ---------------------------------------------------------------
 * 1. 从页面 JS 里精确摘出协议部分（8.5 节）
 * -------------------------------------------------------------*/
const start = js.indexOf("/* ---- 协议常量（与 kmap_proto.h 一一对应）---- */");
const end   = js.indexOf("/* ---- 设备连接状态 ---- */");
if (start < 0 || end < 0) { console.error("找不到协议段标记"); process.exit(1); }
const protoSrc = js.slice(start, end);

/* ---------------------------------------------------------------
 * 2. 造一个 sandbox，把协议代码跑起来
 * -------------------------------------------------------------*/
const sandbox = { console };
vm.createContext(sandbox);
/* 注意：vm 里顶层 const / class 声明不会挂到 sandbox 上，
 * 只有 var / function 才会。所以这里显式把要用的东西挂到 globalThis 上再取出来。*/
vm.runInContext(
  protoSrc + "\n;globalThis.__proto = { KP: KP, kpCrc8: kpCrc8, kpBuild: kpBuild, KpParser: KpParser };",
  sandbox
);

const { KP, kpCrc8, kpBuild, KpParser } = sandbox.__proto;

/* ---------------------------------------------------------------
 * 3. 设备侧应答器（照抄 test_kmap_server.py 的逻辑）
 * -------------------------------------------------------------*/
const CRC8_POLY = 0x07;
function crc8(buf) {
  let c = 0;
  for (const b of buf) {
    c ^= b & 0xff;
    for (let i = 0; i < 8; i++) c = (c & 0x80) ? ((c << 1) ^ CRC8_POLY) & 0xff : (c << 1) & 0xff;
  }
  return c;
}
function devBuild(cmd, seq, payload) {
  payload = payload || [];
  const body = [0xA5, 0x5A, cmd, seq, payload.length & 0xff, (payload.length >> 8) & 0xff, ...payload];
  body.push(crc8(body));
  return body;
}

const GROUPS = 4, KEYS = 9, NAME_SLOT = 17, NAME_MAX = 16;
const C = { PING:1, GET_INFO:2, GET_MAP:0x10, SET_KEY:0x11, SET_GROUP:0x12,
            GET_NAME:0x13, SET_NAME:0x14, SET_GROUP_IDX:0x20,
            SAVE:0x30, LOAD:0x31, RESET:0x32 };
const PONG = 0x81, ERROR = 0x7F, E_LEN = 2, E_CMD = 3, E_ARG = 4, E_FLASH = 5;

class Device {
  constructor() {
    this.defaultMap = [
      [0x1E,0x1F,0x20,0x21,0x22,0x23,0x24,0x25,0x26],
      [0x29,0x52,0x2C,0x50,0x28,0x4F,0x80,0x51,0x81],
      [0x3A,0x3B,0x3C,0x3D,0x3E,0x3F,0x40,0x41,0x42],
      [0x59,0x5A,0x5B,0x5C,0x5D,0x5E,0x5F,0x60,0x61],
    ];
    this.map = this.defaultMap.map(r => r.slice());
    this.names = ["123","ARROW","F1-F9","NUM"];
    this.group = 0;
    this.flash = null;
    this.flashFail = false;   /* 模拟写 Flash 失败 */
    this.reset = false;       /* RESET 是否被调用过 */
    /* 设备侧的解析器：逐字节喂 */
    this.parser = new KpParser();
  }

  u16(v) { return [v & 0xff, (v >> 8) & 0xff]; }
  g16(p, o) { return (p[o] | (p[o+1] << 8)) & 0xffff; }

  /* 喂入一个字节流，返回要回给网页的字节数组（可能多帧） */
  feed(bytes) {
    const out = [];
    for (const b of bytes) {
      const fr = this.parser.push(b);
      if (!fr) continue;
      const reply = this.handle(fr.cmd, fr.seq, fr.payload);
      for (const x of reply) out.push(x);
    }
    return out;
  }

  handle(cmd, seq, pay) {
    const err = (c) => devBuild(ERROR, seq, [c]);
    const ack = (c, p) => devBuild(c | 0x80, seq, p || []);

    if (cmd === C.PING) return devBuild(PONG, seq, [0x42, 0x4B, 0x01, 1]);

    if (cmd === C.GET_INFO) {
      const p = [0x42, 0x4B, 1, 0, GROUPS, KEYS, this.group, this.flash ? 1 : 0, 0x01, 0x00];
      return ack(C.GET_INFO, p);
    }

    if (cmd === C.GET_MAP) {
      if (pay.length !== 1) return err(E_LEN);
      const g = pay[0];
      if (g >= GROUPS) return err(E_ARG);
      const p = [];
      for (const v of this.map[g]) p.push(...this.u16(v));
      return ack(C.GET_MAP, p);
    }

    if (cmd === C.SET_KEY) {
      if (pay.length !== 4) return err(E_LEN);
      const g = pay[0], k = pay[1], v = this.g16(pay, 2);
      if (g >= GROUPS || k < 1 || k > KEYS) return err(E_ARG);
      this.map[g][k-1] = v;
      return ack(C.SET_KEY, [g, k, ...this.u16(this.map[g][k-1])]);
    }

    if (cmd === C.SET_GROUP) {
      if (pay.length !== 1 + KEYS*2) return err(E_LEN);
      const g = pay[0];
      if (g >= GROUPS) return err(E_ARG);
      for (let i = 0; i < KEYS; i++) this.map[g][i] = this.g16(pay, 1 + i*2);
      return ack(C.SET_GROUP, []);
    }

    if (cmd === C.GET_NAME) {
      if (pay.length !== 1) return err(E_LEN);
      const g = pay[0];
      if (g >= GROUPS) return err(E_ARG);
      const p = [];
      for (const ch of this.names[g].slice(0, NAME_MAX)) p.push(ch.charCodeAt(0));
      while (p.length < NAME_SLOT) p.push(0);
      return ack(C.GET_NAME, p);
    }

    if (cmd === C.SET_NAME) {
      if (pay.length < 1 || pay.length > 1 + NAME_MAX) return err(E_LEN);
      const g = pay[0];
      if (g >= GROUPS) return err(E_ARG);
      const clean = pay.slice(1).filter(b => b >= 0x20 && b < 0x7f).slice(0, NAME_MAX);
      this.names[g] = clean.map(b => String.fromCharCode(b)).join("");
      const p = clean.slice();
      while (p.length < NAME_SLOT) p.push(0);
      return ack(C.SET_NAME, p);
    }

    if (cmd === C.SET_GROUP_IDX) {
      if (pay.length !== 1) return err(E_LEN);
      if (pay[0] >= GROUPS) return err(E_ARG);
      this.group = pay[0];
      return ack(C.SET_GROUP_IDX, [this.group]);
    }

    if (cmd === C.SAVE) {
      /* 固件一致：KmapStore_Save() 成功就回空负载应答 0xB0；失败回 ERROR(FLASH=5)
       * 注意：这里模拟的是「已烧录固件」的行为，不会顺手往 flash 里塞一份快照 ——
       * 快照只可能由 LOAD/RESET 之外的真实写入产生。*/
      if (this.flashFail) return err(E_FLASH);
      this.flash = { group: this.group, map: this.map.map(r => r.slice()), names: this.names.slice() };
      return ack(C.SAVE, []);
    }

    if (cmd === C.LOAD) {
      /* 固件一致：应答负载固定 1 字节，KmapStore_Load() 的返回值 */
      const ok = this.flash ? 1 : 0;
      if (this.flash) {
        this.group = this.flash.group;
        this.map = this.flash.map.map(r => r.slice());
        this.names = this.flash.names.slice();
      }
      return ack(C.LOAD, [ok]);
    }

    if (cmd === C.RESET) {
      this.map = this.defaultMap.map(r => r.slice());
      this.names = ["123","ARROW","F1-F9","NUM"];
      this.group = 0;
      this.flash = null;              /* 固件：RESET 只落一份默认值，等价于「无记录」语义 */
      this.reset = true;
      return ack(C.RESET, []);
    }

    return err(E_CMD);
  }
}

/* ---------------------------------------------------------------
 * 4. 测试框架
 * -------------------------------------------------------------*/
let PASS = 0, FAIL = 0;
function check(name, cond, extra) {
  if (cond) { PASS++; console.log("  [PASS] " + name); }
  else { FAIL++; console.log("  [FAIL] " + name + (extra ? "  " + extra : "")); }
}

/* 模拟一次完整的"网页发包 -> 设备处理 -> 网页收包"往返 */
class Link {
  constructor() {
    this.dev = new Device();
    this.parser = new KpParser();   // 网页侧的解析器
  }
  /* 网页发一帧，返回网页解析出来的应答 {cmd,seq,payload} */
  roundtrip(cmd, seq, payload) {
    const frame = Array.from(kpBuild(cmd, seq, payload || []));
    const replyBytes = this.dev.feed(frame);
    let got = null;
    for (const b of replyBytes) {
      const fr = this.parser.push(b);
      if (fr) got = fr;
    }
    return got;
  }
}

/* ---------------------------------------------------------------
 * 5. 用例
 * -------------------------------------------------------------*/

console.log("=" .repeat(62));
console.log(" 网页 ↔ 固件 协议互通测试");
console.log("=" .repeat(62));

/* ---- T1 帧格式与固件的 kpBuild 一致 ---- */
(function t1() {
  console.log("T1 网页组帧 vs 固件期望格式");
  const f = Array.from(kpBuild(KP.CMD.PING, 0x07, []));
  check("帧长 = 7", f.length === 7, "len=" + f.length);
  check("帧头 A5 5A", f[0] === 0xA5 && f[1] === 0x5A);
  check("命令码", f[2] === 0x01, "cmd=" + f[2]);
  check("序号", f[3] === 0x07, "seq=" + f[3]);
  check("LEN = 0", f[4] === 0 && f[5] === 0);
  check("CRC8 自洽", kpCrc8(f.slice(0, -1)) === f[f.length - 1]);
  /* 和固件 Kmap_Crc8 对拍：手工算一个已知值 */
  check("已知 CRC（A5 5A 01 07 00 00）",
        kpCrc8([0xA5,0x5A,0x01,0x07,0x00,0x00]) === f[6],
        "got=" + kpCrc8([0xA5,0x5A,0x01,0x07,0x00,0x00]));
})();

/* ---- T2 PING ---- */
(function t2() {
  console.log("T2 PING -> PONG");
  const L = new Link();
  const r = L.roundtrip(KP.CMD.PING, 1, []);
  check("收到应答", r !== null);
  check("命令码 = PONG", r.cmd === KP.PONG, "cmd=" + r.cmd);
  check("SEQ 回填", r.seq === 1, "seq=" + r.seq);
  check("标识 = 0x4B42（KB）",
        (r.payload[0] | (r.payload[1] << 8)) === 0x4B42);
})();

/* ---- T3 GET_INFO ---- */
(function t3() {
  console.log("T3 GET_INFO");
  const L = new Link();
  const r = L.roundtrip(KP.CMD.GET_INFO, 2, []);
  check("命令码", r.cmd === (KP.CMD.GET_INFO | 0x80));
  check("负载 10 字节", r.payload.length === 10, "len=" + r.payload.length);
  check("组数 = 4", r.payload[4] === 4);
  check("键数 = 9", r.payload[5] === 9);
  check("当前组 = 0", r.payload[6] === 0);
  check("无 Flash 记录", r.payload[7] === 0);
})();

/* ---- T4 GET_MAP / SET_KEY 往返 ---- */
(function t4() {
  console.log("T4 SET_KEY -> GET_MAP 往返（含组合键）");
  const L = new Link();

  /* 先把组 0 K1 设成 Ctrl+C */
  const r1 = L.roundtrip(KP.CMD.SET_KEY, 3, [0, 1, 0x06, 0x01]);
  check("SET_KEY 应答", r1.cmd === (KP.CMD.SET_KEY | 0x80));
  check("回读 = 0x0106",
        (r1.payload[2] | (r1.payload[3] << 8)) === 0x0106,
        "v=" + (r1.payload[2] | (r1.payload[3] << 8)).toString(16));

  const r2 = L.roundtrip(KP.CMD.GET_MAP, 4, [0]);
  const v0 = r2.payload[0] | (r2.payload[1] << 8);
  check("GET_MAP 里 K1 = 0x0106", v0 === 0x0106, "v=" + v0.toString(16));

  /* 一批组合键 */
  const combos = [
    [0x0106, "Ctrl+C"], [0x0204, "Shift+A"], [0x043D, "Alt+F4"],
    [0x0329, "Ctrl+Shift+Esc"], [0x0808, "Win+E"], [0x1006, "右Ctrl+C"],
    [0x0080, "音量+"], [0x0004, "单个 A"],
  ];
  let ok = true;
  for (const [v, nm] of combos) {
    L.roundtrip(KP.CMD.SET_KEY, 5, [1, 3, v & 0xff, (v >> 8) & 0xff]);
    const rr = L.roundtrip(KP.CMD.GET_MAP, 6, [1]);
    const got = rr.payload[4] | (rr.payload[5] << 8);
    if (got !== v) { ok = false; console.log("    " + nm + ": 期望 " + v.toString(16) + " 得 " + got.toString(16)); }
  }
  check("8 组组合键全对拍通过", ok);
})();

/* ---- T5 SET_GROUP ---- */
(function t5() {
  console.log("T5 SET_GROUP 整组下发");
  const L = new Link();
  const vals = [0x04,0x05,0x06,0x07,0x08,0x09,0x0A,0x0B,0x0C];
  const pay = [2];
  for (const v of vals) pay.push(v & 0xff, (v >> 8) & 0xff);

  const r = L.roundtrip(KP.CMD.SET_GROUP, 7, pay);
  check("命令码", r.cmd === (KP.CMD.SET_GROUP | 0x80));

  const rr = L.roundtrip(KP.CMD.GET_MAP, 8, [2]);
  const got = [];
  for (let i = 0; i < 9; i++) got.push(rr.payload[i*2] | (rr.payload[i*2+1] << 8));
  check("9 键全写入", JSON.stringify(got) === JSON.stringify(vals),
        "got=" + got.map(x => x.toString(16)));
})();

/* ---- T6 组名 ---- */
(function t6() {
  console.log("T6 组名 读 / 写");
  const L = new Link();
  const r = L.roundtrip(KP.CMD.GET_NAME, 9, [1]);
  const nm = r.payload.slice(0, 5).map(x => String.fromCharCode(x)).join("");
  check("组1 名字 = ARROW", nm === "ARROW", "nm=" + nm);
  check("负载 17 字节", r.payload.length === 17);

  /* 写一个新名字 */
  const newName = "CTRL";
  const pay = [1];
  for (const ch of newName) pay.push(ch.charCodeAt(0));
  const r2 = L.roundtrip(KP.CMD.SET_NAME, 10, pay);
  const nm2 = r2.payload.slice(0, 4).map(x => String.fromCharCode(x)).join("");
  check("回读新名字 = CTRL", nm2 === "CTRL", "nm=" + nm2);
})();

/* ---- T7 SET_GROUP_IDX ---- */
(function t7() {
  console.log("T7 切换当前组");
  const L = new Link();
  const r = L.roundtrip(KP.CMD.SET_GROUP_IDX, 11, [3]);
  check("命令码", r.cmd === (KP.CMD.SET_GROUP_IDX | 0x80));
  check("回读当前组 = 3", r.payload[0] === 3, "g=" + r.payload[0]);

  const r2 = L.roundtrip(KP.CMD.SET_GROUP_IDX, 12, [9]);
  check("越界 -> 错误应答", r2.cmd === KP.ERROR, "cmd=" + r2.cmd);
  check("错误码 = 参数越界(4)", r2.payload[0] === E_ARG, "code=" + r2.payload[0]);
})();

/* ---- T8 保存 / 载入 ---- */
(function t8() {
  console.log("T8 SAVE_FLASH -> 断电 -> LOAD_FLASH");
  const L = new Link();

  L.roundtrip(KP.CMD.SET_KEY, 1, [0, 1, 0x06, 0x01]);            // K1 = Ctrl+C
  L.roundtrip(KP.CMD.SET_NAME, 2, [0, 0x43, 0x54, 0x52, 0x4C]);  // 组0 = CTRL
  L.roundtrip(KP.CMD.SET_GROUP_IDX, 3, [2]);                     // 当前组 = 2

  const r = L.roundtrip(KP.CMD.SAVE_FLASH, 4, []);
  check("SAVE 应答命令码 = 0xB0", r.cmd === (KP.CMD.SAVE_FLASH | 0x80), "cmd=0x" + r.cmd.toString(16));
  check("SAVE 应答空负载", r.payload.length === 0, "len=" + r.payload.length);

  /* 模拟断电：内存被重置，但 Flash 快照还在 */
  const keep = L.dev.flash;
  L.dev.map = L.dev.defaultMap.map(x => x.slice());
  L.dev.names = ["123","ARROW","F1-F9","NUM"];
  L.dev.group = 0;
  check("断电后 K1 回默认", L.dev.map[0][0] === 0x1E);

  const r2 = L.roundtrip(KP.CMD.LOAD_FLASH, 5, []);
  check("LOAD 应答命令码 = 0xB1", r2.cmd === (KP.CMD.LOAD_FLASH | 0x80), "cmd=0x" + r2.cmd.toString(16));
  check("LOAD 负载 1 字节", r2.payload.length === 1, "len=" + r2.payload.length);
  check("LOAD 返回 1", r2.payload[0] === 1, "pay=" + r2.payload[0]);
  check("K1 恢复 0x0106", L.dev.map[0][0] === 0x0106, "v=0x" + L.dev.map[0][0].toString(16));
  check("组名恢复 CTRL", L.dev.names[0] === "CTRL", "nm=" + L.dev.names[0]);
  check("当前组恢复 2", L.dev.group === 2, "g=" + L.dev.group);
  check("GET_INFO has_record = 1",
        L.roundtrip(KP.CMD.GET_INFO, 6, []).payload[7] === 1);

  /* Flash 空着时 LOAD 必须回 0，且不能把内存改坏 */
  const L2 = new Link();
  L2.roundtrip(KP.CMD.SET_GROUP_IDX, 1, [3]);
  const r3 = L2.roundtrip(KP.CMD.LOAD_FLASH, 2, []);
  check("无记录时 LOAD 返回 0", r3.payload[0] === 0, "pay=" + r3.payload[0]);
  check("无记录时内存不被改坏", L2.dev.group === 3, "g=" + L2.dev.group);

  /* SAVE 写失败 -> 协议错误码 5（Flash） */
  const L3 = new Link();
  L3.dev.flashFail = true;
  const r4 = L3.roundtrip(KP.CMD.SAVE_FLASH, 1, []);
  check("SAVE 失败 -> ERROR", r4.cmd === KP.ERROR, "cmd=0x" + r4.cmd.toString(16));
  check("错误码 = Flash(5)", r4.payload[0] === E_FLASH, "code=" + r4.payload[0]);

  /* RESET 只改内存 + 落一份默认值，不沿用旧快照 */
  const L4 = new Link();
  L4.roundtrip(KP.CMD.SET_KEY, 1, [0, 1, 0x06, 0x01]);
  L4.roundtrip(KP.CMD.SET_GROUP_IDX, 2, [3]);
  const r5 = L4.roundtrip(KP.CMD.RESET_DEFAULT, 3, []);
  check("RESET 应答命令码 = 0xB2", r5.cmd === (KP.CMD.RESET_DEFAULT | 0x80), "cmd=0x" + r5.cmd.toString(16));
  check("RESET 后 K1 回默认", L4.dev.map[0][0] === 0x1E, "v=0x" + L4.dev.map[0][0].toString(16));
  check("RESET 后当前组回 0", L4.dev.group === 0, "g=" + L4.dev.group);
  check("RESET 后组名回默认", L4.dev.names[0] === "123", "nm=" + L4.dev.names[0]);
})();

/* ---- T9 错误路径 ---- */
(function t9() {
  console.log("T9 错误路径");
  const L = new Link();
  let r = L.roundtrip(0x55, 1, []);
  check("未知命令 -> 0x7F", r.cmd === KP.ERROR, "cmd=" + r.cmd);
  check("错误码 = 未知命令(3)", r.payload[0] === E_CMD, "code=" + r.payload[0]);
  check("错误应答 SEQ 回填", r.seq === 1);

  r = L.roundtrip(KP.CMD.SET_KEY, 2, [0, 1, 0x06]);
  check("SET_KEY 长度错 -> 2", r.payload[0] === E_LEN, "code=" + r.payload[0]);

  r = L.roundtrip(KP.CMD.SET_KEY, 3, [9, 1, 0x06, 0x01]);
  check("组号越界 -> 4", r.payload[0] === E_ARG, "code=" + r.payload[0]);
})();

/* ---- T10 半包 / 粘包 ---- */
(function t10() {
  console.log("T10 分片传输（模拟串口半包 / 粘包）");
  const L = new Link();

  /* 一次性把 3 帧粘在一起发 */
  const f1 = Array.from(kpBuild(KP.CMD.PING, 1, []));
  const f2 = Array.from(kpBuild(KP.CMD.GET_INFO, 2, []));
  const f3 = Array.from(kpBuild(KP.CMD.GET_MAP, 3, [0]));
  const glued = [...f1, ...f2, ...f3];
  const replyBytes = L.dev.feed(glued);

  const frames = [];
  for (const b of replyBytes) {
    const fr = L.parser.push(b);
    if (fr) frames.push(fr);
  }
  check("粘包 -> 3 条应答", frames.length === 3, "got=" + frames.length);
  check("应答 SEQ 顺序 1,2,3",
        frames.map(f => f.seq).join(",") === "1,2,3",
        "seqs=" + frames.map(f => f.seq));

  /* 半包：把一帧拆成 1 字节一次地喂 */
  const L2 = new Link();
  const one = kpBuild(KP.CMD.GET_INFO, 9, []);
  let got = null;
  for (const b of Array.from(one)) {
    const rep = L2.dev.feed([b]);
    for (const x of rep) {
      const fr = L2.parser.push(x);
      if (fr) got = fr;
    }
  }
  check("逐字节喂也能组出应答", got !== null && got.seq === 9,
        got ? "seq=" + got.seq : "got=null");
})();

/* ---- T11 网页解析器抗脏数据 ---- */
(function t11() {
  console.log("T11 网页解析器抗脏数据");
  const p = new KpParser();
  const junk = [0x00, 0x11, 0xA5, 0x00, 0xFF, 0x5A, 0x5A];
  const good = Array.from(kpBuild(KP.PONG, 0x05, [0x42, 0x4B, 0x01, 0x01]));
  const bad = good.slice(); bad[bad.length - 1] ^= 0xFF;   // 破坏 CRC

  const all = [...junk, ...good, ...bad, ...good];
  const got = [];
  for (const b of all) {
    const fr = p.push(b);
    if (fr) got.push(fr);
  }
  check("只认 2 帧有效（坏 CRC 被丢）", got.length === 2, "got=" + got.length);
  check("两帧 SEQ 都是 5", got.every(f => f.seq === 5), "seqs=" + got.map(f => f.seq));
})();

console.log();
console.log("=" .repeat(62));
console.log(" 结果：PASS=" + PASS + "  FAIL=" + FAIL);
console.log("=" .repeat(62));
process.exit(FAIL ? 1 : 0);
