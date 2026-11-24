#!/bin/sh
# Run song_builder.py until it exits cleanly. A killed or crashed run restarts
# and continues from its rating cache instead of rating the song again.
# usage: song_builder.sh <log-file> --spec <spec> --report-dir <dir> [more args]
log=$1
shift
here=$(dirname "$0")
while :; do
  python3 -u "$here/song_builder.py" "$@"
  status=$?
  [ "$status" -eq 0 ] && exit 0
  printf '\n[%s] exit %s; restarting from cache\n' "$(date -Is)" "$status"
  sleep 10
done >> "$log" 2>&1
