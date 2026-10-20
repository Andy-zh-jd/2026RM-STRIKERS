import os
import pymupdf

base = r"C:\Users\pc\Desktop\交龙校内赛\27校内赛电控培训资料\27校内赛电控培训资料\手册"
out  = r"F:\Program Files\交龙校内赛\03_docs\img\manual"
os.makedirs(out, exist_ok=True)

jobs = [
    ("Cboard", os.path.join(base, "C板原理图、位号图、用户手册", "RoboMaster  开发板 C 型用户手册.pdf"), [5,6,10,11,13,14,15,16]),
    ("C620",   os.path.join(base, "电机、电调、电池架、中心板", "RoboMaster C620无刷电机调速器使用说明（中英日）V1.01.pdf"), [45,46,47,48]),
    ("C610",   os.path.join(base, "电机、电调、电池架、中心板", "RM C610无刷电机调速器使用说明 发布版.pdf"), [3,4,5,6]),
    ("Rack",   os.path.join(base, "电机、电调、电池架、中心板", "RoboMaster电池架使用说明.pdf"), [10,12,13]),
]
for name, path, pages in jobs:
    if not os.path.exists(path):
        print("MISS", path); continue
    doc = pymupdf.open(path)
    for p in pages:
        if p-1 >= doc.page_count: continue
        page = doc[p-1]
        pix = page.get_pixmap(matrix=pymupdf.Matrix(1.6, 1.6))
        dst = os.path.join(out, f"{name}_p{p}.png")
        pix.save(dst)
        print(f"OK {name}_p{p}.png {pix.width}x{pix.height} {os.path.getsize(dst)//1024}KB")
    doc.close()
