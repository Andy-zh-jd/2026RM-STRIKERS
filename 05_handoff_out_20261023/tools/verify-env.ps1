# 一键环境自检（怀疑环境有问题时先跑这个）
# 用法： powershell -ExecutionPolicy Bypass -File tools\verify-env.ps1
$ErrorActionPreference = 'Continue'
$script:ok = $true

function Section($t) { Write-Host "`n=== $t ===" -ForegroundColor Cyan }

function Check($name, $exe, $arg) {
    $cmd = Get-Command $exe -ErrorAction SilentlyContinue
    if ($cmd) {
        $v = (& $exe $arg 2>&1 | Select-Object -First 1)
        Write-Host ("  OK   {0,-24} {1}" -f $name, $v)
    } else {
        Write-Host ("  MISS {0,-24} 没找到 {1}" -f $name, $exe) -ForegroundColor Red
        $script:ok = $false
    }
}

Section 'A 线：交叉编译链'
Check 'arm-none-eabi-gcc' 'arm-none-eabi-gcc' '--version'
Check 'arm-none-eabi-gdb' 'arm-none-eabi-gdb' '--version'
Check 'cmake' 'cmake' '--version'
Check 'ninja' 'ninja' '--version'
Check 'pyocd' 'pyocd' '--version'

Section 'B 线：仿真环境'
Check 'python' 'python' '--version'
& python -c "import numpy, scipy, matplotlib, serial; print('  OK   numpy %s / scipy %s / matplotlib %s / pyserial %s' % (numpy.__version__, scipy.__version__, matplotlib.__version__, serial.__version__))" 2>&1 | ForEach-Object { $_ }
if ($LASTEXITCODE -ne 0) { Write-Host '  MISS python 包不全（pip install numpy scipy matplotlib pyserial）' -ForegroundColor Red; $script:ok = $false }

Section '调试探针'
$pyocd = & pyocd list 2>&1
$pyocd | ForEach-Object { "  $_" }

Section '固件产物'
$elf = '01_firmware\RM2027_School_Comp_Frame\build\RM_Frame_C.elf'
if (Test-Path -LiteralPath $elf) {
    $size = (Get-Item -LiteralPath $elf).Length
    Write-Host ("  OK   {0}  ({1:N0} B)" -f $elf, $size)
    $bin = '01_firmware\RM2027_School_Comp_Frame\build\RM_Frame_C.bin'
    if (Test-Path -LiteralPath $bin) {
        $b = [System.IO.File]::ReadAllBytes((Resolve-Path -LiteralPath $bin))
        Write-Host ("       向量表：SP=0x{0:X8}  Reset=0x{1:X8}  bin={2} B" -f ([BitConverter]::ToUInt32($b, 0)), ([BitConverter]::ToUInt32($b, 4)), $b.Length)
    }
} else {
    Write-Host '  --   还没编译过：cd 01_firmware\RM2027_School_Comp_Frame; cmake -G Ninja -B build; cmake --build build' -ForegroundColor Yellow
}

Section 'MATLAB'
$matlab = 'D:\Program Files\bin\matlab.exe'
if (Test-Path -LiteralPath $matlab) {
    Write-Host '  OK   MATLAB 在 D:\Program Files\bin\matlab.exe（Simulink 未安装，.slx 打不开）'
} else {
    Write-Host '  MISS MATLAB'
}

Section '结论'
if ($script:ok) { Write-Host '  环境完好，可以干活。' -ForegroundColor Green } else { Write-Host '  有缺项，按上面的 MISS 处理。' -ForegroundColor Yellow }
