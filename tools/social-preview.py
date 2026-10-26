#!/usr/bin/env python3
"""The repository's social preview card (#2954): docs/images/social-preview.png, 1280x640.

GitHub shows it when a link to the repository unfurls (Reddit, Hacker News, Discord, Slack). It is made from
the repository's own assets -- the icon, the Inter font and the Overview screenshot -- so it can be redone
when they change: run this script, commit the PNG, then upload it under Settings > General > Social preview
(GitHub has no API for that). Needs Pillow.
"""

from __future__ import annotations

import pathlib

from PIL import Image, ImageDraw, ImageFilter, ImageFont

ROOT = pathlib.Path(__file__).resolve().parent.parent
OUT = ROOT / "docs" / "images" / "social-preview.png"
WIDTH, HEIGHT = 1280, 640
BACKGROUND_TOP = (24, 28, 38)
BACKGROUND_BOTTOM = (12, 14, 20)
TEXT = (236, 240, 247)
MUTED = (160, 170, 188)
ACCENT = (96, 165, 250)


def font(name: str, size: int) -> ImageFont.FreeTypeFont:
    return ImageFont.truetype(str(ROOT / "assets" / "fonts" / name), size)


def background() -> Image.Image:
    card = Image.new("RGB", (WIDTH, HEIGHT), BACKGROUND_TOP)
    draw = ImageDraw.Draw(card)
    for y in range(HEIGHT):
        t = y / (HEIGHT - 1)
        draw.line([(0, y), (WIDTH, y)], fill=tuple(round(a + (b - a) * t) for a, b in zip(BACKGROUND_TOP, BACKGROUND_BOTTOM)))
    return card


def screenshot(width: int) -> Image.Image:
    """The Overview screenshot, its left part (the CPU details and charts), scaled to @p width with rounded corners."""
    shot = Image.open(ROOT / "docs" / "images" / "screenshots" / "windows" / "overview-windows-dark.webp").convert("RGB")
    crop = shot.crop((0, 0, 1040, 760))
    height = round(crop.height * width / crop.width)
    crop = crop.resize((width, height), Image.Resampling.LANCZOS)
    mask = Image.new("L", crop.size, 0)
    ImageDraw.Draw(mask).rounded_rectangle([(0, 0), (crop.width - 1, crop.height - 1)], radius=18, fill=255)
    framed = Image.new("RGBA", crop.size)
    framed.paste(crop, (0, 0), mask)
    return framed


def main() -> None:
    card = background()
    # The screenshot on the right, past the centre, with a soft shadow; cropped by the card's edge.
    shot = screenshot(700)
    shot_x, shot_y = 600, 110
    shadow = Image.new("RGBA", (shot.width + 80, shot.height + 80), (0, 0, 0, 0))
    ImageDraw.Draw(shadow).rounded_rectangle([(40, 40), (shot.width + 40, shot.height + 40)], radius=22, fill=(0, 0, 0, 160))
    shadow = shadow.filter(ImageFilter.GaussianBlur(18))
    card.paste(shadow, (shot_x - 40, shot_y - 30), shadow)
    card.paste(shot, (shot_x, shot_y), shot)

    # The name and the pitch on the left, inside the centre safe area some sites crop to.
    icon = Image.open(ROOT / "assets" / "icons" / "tasksmack-128.png").convert("RGBA").resize((112, 112), Image.Resampling.LANCZOS)
    card.paste(icon, (80, 150), icon)
    draw = ImageDraw.Draw(card)
    draw.text((80, 286), "TaskSmack", font=font("Inter-Bold.ttf", 72), fill=TEXT)
    pitch = font("Inter-Regular.ttf", 30)
    draw.text((82, 382), "System monitor and", font=pitch, fill=MUTED)
    draw.text((82, 420), "task manager for", font=pitch, fill=MUTED)
    draw.text((82, 458), "Linux and Windows", font=pitch, fill=MUTED)
    draw.text((82, 530), "C++23 · Dear ImGui · Wayland-native", font=font("Inter-Regular.ttf", 22), fill=ACCENT)

    OUT.parent.mkdir(parents=True, exist_ok=True)
    card.save(OUT, optimize=True)
    print(f"Wrote {OUT.relative_to(ROOT)} ({OUT.stat().st_size // 1024} KiB)")


if __name__ == "__main__":
    main()
