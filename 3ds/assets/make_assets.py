#!/usr/bin/env python3
"""Generates the WA3DS icon, HOME Menu banner image and banner jingle.

Run from this directory with Pillow installed:  python3 make_assets.py
Outputs: icon.png (48x48), banner.png (256x128), banner.wav (short chime).
The results are committed, so this only needs re-running when the design changes.
"""
import math
import struct
import sys
import wave

from PIL import Image, ImageDraw, ImageFilter, ImageFont

GREEN_DARK = (7, 94, 84)
GREEN = (18, 140, 126)
GREEN_LIGHT = (37, 211, 102)
WHITE = (255, 255, 255)

FONT_CANDIDATES = [
    "/System/Library/Fonts/Supplemental/Arial Rounded Bold.ttf",
    "/System/Library/Fonts/SFNSRounded.ttf",
    "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf",
]


def font(size):
    for path in FONT_CANDIDATES:
        try:
            return ImageFont.truetype(path, size)
        except OSError:
            continue
    sys.exit("no usable font found; edit FONT_CANDIDATES")


def gradient(w, h, top, bottom):
    img = Image.new("RGBA", (w, h))
    px = img.load()
    for y in range(h):
        t = y / max(1, h - 1)
        c = tuple(round(top[i] + (bottom[i] - top[i]) * t) for i in range(3))
        for x in range(w):
            px[x, y] = c + (255,)
    return img


def chat_glyph(size, scale=4):
    """White speech bubble with three dots, drawn large and downsampled for smooth edges."""
    s = size * scale
    img = Image.new("RGBA", (s, s), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    # bubble body
    pad = s * 0.12
    body = [pad, pad * 1.1, s - pad, s - pad * 2.2]
    d.rounded_rectangle(body, radius=s * 0.22, fill=WHITE)
    # tail at the bottom left
    tx, ty = body[0] + s * 0.16, body[3] - s * 0.04
    d.polygon([(tx, ty), (tx + s * 0.2, ty), (tx - s * 0.06, ty + s * 0.2)], fill=WHITE)
    # three dots
    cy = (body[1] + body[3]) / 2
    r = s * 0.065
    for i in (-1, 0, 1):
        cx = s / 2 + i * s * 0.2
        d.ellipse([cx - r, cy - r, cx + r, cy + r], fill=GREEN)
    return img.resize((size, size), Image.LANCZOS)


def make_icon():
    size, scale = 48, 4
    s = size * scale
    # full square: SMDH icons have no alpha and the HOME Menu frames them itself
    tile = gradient(s, s, GREEN_LIGHT, GREEN_DARK)
    glyph = chat_glyph(int(s * 0.78), scale=1)
    off = (s - glyph.width) // 2
    tile.alpha_composite(glyph, (off, off + s // 40))
    tile.resize((size, size), Image.LANCZOS).save("icon.png")


def make_banner():
    w, h = 256, 128
    img = gradient(w, h, (10, 120, 106), (4, 60, 54))
    # soft light circle for depth
    glow = Image.new("RGBA", (w, h), (0, 0, 0, 0))
    ImageDraw.Draw(glow).ellipse([-60, -80, 150, 130], fill=(255, 255, 255, 38))
    img.alpha_composite(glow.filter(ImageFilter.GaussianBlur(18)))

    # icon tile on the left
    tile = Image.new("RGBA", (76, 76), (0, 0, 0, 0))
    ImageDraw.Draw(tile).rounded_rectangle([0, 0, 75, 75], radius=17, fill=GREEN_LIGHT + (255,))
    g = chat_glyph(60)
    tile.alpha_composite(g, (8, 9))
    shadow = Image.new("RGBA", (w, h), (0, 0, 0, 0))
    ImageDraw.Draw(shadow).rounded_rectangle([18, 30, 96, 108], radius=18, fill=(0, 0, 0, 90))
    img.alpha_composite(shadow.filter(ImageFilter.GaussianBlur(4)))
    img.alpha_composite(tile, (16, 26))

    d = ImageDraw.Draw(img)
    d.text((104, 34), "WA3DS", font=font(36), fill=WHITE)
    d.text((106, 76), "Chats & calls", font=font(15), fill=(200, 240, 228))
    d.text((106, 94), "on your 3DS", font=font(15), fill=(200, 240, 228))
    img.convert("RGB").save("banner.png")


def make_jingle():
    """Two soft notes (E5 -> A5), 1.1 s, 44.1 kHz mono 16-bit."""
    rate = 44100
    notes = [(659.25, 0.0), (880.0, 0.18)]
    n = int(rate * 1.1)
    out = []
    for i in range(n):
        t = i / rate
        v = 0.0
        for f, start in notes:
            if t >= start:
                tt = t - start
                env = min(1.0, tt / 0.01) * math.exp(-tt * 4.0)
                v += env * (math.sin(2 * math.pi * f * tt) + 0.3 * math.sin(4 * math.pi * f * tt))
        out.append(int(max(-1.0, min(1.0, v * 0.35)) * 32767))
    with wave.open("banner.wav", "wb") as wf:
        wf.setnchannels(1)
        wf.setsampwidth(2)
        wf.setframerate(rate)
        wf.writeframes(b"".join(struct.pack("<h", s) for s in out))


if __name__ == "__main__":
    make_icon()
    make_banner()
    make_jingle()
    print("wrote icon.png, banner.png, banner.wav")
