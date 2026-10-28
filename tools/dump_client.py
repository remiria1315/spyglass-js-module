import ctypes
import ctypes.wintypes as wt
import sys

PROCESS_QUERY_INFORMATION = 0x0400
PROCESS_VM_READ = 0x0010
TH32CS_SNAPPROCESS = 0x2
TH32CS_SNAPMODULE = 0x8
TH32CS_SNAPMODULE32 = 0x10
MEM_COMMIT = 0x1000
PAGE_NOACCESS = 0x01
PAGE_GUARD = 0x100
CHUNK = 1 << 20
INVALID_HANDLE = ctypes.c_void_p(-1).value


class ProcessEntry(ctypes.Structure):
    _fields_ = [
        ("dwSize", wt.DWORD),
        ("cntUsage", wt.DWORD),
        ("th32ProcessID", wt.DWORD),
        ("th32DefaultHeapID", ctypes.c_size_t),
        ("th32ModuleID", wt.DWORD),
        ("cntThreads", wt.DWORD),
        ("th32ParentProcessID", wt.DWORD),
        ("pcPriClassBase", wt.LONG),
        ("dwFlags", wt.DWORD),
        ("szExeFile", ctypes.c_wchar * 260),
    ]


class ModuleEntry(ctypes.Structure):
    _fields_ = [
        ("dwSize", wt.DWORD),
        ("th32ModuleID", wt.DWORD),
        ("th32ProcessID", wt.DWORD),
        ("GlblcntUsage", wt.DWORD),
        ("ProccntUsage", wt.DWORD),
        ("modBaseAddr", ctypes.c_void_p),
        ("modBaseSize", wt.DWORD),
        ("hModule", ctypes.c_void_p),
        ("szModule", ctypes.c_wchar * 256),
        ("szExePath", ctypes.c_wchar * 260),
    ]


class MemoryInfo(ctypes.Structure):
    _fields_ = [
        ("BaseAddress", ctypes.c_void_p),
        ("AllocationBase", ctypes.c_void_p),
        ("AllocationProtect", wt.DWORD),
        ("PartitionId", wt.WORD),
        ("RegionSize", ctypes.c_size_t),
        ("State", wt.DWORD),
        ("Protect", wt.DWORD),
        ("Type", wt.DWORD),
    ]


def main():
    name = "Minecraft.Windows.exe"
    output = sys.argv[1] if len(sys.argv) > 1 else "Minecraft.Windows.dump"

    kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
    kernel32.CreateToolhelp32Snapshot.restype = ctypes.c_void_p
    kernel32.CreateToolhelp32Snapshot.argtypes = [wt.DWORD, wt.DWORD]
    kernel32.Process32FirstW.argtypes = [ctypes.c_void_p, ctypes.POINTER(ProcessEntry)]
    kernel32.Process32NextW.argtypes = [ctypes.c_void_p, ctypes.POINTER(ProcessEntry)]
    kernel32.Module32FirstW.argtypes = [ctypes.c_void_p, ctypes.POINTER(ModuleEntry)]
    kernel32.OpenProcess.restype = ctypes.c_void_p
    kernel32.OpenProcess.argtypes = [wt.DWORD, wt.BOOL, wt.DWORD]
    kernel32.VirtualQueryEx.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.POINTER(MemoryInfo), ctypes.c_size_t]
    kernel32.VirtualQueryEx.restype = ctypes.c_size_t
    kernel32.ReadProcessMemory.argtypes = [
        ctypes.c_void_p, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_size_t, ctypes.POINTER(ctypes.c_size_t)
    ]
    kernel32.CloseHandle.argtypes = [ctypes.c_void_p]

    snapshot = kernel32.CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0)
    entry = ProcessEntry()
    entry.dwSize = ctypes.sizeof(entry)
    pid = None
    more = kernel32.Process32FirstW(snapshot, ctypes.byref(entry))
    while more:
        if entry.szExeFile.lower() == name.lower():
            pid = entry.th32ProcessID
            break
        more = kernel32.Process32NextW(snapshot, ctypes.byref(entry))
    kernel32.CloseHandle(snapshot)
    if pid is None:
        sys.exit(f"{name} is not running")

    snapshot = kernel32.CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid)
    if snapshot == INVALID_HANDLE:
        sys.exit(f"the modules of process {pid} cannot be listed, run this as administrator")
    module = ModuleEntry()
    module.dwSize = ctypes.sizeof(module)
    found = kernel32.Module32FirstW(snapshot, ctypes.byref(module))
    kernel32.CloseHandle(snapshot)
    if not found:
        sys.exit(f"the main module of process {pid} cannot be read, run this as administrator")
    base = module.modBaseAddr
    size = module.modBaseSize

    process = kernel32.OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, False, pid)
    if not process:
        sys.exit(f"process {pid} cannot be opened, run this as administrator")

    image = bytearray(size)
    address = base
    unreadable = 0
    info = MemoryInfo()
    while address < base + size:
        if not kernel32.VirtualQueryEx(process, address, ctypes.byref(info), ctypes.sizeof(info)):
            break
        end = min(address + info.RegionSize, base + size)
        readable = info.State == MEM_COMMIT and not info.Protect & (PAGE_NOACCESS | PAGE_GUARD)
        position = address
        while readable and position < end:
            length = min(CHUNK, end - position)
            buffer = (ctypes.c_char * length)()
            read = ctypes.c_size_t()
            if kernel32.ReadProcessMemory(process, position, buffer, length, ctypes.byref(read)):
                image[position - base:position - base + read.value] = buffer.raw[:read.value]
            else:
                unreadable += length
            position += length
        if not readable:
            unreadable += end - address
        address = end
    kernel32.CloseHandle(process)

    with open(output, "wb") as file:
        file.write(image)
    print(f"wrote {size} bytes of {name} (pid {pid}, base 0x{base:x}) to {output}, {unreadable} bytes were not readable")


if __name__ == "__main__":
    main()
