const { writeFile } = require("fs");

// added code
const koffi = require("koffi");
const kernel32 = koffi.load("kernel32.dll");

const PROCESSENTRY32W = koffi.struct("PROCESSENTRY32W", {
  dwSize: "uint32",
  cntUsage: "uint32",
  th32ProcessID: "uint32",
  th32DefaultHeapID: "uintptr_t",
  th32ModuleID: "uint32",
  cntThreads: "uint32",
  th32ParentProcessID: "uint32",
  pcPriClassBase: "int32",
  dwFlags: "uint32",
  szExeFile: koffi.array("char16_t", 260, "String"),
});

const MODULEENTRY32W = koffi.struct("MODULEENTRY32W", {
  dwSize: "uint32",
  th32ModuleID: "uint32",
  th32ProcessID: "uint32",
  GlblcntUsage: "uint32",
  ProccntUsage: "uint32",
  modBaseAddr: "uintptr_t",
  modBaseSize: "uint32",
  hModule: "uintptr_t",
  szModule: koffi.array("char16_t", 256, "String"),
  szExePath: koffi.array("char16_t", 260, "String"),
});

const MEMORY_BASIC_INFORMATION = koffi.struct("MEMORY_BASIC_INFORMATION", {
  BaseAddress: "uintptr_t",
  AllocationBase: "uintptr_t",
  AllocationProtect: "uint32",
  PartitionId: "uint16",
  RegionSize: "uintptr_t",
  State: "uint32",
  Protect: "uint32",
  Type: "uint32",
});

const CreateToolhelp32Snapshot = kernel32.func("uintptr_t __stdcall CreateToolhelp32Snapshot(uint32 dwFlags, uint32 th32ProcessID)");
const Process32First = kernel32.func("bool __stdcall Process32FirstW(uintptr_t hSnapshot, _Inout_ PROCESSENTRY32W *lppe)");
const Process32Next = kernel32.func("bool __stdcall Process32NextW(uintptr_t hSnapshot, _Inout_ PROCESSENTRY32W *lppe)");
const Module32First = kernel32.func("bool __stdcall Module32FirstW(uintptr_t hSnapshot, _Inout_ MODULEENTRY32W *lpme)");
const OpenProcess = kernel32.func("uintptr_t __stdcall OpenProcess(uint32 dwDesiredAccess, bool bInheritHandle, uint32 dwProcessId)");
const CloseHandle = kernel32.func("bool __stdcall CloseHandle(uintptr_t hObject)");
const VirtualQueryEx = kernel32.func("uintptr_t __stdcall VirtualQueryEx(uintptr_t hProcess, uintptr_t lpAddress, _Out_ MEMORY_BASIC_INFORMATION *lpBuffer, uintptr_t dwLength)");
const ReadProcessMemory = kernel32.func("bool __stdcall ReadProcessMemory(uintptr_t hProcess, uintptr_t lpBaseAddress, _Out_ void *lpBuffer, uintptr_t nSize, _Out_ uintptr_t *lpNumberOfBytesRead)");
// end

const PROCESS_QUERY_INFORMATION = 0x0400;
const PROCESS_VM_READ = 0x0010;
const TH32CS_SNAPPROCESS = 0x2;
const TH32CS_SNAPMODULE = 0x8;
const TH32CS_SNAPMODULE32 = 0x10;
const MEM_COMMIT = 0x1000;
const PAGE_NOACCESS = 0x01;
const PAGE_GUARD = 0x100;
const CHUNK = 1 << 20;
const INVALID_HANDLE = BigInt.asUintN(64, -1n);
class ProcessEntry {
  constructor() {
    this.dwSize = 0; //dword
    this.cntUsage = 0; //dword
    this.th32ProcessID = 0; //dword
    this.th32DefaultHeapID = 0; //c_size_t
    this.th32ModuleID = 0; //dword
    this.cntThreads = 0; //dword
    this.th32ParentProcessID = 0; //dword
    this.pcPriClassBase = 0; //long
    this.dwFlags = 0; //dword
    this.szExeFile = ""; //ctypes.c_wchat*260
  }
}
class ModuleEntry {
  constructor() {
    this.dwSize = 0; //dword
    this.th32ModuleID = 0; //dword
    this.th32ProcessID = 0; //dword
    this.GlblcntUsage = 0; //dword
    this.ProccntUsage = 0; //dword
    this.modBaseAddr = 0; //c_void_p
    this.modBaseSize = 0; //dword
    this.hModule = 0; //c_void_p
    this.szModule = ""; //ctypes.c_wchat*256
    this.szExePath = ""; //ctypes.c_wchat*260
  }
}
class MemoryInfo {
  constructor() {
    this.BaseAddress = 0; //c_void_p
    this.AllocationBase = 0; //c_void_p
    this.AllocationProtect = 0; //dword
    this.PartitionId = 0; //word
    this.RegionSize = 0; //c_size_t
    this.State = 0; //dword
    this.Protect = 0; //dword
    this.Type = 0; //dword
  }
}

async function main() {
  const name = "Minecraft.Windows.exe";
  const output = process.argv[2] || "Minecraft.Windows.dump";

  let snapshot = BigInt(CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0));
  if (snapshot === INVALID_HANDLE) {
    throw new Error("CreateToolhelp32Snapshot failed");
  }
  const pe = { dwSize: koffi.sizeof(PROCESSENTRY32W) };
  let pid = 0;
  if (Process32First(snapshot, pe)) {
    do {
      if (pe.szExeFile.toLowerCase() === name.toLowerCase()) {
        pid = pe.th32ProcessID;
        break;
      }
    } while (Process32Next(snapshot, pe));
  }
  CloseHandle(snapshot);
  if (pid === 0) {
    console.error(`${name} is not running`);
    return;
  }

  snapshot = BigInt(
    CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid),
  );
  if (snapshot === INVALID_HANDLE) {
    console.error(
      `the modules of process ${pid} cannot be listed, run this as administrator`,
    );
    return;
  }
  const me = { dwSize: koffi.sizeof(MODULEENTRY32W) };
  const found = Module32First(snapshot, me);
  CloseHandle(snapshot);
  if (!found) {
    console.error(
      `the main module of process ${pid} cannot be read, run this as administrator`,
    );
    return;
  }
  const base = BigInt(me.modBaseAddr);
  const size = BigInt(me.modBaseSize);

  const hProc = BigInt(
    OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, false, pid),
  );
  if (hProc === 0n) {
    console.error(`process ${pid} cannot be opened, run this as administrator`);
    return;
  }

  const image = new Uint8Array(Number(size));
  const limit = base + size;
  const mbiSize = koffi.sizeof(MEMORY_BASIC_INFORMATION);
  let address = base;
  let unreadable = 0;
  while (address < limit) {
    const info = {};
    if (BigInt(VirtualQueryEx(hProc, address, info, mbiSize)) === 0n) {
      break;
    }
    const regionEnd = BigInt(info.BaseAddress) + BigInt(info.RegionSize);
    const end = regionEnd < limit ? regionEnd : limit;
    const readable =
      info.State === MEM_COMMIT &&
      !(info.Protect & (PAGE_NOACCESS | PAGE_GUARD));
    if (readable) {
      let position = address;
      while (position < end) {
        const remain = end - position;
        const length = Number(remain < BigInt(CHUNK) ? remain : BigInt(CHUNK));
        const buffer = Buffer.alloc(length);
        const ok = ReadProcessMemory(hProc, position, buffer, length, [0]);
        if (ok) {
          image.set(buffer, Number(position - base));
        } else {
          unreadable += length;
        }
        position += BigInt(length);
      }
    } else {
      unreadable += Number(end - address);
    }
    address = end;
  }
  CloseHandle(hProc);

  writeFile(output, image, (err) => {
    if (err) {
      console.error(err);
      return;
    }
    console.log(
      `wrote ${size} bytes of ${name} (pid ${pid}, base 0x${base.toString(
        16,
      )}) to ${output}, ${unreadable} bytes were not readable`,
    );
  });
}

main().catch((error) => {
  console.error(error);
  process.exit(1);
});
