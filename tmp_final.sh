cd "/mnt/f/Program Files/?????"
rm -f _clean.sh
git add -A
git commit -q -m "????????????" && echo "???"
GIT_SSH_COMMAND='ssh -o BatchMode=yes' timeout 120 git push origin main 2>&1 | tail -3
echo "=== ???? ==="
git status --short | wc -l
git rev-list --count HEAD
git status -sb | head -1