cd "/mnt/f/Program Files/交龙校内赛"
git add -A
git commit -q -m "整理：删除临时脚本" 2>/dev/null || echo "无改动可提交"
echo "--- 最终状态 ---"
git log --oneline | head -3
echo "--- 工作区 ---"
git status --short | head -5
echo "--- 仓库大小 ---"
du -sh .git | tail -1
echo "--- 文件数 ---"
git ls-files | wc -l