***Agents asked to use the analyze-palettes skill should ignore this document and everything in this directory entirely, using the algorithms as they exist in the skill itself.*** This directory is only for calibrating those algorithms beforehand. Agents tasked with doing this will be explicitly asked to work within this directory. 

## Definitions

- **Segment**: one iteration of a pattern — the audio between two adjacent
dashed lines. Every segment in a pattern is the same length, but different patterns can have different segment lengths, not necessarily multiples of each other. The dashes mark that uniform grid; they are not
measured beat positions.
- **Pattern**: a region that repeats at one fixed loop length **for at least three
segments**, running from its measured start to the end of its last complete
segment. The rectangle bounds it. Patterns are derived by tracking some distinct fundamental element that repeats in the same place in the segment, segment by segment. This could be an identifiably unique onset or a specific pitch that occurs in the same position in the segment. Patterns are independent of each other;
two patterns can share a segment length and still count separately because they are tracking different contents in their repetitions, i.e. an onset in one position of the segment (and/or timbre or pitch) versus another.
- **Phrase**: the full extent of the audio from the beginning to the end of all
of its overlapping patterns. 
- **Palette**: a full jam session recorded beginning to end with no editing. Palettes are analyzed for their patterns and have their phrases extracted before building the final song.
- **Stem**: a single instrument track of the final song. Stems are not relevant during this analysis step and only come into play later when the song is constructed.

## Purpose

Outlined in this document are calibration tests which the algorithm implemented in the analyze-palettes skill must pass. The goal is for that algorithm, whatever form it takes, to identify the exact segments, patterns and phrases detailed below from the given test_phrase.wav files also residing in this directory. In doing so the analysis should treat those test_phrase.wav files as if they were real palettes. 

Each test phrase below will have a key with the "correct" stats that should be derived from the audio. Floating point values such as segment lengths etc. do not need to match absolutely perfectly but they must be close. A discussion will also follow as to why the values in the key are the desired ones. It is essential to understand the intent and the purpose of these algorithms in order to build methods that will generalize to any palette seen in the future, and not just resort to any short-sighted solution that happens to generate the correct answers. 

## Reading the diagram

- The gray trace is the same waveform drawn once per pattern, so every pattern
  has its own row against the shared time axis. The waveform is a single mono recording; every row comes from the same audio and nothing here is a stack of separate stems.
- The **outer rectangle** is the whole pattern: from its measured start to
  the end of its last complete segment.
- The **dashed verticals** divide the row into segments, one per loop
  iteration, at the pattern's loop length.
- The row label gives a subjective description of the element being tracked (these are by no means standard and could be anything that helps orient the listener to the pattern evident) and `loop × segments`.
  
# test_phrase_1.wav

![test_phrase_1](test_phrase_1.png)

## Results 

| Tracked element | Segment length (s) | Segments | Region (s) |
|---|---:|---:|---|
| bass | 1.07 | 73 | 0.00–78.11 |
| cymbal | 2.15 | 35 | 2.15–77.40 |
| kick | 2.15 | 35 | 2.15–77.40 | 
| kick | 2.15 | 6, 5 | 2.15–15.05, 38.65–49.40 | 

## Discussion 

1. **Unique tracked elements.** The most fundamental driver of any pattern is the unique element it is tracking. Again, these descriptions here (e.g., bass, etc.) are purely subjective. The point is that each are uniquely characterizable by things like their onset, their timbre, their pitch, etc. For example, the bass and kick are both low frequency spectrally, but the kick has a much more staccato envelope and so both are distinct. Once a fundamental element has been characterized, it becomes a tracked pattern if it repeats at regular intervals throughout some portion of the recording.
2. **Even more defining than the tracked element itself is its position within the segment.** For example, we see there are two tracked elements labeled "kick". These are actually the exact same drum sample but they occur in different places within the segment length and therefore constitute distinct patterns. The first occurs on the downbeat and the second when it occurs is always on its own syncopated beat.
3. **A single tracked element can have non-contiguous patterns.** We can see that the second, syncopated kick occurs in two separate patterns throughout the phrase. The segments of the patterns must be contiguous but any break in them can divide it into multiple patterns without issue. Of note here is that there could actually be considered a third pattern in this row at approximately 67 seconds to 69 seconds. However, that pattern has been excluded because it is only one segment length long and all patterns are required to be at least three segments long.
4. **A distinct element deserves a distinct pattern, even if that pattern is shared exactly by another element.** Here you can see that the cymbal and the downbeat kick have the exact same pattern and segments but are still listed as separate patterns. In a more experimental or chaotic recording two elements might blend together, losing distinction and therefore could be seen as a single pattern, which would not be wrong. This recording however is crisp and clear and the two can be heard as distinctly separate and so two separate patterns are produced. 
5. **Starts are measured, not shared.** The bass begins at 0.00 s and the other
   two at 2.15 s, so the first 2.15 s hold the bass alone. Pattern starts come
   from the audio; they are not snapped to a common grid or to a pass loop
   point.
6. **Each pattern has its own uniform grid.** The bass divides every 1.07 s from
   0.00 s, the cymbal and kick every 2.15 s from 2.15 s. No grid is imposed across patterns; in reality, the exact start position of a pattern within its segment for a given element is arbitrary, but since all patterns come from the same audio, it can be assumed that their borders should align where sensible. For example, the bass pattern has two preceding segments, but the cymbal when it starts begins exactly aligned with that third bass segment.
7. **The cycle count is an integer and the remainder is dropped.** 73 × 1.07 =
   78.11 s and 35 × 2.15 = 75.25 s. The recording is 78.9 s, so 0.79 s and
   1.50 s of tail sit outside the regions: a pattern ends at its last complete
   segment

# test_phrase_2.wav

![test_phrase_2](test_phrase_2.png)

## Results

| Tracked element | Segment length (s) | Segments | Region (s) |
|---|---:|---:|---|
| D chord | 1.34 | 4, 3, 3 | 0.73–6.09, 11.54–15.56, 22.14–26.16 |
| E chord | 1.34 | 4, 3, 3 | 6.13–11.49, 16.86–20.88, 27.82–31.84 |
| D chord | 1.58 | 3 | 33.11–37.85 |
| pad gate | 1.34 | 3, 3, 3, 3 | 1.40–5.42, 6.80–10.82, 12.21–16.23, 17.53–21.55 |

## Discussion

1. **A tracked element can be a pitch rather than an instrument.** This palette
is built from sustained synth voices rather than drums, so timbre cannot
separate its chord patterns. The pitch does: the D chord is built on 293 Hz with
partials at 583, 883 and 1174 Hz, and the E chord on 330 Hz with partials at
657, 990 and 1313 Hz. Same gesture, different content in the segment, therefore
two elements and two patterns. This is the rule from test_phrase_1 that a
distinct element deserves a distinct pattern even when another shares its region
exactly.
2. **The off-beat strike is a second voice, not a softer chord.** Every chord
strike is followed about two thirds of a second later by another strike.
Measured 60–170 ms after each, the chord's upper partials sit 24 dB above the
F#3 line (183, 366 and 549 Hz) on the accent, while on the off-beat the F#3 line
sits 6 dB above them. The F#3 envelope and the chord-upper envelope correlate
−0.25 to −0.57 at zero lag across the four middle sections and turn positive
only at a lag of ±0.6–0.8 s, half a loop either way: two voices in anti-phase,
not one voice at two levels. The pad is the F#3 one, and it is the third tracked
element.
3. **That voice runs on the same segment, half a segment late.** Its strikes sit
0.6–0.8 s from the chord's, either side of the half-way point of 1.34 s, so the
pad takes the chord's grid rather than a loop of its own: 1.40, 6.80, 12.21 and
17.53 are 0.73, 6.13, 11.54 and 16.86 shifted by half a segment. Sharing the
loop is what makes the two rows nest instead of sliding: every pad box opens one
segment into its chord box and closes one segment before that box ends. This is
the case where imposing one grid across patterns is correct, because the audio
puts both voices on the same pulse.
4. **A single element can have non-contiguous patterns.** The chords alternate
every eight strikes, four loops or about 5.3 s: D, E, D, E, D, E, D. Neither
chord's pattern is contiguous, because each is interrupted by the other chord,
and every interruption starts a new span exactly as the breaks in the syncopated
kick row do. The pad is cut the same way, once per chord span.
5. **A pattern has one loop length, so a slower section is a new pattern.** The
last D chord is gated more slowly: its accent loop measures 1.58 s where every
earlier chord measures 1.34 s, and its strikes land at 33.11, 34.69, 36.27 and
37.85. The voices also stop trading there, correlating +0.43 at zero lag, so the
pad's pattern is not claimed past 21.55 s and the closing section stays a single
pattern.
6. **Starts are measured, and a shared loop keeps them related.** The seven
chord spans begin at 0.73, 6.13, 11.54, 16.86, 22.14, 27.82 and 33.11 s, each
the first accented strike of its chord. The pad's four spans begin half a loop
later, and its own attacks land there: 1.47, 6.92, 12.13 and 17.53 s against the
1.40, 6.80, 12.21 and 17.53 s grid. Nothing is snapped to a common grid for its
own sake; the offset is the measured phase between the two voices.
7. **The cycle count is an integer and the remainder is dropped.** 4 × 1.34 =
5.36 s (0.73–6.09), 3 × 1.34 = 4.02 s (1.40–5.42) and 3 × 1.58 = 4.74 s
(33.11–37.85). The recording is 38.9 s, so the 1.07 s tail after 37.85 and the
sliver left at each chord change sit outside the regions: a pattern ends at its
last complete segment. The pad gets three segments where its chord gets four,
because starting half a segment late leaves no room for a fourth before the next
chord.
