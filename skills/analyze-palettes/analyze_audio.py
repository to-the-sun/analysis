#!/usr/bin/env python3
"""Phrase extractor for palettes.

A segment is one iteration of a loop, a tracked element is an onset or a pitch
that returns to the same place in the segment, a pattern is a tracked element's
run of at least three contiguous segments, and a phrase is the extent of the
patterns that share a loop and overlap by at least one segment. This tool emits
phrases; the report and song steps use them as their patterns.

The audio is cut into regions where the loop length holds, each region's loop is
measured on a lag-similarity curve and confirmed by how tightly the attacks sit
on the lattice, and every element is then tracked segment by segment. The
element's own period decides its loop, so an element repeated every second
segment gets twice the region loop, as a listener hears it.

Usage:
    python3 analyze_audio.py file.wav [more.wav ...] [--json out.json]
                              [--spec report-spec.json]

For non-WAV input, ffmpeg must be on PATH; the file is converted to a temporary
44.1 kHz mono WAV.
"""
import argparse
import json
import os
import subprocess
import tempfile

import numpy as np
import scipy.io.wavfile as wavfile
import scipy.signal as sig

import passes

BANDS = (('sub', 30, 90), ('low', 90, 250), ('mid', 250, 900),
         ('hi', 900, 4000), ('top', 4000, 16000))
PITCH = ('A', 'A#', 'B', 'C', 'C#', 'D', 'D#', 'E', 'F', 'F#', 'G', 'G#')
LOOP_MIN, LOOP_MAX = 0.5, 8.0
MIN_SEGMENTS = 3
LEAD = 0.25          # fraction of a loop of lead-in before the first onset
OFFSET_TOL = 0.08    # element offsets are one place if this close, in loops
CHROMA_MATCH = 0.80  # pitch classes this similar are the same element
COLORS = ('#4dabf7', '#ffa94d', '#51cf66', '#e599f7', '#ffd43b',
          '#ff6b6b', '#38d9a9', '#748ffc', '#f783ac', '#a9e34b')


def load_audio(path):
    ext = os.path.splitext(path)[1].lower()
    if ext == '.wav':
        sr, data = wavfile.read(path)
    else:
        fd, tmp = tempfile.mkstemp(suffix='.wav')
        os.close(fd)
        try:
            subprocess.run(['ffmpeg', '-v', 'error', '-y', '-i', path,
                            '-ac', '1', '-ar', '44100', tmp], check=True)
            sr, data = wavfile.read(tmp)
        finally:
            try:
                os.unlink(tmp)
            except OSError:
                pass
    if data.ndim > 1:
        x = data.astype(np.float64).mean(axis=1)
    else:
        x = data.astype(np.float64)
    if data.dtype == np.int16:
        x /= 32768.0
    elif data.dtype == np.uint8:
        x = (x - 128.0) / 128.0
    elif data.dtype == np.int32:
        x /= 2147483648.0
    elif data.dtype in (np.float32, np.float64):
        pass
    else:
        raise ValueError('Unsupported sample type: %s' % data.dtype)
    return int(sr), x.astype(np.float32)


def melbank(sr, n_fft, nbands=64, fmin=30, fmax=None):
    fmax = fmax or sr / 2.0

    def hz2mel(f):
        return 2595.0 * np.log10(1.0 + f / 700.0)

    def mel2hz(m):
        return 700.0 * (10.0 ** (m / 2595.0) - 1.0)

    pts = mel2hz(np.linspace(hz2mel(fmin), hz2mel(fmax), nbands + 2))
    freqs = np.fft.rfftfreq(n_fft, 1.0 / sr)
    B = np.zeros((nbands, len(freqs)))
    for i in range(nbands):
        lo, ce, hi = pts[i], pts[i + 1], pts[i + 2]
        B[i] = np.clip(np.minimum((freqs - lo) / (ce - lo + 1e-9),
                                  (hi - freqs) / (hi - ce + 1e-9)), 0, None)
    return B


def spectral(x, sr):
    nfft = 2048
    if len(x) < nfft:
        nfft = 1 << max(8, int(np.ceil(np.log2(max(16, len(x))))))
    hop = nfft // 2
    f, _, Z = sig.stft(x, fs=sr, nperseg=nfft, noverlap=nfft - hop,
                       boundary=None, padded=False)
    P = (np.abs(Z) ** 2).astype(np.float32)
    M = np.log1p(melbank(sr, nfft) @ P)
    F = M - M.mean(axis=1, keepdims=True)
    F = F / (F.std(axis=1, keepdims=True) + 1e-9)
    F = F / (np.linalg.norm(F, axis=0, keepdims=True) + 1e-9)
    return f, P, F.astype(np.float32), sr / hop


def detect_sections(x, sr, threshold_db=-55.0, min_silence=2.0, min_section=1.0):
    """Takes separated by silence, on a 10 ms grid.

    A held envelope keeps sparse percussion active, so a drum hit decaying below
    the floor does not cut the take. Short islands of sound (single hits) join
    their neighbours; a long take is cut at any real gap, however brief.
    """
    win = max(1, int(round(0.01 * sr)))
    n = len(x) // win
    if n < 2:
        return [(0.0, len(x) / sr)]
    rms = np.sqrt(np.mean(x[:n * win].reshape(n, win).astype(np.float64) ** 2, axis=1))
    db = 20.0 * np.log10(rms + 1e-12)
    hold = int(round(0.5 / 0.01))
    db = np.maximum.reduce([db[i:len(db) - hold + i + 1] if hold > 1 else db
                            for i in range(hold)]) if hold > 1 else db
    active = db > threshold_db
    runs, start = [], None
    for i, a in enumerate(active):
        if a and start is None:
            start = i
        elif not a and start is not None:
            runs.append([start * win / sr, i * win / sr])
            start = None
    if start is not None:
        runs.append([start * win / sr, n * win / sr])
    if not runs:
        return [(0.0, len(x) / sr)]
    merged = [runs[0]]
    for s, e in runs[1:]:
        gap = s - merged[-1][1]
        short = min(merged[-1][1] - merged[-1][0], e - s) < 8.0
        if gap < (min_silence if short else 0.35):
            merged[-1][1] = e
        else:
            merged.append([s, e])
    out = [(max(0.0, s), min(len(x) / sr, e)) for s, e in merged if e - s >= min_section]
    return out or [(0.0, len(x) / sr)]


def detect_attacks(x, sr):
    """Attack times from a broadband level rise."""
    win = int(0.01 * sr)
    n = len(x) // win
    env = np.sqrt((x[:n * win].reshape(n, win).astype(np.float64) ** 2).mean(axis=1))
    db = 20.0 * np.log10(env + 1e-12)
    rise = db - np.concatenate([np.full(4, -120.0), db[:-4]])
    pk, _ = sig.find_peaks(rise, height=6.0, distance=8)
    return pk * win / sr


def event_features(freqs, P, fps, times):
    """Band levels, pitch class and a coarse label for each attack."""
    lo = (freqs >= 40) & (freqs <= 2000)
    pc = np.round(12.0 * np.log2(freqs[lo] / 440.0) + 69).astype(int) % 12
    events = []
    for t in times:
        a = int(round(t * fps))
        b = a + max(2, int(round(0.22 * fps)))
        if a < 0 or b > P.shape[1]:
            continue
        spec = P[:, a:b].mean(axis=1).astype(np.float64)
        levels = {}
        for name, low, high in BANDS:
            mask = (freqs >= low) & (freqs < high)
            levels[name] = 10.0 * np.log10(spec[mask].mean() + 1e-12) if np.any(mask) else -120.0
        chroma = np.zeros(12)
        np.add.at(chroma, pc, np.sqrt(spec[lo]))
        norm = np.linalg.norm(chroma)
        chroma = chroma / norm if norm > 0 else chroma
        loudest = max(levels, key=levels.get)
        events.append({
            't': round(float(t), 3),
            'bands': {k: round(v, 1) for k, v in levels.items()},
            'chroma': chroma,
            'label': 'kick' if loudest in ('sub', 'low') else
                     ('bright' if loudest in ('hi', 'top') else 'mid'),
            'pitch': PITCH[int(chroma.argmax())] if norm > 0 else None,
        })
    return events


def lag_sim(F, i0, i1, loop, fps):
    """Mean frame similarity between the region and itself one loop later."""
    lagf = loop * fps
    base = int(np.floor(lagf))
    frac = lagf - base
    j1 = min(i1 - base - 1, F.shape[1] - base - 2)
    if j1 <= i0 + 2:
        return -1.0
    A = F[:, i0:j1]
    B = F[:, i0 + base:j1 + base] * (1.0 - frac) + F[:, i0 + base + 1:j1 + base + 1] * frac
    return float((A * B).sum(axis=0).mean())


def align_score(times, loop, width=0.1):
    """Largest fraction of attacks sharing one phase of the lattice."""
    if len(times) < 3:
        return 0.0
    phase = np.sort(np.mod(np.asarray(times, dtype=np.float64), loop))
    phase = np.concatenate([phase, phase + loop])
    ends = np.searchsorted(phase, phase + width, side='right')
    counts = ends - np.arange(len(phase))
    return float(counts[:len(times)].max() / len(times))


def best_loop(F, fps, t0, t1, times=()):
    """Loop length for a region, from how well the audio repeats one loop later.

    The shortest qualifying peak wins, because a stronger match two or four
    loops out is the same loop with another layer on top. Where attacks exist
    the loop must also explain them, which keeps a steady texture from winning.
    """
    i0, i1 = max(0, int(round(t0 * fps))), min(F.shape[1], int(round(t1 * fps)))
    hi = min(LOOP_MAX, (t1 - t0) / MIN_SEGMENTS)
    if hi <= LOOP_MIN or i1 - i0 < 8:
        return None, -1.0
    lags = np.arange(LOOP_MIN, hi, 0.005)
    scores = np.array([lag_sim(F, i0, i1, L, fps) for L in lags])
    scores = np.nan_to_num(scores, nan=-1.0)
    pk, _ = sig.find_peaks(scores, prominence=0.005)
    if not len(pk):
        pk = np.array([int(np.argmax(scores))])
    rows = [(float(lags[p]), float(scores[p]),
             align_score(times, float(lags[p])) if len(times) else 0.0) for p in pk]
    mixed = [(L, s + 0.05 * a, s) for L, s, a in rows]
    top = max(m for _, m, _ in mixed)
    choice = min((r for r in mixed if r[1] >= top - 0.05), key=lambda r: r[0])
    loop = choice[0]
    cand = np.arange(max(LOOP_MIN, loop - 0.02), min(hi, loop + 0.021), 0.001)
    best = (choice[2], loop)
    for L in cand:
        s = lag_sim(F, i0, i1, float(L), fps)
        if len(times):
            s += 0.05 * align_score(times, L)
        if s > best[0]:
            best = (s, float(L))
    return best[1], float(best[0])


def related(a, b, tol=0.12):
    """One loop is a small multiple of the other."""
    ratio = max(a, b) / min(a, b)
    whole = round(ratio)
    return whole >= 2 and abs(ratio - whole) <= 0.03 * whole


def loop_regions(F, fps, attacks, t0, t1, win=16.0, hop=4.0):
    """Stretches of a take that hold one loop.

    Windows are compared with the loop of the run they would join. A multiple
    of that loop is the same grid, so it does not make a new region; a stretch
    that keeps measuring a different grid does.
    """
    starts = np.arange(t0, t1 - win + 1e-9, hop)
    if len(starts) < 3:
        return [(t0, t1, None)]
    runs = []
    for s in starts:
        e = min(t1, s + win)
        cand, _ = best_loop(F, fps, s, e, attacks[(attacks >= s) & (attacks < e)])
        if not cand:
            continue
        base = float(np.median([x for x in runs[-1][2:]])) if runs else None
        if runs and (abs(cand / base - 1.0) <= 0.03 or related(cand, base)):
            runs[-1].append(cand)
        else:
            runs.append([s, e, cand])
    if len(runs) < 2:
        return [(t0, t1, float(np.median(runs[0][2:])) if runs else None)]
    # a run of fewer than three windows is noise: fold it into its neighbour
    changed = True
    while changed and len(runs) > 1:
        changed = False
        for i, run in enumerate(runs):
            if len(run) - 2 < 3:
                near = min((j for j in (i - 1, i + 1) if 0 <= j < len(runs)),
                           key=lambda j: abs(np.median(runs[j][2:]) /
                                             np.median(run[2:]) - 1.0))
                runs[near][1] = run[1]
                runs[near].extend(run[2:])
                runs.pop(i)
                changed = True
                break
    bounds = [t0] + [round((a[0] + b[0]) / 2.0, 3) for a, b in zip(runs, runs[1:])]
    bounds.append(t1)
    return [(bounds[i], bounds[i + 1], float(np.median(runs[i][2:]))) for i in range(len(runs))]


def refine_loop(F, fps, t0, t1, loop):
    """The measured length of a loop already known to hold over a stretch."""
    i0, i1 = int(t0 * fps), min(F.shape[1], int(t1 * fps))
    best = (lag_sim(F, i0, i1, loop, fps), loop)
    for L in np.arange(loop * 0.97, loop * 1.031, 0.002):
        s = lag_sim(F, i0, i1, float(L), fps)
        if s > best[0]:
            best = (s, float(L))
    return best[1], best[0]


def extract(F, fps, attacks, events, t0, t1, duration, depth=0):
    """Phrases in one stretch, split where its loop changes."""
    regions = loop_regions(F, fps, attacks, t0, t1) if t1 - t0 > 24.0 else []
    if len(regions) > 1 and depth < 3:
        out = []
        for r0, r1, _ in regions:
            out += extract(F, fps, attacks, events, r0, r1, duration, depth + 1)
        return out
    if regions and regions[0][2]:
        loop, sim = refine_loop(F, fps, t0, t1, regions[0][2])
    else:
        loop, sim = best_loop(F, fps, t0, t1,
                              attacks[(attacks >= t0) & (attacks < t1)])
    if not loop or sim < 0.1:
        return []
    if depth < 2 and t1 - t0 > 8.0 * loop:
        tail0 = t1 - max(4.0, 4.0 * loop)
        other, score = best_loop(F, fps, tail0, t1,
                                 attacks[(attacks >= tail0) & (attacks < t1)])
        if other and abs(other / loop - 1.0) > 0.02 and \
                score > lag_sim(F, int(tail0 * fps), int(t1 * fps), loop, fps) + 0.05:
            return extract(F, fps, attacks, events, t0, tail0, duration, depth + 1) + \
                   extract(F, fps, attacks, events, tail0, t1, duration, depth + 1)
    patterns = track_elements(events, t0, t1, loop)
    phrase = build_phrase(patterns, t0, (t0, t1), loop, duration, sim)
    return [phrase] if phrase else []


def cluster_offsets(events, tol):
    """One group per place in the segment."""
    order = sorted(range(len(events)), key=lambda i: events[i]['off'])
    groups, cur = [], []
    for i in order:
        if cur and events[i]['off'] - events[cur[-1]]['off'] > tol:
            groups.append(cur)
            cur = []
        cur.append(i)
    if cur:
        groups.append(cur)
    return groups


def split_pitches(events, group):
    """Split an offset group by pitch when pitch is what returns."""
    clusters = []
    for i in group:
        chroma = events[i]['chroma']
        for c in clusters:
            centre = np.mean([events[j]['chroma'] for j in c], axis=0)
            centre /= np.linalg.norm(centre) + 1e-12
            if float(chroma @ centre) >= CHROMA_MATCH:
                c.append(i)
                break
        else:
            clusters.append([i])
    return clusters if len(clusters) > 1 else [group]


def stride_runs(group, events, loop):
    """The element's own period and the segment runs it occupies."""
    segs = sorted({events[i]['seg'] for i in group})
    if len(segs) < MIN_SEGMENTS:
        return None
    for stride in (1, 2, 3, 4):
        runs, cur = [], [segs[0]]
        for v in segs[1:]:
            if v - cur[-1] <= 2 * stride:
                cur.append(v)
            else:
                runs.append(cur)
                cur = [v]
        runs.append(cur)
        good = []
        for run in runs:
            present = len(run)
            count = (run[-1] - run[0]) // stride + 1
            if present >= MIN_SEGMENTS and present / count >= 0.6:
                good.append((run[0], count))
        if good:
            return {'stride': stride, 'loop': round(loop * stride, 6), 'spans': good}
    return None


def track_elements(events, t0, t1, loop):
    """Patterns: one tracked element per place in the segment."""
    inside = [e for e in events if t0 <= e['t'] <= t1]
    if len(inside) < MIN_SEGMENTS:
        return []
    times = np.array([e['t'] for e in inside])
    anchors = []
    for k in range(int((t1 - t0) / loop) + 2):
        t = t0 + k * loop
        i = int(np.argmin(np.abs(times - t)))
        anchors.append(float(times[i]) if abs(times[i] - t) <= 0.3 * loop else t)
    anchors = np.array(anchors)
    for e in inside:
        k = int(np.argmin(np.abs(anchors - e['t'])))
        e['seg'] = k
        e['off'] = round(e['t'] - anchors[k], 4)
    patterns = []
    for group in cluster_offsets(inside, OFFSET_TOL * loop):
        for chunk in split_pitches(inside, group):
            found = stride_runs(chunk, inside, loop)
            if not found:
                continue
            labels = [inside[i]['label'] for i in chunk]
            pitches = [inside[i]['pitch'] for i in chunk]
            label = pitches[0] if len(set(pitches)) == 1 and pitches[0] else \
                max(set(labels), key=labels.count)
            patterns.append({
                'label': label,
                'offset': round(float(np.median([inside[i]['off'] for i in chunk])), 3),
                'loop': found['loop'],
                'stride': found['stride'],
                'spans': found['spans'],
            })
    return patterns


def build_phrase(patterns, origin, region, loop, duration, similarity):
    start = max(0.0, origin)
    if LEAD and patterns:
        first = min(origin + s * loop for p in patterns for s, _ in p['spans'])
        start = max(0.0, min(start, first - LEAD * loop))
    segments = int(np.floor((min(region[1], duration) - start) / loop + 1e-6))
    if segments < MIN_SEGMENTS:
        return None
    end = min(duration, start + segments * loop)
    segments = int((end - start) // loop)
    return {
        'start': round(start, 3),
        'end': round(end, 3),
        'loop': round(loop, 6),
        'segments': segments,
        'similarity': round(similarity, 3),
        'patterns': [{'label': p['label'], 'offset': p['offset'],
                      'loop': p['loop'],
                      'spans': [[round(origin + s * loop, 3),
                                 round(origin + (s + n * p['stride']) * loop, 3)]
                                for s, n in p['spans']]}
                     for p in patterns],
    }


def group_phrases(phrases, duration):
    """Merge phrases of one loop whose patterns share a segment."""
    out = []
    for ph in sorted(phrases, key=lambda p: p['start']):
        for cur in out:
            shared = min(cur['end'], ph['end']) - max(cur['start'], ph['start'])
            if abs(cur['loop'] / ph['loop'] - 1.0) <= 0.02 and \
                    shared >= min(cur['loop'], ph['loop']):
                cur['start'] = min(cur['start'], ph['start'])
                cur['end'] = max(cur['end'], ph['end'])
                cur['patterns'] += ph['patterns']
                break
        else:
            out.append(dict(ph))
    for ph in out:
        n = int((min(ph['end'], duration) - ph['start']) // ph['loop'])
        ph['segments'] = n
        ph['end'] = round(min(duration, ph['start'] + n * ph['loop']), 3)
    return [p for p in out if p['segments'] >= MIN_SEGMENTS]


def estimate_bar(spans, lo=1.2, hi=3.0):
    usable = [s for s in spans if s['end'] - s['start'] >= 3.0 * lo]
    best_err, best_bar = float('inf'), 2.0
    bar = lo
    while bar <= hi:
        err = sum(((s['end'] - s['start']) / bar -
                   round((s['end'] - s['start']) / bar)) ** 2 *
                  (s['end'] - s['start']) for s in usable)
        if err < best_err:
            best_err, best_bar = err, bar
        bar += 0.0002
    return best_bar


def analyze(path, args):
    sr, x = load_audio(path)
    duration = len(x) / sr
    freqs, P, F, fps = spectral(x, sr)
    attacks = detect_attacks(x, sr)
    events = event_features(freqs, P, fps, attacks)

    passes_path = passes.find_passes_file(path)
    spans = passes.pass_spans(passes.parse_passes(passes_path), duration)
    bar = passes.parse_bar(passes_path) or estimate_bar(spans)

    sections = detect_sections(x, sr, args.threshold_db,
                               args.min_silence, args.min_section)
    phrases = []
    for t0, t1 in sections:
        phrases += extract(F, fps, attacks, events, t0, t1, duration)
    phrases = group_phrases(phrases, duration)
    for i, ph in enumerate(phrases):
        ph['id'] = i
        ph['bars'] = round(ph['loop'] / bar, 2) if bar else None
        ph['pass'] = passes.find_pass(ph['start'], ph['end'], spans)['index']

    print('\n===== %s  %.2fs  %d Hz' % (os.path.basename(path), duration, sr))
    print('passes: %s  bar %.4f s  attacks %d'
          % (os.path.basename(passes_path), bar, len(attacks)))
    for ph in phrases:
        print('  phrase %d  %.2f-%.2f  loop %.3f s  %d segments  pass %d  sim %.2f'
              % (ph['id'] + 1, ph['start'], ph['end'], ph['loop'],
                 ph['segments'], ph['pass'], ph['similarity']))
        for p in ph['patterns']:
            print('      %-7s offset %5.3f  loop %.3f  spans %s' % (
                p['label'], p['offset'], p['loop'],
                ', '.join('%d' % round((b - a) / p['loop']) for a, b in p['spans'])))
    return {
        'file': os.path.basename(path),
        'passesFile': os.path.basename(passes_path),
        'sampleRate': int(sr),
        'duration': round(duration, 3),
        'bar': round(bar, 6),
        'phrases': phrases,
    }


def report_spec(results):
    """Phrases as the patterns the report and song builders consume."""
    reports = []
    for res in results:
        pats = []
        for i, ph in enumerate(res['phrases']):
            pats.append({
                'start': ph['start'],
                'end': ph['end'],
                'length': ph['loop'],
                'bars': max(1, int(round(ph['bars'] or 1))),
                'name': 'Phrase %d' % (i + 1),
                'color': COLORS[i % len(COLORS)],
            })
        report = {'file': res['file'], 'patterns': pats}
        if len(results) == 1:
            report['song'] = {'targets': [-20] * len(pats)}
        reports.append(report)
    return {
        'title': 'Analyze Palettes',
        'description': 'Phrases extracted from tracked elements and their loops.',
        'reports': reports,
    }


def main():
    ap = argparse.ArgumentParser(description='Extract phrases from palette audio.')
    ap.add_argument('files', nargs='+')
    ap.add_argument('--json', default=None, help='Write full results to this JSON file')
    ap.add_argument('--spec', default=None, help='Write a report spec from the phrases')
    ap.add_argument('--threshold-db', type=float, default=-55.0)
    ap.add_argument('--min-silence', type=float, default=2.0)
    ap.add_argument('--min-section', type=float, default=1.0)
    args = ap.parse_args()
    results = [analyze(p, args) for p in args.files]
    if args.json:
        with open(args.json, 'w') as f:
            json.dump(results, f, indent=1)
        print('\nWrote', args.json)
    if args.spec:
        with open(args.spec, 'w') as f:
            json.dump(report_spec(results), f, indent=1)
        print('Wrote', args.spec)


if __name__ == '__main__':
    main()
