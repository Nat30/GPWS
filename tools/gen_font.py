#!/usr/bin/env python3
"""Generate LVGL bitmap-font C sources from system Noto Sans CJK faces.

Offline replacement for lv_font_conv (npm registry unreachable on this host):
renders each requested code point with freetype-py at a fixed pixel size,
packs the 8-bit gray raster into 4-bpp LVGL rows, and emits an
`lv_font_fmt_txt` C file understood by LVGL 9 and tools/check_glyphs.py.

Manifest: tools/font_manifest.txt (name, size, weight, charset spec per line).
Charset spec keywords: `cjk` (app CJK superset), `ascii` (U+0020..U+007E),
`digits` (0-9), plus any literal text after a `+` prefix.

Output: assets/fonts/<name>.c
"""

from __future__ import annotations

import argparse
import freetype
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
TTC_PATHS = {
    "regular": "/usr/share/fonts/google-noto-cjk/NotoSansCJK-Regular.ttc",
    "bold": "/usr/share/fonts/google-noto-cjk/NotoSansCJK-Bold.ttc",
}


def load_face(weight: str) -> freetype.Face:
    """Open the Sans CJK SC face by family name (ttc face order may change)."""
    path = TTC_PATHS[weight]
    for i in range(16):
        try:
            face = freetype.Face(path, index=i)
        except Exception:
            break
        if face.family_name == b"Noto Sans CJK SC":
            return face
    raise RuntimeError(f"Noto Sans CJK SC face not found in {path}")

# Superset of every CJK string the UI displays (title, card names, subtitles,
# states, hints). Regenerate fonts when adding new Chinese text.
CJK_SUPERSET = (
    "航空警示音"
    "呼啸拉升近地波音警报失速风切变冲突告警地形拉起下沉率减速连呼提醒主诫"
    "空客合辑自动驾驶断开指点信标液压狗叫通用最低高度客舱叮咚求救注意爬指令"
    "个音效正在播放中已暂停音频不可用移动选择确认"
    "设置量关上键换调整退出长按完成"
    "随机"
    "电量不足任意键继续"  # low-battery scrim (battery guard)
    "聆听统计滚返回"      # listen-mode item, stats popup title/scroll hints
    "菜单"                # main-menu title (rows reuse existing glyphs)
    "硬件诊断压版本线备扫描描止未运行记录唤醒站点本次知等断电读取失败电池总线无设备似卡死疑"  # diagnostics page
    "听声辨机"            # quiz: menu row + page title
    "题挑战无尽模式"        # quiz mode page
    "第场景开始中"          # quiz: question line + hints + scenario word
    "起飞后不升反降，最急促的警告连成一片"
    "前方地形逼近，机载雷达反复念同一个词"
    "波音驾驶舱的告警大合辑"
    "机翼失速，连续急促的警告音"
    "EGPWS:「TERRAIN TERRAIN PULL UP」"
    "起飞爬升中雷达探测到风切变"
    "TCAS 提示有飞机正在靠近"
    "着陆拉平，减速提醒连成一串"
    "空客 AP1 断开的电气音"
    "离地不远，下降却太快"
    "主诫灯亮起的那一声"
    "空客无线电高度喊话合辑"
    "地面通电自检，舱里传来「狗叫」"
    "着陆减速的单次提醒"
    "飞越信标台上空的音频提示"
    "失速警告的另一种风格"
    "波音版连续「PULL UP」"
    "空客版 TCAS 交通提示"
    "决断高度到了，机长喊出那个词"
    "无线电里传来 MAYDAY"
    "地形注意提示音响起"
    "乘务员呼叫铃「叮咚」"
    "最短促的连续拉升警告"
    "TCAS 下达爬升指令"      # quiz scenario prompts (spec §3.3, shown on question page)
    "答对最长连佳新纪录！"    # quiz sum page: score / best / new-record
    "完整起飞进近着陆风切变逃脱紧急下降波音全警告空客全家桶"  # scene rows (scenes.h)
    "条"                  # scene row count suffix
    "闹钟曲停"            # alarm page/ring page: 闹钟/选曲/任意键停止
    "上键 下键 确认键 进行选择"  # quiz answer-page hint (explicit key names)
    "总次数时长分秒"      # stats subtitle: totals / play-time format
    "决服务"              # renames: 波音决断高度 / 客舱服务
    "·"
)

ASCII = "".join(chr(c) for c in range(0x20, 0x7F))


def render_charset(face: freetype.Face, charset: str, size: int):
    """Render each unique code point; return sorted (cp, glyph-dsc, rows) tuples."""
    face.set_pixel_sizes(0, size)
    glyphs = []
    for cp in sorted({ord(c) for c in charset}):
        # Hinted rendering (freetype default): snaps strokes to the pixel grid
        # at small sizes, keeping thin regular strokes legible. The bold fonts
        # the user confirmed as crisp were generated with this same pipeline.
        face.load_char(chr(cp), freetype.FT_LOAD_RENDER)
        slot = face.glyph
        bmp = slot.bitmap
        box_w, box_h = bmp.width, bmp.rows
        adv = round(slot.advance.x / 64 * 16)  # LVGL 1/16 px units
        rows = []
        for y in range(box_h):
            row = []
            for x in range(box_w):
                row.append(bmp.buffer[y * bmp.pitch + x] >> 4)
            rows.append(row)
        if cp != 0x20 and box_h and not any(any(r) for r in rows):
            raise AssertionError(f"U+{cp:04X} rasterized to empty bitmap")
        ofs_y = slot.bitmap_top - box_h  # LVGL: bottom of box vs baseline
        glyphs.append((cp, adv, box_w, box_h, slot.bitmap_left, ofs_y, rows))
    return glyphs


def pack_bitmap(glyphs) -> bytes:
    """Pack 4-bpp as a continuous nibble stream (LVGL 9.5 fmt_txt decode path).

    LVGL decodes A4 bitmaps as one continuous nibble sequence across the whole
    box (no per-line padding, no line start alignment) because lv_font.c:143
    hardcodes g_dsc->stride = 0. Each glyph starts on a byte boundary.
    """
    out = bytearray()
    for _, _, box_w, box_h, _, _, rows in glyphs:
        nibbles = []
        for row in rows:
            nibbles.extend(v & 0xF for v in row)
        if len(nibbles) % 2:
            nibbles.append(0)  # pad half nibble at glyph end only
        for i in range(0, len(nibbles), 2):
            out.append((nibbles[i] << 4) | nibbles[i + 1])
    return bytes(out)


def shown(cp: int) -> str:
    return chr(cp) if chr(cp).isprintable() else "?"


def emit(name: str, size: int, glyphs, out_path: Path):
    bitmap = pack_bitmap(glyphs)
    # bitmap_index per glyph (id 0 reserved): byte offset in the packed stream.
    index, indexes = 0, [0]
    for _, _, box_w, box_h, _, _, _ in glyphs:
        index += (box_w * box_h + 1) // 2
        indexes.append(index)

    metrics = METRICS  # filled by generate()
    lines = []
    lines.append(f"/*******************************************************************************")
    lines.append(f" * {name}: {size} px, bpp 4, generated by tools/gen_font.py")
    lines.append(f" * Face: Noto Sans CJK SC ({metrics['weight']}) via freetype {freetype.version()}")
    lines.append(f" ******************************************************************************/")
    lines.append("")
    lines.append("#include \"lvgl.h\"")
    lines.append("")
    lines.append(f"#ifndef {name.upper()}")
    lines.append(f"    #define {name.upper()} 1")
    lines.append("#endif")
    lines.append("")
    lines.append(f"#if {name.upper()}")
    lines.append("")
    lines.append("/*-----------------")
    lines.append(" *    BITMAPS")
    lines.append(" *----------------*/")
    lines.append("")
    lines.append("static LV_ATTRIBUTE_LARGE_CONST const uint8_t glyph_bitmap[] = {")
    pos = 0
    for cp, _, box_w, box_h, _, _, _ in glyphs:
        lines.append(f"    /* U+{cp:04X} {shown(cp)} */")
        nbytes = (box_w * box_h + 1) // 2
        chunk = bitmap[pos:pos + nbytes]
        pos += nbytes
        if not chunk:
            # Zero-byte glyph (e.g. space): emit NO array byte. A placeholder
            # byte here would shift every following glyph (bitmap_index
            # accounting counts this glyph as 0 bytes) -- exactly the bug that
            # garbled the regular fonts on device.
            continue
        vals = [f"0x{b:x}" for b in chunk]
        for i in range(0, len(vals), 8):
            lines.append("    " + ", ".join(vals[i:i + 8]) + ",")
    lines.append("};")
    lines.append("")
    lines.append("/*---------------------")
    lines.append(" *  GLYPH DESCRIPTION")
    lines.append(" *--------------------*/")
    lines.append("")
    lines.append("static const lv_font_fmt_txt_glyph_dsc_t glyph_dsc[] = {")
    lines.append("    {.bitmap_index = 0, .adv_w = 0, .box_w = 0, .box_h = 0, .ofs_x = 0, .ofs_y = 0} /* id = 0 reserved */,")
    for i, (cp, adv, box_w, box_h, ofs_x, ofs_y, _) in enumerate(glyphs):
        lines.append(
            f"    {{.bitmap_index = {indexes[i]}, .adv_w = {adv}, .box_w = {box_w}, "
            f".box_h = {box_h}, .ofs_x = {ofs_x}, .ofs_y = {ofs_y}}} /* U+{cp:04X} {shown(cp)} */,")
    lines.append("};")
    lines.append("")
    lines.append("/*---------------------")
    lines.append(" *  CHARACTER MAPPING")
    lines.append(" *--------------------*/")
    lines.append("")
    range_start = glyphs[0][0]
    offsets = [f"0x{cp - range_start:x}" for cp, *_ in glyphs]
    lines.append("static const uint16_t unicode_list_0[] = {")
    for i in range(0, len(offsets), 8):
        lines.append("    " + ", ".join(offsets[i:i + 8]) + ",")
    lines.append("};")
    lines.append("")
    lines.append("static const lv_font_fmt_txt_cmap_t cmaps[] = {")
    lines.append("    {")
    lines.append(f"        .range_start = {range_start}, .range_length = {glyphs[-1][0] - range_start + 1}, .glyph_id_start = 1,")
    lines.append("        .unicode_list = unicode_list_0, .glyph_id_ofs_list = NULL, "
                 f".list_length = {len(glyphs)}, .type = LV_FONT_FMT_TXT_CMAP_SPARSE_TINY")
    lines.append("    }")
    lines.append("};")
    lines.append("")
    lines.append("/*--------------------")
    lines.append(" *  ALL CUSTOM DATA")
    lines.append("--------------------*/")
    lines.append("")
    lines.append("static const lv_font_fmt_txt_dsc_t font_dsc = {")
    lines.append("    .glyph_bitmap = glyph_bitmap,")
    lines.append("    .glyph_dsc = glyph_dsc,")
    lines.append("    .cmaps = cmaps,")
    lines.append("    .kern_dsc = NULL,")
    lines.append("    .kern_scale = 0,")
    lines.append("    .cmap_num = 1,")
    lines.append("    .bpp = 4,")
    lines.append("    .stride = 0,  /* continuous nibble stream; LVGL hardcodes stride 0 anyway */")
    lines.append("    .kern_classes = 0,")
    lines.append("    .bitmap_format = 0,")
    lines.append("};")
    lines.append("")
    lines.append("/*-----------------")
    lines.append(" *  PUBLIC FONT")
    lines.append(" *----------------*/")
    lines.append("")
    lines.append(f"const lv_font_t {name} = {{")
    lines.append("    .get_glyph_dsc = lv_font_get_glyph_dsc_fmt_txt,")
    lines.append("    .get_glyph_bitmap = lv_font_get_bitmap_fmt_txt,")
    lines.append(f"    .line_height = {metrics['line_height']},")
    lines.append(f"    .base_line = {metrics['baseline']},")
    lines.append("    .subpx = LV_FONT_SUBPX_NONE,")
    lines.append("    .underline_position = -1,")
    lines.append("    .underline_thickness = 1,")
    lines.append("    .dsc = &font_dsc")
    lines.append("};")
    lines.append("")
    lines.append(f"#endif /*#if {name.upper()}*/")
    lines.append("")
    out_path.write_text("\n".join(lines), encoding="utf-8")


def self_check(out_path: Path, face: freetype.Face, size: int, charset: str):
    """Re-parse the emitted file and verify every glyph byte-for-byte.

    Decodes the file the exact way LVGL does (bpp 4, continuous nibble stream,
    no per-line padding) and compares against a fresh render. Guards against
    emission/accounting drift (the stray placeholder byte that shifted every
    glyph after U+0020 on device).
    """
    import re

    src = out_path.read_text(encoding="utf-8")
    m = re.search(r"const uint8_t glyph_bitmap\[\] = \{(.*?)\};", src, re.S)
    assert m, "glyph_bitmap array not found in emitted file"
    data = bytes(int(x, 16) for x in re.findall(r"0x([0-9A-Fa-f]+)", m.group(1)))
    dscs = re.findall(
        r"\{\.bitmap_index = (\d+), \.adv_w = \d+, \.box_w = (\d+), \.box_h = (\d+), "
        r"\.ofs_x = -?\d+, \.ofs_y = -?\d+\} /\* U\+([0-9A-Fa-f]+)", src)
    m = re.search(r"const uint16_t unicode_list_0\[\] = \{(.*?)\};", src, re.S)
    assert m, "unicode_list_0 not found in emitted file"
    uni_list = [int(x, 16) for x in re.findall(r"0x([0-9A-Fa-f]+)", m.group(1))]
    m = re.search(r"\.range_start = (\d+),", src)
    assert m, "range_start not found in emitted file"
    range_start = int(m.group(1))
    m = re.search(r"\.glyph_id_start = (\d+),", src)
    assert m, "glyph_id_start not found in emitted file"
    glyph_id_start = int(m.group(1))
    cm = {range_start + off: glyph_id_start + i for i, off in enumerate(uni_list)}

    face.set_pixel_sizes(0, size)
    for cp in {ord(c) for c in charset}:
        gid = cm[cp]
        bi, w, h, dcp = dscs[gid - 1]
        bi, w, h, n = int(bi), int(w), int(h), (int(w) * int(h) + 1) // 2
        if n == 0:
            continue
        face.load_char(chr(cp), freetype.FT_LOAD_RENDER)
        b = face.glyph.bitmap
        assert (b.width, b.rows) == (w, h), f"U+{cp:04X}: dsc {w}x{h} vs render {b.width}x{b.rows}"
        nib = []
        for y in range(b.rows):
            nib.extend(b.buffer[y * b.pitch + x] >> 4 for x in range(b.width))
        if len(nib) % 2:
            nib.append(0)
        exp = bytes((nib[i] << 4) | nib[i + 1] for i in range(0, len(nib), 2))
        got = data[bi:bi + n]
        assert got == exp, (
            f"U+{cp:04X}: bitmap data at {bi} does not match render "
            f"(len {len(got)} vs {len(exp)}); bitmap_index accounting is off")


METRICS = {}


def generate(entry: dict, out_dir: Path):
    face = load_face(entry["weight"])
    face.set_pixel_sizes(0, entry["size"])
    METRICS.clear()
    METRICS["weight"] = entry["weight"]
    METRICS["line_height"] = round(face.size.height / 64)
    METRICS["baseline"] = METRICS["line_height"] - round(face.size.ascender / 64)
    glyphs = render_charset(face, entry["charset"], entry["size"])
    out = out_dir / f"{entry['name']}.c"
    emit(entry["name"], entry["size"], glyphs, out)
    self_check(out, face, entry["size"], entry["charset"])
    total = len(pack_bitmap(glyphs))
    print(f"{out.relative_to(ROOT)}: {len(glyphs)} glyphs, bitmap {total} bytes, "
          f"line_height {METRICS['line_height']}, baseline {METRICS['baseline']} (self-check OK)")
    assert glyphs, "empty charset"


def parse_manifest(path: Path):
    entries = []
    for raw in path.read_text(encoding="utf-8").splitlines():
        line = raw.split("#", 1)[0].strip()
        if not line:
            continue
        name, size, weight, spec = line.split()
        charset = ""
        for token in spec.replace("+", ",+").split(","):
            token = token.strip()
            if not token:
                continue
            if token == "cjk":
                charset += CJK_SUPERSET
            elif token == "ascii":
                charset += ASCII
            elif token == "digits":
                charset += "0123456789%"
            elif token.startswith("+"):
                charset += token[1:]
            else:
                raise ValueError(f"bad charset token {token!r}")
        entries.append({"name": name, "size": int(size), "weight": weight, "charset": charset})
    return entries


def main():
    parser = argparse.ArgumentParser(description="Generate LVGL bitmap fonts from Noto Sans CJK.")
    parser.add_argument("--manifest", type=Path, default=ROOT / "tools/font_manifest.txt")
    parser.add_argument("--out", type=Path, default=ROOT / "assets/fonts")
    args = parser.parse_args()
    args.out.mkdir(parents=True, exist_ok=True)
    for entry in parse_manifest(args.manifest):
        generate(entry, args.out)


if __name__ == "__main__":
    main()
