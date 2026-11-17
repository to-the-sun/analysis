# Analyze Palettes

Use this skill when the user gives one or more palette recordings and wants:

- repeated sections/patterns measured independently of project code;
- loop/bar/segment lengths measured;
- an interactive HTML report per file with the full waveform, pattern regions,
  segment rows, zooming, playback and playhead jumping;
- a song assembled from the patterns, with a mastered WAV and one WAV per
  pattern stem, plus a player page with per-stem arrangement waveforms;
- the reports, song and audio packaged for download.

The goal is a reproducible analysis, not a black-box detector. Report what is
measured, what is estimated, and where the uncertainty is.

## Core vocabulary

- **Bar**: the base pulse of a region. Equivalent to a measure in most standard
  depictions of music. A file can change tempo, so measure the bar in each
  region rather than once for the file.
- **Pass**: one recorded loop cycle. Its **loop point** is the millisecond
  position at which it starts. Pass `i` spans
  `[loop_point_i, loop_point_{i+1})`; the last pass runs to the end of the file.
  The line's optional pad is retained as pass metadata but does not move the
  loop anchor.
- **Segment**: a single iteration of a repeating pattern. All segments within
  the same pattern have the same length.
- **Pattern**: one contiguous repeated region of at least three segments. A
  pattern begins where the audio repeats; it is not required to begin on a pass
  loop point.
- **Pattern region**: the span of audio the loop was measured over, from its
  start to the end of its last complete segment. This is the unit the song uses,
  whole.
- **Loop anchor**: the assigned pass's loop point. The song aligns every
  pattern's loop anchor to one common song time.
- **Lead-in**: `pattern start - pass loop point`. It may be negative. The
  earliest lead-in across every pattern in every palette is shifted to 0, and
  the shifted value is the pattern's entry in the song, in seconds.
- **Song**: one master assembled from the patterns of every palette in the
  archive. Every palette contributes all of its patterns, once each. An archive
  gets one song, never one song per palette.

Patterns are independent of each other. A file may hold one loop length in every
region, or a different length in each; both are normal, and neither is imposed
by the tool. Uniform segment length is required only *within* a pattern, never
across patterns or across the file. Measure each region on its own terms.

`calibration_tests/test_phrase_1.png` illustrates this vocabulary

## 0. Inputs and preparation

1. List only the direct, non-trashed children of My Drive/[palettes]
   (`https://drive.google.com/drive/folders/1phwFvw-8xwgtFxkU1dEszvyo-UgXQhGv`).
   Query with `trashed = false`; never process trash, shortcuts, other folders,
   or duplicate copies. Each zip archive in that folder is a separate batch and
   song: give it its own report folder, master and song. Extract one archive into
   `artifacts/<archive-stem>/source/` and list its audio files. Keep the palette,
   its passes file and any sounds file together, and remember which archive they
   came from: that archive names the song master. `[2026-11-12-19-7-9].zip` yields the master
   `[2026-11-12-19-7-9].wav`. Pass the archive to `song_builder.py --archive`;
   `finished_song.wav` is only the fallback for audio that did not arrive in an
   archive. An archive may hold several palettes; they stay in that archive's
   batch and feed its one song.
2. Remove the archive from the Google Drive folder once it is extracted. Palettes
   come from that folder and are kept there, so deleting the processed
   `<stamp>.zip` is what marks the archive done. This is required, not optional.
   Deleting the Drive copy destroys the only stored copy of the source audio,
   and that is intended. The extracted palette and passes stay in the batch
   folder and the reports play the mono copy kept in `[data]/`; `[palettes]/`
   receives a palette generated from the stems (section 2).
3. Probe duration, sample rate and channel count.
4. Convert non-WAV input to mono 16-bit PCM WAV for the report builder:

   ```bash
   ffmpeg -v error -y -i input.mp3 -ac 1 -ar 44100 -c:a pcm_s16le output.wav
   ```

5. **Find the passes file. It is required.** Keep it beside the audio:
   - `palette_<stamp>.wav` pairs with `passes_<stamp>.txt`;
   - otherwise `passes*.txt` in the same folder.

   `passes.py` does this discovery for the scripts. If no passes file is found,
   stop and ask for it; do not fall back to a guessed song placement.

   A `sounds_<stamp>.txt` beside the palette is optional. It holds one
   `index, milliseconds number;` line per sound change. Each pattern is marked
   with the sound it overlaps most; without the file every pattern's `sound` is
   `null`.
6. Create a workspace output directory, for example
   `artifacts/<batch>/pattern_reports`. Keep generated data with the reports;
   the song master is written one level up, beside the report folder.
7. Dependencies: Python 3, NumPy, SciPy. `ffmpeg` only for non-WAV input.

### 0a. The passes format

One line per pass, `index, loop_point_ms pad;`. The pad is optional and is the
arbitrary offset the C analyzer adds when placing a stem, so it is leading blank
space rather than audio. Both the report builder and `song_builder.py` add it to
the lead:

```text
0, 977.324 1.;
1, 8531.882244 1.;
2, 9594.195189 1.;
```

Read the file, keep the pass table sorted by index, and convert milliseconds to
seconds:

```python
rows = passes.parse_passes(passes_path)
spans = passes.pass_spans(rows, duration)
```

Each pass duration must be close to an integer number of bars; estimate the bar
by fitting integer multiples of a candidate bar to all pass durations. A first
pass may be a partial pickup and a last pass a short tail; both are normal and
are not pattern regions.

## 1. Find patterns in the sections

Run the bundled helper:

```bash
python3 analyze_audio.py input.wav [more.wav ...] --json analysis.json
```

It computes, independently of the project:

- the pass table, an estimated bar, and runs of equal-length passes;
- a 64-band log-mel spectrogram (STFT 2048, hop 1024, about 43 fps);
- a positive spectral-flux onset envelope and band envelopes;
- silence-delimited sections using 0.1 s RMS, −50 dB, 0.5 s minimum silence;
- a low-rate self-similarity novelty curve;
- autocorrelation and lag-similarity peaks per section;
- a suggested loop length per section, with mean similarity and confidence;
- each section's assigned pass and raw lead-in.

The helper prints candidates; verify them rather than accepting them blindly.

### 1a. Regions come from the sections

Start from the silence-delimited sections: they are the composer's own
divisions, and a section is normally one pattern. Report one pattern per
section, with the region covering the section from just after the gap to the end
of its last complete segment. Do not carve a section down to the few bars that
repeat most exactly: a pattern is what a listener hears looping through the
whole section.

Split a long section only where the loop itself changes — a new bar length, a
new harmonic rhythm, an obvious change of density. Two or three sub-regions of
sixty to a hundred seconds each are typical for a five-minute section. Do not
leave a stretch of a section unrepresented when it loops at its own length.
Leave a little margin around fades; do not start a pattern in the middle of a
tail. A pattern start is a measured musical start, not a pass loop point.

### 1b. The loop is an integer number of bars

For each region:

1. Estimate the base bar from onset spacing or the ACF. Confirm it against the
   pass-duration estimate; measure it per region.
2. Test integer bar lengths that leave at least three segments. Include 1, 2, 3,
   4 and 8 bars where they fit.
3. Score each candidate by **mean lag similarity over the whole region**: the
   average, across every frame of the region, of the similarity between a frame
   and the frame one loop later.
4. Prefer the shortest candidate that explains the whole region. A stronger
   score at 2× or 4× a shorter loop is usually the same loop with a layer on
   top — but take the longer one when the region's high-level content needs it.
5. Confirm it is a **peak**: the chosen lag must score clearly above lags that
   are not near-integer bar multiples. A held pad or a fade tail matches at
   every lag, so a high score alone is not evidence.

Do **not** gate candidates on the fraction of frames that exceed a strict
similarity. Real loop regions score about 0.55–0.81 on the mean, and a
frame-wise gate rejects most of them while ranking steady textures above the
actual music.

### 1c. Passes place the patterns around one song anchor

Passes do not cut or snap patterns:

1. For each pattern, assign the pass it overlaps most in time. On a tie, use the
   earlier pass. The assigned pass's loop point is the pattern's **loop anchor**.
2. Compute `lead = pattern.start - pass.loop_point`.
3. Across every pattern in every palette, shift all leads by `-min(lead)` and
   clamp at 0. That shifted value is the pattern's **entry** in seconds.
4. Every pattern's loop anchor lands at the same song time, `-min(lead)`. This
   is the song anchor (`arr.js`'s `anchor` and the white line in `song.html`).
5. The song starts each stem at its entry. A negative raw lead is normal: it
   means the pattern starts before its pass's loop point. The pattern with the
   smallest lead defines the song's zero.

Do not round the entry to a bar, and do not move a pattern start onto a loop
point to make the lead zero. The lead is whatever the measured start and the
assigned pass produce. In the worked example the raw leads run from −45.2 s to
−37.3 s, so the song entries are 0–42.1 s, not bar multiples, and all six loop
anchors land at 79.396 s.

### 1d. Checks

1. **Section boundaries.** Prefer actual silence gaps. If the file is
   continuous, use novelty peaks and clear changes in timbre/level.
2. **Fundamental vs harmonics.** A strong peak at 2L, 3L or 4L is often a
   harmonic of the loop, not the loop. Compare onset ACF, mel lag similarity and
   band ACF. Prefer the shortest plausible peak consistent with the structure.
3. **Nested periods.** A low band may repeat at half the full loop while a high
   layer repeats only at the full loop. Choose the full loop when the region's
   high-level content requires it.
4. **Cycle count.** Check that `(end - start) / loop_length` is close to an
   integer. A remainder is normal at the end and is excluded: the pattern ends
   at the last full loop. A region of fewer than three full segments is not a
   pattern.
5. **Bar multiple.** Express the loop as an integer number of bars.
6. **Pass placement.** Confirm the assigned pass is the one with the largest
   temporal overlap, and record the raw lead and the shifted entry.
7. **Confidence.** Record whether the loop is clear, plausible, or ambiguous,
   and record the mean similarity. A useful tolerance is a few tens of
   milliseconds for the loop estimate.

Write down a table before building anything:

| File | Pattern | Pass | Start (s) | End (s) | Loop (s) | Bars | Lead (s) | Entry (s) | Segments |
|---|---|---|---:|---:|---:|---:|---:|---:|---:|
| palette_2026-11-10 | 1 | 6 | 1.00 | 60.10 | 3.652 | 2 | −45.194 | 34.201 | 16 |

## 2. Build the report specification

Create a JSON spec. Paths in `file` are relative to `--src` unless absolute.
`start` is the measured pattern start; `end` is a search bound that the builder
truncates to the last complete segment. Pass the section's `bound` from the
analysis helper as `end`, not its already-truncated `end`: rounding those to
milliseconds can land a hair short of the last segment boundary and silently
cost the region its final segment through `int((end - start) / length)`.

```json
{
  "title": "Analyze Palettes",
  "description": "Sections with pass-derived lead-ins.",
  "reports": [
    {
      "file": "palette_2026-11-10.wav",
      "patterns": [
        {
          "start": 1.00,
          "end": 60.10,
          "length": 3.652,
          "bars": 2,
          "name": "Pattern 1",
          "color": "#4dabf7"
        }
      ]
    }
  ]
}
```

`start`, `end` and `length` are seconds. `bars` is informational for the card.
`name` and `color` are optional. The builder finds the passes file beside the
audio, assigns each pattern to its maximum-overlap pass, and records `pass`,
`passStart`, `passLead` and `entry` in `summary.js`.

Put **every** palette of the archive in the same spec: all of them go into one
report folder and one song. `song` belongs to the whole song, so with several
palettes place it at the top level of the spec, beside `reports`; a
single-report spec may keep it inside its report object. `targets` has one
value per pattern across all palettes, in report order.

Build:

```bash
python3 pattern_report_builder.py \
  --spec report-spec.json \
  --src /path/to/audio \
  --out artifacts/<batch>/pattern_reports
```

The builder fails if the passes file is missing. It does not reject a measured
start for being off the pass loop grid, because pattern starts are measured, not
chosen from the passes. A palette with no region of at least three full segments
is still a report with `patterns: []`; it shows its waveform and contributes no
stems to the song. Into `[palettes]/` it writes a new palette rather than copying
the source: a mono 16-bit WAV at the source rate that lays the palette's stems
back to back, and a passes file whose loop points are the seams between those
stems, the first at 0.0. Each stem's silent `entry` padding goes into that pass's
pad instead of the samples, so reading the pad places the stem without the file
growing by the padding. The stems carry no gain or edge ramps, so each is its
pattern's raw region once. A one-second silent buffer follows each stem and
belongs to no stem: it clears the analyzer's 0.5 s / 0.1 s silence grid with
room to spare, so re-analyzing the compact palette finds the patterns again
instead of one long region. Its passes file opens with `# bar <seconds>`, the
bar the stems were cut on; a re-analysis uses that bar instead of estimating
one from the one-pass-per-stem grid, starts a section on a pass loop within
0.2 s of it, and lets a section shrink by 0.2 s before it loses a segment.
That is what makes the round trip return the patterns the palette was built
from, at the same lengths and the same entries. The builder also writes the source's mono PCM to
`[data]/<audio-basename>/<audio-basename>.wav`, which is what the report player
uses, so a report still plays from the packaged zip without the extracted
source. Beside the copy it writes the original passes file, as
`[data]/<audio-basename>/<passes-basename>.txt`, so the loop placement travels
with the audio. When the source has a sounds file, its original is copied there
too, and a shifted copy is written beside the generated palette and passes: a
mark inside a pattern moves to that pattern's seam plus its offset into the
pattern, and the sound in force at a pattern's start is carried to its seam, so
every time matches the compact WAV.

Output layout:

```
artifacts/<batch>/
  pattern_reports/
    index.html
    <audio-basename>.html
    [palettes]/<audio-basename>.wav
    [palettes]/<passes-basename>.txt
    [palettes]/<sounds-basename>.txt   (only with a sounds file)
    [data]/<audio-basename>/
      <audio-basename>.wav
      <passes-basename>.txt
      <sounds-basename>.txt            (only with a sounds file)
      summary.js
      l8_*.js
      l64_*.js
      l512_*.js
```

`[data]`, `[palettes]` and `[stems]` are percent-encoded as `%5Bdata%5D`,
`%5Bpalettes%5D` and `%5Bstems%5D` in every URL (`src`, chunk requests, audio
files) and kept literal on disk.

The builder validates the resident coarse min/max data and the first fine
chunk against the PCM before finishing.

## 3. Build the song from the patterns

A song is layered from the same patterns the reports show, and every pattern
used in it is used in its entirety. A stem is the pattern's whole measured
region, first sample to last, placed once: no segment is dropped, no portion is
silenced, gated or faded out early, and the region is never tiled or looped to
fill more time.

One song is built per archive, never per palette. If the archive holds several
palettes, every pattern of every palette becomes a stem in the same master; do
not build a separate song for each palette and do not leave one out. If the
folder holds several archives, they are separate batches and separate songs;
never combine patterns from different archives. An archive with no qualifying
pattern still gets its own report (`patterns: []`), but has no song page or
master.

Patterns that share a sound are not layered twice. After placement, patterns are
kept in report order; a pattern whose sound has already been used is dropped
from the stems and master when its span overlaps a kept stem of that sound, and
kept when it does not overlap one. `null` sounds never collide.

The passes file decides where each pattern begins relative to the common song
anchor. `summary.js` stores each report's local shifted lead as `entry`, but
`song_builder.py` recomputes placement from `start - passStart` across every
pattern in every palette, using one global shift. Every pattern's assigned pass
loop point lands at the same song time, the anchor; the pattern with the
smallest lead starts at 0. There is no manual `starts` list, and adding
`song.starts` to the spec is an error. Entries are seconds, not bars.

Add a `song` block for levels only (at the top level of the spec when the archive
holds several palettes):

```json
"song": {
  "targets": [-20, -20, -20, -20]
}
```

- `targets`: pre-master RMS target in dBFS per pattern across every palette,
  in report order, default -20.
- Patterns may differ in segment length and bar length; the builder places each
  stem at the entry that puts its loop anchor on the common anchor, and needs
  no common grid.
- Song length is fixed by the patterns, not chosen: it is the furthest entry
  time plus that pattern's whole region, across every palette.

```bash
python3 song_builder.py --spec report-spec.json \
  --report-dir artifacts/<batch>/pattern_reports \
  --archive 'downloads/[2026-11-12-19-7-9].zip'
```

`--archive` is the zip the palette and passes were extracted from. Its stem names
the master WAV and the link in `song.html`; omitting it writes
`finished_song.wav`. The builder reads the report's `summary.js` and the
`[palettes]/` WAV it describes, cutting each stem at its seam and adding the pass
pad to its lead, so it uses exactly the patterns, pass loops and segment grid the
report shows. Each stem's offset is its own loop point in the generated
palette's passes file, so the silent buffer after a stem is never read.

Output:

```
artifacts/<batch>/
  <archive-stem>.wav
  pattern_reports/
    song.html
    [stems]/
      pattern_1.wav ...
      [data]/arr.js  a4096.js  a512.js  a64.js
```

`song.html` reaches the master with `../<archive-stem>.wav`, URL-quoted, so the
report folder and the master must stay siblings. One master is written per
archive; with several palettes `[stems]/` holds one file per pattern across all
of them, in report order: the first palette's patterns, then the next
palette's.

Guarantees:

- every pattern of every palette is a stem unless an overlapping same-sound
  repeat was dropped; no palette is skipped;
- every stem's assigned pass loop point lands at the same song anchor;
- each stem is the whole measured region, once, from first sample to last,
  with no truncation, muting, gating or tiling; the only attenuation is a 4 ms
  anti-click ramp at each end of the region;
- stems are trimmed to their targets, then share one master scale;
- the master is written as the exact sum of the quantized stems, so the stems
  reconstruct it sample for sample and the builder's sum check is 0 LSB;
- there is no master fade in or out, so no pattern's tail is faded early;
- the index gets a Song card;
- peak levels load as script tags, so `song.html` works from `file://`.

The page plays the master or all stems in sync, seeks, mutes individual stems,
and draws one arrangement row per stem. Drag horizontally on a row to zoom,
right-click to zoom out, Reset Zoom to return, and click to jump and play. Gray
vertical guides mark the other pass loop points; a bold white line marks the
shared song anchor on every row.

Song-level checks:

- [ ] Builder exits 0; printed stem delta is 0 LSB; clipped is 0.
- [ ] Stem count is the report pattern count minus same-sound repeats that
      overlap an earlier stem; every palette still contributes.
- [ ] Each stem's audio is exactly its pattern's measured region
      (`segments × length`) once, beginning at its pass-derived entry.
- [ ] All stem rows draw in `song.html`, silent before their entry.
- [ ] A deep zoom loads a finer `a*.js` level and Reset Zoom restores the view.
- [ ] Click-to-seek starts playback; mute toggles one stem.

## 4. Memory-safe waveform design

Do **not** inline the whole PCM or a dense waveform in the HTML. The report
uses a coarse resident tier plus on-demand min/max chunks:

- `summary.js` contains `window.PATTERN_META` and a base64 int16 coarse
  min/max array at step 4096. This is the only waveform data loaded initially.
- Fine tiers are min/max pairs at steps 8, 64 and 512 samples, written in
  chunks of 65,536 bins.
- On render, the client chooses the finest tier satisfying
  `visibleSamples / step <= width * 4`, and requests only the visible chunks.
- Chunks are cached with a 192 MB byte budget; oldest entries are evicted first.
- If the desired tier is not resident, rendering falls back to a coarser level.
- The full view therefore loads zero `l*.js` chunks. Zooming progressively
  loads `l512`, then `l64`, then `l8`.

Keep this invariant when modifying the report. A full view must remain usable
with only `summary.js`.

## 5. Interaction requirements

The report must provide:

- full-file waveform and a colored band for each pattern;
- pass loop points from the passes file drawn as dashed gray guides with
  `P<index>` labels on the waveform, and as dashed guides inside the pattern
  rows;
- the assigned pass loop point in each pattern row drawn as a bold white
  anchor, with a white edge label when it lies outside the source timeline;
- a row per pattern with one numbered cell per full segment;
- clickable segment chips that jump to the segment start and play;
- drag horizontally to zoom into a time range;
- right-click to zoom out one level; a Reset Zoom button;
- click the waveform to seek and play;
- a red playhead that follows `audio.currentTime` via `requestAnimationFrame`;
- a native `<audio controls>` element using the `[data]/` copy;
- segment grid lines thinned when spacing is under ~2.5 px;
- an index page linking all reports.

Audio seeking requires a server that supports HTTP byte ranges. Use the
bundled server, not a plain static server:

```bash
python3 pattern_report_server.py --root artifacts/<batch> --port 8000
```

Serve the batch folder holding both `pattern_reports/` and the master WAV; the
reports are then at `http://127.0.0.1:8000/pattern_reports/index.html`. It
binds `0.0.0.0` and answers `Range` requests with `206 Partial Content`.
Verify:

```bash
curl -s -D - -o /dev/null -H 'Range: bytes=0-99' \
  http://127.0.0.1:8000/pattern_reports/%5Bpalettes%5D/example.wav
```

Expect `206` and `Content-Range`.

## 6. Validation checklist

Builder-level:

- [ ] Command exits 0.
- [ ] `summary.js` parses; duration, sample rate, pattern count and segment
      count match the spec.
- [ ] Every pattern has `pass`, `passStart`, `passLead` and `entry`; `entry` is
      the raw lead shifted by that report's minimum lead.
- [ ] When a song is built, `arr.js` has one `anchor`, equal to the global
      shift; every entry is `start - passStart + pad + anchor`, so pads move a
      stem without moving the anchor.
- [ ] Every report row shows its assigned pass anchor in white; every song stem
      row shows the shared anchor in white and the other pass loops in gray.
- [ ] Every pattern's segments are exactly `length` long; no partial tail
      segment exists.
- [ ] Coarse min/max matches the WAV at sampled bins (the builder asserts a
      few bins).
- [ ] `[data]/<audio-basename>/l8_0.js` exists and its first bins match the PCM.
- [ ] `[data]/<audio-basename>/<audio-basename>.wav` exists and every report's
      player resolves to it inside the packaged folder, with the original
      `<passes-basename>.txt` beside it, plus `<sounds-basename>.txt` when the
      source had one.
- [ ] `[palettes]/` holds a generated WAV of the palette's stems in order and
      a passes file giving their seams as loop points, 0.0 first, with each
      stem's entry as its pad, and each stem is followed by the one-second
      silent buffer that keeps a re-analysis sectioned. A source sounds file is
      written beside them with every mark shifted onto the compact timeline.
- [ ] When a song is built, the stem sum check passes and the song page loads
      its arrangement data.
- [ ] No pattern region was reduced to a handful of bars to raise its mean
      similarity.

Browser-level, when a browser is available:

- [ ] Initial full view loads `summary.js` only; no `l*.js` requests.
- [ ] Drag zoom narrows the view and eventually loads a finer chunk.
- [ ] Right-click zoom-out and Reset Zoom return to previous/full views.
- [ ] A segment chip sets `audio.currentTime` to that segment's start and
      starts playback; the playhead is visible at the correct position.
- [ ] Pattern Play starts at the pattern start.
- [ ] No console errors and no failed chunk/audio requests.
- [ ] The index page opens each report.

If the browser automation wrapper errors, use the available DOM/evaluate and
click tools instead of claiming the interaction was tested. A screenshot is
not proof of playback or seeking.

## 7. Packaging

```bash
cd artifacts/<batch>
zip -r analyze-palettes.zip '<archive-stem>.wav' pattern_reports \
  -x 'pattern_reports/analyze-palettes.zip'
unzip -t analyze-palettes.zip
```

The archive root holds the master WAV beside the `pattern_reports/` folder.
Include the index, all HTML reports, the generated `[palettes]/` WAVs, passes
and sounds files, the `[data]/` chunks and audio copies, and when a song was built,
`song.html` plus
`[stems]/` (stems and `[stems]/[data]/`). Several palettes change none of this:
they share the folder and the one master. Package each archive's batch
separately when the folder held more than one archive. Do not include the zip
inside itself. Report the final size and entry count.

### 7a. Upload it

```bash
python3 drive_upload.py analyze-palettes.zip --name '<archive-stem>-analyze-palettes.zip'
```

It refreshes an access token, finds the `[processed]` folder beside `[palettes]`
(creating it when missing), uploads the zip as a resumable upload and prints the
file link. Credentials are read from `GOOGLE_CLIENT_ID`,
`GOOGLE_CLIENT_SECRET` and `GOOGLE_REFRESH_TOKEN` in the environment; keep them
out of the repo, the spec and any command line. With the three variables absent
it prints `UPLOAD SKIPPED` and the batch still counts as packaged. Never upload
into `[palettes]`: step 0 treats every zip there as a new batch.

## 8. Worked example

**`palette_2026-11-10-18-16-29.wav`**, 609.10 s mono 44.1 kHz, with 29 passes.
The silence sections are 1.00–60.10, 69.10–182.10, 184.00–289.70, 291.80–293.30
and 304.80–574.80 s. The 1.5 s fourth section is too short; the 270 s final
section splits into three where its loop changes. Every pass is an integer
number of 1.826 s bars.

| Pattern | Pass | Start–End (s) | Loop (s) | Bars | Lead (s) | Entry (s) | Segments |
|---|---|---:|---:|---:|---:|---:|---:|
| 1 | 6 | 1.00–60.10 | 3.652 | 2 | −45.194 | 34.201 | 16 |
| 2 | 14 | 69.10–182.10 | 3.688 | 2 | −79.395 | 0.000 | 30 |
| 3 | 19 | 184.00–289.70 | 7.300 | 4 | −70.430 | 8.965 | 14 |
| 4 | 22 | 304.80–380.00 | 7.300 | 4 | −37.315 | 42.080 | 10 |
| 5 | 24 | 380.00–470.00 | 7.288 | 4 | −38.822 | 40.573 | 12 |
| 6 | 26 | 470.00–574.80 | 5.451 | 3 | −40.134 | 39.261 | 19 |

Pattern 2 has the lowest raw lead (−79.396 s), so it defines the song start at
0; the others enter 8.965, 34.201, 39.261, 40.573 and 42.080 s in. All six loop
anchors land at 79.396 s. An archive holding several palettes shares one global
shift, so all their patterns land on the same song anchor. The song is
142.83 s (78 bars), peak −0.5 dBFS, RMS −17.8 dBFS, 0 clipped; all six stems
sum to the master with 0 LSB, and each stem is silent before its entry and after
its region.

## 9. Pitfalls and lessons

- An archive with several palettes still makes one song. Do not build a song
  per palette, and do not keep one palette and discard the rest.
- Multiple zip archives in My Drive/[palettes] are separate batches and songs.
  Do not combine their reports or stems. A zero-pattern archive has no song page.
- Only direct, non-trashed children of My Drive/[palettes] are inputs. Never
  process trash, shortcuts, other folders, or duplicate copies.
- Passes place patterns; they do not define them. Do not force a pattern start
  onto a pass loop point. Doing so makes every lead-in zero and destroys the
  measured placement.
- Do not round the lead-in to bars. The song builder starts stems at exact
  anchor-relative seconds.
- A negative raw lead-in is normal. Shift all leads by the global minimum
  across every palette; never clamp individual leads to zero before shifting.
- Do not shift each palette independently. That puts their loop anchors at
  different song times; one global shift aligns them.
- A passes file is required. Do not guess a placement or use a staggered
  default when it is missing.
- `song.starts` is gone. Entries come from the passes file; do not reintroduce
  a manual start list or a staggered plan.
- The pass duration is not necessarily the loop length. A 26-bar pass can hold
  a 3-bar loop; measure the loop inside the region.
- Do not name a helper `inspect.py`; it shadows the standard library.
- `scipy.io.wavfile` handles WAV; convert other formats first.
- The analysis helper's STFT memory grows with duration. For very long files,
  analyze sections separately or downsample; the report data is chunked.
- ACF peaks are not segment boundaries. A strong peak can be a harmonic or a
  half-time layer. Cross-check with other bands and the novelty curve.
- Do not gate pattern candidates on the fraction of frames above a strict
  similarity. Steady textures and fade tails score near 1.00 at every lag.
- Do not shrink a region to the bars with the highest similarity. A region of
  ten to thirty segments that loops approximately is the pattern a listener
  hears; three bars of it are not.
- The reports use a uniform grid within each pattern. Do not describe the grid
  as measured per-segment timing, and do not assume two patterns share a grid.
- With several stems, independently rounded int16 stems can differ from a
  separately rounded master by a few LSB. Write the master as the clipped sum
  of the quantized stems so the reconstruction is exact.
- The master is named after the source archive, not after a fixed
  `finished_song.wav`. Only audio that arrived outside an archive falls back to
  that name.
- The report server must support Range; otherwise WAV seeking can fail.
- A chunk file holds `CHUNK_BINS` bins, which is `2 * CHUNK_BINS` interleaved
  int16 values. Slice the interleaved min/max array by `2 * CHUNK_BINS`.
- If a preview host rejects plain curl with 403, that may be the preview's
  authentication layer, not a broken app. Check loopback and use the preview
  panel.
- A song may not repeat a pattern to fill time or gate one out of part of the
  song: that stops it being heard whole.
- Keep the zip out of itself. Re-run the zip after adding a download link if
  the link target must be inside the served folder.
