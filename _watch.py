import re, time, serial
from serial.tools import list_ports
port = [p.device for p in list_ports.comports() if p.vid == 0x0483]
if not port: print("no port"); raise SystemExit
s = serial.Serial(port[0], 115200, timeout=0.2)
cols, sLs, sRs = [], [], []
t0 = time.time()
while time.time() - t0 < 12:
    line = s.readline().decode('utf-8','replace')
    m = re.search(r'col=(-?\d+\.\d+)', line); l = re.search(r'sL=(\d+)', line); r = re.search(r'sR=(\d+)', line)
    if m: cols.append(float(m.group(1)))
    if l: sLs.append(int(l.group(1)))
    if r: sRs.append(int(r.group(1)))
s.close()
def rng(name, xs):
    if not xs: print(f"{name}: 没读到"); return
    print(f"{name}: 样本 {len(xs)}  最小 {min(xs)}  最大 {max(xs)}  不同值 {sorted(set(xs))[:6]}")
rng("col", cols); rng("sL", sLs); rng("sR", sRs)
