import os
import pymupdf

base = r"C:\Users\pc\Desktop\交龙校内赛\27校内赛电控培训资料\27校内赛电控培训资料\手册"
files = {
 "C620": os.path.join(base, "电机、电调、电池架、中心板", "RoboMaster C620无刷电机调速器使用说明（中英日）V1.01.pdf"),
 "Rack": os.path.join(base, "电机、电调、电池架、中心板", "RoboMaster电池架使用说明.pdf"),
 "Cboard": os.path.join(base, "C板原理图、位号图、用户手册", "RoboMaster  开发板 C 型用户手册.pdf"),
}
keys = ["接口及线序", "接线方法", "使用说明", "PIN", "线序", "电源输出", "安装"]
for name, p in files.items():
    doc = pymupdf.open(p)
    print("=====", name, doc.page_count, "pages =====")
    for i in range(doc.page_count):
        t = doc[i].get_text()
        hit = [k for k in keys if k in t]
        if hit:
            print(f"  p{i+1}: {hit}")
    doc.close()
