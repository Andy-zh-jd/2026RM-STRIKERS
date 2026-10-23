import pymupdf, os
base = r"C:\Users\pc\Desktop\交龙校内赛\27校内赛电控培训资料\27校内赛电控培训资料\手册\电机、电调、电池架、中心板"
doc = pymupdf.open(os.path.join(base, "RoboMaster C620无刷电机调速器使用说明（中英日）V1.01.pdf"))
out = r"F:\Program Files\交龙校内赛\03_docs\img"
# 第 23 页（0-based 22）：电调接口示意图，裁到图那一块
page = doc[22]
r = page.rect
clip = pymupdf.Rect(r.x0, r.y0 + r.height*0.30, r.x1, r.y0 + r.height*0.72)
pix = page.get_pixmap(dpi=900, clip=clip)
dst = os.path.join(out, "C620_SET按键位置.png")
pix.save(dst); print("saved", dst, pix.width, "x", pix.height)
