from PIL import Image, ImageFont, ImageDraw
import sys
import os

TTF_PATH = "FZCHAOZYTJW.TTF"
FONT_SIZE = 16
# Missing characters: W, i, F, M, Q, T, O, K, f, a, l
# Also included common ones just in case: ' ' (space)
TARGET_CHARS = "WiFMQTTOKfail"
OUTPUT_FILE = "font_supplement.txt"

def glyph_to_bitmap(char):
    # Create an image of the character
    # Mode '1' (1-bit pixels, black and white, stored with one pixel per byte)
    # However, PIL uses 0 for black and 1 for white usually, or depends on palette.
    # We want white text on black background to extract the 'on' pixels.
    # Actually, the standard display logic usually expects 1 = pixel on.
    
    # Let's verify the font_new_cn.c format.
    # It uses 2 bytes per row for 16 width? 
    # structure: UBYTE Index[3]; UBYTE Msk[32];
    # 16x16 font = 256 bits = 32 bytes. Correct.
    # Row major or column major?
    # Usually standard scanline: Row 0 Byte 0, Row 0 Byte 1, Row 1 Byte 0...
    
    img = Image.new("1", (FONT_SIZE, FONT_SIZE), 0) # Black background
    draw = ImageDraw.Draw(img)
    font = ImageFont.truetype(TTF_PATH, FONT_SIZE)
    
    # Draw character. Position might need adjustment to center or align.
    # Usually (0,0) is top-left.
    # Note: Some fonts have offsets.
    draw.text((0, 0), char, fill=1, font=font)
    
    bitmap = []
    for y in range(FONT_SIZE):
        byte_val = 0
        for x in range(16): # 16 pixels width
             # 0 to 7 -> first byte, 8 to 15 -> second byte
             # wait, if we iterate 0..15
             # bit position: 7-0 for first byte?
             # Let's accumulate.
             
             pixel = img.getpixel((x, y))
             # pixel is 0 or 1 (if fill=1)
             
             # byte filling direction: MSB first usually.
             # x=0 -> bit 7 of byte 0
             # x=7 -> bit 0 of byte 0
             # x=8 -> bit 7 of byte 1
             
             if x < 8:
                 if pixel:
                     byte_val |= (1 << (7 - x))
                 if x == 7:
                     bitmap.append(byte_val)
                     byte_val = 0
             else:
                 if pixel:
                     byte_val |= (1 << (7 - (x - 8)))
                 if x == 15:
                     bitmap.append(byte_val)
                     byte_val = 0
                     
    return bitmap

def generate_ch_cn_entry(char):
    utf8_code = char.encode("utf-8")
    # Pad utf8 code to 3 bytes if needed, but the struct has Index[3].
    # ASCII chars are 1 byte.
    # e.g. 'W' is 0x57.
    # Index[0] = 0x57, Index[1]=0, Index[2]=0?
    # Let's check how font_new_cn.c handles ASCII if any.
    # If it only has Chinese, they are 3 bytes.
    # If we add ASCII, we should check how the lookup works.
    # Assuming the lookup code handles variable length or checks the first byte.
    # If the struct is fixed 3 bytes Index, then for ASCII, we might need to pad with 0.
    
    index_bytes = list(utf8_code)
    while len(index_bytes) < 3:
        index_bytes.append(0)
        
    bitmap = glyph_to_bitmap(char)
    
    # Format as C struct
    # {
    #     {0x57, 0x00, 0x00},
    #     {0x00, ...}
    # }
    
    idx_str = ", ".join(f"0x{b:02X}" for b in index_bytes)
    msk_str = ", ".join(f"0x{b:02X}" for b in bitmap)
    
    return f"    {{\n        {{{idx_str}}},\n        {{{msk_str}}},\n    }},"

if __name__ == "__main__":
    if not os.path.exists(TTF_PATH):
        print(f"Error: {TTF_PATH} not found.")
        sys.exit(1)

    print("Generating font supplement...")
    with open(OUTPUT_FILE, "w") as f:
        for char in TARGET_CHARS:
            entry = generate_ch_cn_entry(char)
            f.write(f"// Character: {char}\n")
            f.write(entry + "\n")
            
    print(f"Done. Written to {OUTPUT_FILE}")
