import ctypes
import queue
import sys
from ctypes import wintypes

DEVICE = "KeyRig 49"
MIM_DATA = 0x3C3
CALLBACK_FUNCTION = 0x00030000

winmm = ctypes.WinDLL("winmm")


class MIDIINCAPSW(ctypes.Structure):
    _fields_ = [("wMid", wintypes.WORD), ("wPid", wintypes.WORD),
                ("vDriverVersion", wintypes.UINT), ("szPname", wintypes.WCHAR * 32),
                ("dwSupport", wintypes.DWORD)]


MidiInProc = ctypes.WINFUNCTYPE(None, wintypes.HANDLE, wintypes.UINT,
                                wintypes.DWORD_PTR, wintypes.DWORD_PTR, wintypes.DWORD_PTR)

winmm.midiInGetNumDevs.restype = wintypes.UINT
winmm.midiInGetDevCapsW.argtypes = [wintypes.UINT, ctypes.POINTER(MIDIINCAPSW), wintypes.UINT]
winmm.midiInOpen.argtypes = [ctypes.POINTER(wintypes.HANDLE), wintypes.UINT,
                             wintypes.DWORD_PTR, wintypes.DWORD_PTR, wintypes.DWORD]
winmm.midiInStart.argtypes = [wintypes.HANDLE]
winmm.midiInStop.argtypes = [wintypes.HANDLE]
winmm.midiInClose.argtypes = [wintypes.HANDLE]
winmm.midiInGetErrorTextW.argtypes = [wintypes.UINT, wintypes.LPWSTR, wintypes.UINT]


def device_name(index):
    caps = MIDIINCAPSW()
    if winmm.midiInGetDevCapsW(index, ctypes.byref(caps), ctypes.sizeof(caps)) != 0:
        return None
    return caps.szPname


def error_text(code):
    text = ctypes.create_unicode_buffer(256)
    if winmm.midiInGetErrorTextW(code, text, ctypes.sizeof(text)) != 0:
        return f"error {code}"
    return text.value


def main():
    names = [device_name(i) for i in range(winmm.midiInGetNumDevs())]
    index = next((i for i, name in enumerate(names) if (name or "").lower() == DEVICE.lower()), None)
    if index is None:
        print(f"MIDI input '{DEVICE}' not found; available inputs: {', '.join(filter(None, names)) or 'none'}")
        return 1

    received = queue.Queue()

    @MidiInProc
    def on_midi(handle, message, instance, param1, param2):
        if message == MIM_DATA:
            received.put(param1)

    handle = wintypes.HANDLE()
    result = winmm.midiInOpen(ctypes.byref(handle), index,
                              ctypes.cast(on_midi, ctypes.c_void_p).value, 0, CALLBACK_FUNCTION)
    if result == 0:
        result = winmm.midiInStart(handle)
        if result != 0:
            winmm.midiInClose(handle)
    if result != 0:
        print(f"MIDI input '{names[index]}' could not be claimed: {error_text(result)} (error {result})")
        return 1

    print(f"claimed MIDI input '{names[index]}'; press Ctrl+C to release it", flush=True)
    try:
        while True:
            data = received.get()
            print(f"MIDI ch {(data & 0x0F) + 1}  {data & 0xFF:02X} "
                  f"{(data >> 8) & 0x7F:02X} {(data >> 16) & 0x7F:02X}", flush=True)
    except KeyboardInterrupt:
        pass
    finally:
        winmm.midiInStop(handle)
        winmm.midiInClose(handle)
    return 0


if __name__ == "__main__":
    sys.exit(main())
