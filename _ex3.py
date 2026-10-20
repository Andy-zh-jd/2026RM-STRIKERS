import os
from pypdf import PdfReader
base = r"C:\Users\pc\Desktop\交龙校内赛\27校内赛电控培训资料\27校内赛电控培训资料\手册\电机、电调、电池架、中心板"
out = r"F:\Program Files\交龙校内赛\_m"
os.makedirs(out, exist_ok=True)
for name in ["RM C610无刷电机调速器使用说明 发布版.pdf", "RM M2006 P36直流无刷减速电机使用说明.pdf"]:
    r = PdfReader(os.path.join(base, name))
    txt = []
    for i, p in enumerate(r.pages):
        txt.append(f"\n===== page {i+1} =====\n" + (p.extract_text() or ""))
    dst = os.path.join(out, name.split()[1] + ".txt")
    open(dst, "w", encoding="utf-8").write("".join(txt))
    print(name, len(r.pages), "pages")
