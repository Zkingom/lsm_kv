#!/usr/bin/env python3
"""Generate the handbook's architecture diagrams as SVG (source of truth for the PNGs).

Usage: python3 gen_diagrams.py        # writes {write,read}_path_{zh,en}.svg next to this file
The PNGs are rendered from these SVGs by ../build.sh (headless Chromium).

Mermaid / Graphviz were not available in the environment where the handbook was built,
so the diagrams are emitted directly as SVG. Every label is checked against the code:
  write path: src/db.cpp:122-168, src/db_background.cpp:52-113
  read path : src/db.cpp:176-191, src/version.cpp:20-36, src/sstable.cpp:44-57
"""
import os
from xml.sax.saxutils import escape

W = 460
FONT = {"zh": "'WenQuanYi Zen Hei', sans-serif", "en": "'Liberation Sans', sans-serif"}
C = {
    "api": ("#eef2f7", "#44546a"),
    "mem": ("#e3f0e3", "#3c763d"),
    "disk": ("#fdf0d5", "#9a6a00"),
    "bg": ("#e8e4f3", "#5b4a9c"),
    "note": ("#ffffff", "#888888"),
}


class Svg:
    def __init__(self, lang, height):
        self.lang, self.h, self.out = lang, height, []

    def box(self, x, y, w, h, kind, title, sub=None, dashed=False):
        fill, stroke = C[kind]
        dash = ' stroke-dasharray="5 3"' if dashed else ""
        self.out.append(
            f'<rect x="{x}" y="{y}" width="{w}" height="{h}" rx="5" fill="{fill}" '
            f'stroke="{stroke}" stroke-width="1.3"{dash}/>'
        )
        cx = x + w / 2
        if sub:
            self.text(cx, y + h / 2 - 3, title, bold=True)
            self.text(cx, y + h / 2 + 13, sub, size=11.5, color="#333")
        else:
            self.text(cx, y + h / 2 + 5, title, bold=True)

    def text(self, x, y, s, size=13.5, bold=False, color="#111", anchor="middle"):
        weight = ' font-weight="bold"' if bold else ""
        self.out.append(
            f'<text x="{x}" y="{y}" font-size="{size}" fill="{color}" text-anchor="{anchor}"{weight}>'
            f"{escape(s)}</text>"
        )

    def arrow(self, x1, y1, x2, y2, label=None, color="#444", lx=None, anchor="start"):
        self.out.append(
            f'<line x1="{x1}" y1="{y1}" x2="{x2}" y2="{y2}" stroke="{color}" stroke-width="1.5" '
            f'marker-end="url(#arr)"/>'
        )
        if label:
            x = lx if lx is not None else max(x1, x2) + 8
            self.text(x, (y1 + y2) / 2 + 4, label, size=11.5, color="#333", anchor=anchor)

    def render(self):
        return (
            f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 {W} {self.h}" '
            f'width="{W}" height="{self.h}" font-family="{FONT[self.lang]}">\n'
            '<defs><marker id="arr" viewBox="0 0 10 10" refX="9" refY="5" markerWidth="7" '
            'markerHeight="7" orient="auto"><path d="M0,0 L10,5 L0,10 z" fill="#444"/></marker></defs>\n'
            f'<rect width="{W}" height="{self.h}" fill="#ffffff"/>\n' + "\n".join(self.out) + "\n</svg>\n"
        )


T = {
    "zh": {
        "w_title": "写路径（Put / Delete）",
        "w_api": ("Put / Delete", "任意线程；write_mu_ 串行化 = 单写者"),
        "w_room": ("MakeRoomForWrite", "imm_ 未刷完 或 L0 ≥ 12：等待（写停顿）"),
        "w_wal": ("① WAL 追加  NNNNNN.log", "[crc32][len][type|klen|key|vlen|value]"),
        "w_mem": ("② MemTable mem_（跳表）", "独占锁插入；同键覆盖，删除写墓碑"),
        "w_full": "≥ 4MB：冻结 + 换新 WAL",
        "w_imm": ("Immutable MemTable imm_", "只读，等待后台刷盘"),
        "w_bg": "后台线程（唯一）：放开 mu_ 做 I/O",
        "w_sst": ("③ 写 L0 SSTable 并 fsync", "数据块 | 布隆 | 索引 | footer"),
        "w_man": ("④ 原子重写 MANIFEST", "tmp→fsync→rename→fsync 目录"),
        "w_del": ("⑤ 装新 Version，删旧 WAL", "编号 < MinLiveLogNumber()"),
        "w_cmp": ("Leveled Compaction  L0→L1→L2→L3", "分数 ≥ 1 触发；trivial move；墓碑回收"),
        "w_sync": "sync_wal=true 时 fdatasync",
        "r_title": "读路径（Get）",
        "r_api": ("Get(key)", "任意线程，可并发"),
        "r_sv": ("拷贝 SuperVersion", "mu_ 内只拷 3 个 shared_ptr，随即放锁"),
        "r_mem": ("MemTable mem_", "共享锁；跳表查找"),
        "r_imm": ("Immutable MemTable imm_", "可能为空"),
        "r_l0": ("L0：按 新→旧 逐个文件", "文件间键范围可重叠"),
        "r_ln": ("L1 → L2 → L3", "每层二分定位至多 1 个文件"),
        "r_miss": "kNotFound：继续",
        "r_res": ("首个 kFound / kDeleted 即返回", "全部 kNotFound → 键不存在"),
        "r_tab": "单个 SSTable 内（Table::Get）",
        "r_t1": "1  键在 [smallest, largest] 内？",
        "r_t2": "2  布隆过滤器（常驻内存）",
        "r_t3": "3  索引二分 → 定位 1 个数据块",
        "r_t4": "4  1 次 pread + CRC 校验",
        "r_t5": "5  整块解析后二分",
    },
    "en": {
        "w_title": "Write path (Put / Delete)",
        "w_api": ("Put / Delete", "any thread; write_mu_ serializes = single writer"),
        "w_room": ("MakeRoomForWrite", "imm_ unflushed or L0 ≥ 12: wait (write stall)"),
        "w_wal": ("① Append to WAL  NNNNNN.log", "[crc32][len][type|klen|key|vlen|value]"),
        "w_mem": ("② MemTable mem_ (skip list)", "exclusive lock; overwrite in place; tombstones"),
        "w_full": "≥ 4MB: freeze + new WAL",
        "w_imm": ("Immutable MemTable imm_", "read-only, waiting for flush"),
        "w_bg": "one background thread; I/O without mu_",
        "w_sst": ("③ Write L0 SSTable, fsync", "data blocks | bloom | index | footer"),
        "w_man": ("④ Atomic MANIFEST swap", "tmp→fsync→rename→fsync dir"),
        "w_del": ("⑤ Install; drop old WAL", "number < MinLiveLogNumber()"),
        "w_cmp": ("Leveled compaction  L0→L1→L2→L3", "score ≥ 1; trivial move; tombstone GC"),
        "w_sync": "fdatasync if sync_wal=true",
        "r_title": "Read path (Get)",
        "r_api": ("Get(key)", "any thread, concurrent"),
        "r_sv": ("Copy SuperVersion", "copy 3 shared_ptr under mu_, unlock"),
        "r_mem": ("MemTable mem_", "shared lock; skip-list lookup"),
        "r_imm": ("Immutable MemTable imm_", "may be absent"),
        "r_l0": ("L0: every file, newest → oldest", "key ranges may overlap"),
        "r_ln": ("L1 → L2 → L3", "binary search: at most 1 file per level"),
        "r_miss": "kNotFound: go on",
        "r_res": ("First kFound / kDeleted wins", "all kNotFound → key absent"),
        "r_tab": "Per SSTable (Table::Get)",
        "r_t1": "1  key within [smallest, largest]?",
        "r_t2": "2  Bloom filter (in memory)",
        "r_t3": "3  index binary search → 1 block",
        "r_t4": "4  one pread + CRC check",
        "r_t5": "5  parse block, binary search",
    },
}


def write_path(lang):
    t = T[lang]
    s = Svg(lang, 560)
    s.text(W / 2, 20, t["w_title"], size=15, bold=True)
    x, w, h = 70, 320, 44
    cx = x + w / 2
    ys = [32, 96, 160, 224]
    for y, key, kind in zip(ys, ["w_api", "w_room", "w_wal", "w_mem"], ["api", "api", "disk", "mem"]):
        s.box(x, y, w, h, kind, *t[key])
    for y in ys[:-1]:
        s.arrow(cx, y + h, cx, y + 64)
    s.text(cx + 8, ys[2] + h + 14, t["w_sync"], size=11, color="#9a6a00", anchor="start")
    s.arrow(cx, ys[3] + h, cx, 296, t["w_full"])
    s.box(x, 296, w, h, "mem", *t["w_imm"])
    # background thread group
    s.box(8, 362, 444, 192, "bg", "", dashed=True)
    s.text(128, 379, t["w_bg"], size=11.5, color="#5b4a9c", anchor="start", bold=True)
    s.arrow(116, 340, 116, 386)
    bw = 196
    s.box(18, 386, bw, h, "disk", *t["w_sst"])
    s.box(246, 386, bw, h, "disk", *t["w_man"])
    s.arrow(18 + bw, 408, 246, 408)
    s.box(246, 442, bw, h, "bg", *t["w_del"])
    s.arrow(344, 430, 344, 442)
    s.box(18, 500, 424, h, "disk", *t["w_cmp"])
    s.arrow(344, 486, 344, 500)
    return s.render()


def read_path(lang):
    t = T[lang]
    s = Svg(lang, 560)
    s.text(W / 2, 20, t["r_title"], size=15, bold=True)
    x, w, h = 10, 238, 44
    cx = x + w / 2
    keys = ["r_api", "r_sv", "r_mem", "r_imm", "r_l0", "r_ln"]
    kinds = ["api", "api", "mem", "mem", "disk", "disk"]
    ys = [32 + i * 76 for i in range(6)]
    for y, key, kind in zip(ys, keys, kinds):
        s.box(x, y, w, h, kind, *t[key])
    for i, y in enumerate(ys[:-1]):
        s.arrow(cx, y + h, cx, y + 76, t["r_miss"] if i >= 2 else None, lx=cx + 8)
    s.arrow(cx, ys[-1] + h, cx, 496)
    s.box(x, 496, w + 60, 48, "api", *t["r_res"])
    # per-table lookup panel
    px, pw = 262, 192
    s.box(px, 300, pw, 172, "note", "", dashed=True)
    s.text(px + pw / 2, 318, t["r_tab"], size=11.5, bold=True, color="#444")
    for i, key in enumerate(["r_t1", "r_t2", "r_t3", "r_t4", "r_t5"]):
        s.text(px + 8, 342 + i * 28, t[key], size=11.2, anchor="start", color="#222")
    for y in (ys[4] + 22, ys[5] + 22):
        s.out.append(
            f'<line x1="{x + w}" y1="{y}" x2="{px}" y2="{y}" stroke="#888" stroke-width="1.2" '
            'stroke-dasharray="4 3"/>'
        )
    return s.render()


if __name__ == "__main__":
    here = os.path.dirname(os.path.abspath(__file__))
    for lang in ("zh", "en"):
        for name, fn in (("write_path", write_path), ("read_path", read_path)):
            with open(os.path.join(here, f"{name}_{lang}.svg"), "w", encoding="utf-8") as f:
                f.write(fn(lang))
            print("wrote", f"{name}_{lang}.svg")
