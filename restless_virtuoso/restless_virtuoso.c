#if !defined(_WIN64)
#error "restless_virtuoso is a 64-bit Windows program"
#endif

#define _WIN32_WINNT 0x0601
#define WIN32_LEAN_AND_MEAN

#include <windows.h>
#include <mmsystem.h>
#include <objbase.h>
#include <shellapi.h>

#include <float.h>
#include <math.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include "asio_abi.h"
#include "dsp.h"
#include "vst_abi.h"
#include "../analysis/cumulative_flux.h"
#define DR_WAV_IMPLEMENTATION
#include "../analysis/dr_wav.h"
#define DR_FLAC_IMPLEMENTATION
#include "dr_flac.h"
#define DR_MP3_IMPLEMENTATION
#include "dr_mp3.h"
#include "stb_vorbis.c"

#define MAX_SELECTED 32
#define QUEUE_FILES 1024
#define MAX_METERS 4
#define MIDI_QUEUE 1024
#define DEFAULT_DRIVER "Focusrite USB ASIO"
#define TASTE_START 1
#define TASTE_GATE 0.001f

typedef struct {
    const char *driver, *in, *out, *midiIn, *midiOut;
    char instrument[MAX_PATH], samplesPath[MAX_PATH], vstsPath[MAX_PATH];
    int rate, buffer, synth, list, inputChannel, samplesRatio, randomScale;
} Options;

typedef struct {
    IASIO *asio;
    char driverName[64];
    int32_t inCount, outCount;
    ASIOSampleRate rate;
    int32_t bufferSize;
    int32_t latencyIn, latencyOut;
    int postOutput;

    int nIn, nOut;
    int inCh[MAX_SELECTED], outCh[MAX_SELECTED];
    SampleFormat inFormat[MAX_SELECTED], outFormat[MAX_SELECTED];
    ASIOBufferInfo bufferInfo[MAX_SELECTED * 2];
    float *inputFloats, *fileFloats, *synthFloats, *mixFloats;

    HMIDIIN midiIn;
    HMIDIOUT midiOut;
    int midiInIndex, midiOutIndex;
    int midiChannel, midiStream;
    int synthEnabled, samplesSound, samplesRatio, soundNumber;
    int buffersCreated, started;

    volatile LONG quit, reset;
    volatile float volume, peak[MAX_SELECTED];
    int lineWidth;
} Monitor;

static Monitor g;
static ASIOCallbacks callbacks;

static FILE *errorLog;
static char logPath[MAX_PATH + 64];

static void recycle_file(const char *path)
{
    char from[MAX_PATH + 66] = {0};
    snprintf(from, sizeof from, "%s", path);

    SHFILEOPSTRUCTA operation;
    memset(&operation, 0, sizeof operation);
    operation.wFunc = FO_DELETE;
    operation.pFrom = from;
    operation.fFlags = FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_SILENT | FOF_NOERRORUI;
    SHFileOperationA(&operation);
}

static void recycle_old_logs(const char *folder, const char *stem)
{
    char pattern[MAX_PATH + 16];
    snprintf(pattern, sizeof pattern, "%s%s*.log", folder, stem);

    WIN32_FIND_DATAA entry;
    HANDLE find = FindFirstFileA(pattern, &entry);
    if (find == INVALID_HANDLE_VALUE) return;

    FILETIME now;
    GetSystemTimeAsFileTime(&now);
    ULARGE_INTEGER cutoff;
    cutoff.LowPart = now.dwLowDateTime;
    cutoff.HighPart = now.dwHighDateTime;
    cutoff.QuadPart -= 7ULL * 24 * 60 * 60 * 10000000;

    do {
        if (entry.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;

        ULARGE_INTEGER written;
        written.LowPart = entry.ftLastWriteTime.dwLowDateTime;
        written.HighPart = entry.ftLastWriteTime.dwHighDateTime;
        if (written.QuadPart >= cutoff.QuadPart) continue;

        char path[MAX_PATH + 64];
        snprintf(path, sizeof path, "%s%s", folder, entry.cFileName);
        recycle_file(path);
    } while (FindNextFileA(find, &entry));
    FindClose(find);
}

static int module_folder(char *folder, size_t size)
{
    char module[MAX_PATH];
    DWORD length = GetModuleFileNameA(NULL, module, MAX_PATH);
    if (!length || length >= MAX_PATH) return -1;

    char *slash = strrchr(module, '\\');
    if (!slash) return -1;

    size_t folderLength = (size_t)(slash - module) + 1;
    if (folderLength >= size) return -1;
    memcpy(folder, module, folderLength);
    folder[folderLength] = 0;
    return 0;
}

static void open_error_log(void)
{
    char module[MAX_PATH];
    DWORD length = GetModuleFileNameA(NULL, module, MAX_PATH);
    if (!length || length >= MAX_PATH) return;

    char *stem = strrchr(module, '\\');
    stem = stem ? stem + 1 : module;
    char *dot = strrchr(stem, '.');
    if (dot) *dot = 0;

    char folder[MAX_PATH + 8];
    char base[MAX_PATH];
    if (module_folder(base, sizeof base) != 0) return;
    snprintf(folder, sizeof folder, "%s[logs]\\", base);
    CreateDirectoryA(folder, NULL);

    HRESULT com = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    recycle_old_logs(folder, stem);
    if (SUCCEEDED(com)) CoUninitialize();

    SYSTEMTIME now;
    GetLocalTime(&now);

    for (int attempt = 1; attempt < 100; attempt++) {
        char suffix[8] = "";
        if (attempt > 1) snprintf(suffix, sizeof suffix, "-%d", attempt);
        snprintf(logPath, sizeof logPath, "%s%s-%04d-%02d-%02d_%02d-%02d-%02d%s.log",
                 folder, stem, now.wYear, now.wMonth, now.wDay,
                 now.wHour, now.wMinute, now.wSecond, suffix);
        if (GetFileAttributesA(logPath) == INVALID_FILE_ATTRIBUTES) break;
    }

    errorLog = fopen(logPath, "w");
    if (errorLog) {
        fprintf(errorLog, "--- restless_virtuoso run ---\n");
        fflush(errorLog);
    }
}

static void log_error(const char *format, ...)
{
    va_list args;
    va_start(args, format);
    vfprintf(stderr, format, args);
    va_end(args);

    if (!errorLog) open_error_log();
    if (errorLog) {
        va_start(args, format);
        vfprintf(errorLog, format, args);
        va_end(args);
        fflush(errorLog);
    }
}

static void log_trace(const char *format, ...)
{
    if (!errorLog) open_error_log();
    if (!errorLog) return;

    va_list args;
    va_start(args, format);
    vfprintf(errorLog, format, args);
    va_end(args);
    fflush(errorLog);
}

static void append_log_line(const char *text)
{
    if (!logPath[0]) return;

    HANDLE file = CreateFileA(logPath, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                              NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) return;

    DWORD written;
    WriteFile(file, text, (DWORD)strlen(text), &written, NULL);
    CloseHandle(file);
}

static volatile LONG initReturned;

static DWORD WINAPI init_watchdog(void *unused)
{
    (void)unused;
    Sleep(10000);
    if (!InterlockedCompareExchange(&initReturned, 1, 1))
        append_log_line("ASIOInit still running after 10 seconds\n");
    return 0;
}

static LONG WINAPI crash_handler(EXCEPTION_POINTERS *info)
{
    const void *address = info->ExceptionRecord->ExceptionAddress;
    HMODULE module = NULL;
    char modulePath[MAX_PATH] = "unknown module";
    char message[512];

    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCSTR)address, &module))
        GetModuleFileNameA(module, modulePath, MAX_PATH);

    snprintf(message, sizeof message, "unhandled exception 0x%08lx at %p (%s+0x%lx)\n",
             (unsigned long)info->ExceptionRecord->ExceptionCode, address, modulePath,
             module ? (unsigned long)((ULONG_PTR)address - (ULONG_PTR)module) : 0);
    append_log_line(message);
    return EXCEPTION_EXECUTE_HANDLER;
}

static volatile uint32_t midiHead, midiTail;
static uint32_t midiQueue[MIDI_QUEUE];

typedef struct {
    const char *name, *pitches;
} Scale;

#define SCALE_NAME 32

static const Scale scales[] = {
    {"major (Ionian)", "C D E F G A B"},
    {"Dorian", "C D Eb F G A Bb"},
    {"Phrygian", "C Db Eb F G Ab Bb"},
    {"Lydian", "C D E F# G A B"},
    {"Mixolydian", "C D E F G A Bb"},
    {"natural minor (Aeolian)", "C D Eb F G Ab Bb"},
    {"Locrian", "C Db Eb F Gb Ab Bb"},
    {"harmonic minor", "C D Eb F G Ab B"},
    {"Locrian natural 6", "C Db Eb F Gb A Bb"},
    {"Ionian sharp 5", "C D E F G# A B"},
    {"Dorian sharp 4", "C D Eb F# G A Bb"},
    {"Phrygian dominant", "C Db E F G Ab Bb"},
    {"Lydian sharp 2", "C D# E F# G A B"},
    {"ultralocrian", "C Db Eb Fb Gb Ab A"},
    {"melodic minor", "C D Eb F G A B"},
    {"Dorian flat 2", "C Db Eb F G A Bb"},
    {"Lydian augmented", "C D E F# G# A B"},
    {"Lydian dominant", "C D E F# G A Bb"},
    {"Mixolydian flat 6", "C D E F G# A Bb"},
    {"Locrian natural 2", "C D Eb F Gb Ab Bb"},
    {"altered", "C Db Eb Fb Gb Ab Bb"},
    {"major pentatonic", "C D E G A"},
    {"minor pentatonic", "C Eb F G Bb"},
    {"suspended pentatonic", "C D F G Bb"},
    {"blues", "C Eb F Gb G Bb"},
    {"major blues", "C D Eb E G A"},
    {"whole tone", "C D E F# G# Bb"},
    {"diminished", "C D Eb F Gb Ab A B"},
    {"dominant diminished", "C Db Eb E F# G A Bb"},
    {"harmonic major", "C D E F G Ab B"},
    {"double harmonic", "C Db E F G Ab B"},
    {"Hungarian minor", "C D Eb F# G Ab B"},
    {"Neapolitan minor", "C Db Eb F G Ab B"},
    {"Neapolitan major", "C Db Eb F G A B"},
    {"enigmatic", "C Db E F# G# Bb B"},
};

static unsigned scaleMask;
static int scaleFollow;
static LONG scaleSeen;

/* "C#", "Bb" and the like, as a pitch class. */
static int pitch_class(const char *name)
{
    static const int semitones[] = {9, 11, 0, 2, 4, 5, 7};

    if (*name < 'A' || *name > 'G') return -1;

    int pitch = semitones[*name - 'A'];
    while (name[1] == '#' || name[1] == 'b') pitch += *++name == '#' ? 1 : -1;
    return (pitch % 12 + 12) % 12;
}

static unsigned scale_mask(const char *pitches)
{
    unsigned mask = 0;

    for (const char *c = pitches; *c; c++)
        if (*c >= 'A' && *c <= 'G') {
            mask |= 1u << pitch_class(c);
            while (c[1] == '#' || c[1] == 'b') c++;
        }
    return mask;
}

/* The scales are listed on C, so a root turns one into its key. */
static unsigned scale_root_mask(unsigned mask, int root)
{
    return root ? ((mask << root) | (mask >> (12 - root))) & 0xfff : mask;
}

static const char *pitch_name(int pitch)
{
    static const char *names[] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
    return names[pitch % 12];
}

static void scale_notes(char *out, size_t size, int root)
{
    size_t at = 0;

    for (int i = 0; i < 12; i++) {
        int pitch = (root + i) % 12;
        if (!(scaleMask >> pitch & 1)) continue;

        int length = snprintf(out + at, size - at, "%s%s", at ? " " : "", pitch_name(pitch));
        if (length < 0 || (size_t)length >= size - at) break;
        at += (size_t)length;
    }
    out[at] = 0;
}

static int scale_note(int note)
{
    for (int pitch = note; pitch >= 0; pitch--)
        if (scaleMask >> (pitch % 12) & 1) return pitch;
    for (int pitch = note; pitch < 128; pitch++)
        if (scaleMask >> (pitch % 12) & 1) return pitch;
    return note;
}

#define VST_MAX_PLUGINS 1024
#define VST_MAX_EVENTS 256
#define VST_MAX_OUTPUTS 32

typedef struct {
    int32_t numEvents;
    intptr_t reserved;
    VstEvent *events[VST_MAX_EVENTS];
    VstMidiEvent midi[VST_MAX_EVENTS];
} MidiEventList;

static MidiEventList midiEvents;
static AEffect *vstEffect;

#define VST_NAME 128
#define VST_MAX_OCTAVE 6
#define VST_OCTAVES (2 * VST_MAX_OCTAVE + 1)

typedef struct {
    char name[VST_NAME];
    int presets, low, high;
} VstStats;

static VstStats vstStats[VST_MAX_PLUGINS];
static int vstStatsCount, vstRangeLow, vstRangeHigh = 127, vstOctave;

static int vst_note(int note)
{
    int span = vstRangeHigh - vstRangeLow + 1;
    note += vstOctave * 12;
    return vstRangeLow + ((note - vstRangeLow) % span + span) % span;
}

static void queue_midi_event(uint32_t message)
{
    if (midiEvents.numEvents == VST_MAX_EVENTS) return;

    VstMidiEvent *event = &midiEvents.midi[midiEvents.numEvents];
    memset(event, 0, sizeof *event);
    event->type = kVstMidiType;
    event->byteSize = sizeof *event;
    event->midiData[0] = (char)(message & 0xff);
    event->midiData[1] = (char)(message >> 8 & 0xff);
    event->midiData[2] = (char)(message >> 16 & 0xff);
    midiEvents.events[midiEvents.numEvents++] = (VstEvent *)event;
}

static void trigger_drum(int note);

static const char *asio_error_text(ASIOError error)
{
    switch (error) {
    case ASE_OK: return "ok";
    case ASE_NotPresent: return "not present";
    case ASE_HWMalfunction: return "hardware malfunction";
    case ASE_InvalidParameter: return "invalid parameter";
    case ASE_InvalidMode: return "invalid mode";
    case ASE_SPNotAdvancing: return "sample position not advancing";
    case ASE_NoClock: return "no clock";
    case ASE_NoMemory: return "out of memory";
    default: return "unknown error";
    }
}

static void log_line(const char *format, ...)
{
    va_list args;
    putchar('\n');
    va_start(args, format);
    vprintf(format, args);
    va_end(args);
    putchar('\n');
    g.lineWidth = 0;
}

/* Single producer (WinMM callback thread), single consumer (ASIO thread).
   x64's memory model keeps the plain volatile indices ordered. */
static void midi_push(uint32_t message)
{
    uint32_t head = midiHead;
    if (head - midiTail >= MIDI_QUEUE) return;
    midiQueue[head % MIDI_QUEUE] = message;
    midiHead = head + 1;
}

static int midi_pop(uint32_t *message)
{
    uint32_t tail = midiTail;
    if (tail == midiHead) return 0;
    *message = midiQueue[tail % MIDI_QUEUE];
    midiTail = tail + 1;
    return 1;
}

static void drain_midi(void)
{
    uint32_t message;
    midiEvents.numEvents = 0;
    while (midi_pop(&message)) {
        int status = (int)(message & 0xf0);
        int note = (int)(message >> 8) & 0x7f;
        int velocity = (int)(message >> 16) & 0x7f;
        if (status == 0x90 && velocity > 0) {
            synth_note_on(note, velocity / 127.0f);
            if (g.samplesSound) trigger_drum(note);
        } else if (status == 0x80 || status == 0x90) {
            synth_note_off(note);
        }
        if (!g.samplesSound && vstEffect && status < 0xf0) {
            if (status == 0x80 || status == 0x90 || status == 0xa0)
                message = (message & 0xffff00ffu) | (uint32_t)vst_note(note) << 8;
            queue_midi_event(message);
        }
    }
}

typedef struct {
    float *samples;
    drwav_uint64 frames, position;
    unsigned channels, rate;
    double fraction;
    double wait;             /* frames left before the stem's lead-in is up */
    volatile LONG muted;     /* an instrument instance is playing the stem's sound */
} Track;

static wchar_t *long_path(const wchar_t *path)
{
    if (!wcsncmp(path, L"\\\\?\\", 4)) {
        size_t size = (wcslen(path) + 1) * sizeof(wchar_t);
        wchar_t *copy = malloc(size);
        if (copy) memcpy(copy, path, size);
        return copy;
    }

    wchar_t full[32768];
    size_t length = wcslen(path);
    int drive = length >= 3 && path[1] == L':' && (path[2] == L'\\' || path[2] == L'/');
    int unc = length >= 2 && path[0] == L'\\' && path[1] == L'\\';
    const wchar_t *source = path;

    if (!drive && !unc) {
        DWORD result = GetFullPathNameW(path, 32768, full, NULL);
        if (!result || result >= 32768) return NULL;
        source = full;
        unc = full[0] == L'\\' && full[1] == L'\\';
    }

    length = wcslen(source);
    wchar_t *result = malloc((length + (unc ? 7 : 5)) * sizeof(wchar_t));
    if (!result) return NULL;
    if (unc) swprintf(result, length + 7, L"\\\\?\\UNC\\%ls", source + 2);
    else swprintf(result, length + 5, L"\\\\?\\%ls", source);
    for (wchar_t *character = result; *character; character++)
        if (*character == L'/') *character = L'\\';
    return result;
}

static wchar_t *widen(const char *text)
{
    int length = MultiByteToWideChar(CP_ACP, 0, text, -1, NULL, 0);
    if (length <= 0) return NULL;
    wchar_t *wide = malloc((size_t)length * sizeof(wchar_t));
    if (wide) MultiByteToWideChar(CP_ACP, 0, text, -1, wide, length);
    return wide;
}

static int is_folder_w(const wchar_t *path)
{
    DWORD attributes = GetFileAttributesW(path);
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY);
}

static void print_wide_path(const wchar_t *path)
{
    if (!wcsncmp(path, L"\\\\?\\UNC\\", 8)) printf("\\\\%ls", path + 8);
    else printf("%ls", path + (wcsncmp(path, L"\\\\?\\", 4) ? 0 : 4));
}

static int playlistMode, instrumentMode;
static volatile LONG playlistRefresh;
#define STEM_NAME 160
#define MIX_BLOCK 4096

/* Each stem's lead-in lives in song.json, so a take carries no silence of its own: it
   is cued that far into the loop instead. */
typedef struct {
    wchar_t name[STEM_NAME];
    double lead;
} StemLead;

static wchar_t *stemFiles[QUEUE_FILES];
static ULONGLONG stemWritten[QUEUE_FILES];
static StemLead stemLeads[QUEUE_FILES];
static int stemLeadCount;

static void load_stem_leads(void);
static double stem_lead(const wchar_t *file);
static void set_stem_lead(const wchar_t *file, double lead);
static void save_stem_leads(void);
static int sound_mode(int number);
static int stem_muted(const wchar_t *file);
static const char *instrumentName;
static wchar_t *audioFiles[QUEUE_FILES];
static int audioFileCount;
static const wchar_t *playlistFolder;
static Track *stems;
static int stemCount;

static int compare_names(const void *a, const void *b)
{
    return _wcsicmp(*(const wchar_t *const *)a, *(const wchar_t *const *)b);
}

static const wchar_t *const audioExtensions[] = {
    L".wav", L".wave", L".aif", L".aiff", L".flac", L".mp3", L".ogg", L".oga"
};

static int is_audio_name(const wchar_t *name)
{
    const wchar_t *dot = wcsrchr(name, L'.');
    if (!dot) return 0;
    for (size_t i = 0; i < sizeof audioExtensions / sizeof *audioExtensions; i++)
        if (!_wcsicmp(dot, audioExtensions[i])) return 1;
    return 0;
}

static ULONGLONG written_time(const wchar_t *path)
{
    WIN32_FILE_ATTRIBUTE_DATA data;
    if (!GetFileAttributesExW(path, GetFileExInfoStandard, &data)) return 0;

    ULARGE_INTEGER value;
    value.LowPart = data.ftLastWriteTime.dwLowDateTime;
    value.HighPart = data.ftLastWriteTime.dwHighDateTime;
    return value.QuadPart;
}

static int scan_audio_files(const wchar_t *folder)
{
    size_t folderLength = wcslen(folder);
    wchar_t *pattern = malloc((folderLength + 3) * sizeof(wchar_t));
    if (!pattern) return -1;
    swprintf(pattern, folderLength + 3, L"%ls\\*", folder);

    WIN32_FIND_DATAW entry;
    HANDLE find = FindFirstFileW(pattern, &entry);
    free(pattern);
    if (find == INVALID_HANDLE_VALUE) return -1;

    do {
        if ((entry.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || audioFileCount == QUEUE_FILES) continue;
        if (!is_audio_name(entry.cFileName)) continue;

        size_t nameLength = wcslen(entry.cFileName);
        wchar_t *file = malloc((folderLength + nameLength + 2) * sizeof(wchar_t));
        if (!file) continue;
        swprintf(file, folderLength + nameLength + 2, L"%ls\\%ls", folder, entry.cFileName);
        audioFiles[audioFileCount++] = file;
    } while (FindNextFileW(find, &entry));
    FindClose(find);

    qsort(audioFiles, (size_t)audioFileCount, sizeof audioFiles[0], compare_names);
    return audioFileCount;
}

static float *allocate_samples(drwav_uint64 frames, unsigned channels)
{
    return frames ? malloc((size_t)(frames * channels) * sizeof(float)) : NULL;
}

typedef float *(*Decoder)(const wchar_t *path, unsigned *channels, unsigned *rate, drwav_uint64 *frames);

static uint32_t read_uint(const unsigned char *bytes, unsigned count, int littleEndian)
{
    uint32_t value = 0;
    for (unsigned i = 0; i < count; i++)
        value |= (uint32_t)bytes[i] << (8 * (littleEndian ? i : count - 1 - i));
    return value;
}

static uint64_t read_uint64(const unsigned char *bytes, int littleEndian)
{
    uint64_t value = 0;
    for (unsigned i = 0; i < 8; i++)
        value |= (uint64_t)bytes[i] << (8 * (littleEndian ? i : 7 - i));
    return value;
}

static double extended_rate(const unsigned char *bytes)
{
    int exponent = (bytes[0] & 0x7f) << 8 | bytes[1];
    if (!exponent) return 0;
    double value = ldexp((double)read_uint64(bytes + 2, 0), exponent - 16383 - 63);
    return bytes[0] & 0x80 ? -value : value;
}

/* AIFF and AIFF-C store their samples big-endian in the SSND chunk, with 80-bit
   extended sample rates and a compression type that only AIFF-C files carry. */

static float *decode_aiff(const wchar_t *path, unsigned *channels, unsigned *rate, drwav_uint64 *frames)
{
    FILE *file = _wfopen(path, L"rb");
    if (!file) return NULL;

    unsigned char header[12], chunk[8];
    if (fread(header, 1, sizeof header, file) != sizeof header || memcmp(header, "FORM", 4)) {
        fclose(file);
        return NULL;
    }

    unsigned bits = 0;
    uint32_t compression = 0;
    drwav_uint64 total = 0;
    long soundStart = -1, soundBytes = 0;

    for (;;) {
        if (fread(chunk, 1, sizeof chunk, file) != sizeof chunk) break;
        uint32_t size = read_uint(chunk + 4, 4, 0);
        long body = ftell(file);
        if (!memcmp(chunk, "COMM", 4) && size >= 18) {
            unsigned char comm[22];
            size_t wanted = size < sizeof comm ? size : sizeof comm;
            if (fread(comm, 1, wanted, file) != wanted) break;
            *channels = read_uint(comm, 2, 0);
            total = read_uint(comm + 2, 4, 0);
            bits = read_uint(comm + 6, 2, 0);
            *rate = (unsigned)(extended_rate(comm + 8) + 0.5);
            if (wanted == sizeof comm) compression = read_uint(comm + 18, 4, 0);
        } else if (!memcmp(chunk, "SSND", 4) && size >= 8) {
            unsigned char sound[8];
            if (fread(sound, 1, sizeof sound, file) != sizeof sound) break;
            uint32_t offset = read_uint(sound, 4, 0);
            soundStart = body + 8 + (long)offset;
            soundBytes = (long)size - 8 - (long)offset;
            break;
        }
        if (fseek(file, body + (long)size + (size & 1), SEEK_SET)) break;
    }

    int littleEndian = compression == 0x736f7774;    /* 'sowt' */
    int floating = compression == 0x666c3332 || compression == 0x464c3332 ||
                   compression == 0x666c3634 || compression == 0x464c3634;  /* 'fl32' 'FL32' 'fl64' 'FL64' */
    int pcm = !compression || compression == 0x4e4f4e45 || littleEndian ||
              compression == 0x696e3234 || compression == 0x696e3332;      /* 'NONE' 'in24' 'in32' */
    unsigned width = (bits + 7) / 8;

    if (!*channels || !*rate || !total || !width || (!pcm && !floating) || soundStart < 0) {
        fclose(file);
        return NULL;
    }

    drwav_uint64 available = (drwav_uint64)(soundBytes / (long)(width * *channels));
    if (total > available) total = available;

    float *samples = allocate_samples(total, *channels);
    unsigned char *buffer = samples ? malloc(16384 * *channels * width) : NULL;
    if (!buffer || fseek(file, soundStart, SEEK_SET)) {
        free(samples);
        free(buffer);
        fclose(file);
        return NULL;
    }

    drwav_uint64 done = 0;
    while (done < total) {
        drwav_uint64 want = total - done < 16384 ? total - done : 16384;
        size_t got = fread(buffer, width * *channels, (size_t)want, file);
        if (!got) break;
        for (size_t frame = 0; frame < got; frame++) {
            for (unsigned channel = 0; channel < *channels; channel++) {
                const unsigned char *sample = buffer + (frame * *channels + channel) * width;
                float value;
                if (floating && width == 4) {
                    uint32_t raw = read_uint(sample, 4, littleEndian);
                    memcpy(&value, &raw, 4);
                } else if (floating) {
                    uint64_t raw = read_uint64(sample, littleEndian);
                    double wide;
                    memcpy(&wide, &raw, 8);
                    value = (float)wide;
                } else
                    value = (float)((int32_t)(read_uint(sample, width, littleEndian) << (32 - bits)) >> (32 - bits)) /
                            (float)(1u << (bits - 1));
                samples[(done + frame) * *channels + channel] = value;
            }
        }
        done += got;
    }

    free(buffer);
    fclose(file);
    *frames = done;
    return samples;
}

static float *decode_wav(const wchar_t *path, unsigned *channels, unsigned *rate, drwav_uint64 *frames)
{
    drwav wav;
    if (!drwav_init_file_w(&wav, path, NULL)) return NULL;
    *channels = wav.channels;
    *rate = wav.sampleRate;
    float *samples = allocate_samples(wav.totalPCMFrameCount, *channels);
    if (samples) *frames = drwav_read_pcm_frames_f32(&wav, wav.totalPCMFrameCount, samples);
    drwav_uninit(&wav);
    return samples;
}

static float *decode_flac(const wchar_t *path, unsigned *channels, unsigned *rate, drwav_uint64 *frames)
{
    drflac *flac = drflac_open_file_w(path, NULL);
    if (!flac) return NULL;
    *channels = flac->channels;
    *rate = flac->sampleRate;
    float *samples = allocate_samples(flac->totalPCMFrameCount, *channels);
    if (samples) *frames = drflac_read_pcm_frames_f32(flac, flac->totalPCMFrameCount, samples);
    drflac_close(flac);
    return samples;
}

static float *decode_mp3(const wchar_t *path, unsigned *channels, unsigned *rate, drwav_uint64 *frames)
{
    drmp3 mp3;
    if (!drmp3_init_file_w(&mp3, path, NULL)) return NULL;
    *channels = mp3.channels;
    *rate = mp3.sampleRate;
    drwav_uint64 total = drmp3_get_pcm_frame_count(&mp3);
    float *samples = allocate_samples(total, *channels);
    if (samples) *frames = drmp3_read_pcm_frames_f32(&mp3, total, samples);
    drmp3_uninit(&mp3);
    return samples;
}

static float *decode_vorbis(const wchar_t *path, unsigned *channels, unsigned *rate, drwav_uint64 *frames)
{
    FILE *file = _wfopen(path, L"rb");
    if (!file) return NULL;
    int error = 0;
    stb_vorbis *vorbis = stb_vorbis_open_file(file, 1, &error, NULL);
    if (!vorbis) {
        fclose(file);
        return NULL;
    }
    stb_vorbis_info info = stb_vorbis_get_info(vorbis);
    *channels = (unsigned)info.channels;
    *rate = info.sample_rate;
    drwav_uint64 total = (drwav_uint64)stb_vorbis_stream_length_in_samples(vorbis);
    float *samples = allocate_samples(total, *channels);
    if (samples)
        *frames = (drwav_uint64)stb_vorbis_get_samples_float_interleaved(vorbis, info.channels, samples, (int)(total * *channels));
    stb_vorbis_close(vorbis);
    return samples;
}

static int decode_track(Track *track, const wchar_t *path)
{
    unsigned char magic[12];
    size_t read = 0;
    FILE *file = _wfopen(path, L"rb");
    if (file) {
        read = fread(magic, 1, sizeof magic, file);
        fclose(file);
    }

    Decoder decode = decode_wav;
    if (read == sizeof magic) {
        if (!memcmp(magic, "FORM", 4) && (!memcmp(magic + 8, "AIFF", 4) || !memcmp(magic + 8, "AIFC", 4)))
            decode = decode_aiff;
        else if (!memcmp(magic, "fLaC", 4)) decode = decode_flac;
        else if (!memcmp(magic, "OggS", 4)) decode = decode_vorbis;
        else if (!memcmp(magic, "ID3", 3) || (magic[0] == 0xff && (magic[1] & 0xe0) == 0xe0)) decode = decode_mp3;
    }

    unsigned channels = 0, rate = 0;
    drwav_uint64 frames = 0;
    float *samples = decode(path, &channels, &rate, &frames);
    if (!samples || !channels || !rate || !frames) {
        free(samples);
        log_error("Could not decode '%ls'.\n", path);
        return -1;
    }

    memset(track, 0, sizeof *track);
    track->samples = samples;
    track->frames = frames;
    track->channels = channels;
    track->rate = rate;
    return 0;
}

static int start_playlist_w(const wchar_t *folder)
{
    wchar_t *path = long_path(folder);
    if (!path) {
        log_error("Could not resolve the folder path.\n");
        return -1;
    }
    if (scan_audio_files(path) <= 0) {
        log_error("Could not find any audio files in '%ls'.\n", path);
        free(path);
        return -1;
    }
    playlistFolder = path;

    stems = calloc(QUEUE_FILES, sizeof *stems);
    if (!stems) {
        log_error("Out of memory for %d audio files.\n", audioFileCount);
        return -1;
    }

    load_stem_leads();
    for (int i = 0; i < audioFileCount; i++)
        if (decode_track(&stems[stemCount], audioFiles[i]) == 0) {
            stems[stemCount].wait = stem_lead(audioFiles[i]) * (double)stems[stemCount].rate;
            stemWritten[stemCount] = written_time(audioFiles[i]);
            stemFiles[stemCount++] = audioFiles[i];
        }

    if (!stemCount) {
        log_error("Every audio file in the folder failed to decode.\n");
        return -1;
    }
    return 0;
}

static int start_playlist(const char *folder)
{
    wchar_t *path = widen(folder);
    if (!path) {
        log_error("Could not convert the folder path.\n");
        return -1;
    }
    int result = start_playlist_w(path);
    free(path);
    return result;
}

static float track_sample(const Track *track, int output)
{
    if (!track->samples || track->position >= track->frames || track->wait > 0.0 || track->muted) return 0.0f;

    size_t channel = (size_t)output % track->channels;
    drwav_uint64 next = track->position + 1 < track->frames ? track->position + 1 : track->position;
    const float *a = track->samples + (size_t)track->position * track->channels + channel;
    const float *b = track->samples + (size_t)next * track->channels + channel;
    return *a + (*b - *a) * (float)track->fraction;
}

/* Loop cues travel through one shared section: the stems instance stamps the
   performance counter and bumps the sequence, every other instance polls it. */

typedef struct {
    volatile LONG seq;
    volatile LONGLONG qpc;
    DWORD pid;
    wchar_t folder[1024];
    wchar_t stems[1024];
    volatile LONG stems_count;
    volatile LONG generation;
    volatile double length;
    volatile double rating;
    volatile LONG scale_seq, scale_mask, scale_root;
    char scale_name[SCALE_NAME];
} LoopCue;

static HANDLE cueMapping;
static LoopCue *cue;

typedef struct TakeChunk {
    struct TakeChunk *next;
    size_t frames;
    float *samples;
} TakeChunk;

typedef struct {
    ULONGLONG qpc;
    int number;
    const char *mode;
} SoundMark;

typedef struct {
    int active, wavOpen;
    unsigned rate;
    TakeChunk *first, *cursor;
    TakeChunk *volatile last;
    drwav wav;
    drwav_int16 *block;
    wchar_t passesPath[1100], soundsPath[1100];
    drwav_uint64 frames;
    ULONGLONG origin;
    SYSTEMTIME at;
    wchar_t folder[1024];
    HANDLE palette;
    LONG seen;
    double *passMs;
    int passCount, passCapacity;
    SoundMark *soundMarks;
    int soundCount, soundCapacity;
} Take;

static Take take;

static void open_cue(void)
{
    if (cueMapping) return;
    cueMapping = CreateFileMappingA(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, 0, sizeof(LoopCue),
                                    "Local\\RestlessVirtuosoLoopCue2");
    cue = cueMapping ? MapViewOfFile(cueMapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(LoopCue)) : NULL;
    if (cue) take.seen = InterlockedCompareExchange(&cue->seq, 0, 0);
}

static double playlist_length(void);

static void send_loop_start(void)
{
    open_cue();
    if (!cue) return;

    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    cue->qpc = now.QuadPart;
    cue->length = playlist_length();
    take.seen = InterlockedIncrement(&cue->seq);
}

static void publish_palette(const wchar_t *stems)
{
    open_cue();
    if (!cue) return;

    size_t length = wcslen(stems);
    while (length > 3 && (stems[length - 1] == L'\\' || stems[length - 1] == L'/')) length--;
    if (length >= sizeof cue->stems / sizeof *cue->stems) return;

    size_t folder = length;
    while (folder > 3 && stems[folder - 1] != L'\\') folder--;
    if (stems[folder - 1] != L'\\' || folder + 12 > sizeof cue->folder / sizeof *cue->folder) return;

    wmemcpy(cue->stems, stems, length);
    cue->stems[length] = 0;
    wmemcpy(cue->folder, stems, folder);
    wcscpy(cue->folder + folder, L"[palettes]\\");
    cue->stems_count = stemCount;
    cue->generation = 0;
    cue->length = playlist_length();
    cue->rating = 0.0;
    cue->qpc = 0;
    cue->pid = GetCurrentProcessId();
}

static void add_pass(double milliseconds)
{
    if (take.passCount == take.passCapacity) {
        int capacity = take.passCapacity ? take.passCapacity * 2 : 64;
        double *grown = realloc(take.passMs, (size_t)capacity * sizeof *grown);
        if (!grown) return;
        take.passMs = grown;
        take.passCapacity = capacity;
    }
    take.passMs[take.passCount++] = milliseconds;
}

static void add_sound_mark(int number, ULONGLONG qpc)
{
    if (!number) return;

    if (take.soundCount == take.soundCapacity) {
        int capacity = take.soundCapacity ? take.soundCapacity * 2 : 16;
        SoundMark *grown = realloc(take.soundMarks, (size_t)capacity * sizeof *grown);
        if (!grown) return;
        take.soundMarks = grown;
        take.soundCapacity = capacity;
    }
    take.soundMarks[take.soundCount].qpc = qpc;
    take.soundMarks[take.soundCount].number = number;
    take.soundMarks[take.soundCount++].mode = g.samplesSound ? "samples" : "synth";
}

static void check_loop_cue(void)
{
    open_cue();
    if (!cue) return;

    LONG seen = InterlockedCompareExchange(&cue->seq, 0, 0);
    if (seen == take.seen) return;

    LONGLONG qpc = cue->qpc;
    if (InterlockedCompareExchange(&cue->seq, 0, 0) != seen) return;

    take.seen = seen;
    if (!take.passCount) {
        take.origin = (ULONGLONG)qpc;
        take.rate = (unsigned)g.rate;
        GetLocalTime(&take.at);
        wcsncpy(take.folder, cue->folder, 1023);
        take.folder[1023] = 0;
        if (take.palette) CloseHandle(take.palette);
        take.palette = cue->pid ? OpenProcess(SYNCHRONIZE, FALSE, cue->pid) : NULL;
    }

    LARGE_INTEGER frequency;
    QueryPerformanceFrequency(&frequency);
    double ms = (double)(LONGLONG)((ULONGLONG)qpc - take.origin) * 1000.0 / (double)frequency.QuadPart;
    add_pass(ms);
    take.active = take.rate > 0;
}

static void take_join(void)
{
    if (playlistMode || take.active || !cue || !cue->pid || !cue->folder[0] || !cue->qpc) return;

    HANDLE palette = OpenProcess(SYNCHRONIZE, FALSE, cue->pid);
    if (!palette || WaitForSingleObject(palette, 0) == WAIT_OBJECT_0) {
        if (palette) CloseHandle(palette);
        return;
    }

    LONGLONG lastCue = cue->qpc;
    if (!lastCue) {
        CloseHandle(palette);
        return;
    }

    LARGE_INTEGER now, frequency;
    QueryPerformanceCounter(&now);
    QueryPerformanceFrequency(&frequency);

    take.active = 1;
    take.rate = (unsigned)g.rate;
    take.palette = palette;
    take.origin = (ULONGLONG)now.QuadPart;
    GetLocalTime(&take.at);
    wcsncpy(take.folder, cue->folder, 1023);
    take.folder[1023] = 0;
    add_pass(-(double)(now.QuadPart - lastCue) * 1000.0 / (double)frequency.QuadPart);
}

static void clear_loop_cue(void)
{
    if (!playlistMode || !cue || cue->pid != GetCurrentProcessId()) return;

    cue->pid = 0;
    cue->qpc = 0;
    cue->rating = 0.0;
    cue->stems_count = 0;
    cue->generation = 0;
    cue->length = 0.0;
    cue->folder[0] = 0;
    cue->stems[0] = 0;
}

/* The session's scale is the first one drawn by an instance that follows one, and a
   song's saved scale takes over when a stems instance loads that song. */
static void publish_scale(unsigned mask, int root, const char *name)
{
    open_cue();
    if (!cue || (cue->scale_mask == (LONG)mask && cue->scale_root == root && !strcmp(cue->scale_name, name)))
        return;

    cue->scale_root = root;
    cue->scale_mask = (LONG)mask;
    snprintf(cue->scale_name, sizeof cue->scale_name, "%s", name);
    InterlockedIncrement(&cue->scale_seq);
}

#define TAKE_CHUNK (1u << 20)

static void take_buffer(int frames)
{
    if (!take.active) return;
    if (take.last && take.last->frames + (size_t)frames <= TAKE_CHUNK) return;

    TakeChunk *chunk = malloc(sizeof *chunk);
    float *samples = malloc((size_t)TAKE_CHUNK * sizeof *samples);
    if (!chunk || !samples) {
        free(chunk);
        free(samples);
        take.active = 0;
        return;
    }

    chunk->next = NULL;
    chunk->frames = 0;
    chunk->samples = samples;
    if (take.last) take.last->next = chunk;
    else take.first = chunk;
    take.last = chunk;
}

static void take_mix(const float *samples, int frames)
{
    float *chunk = take.last->samples;
    for (int i = 0; i < frames; i++)
        chunk[take.last->frames + (size_t)i] += samples[i] / (float)g.nOut;
}

/* The take is streamed to the file from the main loop, which only ever writes
   chunks the ASIO thread has finished filling, so closing stays quick. */

static void take_open(void)
{
    if (!take.active || take.wavOpen || !take.frames || !take.folder[0]) return;

    CreateDirectoryW(take.folder, NULL);

    SYSTEMTIME at = take.at;
    wchar_t stamp[40], suffix[8] = L"", wavPath[1100];
    swprintf(stamp, 40, L"%04d-%02d-%02d-%02d-%02d-%02d",
             at.wYear, at.wMonth, at.wDay, at.wHour, at.wMinute, at.wSecond);
    for (int attempt = 1; attempt < 100; attempt++) {
        if (attempt > 1) swprintf(suffix, 8, L"-%d", attempt);
        swprintf(wavPath, 1100, L"%lspalette_%ls%ls.wav", take.folder, stamp, suffix);
        if (GetFileAttributesW(wavPath) == INVALID_FILE_ATTRIBUTES) break;
    }
    swprintf(take.passesPath, 1100, L"%lspasses_%ls%ls.txt", take.folder, stamp, suffix);
    swprintf(take.soundsPath, 1100, L"%lssounds_%ls%ls.txt", take.folder, stamp, suffix);

    drwav_data_format format;
    memset(&format, 0, sizeof format);
    format.container = drwav_container_riff;
    format.format = DR_WAVE_FORMAT_PCM;
    format.channels = 1;
    format.sampleRate = take.rate;
    format.bitsPerSample = 16;

    take.block = malloc((size_t)TAKE_CHUNK * sizeof *take.block);
    if (!take.block) {
        log_error("Out of memory for the take buffer.\n");
        take.active = 0;
        return;
    }
    if (!drwav_init_file_write_w(&take.wav, wavPath, &format, NULL)) {
        log_error("Could not write '%ls'.\n", wavPath);
        take.active = 0;
        return;
    }

    take.cursor = take.first;
    take.wavOpen = 1;
    log_trace("recording to %ls\n", wavPath);
}

static void take_flush(void)
{
    if (!take.wavOpen) return;
    while (take.cursor != take.last) {
        drwav_f32_to_s16(take.block, take.cursor->samples, take.cursor->frames);
        drwav_write_pcm_frames(&take.wav, take.cursor->frames, take.block);
        take.cursor = take.cursor->next;
    }
}

static void take_finish(void)
{
    if (take.wavOpen) {
        take_flush();
        drwav_f32_to_s16(take.block, take.cursor->samples, take.cursor->frames);
        drwav_write_pcm_frames(&take.wav, take.cursor->frames, take.block);
        drwav_uninit(&take.wav);

        FILE *file = _wfopen(take.passesPath, L"w");
        if (!file) log_error("Could not write '%ls'.\n", take.passesPath);
        else {
            for (int i = 0; i < take.passCount; i++)
                fprintf(file, "%d, %.6f 1.;\n", i, take.passMs[i]);
            fclose(file);
            log_trace("wrote %ls\n", take.passesPath);
        }

        FILE *sounds = _wfopen(take.soundsPath, L"w");
        if (!sounds) log_error("Could not write '%ls'.\n", take.soundsPath);
        else {
            if (!take.soundCount) fputs("0, 0.000000 audio;\n", sounds);
            else {
                LARGE_INTEGER frequency;
                QueryPerformanceFrequency(&frequency);
                for (int i = 0; i < take.soundCount; i++)
                    fprintf(sounds, "%d, %.6f %d %s;\n", i,
                            (double)(LONGLONG)(take.soundMarks[i].qpc - take.origin) * 1000.0 /
                            (double)frequency.QuadPart,
                            take.soundMarks[i].number, take.soundMarks[i].mode);
            }
            fclose(sounds);
            log_trace("wrote %ls\n", take.soundsPath);
        }
    }

    free(take.block);
    take.block = NULL;
    for (TakeChunk *chunk = take.first; chunk;) {
        TakeChunk *next = chunk->next;
        free(chunk->samples);
        free(chunk);
        chunk = next;
    }
    free(take.passMs);
    take.passMs = NULL;
    take.passCapacity = 0;
    take.soundCount = 0;
    if (instrumentMode && g.soundNumber) {
        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        add_sound_mark(g.soundNumber, (ULONGLONG)now.QuadPart);
    }
    take.first = take.last = take.cursor = NULL;
    take.frames = 0;
    take.passCount = 0;
    take.rate = 0;
    take.wavOpen = 0;
    take.active = 0;
    take.folder[0] = 0;
    if (take.palette) CloseHandle(take.palette);
    take.palette = NULL;
}

/* Every instance runs the cumulative flux analyzer over the audio it plays, at the
   15 s window and four bands the offline tools use. A stems instance runs one
   analyzer per stem it plays, and an instrument instance with a stems instance
   running adds one per stem that instance plays. Every analyzer in a group shares
   one buffer, so their scores accumulate together and give the instance's rating. */

#define RATING_WINDOW_MS 15000
#define RATING_BANDS 4
#define RATING_ANALYZERS 32
#define RATING_STEPS (RATING_WINDOW_MS / 100)
#define RATING_PROJECTS L"D:\\[Library]\\[Audio]\\[Works]\\[Projects]"

enum { RATING_SOLO, RATING_RECITATION, RATING_JAMMING };

static void sound_name(char *out, size_t size);
static void console_line(const char *text);

typedef struct {
    const Track *track;      /* a stem being played, or NULL for this instance's own audio */
    TransientAnalyzer *analyzer;
    float *block;
    int rate, hop, step;
    long long pushed;
    double lead;
} RatingCopy;

static SharedTransientBuffer ratingScores;
static RatingCopy ratingCopies[RATING_ANALYZERS];
static int ratingCopyCount;
static TransientAnalyzer *ratingOwn;
static Track *ratingStems;
static double ratingStemLeads[QUEUE_FILES];
static int ratingStemCount;

static HANDLE ratingThread, ratingWake, spawnedStems;
static volatile LONG ratingQuit;
static volatile double ratingValue, ratingBurst;
static struct { double sum; int count; } ratingSteps[RATING_STEPS];
static int ratingStepHead, ratingStepCount;
static double ratingSum;
static int ratingCount;
static CRITICAL_SECTION ratingLock;
static float *ratingFeed;
static int ratingFeedSize, ratingHead, ratingTail, ratingCount;
static int ratingSound, ratingStarted;

static volatile LONG ratingRequest, ratingLoop;
static int ratingRequestMode;
static wchar_t ratingRequestFolder[1024];
static int ratingRequestedMode = -1, ratingRequestedStems, ratingRequestedGeneration;
static wchar_t ratingRequestedFolder[1024];

typedef struct {
    volatile LONG active;
    unsigned rate;
    TakeChunk *first, *last;
    drwav_uint64 frames, offset;
    int number, jamming;
    char sound[96];
} Recording;

static Recording recording;
static CRITICAL_SECTION recordingLock;

static double rating_current(void)
{
    return ratingValue;
}

/* Seconds since the stems instance's loop started, which is where every stem being
   played is in its own file. */
static double rating_elapsed(void)
{
    LARGE_INTEGER now, frequency;

    if (!cue || !cue->qpc) return -1.0;
    QueryPerformanceCounter(&now);
    QueryPerformanceFrequency(&frequency);
    return (double)(now.QuadPart - cue->qpc) / (double)frequency.QuadPart;
}

static void rating_audio(const float *samples, int frames)
{
    if (!ratingFeed) return;

    EnterCriticalSection(&ratingLock);
    for (int i = 0; i < frames && ratingCount < ratingFeedSize; i++) {
        ratingFeed[ratingHead] = samples[i];
        ratingHead = (ratingHead + 1) % ratingFeedSize;
        ratingCount++;
    }
    LeaveCriticalSection(&ratingLock);
    SetEvent(ratingWake);
}

static int rating_take(float *out, int wanted)
{
    int taken = 0;

    EnterCriticalSection(&ratingLock);
    while (taken < wanted && ratingCount > 0) {
        out[taken++] = ratingFeed[ratingTail];
        ratingTail = (ratingTail + 1) % ratingFeedSize;
        ratingCount--;
    }
    LeaveCriticalSection(&ratingLock);
    return taken;
}

static void rating_drop(void)
{
    for (int i = 0; i < ratingCopyCount; i++) {
        analyzer_destroy(ratingCopies[i].analyzer);
        free(ratingCopies[i].block);
    }
    ratingCopyCount = 0;
    analyzer_destroy(ratingOwn);
    ratingOwn = NULL;

    for (int i = 0; i < ratingStemCount; i++) free(ratingStems[i].samples);
    free(ratingStems);
    ratingStems = NULL;
    ratingStemCount = 0;

    memset(&ratingScores, 0, sizeof ratingScores);
    ratingScores.max_peak = 1.0;
    ratingScores.min_score_seen = DBL_MAX;
    ratingScores.max_score_seen = -DBL_MAX;
    ratingValue = 0.0;
    ratingBurst = 0.0;
    ratingStepHead = ratingStepCount = ratingCount = 0;
    ratingSum = 0.0;

    EnterCriticalSection(&ratingLock);
    ratingCount = ratingHead = ratingTail = 0;
    LeaveCriticalSection(&ratingLock);
}

static int rating_add(const Track *track, double lead)
{
    if (ratingCopyCount == RATING_ANALYZERS) return 0;
    if (track && (!track->samples || !track->frames || !track->rate)) return 0;

    RatingCopy *copy = &ratingCopies[ratingCopyCount];
    copy->track = track;
    copy->lead = lead;
    copy->rate = track ? (int)track->rate : (int)g.rate;
    copy->hop = copy->rate / 1000;
    if (copy->hop < 1) copy->hop = 1;
    copy->step = copy->rate / 10;
    if (copy->step < copy->hop) copy->step = copy->hop;
    copy->pushed = 0;
    copy->block = malloc((size_t)copy->step * sizeof *copy->block);
    copy->analyzer = analyzer_create(1.0, &ratingScores, NULL, NULL, NULL, RATING_WINDOW_MS, RATING_BANDS);
    if (!copy->block || !copy->analyzer) {
        analyzer_destroy(copy->analyzer);
        free(copy->block);
        return 0;
    }
    ratingCopyCount++;
    return 1;
}

static int rating_load(const wchar_t *folder)
{
    wchar_t *path = long_path(folder);
    if (!path) return 0;

    size_t length = wcslen(path);
    wchar_t *pattern = malloc((length + 3) * sizeof *pattern);
    wchar_t *files[QUEUE_FILES];
    int count = 0;

    if (pattern) {
        WIN32_FIND_DATAW entry;
        swprintf(pattern, length + 3, L"%ls\\*", path);
        HANDLE find = FindFirstFileW(pattern, &entry);
        free(pattern);
        if (find != INVALID_HANDLE_VALUE) {
            do {
                if ((entry.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || count == QUEUE_FILES) continue;
                if (!is_audio_name(entry.cFileName)) continue;

                size_t nameLength = wcslen(entry.cFileName);
                wchar_t *file = malloc((length + nameLength + 2) * sizeof *file);
                if (!file) continue;
                swprintf(file, length + nameLength + 2, L"%ls\\%ls", path, entry.cFileName);
                files[count++] = file;
            } while (FindNextFileW(find, &entry));
            FindClose(find);
        }
    }
    free(path);

    qsort(files, (size_t)count, sizeof files[0], compare_names);
    ratingStems = count ? calloc((size_t)count, sizeof *ratingStems) : NULL;
    for (int i = 0; i < count; i++) {
        if (ratingStems && decode_track(&ratingStems[ratingStemCount], files[i]) == 0) {
            ratingStemLeads[ratingStemCount] = stem_lead(files[i]);
            ratingStemCount++;
        }
        free(files[i]);
    }
    return ratingStemCount;
}

static void rating_build(int mode, const wchar_t *folder)
{
    rating_drop();

    load_stem_leads();

    if (mode == RATING_RECITATION) {
        for (int i = 0; i < stemCount; i++)
            rating_add(&stems[i], stemFiles[i] ? stem_lead(stemFiles[i]) : 0.0);
        return;
    }

    ratingOwn = analyzer_create(1.0, &ratingScores, NULL, NULL, NULL, RATING_WINDOW_MS, RATING_BANDS);
    if (mode == RATING_JAMMING && folder[0] && rating_load(folder))
        for (int i = 0; i < ratingStemCount; i++) rating_add(&ratingStems[i], ratingStemLeads[i]);
}

static void rating_copy_step(RatingCopy *copy, double elapsed, ChunkAnalysisResult *chunk)
{
    const Track *track = copy->track;
    double since = elapsed - copy->lead;
    int playing = since > 0.0;
    long long base = playing ? (long long)(since * (double)track->rate) : 0;
    unsigned channels = track->channels;
    drwav_uint64 frames = track->frames;

    for (int i = 0; i < copy->step; i++) {
        drwav_uint64 at = (drwav_uint64)(base + i);
        float value = 0.0f;
        if (playing && at < frames) {
            size_t index = (size_t)at * channels;
            value = track->samples[index];
            if (channels > 1) value = (value + track->samples[index + 1]) * 0.5f;
        }
        copy->block[i] = value;
    }

    int active = (int)(copy->pushed - copy->rate / 5);
    if (active < 0) active = 0;
    int window = active - copy->rate * (RATING_WINDOW_MS / 1000);
    if (window < 0) window = 0;
    analyzer_analyze_chunk(copy->analyzer, copy->block, copy->step, copy->rate,
                           window / copy->hop, active / copy->hop, chunk);
    copy->pushed += copy->step;
}

static DWORD WINAPI rating_worker(void *unused)
{
    (void)unused;
    ChunkAnalysisResult *chunk = malloc(sizeof *chunk);
    float *block = NULL;
    int rate = 0, hop = 1, step = 0, filled = 0;
    long long pushed = 0;
    LONG applied = 0;

    if (!chunk) return 0;

    while (!ratingQuit) {
        WaitForSingleObject(ratingWake, 50);
        if (ratingQuit) break;

        LONG request = InterlockedCompareExchange(&ratingRequest, 0, 0);
        if (request != applied) {
            int mode;
            wchar_t folder[1024];

            EnterCriticalSection(&ratingLock);
            mode = ratingRequestMode;
            wcsncpy(folder, ratingRequestFolder, 1023);
            folder[1023] = 0;
            LeaveCriticalSection(&ratingLock);

            rating_build(mode, folder);
            applied = request;
            filled = 0;
            pushed = 0;
        }

        if (InterlockedExchange(&ratingLoop, 0)) {
            ratingStepCount = ratingCount = 0;
            ratingSum = ratingValue = ratingBurst = 0.0;
        }

        int current = (int)g.rate;
        if (current != rate || !block) {
            rate = current;
            hop = rate / 1000;
            if (hop < 1) hop = 1;
            step = rate / 10;
            if (step < hop) step = hop;
            free(block);
            block = malloc((size_t)step * sizeof *block);
            if (!block) break;
            filled = 0;
        }

        while (filled < step) {
            int taken = rating_take(block + filled, step - filled);
            if (!taken) break;
            filled += taken;
        }
        if (filled < step) continue;
        filled = 0;

        double elapsed = rating_elapsed();
        double sumBefore = ratingScores.total_score_sum, ownSum = 0.0;
        int countBefore = ratingScores.score_count, ownCount = 0;

        if (ratingOwn) {
            int active = (int)(pushed - rate / 5);
            if (active < 0) active = 0;
            int window = active - rate * (RATING_WINDOW_MS / 1000);
            if (window < 0) window = 0;
            analyzer_analyze_chunk(ratingOwn, block, step, rate, window / hop, active / hop, chunk);
            pushed += step;
            ownSum = ratingScores.total_score_sum - sumBefore;
            ownCount = ratingScores.score_count - countBefore;
        }
        for (int i = 0; i < ratingCopyCount; i++) rating_copy_step(&ratingCopies[i], elapsed, chunk);

        double sum = ratingScores.total_score_sum - sumBefore;
        int count = ratingScores.score_count - countBefore;

        /* The score is this instance's own side of the last 100 ms: its own audio when
           it has an analyzer of it, otherwise the stems it is playing. */
        if (!ratingOwn) {
            ownSum = sum;
            ownCount = count;
        }
        ratingBurst = ownCount > 0 ? ownSum / ownCount : 0.0;

        if (ratingStepCount == RATING_STEPS) {
            ratingSum -= ratingSteps[ratingStepHead].sum;
            ratingCount -= ratingSteps[ratingStepHead].count;
        } else {
            ratingStepCount++;
        }
        ratingSteps[ratingStepHead].sum = sum;
        ratingSteps[ratingStepHead].count = count;
        ratingStepHead = (ratingStepHead + 1) % RATING_STEPS;
        ratingSum += sum;
        ratingCount += count;

        ratingValue = ratingCount > 0 ? ratingSum / ratingCount : 0.0;
        if (playlistMode && cue && cue->pid == GetCurrentProcessId()) cue->rating = ratingValue;
    }

    free(block);
    free(chunk);
    return 0;
}

static void rating_start(void)
{
    if (ratingThread || (int)g.rate < 1000) return;

    InitializeCriticalSection(&ratingLock);
    InitializeCriticalSection(&recordingLock);
    ratingFeedSize = (int)g.rate * 8;
    ratingFeed = malloc((size_t)ratingFeedSize * sizeof *ratingFeed);
    ratingWake = CreateEventA(NULL, FALSE, FALSE, NULL);
    if (!ratingFeed || !ratingWake) {
        log_error("Out of memory for the analyzer's audio buffer.\n");
        free(ratingFeed);
        ratingFeed = NULL;
        if (ratingWake) {
            CloseHandle(ratingWake);
            ratingWake = NULL;
        }
        return;
    }

    ratingStarted = 1;
    ratingSound = g.soundNumber;
    ratingThread = CreateThread(NULL, 8u << 20, rating_worker, NULL, 0, NULL);
}

static void rating_stop(void)
{
    if (ratingThread) {
        InterlockedExchange(&ratingQuit, 1);
        SetEvent(ratingWake);
        WaitForSingleObject(ratingThread, 5000);
        CloseHandle(ratingThread);
        ratingThread = NULL;
    }
    if (ratingWake) {
        CloseHandle(ratingWake);
        ratingWake = NULL;
    }
    if (ratingStarted) {
        rating_drop();
        ratingStarted = 0;
    }
    free(ratingFeed);
    ratingFeed = NULL;
    ratingCount = ratingHead = ratingTail = 0;
    if (spawnedStems) {
        CloseHandle(spawnedStems);
        spawnedStems = NULL;
    }
}

static void rating_request(int mode, const wchar_t *folder)
{
    EnterCriticalSection(&ratingLock);
    ratingRequestMode = mode;
    wcsncpy(ratingRequestFolder, folder, 1023);
    ratingRequestFolder[1023] = 0;
    InterlockedIncrement(&ratingRequest);
    LeaveCriticalSection(&ratingLock);
}

static void recording_audio(const float *samples, int frames)
{
    if (!recording.active) return;

    EnterCriticalSection(&recordingLock);
    if (!recording.last || recording.last->frames + (size_t)frames > TAKE_CHUNK) {
        TakeChunk *chunk = malloc(sizeof *chunk);
        float *block = malloc((size_t)TAKE_CHUNK * sizeof *block);
        if (!chunk || !block) {
            free(chunk);
            free(block);
            recording.active = 0;
        } else {
            chunk->next = NULL;
            chunk->frames = 0;
            chunk->samples = block;
            if (recording.last) recording.last->next = chunk;
            else recording.first = chunk;
            recording.last = chunk;
        }
    }
    if (recording.active) {
        memcpy(recording.last->samples + recording.last->frames, samples, (size_t)frames * sizeof *samples);
        recording.last->frames += (size_t)frames;
        recording.frames += (drwav_uint64)frames;
    }
    LeaveCriticalSection(&recordingLock);
}

static void make_folders(const wchar_t *path)
{
    wchar_t buffer[1024];
    wcsncpy(buffer, path, 1023);
    buffer[1023] = 0;

    for (wchar_t *p = buffer + 3; *p; p++) {
        if (*p != L'\\') continue;
        *p = 0;
        CreateDirectoryW(buffer, NULL);
        *p = L'\\';
    }
    CreateDirectoryW(buffer, NULL);
}

static void spawn_stems(const wchar_t *folder)
{
    wchar_t exe[MAX_PATH], command[2048 + MAX_PATH];
    if (!GetModuleFileNameW(NULL, exe, MAX_PATH)) return;
    swprintf(command, sizeof command / sizeof *command, L"\"%ls\" \"%ls\"", exe, folder);

    STARTUPINFOW info;
    PROCESS_INFORMATION process;
    memset(&info, 0, sizeof info);
    info.cb = sizeof info;
    if (!CreateProcessW(exe, command, NULL, NULL, FALSE, CREATE_NEW_CONSOLE, NULL, NULL, &info, &process)) {
        log_error("Could not start a stems instance for '%ls' (error %lu).\n", folder, GetLastError());
        return;
    }

    if (spawnedStems) CloseHandle(spawnedStems);
    spawnedStems = process.hProcess;
    CloseHandle(process.hThread);
    log_trace("started stems instance %lu on '%ls'\n", (unsigned long)process.dwProcessId, folder);
}

static int spawned_stems_alive(void)
{
    if (!spawnedStems) return 0;
    if (WaitForSingleObject(spawnedStems, 0) == WAIT_TIMEOUT) return 1;

    CloseHandle(spawnedStems);
    spawnedStems = NULL;
    return 0;
}

/* The stems instance playing a folder, its [stems] path and the rating it derives. */
static int stems_instance(wchar_t *folder, size_t size, double *rating)
{
    open_cue();
    if (!cue) return 0;

    DWORD pid = cue->pid;
    if (!pid || pid == GetCurrentProcessId()) return 0;

    HANDLE process = OpenProcess(SYNCHRONIZE, FALSE, pid);
    if (!process) return 0;

    int alive = WaitForSingleObject(process, 0) == WAIT_TIMEOUT;
    CloseHandle(process);
    if (!alive || !cue->stems[0] || !cue->qpc) return 0;

    if (folder) {
        wcsncpy(folder, cue->stems, size - 1);
        folder[size - 1] = 0;
    }
    if (rating) *rating = cue->rating;
    return 1;
}

static int write_wav(const wchar_t *path, TakeChunk *first, drwav_uint64 frames, unsigned rate,
                     drwav_uint64 offset)
{
    drwav_data_format format;
    memset(&format, 0, sizeof format);
    format.container = drwav_container_riff;
    format.format = DR_WAVE_FORMAT_PCM;
    format.channels = 1;
    format.sampleRate = rate;
    format.bitsPerSample = 16;

    drwav_int16 *block = malloc((size_t)TAKE_CHUNK * sizeof *block);
    drwav file;
    if (!block || !drwav_init_file_write_w(&file, path, &format, NULL)) {
        log_error("Could not write '%ls'.\n", path);
        free(block);
        return 0;
    }

    if (offset) {
        memset(block, 0, (size_t)TAKE_CHUNK * sizeof *block);
        for (drwav_uint64 silent = 0; silent < offset;) {
            drwav_uint64 count = offset - silent;
            if (count > TAKE_CHUNK) count = TAKE_CHUNK;
            drwav_write_pcm_frames(&file, count, block);
            silent += count;
        }
    }

    drwav_uint64 written = 0;
    for (TakeChunk *chunk = first; chunk && written < frames; chunk = chunk->next) {
        size_t count = chunk->frames;
        if (written + count > frames) count = (size_t)(frames - written);
        drwav_f32_to_s16(block, chunk->samples, count);
        drwav_write_pcm_frames(&file, count, block);
        written += count;
    }
    drwav_uninit(&file);
    free(block);
    return 1;
}

/* The take is named after the sound that played it. */
static void recording_name(wchar_t *out, size_t size)
{
    out[0] = 0;
    if (!recording.number || !recording.sound[0]) {
        wcsncpy(out, L"audio", size - 1);
        out[size - 1] = 0;
        return;
    }

    wchar_t sound[128];
    MultiByteToWideChar(CP_ACP, 0, recording.sound, -1, sound, 128);
    for (wchar_t *p = sound; *p; p++)
        if (wcschr(L"\\/:*?\"<>|", *p)) *p = L'-';

    if (sound[0]) swprintf(out, size, L"%d %ls", recording.number, sound);
    else swprintf(out, size, L"%d", recording.number);
}

/* A stem is one voice of the sound its name starts with, and voices collide by mode:
   every drum sample set with every other, a VST preset only with the same sound
   number, and an `audio` take, which carries no sound number, with every other such
   take. Two that collide cannot overlap in the song, so a new take deletes the one it
   replaces. */

enum { MODE_AUDIO, MODE_SAMPLES, MODE_SYNTH };

static void stem_key(const wchar_t *path, wchar_t *key, size_t size)
{
    const wchar_t *name = wcsrchr(path, L'\\');
    name = name ? name + 1 : path;

    size_t length = 0;
    while (length + 1 < size && name[length] >= L'0' && name[length] <= L'9') length++;
    wmemcpy(key, name, length);
    key[length] = 0;
}

static int stem_mode(const wchar_t *path)
{
    wchar_t key[16];
    stem_key(path, key, 16);
    return sound_mode(_wtoi(key));
}

static int stems_collide(const wchar_t *a, const wchar_t *b)
{
    int mode = stem_mode(a);
    if (mode != stem_mode(b)) return 0;
    if (mode != MODE_SYNTH) return 1;

    wchar_t keyA[16], keyB[16];
    stem_key(a, keyA, 16);
    stem_key(b, keyB, 16);
    return !wcscmp(keyA, keyB);
}

static int active_span(const Track *track, double *begin, double *end)
{
    if (!track->rate || !track->samples) return 0;

    drwav_uint64 on = track->frames, off = 0;
    for (drwav_uint64 i = 0; i < track->frames; i++) {
        float peak = 0.0f;
        for (unsigned c = 0; c < track->channels; c++) {
            float value = fabsf(track->samples[i * track->channels + c]);
            if (value > peak) peak = value;
        }
        if (peak <= TASTE_GATE) continue;
        if (on == track->frames) on = i;
        off = i + 1;
    }
    if (on == track->frames) return 0;

    *begin = (double)on / (double)track->rate;
    *end = (double)off / (double)track->rate;
    return 1;
}

static int file_span(const wchar_t *path, double *begin, double *end)
{
    Track track;
    memset(&track, 0, sizeof track);
    if (decode_track(&track, path) != 0) return 0;

    int found = active_span(&track, begin, end);
    free(track.samples);
    return found;
}

static int take_span(TakeChunk *first, drwav_uint64 frames, unsigned rate, drwav_uint64 offset,
                     double *begin, double *end)
{
    if (!rate) return 0;

    drwav_uint64 on = frames, off = 0, at = 0;
    for (TakeChunk *chunk = first; chunk && at < frames; chunk = chunk->next) {
        size_t count = chunk->frames;
        if (at + count > frames) count = (size_t)(frames - at);
        for (size_t i = 0; i < count; i++) {
            if (fabsf(chunk->samples[i]) <= TASTE_GATE) continue;
            if (on == frames) on = at + i;
            off = at + i + 1;
        }
        at += count;
    }
    if (on == frames) return 0;

    *begin = (double)(offset + on) / (double)rate;
    *end = (double)(offset + off) / (double)rate;
    return 1;
}

static void prune_stems(const wchar_t *folder, const wchar_t *path, double begin, double end)
{
    size_t length = wcslen(folder);
    wchar_t *pattern = malloc((length + 3) * sizeof *pattern);
    if (!pattern) return;
    swprintf(pattern, length + 3, L"%ls\\*", folder);

    WIN32_FIND_DATAW entry;
    HANDLE find = FindFirstFileW(pattern, &entry);
    free(pattern);
    if (find == INVALID_HANDLE_VALUE) return;

    do {
        if (entry.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        if (!is_audio_name(entry.cFileName)) continue;

        size_t nameLength = wcslen(entry.cFileName);
        size_t pathLength = length + nameLength + 2;
        wchar_t *other = malloc(pathLength * sizeof *other);
        if (!other) continue;
        swprintf(other, pathLength, L"%ls\\%ls", folder, entry.cFileName);

        double otherBegin, otherEnd;
        if (_wcsicmp(other, path) && stems_collide(path, other) &&
            file_span(other, &otherBegin, &otherEnd)) {
            double otherLead = stem_lead(other);
            if (begin < otherEnd + otherLead && otherBegin + otherLead < end) {
                if (DeleteFileW(other)) log_trace("replaced %ls\n", other);
                else log_error("Could not delete '%ls' (error %lu).\n", other, GetLastError());
            }
        }
        free(other);
    } while (FindNextFileW(find, &entry));
    FindClose(find);
}

static void save_recording(TakeChunk *first, drwav_uint64 frames, unsigned rate, drwav_uint64 offset)
{
    wchar_t stems[1024], folder[1200], path[1400], base[160];
    int shared = stems_instance(stems, 1024, NULL);

    recording_name(base, 160);

    if (shared) wcscpy(folder, stems);
    else {
        SYSTEMTIME at;
        wchar_t stamp[40], project[512];
        GetLocalTime(&at);
        swprintf(stamp, 40, L"%04d-%02d-%02d-%02d-%02d-%02d",
                 at.wYear, at.wMonth, at.wDay, at.wHour, at.wMinute, at.wSecond);
        for (int attempt = 1; attempt < 100; attempt++) {
            if (attempt == 1) swprintf(project, 512, L"%ls\\[%ls]", RATING_PROJECTS, stamp);
            else swprintf(project, 512, L"%ls\\[%ls-%d]", RATING_PROJECTS, stamp, attempt);
            if (GetFileAttributesW(project) == INVALID_FILE_ATTRIBUTES) break;
        }
        swprintf(folder, 1200, L"%ls\\[stems]", project);
        make_folders(folder);
    }

    for (int attempt = 1; attempt < 100; attempt++) {
        if (attempt == 1) swprintf(path, 1400, L"%ls\\%ls.wav", folder, base);
        else swprintf(path, 1400, L"%ls\\%ls %d.wav", folder, base, attempt);
        if (GetFileAttributesW(path) == INVALID_FILE_ATTRIBUTES) break;
    }
    /* The take belongs to the pass of the song it overlaps most, so it is cued from
       that pass's loop point rather than from the loop it happened to start in. */
    double lead = 0.0;
    if (shared && cue && cue->length > 0.0) {
        double phase = (double)offset / (double)rate;
        double duration = (double)frames / (double)rate;
        double length = cue->length;
        int pass = 0;
        double best = 0.0;

        for (int k = 0; k < 64; k++) {
            double begin = (double)k * length, end = begin + length;
            double from = phase > begin ? phase : begin;
            double to = phase + duration < end ? phase + duration : end;
            if (to > from) {
                if (to - from > best) {
                    best = to - from;
                    pass = k;
                }
            } else if (best > 0.0) break;
        }

        lead = phase - (double)pass * length;
    }

    double begin, end;
    if (!take_span(first, frames, rate, 0, &begin, &end)) {
        log_trace("take was silent; not written\n");
        return;
    }
    if (!write_wav(path, first, frames, rate, 0)) return;

    log_trace("recorded %ls\n", path);
    if (!shared) {
        spawn_stems(folder);
        return;
    }

    /* A take that starts before the song's zero moves the zero back for every stem. */
    load_stem_leads();
    set_stem_lead(path, lead);

    double lowest = 0.0;
    for (int i = 0; i < stemLeadCount; i++)
        if (stemLeads[i].lead < lowest) lowest = stemLeads[i].lead;
    if (lowest < 0.0)
        for (int i = 0; i < stemLeadCount; i++) stemLeads[i].lead -= lowest;
    save_stem_leads();

    prune_stems(folder, path, begin + lead - lowest, end + lead - lowest);
}

static void recording_stop(int spawn)
{
    if (!recording.active) return;

    EnterCriticalSection(&recordingLock);
    recording.active = 0;
    TakeChunk *first = recording.first;
    drwav_uint64 frames = recording.frames, offset = recording.offset;
    unsigned rate = recording.rate;
    int jamming = recording.jamming;
    recording.first = recording.last = NULL;
    recording.frames = 0;
    recording.offset = 0;
    LeaveCriticalSection(&recordingLock);

    if (frames && rate) {
        /* A take that jammed into a song is dropped with it, rather than written as a
           project of its own, once that song's instance is gone. */
        if (spawn && (!jamming || stems_instance(NULL, 0, NULL))) save_recording(first, frames, rate, offset);
        else log_trace("discarded %llu recorded frames\n", (unsigned long long)frames);
    }

    for (TakeChunk *chunk = first; chunk;) {
        TakeChunk *next = chunk->next;
        free(chunk->samples);
        free(chunk);
        chunk = next;
    }
}

static void recording_start(void)
{
    int jamming = stems_instance(NULL, 0, NULL);
    double position = jamming ? rating_elapsed() : 0.0;
    if (position < 0.0) position = 0.0;

    recording.number = 0;
    recording.sound[0] = 0;
    if (instrumentMode && g.soundNumber) {
        recording.number = g.soundNumber;
        sound_name(recording.sound, sizeof recording.sound);
    }

    EnterCriticalSection(&recordingLock);
    recording.rate = (unsigned)g.rate;
    recording.frames = 0;
    recording.offset = (drwav_uint64)(position * (double)g.rate);
    recording.jamming = jamming;
    recording.active = 1;
    LeaveCriticalSection(&recordingLock);
}

/* A stems instance records nothing: its group is the recitation. An instrument
   instance with one running judges its jamming group against that instance's
   rating, and otherwise records while its own rating is above zero. */
static void rating_poll(void)
{
    wchar_t folder[1024] = L"";
    double recitation = 0.0;
    int shared = stems_instance(folder, 1024, &recitation);
    int mode = playlistMode ? RATING_RECITATION : shared ? RATING_JAMMING : RATING_SOLO;

    static int jamming;
    if (jamming && mode != RATING_JAMMING) {
        recording_stop(0);
        ratingValue = 0.0;
        ratingBurst = 0.0;
    }
    jamming = mode == RATING_JAMMING;

    int count = shared && cue ? (int)cue->stems_count : 0;
    int generation = shared && cue ? (int)cue->generation : 0;
    if (ratingThread && (mode != ratingRequestedMode ||
                         (shared && (wcscmp(folder, ratingRequestedFolder) || count != ratingRequestedStems ||
                                     generation != ratingRequestedGeneration)))) {
        ratingRequestedMode = mode;
        ratingRequestedStems = count;
        ratingRequestedGeneration = generation;
        wcsncpy(ratingRequestedFolder, folder, 1023);
        ratingRequestedFolder[1023] = 0;
        rating_request(mode, folder);
    }

    if (instrumentMode && g.soundNumber != ratingSound) {
        ratingSound = g.soundNumber;
        if (ratingThread) rating_request(ratingRequestedMode, ratingRequestedFolder);
        recording_stop(1);
    }

    if (playlistMode) {
        if (cue && cue->pid == GetCurrentProcessId()) cue->rating = rating_current();
        return;
    }

    int want;
    if (ratingRequestedMode == RATING_JAMMING) want = rating_current() > recitation;
    else want = rating_current() > 0.0 && !spawned_stems_alive();

    if (want) {
        if (!recording.active) recording_start();
    } else if (recording.active) recording_stop(1);
}

static double playlistElapsed;

static void advance_tracks(void)
{
    playlistElapsed += 1.0 / g.rate;

    int finished = 1;
    for (int i = 0; i < stemCount; i++) {
        Track *track = &stems[i];
        if (track->wait > 0.0) {
            track->wait -= (double)track->rate / g.rate;
            finished = 0;
            continue;
        }
        if (track->position >= track->frames) continue;

        track->fraction += (double)track->rate / g.rate;
        drwav_uint64 whole = (drwav_uint64)track->fraction;
        track->fraction -= (double)whole;
        track->position += whole;
        if (track->position < track->frames) finished = 0;
    }

    if (!finished) return;
    for (int i = 0; i < stemCount; i++) {
        stems[i].position = 0;
        stems[i].fraction = 0;
    }
    playlistElapsed = 0.0;
    send_loop_start();
    InterlockedExchange(&playlistRefresh, 1);
    InterlockedExchange(&ratingLoop, 1);
}

static void render_playlist(float *dst, int outputs, int frames)
{
    for (int i = 0; i < frames; i++) {
        for (int j = 0; j < outputs; j++) {
            float value = 0.0f;
            for (int s = 0; s < stemCount; s++) value += track_sample(&stems[s], j);
            dst[(size_t)j * frames + i] = value;
        }
        advance_tracks();
    }
}

/* How far into the loop the stems have played, in seconds. */
static double playlist_length(void)
{
    double length = 0.0;

    for (int i = 0; i < stemCount; i++) {
        if (!stems[i].rate) continue;
        double lead = stemFiles[i] ? stem_lead(stemFiles[i]) : 0.0;
        double seconds = lead + (double)stems[i].frames / (double)stems[i].rate;
        if (seconds > length) length = seconds;
    }
    return length;
}

static int playlist_stems(void)
{
    int playing = 0;
    for (int i = 0; i < stemCount; i++) if (stems[i].frames) playing++;
    return playing;
}

/* Files dropped into the folder while it plays join the stems at the next loop
   point, where they start from the top of the song with the others, and a stem
   whose file has gone stops playing. */
static void refresh_playlist(void)
{
    if (!playlistFolder) return;

    double position = playlistElapsed;
    int changed = 0;

    load_stem_leads();
    for (int i = 0; i < stemCount; i++) {
        if (!stemFiles[i] || !stems[i].frames) continue;
        stems[i].wait = stem_lead(stemFiles[i]) * (double)stems[i].rate;
        stems[i].position = 0;
        stems[i].fraction = 0.0;
    }

    for (int i = 0; i < stemCount; i++) {
        if (!stemFiles[i] || !stems[i].frames) continue;
        ULONGLONG written = written_time(stemFiles[i]);
        if (written && written == stemWritten[i]) continue;

        stems[i].frames = 0;
        stems[i].position = 0;
        stems[i].fraction = 0.0;

        const wchar_t *base = wcsrchr(stemFiles[i], L'\\');
        char name[128], line[192];
        WideCharToMultiByte(CP_ACP, 0, base ? base + 1 : stemFiles[i], -1, name, sizeof name, NULL, NULL);
        snprintf(line, sizeof line, "  dropped %s, %d stems playing", name, playlist_stems());
        console_line(line);
        log_trace("dropped %ls\n", stemFiles[i]);
        stemFiles[i] = NULL;
        changed = 1;
    }

    size_t length = wcslen(playlistFolder);
    wchar_t *pattern = malloc((length + 3) * sizeof *pattern);
    if (!pattern) return;
    swprintf(pattern, length + 3, L"%ls\\*", playlistFolder);

    WIN32_FIND_DATAW entry;
    HANDLE find = FindFirstFileW(pattern, &entry);
    free(pattern);
    if (find == INVALID_HANDLE_VALUE) return;

    do {
        if (entry.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        if (!is_audio_name(entry.cFileName)) continue;

        size_t nameLength = wcslen(entry.cFileName);
        size_t fileLength = length + nameLength + 2;
        wchar_t *file = malloc(fileLength * sizeof *file);
        if (!file) continue;
        swprintf(file, fileLength, L"%ls\\%ls", playlistFolder, entry.cFileName);

        int known = 0;
        for (int i = 0; i < stemCount && !known; i++)
            known = stemFiles[i] && !_wcsicmp(stemFiles[i], file);
        if (known || stemCount == QUEUE_FILES) {
            free(file);
            continue;
        }

        if (decode_track(&stems[stemCount], file) != 0) {
            free(file);
            continue;
        }

        Track *track = &stems[stemCount++];
        double starts = position - stem_lead(file);
        track->position = starts > 0.0 ? (drwav_uint64)(starts * (double)track->rate) : 0;
        track->wait = starts > 0.0 ? 0.0 : -starts * (double)track->rate;
        track->fraction = 0.0;
        stemFiles[stemCount - 1] = file;
        stemWritten[stemCount - 1] = written_time(file);
        changed = 1;

        char name[128], line[192];
        WideCharToMultiByte(CP_ACP, 0, entry.cFileName, -1, name, sizeof name, NULL, NULL);
        snprintf(line, sizeof line, "  joined %s, %d stems playing", name, playlist_stems());
        console_line(line);
        log_trace("joined %ls\n", file);
    } while (FindNextFileW(find, &entry));
    FindClose(find);

    if (!changed) return;

    if (cue && cue->pid == GetCurrentProcessId()) {
        cue->stems_count = playlist_stems();
        cue->length = playlist_length();
        InterlockedIncrement(&cue->generation);
    }
    if (ratingThread) rating_request(RATING_RECITATION, L"");
}

static void mute_stems(void)
{
    for (int i = 0; i < stemCount; i++)
        InterlockedExchange(&stems[i].muted, stemFiles[i] ? stem_muted(stemFiles[i]) : 0);
}

static void shuffle(int *items, int count)
{
    for (int i = count - 1; i > 0; i--) {
        int j = rand() % (i + 1);
        int swap = items[i];
        items[i] = items[j];
        items[j] = swap;
    }
}

typedef void (*PathVisitor)(const wchar_t *path);

static void scan_tree(const wchar_t *folder, const wchar_t *extension, PathVisitor visit)
{
    size_t folderLength = wcslen(folder);
    wchar_t *pattern = malloc((folderLength + 3) * sizeof(wchar_t));
    if (!pattern) return;
    swprintf(pattern, folderLength + 3, L"%ls\\*", folder);

    WIN32_FIND_DATAW entry;
    HANDLE find = FindFirstFileW(pattern, &entry);
    free(pattern);
    if (find == INVALID_HANDLE_VALUE) return;

    do {
        size_t nameLength = wcslen(entry.cFileName);
        wchar_t *path = malloc((folderLength + nameLength + 2) * sizeof(wchar_t));
        if (!path) continue;
        swprintf(path, folderLength + nameLength + 2, L"%ls\\%ls", folder, entry.cFileName);

        int directory = entry.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY;
        const wchar_t *dot = wcsrchr(entry.cFileName, L'.');
        if (directory && !(entry.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT))
            scan_tree(path, extension, visit);
        else if (!directory && dot && !_wcsicmp(dot, extension)) {
            visit(path);
            continue;
        }
        free(path);
    } while (FindNextFileW(find, &entry));
    FindClose(find);
}

static void scan_audio_tree(const wchar_t *folder, PathVisitor visit)
{
    for (size_t i = 0; i < sizeof audioExtensions / sizeof *audioExtensions; i++)
        scan_tree(folder, audioExtensions[i], visit);
}

static double random_unit(void)
{
    return ((double)rand() * (RAND_MAX + 1.0) + rand()) / ((RAND_MAX + 1.0) * (RAND_MAX + 1.0));
}

static int weighted_pick(const int *taste, int count)
{
    long long total = 0;
    for (int i = 0; i < count; i++) total += taste[i];

    long long ticket = (long long)(random_unit() * (double)total);
    for (int i = 0; i < count; i++)
        if ((ticket -= taste[i]) < 0) return i;
    return count - 1;
}

typedef struct {
    Track track;
    char *name;
} Drum;

static Drum *drums;
static int *drumTaste;
static unsigned char *drumTasted;
static int drumCount, drumCapacity;
static int drumNoteDrum[128], drumVoiceDrum[128];
static Track drumVoices[128];
static float drumNotePeak[128], *drumPeaks;
static size_t drumRootLength;

static int saved_taste(const char *name);

static void add_sample(const wchar_t *path)
{
    if (drumCount == drumCapacity) {
        int capacity = drumCapacity ? drumCapacity * 2 : 64;
        Drum *grown = realloc(drums, (size_t)capacity * sizeof *grown);
        if (!grown) {
            free((void *)path);
            return;
        }
        drums = grown;
        int *tastes = realloc(drumTaste, (size_t)capacity * sizeof *tastes);
        if (!tastes) {
            free((void *)path);
            return;
        }
        drumTaste = tastes;
        drumCapacity = capacity;
    }

    Drum *drum = &drums[drumCount];
    if (decode_track(&drum->track, path) == 0) {
        const wchar_t *relative = path + drumRootLength;
        if (*relative == L'\\') relative++;
        int length = WideCharToMultiByte(CP_ACP, 0, relative, -1, NULL, 0, NULL, NULL);
        drum->name = length > 0 ? malloc((size_t)length) : NULL;
        if (!drum->name) {
            free(drum->track.samples);
            free((void *)path);
            return;
        }
        WideCharToMultiByte(CP_ACP, 0, relative, -1, drum->name, length, NULL, NULL);
        drumTaste[drumCount] = saved_taste(drum->name);
        drumCount++;
    }
    free((void *)path);
}

static int drumSeed;
static uint64_t drumState;

static void seed_drums(int seed)
{
    if (!seed) seed = (int)(((unsigned)rand() << 16 ^ (unsigned)rand()) & 0x7fffffffu);
    drumSeed = seed ? seed : 1;
    drumState = (uint64_t)(unsigned)drumSeed;
}

/* A drum set is drawn from its own seed, so the seed in song.json draws the same set again. */
static int drum_pick(const int *taste, int count)
{
    long long total = 0;
    for (int i = 0; i < count; i++) total += taste[i];

    drumState += 0x9e3779b97f4a7c15ull;
    uint64_t bits = drumState;
    bits = (bits ^ (bits >> 30)) * 0xbf58476d1ce4e5b9ull;
    bits = (bits ^ (bits >> 27)) * 0x94d049bb133111ebull;
    bits ^= bits >> 31;

    long long ticket = (long long)((double)(bits >> 11) * (1.0 / 9007199254740992.0) * (double)total);
    for (int i = 0; i < count; i++)
        if ((ticket -= taste[i]) < 0) return i;
    return count - 1;
}

static void assign_drums(void)
{
    for (int note = 0; note < 128; note++) drumNoteDrum[note] = drum_pick(drumTaste, drumCount);
    memset(drumVoices, 0, sizeof drumVoices);
}

static void load_drums(const char *folder)
{
    wchar_t *wide = widen(folder);
    wchar_t *path = wide ? long_path(wide) : NULL;
    free(wide);
    if (!path || !is_folder_w(path)) {
        log_error("samples_path '%s' is not a folder; drum samples are off.\n", folder);
        free(path);
        return;
    }

    drumRootLength = wcslen(path);
    scan_audio_tree(path, add_sample);
    free(path);
    if (!drumCount) {
        log_error("No audio files under samples_path '%s'.\n", folder);
        return;
    }

    drumPeaks = calloc((size_t)drumCount, sizeof *drumPeaks);
    drumTasted = calloc((size_t)drumCount, 1);
    seed_drums(0);
    assign_drums();
}

static void trigger_drum(int note)
{
    if (!drumCount) return;
    drumVoiceDrum[note] = drumNoteDrum[note];
    drumVoices[note] = drums[drumVoiceDrum[note]].track;
    drumVoices[note].position = 0;
    drumVoices[note].fraction = 0;
    drumVoices[note].wait = 0.0;
}

static void render_drums(float *dst, int outputs, int frames)
{
    memset(drumNotePeak, 0, sizeof drumNotePeak);

    for (int i = 0; i < frames; i++) {
        for (int note = 0; note < 128; note++) {
            Track *voice = &drumVoices[note];
            if (!voice->samples || voice->position >= voice->frames) continue;

            float peak = drumNotePeak[note];
            for (int j = 0; j < outputs; j++) {
                float value = track_sample(voice, j);
                dst[(size_t)j * frames + i] += value;
                float magnitude = fabsf(value);
                if (magnitude > peak) peak = magnitude;
            }
            drumNotePeak[note] = peak;

            voice->fraction += (double)voice->rate / g.rate;
            drwav_uint64 whole = (drwav_uint64)voice->fraction;
            voice->fraction -= (double)whole;
            voice->position += whole;
        }
    }

    if (!drumPeaks || !drumTasted) return;

    int touched[128], count = 0;
    for (int note = 0; note < 128; note++) {
        int drum = drumVoiceDrum[note];
        float peak = drumNotePeak[note];
        if (peak == 0.0f || peak < drumPeaks[drum]) continue;
        if (drumPeaks[drum] == 0.0f) touched[count++] = drum;
        drumPeaks[drum] = peak;
    }

    for (int i = 0; i < count; i++) {
        if (drumPeaks[touched[i]] > TASTE_GATE) drumTasted[touched[i]] = 1;
        drumPeaks[touched[i]] = 0.0f;
    }
}

static HMODULE vstLibraries[VST_MAX_PLUGINS];
static char vstNames[VST_MAX_PLUGINS][VST_NAME];
static int vstLibraryCount, vstActiveLibrary = -1, vstOutputCount, vstProgram, vstPresetCount;
static AEffect *vstInstances[VST_MAX_PLUGINS];
static int vstLibraryPresets[VST_MAX_PLUGINS], vstLibraryLow[VST_MAX_PLUGINS], vstLibraryHigh[VST_MAX_PLUGINS];
static int *vstTastes[VST_MAX_PLUGINS];
static int vstTasted, vstTasteLibrary = -1, vstTasteProgram, vstTasteOctave;

static void init_vst_taste(int library);

static int taste_index(int program, int octave)
{
    return program * VST_OCTAVES + octave + VST_MAX_OCTAVE;
}

static void taste_flush(void)
{
    if (vstTasted) {
        vstTasted = 0;
        if (vstTasteLibrary >= 0 && vstTastes[vstTasteLibrary] &&
            vstTasteProgram < vstLibraryPresets[vstTasteLibrary])
            vstTastes[vstTasteLibrary][taste_index(vstTasteProgram, vstTasteOctave)]++;
    }

    if (!drumTasted) return;
    for (int i = 0; i < drumCount; i++) {
        if (!drumTasted[i]) continue;
        drumTasted[i] = 0;
        drumTaste[i]++;
    }
}

static void taste_tick(int frames)
{
    static long long tasteFrames;

    tasteFrames += frames;
    if (tasteFrames < (long long)g.rate) return;
    tasteFrames -= (long long)g.rate;
    taste_flush();
}

static VstStats *find_vst_stats(const char *name, const char *file)
{
    size_t length = strlen(file);
    if (length > 4 && !_stricmp(file + length - 4, ".dll")) length -= 4;

    for (int i = 0; i < vstStatsCount; i++) {
        if (!_stricmp(vstStats[i].name, name)) return &vstStats[i];
        if (strlen(vstStats[i].name) == length && !_strnicmp(vstStats[i].name, file, length))
            return &vstStats[i];
    }
    return NULL;
}

static float *vstZeros, *vstOutputMemory;
static float **vstInputs, **vstOutputs;

static intptr_t host_callback(AEffect *effect, int32_t opcode, int32_t index, intptr_t value,
                              void *ptr, float opt)
{
    (void)effect;
    (void)index;
    (void)value;
    (void)opt;

    switch (opcode) {
    case audioMasterVersion: return 2400;
    case audioMasterGetSampleRate: return (intptr_t)g.rate;
    case audioMasterGetBlockSize: return g.bufferSize;
    case audioMasterGetCurrentProcessLevel: return 2;
    case audioMasterGetVendorVersion: return 1;
    case audioMasterGetVendorString:
    case audioMasterGetProductString:
        if (ptr) strcpy((char *)ptr, "restless_virtuoso");
        return 1;
    default: return 0;
    }
}

static AEffect *create_effect(HMODULE library)
{
    AEffect *(*entry)(VstHostCallbackProc) = (void *)GetProcAddress(library, "VSTPluginMain");
    if (!entry) entry = (void *)GetProcAddress(library, "main");
    if (!entry) return NULL;

    AEffect *effect = entry(host_callback);
    if (!effect || effect->magic != kVstEffectMagic) return NULL;
    return effect;
}

static void vst_free_buffers(void)
{
    free(vstZeros);
    free(vstInputs);
    free(vstOutputMemory);
    free(vstOutputs);
    vstZeros = NULL;
    vstInputs = NULL;
    vstOutputMemory = NULL;
    vstOutputs = NULL;
}

static int vst_allocate(int frames)
{
    int slots = vstEffect->numInputs > 2 ? vstEffect->numInputs : 2;

    vstZeros = calloc((size_t)frames, sizeof(float));
    vstInputs = malloc((size_t)slots * sizeof *vstInputs);
    vstOutputs = malloc((size_t)vstOutputCount * sizeof *vstOutputs);
    vstOutputMemory = malloc((size_t)vstOutputCount * frames * sizeof *vstOutputMemory);
    if (!vstZeros || !vstInputs || !vstOutputs || !vstOutputMemory) {
        vst_free_buffers();
        return -1;
    }

    for (int i = 0; i < slots; i++) vstInputs[i] = vstZeros;
    for (int j = 0; j < vstOutputCount; j++) vstOutputs[j] = vstOutputMemory + (size_t)j * frames;
    return 0;
}

static int vst_start(void)
{
    if (!vstEffect) return 0;

    vstEffect->dispatcher(vstEffect, effSetSampleRate, 0, 0, NULL, (float)g.rate);
    vstEffect->dispatcher(vstEffect, effSetBlockSize, 0, g.bufferSize, NULL, 0);
    vstEffect->dispatcher(vstEffect, effMainsChanged, 0, 1, NULL, 0);
    if (vst_allocate((int)g.bufferSize) != 0) {
        log_error("Out of memory for the VST buffers.\n");
        return -1;
    }
    return 0;
}

static void vst_stop(void)
{
    if (!vstEffect) return;
    vstEffect->dispatcher(vstEffect, effMainsChanged, 0, 0, NULL, 0);
    vst_free_buffers();
}

static void effect_name(AEffect *effect, char *name, size_t size)
{
    name[0] = 0;
    effect->dispatcher(effect, effGetEffectName, 0, 0, name, 0);
    name[size - 1] = 0;
}

/* Some plugins ignore effSetProgram, so the chosen preset also arrives as raw MIDI:
   bank select MSB and LSB for presets past 127, then the program change. */
static void send_vst_program_change(int program)
{
    VstMidiEvent midi[3];
    struct {
        VstInt32 numEvents;
        VstIntPtr reserved;
        VstEvent *events[3];
    } events;
    int bank = program / 128;

    memset(midi, 0, sizeof midi);
    for (int i = 0; i < 3; i++) {
        midi[i].type = kVstMidiType;
        midi[i].byteSize = sizeof midi[i];
    }
    midi[0].midiData[0] = (char)0xB0;
    midi[0].midiData[2] = (char)(bank >> 7);
    midi[1].midiData[0] = (char)0xB0;
    midi[1].midiData[1] = 32;
    midi[1].midiData[2] = (char)(bank & 0x7f);
    midi[2].midiData[0] = (char)0xC0;
    midi[2].midiData[1] = (char)(program % 128);

    memset(&events, 0, sizeof events);
    events.numEvents = 3;
    for (int i = 0; i < 3; i++) events.events[i] = (VstEvent *)&midi[i];
    vstEffect->dispatcher(vstEffect, effProcessEvents, 0, 0, &events, 0);
}

/* One instance per plugin is kept for the life of the program: a sound change
   activates an instance that is already open instead of destroying it and building
   it again, which some plugins report on the console and all of them pay for in load
   time. Only the active instance is processed. */
static int start_vst(int library, int program, int octave)
{
    int fresh = vstInstances[library] == NULL;
    AEffect *effect = fresh ? create_effect(vstLibraries[library]) : vstInstances[library];
    if (!effect) return -1;

    if (fresh) {
        if (!effect->dispatcher || (!(effect->flags & effFlagsIsSynth) && effect->numInputs > 0) ||
            effect->numOutputs < 1 || effect->numOutputs > VST_MAX_OUTPUTS ||
            (!effect->processReplacing && !effect->process)) {
            effect->dispatcher(effect, effClose, 0, 0, NULL, 0);
            return -1;
        }

        effect->dispatcher(effect, effOpen, 0, 0, NULL, 0);
        vstInstances[library] = effect;
    }

    vstEffect = effect;
    vstActiveLibrary = library;
    vstOutputCount = effect->numOutputs;

    vstPresetCount = vstLibraryPresets[library];
    vstRangeLow = vstLibraryLow[library];
    vstRangeHigh = vstLibraryHigh[library];
    vstProgram = 0;
    vstOctave = octave;
    if (vstPresetCount > 0) {
        if (program >= 0 && program < vstPresetCount) vstProgram = program;
        else {
            int slot = weighted_pick(vstTastes[library], vstPresetCount * VST_OCTAVES);
            vstProgram = slot / VST_OCTAVES;
            vstOctave = slot % VST_OCTAVES - VST_MAX_OCTAVE;
        }
    }
    if (vstPresetCount > 1)
        effect->dispatcher(effect, effSetProgram, 0, vstProgram, NULL, 0);

    if (vst_start() != 0) {
        vst_stop();
        vstEffect = NULL;
        vstActiveLibrary = -1;
        if (fresh) {
            effect->dispatcher(effect, effClose, 0, 0, NULL, 0);
            vstInstances[library] = NULL;
        }
        return -1;
    }
    if (vstPresetCount > 1) send_vst_program_change(vstProgram);
    return 0;
}

static void add_vst(const wchar_t *path)
{
    if (vstLibraryCount == VST_MAX_PLUGINS) {
        free((void *)path);
        return;
    }

    HMODULE library = LoadLibraryExW(path, NULL, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!library) log_error("Could not load '%ls'.\n", path);
    else if (GetProcAddress(library, "VSTPluginMain") || GetProcAddress(library, "main")) {
        const wchar_t *base = wcsrchr(path, L'\\');
        wchar_t file[VST_NAME];
        wcsncpy(file, base ? base + 1 : path, VST_NAME - 1);
        file[VST_NAME - 1] = 0;
        WideCharToMultiByte(CP_ACP, 0, file, -1, vstNames[vstLibraryCount], VST_NAME, NULL, NULL);
        vstLibraries[vstLibraryCount] = library;
        init_vst_taste(vstLibraryCount);
        vstLibraryCount++;
    } else FreeLibrary(library);

    free((void *)path);
}

static const char *json_value(const char *text, const char *key)
{
    size_t length = strlen(key);
    for (const char *at = text; (at = strchr(at, '"')); at++) {
        at++;
        if (strncmp(at, key, length) || at[length] != '"') continue;
        at += length + 1;
        while (*at == ' ' || *at == '\t' || *at == '\r' || *at == '\n') at++;
        if (*at++ != ':') continue;
        while (*at == ' ' || *at == '\t' || *at == '\r' || *at == '\n') at++;
        return at;
    }
    return NULL;
}

static const char *skip_space(const char *at)
{
    while (*at == ' ' || *at == '\t' || *at == '\r' || *at == '\n') at++;
    return at;
}

static const char *json_text(const char *at, char *out, size_t size)
{
    if (*at != '"') return NULL;

    size_t length = 0;
    for (at++; *at && *at != '"'; at++) {
        char value = *at;
        if (value == '\\' && at[1]) value = *++at;
        if (length + 1 < size) out[length++] = value;
    }
    if (*at != '"') return NULL;
    out[length] = 0;
    return at + 1;
}

typedef struct {
    char *name;
    int taste;
} TasteEntry;

typedef struct {
    int number, octave, taste;
} PresetEntry;

typedef struct {
    char *name;
    PresetEntry *entries;
    int count;
} PresetTaste;

static TasteEntry *fileDrums;
static int fileDrumCount, fileDrumCapacity;
static PresetTaste *filePresets;
static int filePresetCount, filePresetCapacity;
static const char *settingsPath;
static Options *settingsOptions;

static int saved_taste(const char *name)
{
    for (int i = 0; i < fileDrumCount; i++)
        if (!strcmp(fileDrums[i].name, name)) return fileDrums[i].taste;
    return TASTE_START;
}

static void add_file_drum(const char *name, int taste)
{
    if (fileDrumCount == fileDrumCapacity) {
        int capacity = fileDrumCapacity ? fileDrumCapacity * 2 : 64;
        TasteEntry *grown = realloc(fileDrums, (size_t)capacity * sizeof *grown);
        if (!grown) return;
        fileDrums = grown;
        fileDrumCapacity = capacity;
    }
    char *copy = malloc(strlen(name) + 1);
    if (!copy) return;
    strcpy(copy, name);
    fileDrums[fileDrumCount].name = copy;
    fileDrums[fileDrumCount].taste = taste;
    fileDrumCount++;
}

static void add_file_preset(const char *name, PresetEntry *entries, int count)
{
    if (filePresetCount == filePresetCapacity) {
        int capacity = filePresetCapacity ? filePresetCapacity * 2 : 32;
        PresetTaste *grown = realloc(filePresets, (size_t)capacity * sizeof *grown);
        if (!grown) {
            free(entries);
            return;
        }
        filePresets = grown;
        filePresetCapacity = capacity;
    }
    char *copy = malloc(strlen(name) + 1);
    if (!copy) {
        free(entries);
        return;
    }
    strcpy(copy, name);
    filePresets[filePresetCount].name = copy;
    filePresets[filePresetCount].entries = entries;
    filePresets[filePresetCount].count = count;
    filePresetCount++;
}

static void load_drum_tastes(const char *object)
{
    const char *at = object + 1;
    for (;;) {
        char name[512];
        at = skip_space(at);
        at = json_text(at, name, sizeof name);
        if (!at) return;
        at = skip_space(at);
        if (*at++ != ':') return;
        at = skip_space(at);

        char *end = NULL;
        long taste = strtol(at, &end, 10);
        if (end == at) return;
        add_file_drum(name, (int)taste);

        at = skip_space(end);
        if (*at != ',') return;
        at++;
    }
}

static void load_preset_tastes(const char *object)
{
    const char *at = object + 1;
    for (;;) {
        char name[VST_NAME];
        at = skip_space(at);
        at = json_text(at, name, sizeof name);
        if (!at) return;
        at = skip_space(at);
        if (*at++ != ':') return;
        at = skip_space(at);
        if (*at++ != '{') return;

        PresetEntry *entries = NULL;
        int count = 0, capacity = 0;
        for (;;) {
            char key[24];
            at = skip_space(at);
            if (*at == '}') {
                at++;
                break;
            }

            at = json_text(at, key, sizeof key);
            int number = 0, octave = 0;
            if (!at || sscanf(key, "%d/%d", &number, &octave) != 2) {
                free(entries);
                return;
            }
            at = skip_space(at);
            if (*at++ != ':') {
                free(entries);
                return;
            }
            at = skip_space(at);

            char *end = NULL;
            long taste = strtol(at, &end, 10);
            if (end == at) {
                free(entries);
                return;
            }
            if (count == capacity) {
                int grown = capacity ? capacity * 2 : 16;
                PresetEntry *values = realloc(entries, (size_t)grown * sizeof *values);
                if (!values) {
                    free(entries);
                    return;
                }
                entries = values;
                capacity = grown;
            }
            entries[count].number = number;
            entries[count].octave = octave;
            entries[count++].taste = (int)taste;

            at = skip_space(end);
            if (*at == ',') at++;
            else if (*at != '}') {
                free(entries);
                return;
            }
        }

        if (count) add_file_preset(name, entries, count);
        else free(entries);

        at = skip_space(at);
        if (*at != ',') return;
        at++;
    }
}

static void load_taste(const char *text)
{
    const char *taste = json_value(text, "taste");
    if (!taste || *taste != '{') return;

    const char *samples = json_value(taste, "samples");
    if (samples && *samples == '{') load_drum_tastes(samples);

    const char *synths = json_value(taste, "synths");
    if (synths && *synths == '{') load_preset_tastes(synths);
}

static void init_vst_taste(int library)
{
    VstStats *stats = find_vst_stats("", vstNames[library]);

    if (!stats || stats->presets <= 0) {
        AEffect *effect = create_effect(vstLibraries[library]);
        if (effect && effect->dispatcher) {
            char name[VST_NAME];
            effect_name(effect, name, sizeof name);
            VstStats *named = find_vst_stats(name, vstNames[library]);
            if (named && named->presets > 0) stats = named;
            vstLibraryPresets[library] = stats && stats->presets > 0 ? stats->presets : effect->numPrograms;
            effect->dispatcher(effect, effClose, 0, 0, NULL, 0);
        }
    } else vstLibraryPresets[library] = stats->presets;

    vstLibraryLow[library] = stats ? stats->low : 0;
    vstLibraryHigh[library] = stats ? stats->high : 127;

    /* A wrap over a span that is not whole octaves changes the pitch class, so the
       range is taken inward to the nearest one. */
    int span = vstLibraryHigh[library] - vstLibraryLow[library] + 1;
    if (span >= 12) {
        int excess = span % 12;
        vstLibraryLow[library] += excess / 2;
        vstLibraryHigh[library] -= excess - excess / 2;
    }

    int count = vstLibraryPresets[library];
    if (count <= 0) return;

    int slots = count * VST_OCTAVES;
    int *taste = malloc((size_t)slots * sizeof *taste);
    if (!taste) {
        vstLibraryPresets[library] = 0;
        return;
    }
    for (int i = 0; i < slots; i++) taste[i] = TASTE_START;

    for (int i = 0; i < filePresetCount; i++) {
        if (_stricmp(filePresets[i].name, vstNames[library])) continue;
        for (int j = 0; j < filePresets[i].count; j++) {
            PresetEntry *entry = &filePresets[i].entries[j];
            if (entry->number < 1 || entry->number > count || entry->octave < -VST_MAX_OCTAVE ||
                entry->octave > VST_MAX_OCTAVE)
                continue;
            taste[taste_index(entry->number - 1, entry->octave)] = entry->taste;
        }
        break;
    }
    vstTastes[library] = taste;
}

static void json_write_text(FILE *file, const char *text)
{
    fputc('"', file);
    for (; *text; text++) {
        if (*text == '\\' || *text == '"') fputc('\\', file);
        fputc(*text, file);
    }
    fputc('"', file);
}

static int write_drums(FILE *file, int keep)
{
    if (keep) {
        for (int i = 0; i < fileDrumCount; i++) {
            fprintf(file, "%s\n      ", i ? "," : "");
            json_write_text(file, fileDrums[i].name);
            fprintf(file, ": %d", fileDrums[i].taste);
        }
        return fileDrumCount;
    }

    for (int i = 0; i < drumCount; i++) {
        fprintf(file, "%s\n      ", i ? "," : "");
        json_write_text(file, drums[i].name);
        fprintf(file, ": %d", drumTaste[i]);
    }
    return drumCount;
}

static void write_preset_entries(FILE *file, const PresetEntry *entries, int count)
{
    if (!count) {
        fprintf(file, "{}");
        return;
    }

    fprintf(file, "{ ");
    for (int i = 0; i < count; i++)
        fprintf(file, "%s\"%d/%+d\": %d", i ? ", " : "", entries[i].number, entries[i].octave,
                entries[i].taste);
    fprintf(file, " }");
}

static int write_presets(FILE *file, int keep)
{
    if (keep) {
        for (int i = 0; i < filePresetCount; i++) {
            fprintf(file, "%s\n      ", i ? "," : "");
            json_write_text(file, filePresets[i].name);
            fprintf(file, ": ");
            write_preset_entries(file, filePresets[i].entries, filePresets[i].count);
        }
        return filePresetCount;
    }

    int written = 0;
    for (int i = 0; i < vstLibraryCount; i++) {
        int count = vstLibraryPresets[i];
        if (!count || !vstTastes[i]) continue;
        fprintf(file, "%s\n      ", written++ ? "," : "");
        json_write_text(file, vstNames[i]);
        fprintf(file, ": { ");
        for (int j = 0; j < count * VST_OCTAVES; j++)
            fprintf(file, "%s\"%d/%+d\": %d", j ? ", " : "", j / VST_OCTAVES + 1,
                    j % VST_OCTAVES - VST_MAX_OCTAVE, vstTastes[i][j]);
        fprintf(file, " }");
    }
    return written;
}

static int save_settings(const char *path, const Options *options)
{
    FILE *file = fopen(path, "wb");
    if (!file) {
        log_error("Could not save settings '%s'.\n", path);
        return -1;
    }

    taste_flush();
    fprintf(file, "{\n  \"instrument\": ");
    json_write_text(file, options->instrument);
    fprintf(file, ",\n  \"input-channel\": %d,\n", options->inputChannel);
    if (options->samplesPath[0]) {
        fprintf(file, "  \"samples_path\": ");
        json_write_text(file, options->samplesPath);
        fprintf(file, ",\n");
    }
    if (options->vstsPath[0]) {
        fprintf(file, "  \"vsts_path\": ");
        json_write_text(file, options->vstsPath);
        fprintf(file, ",\n");
    }
    if (options->randomScale) fprintf(file, "  \"random_scale\": 1,\n");
    fprintf(file, "  \"samples_ratio\": %d,\n  \"taste\": {\n    \"samples\": {", options->samplesRatio);

    int drumsWritten = write_drums(file, options->samplesPath[0] && !drumCount);
    fprintf(file, "%s},\n    \"synths\": {", drumsWritten ? "\n    " : "");
    int presetsWritten = write_presets(file, options->vstsPath[0] && !vstLibraryCount);
    fprintf(file, "%s}\n  }\n}\n", presetsWritten ? "\n    " : "");

    int failed = ferror(file);
    fclose(file);
    if (failed) {
        log_error("Could not write settings '%s'.\n", path);
        return -1;
    }
    return 0;
}

static void load_vst_stats(const wchar_t *folder)
{
    static const wchar_t *names[] = {L"vsti_stats.txt", L"vst_stats.txt"};
    wchar_t file[32768];
    FILE *input = NULL;
    for (int i = 0; i < 2 && !input; i++) {
        swprintf(file, 32768, L"%ls\\%ls", folder, names[i]);
        input = _wfopen(file, L"rb");
    }
    if (!input) return;

    fseek(input, 0, SEEK_END);
    long size = ftell(input);
    fseek(input, 0, SEEK_SET);
    char *text = size > 0 ? malloc((size_t)size + 1) : NULL;
    if (text) text[fread(text, 1, (size_t)size, input)] = 0;
    fclose(input);
    if (!text) return;

    char *at = text;
    while (vstStatsCount < VST_MAX_PLUGINS && (at = strchr(at, '"'))) {
        char *name = ++at;
        char *nameEnd = strchr(name, '"');
        if (!nameEnd) break;
        at = nameEnd + 1;

        char *value = nameEnd + 1;
        while (*value == ' ' || *value == '\t' || *value == '\r' || *value == '\n') value++;
        if (*value++ != ':') continue;
        while (*value == ' ' || *value == '\t' || *value == '\r' || *value == '\n') value++;
        if (*value != '{') continue;

        char *objectEnd = strchr(value, '}');
        if (!objectEnd) break;
        char brace = *objectEnd;
        *objectEnd = 0;

        VstStats *stats = &vstStats[vstStatsCount++];
        size_t length = (size_t)(nameEnd - name);
        if (length >= VST_NAME) length = VST_NAME - 1;
        memcpy(stats->name, name, length);
        stats->name[length] = 0;
        if (length > 4 && !_stricmp(stats->name + length - 4, ".dll")) stats->name[length - 4] = 0;

        const char *presets = json_value(value, "presets");
        const char *range = json_value(value, "range");
        stats->presets = presets ? atoi(presets) : 0;
        stats->low = 0;
        stats->high = 127;
        if (range && *range == '[') {
            const char *comma = strchr(range, ',');
            stats->low = atoi(range + 1);
            stats->high = comma ? atoi(comma + 1) : stats->low;
            if (stats->low < 0) stats->low = 0;
            if (stats->low > 127) stats->low = 127;
            if (stats->high < 0) stats->high = 0;
            if (stats->high > 127) stats->high = 127;
            if (stats->low > stats->high) {
                int swap = stats->low;
                stats->low = stats->high;
                stats->high = swap;
            }
        }

        *objectEnd = brace;
        at = objectEnd + 1;
    }
    free(text);
}

static void load_vsts(const char *folder)
{
    wchar_t *wide = widen(folder);
    wchar_t *path = wide ? long_path(wide) : NULL;
    free(wide);
    if (!path || !is_folder_w(path)) {
        log_error("vsts_path '%s' is not a folder; no VST is used.\n", folder);
        free(path);
        return;
    }

    load_vst_stats(path);
    scan_tree(path, L".dll", add_vst);
    free(path);
    if (!vstLibraryCount)
        log_error("No VST plugins under vsts_path '%s'.\n", folder);
}

static void render_vst(float *dst, int outputs, int frames)
{
    if (!vstEffect) return;

    if (midiEvents.numEvents)
        vstEffect->dispatcher(vstEffect, effProcessEvents, 0, 0, (VstEvents *)&midiEvents, 0);

    if (vstEffect->processReplacing) {
        vstEffect->processReplacing(vstEffect, vstInputs, vstOutputs, frames);
    } else {
        memset(vstOutputMemory, 0, (size_t)vstOutputCount * frames * sizeof *vstOutputMemory);
        vstEffect->process(vstEffect, vstInputs, vstOutputs, frames);
    }

    float peak = 0.0f;
    for (int j = 0; j < outputs; j++) {
        const float *source = vstOutputs[j % vstOutputCount];
        for (int i = 0; i < frames; i++) {
            float value = source[i];
            dst[(size_t)j * frames + i] += value;
            float magnitude = fabsf(value);
            if (magnitude > peak) peak = magnitude;
        }
    }

    if (!vstTasted && peak > TASTE_GATE && vstActiveLibrary >= 0 && vstTastes[vstActiveLibrary] &&
        vstProgram < vstLibraryPresets[vstActiveLibrary]) {
        vstTasted = 1;
        vstTasteLibrary = vstActiveLibrary;
        vstTasteProgram = vstProgram;
        vstTasteOctave = vstOctave;
    }
}

static void choose_sound(int samplesRatio)
{
    g.samplesSound = drumCount && (!vstLibraryCount || rand() % 100 < samplesRatio);
}

static void render_instruments(float *dst, int outputs, int frames)
{
    memset(dst, 0, (size_t)outputs * frames * sizeof *dst);
    if (g.samplesSound) render_drums(dst, outputs, frames);
    else render_vst(dst, outputs, frames);
}

static void suspend_vst_effect(void)
{
    taste_flush();
    vst_stop();
    vstEffect = NULL;
    vstActiveLibrary = -1;
}

static void close_vst_effect(void)
{
    suspend_vst_effect();
    for (int i = 0; i < VST_MAX_PLUGINS; i++) {
        if (!vstInstances[i]) continue;
        vstInstances[i]->dispatcher(vstInstances[i], effClose, 0, 0, NULL, 0);
        vstInstances[i] = NULL;
    }
}

/* One draw over every preset of every plugin at every octave, so a plugin's share of
   the lottery is the sum of its entries' tickets and nothing depends on which plugin
   came up first. */
static int pick_vst_sound(int *library, int *program, int *octave)
{
    long long total = 0;
    for (int i = 0; i < vstLibraryCount; i++) {
        if (!vstTastes[i]) continue;
        for (int j = 0; j < vstLibraryPresets[i] * VST_OCTAVES; j++) total += vstTastes[i][j];
    }
    if (total <= 0) return -1;

    long long ticket = (long long)(random_unit() * (double)total);
    for (int i = 0; i < vstLibraryCount; i++) {
        if (!vstTastes[i]) continue;
        for (int j = 0; j < vstLibraryPresets[i] * VST_OCTAVES; j++)
            if ((ticket -= vstTastes[i][j]) < 0) {
                *library = i;
                *program = j / VST_OCTAVES;
                *octave = j % VST_OCTAVES - VST_MAX_OCTAVE;
                return 0;
            }
    }
    return -1;
}

static int start_drawn_vst(void)
{
    if (!vstLibraryCount) return -1;

    int library = -1, program = -1, octave = 0, order[VST_MAX_PLUGINS], count = 0;

    if (pick_vst_sound(&library, &program, &octave) == 0) order[count++] = library;
    for (int i = 0; i < vstLibraryCount; i++)
        if (i != library) order[count++] = i;
    shuffle(order + (library >= 0), count - (library >= 0));

    for (int i = 0; i < count; i++)
        if (start_vst(order[i], order[i] == library ? program : -1,
                      order[i] == library ? octave : 0) == 0)
            return 0;

    log_error("None of the %d loaded plugins could be started.\n", vstLibraryCount);
    return -1;
}

static void new_vst_sound(void)
{
    if (!vstLibraryCount) return;

    if (g.started) {
        g.asio->vtable->stop(g.asio);
        g.started = 0;
    }

    suspend_vst_effect();
    start_drawn_vst();

    if (g.asio->vtable->start(g.asio) != ASE_OK) {
        log_error("ASIOStart failed after changing the VST.\n");
        InterlockedExchange(&g.quit, 1);
    } else g.started = 1;
}

#define SOUND_MAX 1024
#define SOUND_NAME 128
#define INSTRUMENT_MAX 64

typedef struct {
    volatile LONG lock;
    volatile LONG count;
    char name[SOUND_MAX][SOUND_NAME];
    int seed[SOUND_MAX];
    volatile LONG instrumentCount;
    char instrument[INSTRUMENT_MAX][MAX_PATH];
    int last[INSTRUMENT_MAX];
    volatile LONG pid[INSTRUMENT_MAX];
    volatile LONGLONG beat[INSTRUMENT_MAX];
} SoundList;

static HANDLE soundMapping;
static SoundList *soundList;
static int songReady, songLive;

static void open_sounds(void)
{
    if (soundMapping) return;
    soundMapping = CreateFileMappingA(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, 0, sizeof(SoundList),
                                      "Local\\RestlessVirtuosoSounds");
    soundList = soundMapping ? MapViewOfFile(soundMapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(SoundList)) : NULL;
}

/* One list for every running instance, so a number names the same sound in each take.
   Each shuffled drum set is a different sound, so those always get a new number. */
static int sound_number(const char *name, int unique, int seed)
{
    open_sounds();
    if (!soundList) return 0;

    while (InterlockedCompareExchange(&soundList->lock, 1, 0)) Sleep(0);

    int count = (int)soundList->count, number = 0;
    for (int i = 0; !unique && i < count; i++)
        if (!strcmp(soundList->name[i], name)) { number = i + 1; break; }

    if (!number && count < SOUND_MAX) {
        strncpy(soundList->name[count], name, SOUND_NAME - 1);
        soundList->name[count][SOUND_NAME - 1] = 0;
        soundList->seed[count] = seed;
        number = count + 1;
        soundList->count = count + 1;
    }

    InterlockedExchange(&soundList->lock, 0);
    return number;
}

/* An instrument is named by its MIDI input, the same identity its settings file has. */
static int instrument_slot(const char *name, int create)
{
    open_sounds();
    if (!soundList || !name[0]) return -1;

    while (InterlockedCompareExchange(&soundList->lock, 1, 0)) Sleep(0);

    int count = (int)soundList->instrumentCount, slot = -1;
    for (int i = 0; i < count; i++)
        if (!_stricmp(soundList->instrument[i], name)) { slot = i; break; }

    if (slot < 0 && create && count < INSTRUMENT_MAX) {
        strncpy(soundList->instrument[count], name, MAX_PATH - 1);
        soundList->instrument[count][MAX_PATH - 1] = 0;
        soundList->last[count] = 0;
        slot = count;
        soundList->instrumentCount = count + 1;
    }

    InterlockedExchange(&soundList->lock, 0);
    return slot;
}

static int last_sound_number(void)
{
    int slot = instrumentMode && instrumentName ? instrument_slot(instrumentName, 0) : -1;
    return slot >= 0 ? soundList->last[slot] : 0;
}

static void remember_sound(int number)
{
    int slot = instrumentMode && instrumentName ? instrument_slot(instrumentName, 1) : -1;
    if (slot >= 0) soundList->last[slot] = number;
}

/* A running instrument instance leaves its sound and a heartbeat in its slot, so a
   stems instance can mute the stems that sound already covers and a crashed instance
   stops covering them within a couple of ticks. */
static void announce_instrument(void)
{
    if (!instrumentMode || !instrumentName) return;
    int slot = instrument_slot(instrumentName, 1);
    if (slot < 0) return;

    while (InterlockedCompareExchange(&soundList->lock, 1, 0)) Sleep(0);
    soundList->pid[slot] = GetCurrentProcessId();
    soundList->beat[slot] = (LONGLONG)GetTickCount64();
    InterlockedExchange(&soundList->lock, 0);
}

static void withdraw_instrument(void)
{
    open_sounds();
    if (!soundList || !instrumentMode || !instrumentName) return;
    int slot = instrument_slot(instrumentName, 0);
    if (slot >= 0) soundList->pid[slot] = 0;
}

static int sound_mode(int number)
{
    open_sounds();
    if (!soundList || number < 1 || number > (int)soundList->count) return MODE_AUDIO;

    char name[SOUND_NAME];
    memcpy(name, soundList->name[number - 1], SOUND_NAME);
    name[SOUND_NAME - 1] = 0;
    if (strstr(name, " drum samples")) return MODE_SAMPLES;
    if (!strncmp(name, "VST ", 4)) return MODE_SYNTH;
    return MODE_AUDIO;
}

/* Whether a running instrument instance is playing a sound this stem collides with. */
static int stem_muted(const wchar_t *file)
{
    open_sounds();
    if (!soundList) return 0;

    wchar_t key[16];
    stem_key(file, key, 16);
    int number = _wtoi(key), mode = sound_mode(number);

    ULONGLONG now = GetTickCount64();
    for (int i = 0; i < (int)soundList->instrumentCount; i++) {
        if (!soundList->pid[i] || now - (ULONGLONG)soundList->beat[i] > 2000) continue;

        int playing = soundList->last[i];
        if (sound_mode(playing) != mode) continue;
        if (mode != MODE_SYNTH || playing == number) return 1;
    }
    return 0;
}

/* Length of the stems folder's parent, its separator included. */
static size_t project_root(const wchar_t *folder)
{
    size_t length = wcslen(folder);
    while (length > 3 && (folder[length - 1] == L'\\' || folder[length - 1] == L'/')) length--;
    while (length > 3 && folder[length - 1] != L'\\' && folder[length - 1] != L'/') length--;
    return length;
}

/* song.json sits beside [palettes], one folder above the stems folder. An instrument
   takes the folder from the cue the stems instance publishes. */
static int song_path(wchar_t *out, size_t size)
{
    open_cue();
    const wchar_t *from = playlistMode ? playlistFolder : cue && cue->folder[0] ? cue->folder : NULL;
    if (!from) return -1;

    size_t length = project_root(from);
    if (length <= 3 || length + 10 > size) return -1;

    wmemcpy(out, from, length);
    wcscpy(out + length, L"song.json");
    return 0;
}

/* The mix is named after the project folder itself. */
static int mix_path(wchar_t *out, size_t size)
{
    if (!playlistFolder) return -1;

    size_t root = project_root(playlistFolder);
    size_t end = root;
    while (end > 3 && (playlistFolder[end - 1] == L'\\' || playlistFolder[end - 1] == L'/')) end--;
    size_t start = end;
    while (start > 3 && playlistFolder[start - 1] != L'\\' && playlistFolder[start - 1] != L'/') start--;
    if (end <= start || root + (end - start) + 5 > size) return -1;

    wmemcpy(out, playlistFolder, root);
    wmemcpy(out + root, playlistFolder + start, end - start);
    wcscpy(out + root + (end - start), L".wav");
    return 0;
}

static const char *song_field(const char *at, char *key, size_t size)
{
    at = json_text(skip_space(at), key, size);
    if (!at) return NULL;

    at = skip_space(at);
    if (*at++ != ':') return NULL;
    return skip_space(at);
}

static size_t json_span(const char *value)
{
    const char *at = skip_space(value);

    if (*at == '"') {
        const char *start = at++;
        while (*at && *at != '"') {
            if (*at == '\\' && at[1]) at++;
            at++;
        }
        return (size_t)((*at ? at + 1 : at) - start);
    }

    if (*at == '{' || *at == '[') {
        char open = *at, close = open == '{' ? '}' : ']';
        const char *start = at;
        int depth = 0, quoted = 0;
        for (; *at; at++) {
            if (quoted) {
                if (*at == '\\' && at[1]) at++;
                else if (*at == '"') quoted = 0;
            } else if (*at == '"') quoted = 1;
            else if (*at == open) depth++;
            else if (*at == close && --depth == 0) {
                at++;
                break;
            }
        }
        return (size_t)(at - start);
    }

    const char *start = at;
    while (*at && *at != ',' && *at != '}' && *at != ']') at++;
    return (size_t)(at - start);
}

static char *read_song_text(wchar_t *path, size_t size)
{
    if (song_path(path, size) != 0) return NULL;

    FILE *file = _wfopen(path, L"rb");
    if (!file) return NULL;

    fseek(file, 0, SEEK_END);
    long length = ftell(file);
    fseek(file, 0, SEEK_SET);
    char *text = length >= 0 ? malloc((size_t)length + 1) : NULL;
    if (text) text[fread(text, 1, (size_t)length, file)] = 0;
    fclose(file);
    return text;
}

static void load_stem_leads(void)
{
    stemLeadCount = 0;

    wchar_t path[1100];
    char *text = read_song_text(path, sizeof path / sizeof *path);
    if (!text) return;

    const char *at = json_value(text, "stems");
    at = at ? skip_space(at) : NULL;
    if (at && *at == '{') {
        at++;
        for (;;) {
            char key[STEM_NAME];
            at = skip_space(at);
            if (*at == '}' || !*at) break;

            at = json_text(at, key, sizeof key);
            if (!at) break;

            at = skip_space(at);
            if (*at++ != ':') break;
            at = skip_space(at);

            char *end = NULL;
            double lead = strtod(at, &end);
            if (end == at) break;
            at = end;

            if (stemLeadCount < QUEUE_FILES) {
                MultiByteToWideChar(CP_UTF8, 0, key, -1, stemLeads[stemLeadCount].name, STEM_NAME);
                stemLeads[stemLeadCount].lead = lead;
                stemLeadCount++;
            }

            at = skip_space(at);
            if (*at == ',') at++;
        }
    }
    free(text);
}

static double stem_lead(const wchar_t *file)
{
    const wchar_t *base = wcsrchr(file, L'\\');
    base = base ? base + 1 : file;

    for (int i = 0; i < stemLeadCount; i++)
        if (!_wcsicmp(stemLeads[i].name, base)) return stemLeads[i].lead;
    return 0.0;
}

static void set_stem_lead(const wchar_t *file, double lead)
{
    const wchar_t *base = wcsrchr(file, L'\\');
    base = base ? base + 1 : file;

    for (int i = 0; i < stemLeadCount; i++)
        if (!_wcsicmp(stemLeads[i].name, base)) {
            stemLeads[i].lead = lead;
            return;
        }

    if (stemLeadCount == QUEUE_FILES) return;
    wcsncpy(stemLeads[stemLeadCount].name, base, STEM_NAME - 1);
    stemLeads[stemLeadCount].name[STEM_NAME - 1] = 0;
    stemLeads[stemLeadCount++].lead = lead;
}

static void save_stem_leads(void)
{
    wchar_t path[1100];
    char *text = read_song_text(path, sizeof path / sizeof *path);
    const char *sounds = text ? json_value(text, "sounds") : NULL;
    const char *instruments = text ? json_value(text, "instruments") : NULL;
    size_t soundsLength = sounds ? json_span(sounds) : 0;
    size_t instrumentsLength = instruments ? json_span(instruments) : 0;

    FILE *file = _wfopen(path, L"wb");
    if (!file) {
        log_error("Could not write '%ls'.\n", path);
        free(text);
        return;
    }

    fputs("{\n", file);
    if (soundsLength) {
        fputs("  \"sounds\": ", file);
        fwrite(skip_space(sounds), 1, soundsLength, file);
        fputs(",\n", file);
    }
    if (instrumentsLength) {
        fputs("  \"instruments\": ", file);
        fwrite(skip_space(instruments), 1, instrumentsLength, file);
        fputs(",\n", file);
    }

    fputs("  \"stems\": {", file);
    for (int i = 0; i < stemLeadCount; i++) {
        char name[STEM_NAME * 2];
        WideCharToMultiByte(CP_UTF8, 0, stemLeads[i].name, -1, name, sizeof name, NULL, NULL);
        fputs(i ? ",\n    " : "\n    ", file);
        json_write_text(file, name);
        fprintf(file, ": %.6f", stemLeads[i].lead);
    }
    fprintf(file, "%s}\n}\n", stemLeadCount ? "\n  " : "");

    if (ferror(file)) log_error("Could not write '%ls'.\n", path);
    else log_trace("wrote %ls\n", path);
    fclose(file);
    free(text);
}

/* The sounds are keyed by their number and written in that order, so a number names the
   same sound in every take. A drum sound carries the seed that draws its set. */
static int load_song_sounds(const char *object, char **names, int **seeds)
{
    const char *at = skip_space(object);
    if (*at++ != '{') return 0;

    int count = 0;
    for (;;) {
        char key[16];
        at = skip_space(at);
        if (*at == '}' || !*at) break;

        at = json_text(at, key, sizeof key);
        if (!at) return count;

        at = skip_space(at);
        if (*at++ != ':') return count;

        char name[SOUND_NAME] = "";
        at = json_text(skip_space(at), name, sizeof name);
        if (!at || !name[0]) return count;

        int seed = 0;
        const char *mark = strstr(name, ", seed ");
        if (mark) {
            seed = (int)strtol(mark + 7, NULL, 10);
            name[mark - name] = 0;
        }

        char *grownNames = realloc(*names, (size_t)(count + 1) * SOUND_NAME);
        if (!grownNames) return count;
        *names = grownNames;
        int *grownSeeds = realloc(*seeds, (size_t)(count + 1) * sizeof *grownSeeds);
        if (!grownSeeds) return count;
        *seeds = grownSeeds;
        memcpy(*names + (size_t)count * SOUND_NAME, name, SOUND_NAME);
        (*seeds)[count++] = seed;

        at = skip_space(at);
        if (*at == ',') { at++; continue; }
        break;
    }
    return count;
}

static void load_song_instruments(const char *array)
{
    const char *at = skip_space(array);
    if (*at++ != '[') return;

    for (;;) {
        at = skip_space(at);
        if (*at == ']' || !*at) return;
        if (*at++ != '{') return;

        char instrument[MAX_PATH] = "";
        int number = 0;
        for (;;) {
            char key[16];
            at = song_field(at, key, sizeof key);
            if (!at) return;

            if (!strcmp(key, "instrument")) {
                at = json_text(at, instrument, sizeof instrument);
                if (!at) return;
            } else if (!strcmp(key, "sound")) {
                char *end;
                number = (int)strtol(at, &end, 10);
                if (end == at) return;
                at = end;
            } else return;

            at = skip_space(at);
            if (*at == ',') { at++; continue; }
            if (*at++ != '}') return;
            break;
        }

        if (instrument[0]) {
            int slot = instrument_slot(instrument, 1);
            if (slot >= 0) soundList->last[slot] = number;
        }

        at = skip_space(at);
        if (*at != ',') return;
        at++;
    }
}

/* A song keeps the scale it was played in, so every instrument of it plays that one. */
static void load_song_scale(const char *object)
{
    char name[SCALE_NAME] = "", root[8] = "";
    const char *value = json_value(object, "scale");
    if (!value || !json_text(value, name, sizeof name)) return;
    value = json_value(object, "root");
    if (!value || !json_text(value, root, sizeof root)) return;

    int pitch = pitch_class(root);
    for (size_t i = 0; pitch >= 0 && i < sizeof scales / sizeof *scales; i++)
        if (!_stricmp(scales[i].name, name)) {
            publish_scale(scale_root_mask(scale_mask(scales[i].pitches), pitch), pitch, scales[i].name);
            return;
        }
}

/* The stems instance reads the song it is resuming, so every instrument can take its
   sound back and the numbers in the old sounds files keep naming the same sounds. */
static void load_song(void)
{
    wchar_t path[1100];
    if (song_path(path, sizeof path / sizeof *path) != 0) return;

    open_sounds();
    if (!soundList) return;

    FILE *file = _wfopen(path, L"rb");
    if (!file) {
        songReady = 1;
        return;
    }

    fseek(file, 0, SEEK_END);
    long size = ftell(file);
    fseek(file, 0, SEEK_SET);
    char *text = size > 0 ? malloc((size_t)size + 1) : NULL;
    if (text) text[fread(text, 1, (size_t)size, file)] = 0;
    fclose(file);
    if (!text) return;

    char *names = NULL;
    int *seeds = NULL, count = 0;
    const char *sounds = json_value(text, "sounds");
    if (sounds && *sounds == '{') count = load_song_sounds(sounds, &names, &seeds);

    /* The song's file is the numbering, so a session holding other sounds gives way to it. */
    int adopt = !soundList->count;
    if (!adopt && count) {
        adopt = (int)soundList->count < count;
        for (int i = 0; !adopt && i < count; i++)
            if (strcmp(soundList->name[i], names + (size_t)i * SOUND_NAME)) adopt = 1;
        if (adopt) {
            soundList->count = 0;
            soundList->instrumentCount = 0;
        }
    }
    if (adopt)
        for (int i = 0; i < count; i++) sound_number(names + (size_t)i * SOUND_NAME, 1, seeds[i]);

    const char *instruments = json_value(text, "instruments");
    if (instruments && *instruments == '[') load_song_instruments(instruments);

    const char *scale = json_value(text, "random_scale");
    if (scale && *scale == '{') load_song_scale(scale);
    songReady = 1;

    free(names);
    free(seeds);
    free(text);
}

static void save_song(void)
{
    wchar_t path[1100];
    open_sounds();
    if (!songLive || !songReady || !soundList || song_path(path, sizeof path / sizeof *path) != 0) return;

    char *existing = read_song_text(path, sizeof path / sizeof *path);
    const char *leads = existing ? json_value(existing, "stems") : NULL;
    size_t leadsLength = leads ? json_span(leads) : 0;

    FILE *file = _wfopen(path, L"wb");
    if (!file) {
        log_error("Could not write '%ls'.\n", path);
        return;
    }

    while (InterlockedCompareExchange(&soundList->lock, 1, 0)) Sleep(0);

    int count = (int)soundList->count;
    fputs("{\n  \"sounds\": {", file);
    for (int i = 0; i < count; i++) {
        fprintf(file, "%s\n    \"%d\": ", i ? "," : "", i + 1);
        char entry[SOUND_NAME + 24];
        snprintf(entry, sizeof entry, "%s, seed %d", soundList->name[i], (int)soundList->seed[i]);
        json_write_text(file, soundList->seed[i] ? entry : soundList->name[i]);
    }

    int instruments = (int)soundList->instrumentCount;
    fprintf(file, "%s},\n  \"instruments\": [", count ? "\n  " : "");
    for (int i = 0; i < instruments; i++) {
        fprintf(file, "%s\n    { \"instrument\": ", i ? "," : "");
        json_write_text(file, soundList->instrument[i]);
        fprintf(file, ", \"sound\": %d }", soundList->last[i]);
    }
    fprintf(file, "%s]", instruments ? "\n  " : "");
    if (leadsLength) {
        fputs(",\n  \"stems\": ", file);
        fwrite(skip_space(leads), 1, leadsLength, file);
    }
    if (cue && cue->scale_mask)
        fprintf(file, ",\n  \"random_scale\": { \"scale\": \"%s\", \"root\": \"%s\" }",
                cue->scale_name, pitch_name((int)cue->scale_root));
    fputs("\n}\n", file);

    int failed = ferror(file);
    fclose(file);
    free(existing);
    InterlockedExchange(&soundList->lock, 0);
    if (failed) log_error("Could not write '%ls'.\n", path);
    else log_trace("wrote %ls\n", path);
}

typedef struct {
    const float *samples;
    drwav_uint64 frames, start;
    unsigned channels;
} MixStem;

/* A stem placed at its lead-in on the output's channel, silent outside it. */
static float mix_sample(const MixStem *stem, drwav_uint64 frame, int output)
{
    if (frame < stem->start || frame - stem->start >= stem->frames) return 0.0f;
    return stem->samples[(size_t)(frame - stem->start) * stem->channels + (size_t)output % stem->channels];
}

/* The finished mix also lands in the works library, when that folder is there. */
static void copy_mix(const wchar_t *path)
{
    static const wchar_t works[] = L"D:\\[Library]\\[Audio]\\[Works]";
    DWORD attributes = GetFileAttributesW(works);
    if (attributes == INVALID_FILE_ATTRIBUTES || !(attributes & FILE_ATTRIBUTE_DIRECTORY)) return;

    const wchar_t *name = wcsrchr(path, L'\\');
    name = name ? name + 1 : path;
    wchar_t target[1200];
    int written = swprintf(target, 1200, L"%ls\\%ls", works, name);
    if (written < 0 || written >= 1200) return;

    if (CopyFileW(path, target, FALSE)) log_trace("copied %ls\n", target);
    else log_error("Could not copy '%ls' (error %lu).\n", target, GetLastError());
}

/* Every stem file in the folder, placed by its lead-in and peak-normalized to 1,
   written beside song.json when the stems instance closes. Stems already loaded
   are reused, so only a take that never reached a loop point is decoded again. */
static void write_mix(void)
{
    wchar_t path[1100], file[1100];
    MixStem mixes[QUEUE_FILES];
    float *decoded[QUEUE_FILES] = {0};
    unsigned rate = 0;
    int count = 0;

    if (mix_path(path, sizeof path / sizeof *path) != 0) return;
    load_stem_leads();

    size_t length = wcslen(playlistFolder);
    wchar_t *pattern = malloc((length + 3) * sizeof *pattern);
    if (!pattern) return;
    swprintf(pattern, length + 3, L"%ls\\*", playlistFolder);

    WIN32_FIND_DATAW entry;
    HANDLE find = FindFirstFileW(pattern, &entry);
    free(pattern);
    if (find == INVALID_HANDLE_VALUE) return;

    do {
        if (count == QUEUE_FILES) break;
        if ((entry.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || !is_audio_name(entry.cFileName)) continue;
        int written = swprintf(file, sizeof file / sizeof *file, L"%ls\\%ls", playlistFolder, entry.cFileName);
        if (written < 0 || written >= (int)(sizeof file / sizeof *file)) continue;

        const Track *track = NULL;
        for (int i = 0; i < stemCount && !track; i++)
            if (stemFiles[i] && stems[i].frames && !_wcsicmp(stemFiles[i], file)) track = &stems[i];

        Track fresh;
        if (!track) {
            if (decode_track(&fresh, file) != 0) continue;
            decoded[count] = fresh.samples;
            track = &fresh;
        }

        double lead = stem_lead(file);
        MixStem *mix = &mixes[count];
        mix->samples = track->samples;
        mix->frames = track->frames;
        mix->channels = track->channels;
        mix->start = lead > 0.0 ? (drwav_uint64)(lead * (double)track->rate + 0.5) : 0;
        if (!rate) rate = track->rate;
        count++;
    } while (FindNextFileW(find, &entry));
    FindClose(find);

    drwav_uint64 frames = 0;
    for (int i = 0; i < count; i++)
        if (mixes[i].start + mixes[i].frames > frames) frames = mixes[i].start + mixes[i].frames;

    if (count && rate && frames) {
        float peak = 0.0f;
        for (drwav_uint64 at = 0; at < frames; at++)
            for (int output = 0; output < 2; output++) {
                float value = 0.0f;
                for (int i = 0; i < count; i++) value += mix_sample(&mixes[i], at, output);
                float magnitude = fabsf(value);
                if (magnitude > peak) peak = magnitude;
            }

        drwav_data_format format;
        memset(&format, 0, sizeof format);
        format.container = drwav_container_riff;
        format.format = DR_WAVE_FORMAT_PCM;
        format.channels = 2;
        format.sampleRate = rate;
        format.bitsPerSample = 16;

        float *sums = malloc(MIX_BLOCK * 2 * sizeof *sums);
        drwav_int16 *block = malloc(MIX_BLOCK * 2 * sizeof *block);
        drwav wav;
        if (!sums || !block || !drwav_init_file_write_w(&wav, path, &format, NULL)) {
            log_error("Could not write '%ls'.\n", path);
            free(sums);
            free(block);
        } else {
            float scale = peak > 0.0f ? 1.0f / peak : 1.0f;
            for (drwav_uint64 at = 0; at < frames; ) {
                drwav_uint64 want = frames - at < MIX_BLOCK ? frames - at : MIX_BLOCK;
                for (drwav_uint64 i = 0; i < want; i++)
                    for (int output = 0; output < 2; output++) {
                        float value = 0.0f;
                        for (int s = 0; s < count; s++) value += mix_sample(&mixes[s], at + i, output);
                        sums[i * 2 + output] = value * scale;
                    }
                drwav_f32_to_s16(block, sums, (size_t)(want * 2));
                drwav_write_pcm_frames(&wav, want, block);
                at += want;
            }
            drwav_uninit(&wav);
            free(sums);
            free(block);
            log_trace("wrote %ls\n", path);
            copy_mix(path);
        }
    }

    for (int i = 0; i < count; i++) free(decoded[i]);
}

/* An instrument that is already running when a stems instance starts, or that starts
   while one plays, takes the song's sounds on as soon as its folder is known. */
static void sync_song(void)
{
    static wchar_t tried[1100];
    wchar_t path[1100];

    if (songReady || song_path(path, sizeof path / sizeof *path) != 0 || !wcscmp(tried, path)) return;
    wcscpy(tried, path);
    load_song();
}

static void sound_name(char *out, size_t size)
{
    out[0] = 0;
    if (g.samplesSound) snprintf(out, size, "%d drum samples", drumCount);
    else if (vstEffect) {
        char name[64];
        effect_name(vstEffect, name, sizeof name);
        char octave[24] = "";
        if (vstOctave) snprintf(octave, sizeof octave, ", octave %+d", vstOctave);
        if (vstPresetCount > 0)
            snprintf(out, size, "VST %s, preset %d/%d%s", name[0] ? name : "plugin", vstProgram + 1,
                     vstPresetCount, octave);
        else
            snprintf(out, size, "VST %s, default preset%s", name[0] ? name : "plugin", octave);
    }
}

static int find_vst_library(const char *name)
{
    for (int i = 0; i < vstLibraryCount; i++) {
        size_t length = strlen(vstNames[i]);
        if (length > 4 && !_stricmp(vstNames[i] + length - 4, ".dll")) length -= 4;
        if (!_strnicmp(vstNames[i], name, length) && !name[length]) return i;
    }

    for (int i = 0; i < vstLibraryCount; i++) {
        AEffect *effect = create_effect(vstLibraries[i]);
        if (!effect || !effect->dispatcher) continue;

        char effectName[VST_NAME];
        effect_name(effect, effectName, sizeof effectName);
        effect->dispatcher(effect, effClose, 0, 0, NULL, 0);
        if (effectName[0] && !_stricmp(effectName, name)) return i;
    }
    return -1;
}

static int restore_sound(int number)
{
    open_sounds();
    if (!soundList) return -1;

    while (InterlockedCompareExchange(&soundList->lock, 1, 0)) Sleep(0);

    int count = (int)soundList->count, seed = 0;
    char name[SOUND_NAME];
    int found = number >= 1 && number <= count;
    if (found) {
        memcpy(name, soundList->name[number - 1], SOUND_NAME);
        seed = soundList->seed[number - 1];
    }

    InterlockedExchange(&soundList->lock, 0);
    if (!found) return -1;
    name[SOUND_NAME - 1] = 0;

    if (strstr(name, " drum samples")) {
        if (!drumCount) return -1;
        if (vstEffect) suspend_vst_effect();
        seed_drums(seed);
        g.samplesSound = 1;
        assign_drums();
        g.soundNumber = number;
        return 0;
    }

    if (strncmp(name, "VST ", 4)) return -1;
    const char *comma = strstr(name + 4, ", ");
    if (!comma || (size_t)(comma - name - 4) >= VST_NAME) return -1;

    char plugin[VST_NAME];
    size_t length = (size_t)(comma - name - 4);
    memcpy(plugin, name + 4, length);
    plugin[length] = 0;

    int preset = 0, octave = 0;
    const char *field = strstr(name, ", preset ");
    if (field) sscanf(field, ", preset %d", &preset);
    field = strstr(name, ", octave ");
    if (field) sscanf(field, ", octave %d", &octave);
    if (octave < -VST_MAX_OCTAVE) octave = -VST_MAX_OCTAVE;
    else if (octave > VST_MAX_OCTAVE) octave = VST_MAX_OCTAVE;

    int library = find_vst_library(plugin);
    if (library < 0) {
        log_error("The sound '%s' needs a plugin that is not loaded.\n", name);
        return -1;
    }
    if (preset < 1 || preset > vstLibraryPresets[library]) preset = 0;

    int restart = g.started;
    if (restart) {
        g.asio->vtable->stop(g.asio);
        g.started = 0;
    }
    suspend_vst_effect();
    if (start_vst(library, preset - 1, octave) != 0)
        log_error("Could not start the plugin for the sound '%s'.\n", name);
    else {
        vstOctave = octave;
        g.samplesSound = 0;
        g.soundNumber = number;
    }

    if (restart) {
        if (g.asio->vtable->start(g.asio) == ASE_OK) g.started = 1;
        else {
            log_error("ASIOStart failed after restoring a sound.\n");
            InterlockedExchange(&g.quit, 1);
        }
    }
    return g.soundNumber == number ? 0 : -1;
}

/* Overwrites the meters line in place, so a message leaves no blank line behind it. */
static void console_line(const char *text)
{
    printf("\r%-*s\n", g.lineWidth, text);
    fflush(stdout);
    g.lineWidth = 0;
}

static void apply_scale(unsigned mask, int root, const char *name)
{
    char notes[64], line[160];

    scaleMask = mask;
    scale_notes(notes, sizeof notes, root);
    snprintf(line, sizeof line, "  random scale: %s %s (%s)", pitch_name(root), name, notes);
    console_line(line);
    log_trace("random scale: %s %s (%s)\n", pitch_name(root), name, notes);
}

/* A follower takes the session's scale on, and takes it over again whenever it moves. */
static void follow_scale(void)
{
    open_cue();
    if (!scaleFollow || !cue || !cue->scale_mask || cue->scale_seq == scaleSeen) return;

    scaleSeen = cue->scale_seq;
    apply_scale((unsigned)cue->scale_mask, (int)cue->scale_root, cue->scale_name);
}

static void draw_scale(void)
{
    int root = rand() % 12;
    const Scale *scale = &scales[rand() % (int)(sizeof scales / sizeof *scales)];
    unsigned mask = scale_root_mask(scale_mask(scale->pitches), root);

    publish_scale(mask, root, scale->name);
    scaleSeen = cue ? cue->scale_seq : 0;
    apply_scale(mask, root, scale->name);
}

static void print_sound(const char *prefix)
{
    char name[160], line[192];
    sound_name(name, sizeof name);
    if (!name[0]) return;

    if (!g.soundNumber)
        g.soundNumber = sound_number(name, g.samplesSound, g.samplesSound ? drumSeed : 0);
    remember_sound(g.soundNumber);
    save_song();

    snprintf(line, sizeof line, "  %s%d: %s", prefix, g.soundNumber, name);
    console_line(line);
    log_trace("sound %d: %s\n", g.soundNumber, name);
}

static int take_sound(int number)
{
    if (restore_sound(number) != 0) return -1;

    print_sound("sound ");
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    add_sound_mark(g.soundNumber, (ULONGLONG)now.QuadPart);
    return 0;
}

/* A stems instance that loads a song sets every instrument's last sound, so an
   instance that was already running takes its sound on too. */
static void follow_song_sound(void)
{
    static int failed;

    int number = last_sound_number();
    if (!number || number == g.soundNumber || number == failed) return;
    failed = take_sound(number) == 0 ? 0 : number;
}

static void new_sound(void)
{
    g.soundNumber = 0;
    choose_sound(g.samplesRatio);
    if (g.samplesSound) {
        seed_drums(0);
        assign_drums();
    } else new_vst_sound();

    print_sound("new sound ");
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    add_sound_mark(g.soundNumber, (ULONGLONG)now.QuadPart);
}

/* The console's clicks walk the shared sounds, and a click past the highest draws a new one. */
static void step_sound(int step)
{
    if (step > 0 && g.soundNumber >= 1 && g.soundNumber < (int)soundList->count) take_sound(g.soundNumber + 1);
    else if (step < 0 && g.soundNumber > 1) take_sound(g.soundNumber - 1);
    else if (step > 0) new_sound();
}

static void close_vst(void)
{
    close_vst_effect();
    for (int i = 0; i < vstLibraryCount; i++) {
        FreeLibrary(vstLibraries[i]);
        free(vstTastes[i]);
        vstTastes[i] = NULL;
    }
    vstLibraryCount = 0;
}

/* The Windows master volume, through Core Audio's endpoint volume. The SDK headers are
   C++ only, so only the vtable slots up to GetMasterVolumeLevelScalar are declared. */

static const GUID enumeratorClass = {0xBCDE0395, 0xE52F, 0x467C, {0x8E, 0x3D, 0xC4, 0x57, 0x92, 0x91, 0x69, 0x2E}};
static const GUID enumeratorInterface = {0xA95664D2, 0x9614, 0x4F35, {0xA7, 0x46, 0xDE, 0x8D, 0xB6, 0x36, 0x17, 0xE6}};
static const GUID endpointVolumeInterface = {0x5CDF2C82, 0x841E, 0x4546, {0x97, 0x22, 0x0C, 0xF7, 0x40, 0x78, 0x22, 0x9A}};

typedef struct {
    void *queryInterface, *addRef;
    ULONG (STDMETHODCALLTYPE *Release)(void *);
    void *enumAudioEndpoints;
    HRESULT (STDMETHODCALLTYPE *GetDefaultAudioEndpoint)(void *, int, int, void **);
} EnumeratorVtbl;

typedef struct {
    void *queryInterface, *addRef;
    ULONG (STDMETHODCALLTYPE *Release)(void *);
    HRESULT (STDMETHODCALLTYPE *Activate)(void *, const GUID *, DWORD, void *, void **);
} DeviceVtbl;

typedef struct {
    void *queryInterface, *addRef;
    ULONG (STDMETHODCALLTYPE *Release)(void *);
    void *registerControlChangeNotify, *unregisterControlChangeNotify, *getChannelCount,
         *setMasterVolumeLevel, *setMasterVolumeLevelScalar, *getMasterVolumeLevel;
    HRESULT (STDMETHODCALLTYPE *GetMasterVolumeLevelScalar)(void *, float *);
} EndpointVolumeVtbl;

static void *deviceEnumerator;

static void open_master_volume(void)
{
    if (FAILED(CoCreateInstance(&enumeratorClass, NULL, CLSCTX_ALL, &enumeratorInterface, &deviceEnumerator)))
        deviceEnumerator = NULL;
}

static float read_master_volume(void)
{
    if (!deviceEnumerator) return 1.0f;

    void *device = NULL, *endpoint = NULL;
    float volume = 1.0f;
    EnumeratorVtbl *enumerator = *(EnumeratorVtbl **)deviceEnumerator;

    if (SUCCEEDED(enumerator->GetDefaultAudioEndpoint(deviceEnumerator, 0 /* eRender */, 1 /* eMultimedia */, &device)) && device) {
        DeviceVtbl *deviceVtable = *(DeviceVtbl **)device;
        if (SUCCEEDED(deviceVtable->Activate(device, &endpointVolumeInterface, CLSCTX_ALL, NULL, &endpoint)) && endpoint) {
            EndpointVolumeVtbl *volumeVtable = *(EndpointVolumeVtbl **)endpoint;
            if (FAILED(volumeVtable->GetMasterVolumeLevelScalar(endpoint, &volume))) volume = 1.0f;
            volumeVtable->Release(endpoint);
        }
        deviceVtable->Release(device);
    }
    return volume;
}

static void process_buffer(int32_t index)
{
    const int frames = (int)g.bufferSize;
    const float volume = g.volume;

    check_loop_cue();
    take_buffer(frames);

    for (int c = 0; c < g.nIn; c++) {
        float *samples = g.inputFloats + (size_t)c * frames;
        sample_to_float(g.bufferInfo[c].buffers[index], g.inFormat[c], samples, frames);

        float peak = 0.0f;
        for (int i = 0; i < frames; i++) {
            float value = fabsf(samples[i]);
            if (value > peak) peak = value;
        }
        float held = g.peak[c] * 0.97f;
        g.peak[c] = peak > held ? peak : held;
    }

    memset(g.synthFloats, 0, (size_t)frames * sizeof(float));
    if (g.synthEnabled || instrumentMode) drain_midi();
    if (g.synthEnabled) synth_render(g.synthFloats, frames);

    if (playlistMode) render_playlist(g.fileFloats, g.nOut, frames);
    else if (instrumentMode) {
        render_instruments(g.fileFloats, g.nOut, frames);
        taste_tick(frames);
    }

    for (int j = 0; j < g.nOut; j++) {
        int source = g.nIn ? j % g.nIn : -1;
        const float *input = source >= 0 ? g.inputFloats + (size_t)source * frames : NULL;
        const float *music = g.fileFloats ? g.fileFloats + (size_t)j * frames : NULL;
        void *output = g.bufferInfo[g.nIn + j].buffers[index];

        for (int i = 0; i < frames; i++) {
            float value = (input ? input[i] : 0.0f) + g.synthFloats[i];
            if (music) value += music[i];
            value *= volume;
            g.mixFloats[i] = value > 1.0f ? 1.0f : value < -1.0f ? -1.0f : value;
        }
        sample_from_float(g.mixFloats, output, g.outFormat[j], frames);
        if (take.active) take_mix(g.mixFloats, frames);
        if (j == 0) {
            rating_audio(g.mixFloats, frames);
            if (recording.active) recording_audio(g.mixFloats, frames);
        }
    }

    if (take.active) {
        take.last->frames += (size_t)frames;
        take.frames += (drwav_uint64)frames;
    }
}

static void on_buffer_switch(int32_t index, ASIOBool directProcess)
{
    (void)directProcess;
    process_buffer(index);
    if (g.postOutput) g.asio->vtable->outputReady(g.asio);
}

static ASIOTime *on_buffer_switch_time_info(ASIOTime *params, int32_t index, ASIOBool directProcess)
{
    (void)directProcess;
    process_buffer(index);
    if (g.postOutput) g.asio->vtable->outputReady(g.asio);
    return params;
}

static void on_sample_rate_changed(ASIOSampleRate rate)
{
    if (rate > 0) InterlockedExchange(&g.reset, 1);
}

static int32_t on_asio_message(int32_t selector, int32_t value, void *message, double *opt)
{
    (void)message;
    (void)opt;

    switch (selector) {
    case kAsioSelectorSupported:
        return value == kAsioEngineVersion || value == kAsioResetRequest ||
               value == kAsioBufferSizeChange || value == kAsioResyncRequest ||
               value == kAsioLatenciesChanged || value == kAsioSupportsTimeInfo ||
               value == kAsioSupportsTimeCode;
    case kAsioEngineVersion:
        return 2;
    case kAsioResetRequest:
    case kAsioBufferSizeChange:
        InterlockedExchange(&g.reset, 1);
        return 1;
    case kAsioResyncRequest:
    case kAsioLatenciesChanged:
        return 1;
    case kAsioSupportsTimeInfo:
        return 1;
    case kAsioSupportsTimeCode:
        return 0;
    default:
        return 0;
    }
}

static void CALLBACK midi_in_proc(HMIDIIN handle, UINT message, DWORD_PTR instance,
                                  DWORD_PTR param1, DWORD_PTR param2)
{
    (void)handle;
    (void)instance;
    (void)param2;

    if (message != MIM_DATA) return;

    uint32_t data = (uint32_t)param1;
    int channel = (int)(data & 0x0f) + 1;
    if (g.midiChannel && channel != g.midiChannel) return;

    follow_scale();
    int status = (int)(data & 0xf0);
    if (scaleMask && (status == 0x80 || status == 0x90 || status == 0xa0))
        data = (data & 0xffff00ffu) | (uint32_t)scale_note((int)(data >> 8) & 0x7f) << 8;

    if (g.midiStream)
        log_line("MIDI ch %d  %02X %02X %02X", channel, (int)(data & 0xff),
                 (int)(data >> 8) & 0x7f, (int)(data >> 16) & 0x7f);
    if (g.midiOut) midiOutShortMsg(g.midiOut, data);
    midi_push(data);
}

static HANDLE quitEvent;

static BOOL WINAPI ctrl_handler(DWORD type)
{
    InterlockedExchange(&g.quit, 1);
    if (quitEvent && (type == CTRL_CLOSE_EVENT || type == CTRL_LOGOFF_EVENT || type == CTRL_SHUTDOWN_EVENT))
        WaitForSingleObject(quitEvent, 4000);
    return TRUE;
}

static LRESULT CALLBACK wnd_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam)
{
    return DefWindowProcA(window, message, wparam, lparam);
}

static HWND create_window(HINSTANCE instance)
{
    WNDCLASSA windowClass;
    memset(&windowClass, 0, sizeof windowClass);
    windowClass.lpfnWndProc = wnd_proc;
    windowClass.hInstance = instance;
    windowClass.lpszClassName = "RestlessVirtuosoWindow";
    RegisterClassA(&windowClass);
    return CreateWindowExA(0, windowClass.lpszClassName, "restless_virtuoso", 0, 0, 0, 0, 0,
                           NULL, NULL, instance, NULL);
}

static void pump_messages(void)
{
    MSG message;
    while (PeekMessageA(&message, NULL, 0, 0, PM_REMOVE)) {
        TranslateMessage(&message);
        DispatchMessageA(&message);
    }
}

static int list_drivers(char names[][128], int max)
{
    HKEY root;
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, "SOFTWARE\\ASIO", 0, KEY_READ | KEY_WOW64_64KEY, &root) != ERROR_SUCCESS)
        return 0;

    int count = 0;
    for (DWORD index = 0; count < max; index++) {
        char name[128];
        DWORD length = sizeof name;
        if (RegEnumKeyExA(root, index, name, &length, NULL, NULL, NULL, NULL) != ERROR_SUCCESS) break;

        length = (DWORD)strlen(name);
        if (length > sizeof names[count] - 1) length = sizeof names[count] - 1;
        memcpy(names[count], name, length);
        names[count][length] = 0;
        count++;
    }

    RegCloseKey(root);
    return count;
}

static int driver_clsid(const char *name, CLSID *clsid)
{
    char path[512];
    snprintf(path, sizeof path, "SOFTWARE\\ASIO\\%s", name);

    HKEY key;
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, path, 0, KEY_READ | KEY_WOW64_64KEY, &key) != ERROR_SUCCESS)
        return -1;

    char text[128] = {0};
    DWORD size = sizeof text - 1;
    DWORD type = 0;
    LONG result = RegQueryValueExA(key, "CLSID", NULL, &type, (LPBYTE)text, &size);
    RegCloseKey(key);
    if (result != ERROR_SUCCESS || type != REG_SZ) return -1;

    wchar_t wide[128];
    if (!MultiByteToWideChar(CP_ACP, 0, text, -1, wide, 128)) return -1;
    return SUCCEEDED(CLSIDFromString(wide, clsid)) ? 0 : -1;
}

static const char *resolve_driver(const char *argument, char names[][128], int count)
{
    if (count == 0) return NULL;
    if (!argument) return names[0];

    char *end;
    long index = strtol(argument, &end, 10);
    if (*argument && !*end) return index >= 0 && index < count ? names[index] : NULL;

    for (int i = 0; i < count; i++)
        if (!_stricmp(names[i], argument)) return names[i];
    return NULL;
}

static int open_driver(const char *name, HWND window)
{
    CLSID clsid;
    if (driver_clsid(name, &clsid) != 0) {
        log_error("ASIO driver '%s' is not registered.\n", name);
        return -1;
    }

    log_trace("creating driver object\n");
    HRESULT result = CoCreateInstance(&clsid, NULL, CLSCTX_INPROC_SERVER, &clsid, (void **)&g.asio);
    if (FAILED(result) || !g.asio) {
        log_error("Could not create the ASIO driver object (0x%08lx).\n", (unsigned long)result);
        g.asio = NULL;
        return -1;
    }
    log_trace("driver object 0x%08lx\n", (unsigned long)result);

    log_trace("initializing driver\n");
    HANDLE watchdog = errorLog ? CreateThread(NULL, 0, init_watchdog, NULL, 0, NULL) : NULL;
    ASIOBool initialized = g.asio->vtable->init(g.asio, window);
    InterlockedExchange(&initReturned, 1);
    if (watchdog) CloseHandle(watchdog);
    log_trace("init returned %d\n", initialized);
    if (!initialized) {
        char error[124] = {0};
        g.asio->vtable->getErrorMessage(g.asio, error);
        error[123] = 0;
        log_error("ASIOInit failed: %s\n", error[0] ? error : "no error message from the driver");
        g.asio->vtable->Release(g.asio);
        g.asio = NULL;
        return -1;
    }

    g.asio->vtable->getDriverName(g.asio, g.driverName);
    g.driverName[63] = 0;
    log_trace("driver name '%s'\n", g.driverName);

    log_trace("querying channels\n");
    ASIOError channels = g.asio->vtable->getChannels(g.asio, &g.inCount, &g.outCount);
    log_trace("channels %s, in %d out %d\n", asio_error_text(channels), g.inCount, g.outCount);
    if (channels != ASE_OK) {
        log_error("ASIOGetChannels failed.\n");
        return -1;
    }

    g.postOutput = g.asio->vtable->outputReady(g.asio) == ASE_OK;
    return 0;
}

static int select_channels(const char *spec, int available, int *channels, int *count,
                           int defaultCount, const char *what)
{
    if (spec && !_stricmp(spec, "none")) {
        *count = 0;
        return 0;
    }

    if (!spec) {
        *count = available < defaultCount ? available : defaultCount;
        for (int i = 0; i < *count; i++) channels[i] = i;
        return 0;
    }

    int parsed = parse_channels(spec, available, channels, MAX_SELECTED);
    if (parsed < 0) {
        log_error("Invalid %s channel list '%s' (the driver has %d).\n", what, spec, available);
        return -1;
    }
    *count = parsed;
    return 0;
}

static int setup_rate(int requested)
{
    if (requested > 0) {
        if (g.asio->vtable->canSampleRate(g.asio, (ASIOSampleRate)requested) != ASE_OK) {
            log_error("The driver does not support %d Hz.\n", requested);
            return -1;
        }
        ASIOError error = g.asio->vtable->setSampleRate(g.asio, (ASIOSampleRate)requested);
        if (error != ASE_OK) {
            log_error("ASIOSetSampleRate failed (%s).\n", asio_error_text(error));
            return -1;
        }
    }

    ASIOSampleRate rate = 0;
    g.asio->vtable->getSampleRate(g.asio, &rate);
    if (rate <= 0) rate = requested > 0 ? requested : 48000;
    g.rate = rate;
    synth_set_rate((int)rate);
    return 0;
}

static int setup_buffer(int requested)
{
    int32_t minimum = 0, maximum = 0, preferred = 0, granularity = 0;
    ASIOError error = g.asio->vtable->getBufferSize(g.asio, &minimum, &maximum, &preferred, &granularity);
    if (error != ASE_OK) {
        log_error("ASIOGetBufferSize failed (%s).\n", asio_error_text(error));
        return -1;
    }

    int32_t size = preferred;
    if (requested > 0) {
        if (granularity <= 0) {
            size = requested == preferred || requested == minimum || requested == maximum ? requested : preferred;
        } else {
            size = minimum + ((requested - minimum + granularity / 2) / granularity) * granularity;
            if (size < minimum) size = minimum;
            if (size > maximum) size = maximum;
        }
        if (size != requested)
            log_line("note: buffer size %d is not available, using %d frames", requested, size);
    }

    g.bufferSize = size;
    return 0;
}

static SampleFormat channel_format(int channel, ASIOBool isInput)
{
    ASIOChannelInfo info;
    memset(&info, 0, sizeof info);
    info.channel = channel;
    info.isInput = isInput;

    if (g.asio->vtable->getChannelInfo(g.asio, &info) != ASE_OK) {
        log_error("ASIOGetChannelInfo failed for %s %d.\n", isInput ? "input" : "output", channel + 1);
        return SAMPLE_UNSUPPORTED;
    }

    SampleFormat format = sample_format_from_asio(info.type);
    if (format == SAMPLE_UNSUPPORTED)
        log_error("%s %d uses an unsupported sample format (%s).\n",
                isInput ? "Input" : "Output", channel + 1, sample_format_name(info.type));
    return format;
}

static void free_buffers(void)
{
    free(g.inputFloats);
    free(g.fileFloats);
    free(g.synthFloats);
    free(g.mixFloats);
    g.inputFloats = g.fileFloats = g.synthFloats = g.mixFloats = NULL;
}

static int create_buffers(void)
{
    for (int c = 0; c < g.nIn; c++) {
        g.inFormat[c] = channel_format(g.inCh[c], ASIOTrue);
        if (g.inFormat[c] == SAMPLE_UNSUPPORTED) return -1;
    }
    for (int j = 0; j < g.nOut; j++) {
        g.outFormat[j] = channel_format(g.outCh[j], ASIOFalse);
        if (g.outFormat[j] == SAMPLE_UNSUPPORTED) return -1;
    }

    size_t frames = (size_t)g.bufferSize;
    free_buffers();
    g.inputFloats = g.nIn ? malloc(frames * g.nIn * sizeof(float)) : NULL;
    g.fileFloats = (playlistMode || instrumentMode) ? malloc(frames * g.nOut * sizeof(float)) : NULL;
    g.synthFloats = malloc(frames * sizeof(float));
    g.mixFloats = malloc(frames * sizeof(float));
    if ((g.nIn && !g.inputFloats) || ((playlistMode || instrumentMode) && !g.fileFloats) ||
        !g.synthFloats || !g.mixFloats) {
        log_error("Out of memory for %d-frame buffers.\n", g.bufferSize);
        free_buffers();
        return -1;
    }

    memset(g.bufferInfo, 0, sizeof g.bufferInfo);
    for (int c = 0; c < g.nIn; c++) {
        g.bufferInfo[c].isInput = ASIOTrue;
        g.bufferInfo[c].channelNum = g.inCh[c];
    }
    for (int j = 0; j < g.nOut; j++) {
        g.bufferInfo[g.nIn + j].isInput = ASIOFalse;
        g.bufferInfo[g.nIn + j].channelNum = g.outCh[j];
    }

    ASIOError error = g.asio->vtable->createBuffers(g.asio, g.bufferInfo, g.nIn + g.nOut,
                                            g.bufferSize, &callbacks);
    if (error != ASE_OK) {
        log_error("ASIOCreateBuffers failed (%s).\n", asio_error_text(error));
        free_buffers();
        return -1;
    }
    g.buffersCreated = 1;

    int32_t latencyIn = 0, latencyOut = 0;
    g.asio->vtable->getLatencies(g.asio, &latencyIn, &latencyOut);
    g.latencyIn = latencyIn;
    g.latencyOut = latencyOut;
    return 0;
}

static const char *midi_error_text(MMRESULT result)
{
    static char text[256];
    if (midiInGetErrorTextA(result, text, sizeof text) == MMSYSERR_NOERROR) return text;

    switch (result) {
    case MMSYSERR_ALLOCATED: return "the device is already in use";
    case MMSYSERR_BADDEVICEID: return "invalid device index";
    case MMSYSERR_NODRIVER: return "the device driver is not installed";
    case MMSYSERR_NOTENABLED: return "the device driver is disabled";
    case MMSYSERR_INVALPARAM: return "invalid parameter";
    default: return "unknown MIDI error";
    }
}

static const char *midi_error_hint(MMRESULT result)
{
    switch (result) {
    case MMSYSERR_ALLOCATED: return "another program is using the device; close it and retry";
    case MMSYSERR_NODRIVER: return "the driver failed to load; reconnect the device or restart Windows";
    case MMSYSERR_NOTENABLED: return "the driver is disabled in Device Manager";
    case MMSYSERR_BADDEVICEID: return "the device index is out of date; the device list changed";
    case MMSYSERR_INVALPARAM: return "the device vanished after it was listed; reconnect it";
    case MMSYSERR_NOMEM: return "the port could not be allocated; another program may hold it, or the device is stuck";
    case MMSYSERR_NOTSUPPORTED: return "the driver rejects this open mode";
    default: return "the code above is the only detail Windows reports";
    }
}

static const char *midi_in_label(int index, char *label, size_t size)
{
    MIDIINCAPS caps;
    label[0] = 0;
    if (midiInGetDevCaps((UINT)index, &caps, sizeof caps) == MMSYSERR_NOERROR)
        snprintf(label, size, " (%s)", caps.szPname);
    return label;
}

static void diagnose_midi_input(int index, MMRESULT failure)
{
    UINT count = midiInGetNumDevs();
    log_error("  %u MIDI inputs are listed now\n", count);

    if ((UINT)index >= count)
        log_error("  input %d is no longer listed; it was removed after the device list was read\n", index);
    else {
        MIDIINCAPS caps;
        if (midiInGetDevCaps((UINT)index, &caps, sizeof caps) != MMSYSERR_NOERROR)
            log_error("  input %d no longer reports any details\n", index);
        else
            log_error("  input %d is '%s', driver %u.%u, manufacturer 0x%04x, product 0x%04x\n",
                      index, caps.szPname, (unsigned)(caps.vDriverVersion >> 8),
                      (unsigned)(caps.vDriverVersion & 0xff), (unsigned)caps.wMid, (unsigned)caps.wPid);
    }

    if (failure == MMSYSERR_NODRIVER || failure == MMSYSERR_NOTENABLED) return;
    if ((UINT)index >= count) return;

    HMIDIIN probe = NULL;
    MMRESULT result = midiInOpen(&probe, (UINT)index, 0, 0, CALLBACK_NULL);
    if (result == MMSYSERR_NOERROR) {
        midiInClose(probe);
        log_error("  it opens without a callback, so the driver is rejecting callback mode\n");
    } else
        log_error("  it also fails to open without a callback: %s (error %u)\n",
                  midi_error_text(result), (unsigned)result);
}

static int resolve_midi_in(const char *argument)
{
    UINT count = midiInGetNumDevs();
    char *end;
    long index = strtol(argument, &end, 10);
    if (*argument && !*end) return index >= 0 && index < (long)count ? (int)index : -1;

    for (UINT i = 0; i < count; i++) {
        MIDIINCAPS caps;
        if (midiInGetDevCaps(i, &caps, sizeof caps) == MMSYSERR_NOERROR && !_stricmp(caps.szPname, argument))
            return (int)i;
    }
    return -1;
}

static int resolve_midi_out(const char *argument)
{
    UINT count = midiOutGetNumDevs();
    char *end;
    long index = strtol(argument, &end, 10);
    if (*argument && !*end) return index >= 0 && index < (long)count ? (int)index : -1;

    for (UINT i = 0; i < count; i++) {
        MIDIOUTCAPS caps;
        if (midiOutGetDevCaps(i, &caps, sizeof caps) == MMSYSERR_NOERROR && !_stricmp(caps.szPname, argument))
            return (int)i;
    }
    return -1;
}

static int open_midi_input(const char *spec)
{
    int index = resolve_midi_in(spec);
    if (index < 0) {
        log_error("MIDI input '%s' not found (use --list).\n", spec);
        return -1;
    }

    char label[128];
    midi_in_label(index, label, sizeof label);

    MMRESULT result = MMSYSERR_NOERROR;
    for (int attempt = 1; attempt <= 3; attempt++) {
        g.midiIn = NULL;
        result = midiInOpen(&g.midiIn, (UINT)index, (DWORD_PTR)midi_in_proc, 0, CALLBACK_FUNCTION);
        if (result == MMSYSERR_NOERROR) break;
        log_error("MIDI input %d%s failed to open on attempt %d: %s (error %u).\n",
                  index, label, attempt, midi_error_text(result), (unsigned)result);
        Sleep(250);
    }
    if (result != MMSYSERR_NOERROR) {
        log_error("  likely cause: %s\n", midi_error_hint(result));
        diagnose_midi_input(index, result);
        return -1;
    }

    result = midiInStart(g.midiIn);
    if (result != MMSYSERR_NOERROR) {
        log_error("MIDI input %d%s opened but failed to start: %s (error %u).\n",
                  index, label, midi_error_text(result), (unsigned)result);
        log_error("  likely cause: %s\n", midi_error_hint(result));
        midiInClose(g.midiIn);
        g.midiIn = NULL;
        return -1;
    }
    g.midiInIndex = index;
    return 0;
}

static int open_midi_output(const char *spec)
{
    int index = resolve_midi_out(spec);
    if (index < 0) {
        log_error("MIDI output '%s' not found (use --list).\n", spec);
        return -1;
    }

    MIDIOUTCAPS caps;
    char label[128] = "";
    if (midiOutGetDevCaps((UINT)index, &caps, sizeof caps) == MMSYSERR_NOERROR)
        snprintf(label, sizeof label, " (%s)", caps.szPname);

    MMRESULT result = midiOutOpen(&g.midiOut, (UINT)index, 0, 0, CALLBACK_NULL);
    if (result != MMSYSERR_NOERROR) {
        log_error("Could not open MIDI output %d%s: %s (error %u).\n",
                  index, label, midi_error_text(result), (unsigned)result);
        log_error("  likely cause: %s\n", midi_error_hint(result));
        return -1;
    }
    g.midiOutIndex = index;
    return 0;
}

static void list_devices(void)
{
    char names[64][128];
    int drivers = list_drivers(names, 64);

    printf("ASIO drivers:\n");
    for (int i = 0; i < drivers; i++) printf("  %d  %s\n", i, names[i]);
    if (!drivers) printf("  (none)\n");

    printf("MIDI inputs:\n");
    UINT inputs = midiInGetNumDevs();
    for (UINT i = 0; i < inputs; i++) {
        MIDIINCAPS caps;
        MMRESULT result = midiInGetDevCaps(i, &caps, sizeof caps);
        if (result == MMSYSERR_NOERROR) printf("  %u  %s\n", i, caps.szPname);
        else printf("  %u  (no details: %s)\n", i, midi_error_text(result));
    }
    if (!inputs) printf("  (none)\n");
}

static HANDLE consoleInput;
static DWORD consoleMode;
static int consoleModeSet;

static void init_console_input(void)
{
    consoleInput = GetStdHandle(STD_INPUT_HANDLE);
    if (consoleInput == INVALID_HANDLE_VALUE || !GetConsoleMode(consoleInput, &consoleMode)) {
        consoleInput = NULL;
        return;
    }
    consoleModeSet = SetConsoleMode(consoleInput,
        ENABLE_EXTENDED_FLAGS | ENABLE_MOUSE_INPUT | ENABLE_PROCESSED_INPUT);
}

static void read_console_input(void)
{
    if (!consoleInput) return;

    DWORD available = 0;
    while (GetNumberOfConsoleInputEvents(consoleInput, &available) && available) {
        INPUT_RECORD records[8];
        DWORD read = 0, wanted = available < 8 ? available : 8;
        if (!ReadConsoleInputA(consoleInput, records, wanted, &read) || !read) return;

        for (DWORD i = 0; i < read; i++) {
            if (records[i].EventType != MOUSE_EVENT || !instrumentMode) continue;
            MOUSE_EVENT_RECORD *mouse = &records[i].Event.MouseEvent;
            if (mouse->dwEventFlags) continue;
            if (mouse->dwButtonState & FROM_LEFT_1ST_BUTTON_PRESSED) step_sound(1);
            else if (mouse->dwButtonState & RIGHTMOST_BUTTON_PRESSED) step_sound(-1);
        }
    }
}

static void draw_meters(void)
{
    char line[512];
    int length = 0;
    line[0] = 0;
    int shown = g.nIn < MAX_METERS ? g.nIn : MAX_METERS;

    for (int c = 0; c < shown; c++) {
        float peak = g.peak[c];
        float db = peak > 0.000001f ? 20.0f * log10f(peak) : -60.0f;
        int bars = (int)((db + 60.0f) / 6.0f);
        if (bars < 0) bars = 0;
        if (bars > 10) bars = 10;

        char bar[11];
        memset(bar, '-', 10);
        bar[10] = 0;
        memset(bar, '#', (size_t)bars);

        length += snprintf(line + length, sizeof line - (size_t)length, "%2d [%s] %6.1f  ", g.inCh[c] + 1, bar, db);
        if (length > (int)sizeof line - 1) length = (int)sizeof line - 1;
    }

    if (g.nIn > shown && length < (int)sizeof line - 4)
        length += snprintf(line + length, sizeof line - (size_t)length, "...");

    if (length > (int)sizeof line - 1) length = (int)sizeof line - 1;

    if (playlistMode) {
        double position = playlistElapsed, total = playlist_length();
        int filled = total > 0.0 ? (int)(position / total * 24.0) : 0;
        if (filled > 24) filled = 24;

        char bar[25];
        memset(bar, '-', 24);
        memset(bar, '=', (size_t)filled);
        bar[24] = 0;

        int played = (int)(position + 0.5), song = (int)(total + 0.5);
        length += snprintf(line + length, sizeof line - (size_t)length, "%s[%s] %d:%02d/%d:%02d",
                           length ? "  " : "", bar, played / 60, played % 60, song / 60, song % 60);
        if (length > (int)sizeof line - 1) length = (int)sizeof line - 1;
    }

    length += snprintf(line + length, sizeof line - (size_t)length, "%srating %.6f  score %.6f",
                       length ? "  " : "", rating_current(), ratingBurst);
    if (length > (int)sizeof line - 1) length = (int)sizeof line - 1;
    if (length < g.lineWidth) {
        memset(line + length, ' ', (size_t)(g.lineWidth - length));
        length = g.lineWidth;
        line[length] = 0;
    }
    g.lineWidth = length;

    printf("\r%s", line);
    fflush(stdout);
}

static void shutdown_monitor(void)
{
    if (g.midiIn) {
        midiInStop(g.midiIn);
        midiInClose(g.midiIn);
        g.midiIn = NULL;
    }
    if (g.midiOut) {
        midiOutClose(g.midiOut);
        g.midiOut = NULL;
    }
    if (g.asio) {
        if (g.started) g.asio->vtable->stop(g.asio);
        if (g.buffersCreated) g.asio->vtable->disposeBuffers(g.asio);
        g.asio->vtable->Release(g.asio);
        g.asio = NULL;
    }
    if (settingsPath) save_settings(settingsPath, settingsOptions);
    if (playlistMode) {
        save_song();
        write_mix();
    }
    take_finish();
    recording_stop(0);
    rating_stop();
    withdraw_instrument();
    clear_loop_cue();
    if (deviceEnumerator) {
        (*(EnumeratorVtbl **)deviceEnumerator)->Release(deviceEnumerator);
        deviceEnumerator = NULL;
    }
    free_buffers();
    for (int i = 0; i < stemCount; i++) free(stems[i].samples);
    free(stems);
    for (int i = 0; i < drumCount; i++) {
        free(drums[i].track.samples);
        free(drums[i].name);
    }
    free(drums);
    free(drumTaste);
    free(drumPeaks);
    free(drumTasted);
    drumTasted = NULL;
    for (int i = 0; i < fileDrumCount; i++) free(fileDrums[i].name);
    free(fileDrums);
    for (int i = 0; i < filePresetCount; i++) {
        free(filePresets[i].name);
        free(filePresets[i].entries);
    }
    free(filePresets);
    close_vst();
    if (consoleModeSet) SetConsoleMode(consoleInput, consoleMode);
    consoleModeSet = 0;
    if (quitEvent) SetEvent(quitEvent);
}

static int stream_midi(const char *device, int channel)
{
    if (channel > 16) {
        log_error("MIDI channel %d is out of range (1-16).\n", channel);
        return 1;
    }
    g.midiChannel = channel;
    g.midiStream = 1;
    if (open_midi_input(device) != 0) return 1;

    if (channel) printf("streaming MIDI input '%s', channel %d\n", device, channel);
    else printf("streaming MIDI input '%s', all channels\n", device);

    while (!g.quit) Sleep(5);

    shutdown_monitor();
    return 0;
}

static void usage(void)
{
    printf(
        "restless_virtuoso - play ASIO inputs on ASIO outputs\n"
        "\n"
        "Usage:\n"
        "  restless_virtuoso --list\n"
        "  restless_virtuoso [options]\n"
        "\n"
        "Options:\n"
        "  -d, --driver NAME|INDEX  ASIO driver (default: " DEFAULT_DRIVER ", else first installed)\n"
        "  -i, --in LIST            input channels, 1-based: 1,2 or 1-4 (default 1,2; \"none\" for MIDI only)\n"
        "  -o, --out LIST           output channels (default 1,2)\n"
        "      --rate HZ            sample rate (default: the driver's current rate)\n"
        "      --buffer FRAMES      ASIO buffer size (default: the driver's preferred size)\n"
        "      --midi-in DEVICE     MIDI input by index or name\n"
        "      --midi-out DEVICE    forward MIDI input to this MIDI output\n"
        "      --no-synth           disable the built-in sine synth for MIDI input\n"
        "  -l, --list               list ASIO drivers and MIDI devices\n"
        "  -h, --help               show this help\n");
}

static int is_json_path(const char *path)
{
    const char *dot = strrchr(path, '.');
    return dot && !_stricmp(dot, ".json");
}

static int is_folder(const char *path)
{
    DWORD attributes = GetFileAttributesA(path);
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY);
}

static int json_string(const char *text, const char *key, char *out, size_t size)
{
    const char *value = json_value(text, key);
    if (!value || *value != '"') return 0;

    const char *end = strchr(value + 1, '"');
    if (!end || end == value + 1 || (size_t)(end - value - 1) >= size) return 0;

    size_t length = (size_t)(end - value - 1);
    memcpy(out, value + 1, length);
    out[length] = 0;
    return 1;
}

static int load_settings(const char *path, Options *options)
{
    FILE *file = fopen(path, "rb");
    if (!file) {
        log_error("Could not open settings '%s'.\n", path);
        return -1;
    }

    fseek(file, 0, SEEK_END);
    long size = ftell(file);
    fseek(file, 0, SEEK_SET);
    char *text = size >= 0 ? malloc((size_t)size + 1) : NULL;
    if (!text) {
        fclose(file);
        log_error("Could not read settings '%s'.\n", path);
        return -1;
    }
    text[fread(text, 1, (size_t)size, file)] = 0;
    fclose(file);

    int result = 0;
    if (!json_string(text, "instrument", options->instrument, sizeof options->instrument)) {
        log_error("Settings '%s' need an \"instrument\" string.\n", path);
        result = -1;
    } else {
        json_string(text, "samples_path", options->samplesPath, sizeof options->samplesPath);
        json_string(text, "vsts_path", options->vstsPath, sizeof options->vstsPath);
        const char *ratio = json_value(text, "samples_ratio");
        options->samplesRatio = ratio ? atoi(ratio) : 50;

        const char *scale = json_value(text, "random_scale");
        options->randomScale = scale && atoi(scale) == 1;

        const char *channel = json_value(text, "input-channel");
        char *stop = NULL;
        long number = channel ? strtol(channel, &stop, 10) : 0;
        if (!channel || stop == channel || number < 1) {
            log_error("Settings '%s' need a positive \"input-channel\".\n", path);
            result = -1;
        } else {
            options->inputChannel = (int)number;
            load_taste(text);
        }
    }
    free(text);
    return result;
}

static int parse_args(int argc, char **argv, Options *options)
{
    for (int i = 1; i < argc; i++) {
        const char *argument = argv[i];
        int hasValue = i + 1 < argc;

        if (!strcmp(argument, "--list") || !strcmp(argument, "-l")) options->list = 1;
        else if (!strcmp(argument, "--help") || !strcmp(argument, "-h")) { usage(); return 2; }
        else if (!strcmp(argument, "--no-synth")) options->synth = 0;
        else if (!strcmp(argument, "--driver") || !strcmp(argument, "-d")) {
            if (!hasValue) return 1;
            options->driver = argv[++i];
        } else if (!strcmp(argument, "--in") || !strcmp(argument, "-i")) {
            if (!hasValue) return 1;
            options->in = argv[++i];
        } else if (!strcmp(argument, "--out") || !strcmp(argument, "-o")) {
            if (!hasValue) return 1;
            options->out = argv[++i];
        } else if (!strcmp(argument, "--midi-in")) {
            if (!hasValue) return 1;
            options->midiIn = argv[++i];
        } else if (!strcmp(argument, "--midi-out")) {
            if (!hasValue) return 1;
            options->midiOut = argv[++i];
        } else if (!strcmp(argument, "--rate")) {
            if (!hasValue) return 1;
            options->rate = atoi(argv[++i]);
        } else if (!strcmp(argument, "--buffer")) {
            if (!hasValue) return 1;
            options->buffer = atoi(argv[++i]);
        } else {
            log_error("Unknown option '%s'.\n\n", argument);
            usage();
            return 1;
        }
    }

    if (options->rate < 0 || options->buffer < 0) {
        log_error("--rate and --buffer must not be negative.\n");
        return 1;
    }
    return 0;
}

static void pause_on_error(int exitCode)
{
    if (!exitCode) return;
    printf("\nPress Enter to close...");
    fflush(stdout);
    getchar();
}

static int run(int argc, char **argv, const wchar_t *folder)
{
    log_trace("run start\n");
    Options options;
    memset(&options, 0, sizeof options);
    options.synth = 1;

    const char *settings = !folder && argc > 1 && is_json_path(argv[1]) ? argv[1] : NULL;
    if (settings && load_settings(settings, &options) != 0) return 1;
    settingsPath = settings;
    settingsOptions = &options;

    int parsed = parse_args(argc - (settings ? 1 : 0), argv + (settings ? 1 : 0), &options);
    if (parsed == 2) return 0;
    if (parsed != 0) return 1;
    log_trace("arguments parsed\n");
    if (options.list) {
        list_devices();
        return 0;
    }

    memset(&g, 0, sizeof g);
    g.volume = 1.0f;
    g.synthEnabled = options.synth;
    g.samplesRatio = options.samplesRatio;
    g.midiInIndex = -1;
    g.midiOutIndex = -1;

    callbacks.bufferSwitch = on_buffer_switch;
    callbacks.sampleRateDidChange = on_sample_rate_changed;
    callbacks.asioMessage = on_asio_message;
    callbacks.bufferSwitchTimeInfo = on_buffer_switch_time_info;

    SetConsoleTitleA("restless_virtuoso");
    quitEvent = CreateEventA(NULL, TRUE, FALSE, NULL);
    SetConsoleCtrlHandler(ctrl_handler, TRUE);

    char names[64][128];
    int driverCount = list_drivers(names, 64);

    if (folder) {
        playlistMode = 1;
        if (start_playlist_w(folder) != 0) return 1;
    } else if (options.instrument[0]) {
        if (is_folder(options.instrument)) {
            playlistMode = 1;
            if (start_playlist(options.instrument) != 0) return 1;
        } else if (resolve_driver(options.instrument, names, driverCount)) {
            if (!options.driver) options.driver = options.instrument;
        } else if (resolve_midi_in(options.instrument) >= 0) {
            if (!options.samplesPath[0] && !options.vstsPath[0]) {
                int result = stream_midi(options.instrument, options.inputChannel);
                printf("\n");
                return result;
            }
            if (options.inputChannel > 16) {
                log_error("MIDI channel %d is out of range (1-16).\n", options.inputChannel);
                return 1;
            }
            instrumentMode = 1;
            instrumentName = options.instrument;
            g.synthEnabled = 0;
            g.midiChannel = options.inputChannel;
            if (!options.midiIn) options.midiIn = options.instrument;
        } else {
            log_error("Instrument '%s' is neither a folder of audio files, an installed ASIO driver "
                      "nor a MIDI input.\n", options.instrument);
            list_devices();
            return 1;
        }
    }

    HWND window = create_window(GetModuleHandleA(NULL));
    if (!window) {
        log_error("Could not create the ASIO message window.\n");
        return 1;
    }
    log_trace("window %p\n", (void *)window);

    HRESULT com = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    if (FAILED(com) && com != RPC_E_CHANGED_MODE) {
        log_error("COM initialization failed (0x%08lx).\n", (unsigned long)com);
        DestroyWindow(window);
        return 1;
    }
    log_trace("com 0x%08lx\n", (unsigned long)com);
    open_master_volume();
    g.volume = read_master_volume();

    const char *driver = resolve_driver(options.driver ? options.driver : DEFAULT_DRIVER, names, driverCount);
    if (!driver && !options.driver) driver = resolve_driver(NULL, names, driverCount);
    if (!driver) {
        if (!driverCount) log_error("No ASIO driver is installed.\n");
        else {
            log_error("ASIO driver '%s' not found. Installed drivers:\n", options.driver);
            for (int i = 0; i < driverCount; i++) log_error("  %d  %s\n", i, names[i]);
        }
        shutdown_monitor();
        DestroyWindow(window);
        if (SUCCEEDED(com)) CoUninitialize();
        return 1;
    }
    log_trace("driver '%s'\n", driver);

    int exitCode = 1;
    if (open_driver(driver, window) != 0) goto done;
    log_trace("driver opened\n");

    if (playlistMode || instrumentMode) {
        if (select_channels(options.in, g.inCount, g.inCh, &g.nIn, 0, "input") != 0) goto done;
    } else if (options.inputChannel > 0 && !options.in) {
        if (options.inputChannel > g.inCount) {
            log_error("input-channel %d is out of range (the driver has %d inputs).\n",
                      options.inputChannel, g.inCount);
            goto done;
        }
        g.inCh[0] = options.inputChannel - 1;
        g.nIn = 1;
    } else if (select_channels(options.in, g.inCount, g.inCh, &g.nIn, 2, "input") != 0) {
        goto done;
    }
    if (select_channels(options.out, g.outCount, g.outCh, &g.nOut, 2, "output") != 0) goto done;

    if (g.nOut == 0) {
        log_error("At least one output channel is required.\n");
        goto done;
    }
    if (g.nIn == 0 && !options.midiIn && !playlistMode) {
        log_error("No input channels and no MIDI input, so there is nothing to play.\n");
        goto done;
    }

    if (options.midiIn && open_midi_input(options.midiIn) != 0) goto done;
    if (options.midiOut && open_midi_output(options.midiOut) != 0) goto done;

    if (setup_rate(options.rate) != 0) goto done;
    if (setup_buffer(options.buffer) != 0) goto done;
    open_cue();
    if (playlistMode || instrumentMode) load_song();
    if (options.randomScale) {
        scaleFollow = 1;
        follow_scale();
        if (!scaleMask) draw_scale();
    }
    if (instrumentMode) {
        if (options.samplesPath[0]) load_drums(options.samplesPath);
        if (options.vstsPath[0]) load_vsts(options.vstsPath);
        int previous = last_sound_number();
        if (!previous || restore_sound(previous) != 0) {
            choose_sound(options.samplesRatio);
            if (!g.samplesSound) start_drawn_vst();
        }
    }
    log_trace("rate and buffer set\n");
    if (create_buffers() != 0) goto done;
    log_trace("buffers created\n");
    rating_start();

    if (playlistMode) {
        printf("  %d audio files mixed from ", stemCount);
        print_wide_path(playlistFolder);
        printf("\n");
    }
    print_sound("sound ");

    if (g.midiInIndex >= 0) {
        MIDIINCAPS caps;
        if (midiInGetDevCaps((UINT)g.midiInIndex, &caps, sizeof caps) == MMSYSERR_NOERROR)
            printf("  MIDI in %d (%s)%s\n", g.midiInIndex, caps.szPname, g.synthEnabled ? ", sine synth on" : "");
    }
    if (g.midiOutIndex >= 0) {
        MIDIOUTCAPS caps;
        if (midiOutGetDevCaps((UINT)g.midiOutIndex, &caps, sizeof caps) == MMSYSERR_NOERROR)
            printf("  MIDI thru -> %d (%s)\n", g.midiOutIndex, caps.szPname);
    }

    if (playlistMode) publish_palette(playlistFolder);
    take_join();
    if (instrumentMode) {
        LARGE_INTEGER at;
        QueryPerformanceCounter(&at);
        add_sound_mark(g.soundNumber, (ULONGLONG)at.QuadPart);
    }
    if (g.asio->vtable->start(g.asio) != ASE_OK) {
        log_error("ASIOStart failed.\n");
        goto done;
    }
    g.started = 1;
    songLive = 1;
    if (playlistMode) send_loop_start();
    log_trace("started\n");

    init_console_input();

    ULONGLONG lastDraw = 0;
    while (!g.quit) {
        pump_messages();

        if (InterlockedExchange(&g.reset, 0)) {
            log_line("driver settings changed, reconfiguring");
            if (g.started) {
                g.asio->vtable->stop(g.asio);
                g.started = 0;
            }
            if (g.buffersCreated) {
                g.asio->vtable->disposeBuffers(g.asio);
                g.buffersCreated = 0;
            }
            free_buffers();
            vst_stop();

            if (setup_rate(0) != 0 || setup_buffer(options.buffer) != 0 ||
                vst_start() != 0 || create_buffers() != 0 ||
                g.asio->vtable->start(g.asio) != ASE_OK) {
                log_line("could not reopen the ASIO buffers; restart the program");
                break;
            }
            g.started = 1;
            lastDraw = 0;
        }

        if (take.active && take.palette && WaitForSingleObject(take.palette, 0) == WAIT_OBJECT_0) {
            if (g.started) {
                g.asio->vtable->stop(g.asio);
                g.started = 0;
            }
            take_finish();
            if (g.asio->vtable->start(g.asio) == ASE_OK) g.started = 1;
            lastDraw = 0;
        }
        if (playlistMode && InterlockedExchange(&playlistRefresh, 0)) {
            if (g.started) {
                g.asio->vtable->stop(g.asio);
                g.started = 0;
            }
            refresh_playlist();
            if (g.asio->vtable->start(g.asio) == ASE_OK) g.started = 1;
            lastDraw = 0;
        }
        take_open();
        take_flush();

        read_console_input();
        sync_song();
        follow_song_sound();

        ULONGLONG now = GetTickCount64();
        if (now - lastDraw >= 100) {
            g.volume = read_master_volume();
            if (playlistMode) mute_stems();
            else if (instrumentMode) announce_instrument();
            rating_poll();
            draw_meters();
            lastDraw = now;
        }
        Sleep(5);
    }

    exitCode = 0;

done:
    shutdown_monitor();
    DestroyWindow(window);
    if (SUCCEEDED(com)) CoUninitialize();
    printf("\n");
    return exitCode;
}

static wchar_t *dropped_folder(void)
{
    int count = 0;
    LPWSTR *arguments = CommandLineToArgvW(GetCommandLineW(), &count);
    if (!arguments) return NULL;

    wchar_t *folder = NULL;
    if (count > 1) {
        wchar_t *path = long_path(arguments[1]);
        if (path && is_folder_w(path)) folder = path;
        else free(path);
    }
    LocalFree(arguments);
    return folder;
}

int main(int argc, char **argv)
{
    SetUnhandledExceptionFilter(crash_handler);
    open_error_log();
    srand((unsigned)GetTickCount64());
    wchar_t *folder = dropped_folder();
    int exitCode = run(argc - (folder ? 1 : 0), argv + (folder ? 1 : 0), folder);
    if (quitEvent) SetEvent(quitEvent);
    pause_on_error(exitCode);
    if (errorLog) fclose(errorLog);
    return exitCode;
}
