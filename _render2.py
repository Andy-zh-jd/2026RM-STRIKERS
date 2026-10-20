import os
import pymupdf

base = r"C:\Users\pc\Desktop\交龙校内赛\27校内赛电控培训资料\27校内赛电控培训资料\手册"
out  = r"F:\Program Files\交龙校内赛\03_docs\img\manual"
os.makedirs(out, exist_ok=True)
for f in os.listdir(out):
    os.remove(os.path.join(out, f))

jobs = [
    ("Cboard", "C板原理图、位号图、用户手册/RoboMaster  开发板 C 型用户手册.pdf", [5,6,10,11,14,15,16], 2.6),
    ("C620",   "电机、电调、电池架、中心板/RoboMaster C620无刷电机调速器使用说明（中英日）V1.01.pdf", [23,25,26], 4.5),
    ("C610",   "电机、电调、电池架、中心板/RM C610无刷电机调速器使用说明 发布版.pdf", [4,5], 2.6),
    ("Rack",   "电机、电调、电池架、中心板/RoboMaster电池架使用说明.pdf", [10,13], 4.0),
]
for name, rel, pages, zoom in jobs:
    path = os.path.join(base, rel.replace("/", os.sep))
    doc = pymupdf.open(path)
    for p in pages:
        if p-1 >= doc.page_count: continue
        pix = doc[p-1].get_pixmap(matrix=pymupdf.Matrix(zoom, zoom))
        dst = os.path.join(out, f"{name}_p{p}.png")
        pix.save(dst)
        print(f"OK {name}_p{p} {pix.width}x{pix.height} {os.path.getsize(dst)//1024}KB")
    doc.close()
