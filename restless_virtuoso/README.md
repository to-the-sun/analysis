# restless_virtuoso

Plays selected ASIO input channels, or the audio files in a folder, on selected ASIO
output channels, for monitoring an audio interface at ASIO latency. MIDI input can
be played through a built-in sine synth, drum samples or a VST instrument and/or
forwarded to a MIDI output.

The driver's own ASIO interface is used directly, so no ASIO SDK download, PortAudio
or WASAPI is involved and no non-ASIO device can be selected.

## Build

```bash
make -C restless_virtuoso   # x86_64-w64-mingw32-gcc on PATH
```

Or build with the same source in Visual Studio / MSVC as a console project linked
against `winmm.lib` and `shell32.lib`.

## Usage

```bash
restless_virtuoso.exe --list
restless_virtuoso.exe --driver "Focusrite USB ASIO" --in 1,2 --out 1,2
restless_virtuoso.exe --in 1 --out 1,2 --buffer 128 --rate 48000
restless_virtuoso.exe --in none --midi-in "Keystation" --midi-out 0
restless_virtuoso.exe "Focusrite USB ASIO Input 1.json"
```

- Without `--driver`, `Focusrite USB ASIO` is loaded when installed; otherwise the first installed driver is used.
- `--in` / `--out` take 1-based channel numbers (`1,2`, `1-4`, `3,5-6`), default `1,2`.
  Output channel `j` plays input channel `j`, wrapping if there are more outputs than inputs.
- `--buffer` picks the ASIO buffer size; omitted, the driver's preferred size is used.
- `--rate` is validated with the driver; omitted, the driver's current rate is used.
- `--midi-in` accepts a device index from `--list` or an exact device name.
- `--midi-out` also accepts an index or name and forwards incoming MIDI unchanged.

## Settings file

Drop a JSON file onto `restless_virtuoso.exe` (or pass it as the first argument)
to start from it:

```json
{
  "instrument": "Focusrite USB ASIO",
  "input-channel": 1
}
```

A folder dropped onto `restless_virtuoso.exe` (or passed as the first argument) is
played directly, as if it were the `instrument` key, and may be longer than
`MAX_PATH`.

`instrument` is an ASIO driver or MIDI input name from `--list`, or the absolute path
of a folder. A folder's audio files play together as stems and loop when the longest
one ends. `input-channel` is the 1-based ASIO input, or the MIDI channel for a MIDI
device, whose messages are then streamed to the console.
`Focusrite USB ASIO Input 1.json` is a ready template; settings files are named after
the input device, with its exact capitalization.

When `instrument` is a MIDI input, two more keys add instruments to it:

```json
{
  "instrument": "Keystation",
  "input-channel": 1,
  "samples_path": "C:\\samples",
  "vsts_path": "C:\\VSTs",
  "samples_ratio": 50
}
```

- `samples_path` loads every audio file under the folder and its subfolders as drum
  samples. Each of the 128 MIDI notes gets one by the taste lottery, and a note-on
  plays it.
- `vsts_path` loads every 64-bit VST 2 `.dll` under the folder and its subfolders,
  then plays a preset drawn by the taste lottery from one pool holding every preset
  of every plugin. Plugins without factory presets keep their default. A plugin is
  opened once and kept open, inactive while another sound plays, so a sound change
  only switches the preset of an instance that is already there.
  `vsti_stats.txt` in that folder supplies the real preset count and MIDI range for
  each plugin:

  ```json
  {
    "Serum.dll" : { "range" : [ 24, 96 ], "presets" : 128 },
    "Vital.dll" : { "range" : [ 0, 127 ], "presets" : 300 }
  }
  ```

    Each plugin's range is printed as it loads, and a plugin with no entry is named in a
  warning and left at 0-127. Entries are named by their `.dll` file name or the
  plugin's own name, case-insensitive. `presets` is how many presets the plugin
  actually has, and
  random presets are drawn from it instead of the count the plugin reports. `range`
  is the MIDI notes the plugin handles: notes outside it wrap around into it, and the
  range is taken inward to a whole number of octaves so a wrapped note keeps its pitch
  class. Every
  `new sound` on a VST also shifts all its notes by a random -6 to +6 octaves before
  that wrap, enough to move middle C anywhere in 0-127. `vst_stats.txt` is read when
  `vsti_stats.txt` is not there.
- `samples_ratio` is the percentage chance, 0 to 100 (default 50), that a new sound
  comes from `samples_path` instead of `vsts_path` when both are loaded. Only one of
  them plays, and MIDI reaches only that one: note-ons trigger drum samples only
  while a drum sound is drawn, and reach the plugin only while a VST preset is.
- `taste` holds, in whole seconds, how long each drum sample and each preset of
  each plugin has sounded above -60 dBFS. A second counts in full if the sound
  peaked above the gate anywhere in it. Every entry starts at 1; `new sound` draws
  by weighted lottery with one ticket per second of taste, from one pool of every
  preset of every plugin, and once per MIDI note over the drum samples. Samples are
  keyed by their path under `samples_path`, presets by their 1-based number under the
  plugin's file name, e.g. `Dexed.dll`. The instance writes the session's totals back
  into the settings file when it closes.

The sine synth stays off in this mode; the loaded instrument is the sound source.
A key that is not an existing folder is logged and ignored.

A folder is mixed onto the selected outputs with no ASIO input monitoring unless
`--in` is given. Files of any channel count play on all outputs, wrapping when there
are more outputs than file channels, and each file's sample rate is converted to the
driver's rate. `.wav`, `.wave`, `.aif`, `.aiff`, `.flac`, `.mp3`, `.ogg` and `.oga` files are
picked up, and each one is decoded as WAV, AIFF, FLAC, MP3 or Ogg Vorbis by its content
rather than its extension.

## Loop recordings

The instance playing a folder sends a loop start cue to every other running instance
when its stems start and each time they loop. An instance started while a stems
instance is already playing records at once, and its first pass holds the negative of
the milliseconds since that stems instance last looped, so it is never further back
than one loop of the stems; every later pass is the milliseconds since the recording
instance started. An instance started before the stems instance begins at the palette's
first cue instead, its first pass at zero. The take, a
mono mix of the outputs, streams into the `[palettes]` folder beside the stems folder as
`palette_YYYY-MM-DD-HH-MM-SS.wav`, 16-bit PCM, and
`passes_YYYY-MM-DD-HH-MM-SS.txt` with one `index, milliseconds 1.;` line per pass. The
take ends, and its files are completed, when the stems instance exits or the recording
instance is closed. Closing with the console's own close button is handled too, since
the program waits for the files before it lets Windows end it.

Every take also gets a `sounds_YYYY-MM-DD-HH-MM-SS.txt` beside it. An instrument take
records one `index, milliseconds number mode;` line for the sound it started with and
each `new sound`, timed from the take's start; `mode` is `samples` for a drum sample
set and `synth` for a VST preset. Every running instance shares one numbered list of
sounds, so a number identifies a drum sample set or VST preset in every take, and the
console prints the number with the description. A take with no sound applied holds
the single line `0, 0.000000 other;`.

Beside `[palettes]` the song has a `song.json`, holding every shared sound keyed by its
number (`"3": "16 drum samples, seed 41290318"`) and the number of the sound each
instrument was last on. It also holds a `"stems"` object with each stem's lead-in in
seconds (`"3 16 drum samples.wav": 1.250000`). Every running
instance writes it when its sound changes and when it closes, so the order the
instruments and the stems instance are closed in makes no difference, and an instrument
that has already closed still has its sound saved. An instance started on a song with a
`song.json` takes those sounds as the numbering, replacing whatever list the session
already held, and sets every instrument to its saved sound: instruments that are already running take it on when the stems instance
starts, and instances started later begin on it. A drum sound keeps the seed that draws
its sample set, so it comes back the same.

On startup the program lists every installed ASIO driver, then every MIDI input
device.

In instrument mode, left-click anywhere in the console to move up to the next shared
sound, or to draw a new one when it is already on the highest, and right-click to move
back down to sound 1. A new sound comes from `samples_path` or `vsts_path` by
`samples_ratio`.

## Rating and stems instances

Every instance sends the audio it plays through the cumulative flux analyzer on a
thread of its own, with the 15 s window, four bands and cumulative score history the
offline tools use. An instance playing a folder runs one analyzer per stem it plays;
an instance without one running runs a single analyzer over the audio it plays. The
analyzers of an instance share one group buffer, so their scores accumulate together
and give the instance one rating: the average of the scores from the last 15 s of
audio, rolling. In an instrument instance the group starts again with a new sound, so
the rating does too. The console's last line shows it and replaces it as it changes,
beside a `score`: the last 100 ms of scores, but from the instance's own side only —
its stems when it plays a folder, its own audio when it has an analyzer of it.

An instance playing a folder publishes its rating as the recitation rating. An
instrument instance with one running adds one analyzer per stem that instance plays,
all sharing a second group buffer with its own analyzer, so their scores together give
it a separate jamming rating. It then records while the jamming rating is above the
recitation rating, and its takes are written into that instance's `[stems]` folder.
Closing that instance drops the take in progress and its memory instead of writing it
as a project of its own, and the rating starts again from zero.
With no stems instance running it records while its own rating is above zero, and each
take is written to
`D:\[Library]\[Audio]\[Works]\[Projects]\[YYYY-MM-DD-HH-MM-SS]\[stems]`, after which a
`restless_virtuoso.exe` is started on that folder. A take is named after the sound that
played it, e.g. `3 16 drum samples.wav`, and one recorded while a stems instance runs is
placed from the loop point of the pass it overlaps most, as the song builder places a
pattern in its pass, so the audio lands where it was played in the song. That placement
is a lead-in per stem in `song.json`, not silence in the file: a stem with a lead-in is
cued that far into each loop, a take that mostly lies past a loop point carries a
lead-in of zero and moves the others' lead-ins back with it, and the song's length is
the last lead-in plus its stem, which the console's progress bar follows. A take with
nothing above the noise floor is not written.

An instance playing a folder puts a bar of the loop before its rating and score, e.g.
`[=========---------------] 0:12/0:32 rating 0.000000  score 0.000000`: the play position
in the song over the song's length, in minutes and seconds.

A WAV dropped into a folder that a stems instance is playing joins the others at the
next loop point, aligned with them rather than at the top of the song, and the console
prints its name and how many stems are playing. A stem whose file has gone stops
playing at that point and is named the same way, as `dropped`.

Stems collide as their sounds' modes do: every drum sample set collides with every
other, every soundless take with every other, and a VST preset only with the same sound
number. A take whose active audio overlaps an older stem it collides with deletes that
file, and the stems instance drops it from the loop. For the same reason a stems
instance mutes every stem a sound an instrument instance is playing collides with, so
the live instance is the only voice of that sound, and plays them again when that
instance stops or changes sound.

## Notes

- 64-bit Windows only; ASIO drivers are loaded in-process, so the driver must be 64-bit.
- Supported ASIO sample formats: `Int16LSB`, `Int24LSB`, `Int32LSB`, the 16/18/20/24-bit
  aligned `Int32LSB` variants, and `Float32LSB`. Big-endian formats are rejected.
- ASIO is exclusive: close your DAW before starting the monitor.
- The synth is a 16-voice sine with a short attack/release, mixed onto the first two
  selected outputs; `--no-synth` leaves MIDI input to the MIDI thru path only.
- Changing the sample rate or buffer size in the control panel reopens the buffers
  automatically.
- The master output is scaled by the Windows master volume, re-read while running.
- A MIDI input that fails to open is retried, then reported with the Windows error code,
  its device name and driver version, and whether it opens without a callback.
- Every run writes a new `restless_virtuoso-YYYY-MM-DD_HH-MM-SS.log` into the `[logs]` folder next to the exe with startup stages and errors.
- At startup, the program's logs older than 7 days are moved to the Recycle Bin.
