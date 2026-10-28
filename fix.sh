cd "/mnt/f/Program Files/?????"
rm -f tmp_final.sh
git rm --cached -q tmp_final.sh 2>/dev/null
printf '\ntmp_*.sh\n' >> .gitignore
git add -A
git commit -q -m "chore: remove leftover temp script, ignore tmp_*.sh"
GIT_SSH_COMMAND='ssh -o BatchMode=yes' timeout 120 git push origin main 2>&1 | tail -3
echo "=== local ==="
git status --short | wc -l
git rev-list --count HEAD
git log --oneline -1