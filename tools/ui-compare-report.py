#!/usr/bin/env python3
"""
tools/ui-compare-report.py -- The report tools/ui-compare.sh writes (#2712).

Usage:
    ui-compare-report.py <out-dir> <label-a> <label-b>

<out-dir>/a and <out-dir>/b hold each ref's UI scenario frames (<NN>-<step>.png, from
TASKSMACK_UI_CAPTURE_DIR, #2711) and its run log (run.log). For each step this writes
<out-dir>/diff/<NN>-<step>.png, the pixels that changed drawn red over a dimmed copy of the first
frame, and works out the share of pixels that changed; then <out-dir>/index.html, with the pairs, the
diffs and their percentages, and each run's warnings and errors (log lines at warning level or
above, and ImGui assertions). A step only one side captured is listed as missing on the other.

Only the standard library: the frames are the 8-bit RGBA, non-interlaced PNGs stb_image_write writes,
which is all read_png() reads. Exits 1 when neither side has a frame.
"""

import html
import re
import struct
import sys
import zlib
from pathlib import Path

PNG_SIGNATURE = b"\x89PNG\r\n\x1a\n"
# The channel difference above which a pixel counts as changed: anti-aliasing noise stays under it.
CHANGE_THRESHOLD = 16
# Log lines worth a reviewer's look: spdlog's warning, error and critical levels, and ImGui asserts.
CTEST_PREFIX = re.compile(r"^\d+: ")
NOTEWORTHY = re.compile(r"\[(warning|error|critical)\]|IM_ASSERT|Assertion .* failed", re.IGNORECASE)


class Image:
    """Straight RGBA, 8 bits a channel, rows top-down."""

    def __init__(self, width: int, height: int, rgba: bytes) -> None:
        if len(rgba) != width * height * 4:
            raise ValueError(f"{width}x{height} RGBA needs {width * height * 4} bytes, not {len(rgba)}")
        self.width = width
        self.height = height
        self.rgba = rgba


def _paeth(left: int, up: int, up_left: int) -> int:
    estimate = left + up - up_left
    to_left, to_up, to_up_left = abs(estimate - left), abs(estimate - up), abs(estimate - up_left)
    if to_left <= to_up and to_left <= to_up_left:
        return left
    return up if to_up <= to_up_left else up_left


def read_png(data: bytes) -> Image:
    """Decodes an 8-bit RGBA (colour type 6), non-interlaced PNG; anything else is a ValueError."""
    if not data.startswith(PNG_SIGNATURE):
        raise ValueError("not a PNG")
    offset = len(PNG_SIGNATURE)
    width = height = 0
    compressed = bytearray()
    while offset + 8 <= len(data):
        (length,) = struct.unpack(">I", data[offset : offset + 4])
        kind = data[offset + 4 : offset + 8]
        body = data[offset + 8 : offset + 8 + length]
        offset += 12 + length
        if kind == b"IHDR":
            width, height, depth, colour, _, _, interlace = struct.unpack(">IIBBBBB", body)
            if depth != 8 or colour != 6 or interlace != 0:
                raise ValueError(f"only 8-bit RGBA non-interlaced PNGs are read (depth {depth}, colour type {colour})")
        elif kind == b"IDAT":
            compressed += body
        elif kind == b"IEND":
            break
    if width == 0 or height == 0:
        raise ValueError("no IHDR")
    raw = zlib.decompress(bytes(compressed))
    stride = width * 4
    if len(raw) != (stride + 1) * height:
        raise ValueError("pixel data of the wrong length")
    out = bytearray(stride * height)
    previous = bytearray(stride)
    for y in range(height):
        start = y * (stride + 1)
        kind = raw[start]
        row = bytearray(raw[start + 1 : start + 1 + stride])
        for x in range(stride):
            left = row[x - 4] if x >= 4 else 0
            up = previous[x]
            up_left = previous[x - 4] if x >= 4 else 0
            if kind == 1:
                row[x] = (row[x] + left) & 0xFF
            elif kind == 2:
                row[x] = (row[x] + up) & 0xFF
            elif kind == 3:
                row[x] = (row[x] + ((left + up) >> 1)) & 0xFF
            elif kind == 4:
                row[x] = (row[x] + _paeth(left, up, up_left)) & 0xFF
            elif kind != 0:
                raise ValueError(f"unknown filter type {kind}")
        out[y * stride : (y + 1) * stride] = row
        previous = row
    return Image(width, height, bytes(out))


def write_png(image: Image) -> bytes:
    """Encodes @p image as an 8-bit RGBA PNG, each row unfiltered."""

    def chunk(kind: bytes, body: bytes) -> bytes:
        return struct.pack(">I", len(body)) + kind + body + struct.pack(">I", zlib.crc32(kind + body) & 0xFFFFFFFF)

    stride = image.width * 4
    raw = b"".join(b"\x00" + image.rgba[y * stride : (y + 1) * stride] for y in range(image.height))
    header = struct.pack(">IIBBBBB", image.width, image.height, 8, 6, 0, 0, 0)
    return PNG_SIGNATURE + chunk(b"IHDR", header) + chunk(b"IDAT", zlib.compress(raw, 6)) + chunk(b"IEND", b"")


def diff_images(a: Image, b: Image) -> tuple[Image, float]:
    """The pixels that differ by more than CHANGE_THRESHOLD in any channel, red over a dimmed grey copy of
    @p a, and the percentage of pixels they are. Frames of different sizes are compared over the area
    both cover, and every pixel outside it counts as changed."""
    width, height = max(a.width, b.width), max(a.height, b.height)
    out = bytearray(width * height * 4)
    changed = 0
    for y in range(height):
        for x in range(width):
            o = (y * width + x) * 4
            inside = x < a.width and y < a.height and x < b.width and y < b.height
            if inside:
                ia, ib = (y * a.width + x) * 4, (y * b.width + x) * 4
                pa, pb = a.rgba[ia : ia + 3], b.rgba[ib : ib + 3]
                if max(abs(pa[0] - pb[0]), abs(pa[1] - pb[1]), abs(pa[2] - pb[2])) <= CHANGE_THRESHOLD:
                    grey = (pa[0] * 3 + pa[1] * 6 + pa[2]) // 10 // 3
                    out[o : o + 4] = bytes((grey, grey, grey, 255))
                    continue
            changed += 1
            out[o : o + 4] = b"\xff\x30\x30\xff"
    return Image(width, height, bytes(out)), 100.0 * changed / (width * height)


def noteworthy_lines(log: Path) -> list[str]:
    """The run log's warnings, errors and ImGui assertions, each once; none when there is no log."""
    if not log.is_file():
        return []
    seen: dict[str, None] = {}
    for line in log.read_text(encoding="utf-8", errors="replace").splitlines():
        # ctest -V prefixes each line with the test's number, and prints a failing test's output again.
        line = CTEST_PREFIX.sub("", line.rstrip())
        if NOTEWORTHY.search(line):
            seen.setdefault(line)
    return list(seen)


def frames(directory: Path) -> dict[str, Path]:
    return {path.stem: path for path in sorted(directory.glob("*.png"))} if directory.is_dir() else {}


def build_report(out: Path, label_a: str, label_b: str) -> int:
    """Writes the diffs and index.html; the number of steps compared."""
    side_a, side_b = frames(out / "a"), frames(out / "b")
    steps = sorted(set(side_a) | set(side_b))
    (out / "diff").mkdir(parents=True, exist_ok=True)
    rows = []
    for step in steps:
        cells = []
        for side, found in (("a", side_a), ("b", side_b)):
            cells.append(f'<td><a href="{side}/{step}.png"><img src="{side}/{step}.png"></a></td>' if step in found else "<td>missing</td>")
        if step in side_a and step in side_b:
            diff, percent = diff_images(read_png(side_a[step].read_bytes()), read_png(side_b[step].read_bytes()))
            (out / "diff" / f"{step}.png").write_bytes(write_png(diff))
            cells.append(f'<td><a href="diff/{step}.png"><img src="diff/{step}.png"></a></td>')
            verdict = f"{percent:.2f}% changed"
        else:
            cells.append("<td></td>")
            verdict = "only one side"
        rows.append(f"<tr><th>{html.escape(step)}<br><small>{verdict}</small></th>{''.join(cells)}</tr>")

    def log_section(side: str, label: str) -> str:
        lines = noteworthy_lines(out / side / "run.log")
        body = "\n".join(html.escape(line) for line in lines) if lines else "none"
        return f"<h2>{html.escape(label)}: warnings and errors</h2><pre>{body}</pre>"

    page = f"""<!doctype html>
<html lang="en"><head><meta charset="utf-8"><title>UI compare</title>
<style>
body {{ font-family: sans-serif; margin: 16px; background: #1e1e1e; color: #ddd; }}
table {{ border-collapse: collapse; }} th, td {{ border: 1px solid #444; padding: 4px; vertical-align: top; }}
th {{ text-align: left; white-space: nowrap; }} img {{ width: 420px; }} pre {{ white-space: pre-wrap; }}
</style></head><body>
<h1>UI compare: {html.escape(label_a)} vs {html.escape(label_b)}</h1>
<p>Changed: a pixel differing by more than {CHANGE_THRESHOLD} in a channel, red in the diff.</p>
<table><tr><th>Step</th><th>{html.escape(label_a)}</th><th>{html.escape(label_b)}</th><th>Diff</th></tr>
{chr(10).join(rows)}
</table>
{log_section("a", label_a)}
{log_section("b", label_b)}
</body></html>
"""
    (out / "index.html").write_text(page, encoding="utf-8")
    return len(steps)


def main(argv: list[str]) -> int:
    if len(argv) != 4:
        print(__doc__.strip(), file=sys.stderr)
        return 2
    out = Path(argv[1])
    compared = build_report(out, argv[2], argv[3])
    if compared == 0:
        print(f"ui-compare-report: no frames under {out}/a or {out}/b", file=sys.stderr)
        return 1
    print(f"ui-compare-report: {compared} steps -> {out / 'index.html'}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
