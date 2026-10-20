import os
from pypdf import PdfReader
p = r"C:\Users\pc\Desktop\交龙校内赛\27校内赛电控培训资料\27校内赛电控培训资料\手册\天地飞 ET08遥控器 使用说明书.pdf"
out = r"F:\Program Files\交龙校内赛\_m\ET08.txt"
r = PdfReader(p)
txt = []
for i, page in enumerate(r.pages):
    txt.append(f"\n===== page {i+1} =====\n" + (page.extract_text() or ""))
open(out, "w", encoding="utf-8").write("".join(txt))
print("pages", len(r.pages), "bytes", os.path.getsize(out))
