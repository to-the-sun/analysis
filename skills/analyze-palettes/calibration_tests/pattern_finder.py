#!/usr/bin/env python3
"""Find palette patterns by decomposing every attack into harmonic series and percussion.

Each attack's spectrum is reduced to up to three harmonic series (a fundamental
tested against integer multiples) plus the broadband energy those series leave
behind, split into percussive parts at its deepest spectral valley. Components
are grouped into elements, each element's onsets are searched for a uniform
grid, and the grid's contiguous runs become patterns. Patterns whose runs recur
at a common interval become superpatterns; patterns sharing a full segment
become one phrase.

Usage:
    python3 pattern_finder.py [file.wav ...] [--json out.json] [--debug]
"""
import argparse
import json
import os

import numpy as np
import scipy.io.wavfile as wavfile
import scipy.signal as sig
from scipy.cluster.hierarchy import fcluster, linkage
from scipy.ndimage import maximum_filter1d
from scipy.spatial.distance import squareform

HERE = os.path.dirname(os.path.abspath(__file__))
HOP, NFFT = 256, 2048
ATTACK = 0.10              # window characterized for one attack
SPEC_NFFT = 16384
MIN_P, MAX_P = 0.5, 8.0    # pattern loop bounds
MIN_SEGMENTS = 3


# --- audio and frames -------------------------------------------------------

def load(path):
    sr, data = wavfile.read(path)
    x = data.astype(np.float64)
    if data.dtype == np.int16:
        x /= 32768.0
    elif data.dtype == np.int32:
        x /= 2147483648.0
    elif data.dtype == np.uint8:
        x = (x - 128.0) / 128.0
    if x.ndim > 1:
        x = x.mean(axis=1)
    return int(sr), x


def frames(x, sr):
    f, t, Z = sig.stft(x, fs=sr, nperseg=NFFT, noverlap=NFFT - HOP,
                       boundary=None, padded=False)
    power = np.abs(Z) ** 2
    return f, t, power


def spectrum(x, sr, t0, win=ATTACK):
    a = int(t0 * sr)
    seg = x[a:a + int(win * sr)]
    if len(seg) < 64:
        return None, None
    taper = np.hanning(len(seg))
    mag = np.abs(np.fft.rfft(seg * taper, SPEC_NFFT)) * 2.0 / taper.sum()
    return np.fft.rfftfreq(SPEC_NFFT, 1.0 / sr), mag


# --- attacks ----------------------------------------------------------------

def detect_attacks(x, sr, f, t, power):
    loud = np.log1p(200.0 * power / (power.max() + 1e-20))
    # a three-bin maximum across frequency keeps vibrato from firing (superflux)
    peak = maximum_filter1d(loud, 3, axis=0)
    flux = np.maximum(peak[:, 2:] - peak[:, :-2], 0).sum(0)
    flux = np.convolve(np.pad(flux, (2, 0)), [0.25, 0.5, 0.25], 'same')
    half = max(2, int(0.4 / (HOP / sr)))
    local = flux - np.array([np.median(flux[max(0, i - half):i + half])
                             for i in range(len(flux))])
    peaks, _ = sig.find_peaks(local, distance=int(0.05 * sr / HOP), prominence=2.0)
    return [float(t[p]) for p in peaks if p >= 2 and p < len(flux) - 2]


# --- harmonic decomposition -------------------------------------------------

def spectral_peaks(freqs, mag, floor_db=-52.0, prominence_db=9.0):
    db = 20 * np.log10(mag / (mag.max() + 1e-20) + 1e-20)
    band = (freqs >= 30) & (freqs <= min(14000, freqs[-1]))
    idx = np.flatnonzero(band)
    level = np.maximum(db[idx], floor_db)
    peaks, props = sig.find_peaks(level, prominence=prominence_db, distance=2)
    rows = []
    for i, p in enumerate(peaks):
        if p == 0 or p == len(level) - 1:
            continue
        y0, y1, y2 = level[p - 1], level[p], level[p + 1]
        den = y0 - 2 * y1 + y2
        off = 0.5 * (y0 - y2) / den if den else 0.0
        freq = freqs[idx[p]] + off * (freqs[1] - freqs[0])
        rows.append((float(freq), float(10 ** (y1 / 20))))
    return rows, db


def harmonic_series(peaks, max_series=3):
    """Take the strongest harmonic series off the peak list, up to max_series."""
    left = [[fr, amp, False] for fr, amp in peaks]
    series, taken = [], []
    for _ in range(max_series):
        live = [p for p in left if not p[2]]
        if not live:
            break
        cands = set()
        for fr, _, _ in live:
            for k in range(1, 6):
                f0 = fr / k
                if 30 <= f0 <= 2000:
                    cands.add(f0)
        size = max(p[1] for p in live) + 1e-20
        best = None
        for f0 in cands:
            if any(abs(f0 / t - k) < 0.04 for t in taken for k in (2, 3, 4, 5, 6)):
                continue                                  # another element's overtone
            tol = 0.03 * f0
            hits, used = [], set()
            for k in range(1, 15):
                want = k * f0
                pick = None
                for j, (fr, amp, taken_) in enumerate(left):
                    if taken_ or j in used or abs(fr - want) > max(tol * k, 3.0):
                        continue
                    if pick is None or amp > pick[2]:
                        pick = (j, fr, amp)
                if pick:
                    used.add(pick[0])
                    hits.append((k, pick[1], pick[2]))
            if len(hits) < 3 or not any(k <= 3 for k, _, _ in hits):
                continue
            # a missing fundamental is normal: the ladder below k=3 still counts
            explained = sum(a * (1.0 if k <= 8 else 0.6) for k, _, a in hits)
            score = explained / size * min(1.0, len(hits) / 6.0)
            if best is None or score > best[0]:
                best = (score, f0, hits)
        if best is None or best[0] < 0.45:
            break
        score, f0, hits = best
        taken.append(f0)
        for k, fr, _ in hits:
            for p in left:
                if not p[2] and abs(p[0] - fr) < 1e-9:
                    p[2] = True
        f0s = np.array([fr / k for k, fr, _ in hits])
        series.append({'f0': float(np.median(f0s)), 'score': score,
                       'hits': hits, 'amp': float(sum(a for _, _, a in hits))})
    return series, left


def band_energy(freqs, power, lo, hi):
    m = (freqs >= lo) & (freqs < hi)
    return float(power[m].sum())


def percussive_parts(freqs, power, floor=35.0, split=250.0, top=16000.0):
    """Split what the series left behind into a low and an upper register."""
    top = min(top, freqs[-1])
    parts = [(floor, top)]
    low, high = band_energy(freqs, power, floor, split), band_energy(freqs, power, split, top)
    if low >= 0.08 * (low + high) and high >= 0.08 * (low + high):
        parts = [(floor, split), (split, top)]
    out = []
    for lo, hi in parts:
        m = (freqs >= lo) & (freqs < hi)
        p = power[m]
        if p.sum() <= 0:
            continue
        fs = freqs[m]
        edges = np.geomspace(lo, hi, 13)
        bands = np.array([p[(fs >= a) & (fs < b)].sum() for a, b in zip(edges[:-1], edges[1:])])
        flat = float(np.exp(np.mean(np.log(bands / bands.sum() + 1e-12))) * len(bands))
        out.append({'lo': float(lo), 'hi': float(hi),
                    'centroid': float((p / p.sum() * fs).sum()),
                    'flatness': flat, 'energy': float(p.sum())})
    return out


def band_decay(f, t, power, lo, hi, t0, floor_ms=12, span=0.5):
    m = (f >= lo) & (f < hi)
    e = power[m].sum(0)
    step = t[1] - t[0]
    i0 = int(np.searchsorted(t, t0))
    window = e[i0:i0 + max(4, int(span / step))]
    if len(window) < 4 or window.max() <= 0:
        return span
    peak = int(np.argmax(window[:max(2, int(0.05 / step))]))
    target = window[peak] * 0.06
    after = np.flatnonzero(window[peak:] < target)
    return float(after[0] * step) if len(after) else float(len(window) * step)


def components(x, sr, f, t, power, attacks):
    out = []
    for t0 in attacks:
        freqs, mag = spectrum(x, sr, t0)
        if freqs is None or mag.max() <= 0:
            continue
        pw = mag ** 2
        peaks, _ = spectral_peaks(freqs, mag)
        if not peaks:
            continue
        series, left = harmonic_series(peaks)
        for s in series:
            amps = np.zeros(8)
            for k, _, a in s['hits']:
                if k <= 8:
                    amps[k - 1] += a
            amps /= (amps.max() + 1e-20)
            out.append({'kind': 'pitched', 't': t0, 'f0': s['f0'],
                        'timbre': amps, 'decay': band_decay(f, t, power, s['f0'] * 0.9, s['f0'] * 4.05, t0),
                        'amp': s['amp']})
        mask = np.ones(len(freqs), bool)
        for p in left:
            if p[2]:
                mask &= np.abs(freqs - p[0]) > max(0.03 * p[0], 6.0)
        resid = pw * mask
        total = pw.sum() + 1e-20
        if resid.sum() / total < 0.12:
            continue
        for part in percussive_parts(freqs, resid):
            out.append({'kind': 'percussive', 't': t0,
                        'centroid': part['centroid'], 'flatness': part['flatness'],
                        'lo': part['lo'], 'hi': part['hi'],
                        'decay': band_decay(f, t, power, part['lo'] * 0.9, part['hi'] * 1.1, t0),
                        'amp': np.sqrt(part['energy'])})
    return out


# --- elements ---------------------------------------------------------------

def distance(a, b):
    d = 0.0
    if a['kind'] != b['kind']:
        return 99.0
    if a['kind'] == 'pitched':
        d += 4.0 * abs(np.log2(a['f0'] / b['f0']))
        d += 0.35 * np.abs(a['timbre'] - b['timbre']).sum()
        d += 1.2 * abs(np.log((a['decay'] + 0.02) / (b['decay'] + 0.02)))
    else:
        d += 1.6 * abs(np.log2((a['centroid'] + 40) / (b['centroid'] + 40)))
        d += 1.0 * abs(np.log((a['decay'] + 0.02) / (b['decay'] + 0.02)))
        d += 2.5 * abs(a['flatness'] - b['flatness'])
    return d


def merge(comps, threshold):
    n = len(comps)
    if n < 2:
        return np.zeros(n, int)
    D = np.zeros((n, n))
    for i in range(n):
        for j in range(i + 1, n):
            D[i, j] = D[j, i] = distance(comps[i], comps[j])
    return fcluster(linkage(squareform(D, checks=False), 'single'), threshold, 'distance')


def elements(comps):
    if not comps:
        return []
    labels = merge(comps, 0.7)
    groups = {lab: np.flatnonzero(labels == lab) for lab in np.unique(labels)}
    # Series of one attack that always arrive together are one element: a
    # fundamental plus a strong overtone, not two instruments.
    base = {lab: lab for lab in groups}
    pitched = [lab for lab in groups if comps[groups[lab][0]]['kind'] == 'pitched']
    for i, a in enumerate(pitched):
        for b in pitched[i + 1:]:
            ta = {round(comps[k]['t'], 2) for k in groups[a]}
            tb = {round(comps[k]['t'], 2) for k in groups[b]}
            if len(ta & tb) / max(1, min(len(ta), len(tb))) >= 0.5:
                base[b] = base[a]
    for lab in groups:
        labels[groups[lab]] = base[lab]
    out = []
    for lab in np.unique(labels):
        idx = np.flatnonzero(labels == lab)
        rows = [comps[i] for i in idx]
        times = np.array(sorted({round(c['t'], 3) for c in rows}))
        e = {'kind': rows[0]['kind'], 'times': times,
             'count': len(times), 'amp': float(np.median([c['amp'] for c in rows]))}
        if e['kind'] == 'pitched':
            strong = [c for c in rows if c['amp'] >= 0.6 * max(r['amp'] for r in rows)]
            e['f0'] = float(np.median([c['f0'] for c in strong]))
            e['label'] = 'pitch %.1f Hz' % e['f0']
        else:
            e['centroid'] = float(np.median([c['centroid'] for c in rows]))
            e['decay'] = float(np.median([c['decay'] for c in rows]))
            e['label'] = 'percussion %.0f Hz' % e['centroid']
        out.append(e)
    return out


# --- grids ------------------------------------------------------------------

def chain(times, p, max_skip=64):
    """Longest sequence of onsets that lands in the same place each loop.

    A link i -> j needs t[j]-t[i] to be a whole number of loops, so a run of
    single-loop links is the element present in every segment of that run.
    """
    n = len(times)
    if n < MIN_SEGMENTS:
        return []
    tol = min(0.12 * p, 0.12)
    best = np.ones(n, int)
    prev = np.full(n, -1, int)
    skip = np.zeros(n, int)
    for j in range(1, n):
        for i in range(max(0, j - max_skip), j):
            d = times[j] - times[i]
            k = int(round(d / p))
            if k < 1 or abs(d - k * p) > tol:
                continue
            if best[i] + 1 > best[j]:
                best[j], prev[j], skip[j] = best[i] + 1, i, k
    if best.max() < MIN_SEGMENTS:
        return []
    out, j = [], int(np.argmax(best))
    while j >= 0:
        out.append((j, skip[j]))
        j = prev[j]
    return out[::-1]


def runs_from(chain_, times, p):
    """Split the chain where it skipped a loop; keep runs of >=3 segments."""
    runs, cur = [], [chain_[0]]
    for step in chain_[1:]:
        if step[1] == 1:
            cur.append(step)
        else:
            runs.append(cur)
            cur = [step]
    runs.append(cur)
    out = []
    for run in runs:
        base = np.array([times[i] for i, _ in run])
        if len(base) < MIN_SEGMENTS:
            continue
        for _ in range(3):
            n = np.arange(len(base), dtype=float)
            (a, loop), *_ = np.linalg.lstsq(np.vstack([np.ones(len(n)), n]).T, base, rcond=None)
            if not MIN_P / 2 <= loop <= 2 * MAX_P:
                break
            # a missed detection must not cut a run: fill the next loop slot
            for step in (1, -1):
                while True:
                    want = (base[-1] if step == 1 else base[0]) + step * loop
                    near = times[np.abs(times - want) <= 0.12 * loop]
                    if not len(near):
                        break
                    base = np.sort(np.append(base, near[0] if step == 1 else near[-1]))
        n = np.arange(len(base), dtype=float)
        (a, loop), *_ = np.linalg.lstsq(np.vstack([np.ones(len(n)), n]).T, base, rcond=None)
        resid = base - (a + n * loop)
        out.append({'loop': float(loop), 'start': float(a),
                    'end': float(base[-1] + loop), 'segments': int(len(base)),
                    'times': base, 'resid': float(np.mean(np.abs(resid)) / loop)})
    return out


def patterns_for(times, min_p=MIN_P, max_p=MAX_P, tol_loop=0.05):
    """Every uniform grid an element's onsets support, one onset per pattern."""
    times = np.sort(np.unique(np.round(times, 4)))
    if len(times) < MIN_SEGMENTS:
        return []
    cands = []
    for i in range(len(times)):
        for j in range(i + 1, min(len(times), i + 10)):
            d = times[j] - times[i]
            for k in (1, 2, 4, 8):
                p = d / k
                if min_p <= p <= max_p:
                    cands.append(p)
    keep = []
    for p in sorted(cands):
        if not keep or p > keep[-1] * 1.02:
            keep.append(p)
    found = []
    for p in keep:
        ch = chain(times, p)
        if not ch:
            continue
        for r in runs_from(ch, times, p):
            if abs(r['loop'] / p - 1) <= 0.08:
                found.append(r)
    found.sort(key=lambda r: (-r['segments'], r['resid']))
    kept, used = [], set()
    for r in found:
        events = {round(v, 3) for v in r['times']}
        if len(events & used) > 0.2 * len(events):
            continue                             # these onsets already belong to a pattern
        if any(abs(r['loop'] / k['loop'] - 1) < tol_loop and
               len(events & {round(v, 3) for v in k['times']}) > 0.5 * len(events)
               for k in kept):
            continue
        kept.append(r)
        used |= events
    return kept


def activation(e, f, t, power, width=0.04, harmonics=8):
    """The element's harmonic energy through time, so a missed attack cannot
    break its grid."""
    f0 = e['f0']
    mask = np.zeros(len(f), bool)
    for k in range(1, harmonics + 1):
        if k * f0 > f[-1]:
            break
        mask |= (f >= k * f0 * (1 - width)) & (f <= k * f0 * (1 + width))
    if not mask.any():
        return None
    a = power[mask].sum(0)
    return np.convolve(a, np.ones(5) / 5, 'same')


def activation_patterns(e, f, t, power):
    """Loop and runs measured from how often the element itself returns."""
    a = activation(e, f, t, power)
    if a is None or a.max() <= 0 or len(e['times']) < MIN_SEGMENTS:
        return []
    fs = 1.0 / (t[1] - t[0])
    # outside the span its attacks cover, the element is not being played
    lo = int(max(0, (e['times'].min() - 0.15) * fs))
    hi = min(len(a), int((e['times'].max() + 0.6) * fs))
    a = np.where((np.arange(len(a)) >= lo) & (np.arange(len(a)) < hi), a, 0.0)
    if a.max() <= 0:
        return []
    a = a / np.percentile(a[a > 0], 95)
    ac = np.correlate(a - a.mean(), a - a.mean(), 'full')[len(a) - 1:]
    ac /= (ac[0] + 1e-20)
    lo, hi = int(MIN_P * fs), min(int(MAX_P * fs), len(ac) - 2)
    if hi - lo < 4:
        return []
    peaks, _ = sig.find_peaks(ac[lo:hi], height=0.3)
    if not len(peaks):
        return []
    best = ac[lo + peaks].max()
    ok = [p for p in peaks if ac[lo + p] >= 0.88 * best]
    lag = lo + int(ok[0])                       # the shortest loop that repeats
    y0, y1, y2 = ac[lag - 1], ac[lag], ac[lag + 1]
    den = y0 - 2 * y1 + y2
    lag = lag + (0.5 * (y0 - y2) / den if den else 0.0)
    loop = lag / fs
    n = int(len(a) / lag)
    grid = np.arange(n) * lag
    # the phase that puts the most segments on the element
    shifts = np.arange(int(lag))
    fold = np.array([sum(a[int(g + s)] for g in grid if int(g + s) < len(a)) for s in shifts])
    off = int(np.argmax(fold)) / fs
    level = np.array([a[min(len(a) - 1, int(round(off * fs + k * lag)))] for k in range(n)])
    thr = 0.35 * np.percentile(level, 90)
    present = level >= thr
    runs, start = [], None
    for k, keep in enumerate(present):
        if keep and start is None:
            start = k
        elif not keep and start is not None:
            runs.append((start, k - 1))
            start = None
    if start is not None:
        runs.append((start, n - 1))
    out = []
    for k0, k1 in runs:
        if k1 - k0 + 1 < MIN_SEGMENTS:
            continue
        out.append({'loop': float(loop), 'start': float(off + k0 * loop),
                    'end': float(off + (k1 + 1) * loop),
                    'segments': int(k1 - k0 + 1),
                    'times': np.array([off + k * loop for k in range(k0, k1 + 1)]),
                    'resid': 0.0})
    return out


def group_rows(patterns, element):
    """One row per element and phase, its non-contiguous runs as spans."""
    rows = []
    for p in sorted(patterns, key=lambda r: r['start']):
        for row in rows:
            phase = (p['start'] - row['start']) % p['loop']
            if abs(row['loop'] / p['loop'] - 1) < 0.06 and \
                    min(phase, p['loop'] - phase) < 0.15 * p['loop']:
                row['spans'].append(p)
                break
        else:
            rows.append({'loop': p['loop'], 'start': p['start'],
                         'spans': [p], 'element': element})
    out = []
    for row in rows:
        spans = sorted(row['spans'], key=lambda r: r['start'])
        start = max(0.0, row['start'])
        total = sum(s['segments'] for s in spans)
        out.append({'element': row['element'], 'loop': row['loop'],
                    'spans': [(s['start'], s['segments']) for s in spans],
                    'start': start, 'end': start + total * row['loop'],
                    'segments': total})
    return out


# --- superpatterns and phrases ---------------------------------------------

def superpatterns(patterns):
    out = []
    for row in patterns:
        spans = row['spans']
        if len(spans) < MIN_SEGMENTS:
            continue
        starts = np.array([s for s, _ in spans])
        steps = np.diff(starts)
        unit = row['loop'] * max(1, int(round(np.median(steps) / row['loop'])))
        if unit < 2 * row['loop']:
            continue
        fitted = [spans[0]]
        for (s, c), step in zip(spans[1:], steps):
            if abs(step - unit) <= 0.2 * unit:
                fitted.append((s, c))
        if len(fitted) < MIN_SEGMENTS:
            continue
        out.append({'element': row['element'], 'loop': float(unit),
                    'start': fitted[0][0], 'segments': len(fitted),
                    'end': fitted[0][0] + fitted[0][1] * row['loop']
                    + (len(fitted) - 1) * unit,
                    'of': row['spans']})
    return out


def dedupe(patterns):
    """One row per heard pattern: weaker read-outs of the same sound fold in."""
    out = []
    for p in sorted(patterns, key=lambda r: -r['amplitude']):
        same = False
        for q in out:
            if p['kind'] != q['kind'] or abs(np.log2((p['pitch'] + 40) / (q['pitch'] + 40))) > 0.8:
                continue                    # a different register is a different element
            if abs(p['loop'] / q['loop'] - 1) > 0.05 or abs(p['start'] - q['start']) > 0.4 * p['loop']:
                continue
            if [c for _, c in p['spans']] != [c for _, c in q['spans']]:
                continue
            same = True
            break
        if not same:
            out.append(p)
    return out


def phrases(patterns):
    spans = sorted([(p['start'], p['end'], p['loop']) for p in patterns])
    out = []
    for s, e, loop in spans:
        if out and s < out[-1][1] - min(loop, out[-1][2]) * 0.999 + 1e-9:
            out[-1][1] = max(out[-1][1], e)
            out[-1][2] = min(out[-1][2], loop)
        else:
            out.append([s, e, loop])
    return [{'start': s, 'end': e} for s, e, _ in out]


# --- sustained elements -----------------------------------------------------

def sustain_track(x, sr, f, t, power):
    """Dominant pitch every quarter second, then the runs where it holds."""
    step = 0.25
    track = []
    for t0 in np.arange(0, t[-1] - 0.6, step):
        freqs, mag = spectrum(x, sr, t0, 0.30)
        if freqs is None:
            continue
        peaks, _ = spectral_peaks(freqs, mag)
        series, _ = harmonic_series(peaks, 1)
        track.append((float(t0), series[0]['f0'] if series else 0.0))
    runs, cur = [], None
    for t0, f0 in track:
        if f0 and cur and abs(np.log2(f0 / cur['f0'])) < 0.02:
            cur['end'] = t0
            cur['vals'].append(f0)
        else:
            cur = {'start': t0, 'end': t0, 'f0': f0, 'vals': [f0]} if f0 else None
            if cur:
                runs.append(cur)
    return [r for r in runs if r['end'] - r['start'] >= 1.5]


def sustained(x, sr, f, t, power, known):
    """Elements without an attack: a held pitch whose envelope repeats."""
    fs_ = 1.0 / (t[1] - t[0])
    out = []
    for run in sustain_track(x, sr, f, t, power):
        f0 = float(np.median(run['vals']))
        m = (f >= f0 * 0.97) & (f < f0 * 1.03)
        if not m.any():
            continue
        env = np.sqrt(power[m].sum(0)[int(run['start'] * fs_):int(run['end'] * fs_)])
        if len(env) < 8 or env.mean() <= 0 or env.std() / env.mean() < 0.18:
            continue
        e = env - env.mean()
        ac = np.correlate(e, e, 'full')[len(e) - 1:]
        ac /= (ac[0] + 1e-20)
        lo_lag, hi_lag = int(MIN_P * fs_), min(int(MAX_P * fs_), len(ac) - 1)
        if hi_lag - lo_lag < 4:
            continue
        lag = lo_lag + int(np.argmax(ac[lo_lag:hi_lag]))
        period = lag / fs_
        segs = int((len(env) / fs_) // period)
        if ac[lag] < 0.35 or segs < MIN_SEGMENTS:
            continue
        if any(abs(np.log2(f0 / k['f0'])) < 0.06 and k['start'] - 1.0 <= run['start'] <= k['end'] + 1.0
               for k in known):
            continue
        out.append({'kind': 'sustained', 'f0': f0, 'label': 'pitch %.1f Hz' % f0,
                    'loop': float(period), 'start': float(run['start']),
                    'end': float(run['start'] + segs * period), 'segments': segs})
    return out


# --- analysis ---------------------------------------------------------------

def analyse(path):
    sr, x = load(path)
    f, t, power = frames(x, sr)
    attacks = detect_attacks(x, sr, f, t, power)
    comps = components(x, sr, f, t, power, attacks)
    els = elements(comps)

    patterns = []
    for e in els:
        runs = (activation_patterns(e, f, t, power) if e['kind'] == 'pitched'
                else patterns_for(e['times']))
        ioi = np.median(np.diff(np.unique(np.round(e['times'], 3)))) if len(e['times']) > 1 else 0.0
        runs = [r for r in runs if r['loop'] >= 0.9 * ioi]
        for row in group_rows(runs, e):
            row['element'] = e['label']
            row['kind'] = e['kind']
            row['pitch'] = e.get('f0', e.get('centroid', 0.0))
            row['amplitude'] = e['amp']
            patterns.append(row)

    known = [{'f0': e['f0'], 'start': min(p['start'] for p in patterns if p['element'] == e['label']),
              'end': max(p['end'] for p in patterns if p['element'] == e['label'])}
             for e in els if e['kind'] == 'pitched'
             and any(p['element'] == e['label'] for p in patterns)]
    sustains = sustained(x, sr, f, t, power, known)
    for s in sustains:
        patterns.append({'element': s['label'], 'loop': s['loop'],
                         'spans': [(s['start'], s['segments'])], 'start': s['start'],
                         'end': s['end'], 'segments': s['segments'],
                         'amplitude': 0.0})

    patterns = dedupe(patterns)
    patterns.sort(key=lambda p: p['start'])
    supers = superpatterns(patterns)
    return {'file': os.path.basename(path), 'duration': len(x) / sr, 'sampleRate': sr,
            'attacks': len(attacks), 'elements': els, 'patterns': patterns,
            'sustains': sustains, 'superpatterns': supers,
            'phrases': phrases(patterns)}


# --- calibration report -----------------------------------------------------

KEYS = {
    'test_audio_1.wav': {
        'rows': [('bass', 1.07, [(0, 73)], [(0.00, 73)]),
                 ('cymbal', 2.15, [(0, 35)], [(2.15, 35)]),
                 ('kick', 2.15, [(0, 35)], [(2.15, 35)]),
                 ('kick', 2.15, [(0, 6), (17, 5)], [(2.15, 6), (38.65, 5)])],
        'superpatterns': [],
        'phrases': [(0.0, 78.11)],
    },
    'test_audio_2.wav': {
        'rows': [('D4', 1.34, [(0, 4), (8, 4), (16, 4)], [(0.40, 4), (11.21, 4), (21.81, 4)]),
                 ('D4', 1.58, [(0, 4)], [(32.78, 4)]),
                 ('E4', 1.34, [(0, 4), (8, 4)], [(5.80, 4), (16.53, 4)]),
                 ('G/D3', 1.34, [(0, 4)], [(27.35, 4)]),
                 ('F#3', 1.34, [(0, 16)], [(0.40, 16)])],
        'superpatterns': [('D4', 10.77, 3, 0.40, 32.71)],
        'phrases': [(0.40, 32.71), (32.78, 39.10)],
    },
    'test_audio_3.wav': {
        'rows': [('D#2', 3.01, [(0, 5)], [(0.19, 5)]),
                 ('G#1', 3.01, [(0, 5)], [(0.19, 5)]),
                 ('C#2', 3.01, [(0, 5)], [(0.19, 5)]),
                 ('A#1', 3.01, [(0, 5)], [(0.19, 5)]),
                 ('F#2', 3.18, [(0, 16)], [(15.43, 16)])],
        'superpatterns': [],
        'phrases': [(0.19, 15.25), (15.43, 66.31)],
    },
}


def verdict(measured, loop, segs, spans):
    """Compare one key row with the closest measured row of a matching loop."""
    near = [m for m in measured if abs(m['loop'] / loop - 1) <= 0.06]
    if not near:
        return None, 'no row with this loop'
    m = min(near, key=lambda r: abs(r['loop'] - loop) + 0.5 * abs(r['segments'] - sum(c for _, c in segs)))
    problems = []
    if abs(m['loop'] - loop) > 0.03:
        problems.append('loop off %.2f' % (m['loop'] - loop))
    if len(m['spans']) != len(segs):
        problems.append('%d runs not %d' % (len(m['spans']), len(segs)))
    if abs(m['segments'] - sum(c for _, c in segs)) > 1:
        problems.append('%d segments not %d' % (m['segments'], sum(c for _, c in segs)))
    elif [c for _, c in m['spans']] != [c for _, c in segs]:
        problems.append('runs %s' % '+'.join(str(c) for _, c in m['spans']))
    if spans and abs(m['start'] - spans[0][0]) > 0.9 * loop:
        problems.append('start %.2f' % (m['start'] - spans[0][0]))
    return m, 'exact' if not problems else ', '.join(problems)


def report(res):
    key = KEYS.get(res['file'])
    lines = ['=== %s  %.2f s  %d Hz  %d attacks' % (res['file'], res['duration'],
                                                    res['sampleRate'], res['attacks'])]
    lines.append('measured patterns (label is arbitrary; pitch/instrument identity only)')
    lines.append('%-18s %7s %9s  %s' % ('element', 'loop', 'segments', 'region'))
    for p in res['patterns']:
        spans = ', '.join('%.2f' % s for s, _ in p['spans'])
        segs = ', '.join(str(c) for _, c in p['spans'])
        lines.append('%-18s %7.2f %9s  %.2f-%.2f  spans at %s'
                     % (p['element'], p['loop'], segs, p['start'], p['end'], spans))
    for s in res['superpatterns']:
        lines.append('%-18s %7.2f %9d  %.2f-%.2f  superpattern'
                     % (s['element'] + ' super', s['loop'], s['segments'], s['start'], s['end']))
    lines.append('phrases: ' + ', '.join('%.2f-%.2f' % (p['start'], p['end'])
                                         for p in res['phrases']))
    lines.append('')
    if key:
        lines.append('against the calibration key')
        exact = 0
        for label, loop, segs, spans in key['rows']:
            m, note = verdict(res['patterns'], loop, segs, spans)
            exact += note == 'exact'
            want = '%5.2f %s  %.2f-%s' % (loop, '+'.join(str(c) for _, c in segs),
                                         spans[0][0],
                                         '%.2f' % (spans[0][0] + sum(c for _, c in segs) * loop))
            got = ('%5.2f %s  %.2f-%.2f' % (m['loop'], '+'.join(str(c) for _, c in m['spans']),
                                            m['start'], m['end'])) if m else '-' * 22
            lines.append('  %-6s want %s  got %s  %s' % (label, want, got, note))
        lines.append('  %d of %d rows exact' % (exact, len(key['rows'])))
        for label, loop, segs, start, end in key['superpatterns']:
            hit = [s for s in res['superpatterns']
                   if abs(s['loop'] / loop - 1) < 0.06 and abs(s['segments'] - segs) <= 1]
            if hit:
                s = hit[0]
                lines.append('  %-6s want %5.2f %d  %.2f-%.2f  got %5.2f %d  %.2f-%.2f  exact'
                             % (label + ' super', loop, segs, start, end,
                                s['loop'], s['segments'], s['start'], s['end']))
            else:
                lines.append('  %-6s want %5.2f %d  %.2f-%.2f  no superpattern'
                             % (label + ' super', loop, segs, start, end))
        got = ', '.join('%.2f-%.2f' % (p['start'], p['end']) for p in res['phrases'])
        want = ', '.join('%.2f-%.2f' % (a, b) for a, b in key['phrases'])
        lines.append('  phrases want %s  got %s' % (want, got))
    return '\n'.join(lines)


def main():
    ap = argparse.ArgumentParser(description='Find palette patterns from attack spectra.')
    ap.add_argument('files', nargs='*', default=sorted(
        os.path.join(HERE, n) for n in os.listdir(HERE) if n.endswith('.wav')))
    ap.add_argument('--json', default=None)
    ap.add_argument('--debug', action='store_true')
    args = ap.parse_args()
    results = [analyse(p) for p in args.files]
    for res in results:
        if args.debug:
            for e in res['elements']:
                print('  element %-20s %-9s n=%3d  %s' % (
                    e['label'], e['kind'], e['count'],
                    ' '.join('%.2f' % v for v in e['times'][:24])))
            for s in res['sustains']:
                print('  element %-20s sustained n=%3d  %.2f-%.2f' % (
                    s['label'], s['segments'], s['start'], s['end']))
        print(report(res))
        print()
    if args.json:
        slim = []
        for r in results:
            r = dict(r)
            r['elements'] = [{k: v for k, v in e.items() if k != 'times'} for e in r['elements']]
            r['patterns'] = [{k: (list(v) if k == 'spans' else v)
                              for k, v in p.items() if k != 'times'} for p in r['patterns']]
            r['superpatterns'] = [{k: v for k, v in s.items() if k != 'of'}
                                  for s in r['superpatterns']]
            slim.append(r)
        with open(args.json, 'w') as fh:
            json.dump(slim, fh, indent=1, default=float)
        print('Wrote', args.json)


if __name__ == '__main__':
    main()
