#!/usr/bin/env python3
"""Build the PS Vita LiveArea artwork for F-Zero Recomp from the real game
artwork in the repo:
  assets/boxart.jpg                    SNES F-Zero box art (background, startup image)
  assets/AppIcon.png                   original repo's app icon (bubble icon)

Both sources are Nintendo's copyrighted artwork (the box art is the non-free
Wikipedia image "SNES F-Zero boxart.jpg"). Fine for a local VPK; think twice
before publishing them.

The Vita's package promoter rejects 24-bit RGB sce_sys images with error
0x8010113D. Every image must be a non-interlaced 8-bit indexed (palette)
PNG with no transparency, at exactly these sizes:
  sce_sys/icon0.png                      128x128  LiveArea bubble icon
  sce_sys/livearea/contents/bg.png       840x500  LiveArea background
  sce_sys/livearea/contents/startup.png  280x158  gate startup image

Run: python3 tools/make_livearea_art.py
"""

import os

from PIL import Image

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BOXART = os.path.join(REPO, "assets", "boxart.jpg")
APPICON = os.path.join(REPO, "assets", "AppIcon.png")
SCE_SYS = os.path.join(REPO, "sce_sys")
CONTENTS = os.path.join(SCE_SYS, "livearea", "contents")


def cover(img, size, anchor="center"):
    """Resize to exactly size, cropping the overflow. anchor="top" keeps the
    top of the artwork (e.g. the box-art logo) and crops the bottom."""
    tw, th = size
    crop_h = int(round(img.width * th / tw))
    if crop_h < img.height:  # Crop top/bottom.
        top = 0 if anchor == "top" else (img.height - crop_h) // 2
        img = img.crop((0, top, img.width, top + crop_h))
    else:  # Crop left/right.
        crop_w = int(round(img.height * tw / th))
        left = (img.width - crop_w) // 2
        img = img.crop((left, 0, left + crop_w, img.height))
    return img.resize(size, Image.Resampling.LANCZOS)


def save_indexed(img, path):
    """Save as a non-interlaced 8-bit indexed (palette) PNG (see header)."""
    img.convert("P", palette=Image.Palette.ADAPTIVE, colors=256,
                dither=Image.Dither.FLOYDSTEINBERG).save(path)


def main():
    os.makedirs(CONTENTS, exist_ok=True)
    boxart = Image.open(BOXART).convert("RGB")
    appicon = Image.open(APPICON).convert("RGB")
    print(f"source {BOXART}  {boxart.size[0]}x{boxart.size[1]}")
    print(f"source {APPICON}  {appicon.size[0]}x{appicon.size[1]}")
    outputs = [
        (os.path.join(CONTENTS, "bg.png"),
         cover(boxart, (840, 500), anchor="top")),
        (os.path.join(CONTENTS, "startup.png"),
         cover(boxart, (280, 158), anchor="top")),
        (os.path.join(SCE_SYS, "icon0.png"), cover(appicon, (128, 128))),
    ]
    for path, img in outputs:
        save_indexed(img, path)
        saved = Image.open(path)
        print(f"{path}  {saved.size[0]}x{saved.size[1]}  "
              f"mode={saved.mode}  {os.path.getsize(path)} bytes")


if __name__ == "__main__":
    main()
