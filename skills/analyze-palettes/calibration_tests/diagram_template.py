#!/usr/bin/env python3
"""Render test_phrase_1.png: one row per pattern over the shared waveform."""
from pathlib import Path

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt
import numpy as np
from matplotlib.patches import Rectangle
from scipy.io import wavfile

HERE = Path(__file__).resolve().parent
WAV = HERE / "test_phrase_1.wav"
OUT = HERE / "test_phrase_1.png"

W, H, DPI = 4600, 1600, 200
LEFT, RIGHT, TOP, BOTTOM = 330, 4570, 232, 1439
BG, WAVE, MUTED, RULE = "#07080c", "#5e6773", "#9aa2ae", "#262832"

# name, color, band top/bottom (px), pattern start, loop, (index, segments) spans,
# envelope range mapped onto the row, and the edge the trace grows from
ROWS = [
    ("bass", "#f12a1e", (232, 533), 0.0, 1.074910, [(0, 73)], (0.543, 1.135), "bottom"),
    ("cymbal", "#f1ad03", (535, 835), 2.158, 2.149700, [(0, 35)], (0.006, 0.673), "bottom"),
    ("kick", "#3ad07d", (837, 1137), 2.158, 2.149700, [(0, 35)], (0.001, 0.695), "top"),
    ("kick", "#87fa96", (1139, 1439), 2.158, 2.149700, [(0, 6), (17, 5)], (0.514, 1.143), "top"),
]

PAD = 2  # px between a pattern box and its row edges


def env_at(sr, x, edges):
    """Peak amplitude per pixel column."""
    step = sr * (edges[1] - edges[0])
    out = np.empty(len(edges) - 1)
    for i, t in enumerate(edges[:-1]):
        a = int(t * sr)
        b = min(int(a + step) + 1, len(x))
        out[i] = np.abs(x[a:b]).max() if b > a else 0.0
    return out


def main():
    sr, pcm = wavfile.read(WAV)
    x = pcm.astype(np.float64) / 32768.0
    duration = len(x) / sr

    def y(px):
        return (BOTTOM - px) / (BOTTOM - TOP)

    fig = plt.figure(figsize=(W / DPI, H / DPI), dpi=DPI, facecolor=BG)
    ax = fig.add_axes([LEFT / W, 1 - BOTTOM / H, (RIGHT - LEFT) / W, (BOTTOM - TOP) / H])
    ax.set_xlim(0, duration)
    ax.set_ylim(0, 1)
    ax.axis("off")

    edges = np.linspace(0, duration, RIGHT - LEFT + 1)
    env = env_at(sr, x, edges)

    for name, color, (top, bottom), start, loop, spans, (lo, hi), anchor in ROWS:
        level = np.clip((env - lo) / (hi - lo), 0, 1) * (bottom - top) / (BOTTOM - TOP)
        base = y(bottom) if anchor == "bottom" else y(top)
        ax.fill_between(edges[:-1], base, base + level if anchor == "bottom" else base - level,
                        step="post", color=WAVE, lw=0, zorder=0)
        ax.add_patch(Rectangle((0, y(bottom)), duration, y(top) - y(bottom),
                               facecolor=color, alpha=0.04, edgecolor="none", zorder=1))

        for seg, count in spans:
            for j in range(count + 1):
                ax.plot([start + (seg + j) * loop] * 2, [y(bottom - PAD), y(top + PAD)],
                        color=color, lw=1.05, dashes=(2.0, 2.0), zorder=2)
            x0, x1 = start + seg * loop, start + (seg + count) * loop
            ax.add_patch(Rectangle((x0, y(bottom - PAD)), x1 - x0, y(top + PAD) - y(bottom - PAD),
                                   fill=False, edgecolor=color, lw=1.05, zorder=3))

        fig.add_artist(Rectangle((48 / W, 1 - ((top + bottom) / 2 + 7.5) / H), 15 / W, 15 / H,
                                 transform=fig.transFigure, facecolor=color,
                                 edgecolor="none", zorder=5))
        cy = 1 - (top + bottom) / 2 / H
        fig.text(76 / W, cy + 0.5 / H, name, fontsize=11.8, color=color, fontweight="bold",
                 va="baseline")
        fig.text(76 / W, cy - 26.5 / H, f"{loop:.2f} s × {sum(c for _, c in spans)}",
                 fontsize=8.0, color=MUTED, va="baseline")

    for px in (534, 836, 1138, 1439.5):
        fig.lines.append(plt.Line2D([LEFT / W, RIGHT / W], [1 - px / H] * 2,
                                    transform=fig.transFigure, color=RULE, lw=0.72, zorder=4))

    def text(x_px, y_px, s, size, color, weight="normal", ha="left"):
        fig.text(x_px / W, 1 - y_px / H, s, fontsize=size, color=color,
                 fontweight=weight, ha=ha, va="baseline")

    text(51, 89, WAV.name, 15.5, "#ffffff", "bold")
    text(50, 135, f"{duration:.1f} s · {sr / 1000:.1f} kHz · mono", 8.8, MUTED)
    text(48, 180, "outer rectangle = the whole pattern · "
                  "dashed verticals = the division between each segment", 8.6, MUTED)
    for t in np.arange(0, duration, 5):
        text(LEFT + t / duration * (RIGHT - LEFT), 1477, f"{t:.0f}", 7.6, MUTED, ha="center")
    text((LEFT + RIGHT) / 2, 1529, "time (s)", 7.6, MUTED, ha="center")

    fig.savefig(OUT, dpi=DPI, facecolor=BG)


if __name__ == "__main__":
    main()
