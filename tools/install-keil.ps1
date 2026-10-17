# 装 Keil MDK 538a（需要管理员，本机当前账户不是管理员，所以脚本只负责把安装器拉起来）
#
# 用法（二选一）：
#   1) 右键本文件 → “使用 PowerShell 运行”（会弹 UAC，点“是”）
#   2) 管理员 PowerShell 里执行： powershell -ExecutionPolicy Bypass -File tools\install-keil.ps1
#
# 装完要做的事：
#   - 装 ST-Link 驱动：解压 “桌面\交龙校内赛\...\Keil安装包及补丁\可能遇到的问题和解决方法\stsw-link009.zip”，
#     右键 dpinst_amd64.exe → 以管理员身份运行
#   - 打开 01_firmware\RM2027_School_Comp_Frame\MDK-ARM\RM_Frame_C.uvprojx
#   - 注意：MDK-Lite 有 32 KB 代码上限，而本框架编译出来 41.8 KB，
#     所以不买 License 的话 Keil 只能看代码、不能链接。
#     ==> 日常开发请用 GCC 链（VS Code 里 Ctrl+Shift+B），Keil 只作对照/应急。
#
# 本脚本不会碰 keygen 和破解补丁：那属于规避授权，自己决定、自己负责。

$ErrorActionPreference = 'Stop'

$installer = 'C:\Users\pc\Desktop\交龙校内赛\27校内赛电控培训资料\27校内赛电控培训资料\Keil安装包及补丁\MDK538a（Keil安装包）.EXE'
$driverZip = 'C:\Users\pc\Desktop\交龙校内赛\27校内赛电控培训资料\27校内赛电控培训资料\Keil安装包及补丁\可能遇到的问题和解决方法\stsw-link009.zip'

$isAdmin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)

if (-not (Test-Path -LiteralPath $installer)) {
    Write-Host "找不到安装包：$installer" -ForegroundColor Red
    exit 1
}

if (-not $isAdmin) {
    Write-Host '当前不是管理员，正在请求提权（会弹 UAC，请点“是”）…' -ForegroundColor Yellow
    Start-Process -FilePath 'powershell.exe' -Verb RunAs -ArgumentList @(
        '-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', "'$PSCommandPath'"
    )
    exit 0
}

Write-Host '开始安装 Keil MDK（图形界面，跟着点 Next）…' -ForegroundColor Cyan
Start-Process -FilePath $installer -Wait

Write-Host ''
Write-Host 'Keil 安装器已结束。接下来装 ST-Link 驱动（可选，但装了 pyOCD 才好用）：' -ForegroundColor Cyan
if (Test-Path -LiteralPath $driverZip) {
    $tmp = Join-Path $env:TEMP 'stsw-link009'
    if (Test-Path -LiteralPath $tmp) { Remove-Item -Recurse -Force $tmp }
    Expand-Archive -LiteralPath $driverZip -DestinationPath $tmp -Force
    $dpinst = Get-ChildItem -LiteralPath $tmp -Recurse -Filter 'dpinst_amd64.exe' | Select-Object -First 1
    if ($dpinst) {
        Write-Host "运行 $($dpinst.FullName)"
        Start-Process -FilePath $dpinst.FullName -Wait
    } else {
        Write-Host '解压后没找到 dpinst_amd64.exe，请手动到 tools 里指定驱动安装。' -ForegroundColor Yellow
    }
} else {
    Write-Host "没找到驱动包：$driverZip" -ForegroundColor Yellow
}

Write-Host ''
Write-Host '全部完成。验证：插上 C 板 + ST-Link，然后跑 pyocd list 应该能看到探针。' -ForegroundColor Green
