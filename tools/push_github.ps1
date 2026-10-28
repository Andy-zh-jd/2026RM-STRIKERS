param(
    [string]$Repo = "Jasonchenczx/2026RM-STRIKERS"
)

# 把本地仓库推到 GitHub（走 SSH）
# 用法：
#   powershell -File tools\push_github.ps1
#       -> 推到自己的 fork（Jasonchenczx/2026RM-STRIKERS）
#   powershell -File tools\push_github.ps1 -Repo Andy-zh-jd/2026RM-STRIKERS
#       -> 拿到协作者权限后，直接推原仓库
#
# 前提：本机 SSH 公钥已加到 GitHub 账号
#       已验证：ssh -T git@github.com -> "Hi Jasonchenczx!"

$ErrorActionPreference = 'Continue'

Write-Host "=== 1. 检查 SSH 身份 ===" -ForegroundColor Cyan
wsl -- bash -lc "ssh -o BatchMode=yes -o StrictHostKeyChecking=accept-new -T git@github.com 2>&1 | head -2"

Write-Host "`n=== 2. 指向 $Repo 并推送 main ===" -ForegroundColor Cyan
wsl -- bash -lc "cd '/mnt/f/Program Files/交龙校内赛' && git remote set-url origin git@github.com:$Repo.git && GIT_SSH_COMMAND='ssh -o BatchMode=yes -o StrictHostKeyChecking=accept-new' git push -u origin main 2>&1 | tail -10"

Write-Host "`n=== 3. 结果 ===" -ForegroundColor Cyan
wsl -- bash -lc "cd '/mnt/f/Program Files/交龙校内赛' && git log --oneline | head -3 && echo '---' && git rev-list --count HEAD && echo '个提交'"
