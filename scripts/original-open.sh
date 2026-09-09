#!/bin/sh
# Opens a file in the original program under Wine (from the WindowsInstall
# backup) and captures its main window after it has loaded. Use it to check
# that files Firn writes are accepted by the real thing.
#
#   scripts/original-open.sh some.pspimage [screenshot.png] [wait-seconds]
set -e
here=$(cd "$(dirname "$0")/.." && pwd)
exe="$here/WindowsInstall/Paint Shop Pro 9.exe"
file=$(realpath "$1")
shot=${2:-/tmp/original.png}
wait=${3:-40}
[ -f "$exe" ] || { echo "original executable not found at $exe" >&2; exit 1; }
(cd "$here/WindowsInstall" && timeout $((wait + 60)) wine "$exe" "$(winepath -w "$file")" >/dev/null 2>&1 &)
sleep "$wait"
win=$(xwininfo -root -tree | grep -i 'paint shop pro 9.exe' | grep -v ' 1x1+' |
      awk '{for(i=1;i<=NF;i++) if ($i ~ /^[0-9]+x[0-9]+\+/) {split($i,a,/[x+]/); print a[1]*a[2], $1}}' |
      sort -n | tail -1 | awk '{print $2}')
[ -n "$win" ] || { echo "no window found" >&2; exit 1; }
import -window "$win" "$shot"
echo "captured $shot"
wineserver -k
