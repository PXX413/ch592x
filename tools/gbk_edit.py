#!/usr/bin/env python3
# -*- coding: gbk -*-
"""
gbk_edit.py —— 在 GBK / UTF-8 源码上做"字节安全"的字面量替换。

背景：本工程源码编码混用（ui/ User/ games/ rf24/ hardware/tft_* 是 GBK；
      hardware/key_matrix.* keyb oard/kmap_* keyboard/usb_* 是 UTF-8）。
      直接用 UTF-8 编辑器改写 GBK 文件会把中文注释变成乱码，所以统一走这个脚本。

用法：
    from gbk_edit import Ed, show
    e = Ed('ui/ui_pages.c')          # 自动探测编码
    e.sub("old", "new")              # 必须命中且只命中 1 次，否则报错
    e.sub("a", "b", count=3)         # 指定命中次数
    e.sub_opt("a", "b")              # 允许 0 次
    e.save()                         # 写回；编码/换行/BOM 保持原样

要点：按字节读入 -> surrogateescape 解码 -> 替换 -> 同编码写回，
      未触及部分字节完全不变（含 CRLF/LF、尾随空白）。
"""
import os
import sys

try:
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
except Exception:
    pass

UTF8_HINT = {
    "key_matrix.c", "key_matrix.h",
    "kmap_proto.c", "kmap_proto.h", "kmap_server.c", "kmap_server.h",
    "kmap_store.c", "kmap_store.h", "usb_cdc.c", "usb_cdc.h",
    "usb_desc_cdc.c", "usb_desc_cdc.h", "usb_kbd.c",
    "usb_kbd_port.h", "usb_kbd_port_ch592.c",
}

LOG = []


def guess_encoding(path):
    if os.path.basename(path) in UTF8_HINT:
        return "utf-8"
    data = open(path, "rb").read()
    try:
        data.decode("utf-8")
        return "utf-8"
    except UnicodeDecodeError:
        return "gbk"


class Ed(object):
    def __init__(self, path, encoding=None, verbose=True):
        self.path = path
        self.encoding = encoding or guess_encoding(path)
        self.raw_before = open(path, "rb").read()
        self.text = self.raw_before.decode(self.encoding, "surrogateescape")
        self.verbose = verbose
        self.changes = []

    def sub(self, old, new, count=1, label=""):
        n = self.text.count(old)
        if n != count:
            raise AssertionError(
                "%s: 期望命中 %d 次，实际 %d 次\n--- 片段 ---\n%s"
                % (self.path, count, n, old[:400]))
        self.text = self.text.replace(old, new, count)
        self.changes.append((old, new, count))
        if self.verbose:
            print("  [%s] %s替换 %d 处: %s" % (
                self.path, (label + " ") if label else "", count,
                old.strip().splitlines()[0][:70]))
        return self

    def sub_opt(self, old, new, count=None, label=""):
        n = self.text.count(old)
        if n == 0:
            if self.verbose:
                print("  [%s] %s跳过(未命中): %s" % (
                    self.path, (label + " ") if label else "",
                    old.strip().splitlines()[0][:70]))
            return self
        if (count is not None) and (n != count):
            raise AssertionError("%s: 期望 %d 次，实际 %d 次" % (self.path, count, n))
        self.text = self.text.replace(old, new)
        self.changes.append((old, new, n))
        if self.verbose:
            print("  [%s] %s替换 %d 处: %s" % (
                self.path, (label + " ") if label else "", n,
                old.strip().splitlines()[0][:70]))
        return self

    def save(self):
        out = self.text.encode(self.encoding, "surrogateescape")
        if out == self.raw_before:
            if self.verbose:
                print("  [%s] 无变化，未写入" % self.path)
            return 0
        open(self.path, "wb").write(out)
        LOG.append((self.path, self.encoding, len(self.changes)))
        if self.verbose:
            print("  [%s] 已写回 (%s, %d->%d 字节, %d 处替换)" % (
                self.path, self.encoding, len(self.raw_before), len(out),
                len(self.changes)))
        return len(self.changes)


def show(path, a=1, b=None, encoding=None):
    enc = encoding or guess_encoding(path)
    lines = open(path, "rb").read().decode(enc, "surrogateescape").splitlines()
    b = b or len(lines)
    print("=" * 12, "%s [%d-%d] (%s)" % (path, a, b, enc), "=" * 12)
    for i in range(a - 1, min(b, len(lines))):
        print("%5d| %s" % (i + 1, lines[i]))


def backup(path, stamp):
    """按工程既有习惯生成 .bak-<stamp> 备份（已存在则不覆盖）。"""
    dst = "%s.bak-%s" % (path, stamp)
    if not os.path.exists(dst):
        data = open(path, "rb").read()
        open(dst, "wb").write(data)
        print("  备份 -> %s" % dst)
    return dst


if __name__ == "__main__":
    a = sys.argv[1:]
    if a:
        show(a[0], int(a[1]) if len(a) > 1 else 1,
             int(a[2]) if len(a) > 2 else None)
