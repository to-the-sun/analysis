"""Live view of the shared group buffer of a running restless_virtuoso.exe.

The cumulative history and average lag buffers live only in the instance's own
memory, so they are read out of the running process with ReadProcessMemory at the
ratingScores symbol its linked exe carries. Nothing in the app has to change.
Double-click this file; it finds the newest running instance by itself.
"""

import ctypes
import struct
import tkinter as tk
import traceback
from ctypes import wintypes

PROCESS_QUERY_INFORMATION = 0x0400
PROCESS_VM_READ = 0x0010
TH32CS_SNAPPROCESS = 0x00000002
INVALID = (None, -1, 0xFFFFFFFFFFFFFFFF)

# The field layout of SharedTransientBuffer, the group buffer the analyzers share.
VALUES = 15001                                      # BUFFER_LEN, the 15 s window in ms
MAX_PEAK = VALUES * 8
SCORE_COUNT = VALUES * 8 + 32
LAG_OFFSET = VALUES * 8 + 48
LAG_COUNT = LAG_OFFSET + VALUES * 8
SIZE = LAG_COUNT + 8

BACKGROUND = "#14161a"
HISTORY_COLOUR = "#7fd1ff"
LAG_COLOUR = "#ffb36b"
INK = "#8b93a1"

kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
psapi = ctypes.WinDLL("psapi", use_last_error=True)


class PROCESSENTRY32W(ctypes.Structure):
    _fields_ = [("dwSize", wintypes.DWORD), ("cntUsage", wintypes.DWORD),
                ("th32ProcessID", wintypes.DWORD), ("th32DefaultHeapID", ctypes.c_size_t),
                ("th32ModuleID", wintypes.DWORD), ("cntThreads", wintypes.DWORD),
                ("th32ParentProcessID", wintypes.DWORD), ("pcPriClassBase", wintypes.LONG),
                ("dwFlags", wintypes.DWORD), ("szExeFile", wintypes.WCHAR * 260)]


kernel32.CreateToolhelp32Snapshot.restype = wintypes.HANDLE
kernel32.CreateToolhelp32Snapshot.argtypes = [wintypes.DWORD, wintypes.DWORD]
kernel32.Process32FirstW.argtypes = [wintypes.HANDLE, ctypes.POINTER(PROCESSENTRY32W)]
kernel32.Process32NextW.argtypes = [wintypes.HANDLE, ctypes.POINTER(PROCESSENTRY32W)]
kernel32.OpenProcess.restype = wintypes.HANDLE
kernel32.OpenProcess.argtypes = [wintypes.DWORD, wintypes.BOOL, wintypes.DWORD]
kernel32.CloseHandle.argtypes = [wintypes.HANDLE]
kernel32.ReadProcessMemory.argtypes = [wintypes.HANDLE, ctypes.c_void_p, ctypes.c_void_p,
                                       ctypes.c_size_t, ctypes.POINTER(ctypes.c_size_t)]
kernel32.ReadProcessMemory.restype = wintypes.BOOL
kernel32.QueryFullProcessImageNameW.argtypes = [wintypes.HANDLE, wintypes.DWORD,
                                                wintypes.LPWSTR, ctypes.POINTER(wintypes.DWORD)]
kernel32.GetProcessTimes.argtypes = [wintypes.HANDLE] + [ctypes.c_void_p] * 4
psapi.EnumProcessModules.argtypes = [wintypes.HANDLE, ctypes.POINTER(wintypes.HMODULE),
                                     wintypes.DWORD, ctypes.POINTER(wintypes.DWORD)]


def image_path(handle):
    text = ctypes.create_unicode_buffer(32768)
    length = wintypes.DWORD(len(text))
    if not kernel32.QueryFullProcessImageNameW(handle, 0, text, ctypes.byref(length)):
        return None
    return text.value


def created(handle):
    times = [wintypes.FILETIME() for _ in range(4)]
    if not kernel32.GetProcessTimes(handle, *[ctypes.byref(t) for t in times]):
        return 0
    return times[0].dwHighDateTime << 32 | times[0].dwLowDateTime


def module_base(handle):
    module = wintypes.HMODULE()
    needed = wintypes.DWORD()
    if not psapi.EnumProcessModules(handle, ctypes.byref(module), ctypes.sizeof(module),
                                    ctypes.byref(needed)):
        return 0
    return module.value


def instances():
    """The running instances, newest last."""
    snapshot = kernel32.CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0)
    if snapshot in INVALID:
        return []

    entry = PROCESSENTRY32W()
    entry.dwSize = ctypes.sizeof(entry)
    found = []
    more = kernel32.Process32FirstW(snapshot, ctypes.byref(entry))
    while more:
        if entry.szExeFile.lower() == "restless_virtuoso.exe":
            handle = kernel32.OpenProcess(
                PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, False, entry.th32ProcessID)
            path = image_path(handle) if handle else None
            if path:
                found.append((created(handle), entry.th32ProcessID, handle, path))
            elif handle:
                kernel32.CloseHandle(handle)
        more = kernel32.Process32NextW(snapshot, ctypes.byref(entry))

    kernel32.CloseHandle(snapshot)
    return sorted(found, key=lambda instance: instance[0])


def symbol_rva(path, name):
    """The RVA of a symbol, from the symbol table the linked exe keeps."""
    data = open(path, "rb").read()
    header = struct.unpack_from("<I", data, 0x3C)[0]
    _, _, _, table, entries, options, _ = struct.unpack_from("<HHIIIHH", data, header + 4)
    sections = header + 24 + options
    strings = table + entries * 18

    i = 0
    while i < entries:
        entry = table + i * 18
        if data[entry:entry + 4] == b"\0\0\0\0":
            offset = struct.unpack_from("<I", data, entry + 4)[0]
            end = data.index(b"\0", strings + offset)
            symbol = data[strings + offset:end].decode("ascii", "replace")
        else:
            symbol = data[entry:entry + 8].rstrip(b"\0").decode("ascii", "replace")
        if symbol == name:
            value, section = struct.unpack_from("<Ih", data, entry + 8)
            return struct.unpack_from("<I", data, sections + (section - 1) * 40 + 12)[0] + value
        i += 1 + data[entry + 17]
    return 0


def read_group(handle, address):
    buffer = ctypes.create_string_buffer(SIZE)
    read = ctypes.c_size_t()
    if not kernel32.ReadProcessMemory(handle, ctypes.c_void_p(address), buffer, SIZE,
                                      ctypes.byref(read)) or read.value != SIZE:
        return None

    raw = buffer.raw
    scores = struct.unpack_from("<i", raw, SCORE_COUNT)[0]
    lags = struct.unpack_from("<q", raw, LAG_COUNT)[0]
    peak = struct.unpack_from("<d", raw, MAX_PEAK)[0]
    if not (0 <= scores < 1 << 31 and 0 <= lags < 1 << 40 and -1e300 < peak < 1e300):
        return None

    return (struct.unpack_from("<%dd" % VALUES, raw, 0),
            struct.unpack_from("<%dd" % VALUES, raw, LAG_OFFSET))


class Watch:
    def __init__(self, root):
        self.root = root
        self.target = None
        self.address = 0
        self.ceiling = {HISTORY_COLOUR: 1e-6, LAG_COLOUR: 1e-6}
        self.plots = {}

        root.title("restless_virtuoso  shared group buffer")
        root.geometry("900x560")
        root.configure(bg=BACKGROUND)

        self.status = tk.Label(root, text="", anchor="w", fg=INK, bg=BACKGROUND,
                               font=("Consolas", 9))
        self.status.pack(fill="x", padx=12, pady=(10, 0))

        for label, colour in (("cumulative history", HISTORY_COLOUR), ("average lag", LAG_COLOUR)):
            frame = tk.Frame(root, bg=BACKGROUND)
            tk.Label(frame, text=label, anchor="w", fg=colour, bg=BACKGROUND,
                     font=("Consolas", 10)).pack(fill="x")
            canvas = tk.Canvas(frame, bg="#0b0c0f", height=230, highlightthickness=0)
            canvas.pack(fill="both", expand=True, pady=(2, 10))
            frame.pack(fill="both", expand=True, padx=12)
            self.plots[colour] = canvas

        root.after(0, self.tick)

    def tick(self):
        data = self.read()
        if data is None:
            self.status.config(text="waiting for a running restless_virtuoso.exe")
        else:
            self.status.config(text="pid %d  %s" % (self.target[1], self.target[3]))
            self.plot(HISTORY_COLOUR, data[0])
            self.plot(LAG_COLOUR, data[1])
        self.root.after(80, self.tick)

    def attach(self):
        found = instances()
        if not found:
            return
        for other in found[:-1]:
            kernel32.CloseHandle(other[2])
        self.target = found[-1]

    def read(self):
        if self.target is None:
            self.attach()
            if self.target is None:
                return None

        handle, path = self.target[2], self.target[3]
        if not self.address:
            self.address = module_base(handle) + symbol_rva(path, "ratingScores")

        data = read_group(handle, self.address) if self.address else None
        if data is None:
            kernel32.CloseHandle(handle)
            self.target = None
            self.address = 0
        return data

    def plot(self, colour, values):
        canvas = self.plots[colour]
        width, height = canvas.winfo_width(), canvas.winfo_height()
        if width < 8 or height < 8:
            return

        ceiling = max(self.ceiling[colour] * 0.99, max(values) * 1.1, 1e-6)
        self.ceiling[colour] = ceiling

        step = max(1, len(values) // width)
        points = []
        for start in range(0, len(values), step):
            points.append(start * (width - 1) / (len(values) - 1))
            points.append(height - 3 - max(values[start:start + step]) / ceiling * (height - 12))

        canvas.delete("all")
        canvas.create_line(*points, fill=colour, width=1)
        canvas.create_text(6, height - 6, text="15 s ago", fill=INK, anchor="sw",
                           font=("Consolas", 8))
        canvas.create_text(width - 6, height - 6, text="now", fill=INK, anchor="se",
                           font=("Consolas", 8))


def main():
    root = tk.Tk()
    Watch(root)
    root.mainloop()
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except SystemExit:
        raise
    except BaseException:
        from tkinter import messagebox
        messagebox.showerror("group buffer watch", traceback.format_exc())
