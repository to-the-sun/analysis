"""Watch the projects folder for palette/passes pairs and copy each pair zipped into the Google Drive folder."""

import ctypes
import json
import os
import time
import traceback
import zipfile
from datetime import datetime, timedelta
from io import BytesIO
from pathlib import Path

PROJECTS = Path(r"D:\[Library]\[Audio]\[Works]\[Projects]")
DESTINATION = Path(r"G:\My Drive\[palettes]")
POLL_SECONDS = 30
SETTLE_SECONDS = 10

HERE = Path(__file__).resolve().parent
STATE = HERE / "palette_watcher_drive_state.json"
LOG = HERE / "palette_watcher_drive.log"
PAIR = (("palette_", ".wav"), ("passes_", ".txt"))
EXTRA = (("sounds_", ".txt"),)


def log(message):
    with LOG.open("a", encoding="utf-8") as handle:
        handle.write(f"{datetime.now():%Y-%m-%d %H:%M:%S} {message}\n")


def say(message):
    print(f"{datetime.now():%H:%M:%S} {message}", flush=True)


def describe():
    return traceback.format_exc().strip().splitlines()[-1]


def fail(message):
    log(message)
    ctypes.windll.user32.MessageBoxW(None, message, "Palette watcher", 0x10)
    raise SystemExit(1)


def store(name, data):
    target = DESTINATION / name
    if target.exists() and target.read_bytes() == data:
        return False
    target.write_bytes(data)
    return True


def bundle(entries):
    buffer = BytesIO()
    with zipfile.ZipFile(buffer, "w", zipfile.ZIP_DEFLATED) as archive:
        for entry in entries:
            archive.write(entry, entry.name)
    return buffer.getvalue()


def modified(entry):
    return datetime.fromtimestamp(entry.stat().st_mtime)


def pair_dirs():
    # Projects sit directly below PROJECTS; palette batches are also collected
    # into [palettes] folders at any depth inside a project.
    found = {entry for entry in PROJECTS.iterdir() if entry.is_dir()}
    for root, dirs, _ in os.walk(PROJECTS):
        found |= {Path(root, name) for name in dirs if name.lower() == "[palettes]"}
    return sorted(found)


def pairs(since, until):
    for folder in pair_dirs():
        found = {}
        for entry in folder.iterdir():
            name = entry.name.lower()
            for prefix, extension in PAIR + EXTRA:
                if name.startswith(prefix) and name.endswith(extension):
                    found.setdefault(name[len(prefix):-len(extension)], {})[prefix] = entry
        settled = {
            key: match
            for key, match in found.items()
            if all(prefix in match for prefix, _ in PAIR)
            and all(modified(entry) <= until for entry in match.values())
        }
        if any(since < modified(entry) for match in settled.values() for entry in match.values()):
            yield folder.relative_to(PROJECTS).parts[0], [settled[key][prefix] for key in sorted(settled)
                                                          for prefix, _ in PAIR + EXTRA if prefix in settled[key]]


def load_state():
    try:
        return datetime.fromisoformat(json.loads(STATE.read_text(encoding="utf-8"))["last_check"])
    except (OSError, ValueError, KeyError):
        return None


def save_state(moment):
    STATE.write_text(json.dumps({"last_check": moment.isoformat()}), encoding="utf-8")


def sweep():
    since = load_state()
    if since is None:
        baseline = datetime.now()
        save_state(baseline)
        message = f"no previous check time; nothing delivered, next check starts at {baseline:%Y-%m-%d %H:%M:%S}"
        say(message)
        log(message)
        return
    until = datetime.now() - timedelta(seconds=SETTLE_SECONDS)
    delivered = 0
    earliest_failure = None
    for folder, entries in pairs(since, until):
        data = b""
        try:
            data = bundle(entries)
            name = f"{folder}.zip"
            if store(name, data):
                message = f"delivered {DESTINATION / name} ({len(data) / 1048576:.1f} MB)"
                say(message)
                log(message)
                delivered += 1
            else:
                say(f"already delivered: {DESTINATION / name}")
        except Exception:
            message = f"failed {folder}.zip ({len(data) / 1048576:.1f} MB): {describe()}"
            say(message)
            log(message)
            stuck = min(modified(entry) for entry in entries)
            earliest_failure = stuck if earliest_failure is None else min(earliest_failure, stuck)
    if not delivered:
        say(f"nothing new up to {until:%H:%M:%S}")
    save_state(until if earliest_failure is None else min(until, earliest_failure - timedelta(seconds=1)))


def main():
    ctypes.windll.user32.ShowWindow(ctypes.windll.kernel32.GetConsoleWindow(), 6)
    if not DESTINATION.is_dir():
        fail(f"{DESTINATION} is not a folder, so nothing can be delivered. Set DESTINATION near the top of {Path(__file__).name} to the Google Drive folder the zips should land in.")
    say(f"watching {PROJECTS}")
    say(f"delivering to {DESTINATION}")
    say(f"log: {LOG}")
    log(f"watching {PROJECTS}")
    problem = None
    while True:
        try:
            sweep()
            problem = None
        except Exception:
            message = f"failed: {describe()}"
            if message != problem:
                say(message)
                log(message)
                problem = message
        time.sleep(POLL_SECONDS)


if __name__ == "__main__":
    main()
