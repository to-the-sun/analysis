#!/usr/bin/env python3
"""Self-similarity report for every palette WAV in a folder.

Drag a folder onto this script (or double-click it and pick the folder). Each
WAV gets one SSM per stretch between sound changes, and the page plays the
palette in place with a playhead that follows the matrix.
"""
import html
import json
import sys
import webbrowser
from pathlib import Path

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib.ticker import FuncFormatter, MaxNLocator
import numpy as np
from scipy.io import wavfile
from scipy.signal import find_peaks

WIN, HOP = 4096, 0.1
MIN_LAG, MIN_DUR, FLOOR_DB, MAX_FRAMES = 0.5, 2.0, -50.0, 1100
OUT, IMG = "ssm_report", "img"


def mmss(t, prec=1):
    m = int(t // 60)
    return f"{m}:{t - 60 * m:0{2 if not prec else 3 + prec}.{prec}f}"


PAGE = """<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<title>Self-similarity by sound change</title>
<style>
  :root { --ink:#e8eef2; --dim:#8fa4b0; --accent:#22d3c5; --card:#161c24; --line:#232c37; }
  * { box-sizing:border-box; }
  body { margin:0; background:#0d1117; color:var(--ink);
         font:15px/1.55 -apple-system,"Segoe UI",Roboto,Helvetica,Arial,sans-serif; }
  main { max-width:1320px; margin:0 auto; padding:26px 22px 70px; }
  h1 { font-size:22px; margin:0 0 6px; }
  h2 { font-size:16px; margin:0; }
  p.lede { color:var(--dim); max-width:82ch; margin:0 0 26px; }
  section { margin-bottom:44px; }
  .head { display:flex; gap:14px; align-items:baseline; flex-wrap:wrap; margin-bottom:10px; }
  .head span { color:var(--dim); font-size:13px; }
  .bar { display:flex; align-items:center; gap:14px; flex-wrap:wrap; background:var(--card);
         border:1px solid var(--line); border-radius:10px; padding:10px 13px;
         margin-bottom:14px; position:sticky; top:0; z-index:6; }
  audio { height:34px; flex:1 1 360px; min-width:260px; }
  .clock { font-variant-numeric:tabular-nums; color:var(--accent); font-weight:600; min-width:112px; }
  .hint { color:var(--dim); font-size:12.5px; }
  .grid { display:grid; gap:18px; grid-template-columns:repeat(auto-fill,minmax(460px,1fr)); }
  figure.ssm { margin:0; background:#fff; border-radius:10px; overflow:hidden;
               user-select:none; box-shadow:0 14px 40px rgba(0,0,0,.42); }
  .scene { position:relative; transform-origin:0 0; }
  .scene img { display:block; width:100%; height:auto; }
  figcaption { background:var(--card); color:var(--dim); font-size:12.5px; padding:9px 13px;
               display:flex; justify-content:space-between; gap:12px; flex-wrap:wrap; }
  figcaption b { color:var(--ink); font-weight:600; }
  .ph { position:absolute; background:var(--accent); opacity:.95; pointer-events:none; z-index:3;
        display:none; box-shadow:0 0 0 1px rgba(0,0,0,.35),0 0 10px rgba(34,211,197,.75); }
  .phv { width:2px; } .phh { height:2px; }
  .grab { position:absolute; cursor:crosshair; z-index:2; }
  .sel { position:absolute; display:none; pointer-events:none; z-index:2;
         border:1.5px dashed var(--accent); background:rgba(34,211,197,.16); }
  p.quiet { color:var(--dim); font-size:12.5px; margin:14px 0 0; }
</style>
</head>
<body>
<main>
<h1>Self-similarity by sound change</h1>
<p class="lede">Every stretch between one sound change and the next gets its own matrix: chroma
  per 0.1 s frame, cosine similarity. Bright blocks repeat at a diagonal step equal to the loop
  length, which the lag curve on the right reports. Press play and the cursor marks the moment on
  the matrix and the row it is being compared with; drag a square on a matrix to zoom in,
  right-click to reset, click to jump there.</p>
__SECTIONS__
</main>
<script>
const GEO = __GEO__;
const views = [];
const clamp = (v, lo, hi) => Math.max(lo, Math.min(hi, v));
const mmss = (t) => `${Math.floor(t / 60)}:${(t % 60).toFixed(2).padStart(5, "0")}`;

document.querySelectorAll("figure.ssm").forEach((fig) => {
  const v = {
    fig, scene: fig.querySelector(".scene"), img: fig.querySelector("img"),
    phS: fig.querySelector(".phv"), phH: fig.querySelector(".phh"),
    grab: fig.querySelector(".grab"), sel: fig.querySelector(".sel"),
    audio: fig.closest("section").querySelector("audio"),
    start: +fig.dataset.start, dur: +fig.dataset.dur, k: 1, tx: 0, ty: 0, W: 0, H: 0
  };
  Object.assign(v.grab.style, { left: GEO.s.x * 100 + "%", top: GEO.s.y * 100 + "%",
                                width: GEO.s.w * 100 + "%", height: GEO.s.h * 100 + "%" });
  const measure = () => { v.W = v.scene.offsetWidth; v.H = v.scene.offsetHeight; };
  const focus = (px, py) => {
    v.tx = clamp(v.W / 2 - v.k * px, v.W - v.k * v.W, 0);
    v.ty = clamp(v.H / 2 - v.k * py, v.H - v.k * v.H, 0);
    v.phS.style.width = 2 / v.k + "px";
    v.phH.style.height = 2 / v.k + "px";
    v.scene.style.transform = `translate(${v.tx}px,${v.ty}px) scale(${v.k})`;
  };
  const at = (e) => {
    const r = v.fig.getBoundingClientRect();
    return { x: (e.clientX - r.left - v.tx) / v.k, y: (e.clientY - r.top - v.ty) / v.k };
  };
  const timeAt = (x) => v.start + clamp((x / (v.W || 1) - GEO.s.x) / GEO.s.w, 0, 1) * v.dur;

  v.place = (t) => {
    const local = t - v.start;
    if (local < 0 || local > v.dur || !v.W) {
      v.phS.style.display = v.phH.style.display = "none";
      return;
    }
    const f = local / v.dur;
    v.phS.style.display = v.phH.style.display = "block";
    v.phS.style.left = (GEO.s.x + GEO.s.w * f) * 100 + "%";
    v.phS.style.top = GEO.s.y * 100 + "%";
    v.phS.style.height = GEO.s.h * 100 + "%";
    v.phH.style.left = GEO.s.x * 100 + "%";
    v.phH.style.top = (GEO.s.y + GEO.s.h * f) * 100 + "%";
    v.phH.style.width = GEO.s.w * 100 + "%";
    if (v.k > 1) focus((GEO.s.x + GEO.s.w * f) * v.W, (GEO.s.y + GEO.s.h * f) * v.H);
  };

  let drag = null;
  v.grab.addEventListener("pointerdown", (e) => {
    if (e.button !== 0) return;
    measure();
    drag = Object.assign(at(e), { side: 0, sx: 1, sy: 1 });
  });
  window.addEventListener("pointermove", (e) => {
    if (!drag) return;
    const p = at(e), side = Math.max(Math.abs(p.x - drag.x), Math.abs(p.y - drag.y));
    Object.assign(drag, { side, sx: p.x < drag.x ? -1 : 1, sy: p.y < drag.y ? -1 : 1 });
    Object.assign(v.sel.style, {
      display: "block",
      left: Math.min(drag.x, drag.x + side * drag.sx) / v.W * 100 + "%",
      top: Math.min(drag.y, drag.y + side * drag.sy) / v.H * 100 + "%",
      width: side / v.W * 100 + "%", height: side / v.H * 100 + "%"
    });
  });
  window.addEventListener("pointerup", () => {
    if (!drag) return;
    const { side, x } = drag;
    drag = null;
    v.sel.style.display = "none";
    if (side >= 8) {
      v.k = Math.min(v.W / side, 16);
      v.place(v.audio.currentTime);
      return;
    }
    seek(v, timeAt(x));
  });
  v.fig.addEventListener("contextmenu", (e) => {
    e.preventDefault();
    v.k = 1;
    focus(0, 0);
  });
  v.img.addEventListener("load", () => { measure(); v.place(v.audio.currentTime); });
  views.push(v);
});

function seek(v, t) {
  const go = () => { v.audio.currentTime = t; v.place(t); v.audio.play().catch(() => {}); };
  if (v.audio.readyState >= 1) go();
  else v.audio.addEventListener("loadedmetadata", go, { once: true });
}

function draw() {
  views.forEach((v) => v.place(v.audio.currentTime));
  views.forEach((v) => {
    const clock = v.fig.closest("section").querySelector(".clock");
    if (clock) clock.textContent = mmss(v.audio.currentTime);
  });
}

function loop() {
  draw();
  if (views.some((v) => !v.audio.paused)) requestAnimationFrame(loop);
}

views.forEach((v) => {
  v.audio.addEventListener("play", loop);
  v.audio.addEventListener("seeked", draw);
  v.audio.addEventListener("timeupdate", () => { if (v.audio.paused) draw(); });
  v.audio.addEventListener("loadedmetadata", draw);
});
window.addEventListener("resize", () => {
  views.forEach((v) => { v.W = v.scene.offsetWidth; v.H = v.scene.offsetHeight; v.place(v.audio.currentTime); });
});
draw();
</script>
</body>
</html>
"""

SECTION = """<section>
  <div class="head"><h2>__NAME__</h2><span>__META__</span></div>
  <div class="bar">
    <audio controls preload="metadata" src="__SRC__"></audio>
    <span class="clock">0:00.00</span>
    <span class="hint">cursor = now on each matrix · drag a square to zoom · click to jump</span>
  </div>
  <div class="grid">
__FIGURES__
  </div>
__QUIET__
</section>"""

FIGURE = """    <figure class="ssm" data-start="__START__" data-dur="__DUR__">
      <div class="scene">
        <img src="__IMG__" alt="__ALT__">
        <div class="ph phv"></div><div class="ph phh"></div>
        <div class="grab"></div><div class="sel"></div>
      </div>
      <figcaption><b>__TITLE__</b><span>__NOTE__</span></figcaption>
    </figure>"""


def read_wav(path):
    sr, data = wavfile.read(path)
    if data.dtype == np.int16:
        x = data.astype(np.float32) / 32768
    elif data.dtype == np.int32:
        x = data.astype(np.float32) / 2147483648
    elif np.issubdtype(data.dtype, np.floating):
        x = data.astype(np.float32)
    else:
        x = (data.astype(np.float32) - 128) / 128
    return (x.mean(axis=1) if x.ndim > 1 else x), sr


def chromagram(x, sr):
    hop, window = int(HOP * sr), np.hanning(WIN)
    frames = 1 + (len(x) - WIN) // hop
    freq = np.fft.rfftfreq(WIN, 1 / sr)
    pc = (np.round(12 * np.log2(np.maximum(freq, 1e-6) / 440)) % 12).astype(int)
    fold = np.zeros((len(freq), 12))
    fold[np.arange(len(freq)), pc] = freq > 40
    out = np.zeros((frames, 12), np.float32)
    for start in range(0, frames, 1500):
        stop = min(frames, start + 1500)
        idx = np.arange(WIN)[None, :] + hop * np.arange(start, stop)[:, None]
        out[start:stop] = np.abs(np.fft.rfft(x[idx] * window, axis=1)) @ fold
    return out


def sounds_file(wav):
    stem = wav.stem
    prefix = "sounds" + stem[7:] if stem.lower().startswith("palette") else "sounds"
    found = []
    for path in wav.parent.iterdir():
        if path.suffix.lower() not in (".txt", ".text"):
            continue
        if not path.stem.lower().startswith(prefix.lower()):
            continue
        rest = path.stem[len(prefix):]
        if not rest or not rest[0].isalnum():
            found.append(path)
    return sorted(found, key=lambda p: (len(p.name), p.name))[0] if found else None


def marks(path):
    out = []
    for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
        text = line.strip()
        if not text or text[0] in "#/":
            continue
        parts = text.replace(";", " ").replace(",", " ").split()
        if len(parts) < 3:
            continue
        try:
            out.append((float(parts[1]) / 1000, " ".join(parts[2:])))
        except ValueError:
            continue
    return sorted(out)


def portions(wav, duration):
    hits = marks(sounds_file(wav)) if sounds_file(wav) else []
    if not hits:
        return [(0.0, duration, "whole file")]
    spans = [(max(0.0, t), hits[i + 1][0] if i + 1 < len(hits) else duration, label)
             for i, (t, label) in enumerate(hits)]
    return [(s, min(e, duration), label) for s, e, label in spans if min(e, duration) > s]


def features(part):
    step = int(np.ceil(len(part) / MAX_FRAMES))
    usable = len(part) // step * step
    if not usable:
        return None, HOP
    c = part[:usable].reshape(-1, step, 12).mean(1)
    c = np.stack([c[max(0, i - 1):i + 2].mean(0) for i in range(len(c))])
    X = np.log1p(12 * c)
    X -= X.mean(axis=1, keepdims=True)
    X /= np.maximum(np.linalg.norm(X, axis=1, keepdims=True), 1e-9)
    return X, HOP * step


def period(S, step, duration):
    lags = np.arange(int(np.ceil(MIN_LAG / step)), len(S))
    curve = np.array([np.mean(np.diag(S, k)) for k in lags])
    t = lags * step
    peaks = find_peaks(curve, prominence=0.01)[0]
    inside = peaks[curve[peaks] >= curve.max() - 0.02]
    chosen = float(t[inside[0]]) if len(inside) else None
    if chosen and 3 * chosen > duration:
        chosen = None
    return t, curve, chosen


def figure(S, duration, t, curve, chosen, title, path):
    fig, (axS, axC) = plt.subplots(1, 2, figsize=(11.2, 5.0),
                                   gridspec_kw={"width_ratios": [1.15, 1]})
    axS.imshow(S, origin="upper", cmap="magma", vmin=0.55, vmax=1.0,
               extent=[0, duration, duration, 0], interpolation="nearest")
    clock = FuncFormatter(lambda v, _: mmss(v))
    for axis in (axS.xaxis, axS.yaxis, axC.xaxis):
        axis.set_major_locator(MaxNLocator(6, steps=[1, 2, 2.5, 5, 10]))
        axis.set_major_formatter(clock)
    axS.set_xlabel("time in portion (m:ss)")
    axS.set_ylabel("time in portion (m:ss)")
    axS.set_title("self-similarity", loc="left", fontsize=10)
    axC.plot(t, curve, color="#1b3b52", lw=1.5)
    axC.axhline(curve.max(), color="#c0392b", lw=0.9, ls="--")
    axC.axhline(curve.max() - 0.02, color="#c0392b", lw=0.8, ls=":")
    if chosen:
        at = curve[np.argmin(abs(t - chosen))]
        axC.plot([chosen], [at], "o", ms=6, mfc="#c0392b", mec="white", zorder=5)
        axC.annotate(mmss(chosen, 2), (chosen, at), xytext=(8, 8),
                     textcoords="offset points", fontsize=9, color="#c0392b", weight="bold")
    axC.set_xlim(0, min(duration, 8 * (chosen or 1.5)))
    axC.set_ylim(max(0, curve.min() - 0.05), 1.02)
    axC.set_xlabel("lag (m:ss)")
    axC.set_ylabel("mean similarity on the diagonal")
    axC.set_title("lag curve", loc="left", fontsize=10)
    axC.grid(alpha=0.22)
    fig.suptitle(title, x=0.012, ha="left", fontsize=12)
    fig.tight_layout(rect=(0, 0, 1, 0.94))
    fig.savefig(path, dpi=105, facecolor="white")
    box = axS.get_position()
    plt.close(fig)
    return {"s": {"x": box.x0, "w": box.width, "y": 1 - (box.y0 + box.height), "h": box.height}}


def palette_section(wav, out):
    audio, sr = read_wav(wav)
    duration = len(audio) / sr
    chroma = chromagram(audio, sr)
    figures, quiet, geo = [], [], None
    for i, (start, end, label) in enumerate(portions(wav, duration), 1):
        slice_ = audio[int(start * sr):int(end * sr)]
        rms = 20 * np.log10(np.sqrt(np.mean(slice_.astype(np.float64) ** 2)) + 1e-12) if len(slice_) else -240.0
        if end - start < MIN_DUR or rms < FLOOR_DB:
            quiet.append(f"{mmss(start)}–{mmss(end)}")
            continue
        X, step = features(chroma[int(start / HOP):int(end / HOP)])
        S = X @ X.T
        length = len(X) * step
        t, curve, chosen = period(S, step, length)
        stem = f"{wav.stem}_{i:02d}"
        title = f"sound {label} · {mmss(start)}–{mmss(end)} · {mmss(length)}"
        box = figure(S, length, t, curve, chosen, title, out / IMG / f"{stem}.png")
        geo = geo or box
        figures.append({
            "img": f"{IMG}/{stem}.png", "start": start, "dur": length, "title": title,
            "note": (f"lag peak {mmss(chosen, 2)}" if chosen else "no lag peak")
                    + f" · {len(X)} × {step:.2f} s frames · RMS {rms:.0f} dBFS"
        })
        print(f"  {label:>10s} {mmss(start):>9s}–{mmss(end):<9s}  {mmss(length):>9s}  "
              f"{'period ' + mmss(chosen, 2) if chosen else 'no period'}")
    if not figures:
        print(f"  {wav.name}: no usable sound stretch")
        return None
    body = SECTION
    for key, value in [
        ("__NAME__", html.escape(wav.name)),
        ("__META__", f"{mmss(duration)} · {sr} Hz · {len(figures)} of "
                     f"{len(figures) + len(quiet)} stretches with significant audio"),
        ("__SRC__", html.escape("../" + wav.name, quote=True)),
        ("__FIGURES__", "\n".join(FIGURE
                                  .replace("__START__", f"{f['start']:.3f}")
                                  .replace("__DUR__", f"{f['dur']:.3f}")
                                  .replace("__IMG__", html.escape(f["img"]))
                                  .replace("__ALT__", html.escape("self-similarity matrix " + f["title"], quote=True))
                                  .replace("__TITLE__", html.escape(f["title"]))
                                  .replace("__NOTE__", html.escape(f["note"]))
                                  for f in figures)),
        ("__QUIET__", f'  <p class="quiet">Not shown, shorter than {MIN_DUR:.0f} s or quieter '
                      f'than {FLOOR_DB:.0f} dBFS: {", ".join(quiet)}</p>' if quiet else ""),
    ]:
        body = body.replace(key, value)
    return body, geo


def main():
    source = Path(sys.argv[1].strip('"').strip("'")) if len(sys.argv) > 1 else None
    if not source or not source.is_dir():
        from tkinter import Tk, filedialog
        root = Tk()
        root.withdraw()
        chosen = filedialog.askdirectory(title="Pick the folder holding the palette WAVs")
        root.destroy()
        if not chosen:
            return
        source = Path(chosen)
    wavs = sorted(p for p in source.iterdir() if p.suffix.lower() == ".wav")
    if not wavs:
        print(f"No WAV files in {source}")
        return
    out = source / OUT
    (out / IMG).mkdir(parents=True, exist_ok=True)
    for stale in (out / IMG).glob("*.png"):
        stale.unlink()
    built, geo = [], None
    for wav in wavs:
        print(wav.name)
        result = palette_section(wav, out)
        if result:
            built.append(result[0])
            geo = geo or result[1]
    if not built:
        print("Nothing to report")
        return
    page = (PAGE.replace("__GEO__", json.dumps(geo))
                .replace("__SECTIONS__", "\n".join(built)))
    index = out / "index.html"
    index.write_text(page, encoding="utf-8")
    print(f"Wrote {index}")
    webbrowser.open(index.resolve().as_uri())


if __name__ == "__main__":
    main()
