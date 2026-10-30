#!/usr/bin/env python3
"""Derive every NoxisOS artwork variant from the two master images.

Masters (committed to the repository):

    assets/noxis-logo.png         square logo mark
    assets/noxis-wallpaper.png    wide background artwork

Derived (also committed, so a plain `mkarchiso` run works without Pillow):

    configs/noxis/airootfs/usr/share/backgrounds/noxis/noxis-wallpaper.png
    configs/noxis/airootfs/usr/share/noxis/noxis-logo.png
    configs/noxis/syslinux/splash.png                       (BIOS boot splash)
    configs/noxis/airootfs/etc/calamares/branding/noxis/*.png
        logo.png productIcon.png productWelcome.png productWallpaper.png
        slide1.png slide2.png slide3.png
    configs/noxis/airootfs/usr/share/plymouth/themes/noxis-paws/paw-{a,b}.png

Usage:  python3 scripts/make-branding-assets.py
Requires: Pillow (`pip install Pillow`) and a DejaVu font for the slides.
"""

from __future__ import annotations

import sys
from pathlib import Path

try:
    from PIL import Image, ImageDraw, ImageEnhance, ImageFont
except ImportError:  # pragma: no cover - helpful message for humans
    sys.exit("Pillow is required: python3 -m pip install Pillow")

ROOT = Path(__file__).resolve().parent.parent
ASSETS = ROOT / "assets"
PROFILE = ROOT / "configs" / "noxis"
BRAND = PROFILE / "airootfs" / "etc" / "calamares" / "branding" / "noxis"

VIOLET = (139, 92, 246)
CYAN = (34, 211, 238)
INK = (11, 11, 18)
TEXT = (229, 231, 235)
MUTED = (161, 161, 170)

FONT_CANDIDATES = [
    "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
    "/usr/share/fonts/TTF/DejaVuSans.ttf",
    "/usr/share/fonts/dejavu/DejaVuSans.ttf",
    "/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf",
]
FONT_BOLD_CANDIDATES = [
    "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf",
    "/usr/share/fonts/TTF/DejaVuSans-Bold.ttf",
    "/usr/share/fonts/dejavu/DejaVuSans-Bold.ttf",
    "/usr/share/fonts/truetype/liberation/LiberationSans-Bold.ttf",
]


def load_font(candidates: list[str], size: int) -> ImageFont.FreeTypeFont | ImageFont.ImageFont:
    for path in candidates:
        if Path(path).exists():
            return ImageFont.truetype(path, size)
    return ImageFont.load_default()


def fit(image: Image.Image, size: tuple[int, int], *, mode: str = "cover") -> Image.Image:
    """Resize `image` to `size` (cover = crop to fill, contain = letterbox)."""
    target_w, target_h = size
    src_w, src_h = image.size
    if mode == "cover":
        scale = max(target_w / src_w, target_h / src_h)
    else:
        scale = min(target_w / src_w, target_h / src_h)
    resized = image.resize((max(1, round(src_w * scale)), max(1, round(src_h * scale))), Image.LANCZOS)
    if mode == "cover":
        left = (resized.width - target_w) // 2
        top = (resized.height - target_h) // 2
        return resized.crop((left, top, left + target_w, top + target_h))
    canvas = Image.new("RGB", size, INK)
    canvas.paste(resized, ((target_w - resized.width) // 2, (target_h - resized.height) // 2))
    return canvas


def write(image: Image.Image, path: Path) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    image.save(path, optimize=True)
    print(f"  {path.relative_to(ROOT)}  {image.size[0]}x{image.size[1]}")


def slide(background: Image.Image, title: str, lines: list[str], index: int) -> Image.Image:
    img = background.copy()
    draw = ImageDraw.Draw(img, "RGBA")

    # darken the left half so the text stays readable
    for x in range(0, int(img.width * 0.72)):
        alpha = int(190 * (1 - x / (img.width * 0.72)) + 40)
        draw.line([(x, 0), (x, img.height)], fill=(*INK, alpha))

    title_font = load_font(FONT_BOLD_CANDIDATES, 46)
    body_font = load_font(FONT_CANDIDATES, 22)
    small_font = load_font(FONT_CANDIDATES, 18)

    draw.rectangle([(60, 60), (66, 60 + 14)], fill=VIOLET)
    draw.text((84, 56), "NoxisOS Cachy Edition", font=small_font, fill=MUTED)

    draw.text((60, img.height // 2 - 70), title, font=title_font, fill=TEXT)
    y = img.height // 2 + 10
    for line in lines:
        draw.text((60, y), line, font=body_font, fill=MUTED)
        y += 34

    draw.text((64, img.height - 56), f"{index}/3", font=small_font, fill=VIOLET)
    return img


def paw_sprite_frame(*, a: int, size: int = 56) -> Image.Image:
    """One frame of the plymouth paw. `a=1` lifts the left toes, `a=0` the
    right ones — alternating frames read as a paw taking a step."""
    img = Image.new("RGBA", (size, size), (0, 0, 0, 0))
    draw = ImageDraw.Draw(img, "RGBA")
    s = size / 56.0
    pad = (*VIOLET, 235)

    # main pad
    draw.ellipse([(18 * s, 28 * s), (38 * s, 46 * s)], fill=pad)
    # toe beans; the lifted side shrinks a little to suggest a step
    toes = [(12, 20, 14 if a else 10), (24, 14, 12), (36, 20, 10 if a else 14)]
    for cx, cy, r in toes:
        draw.ellipse([(cx * s - r * s, cy * s - r * s),
                      (cx * s + r * s, cy * s + r * s)], fill=pad)
    return img


def main() -> int:
    logo_master = Image.open(ASSETS / "noxis-logo.png").convert("RGB")
    wallpaper_master = Image.open(ASSETS / "noxis-wallpaper.png").convert("RGB")

    print("desktop assets")
    write(fit(wallpaper_master, (1920, 1080), mode="cover"),
          PROFILE / "airootfs" / "usr" / "share" / "backgrounds" / "noxis" / "noxis-wallpaper.png")
    write(fit(logo_master, (512, 512), mode="contain"),
          PROFILE / "airootfs" / "usr" / "share" / "noxis" / "noxis-logo.png")

    print("syslinux boot splash")
    splash = fit(wallpaper_master, (640, 480), mode="cover")
    splash = ImageEnhance.Brightness(splash).enhance(0.55)
    draw = ImageDraw.Draw(splash, "RGBA")
    logo_small = fit(logo_master, (150, 150), mode="contain")
    splash.paste(logo_small, ((640 - 150) // 2, 120))
    draw.text((320, 320), "NoxisOS", font=load_font(FONT_BOLD_CANDIDATES, 40),
              fill=TEXT, anchor="mm")
    draw.text((320, 362), "Cachy Edition", font=load_font(FONT_CANDIDATES, 20),
              fill=VIOLET, anchor="mm")
    write(splash, PROFILE / "syslinux" / "splash.png")

    print("calamares branding (installed system / installer)")
    write(fit(logo_master, (256, 256), mode="contain"), BRAND / "logo.png")
    write(fit(logo_master, (128, 128), mode="contain"), BRAND / "icon.png")

    welcome = fit(wallpaper_master, (480, 220), mode="cover")
    welcome = ImageEnhance.Brightness(welcome).enhance(0.65)
    wdraw = ImageDraw.Draw(welcome, "RGBA")
    mini = fit(logo_master, (96, 96), mode="contain")
    welcome.paste(mini, (28, (220 - 96) // 2))
    wdraw.text((140, 82), "NoxisOS", font=load_font(FONT_BOLD_CANDIDATES, 40), fill=TEXT)
    wdraw.text((142, 130), "Cachy Edition", font=load_font(FONT_CANDIDATES, 20), fill=VIOLET)
    write(welcome, BRAND / "welcome.png")

    write(fit(wallpaper_master, (1100, 580), mode="cover"), BRAND / "productWallpaper.png")

    print("installation slideshow")
    base = fit(wallpaper_master, (1100, 580), mode="cover")
    base = ImageEnhance.Brightness(base).enhance(0.8)
    slides = [
        ("Welcome to NoxisOS", [
            "An Arch/CachyOS rolling release.",
            "Optimized linux-cachyos kernel with the BORE scheduler.",
            "Hyprland (Wayland) desktop, tuned by this project.",
        ]),
        ("Installing", [
            "Everything ships inside the ISO — the installer",
            "works with no internet connection at all.",
            "Pick a disk, create your user, done.",
        ]),
        ("After the install", [
            "noxis-welcome .... tour, tips and keybindings",
            "noxis-repo-tier .. x86-64-v3/v4 optimized packages",
            "sudo pacman -Syu .. update the rolling release",
        ]),
    ]
    for index, (title, lines) in enumerate(slides, start=1):
        write(slide(base, title, lines, index), BRAND / f"slide{index}.png")

    print("plymouth paws splash")
    paws = PROFILE / "airootfs" / "usr" / "share" / "plymouth" / "themes" / "noxis-paws"
    write(paw_sprite_frame(a=1), paws / "paw-a.png")
    write(paw_sprite_frame(a=0), paws / "paw-b.png")

    print("done")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
