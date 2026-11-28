"""Live view of the shared group buffer of every running restless_virtuoso.exe.

The cumulative history and average lag buffers live only in an instance's own
memory, so each window reads them out of its process with ReadProcessMemory at the
ratingScores symbol its linked exe carries. Nothing in the app has to change.
Double-click this file; a window opens for every instance it finds, and for each
one that starts while it runs. Anything that goes wrong is printed in the console
this script runs in and shown in the window it belongs to.
"""

import ctypes
import struct
import sys
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
PLOT_LABELS = (("cumulative history", HISTORY_COLOUR), ("average lag", LAG_COLOUR))

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
kernel32.GetExitCodeProcess.argtypes = [wintypes.HANDLE, ctypes.POINTER(wintypes.DWORD)]
STILL_ACTIVE = 259
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


def alive(handle):
    code = wintypes.DWORD()
    return bool(kernel32.GetExitCodeProcess(handle, ctypes.byref(code))) and code.value == STILL_ACTIVE


def module_base(handle):
    module = wintypes.HMODULE()
    needed = wintypes.DWORD()
    if not psapi.EnumProcessModules(handle, ctypes.byref(module), ctypes.sizeof(module),
                                    ctypes.byref(needed)):
        return 0
    return module.value


def instances():
    """The running instances, oldest first."""
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
                                      ctypes.byref(read)):
        raise OSError(ctypes.get_last_error(),
                      "ReadProcessMemory at 0x%x, %d of %d bytes" % (address, read.value, SIZE))
    if read.value != SIZE:
        raise OSError(0, "ReadProcessMemory at 0x%x read %d of %d bytes"
                      % (address, read.value, SIZE))

    raw = buffer.raw
    scores = struct.unpack_from("<i", raw, SCORE_COUNT)[0]
    lags = struct.unpack_from("<q", raw, LAG_COUNT)[0]
    peak = struct.unpack_from("<d", raw, MAX_PEAK)[0]
    if not (0 <= scores < 1 << 31 and 0 <= lags < 1 << 40 and -1e300 < peak < 1e300):
        raise ValueError("group buffer at 0x%x does not look like one: scores %d, lags %d, peak %r"
                         % (address, scores, lags, peak))

    return (struct.unpack_from("<%dd" % VALUES, raw, 0),
            struct.unpack_from("<%dd" % VALUES, raw, LAG_OFFSET))


class InstanceWindow(tk.Toplevel):
    """One instance's group buffer, read from that instance as it runs."""

    def __init__(self, watcher, count, pid, handle, path):
        rva = symbol_rva(path, "ratingScores")
        if not rva:
            raise LookupError("no ratingScores symbol in %s" % path)

        super().__init__(watcher.root)
        self.watcher = watcher
        self.pid = pid
        self.handle = handle
        self.address = module_base(handle) + rva
        self.ceiling = {HISTORY_COLOUR: 1e-6, LAG_COLOUR: 1e-6}
        self.plots = {}
        self.job = None
        self.caption = "pid %d  buffer 0x%x  %s" % (pid, self.address, path)

        self.title("group buffer  pid %d" % pid)
        self.geometry("900x560+%d+%d" % (420 + 28 * count, 40 + 28 * count))
        self.configure(bg=BACKGROUND)
        self.protocol("WM_DELETE_WINDOW", self.close)

        self.status = tk.Label(self, text=self.caption, anchor="w", fg=INK, bg=BACKGROUND,
                               justify="left", wraplength=860, font=("Consolas", 9))
        self.status.pack(fill="x", padx=12, pady=(10, 0))
        for label, colour in PLOT_LABELS:
            frame = tk.Frame(self, bg=BACKGROUND)
            tk.Label(frame, text=label, anchor="w", fg=colour, bg=BACKGROUND,
                     font=("Consolas", 10)).pack(fill="x")
            canvas = tk.Canvas(frame, bg="#0b0c0f", height=230, highlightthickness=0)
            canvas.pack(fill="both", expand=True, pady=(2, 10))
            frame.pack(fill="both", expand=True, padx=12)
            self.plots[colour] = canvas

        self.job = self.after(0, self.tick)

    def tick(self):
        try:
            history, lag = read_group(self.handle, self.address)
            self.plot(HISTORY_COLOUR, history)
            self.plot(LAG_COLOUR, lag)
            self.status.config(text=self.caption, fg=INK)
        except Exception as error:
            if not alive(self.handle):
                self.close()
                return
            message = "%s: %s" % (type(error).__name__, error)
            if message != self.status.cget("text"):
                self.status.config(text=message, fg=LAG_COLOUR)
                traceback.print_exc()
        self.job = self.after(100, self.tick)

    def close(self):
        if self.job is not None:
            self.after_cancel(self.job)
            self.job = None
        if self.handle:
            kernel32.CloseHandle(self.handle)
            self.handle = None
        self.watcher.windows.pop(self.pid, None)
        self.destroy()

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


class Watcher:
    """Opening windows for the instances that are running, and for the ones that start."""

    def __init__(self, root):
        self.root = root
        self.windows = {}
        self.seen = set()
        self.count = 0

        root.title("group buffer watch")
        root.geometry("380x64+40+40")
        root.configure(bg=BACKGROUND)
        tk.Label(root, text="a window opens for each running instance, and each new one",
                 fg=INK, bg=BACKGROUND, wraplength=350, justify="left",
                 font=("Consolas", 9)).pack(padx=12, pady=12, anchor="w")
        root.after(0, self.scan)

    def scan(self):
        found = instances()
        self.seen &= {pid for _, pid, _, _ in found}
        for _, pid, handle, path in found:
            if pid in self.seen:
                kernel32.CloseHandle(handle)
                continue
            self.count += 1
            self.seen.add(pid)
            try:
                self.windows[pid] = InstanceWindow(self, self.count, pid, handle, path)
            except Exception:
                kernel32.CloseHandle(handle)
                traceback.print_exc()
        self.root.after(1000, self.scan)


def main():
    root = tk.Tk()
    Watcher(root)
    root.mainloop()
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except SystemExit:
        raise
    except KeyboardInterrupt:
        pass
    except BaseException:
        traceback.print_exc()
        input("press Enter to close")
