#!/usr/bin/env python3
"""Build an interactive report scoring each stem's transience overlap with all the others."""

import argparse
import base64
import html
import json
import os
import re
import shutil
from urllib.parse import quote

import numpy as np
from scipy.io import wavfile
from scipy.ndimage import maximum_filter1d

ENV_RATE = 400.0
PEAK_SECONDS = 0.012
CHUNK_BINS = 65536
COARSE_STEP = 8192
LEVELS = [1, 8, 64, 512, 4096]
DATA_DIR = '[data]'
DATA_URL = '%5Bdata%5D'
REPORT_NAME = 'stem_correlation_report.html'


def fmt(t):
    m = int(t // 60)
    s = t - m * 60
    return f'{m}:{s:05.2f}'


def natural(name):
    return [int(p) if p.isdigit() else p.lower() for p in re.split(r'(\d+)', name)]


def b64_i16(arr):
    return base64.b64encode(np.ascontiguousarray(arr, dtype='<i2').tobytes()).decode('ascii')


def read_mono(path):
    sr, data = wavfile.read(path)
    channels = 1 if data.ndim == 1 else data.shape[1]
    if data.ndim > 1:
        data = data.mean(axis=1)
    if data.dtype == np.int16:
        x = data / 32768.0
    elif data.dtype == np.int32:
        x = data / 2147483648.0
    elif data.dtype == np.uint8:
        x = (data - 128.0) / 128.0
    else:
        x = data.astype(np.float64)
    return sr, channels, x.astype(np.float32)


def transience(x, sr):
    """Positive derivative of the peak-amplitude envelope, sampled at ENV_RATE."""
    if not len(x):
        return np.zeros(0, np.float32), 0.0
    win = max(1, int(round(sr * PEAK_SECONDS)))
    hop = max(1, int(round(sr / ENV_RATE)))
    env = maximum_filter1d(np.abs(x), size=win, mode='nearest')[::hop]
    return np.maximum(np.diff(env, prepend=env[0]), 0.0).astype(np.float32), hop / sr


def resample_env(env, step_seconds, frames, offset=0.0):
    if not len(env):
        return np.zeros(frames, np.float32)
    src = np.arange(len(env)) * step_seconds + offset
    return np.interp(np.arange(frames) / ENV_RATE, src, env, left=0.0, right=0.0).astype(np.float32)


def read_leads(stems_dir):
    path = os.path.join(os.path.dirname(stems_dir), 'song.json')
    if not os.path.isfile(path):
        return {}, path
    with open(path) as f:
        raw = json.load(f).get('stems') or {}
    leads = {}
    for name, value in raw.items():
        try:
            leads[os.path.basename(name).lower()] = max(0.0, float(value))
        except (TypeError, ValueError):
            pass
    return leads, path


def minmax_bins(pcm, step):
    if step <= 1:
        out = np.empty(len(pcm) * 2, dtype='<i2')
        out[0::2] = pcm
        out[1::2] = pcm
        return out, len(pcm)
    n = len(pcm)
    nb = (n + step - 1) // step
    full = n // step
    out = np.empty(nb * 2, dtype='<i2')
    if full:
        blocks = pcm[:full * step].reshape(full, step)
        out[0:full * 2:2] = blocks.min(axis=1)
        out[1:full * 2:2] = blocks.max(axis=1)
    if full < nb:
        tail = pcm[full * step:]
        out[full * 2] = tail.min()
        out[full * 2 + 1] = tail.max()
    return out, nb


def write_level(pcm, step, stem_dir, stem_id):
    inter, bins = minmax_bins(pcm, step)
    chunks = max(1, (bins + CHUNK_BINS - 1) // CHUNK_BINS)
    for c in range(chunks):
        block = inter[c * CHUNK_BINS * 2:(c + 1) * CHUNK_BINS * 2]
        with open(os.path.join(stem_dir, f'l{step}_{c}.js'), 'w') as f:
            f.write('window.__stemChunkArrive(%d,%d,%d,"%s");\n' % (stem_id, step, c, b64_i16(block)))
    return bins


def score_color(score):
    if score is None:
        return '#a0a0b0'
    return '#51cf66' if score >= 0.5 else '#ffa94d' if score >= 0.25 else '#4dabf7'


def card(stem, i):
    pct = 'no transients' if stem['score'] is None else f"{stem['score'] * 100:.1f}%"
    lead = f" &middot; lead-in {stem['lead']:.2f} s" if stem['lead'] else ''
    meta = (f"{fmt(stem['duration'])} &middot; {stem['sampleRate']} Hz &middot; "
            f"{'mono' if stem['channels'] == 1 else str(stem['channels']) + ' ch'}{lead}")
    return (
        f'<section class="card">'
        f'<div class="head"><span class="name">{html.escape(stem["name"])}</span>'
        f'<span class="meta">{meta}</span>'
        f'<span class="score" style="--sc:{score_color(stem["score"])}">{pct}</span>'
        f'<span class="view" id="view{i}"></span></div>'
        f'<div class="wrap"><canvas id="canvas{i}"></canvas>'
        f'<div class="tooltip" id="tip{i}"></div></div>'
        f'<div class="toolbar"><audio id="audio{i}" controls preload="metadata" src="{html.escape(stem["audio"])}"></audio>'
        f'<button class="btn" id="reset{i}">Reset Zoom</button></div></section>'
    )


JS = r"""
const META = window.STEM_REPORT;
const STEMS = META.stems;
const RATE = META.envRate;
const CHUNK_BINS = META.chunkBins;
const CACHE_BUDGET = 192 * 1024 * 1024;
const MIN_SPAN = 0.002;
const cache = new Map();
const pending = new Set();
const queued = new Set();
const rangeOut = { mn: 0, mx: 0 };
let cacheBytes = 0;

function b64ToI16(b64) {
  if (!b64) return new Int16Array(0);
  const bin = atob(b64);
  const bytes = new Uint8Array(bin.length);
  for (let i = 0; i < bin.length; i++) bytes[i] = bin.charCodeAt(i);
  return new Int16Array(bytes.buffer, 0, bin.length >> 1);
}

function fmt(t) {
  t = Math.max(0, t);
  const m = Math.floor(t / 60), s = t - m * 60;
  return m + ':' + (s < 10 ? '0' : '') + s.toFixed(2);
}

const TOTAL = b64ToI16(META.total);
const MAX_TOTAL = TOTAL.reduce((m, v) => Math.max(m, v), 1);

for (let i = 0; i < STEMS.length; i++) {
  const s = STEMS[i];
  s.id = i;
  s.own = b64ToI16(s.ownB64);
  s.coarse = b64ToI16(s.coarseB64);
  s.others = new Float32Array(TOTAL.length);
  for (let k = 0; k < TOTAL.length; k++) {
    s.others[k] = Math.max(0, TOTAL[k] - (k < s.own.length ? s.own[k] : 0));
  }
  let m = 1;
  for (let k = 0; k < TOTAL.length; k++) {
    m = Math.max(m, TOTAL[k], s.own[k] || 0);
  }
  s.scale = Math.min(m, MAX_TOTAL) || 1;
  s.view0 = 0;
  s.view1 = META.songDuration;
  s.hist = [];
  s.canvas = document.getElementById('canvas' + i);
  s.ctx = s.canvas.getContext('2d');
  s.wrap = s.canvas.parentElement;
  s.tip = document.getElementById('tip' + i);
  s.audio = document.getElementById('audio' + i);
  s.viewEl = document.getElementById('view' + i);
  s.raf = 0;
  attach(s);
  render(s);
}

function evict() {
  while (cacheBytes > CACHE_BUDGET) {
    const first = cache.entries().next();
    if (first.done) return;
    cacheBytes -= first.value[1].byteLength;
    cache.delete(first.value[0]);
  }
}

window.__stemChunkArrive = function (stemId, step, chunk, encoded) {
  const key = stemId + ':' + step + ':' + chunk;
  pending.delete(key);
  if (!cache.has(key)) {
    const arr = b64ToI16(encoded);
    cacheBytes += arr.byteLength;
    cache.set(key, arr);
    evict();
  }
  schedule(STEMS[stemId]);
};

function requestChunk(st, li, c, attempt) {
  const step = st.levels[li].step;
  const key = st.id + ':' + step + ':' + c;
  if (cache.has(key) || pending.has(key)) return;
  pending.add(key);
  const el = document.createElement('script');
  el.src = META.dataDir + '/' + st.id + '/l' + step + '_' + c + '.js';
  el.onload = () => el.remove();
  el.onerror = () => {
    pending.delete(key);
    if ((attempt || 0) < 2) setTimeout(() => requestChunk(st, li, c, (attempt || 0) + 1), 200);
  };
  document.head.appendChild(el);
}

function chunksForRange(st, li, s1, s2) {
  const L = st.levels[li];
  const last = Math.max(1, Math.ceil(L.bins / CHUNK_BINS)) - 1;
  let c1 = Math.floor(Math.floor(s1 / L.step) / CHUNK_BINS);
  let c2 = Math.floor(Math.floor(s2 / L.step) / CHUNK_BINS);
  c1 = Math.max(0, Math.min(last, c1));
  c2 = Math.max(0, Math.min(last, c2));
  const out = [];
  for (let c = c1; c <= c2; c++) out.push(c);
  return out;
}

function resident(st, li, s1, s2) {
  const step = st.levels[li].step;
  for (const c of chunksForRange(st, li, s1, s2)) {
    if (!cache.has(st.id + ':' + step + ':' + c)) return false;
  }
  return true;
}

function pickLevel(st, visible, width) {
  const maxBins = Math.max(1, width * 4);
  for (let i = 0; i < st.levels.length; i++) {
    if (visible / st.levels[i].step <= maxBins) return i;
  }
  return -1;
}

function usableLevel(st, s1, s2, width) {
  const want = pickLevel(st, s2 - s1 + 1, width);
  if (want < 0) return -1;
  for (let i = want; i < st.levels.length; i++) {
    if (resident(st, i, s1, s2)) return i;
  }
  return -1;
}

function requestViewLevel(st, s1, s2, width) {
  const want = pickLevel(st, s2 - s1 + 1, width);
  if (want < 0) return;
  for (const c of chunksForRange(st, want, s1, s2)) requestChunk(st, want, c);
}

function streamRange(st, li, a, b) {
  let step, bins, arr;
  if (li < 0) {
    step = META.coarseStep;
    bins = st.coarseBins;
    arr = st.coarse;
  } else {
    step = st.levels[li].step;
    bins = st.levels[li].bins;
    arr = null;
  }
  let b1 = Math.max(0, Math.floor(a / step));
  let b2 = Math.min(bins - 1, Math.floor(b / step));
  if (b2 < b1) return false;
  let ok = false;
  for (let k = b1; k <= b2; k++) {
    let lo, hi;
    if (li < 0) {
      const loc = k * 2;
      if (loc + 1 >= arr.length) continue;
      lo = arr[loc] / 32767;
      hi = arr[loc + 1] / 32767;
    } else {
      const c = Math.floor(k / CHUNK_BINS);
      const chunk = cache.get(st.id + ':' + step + ':' + c);
      if (!chunk) return false;
      const loc = (k - c * CHUNK_BINS) * 2;
      if (loc + 1 >= chunk.length) continue;
      lo = chunk[loc] / 32767;
      hi = chunk[loc + 1] / 32767;
    }
    if (!ok) { rangeOut.mn = lo; rangeOut.mx = hi; ok = true; }
    else { if (lo < rangeOut.mn) rangeOut.mn = lo; if (hi > rangeOut.mx) rangeOut.mx = hi; }
  }
  return ok;
}

function sampleRange(st) {
  const sr = st.sampleRate, n = st.totalSamples;
  const a = Math.max(0, Math.floor((st.view0 - st.lead) * sr));
  const b = Math.min(n - 1, Math.ceil((st.view1 - st.lead) * sr));
  return [a, Math.max(a, b)];
}

function xToTime(st, x, w) {
  return st.view0 + (x / w) * (st.view1 - st.view0);
}

function envPoints(pick, len, t0, t1, w, mapY, off) {
  const pts = [];
  if (!len || t1 <= t0) return pts;
  const span = t1 - t0;
  const f0 = Math.max(0, Math.floor((t0 - off) * RATE));
  const f1 = Math.min(len - 1, Math.ceil((t1 - off) * RATE));
  if (f1 < f0) return pts;
  const xOf = k => ((k / RATE + off) - t0) / span * w;
  const ppf = (f1 - f0) / Math.max(1, w);
  if (ppf >= 2) {
    for (let px = 0; px <= w; px++) {
      const a = Math.max(f0, Math.floor((t0 - off + (px / w) * span) * RATE));
      if (a > f1) break;
      const b = Math.min(f1, Math.floor((t0 - off + ((px + 1) / w) * span) * RATE));
      let v = 0;
      for (let k = a; k <= Math.max(a, b); k++) {
        const q = pick(k);
        if (q > v) v = q;
      }
      pts.push([px, mapY(v)]);
    }
  } else {
    for (let k = Math.floor(f0); k <= Math.ceil(f1); k++) {
      pts.push([xOf(k), mapY(pick(k))]);
    }
  }
  return pts;
}

function paint(ctx, pts, baseY, fill, stroke) {
  if (!pts.length) return;
  ctx.beginPath();
  ctx.moveTo(pts[0][0], baseY);
  for (const p of pts) ctx.lineTo(p[0], p[1]);
  ctx.lineTo(pts[pts.length - 1][0], baseY);
  ctx.closePath();
  ctx.fillStyle = fill;
  ctx.fill();
  if (!stroke) return;
  ctx.beginPath();
  ctx.moveTo(pts[0][0], pts[0][1]);
  for (let i = 1; i < pts.length; i++) ctx.lineTo(pts[i][0], pts[i][1]);
  ctx.strokeStyle = stroke;
  ctx.lineWidth = 1.4;
  ctx.stroke();
}

function updateView(st) {
  const x = st.view1 > st.view0 ? META.songDuration / (st.view1 - st.view0) : 1;
  st.viewEl.textContent = fmt(st.view0) + ' \u2013 ' + fmt(st.view1) + '  (x' + x.toFixed(1) + ')';
}

function draw(st) {
  const c = st.canvas, ctx = st.ctx;
  const w = c.clientWidth, h = c.clientHeight;
  if (!w || !h) return;
  const dpr = window.devicePixelRatio || 1;
  if (c.width !== Math.round(w * dpr) || c.height !== Math.round(h * dpr)) {
    c.width = Math.round(w * dpr);
    c.height = Math.round(h * dpr);
  }
  ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
  ctx.fillStyle = '#0d0d11';
  ctx.fillRect(0, 0, w, h);

  const topY = 6, baseY = h - 24;
  const midY = (topY + baseY) / 2;
  const waveH = (baseY - topY) * 0.34;
  const mapY = v => baseY - Math.min(1, Math.max(0, v / st.scale)) * (baseY - topY);

  ctx.strokeStyle = '#1e1e2c';
  ctx.lineWidth = 1;
  for (const f of [0.25, 0.5, 0.75, 1]) {
    const y = Math.round(baseY - (baseY - topY) * f) + 0.5;
    ctx.beginPath();
    ctx.moveTo(0, y);
    ctx.lineTo(w, y);
    ctx.stroke();
  }

  const [s1, s2] = sampleRange(st);
  const level = usableLevel(st, s1, s2, w);
  const sr = st.sampleRate, n = st.totalSamples, span = st.view1 - st.view0;
  ctx.fillStyle = 'rgba(150,158,178,0.34)';
  for (let px = 0; px < w; px++) {
    let a = Math.floor((st.view0 + (px / w) * span - st.lead) * sr);
    let b = Math.ceil((st.view0 + ((px + 1) / w) * span - st.lead) * sr);
    if (b < 0 || a > n - 1) continue;
    a = Math.max(0, a);
    b = Math.min(n - 1, b);
    if (!streamRange(st, level, a, b)) continue;
    const y1 = midY - rangeOut.mx * waveH;
    const y2 = midY - rangeOut.mn * waveH;
    ctx.fillRect(px, Math.min(y1, y2), 1, Math.max(1, Math.abs(y2 - y1)));
  }

  const own = st.own, others = st.others;
  const t0 = st.view0, t1 = st.view1;
  const ownPts = envPoints(k => own[k], own.length, t0, t1, w, mapY, st.lead);
  const otherPts = envPoints(k => others[k], others.length, t0, t1, w, mapY, st.lead);
  const overlapPts = envPoints(k => Math.min(own[k] || 0, others[k] || 0), TOTAL.length, t0, t1, w, mapY, st.lead);

  paint(ctx, otherPts, baseY, 'rgba(255,169,77,0.13)', 'rgba(255,169,77,0.5)');
  paint(ctx, ownPts, baseY, 'rgba(77,171,247,0.20)', 'rgba(77,171,247,0.5)');
  paint(ctx, overlapPts, baseY, 'rgba(81,207,102,0.26)', null);

  ctx.strokeStyle = 'rgba(255,255,255,0.30)';
  ctx.lineWidth = 1;
  ctx.beginPath();
  ctx.moveTo(0, baseY + 0.5);
  ctx.lineTo(w, baseY + 0.5);
  ctx.stroke();

  const axisY = h - 18;
  ctx.fillStyle = '#a0a0b0';
  ctx.font = '10px monospace';
  const ticks = Math.max(2, Math.min(8, Math.floor(w / 90)));
  for (let i = 0; i <= ticks; i++) {
    const x = (i / ticks) * w;
    ctx.beginPath();
    ctx.moveTo(x, axisY);
    ctx.lineTo(x, axisY + 4);
    ctx.strokeStyle = 'rgba(255,255,255,0.28)';
    ctx.stroke();
    ctx.textAlign = i === 0 ? 'left' : i === ticks ? 'right' : 'center';
    const label = i === ticks ? fmt(t1) : fmt(t0 + (i / ticks) * (t1 - t0));
    ctx.fillText(label, x, h - 3);
  }

  const t = st.lead + (st.audio ? st.audio.currentTime : 0);
  if (t >= st.view0 && t <= st.view1 && (t > 0 || !st.audio.paused)) {
    const x = (t - st.view0) / (st.view1 - st.view0) * w;
    ctx.strokeStyle = '#ff5252';
    ctx.lineWidth = 2;
    ctx.beginPath();
    ctx.moveTo(x, 0);
    ctx.lineTo(x, h);
    ctx.stroke();
  }

  if (st.drag && Math.abs(st.drag.x1 - st.drag.x0) > 4) {
    const a = Math.max(0, Math.min(st.drag.x0, st.drag.x1));
    const b = Math.min(w, Math.max(st.drag.x0, st.drag.x1));
    ctx.fillStyle = 'rgba(255,235,59,0.16)';
    ctx.fillRect(a, topY, b - a, baseY - topY);
    ctx.strokeStyle = '#ffeb3b';
    ctx.lineWidth = 1;
    ctx.strokeRect(a + 0.5, topY + 0.5, Math.max(0, b - a - 1), baseY - topY - 1);
  }

  if (st.hover !== null) {
    ctx.strokeStyle = 'rgba(255,255,255,0.45)';
    ctx.lineWidth = 1;
    ctx.setLineDash([4, 4]);
    ctx.beginPath();
    ctx.moveTo(st.hover, topY);
    ctx.lineTo(st.hover, baseY);
    ctx.stroke();
    ctx.setLineDash([]);
  }
  updateView(st);
}

function render(st) {
  if (!st) return;
  const w = st.canvas.clientWidth;
  if (!w) return;
  const [s1, s2] = sampleRange(st);
  requestViewLevel(st, s1, s2, w);
  draw(st);
}

function schedule(st) {
  if (!st || queued.has(st)) return;
  queued.add(st);
  requestAnimationFrame(() => { queued.delete(st); render(st); });
}

function zoomTo(st, a, b) {
  a = Math.max(0, a);
  b = Math.min(META.songDuration, b);
  if (b - a < Math.min(MIN_SPAN, META.songDuration)) return;
  st.hist.push([st.view0, st.view1]);
  st.view0 = a;
  st.view1 = b;
  st.hover = null;
  st.tip.style.opacity = '0';
  render(st);
}

function zoomOut(st) {
  const prev = st.hist.pop();
  if (!prev) return;
  st.view0 = prev[0];
  st.view1 = prev[1];
  st.hover = null;
  st.tip.style.opacity = '0';
  render(st);
}

function resetZoom(st) {
  st.hist = [];
  st.view0 = 0;
  st.view1 = META.songDuration;
  st.hover = null;
  st.tip.style.opacity = '0';
  render(st);
}

function seekAndPlay(st, t) {
  t = Math.max(0, Math.min(st.duration, t - st.lead));
  const go = () => {
    try { st.audio.currentTime = t; } catch (e) {}
    draw(st);
    const p = st.audio.play();
    if (p && p.catch) p.catch(() => {});
  };
  if (st.audio.readyState >= 1) go();
  else st.audio.addEventListener('loadedmetadata', go, { once: true });
}

let raf = 0;
function playLoop() {
  raf = 0;
  let live = false;
  for (const st of STEMS) {
    if (!st.audio || st.audio.paused) continue;
    live = true;
    draw(st);
  }
  if (live) raf = requestAnimationFrame(playLoop);
}

function attach(st) {
  const c = st.canvas;
  c.addEventListener('mousedown', e => {
    if (e.button !== 0) return;
    e.preventDefault();
    const r = c.getBoundingClientRect();
    const x = Math.max(0, Math.min(r.width, e.clientX - r.left));
    st.drag = { x0: x, x1: x };
    draw(st);
  });

  c.addEventListener('mousemove', e => {
    const r = c.getBoundingClientRect();
    const x = Math.max(0, Math.min(r.width, e.clientX - r.left));
    if (st.drag) st.drag.x1 = x;
    st.hover = x;
    const t = xToTime(st, x, r.width);
    const k = Math.round(t * RATE);
    const own = k >= 0 && k < st.own.length ? st.own[k] / 32767 : 0;
    const other = k >= 0 && k < st.others.length ? st.others[k] / 32767 : 0;
    st.tip.innerHTML = 'Time ' + fmt(t) + ' &middot; sample ' +
      Math.round(Math.max(0, Math.min(t - st.lead, st.duration)) * st.sampleRate) +
      ' &middot; own ' + own.toFixed(3) + ' &middot; others ' + other.toFixed(3);
    st.tip.style.opacity = '1';
    draw(st);
  });

  c.addEventListener('mouseup', e => {
    if (!st.drag) return;
    const d = st.drag;
    const w = c.clientWidth;
    st.drag = null;
    if (Math.abs(d.x1 - d.x0) > 4) {
      zoomTo(st, xToTime(st, Math.min(d.x0, d.x1), w), xToTime(st, Math.max(d.x0, d.x1), w));
    } else {
      seekAndPlay(st, xToTime(st, d.x0, w));
    }
    draw(st);
  });

  c.addEventListener('mouseleave', () => {
    st.drag = null;
    st.hover = null;
    st.tip.style.opacity = '0';
    draw(st);
  });

  c.addEventListener('contextmenu', e => {
    e.preventDefault();
    zoomOut(st);
  });

  document.getElementById('reset' + st.id).addEventListener('click', () => resetZoom(st));
  st.audio.addEventListener('play', () => { if (!raf) raf = requestAnimationFrame(playLoop); });
  st.audio.addEventListener('pause', () => draw(st));
  st.audio.addEventListener('ended', () => draw(st));
  st.audio.addEventListener('seeked', () => draw(st));
  st.audio.addEventListener('timeupdate', () => draw(st));
  window.addEventListener('resize', () => schedule(st));
}
"""

HTML_TEMPLATE = r"""<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1.0">
<link rel="icon" href="data:,">
<title>__TITLE__</title>
<style>
  :root { --bg:#121216; --card:#1a1a22; --border:#2a2a36; --text:#e1e1e6; --muted:#a0a0b0; --accent:#2196f3; }
  * { box-sizing:border-box; margin:0; padding:0; font-family:-apple-system,BlinkMacSystemFont,'Segoe UI',Roboto,Helvetica,Arial,sans-serif; }
  body { background:var(--bg); color:var(--text); padding:20px; line-height:1.5; }
  header { background:var(--card); padding:18px 20px; border-radius:10px; border:1px solid var(--border); margin-bottom:16px; }
  h1 { font-size:1.45rem; color:#fff; margin-bottom:4px; }
  .meta { font-size:0.85rem; color:var(--muted); display:flex; gap:18px; flex-wrap:wrap; }
  .hint { font-size:0.78rem; color:var(--muted); margin-top:10px; padding-top:10px; border-top:1px solid var(--border); font-style:italic; }
  .legend { display:flex; gap:16px; flex-wrap:wrap; font-size:0.78rem; color:var(--muted); margin-top:8px; }
  .legend span { display:flex; align-items:center; gap:5px; }
  .swatch { width:20px; height:10px; border-radius:2px; }
  .card { background:var(--card); border:1px solid var(--border); border-radius:10px; padding:12px; margin-bottom:16px; }
  .head { display:flex; align-items:baseline; gap:12px; flex-wrap:wrap; margin-bottom:10px; }
  .name { font-weight:700; color:#fff; font-size:0.92rem; font-family:monospace; }
  .score { font-family:monospace; font-weight:700; font-size:0.9rem; color:var(--sc); border:1px solid var(--sc); border-radius:999px; padding:1px 9px; }
  .view { margin-left:auto; font-size:0.78rem; color:#4caf50; font-family:monospace; font-weight:bold; }
  .wrap { position:relative; width:100%; height:220px; background:#0d0d11; border:1px solid #22222e; border-radius:6px; overflow:hidden; user-select:none; }
  canvas { display:block; width:100%; height:100%; cursor:crosshair; }
  .tooltip { position:absolute; top:8px; right:12px; background:rgba(10,10,15,0.88); border:1px solid #333344; padding:5px 8px; border-radius:4px; font-size:0.74rem; font-family:monospace; color:#fff; pointer-events:none; opacity:0; transition:opacity 0.12s; max-width:calc(100% - 24px); }
  .toolbar { display:flex; align-items:center; gap:12px; flex-wrap:wrap; margin-top:10px; }
  audio { height:34px; outline:none; border-radius:4px; }
  .btn { background:var(--accent); color:#fff; border:none; border-radius:6px; padding:7px 14px; font-size:0.85rem; font-weight:600; cursor:pointer; }
  .btn:hover { opacity:0.88; }
  footer { font-size:0.78rem; color:var(--muted); padding:0 4px; }
</style>
</head>
<body>
<header>
  <h1>Stem Transience Correlation</h1>
  <div class="meta">__META_LINE__</div>
  <div class="legend">
    <span><span class="swatch" style="background:rgba(150,158,178,0.34)"></span>Stem waveform</span>
    <span><span class="swatch" style="background:#4dabf7"></span>This stem's transience</span>
    <span><span class="swatch" style="background:#ffa94d"></span>All other stems combined</span>
    <span><span class="swatch" style="background:#51cf66"></span>Overlap</span>
    <span><span class="swatch" style="background:#ff5252"></span>Playhead</span>
  </div>
  <div class="hint">Score = the area shared by this stem's transience envelope and the sum of every other stem's, divided by this stem's own transience area. Transience is the positive derivative of the peak-amplitude envelope. Stems sit at their <code>song.json</code> lead-ins, so every graph shares the song timeline. Drag horizontally to zoom, right-click to zoom out one level, click to move the playhead and play. Reset Zoom restores the whole song.</div>
</header>
<main>
__CARDS__
</main>
<footer>Waveform detail is streamed in progressively finer chunked tiers with a 192 MB browser cache, so deep zooming does not load the whole file. Audio seeking needs byte-range support, e.g. <code>python3 skills/analyze-palettes/pattern_report_server.py --root . --port 8000</code>.</footer>
<script src="__DATA_SCRIPT__"></script>
<script>
__REPORT_JS__
</script>
</body>
</html>
"""


def build(stems_dir, out_dir):
    leads, leads_path = read_leads(stems_dir)
    out_root = os.path.abspath(out_dir) + os.sep
    files = sorted(
        (os.path.join(root, f)
         for root, _, names in os.walk(stems_dir)
         for f in names if f.lower().endswith('.wav')
         and not os.path.abspath(os.path.join(root, f)).startswith(out_root)),
        key=lambda p: natural(os.path.basename(p)))
    if not files:
        raise SystemExit('No WAV files in %s' % stems_dir)

    data_dir = os.path.join(out_dir, DATA_DIR)
    shutil.rmtree(data_dir, ignore_errors=True)
    os.makedirs(data_dir, exist_ok=True)

    stems = []
    for path in files:
        sr, channels, x = read_mono(path)
        env, step_seconds = transience(x, sr)
        stems.append({
            'path': path,
            'name': os.path.splitext(os.path.basename(path))[0],
            'sampleRate': sr,
            'channels': channels,
            'pcm': np.clip(np.round(x * 32767), -32768, 32767).astype('<i2'),
            'env': env,
            'envStep': step_seconds,
            'duration': len(x) / sr if sr else 0.0,
            'lead': leads.get(os.path.basename(path).lower(), 0.0),
        })
        print('Read %s (%.2fs, %d Hz, lead-in %.3fs)'
              % (stems[-1]['name'], stems[-1]['duration'], sr, stems[-1]['lead']))

    duration = max(s['lead'] + s['duration'] for s in stems)
    frames = int(np.ceil(duration * ENV_RATE)) + 1
    matrix = np.stack([resample_env(s['env'], s['envStep'], frames, s['lead']) for s in stems])
    total = matrix.sum(axis=0)
    own_area = matrix.sum(axis=1)
    overlap = np.minimum(matrix, np.maximum(total[None, :] - matrix, 0.0)).sum(axis=1)
    scores = [float(o / a) if a > 0 else None for o, a in zip(overlap, own_area)]

    scale = float(max(total.max() if len(total) else 0.0, matrix.max() if matrix.size else 0.0)) or 1.0
    def q(env):
        return np.clip(np.round(env / scale * 32767.0), 0, 32767).astype('<i2')

    order = sorted(range(len(stems)), key=lambda i: (-(scores[i] if scores[i] is not None else -1), natural(stems[i]['name'])))
    meta_stems = []
    for display, idx in enumerate(order):
        s = stems[idx]
        stem_dir = os.path.join(data_dir, str(display))
        os.makedirs(stem_dir, exist_ok=True)
        shutil.copyfile(s['path'], os.path.join(stem_dir, os.path.basename(s['path'])))
        print('Writing tiers for %s' % s['name'])
        levels = []
        for step in LEVELS:
            levels.append({'step': step, 'bins': write_level(s['pcm'], step, stem_dir, display)})
        coarse, coarse_bins = minmax_bins(s['pcm'], COARSE_STEP)
        meta_stems.append({
            'name': s['name'],
            'sampleRate': int(s['sampleRate']),
            'channels': int(s['channels']),
            'totalSamples': int(len(s['pcm'])),
            'duration': round(s['duration'], 6),
            'lead': round(s['lead'], 6),
            'score': None if scores[idx] is None else round(scores[idx], 6),
            'audio': '%s/%d/%s' % (DATA_URL, display, quote(os.path.basename(s['path']))),
            'ownB64': b64_i16(q(matrix[idx])),
            'coarseB64': b64_i16(coarse),
            'coarseBins': int(coarse_bins),
            'levels': levels,
        })
        del s['pcm']

    meta = {
        'envRate': ENV_RATE,
        'songDuration': round(duration, 6),
        'chunkBins': CHUNK_BINS,
        'coarseStep': COARSE_STEP,
        'dataDir': DATA_URL,
        'total': b64_i16(q(total)),
        'stems': meta_stems,
    }
    with open(os.path.join(data_dir, 'report_data.js'), 'w') as f:
        f.write('window.STEM_REPORT = ' + json.dumps(meta, separators=(',', ':')) + ';\n')

    cards = '\n'.join(card(s, i) for i, s in enumerate(meta_stems))
    leads_line = ('<span><strong>Lead-ins:</strong> %s</span>' % html.escape(os.path.basename(leads_path))
                  if leads else '')
    meta_line = (
        '<span><strong>Source:</strong> %s</span>'
        '<span><strong>Stems:</strong> %d</span>'
        '<span><strong>Song:</strong> %s</span>'
        '<span><strong>Envelope:</strong> %d Hz, %d ms peak window</span>%s'
    ) % (html.escape(os.path.basename(os.path.normpath(stems_dir))), len(stems),
         fmt(duration), ENV_RATE, round(PEAK_SECONDS * 1000), leads_line)
    title = 'Stem Transience Correlation - ' + os.path.basename(os.path.normpath(stems_dir))
    text = (HTML_TEMPLATE
            .replace('__TITLE__', html.escape(title))
            .replace('__META_LINE__', meta_line)
            .replace('__CARDS__', cards)
            .replace('__DATA_SCRIPT__', DATA_URL + '/report_data.js')
            .replace('__REPORT_JS__', JS))
    report_path = os.path.join(out_dir, REPORT_NAME)
    with open(report_path, 'w') as f:
        f.write(text)
    print('Report:', report_path)


def main():
    ap = argparse.ArgumentParser(description='Build an interactive stem transience-correlation report.')
    ap.add_argument('stems', help='Folder of WAV stems')
    ap.add_argument('--out', default=None, help='Output folder (default: <stems>/[correlation])')
    args = ap.parse_args()
    stems_dir = os.path.abspath(args.stems)
    if not os.path.isdir(stems_dir):
        raise SystemExit('Not a folder: %s' % stems_dir)
    out_dir = os.path.abspath(args.out) if args.out else os.path.join(stems_dir, '[correlation]')
    os.makedirs(out_dir, exist_ok=True)
    build(stems_dir, out_dir)


if __name__ == '__main__':
    main()
