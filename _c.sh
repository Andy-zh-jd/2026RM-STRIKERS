cd "/mnt/f/Program Files/交龙校内赛"
git add -A
git commit -q -m "上传准备：推送脚本(SSH/可指定repo) + 06_upload 说明 + 完整历史 bundle" 2>/dev/null || echo "无改动"
git log --oneline | head -2
ls -1 06_upload