#!/usr/bin/env bash
# Relaunches song_builder.py after it dies, so a killed session resumes from the
# last rating it wrote instead of starting the song again.
set -u
trap 'exit 130' INT
here=$(cd "$(dirname "$0")" && pwd)
while ! python3 "$here/song_builder.py" "$@"; do
  echo 'song_builder exited; resuming from the last rating' >&2
  sleep 2
done
