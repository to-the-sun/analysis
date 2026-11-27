#!/usr/bin/env python3
"""Move old song.json lead-ins into the stem WAVs themselves.

Run this in a folder of project folders. Every subfolder holding a [stems] folder
and a song.json with a "stems" object gets each WAV padded with its lead-in, after
which the lead-ins are removed from song.json.
"""

import json
import os
import sys
import wave
from pathlib import Path

CHUNK = 65536


def stems_folder(project):
    for child in project.iterdir():
        if child.is_dir() and child.name.lower() == "[stems]":
            return child
    return None


def padded_copy(source, target, frames):
    with wave.open(str(source), "rb") as reader:
        channels = reader.getnchannels()
        width = reader.getsampwidth()
        rate = reader.getframerate()

        with wave.open(str(target), "wb") as writer:
            writer.setnchannels(channels)
            writer.setsampwidth(width)
            writer.setframerate(rate)

            block = channels * width
            zeros = b"\0" * (CHUNK * block)
            while frames:
                count = min(frames, CHUNK)
                writer.writeframesraw(zeros[: count * block])
                frames -= count

            while True:
                data = reader.readframes(CHUNK)
                if not data:
                    break
                writer.writeframesraw(data)

            writer.writeframes(b"")


def convert(project):
    stems = stems_folder(project)
    song = project / "song.json"
    if not stems or not song.is_file():
        return False

    data = json.loads(song.read_text(encoding="utf-8"))
    leads = data.get("stems")
    if not isinstance(leads, dict):
        return False
    if not leads:
        del data["stems"]
        write_song(song, data)
        return True

    files = {path.name.lower(): path for path in stems.iterdir() if path.is_file()}
    offsets = {}
    for name, lead in leads.items():
        path = files.get(name.lower())
        if path is None:
            print("  %s: no such stem, ignored" % name)
        elif path.suffix.lower() != ".wav":
            raise ValueError("%s is not a WAV, so it cannot be padded" % path.name)
        else:
            offsets[path] = float(lead)

    wavs = [path for path in files.values() if path.suffix.lower() == ".wav"]
    base = min([0.0] + list(offsets.values()))
    plans = []
    for path in wavs:
        pad = offsets.get(path, 0.0) - base
        if pad <= 0.0:
            continue
        with wave.open(str(path), "rb") as reader:
            rate = reader.getframerate()
        plans.append((path, int(pad * rate + 0.5)))

    written = []
    try:
        for path, frames in plans:
            temp = path.with_name(path.name + ".pad.tmp")
            padded_copy(path, temp, frames)
            written.append((path, temp))
        for path, temp in written:
            os.replace(temp, path)
    except Exception:
        for _, temp in written:
            temp.unlink(missing_ok=True)
        raise

    del data["stems"]
    write_song(song, data)
    print("  %d stem(s) padded" % len(written))
    return True


def write_song(song, data):
    temp = song.with_name(song.name + ".tmp")
    temp.write_text(json.dumps(data, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
    os.replace(temp, song)


def main():
    converted = 0
    for project in sorted(path for path in Path.cwd().iterdir() if path.is_dir()):
        try:
            if convert(project):
                print("%s: converted" % project.name)
                converted += 1
        except Exception as error:
            print("%s: %s" % (project.name, error), file=sys.stderr)

    print("%d project(s) converted" % converted)


if __name__ == "__main__":
    main()
