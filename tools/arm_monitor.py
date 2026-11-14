import sys, time, serial
from serial.tools import list_ports

def find_port():
    for p in list_ports.comports():
        if p.vid == 0x0483 and p.pid == 0x5740:
            return p.device
    return None

sys.stdout.write("\n=== 机械臂角度监视器（C 页，实时）===\n")
sys.stdout.write("  推【左摇杆 横向】= 底座 a0\n")
sys.stdout.write("  按 Ctrl+C 退出；拔插/烧录后会自动重连\n\n")
sys.stdout.flush()

s = None; port = None; last = ""
while True:
    try:
        if port is None:
            port = find_port()
            if port is None:
                sys.stdout.write("\r等待串口...                            "); sys.stdout.flush()
                time.sleep(0.5); continue
            s = serial.Serial(port, 115200, timeout=0.2)
            sys.stdout.write("\r已连接 %s                            \n" % port); sys.stdout.flush()
        line = s.readline().decode('utf-8', 'replace').strip()
        if 'a0=' in line and 'a1=' in line and 'l0=' in line:
            if line != last:
                last = line
                sys.stdout.write("\r" + line + "        ")
                sys.stdout.flush()
    except KeyboardInterrupt:
        break
    except Exception as e:
        sys.stdout.write("\r串口断开(%s)，重连中...          " % type(e).__name__); sys.stdout.flush()
        try:
            if s: s.close()
        except Exception:
            pass
        s = None; port = None
        time.sleep(1.0)
sys.stdout.write("\n退出\n")