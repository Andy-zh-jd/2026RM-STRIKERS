from PIL import Image, ImageDraw, ImageFont
src = r"F:\Program Files\交龙校内赛\03_docs\img\annot\Cboard_photo.jpg"
im = Image.open(src).convert("RGB")
W, H = im.size
d = ImageDraw.Draw(im)
try:
    font = ImageFont.truetype("arial.ttf", 42)
except Exception:
    font = ImageFont.load_default()
for frac in [i/10 for i in range(1, 10)]:
    x = int(W*frac); y = int(H*frac)
    d.line([(x,0),(x,H)], fill=(255,0,255), width=3)
    d.line([(0,y),(W,y)], fill=(255,0,255), width=3)
    d.text((x+6, 8), f"{int(frac*100)}%", fill=(255,0,255), font=font)
    d.text((8, y+4), f"{int(frac*100)}%", fill=(255,0,255), font=font)
out = r"F:\Program Files\交龙校内赛\03_docs\img\annot\grid.png"
im.save(out)
print("saved", out, im.size)
