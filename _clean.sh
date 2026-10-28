cd "/mnt/f/Program Files/交龙校内赛"
echo "=== 被跟踪的临时文件 ==="
git ls-files | grep -E '^(_c\.sh|_m/|_packs/|_backup_TOOLS\.INI|_watch\.py|_串口抓包)' | head -20
echo "=== 删除并提交 ==="
git rm -r --cached -q _m _packs _c.sh _backup_TOOLS.INI 2>/dev/null
rm -rf _m _c.sh
printf '\n# 临时/中间产物\n_m/\n_packs/\n_c.sh\n_backup_TOOLS.INI\n_watch.py\n_串口抓包.txt\n' >> .gitignore
git add -A
git commit -q -m "清理：移除误入库的临时提取目录与脚本，补 .gitignore" && echo "已提交"
echo "=== 推送 ==="
GIT_SSH_COMMAND='ssh -o BatchMode=yes' timeout 120 git push origin main 2>&1 | tail -4