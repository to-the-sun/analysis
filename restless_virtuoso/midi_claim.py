import ctypes
import queue
import sys
import time
import traceback

DEVICE = "KeyRig 49"
RETRY_SECONDS = 2
MIM_DATA = 0x3C3
CALLBACK_FUNCTION = 0x00030000

winmm = ctypes.WinDLL("winmm")


class MIDIINCAPSW(ctypes.Structure):
    _fields_ = [("wMid", ctypes.c_ushort), ("wPid", ctypes.c_ushort),
                ("vDriverVersion", ctypes.c_uint), ("szPname", ctypes.c_wchar * 32),
                ("dwSupport", ctypes.c_uint)]


MidiInProc = ctypes.WINFUNCTYPE(None, ctypes.c_void_p, ctypes.c_uint,
                                ctypes.c_size_t, ctypes.c_size_t, ctypes.c_size_t)

winmm.midiInGetNumDevs.restype = ctypes.c_uint
winmm.midiInGetDevCapsW.argtypes = [ctypes.c_uint, ctypes.POINTER(MIDIINCAPSW), ctypes.c_uint]
winmm.midiInOpen.argtypes = [ctypes.POINTER(ctypes.c_void_p), ctypes.c_uint,
                             ctypes.c_size_t, ctypes.c_size_t, ctypes.c_uint]
winmm.midiInStart.argtypes = [ctypes.c_void_p]
winmm.midiInStop.argtypes = [ctypes.c_void_p]
winmm.midiInClose.argtypes = [ctypes.c_void_p]
winmm.midiInGetErrorTextW.argtypes = [ctypes.c_uint, ctypes.POINTER(ctypes.c_wchar), ctypes.c_uint]

received = queue.Queue()
status = None


@MidiInProc
def on_midi(handle, message, instance, param1, param2):
    if message == MIM_DATA:
        received.put(param1)


def device_name(index):
    caps = MIDIINCAPSW()
    if winmm.midiInGetDevCapsW(index, ctypes.byref(caps), ctypes.sizeof(caps)) != 0:
        return None
    return caps.szPname


def error_text(code):
    text = ctypes.create_unicode_buffer(256)
    if winmm.midiInGetErrorTextW(code, text, len(text)) != 0:
        return f"error {code}"
    return text.value


def report(text):
    global status
    if text != status:
        status = text
        print(text, flush=True)


def claim():
    names = [device_name(i) for i in range(winmm.midiInGetNumDevs())]
    index = next((i for i, name in enumerate(names) if (name or "").lower() == DEVICE.lower()), None)
    if index is None:
        report(f"MIDI input '{DEVICE}' not found; available inputs: {', '.join(filter(None, names)) or 'none'}")
        return None

    handle = ctypes.c_void_p()
    result = winmm.midiInOpen(ctypes.byref(handle), index,
                              ctypes.cast(on_midi, ctypes.c_void_p).value, 0, CALLBACK_FUNCTION)
    if result == 0:
        result = winmm.midiInStart(handle)
        if result != 0:
            winmm.midiInClose(handle)
    if result != 0:
        report(f"MIDI input '{names[index]}' could not be claimed: {error_text(result)} (error {result})")
        return None

    report(f"claimed MIDI input '{names[index]}'; press Ctrl+C to release it")
    return handle


def listen(handle):
    try:
        while True:
            data = received.get()
            print(f"MIDI ch {(data & 0x0F) + 1}  {data & 0xFF:02X} "
                  f"{(data >> 8) & 0x7F:02X} {(data >> 16) & 0x7F:02X}", flush=True)
    finally:
        winmm.midiInStop(handle)
        winmm.midiInClose(handle)


def main():
    while True:
        try:
            handle = claim()
            if handle:
                listen(handle)
                return 0
        except KeyboardInterrupt:
            return 0
        except Exception:
            print(traceback.format_exc(), end="", flush=True)
        time.sleep(RETRY_SECONDS)


if __name__ == "__main__":
    try:
        sys.exit(main())
    except KeyboardInterrupt:
        pass
    except BaseException:
        traceback.print_exc()
        input("press Enter to close")
