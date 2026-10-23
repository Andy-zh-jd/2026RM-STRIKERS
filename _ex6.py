import os
from pypdf import PdfReader
base = r"C:\Users\pc\Desktop\交龙校内赛\27校内赛电控培训资料\27校内赛电控培训资料\手册\电机、电调、电池架、中心板"
r = PdfReader(os.path.join(base, "RoboMaster C620无刷电机调速器使用说明（中英日）V1.01.pdf"))
txt = []
for i, p in enumerate(r.pages):
    txt.append(f"\n===== page {i+1} =====\n"); txt.append(p.extract_text() or "")
open(r"F:\Program Files\交龙校内赛\_c620.txt","w",encoding="utf-8").write("".join(txt))
print("ok", len(r.pages))
