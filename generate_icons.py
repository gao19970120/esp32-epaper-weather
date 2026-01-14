import os
from pathlib import Path
from PIL import Image
import shutil
import subprocess
try:
    import cairosvg
except:
    cairosvg = None

def svg_to_png(svg_path, png_path, size):
    if cairosvg is not None:
        cairosvg.svg2png(url=str(svg_path), write_to=str(png_path), output_width=size, output_height=size)
        return True
    inkscape = shutil.which("inkscape")
    if inkscape:
        try:
            subprocess.run([inkscape, str(svg_path), "--export-type=png", f"--export-filename={png_path}", f"--export-width={size}", f"--export-height={size}"], check=True)
            return True
        except Exception:
            pass
    magick = shutil.which("magick")
    if magick:
        try:
            subprocess.run([magick, "convert", str(svg_path), "-resize", f"{size}x{size}", str(png_path)], check=True)
            return True
        except Exception:
            pass
    print(f"[WARN] SVG->PNG skipped (need cairosvg/inkscape/magick): {svg_path}")
    return False

def png_to_1bit_bin(png_path, bin_path, size=64):
    src = Image.open(png_path).convert("RGBA")
    bg = Image.new("RGBA", src.size, (255, 255, 255, 255))
    bg.paste(src, (0, 0), src)
    img = bg.convert("L")
    if img.size != (size, size):
        img = img.resize((size, size), Image.LANCZOS)
    img = img.point(lambda p: 255 if p > 128 else 0, mode="1")
    img = img.convert("1")
    w, h = img.size
    pixels = img.load()
    wbyte = (w // 8) + (1 if w % 8 else 0)
    data = bytearray(wbyte * h)
    for y in range(h):
        for x in range(w):
            b = 0 if pixels[x, y] == 0 else 1
            idx = (x // 8) + y * wbyte
            bit = 7 - (x % 8)
            if b:
                data[idx] |= (1 << bit)
    with open(bin_path, "wb") as f:
        f.write(data)

def main():
    base = Path(__file__).parent
    icon_dir = base / "icon"
    out_dir = base / "icons_bin"
    out_dir.mkdir(exist_ok=True)
    size = 64
    # Prefer existing PNGs first
    for png in icon_dir.glob("*.png"):
        name = png.stem
        bin_out = out_dir / (name + ".bin")
        png_to_1bit_bin(png, bin_out, size)
    # Then try SVGs
    for svg in icon_dir.glob("*.svg"):
        name = svg.stem
        png_tmp = out_dir / (name + ".png")
        bin_out = out_dir / (name + ".bin")
        if svg_to_png(svg, png_tmp, size):
            png_to_1bit_bin(png_tmp, bin_out, size)
            try:
                png_tmp.unlink()
            except:
                pass

if __name__ == "__main__":
    main()
