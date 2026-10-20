from PIL import Image, ImageDraw, ImageFont

src = r"F:\Program Files\交龙校内赛\03_docs\img\annot\Cboard_photo.jpg"
im = Image.open(src).convert("RGB")
W, H = im.size
d = ImageDraw.Draw(im)

def F(sz):
    return ImageFont.truetype(r"C:\Windows\Fonts\msyh.ttc", sz)

f_big = F(58); f_mid = F(46); f_sml = F(38)

GREEN=(0,170,80); RED=(210,40,35); YELLOW=(240,170,0); WHITE=(255,255,255); DARK=(20,20,20)

# 1) 整块排针
d.rounded_rectangle([375,1385, 885,1725], radius=18, outline=GREEN, width=8)
d.text((390, 1300), "① 这整块 = PWM/DBUS 排针（3 排 × 8 列）", font=f_mid, fill=GREEN)

# 2) 指向 DBUS 那一端
x0,y0 = 585,1445
d.ellipse([x0-26,y0-26, x0+26,y0+26], outline=RED, width=8)
d.line([(x0, y0), (x0-120, y0-170)], fill=RED, width=7)
d.text((190, 1180), "② 3 针插头插这个角（紧挨 DBUS 丝印）", font=f_mid, fill=RED)
d.text((190, 1240), "   竖着跨 3 排，不是躺着插", font=f_mid, fill=RED)

# 3) 丝印标注
d.text((560, 1215), "DBUS 丝印", font=f_sml, fill=YELLOW)
d.line([(600,1255),(640,1300)], fill=YELLOW, width=5)
d.text((300, 1120), "5V 丝印", font=f_sml, fill=YELLOW)
d.line([(360,1160),(365,1290)], fill=YELLOW, width=5)

# 4) 右下角放大示意图
ox, oy = 940, 1500
cw, ch, gap = 62, 46, 10
d.rounded_rectangle([ox-40, oy-90, ox+cw*8+gap*7+60, oy+ch*3+gap*2+230], radius=16, outline=(255,255,255), width=6)
d.text((ox-10, oy-76), "正确插法（放大）", font=f_big, fill=WHITE)
rows = ["A  GND", "B  5V", "C  DBUS"]
for r in range(3):
    d.text((ox-10, oy+r*(ch+gap)+6), rows[r], font=f_sml, fill=WHITE)
    for c in range(8):
        x = ox+110+c*(cw+gap); y = oy+r*(ch+gap)
        if c == 7:
            d.rectangle([x,y,x+cw,y+ch], fill=(60,190,110), outline=WHITE, width=3)
        else:
            d.rectangle([x,y,x+cw,y+ch], fill=(200,190,120), outline=(120,110,50), width=2)
for c in range(8):
    d.text((ox+110+c*(cw+gap)+22, oy+3*(ch+gap)+8), str(c+1), font=f_sml, fill=WHITE)

# 竖插头示意（第 8 列）
px = ox+110+7*(cw+gap)-12
d.rounded_rectangle([px, oy-14, px+cw+24, oy+3*ch+2*gap+14], radius=10, outline=GREEN, width=8)
d.text((px-6, oy+3*(ch+gap)+60), "插头", font=f_sml, fill=GREEN)

# 错误示意：横插
ey = oy+3*(ch+gap)+110
d.text((ox-10, ey), "✗ 横着插 = 3 根针全压在 A 排（全是地）→ 没有 5V", font=f_sml, fill=(255,150,140))

im.save(r"F:\Program Files\交龙校内赛\03_docs\img\annot\C板DBUS插法.jpg", quality=92)
print("ok", im.size)
