"""Pass-file discovery, parsing and pass-relative placement for palettes.

A passes file holds one line per recorded pass, ``index, loop_point_ms pad;``.
Pass i spans [loop_point_i, loop_point_{i+1}); the last pass runs to the end of
the audio. As in analyze.c, a pattern is assigned to the pass it overlaps most,
and its lead-in is the pattern start minus that pass's loop point plus the pad.

A sounds file, when present, holds one line per sound change,
``index, milliseconds number mode;``, where number names a drum set or VST
preset and mode is ``samples`` or ``synth``. A line with no number,
``index, milliseconds audio;``, marks a device that applies no sound.
"""
import os

TEXT_EXTS = ('.txt', '.text')


def _find_sidecar(audio_path, prefix):
    folder = os.path.dirname(os.path.abspath(audio_path))
    stem = os.path.splitext(os.path.basename(audio_path))[0]
    prefix = prefix + stem[7:] if stem.lower().startswith('palette') else prefix
    found = []
    for name in os.listdir(folder):
        base, ext = os.path.splitext(name)
        if ext.lower() not in TEXT_EXTS or not base.lower().startswith(prefix.lower()):
            continue
        rest = base[len(prefix):]
        if rest and rest[0].isalnum():
            continue
        found.append(os.path.join(folder, name))
    return sorted(found, key=lambda p: (len(os.path.basename(p)), os.path.basename(p)))


def find_passes_file(audio_path):
    stem = os.path.splitext(os.path.basename(audio_path))[0]
    prefix = 'passes' + stem[7:] if stem.lower().startswith('palette') else 'passes'
    found = _find_sidecar(audio_path, 'passes')
    if not found:
        raise FileNotFoundError(
            'No passes text file for %s (expected %s*.txt beside the audio)'
            % (os.path.basename(audio_path), prefix))
    return found[0]


def find_sounds_file(audio_path):
    found = _find_sidecar(audio_path, 'sounds')
    return found[0] if found else None


def parse_bar(path):
    with open(path, encoding='utf-8', errors='replace') as f:
        for line in f:
            text = line.strip()
            if text.lower().startswith('# bar'):
                try:
                    return float(text.split()[-1])
                except (ValueError, IndexError):
                    return None
    return None


def parse_passes(path):
    rows = []
    with open(path, encoding='utf-8', errors='replace') as f:
        for line in f:
            text = line.strip()
            if not text or text[0] in '#/':
                continue
            parts = text.replace(';', ' ').replace(',', ' ').split()
            if len(parts) < 2:
                continue
            try:
                rows.append({
                    'index': int(parts[0]),
                    'loop': float(parts[1]) / 1000.0,
                    'pad': float(parts[2]) / 1000.0 if len(parts) > 2 else 0.0,
                })
            except ValueError:
                continue
    if not rows:
        raise ValueError('No pass lines in %s' % path)
    rows.sort(key=lambda r: (r['index'], r['loop']))
    return rows


def parse_sounds(path):
    marks = []
    with open(path, encoding='utf-8', errors='replace') as f:
        for line in f:
            text = line.strip()
            if not text or text[0] in '#/':
                continue
            parts = text.replace(';', ' ').replace(',', ' ').split()
            if len(parts) < 3:
                continue
            try:
                index, time = int(parts[0]), float(parts[1]) / 1000.0
            except ValueError:
                continue
            try:
                number, mode = int(parts[2]), (parts[3].lower() if len(parts) > 3 else 'synth')
            except ValueError:
                number, mode = None, parts[2].lower()
            marks.append({'index': index, 'time': time, 'number': number, 'mode': mode})
    marks.sort(key=lambda m: (m['time'], m['index']))
    return marks


def dominant_mark(start, end, marks, duration):
    best, overlap = None, 0.0
    for i, mark in enumerate(marks):
        begin = max(start, mark['time'])
        finish = min(end, marks[i + 1]['time'] if i + 1 < len(marks) else duration)
        if finish - begin > overlap:
            best, overlap = mark, finish - begin
    return best


def pass_spans(rows, duration):
    spans = []
    for i, row in enumerate(rows):
        end = rows[i + 1]['loop'] if i + 1 < len(rows) else duration
        start = max(0.0, row['loop'])
        end = min(duration, end)
        if end > start:
            spans.append({
                'index': row['index'],
                'loop': row['loop'],
                'pad': row['pad'],
                'start': start,
                'end': end,
            })
    return spans


def find_pass(start, end, spans):
    best = spans[0]
    best_overlap = -1.0
    for span in spans:
        overlap = max(0.0, min(end, span['end']) - max(start, span['loop']))
        if overlap > best_overlap:
            best, best_overlap = span, overlap
    return best


def lead_in(start, span):
    return start - span['loop'] + span['pad']
