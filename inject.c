// 通用 DLL 注入器（Wine 友好）
// 用法：
//   inject.exe --pid <windows-pid> <dll-path>
//   inject.exe --name <exe-name> <dll-path>      # 按进程名找 Windows PID（Wine 下 Linux PID != Windows PID）
//   inject.exe --list                            # 列出所有进程（Windows PID + 名称）
// 走 OpenProcess + VirtualAllocEx + CreateRemoteThread(LoadLibraryW)。
#include <windows.h>
#include <tlhelp32.h>
#include <stdio.h>
#include <wchar.h>

static int ci_equal(const wchar_t* a, const wchar_t* b)
{
    return _wcsicmp(a, b) == 0;
}

static DWORD find_by_name(const wchar_t* name)
{
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) {
        wprintf(L"snapshot failed: %lu\n", GetLastError());
        return 0;
    }
    PROCESSENTRY32W pe;
    pe.dwSize = sizeof(pe);
    DWORD found = 0;
    if (Process32FirstW(snap, &pe)) {
        do {
            if (ci_equal(pe.szExeFile, name)) {
                DWORD pid = pe.th32ProcessID;
                if (pid != GetCurrentProcessId()) {
                    found = pid;
                    wprintf(L"match: %ls pid=%lu\n", pe.szExeFile, (unsigned long)pid);
                    break;
                }
            }
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    return found;
}

static void list_procs(void)
{
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE)
        return;
    PROCESSENTRY32W pe;
    pe.dwSize = sizeof(pe);
    if (Process32FirstW(snap, &pe)) {
        do {
            wprintf(L"%6lu  %ls\n", (unsigned long)pe.th32ProcessID, pe.szExeFile);
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
}

static int inject(DWORD pid, const wchar_t* dll)
{
    DWORD attr = GetFileAttributesW(dll);
    if (attr == INVALID_FILE_ATTRIBUTES || (attr & FILE_ATTRIBUTE_DIRECTORY)) {
        wprintf(L"dll not found: %ls\n", dll);
        return 4;
    }
    HANDLE h = OpenProcess(PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION |
                           PROCESS_VM_OPERATION | PROCESS_VM_WRITE | PROCESS_VM_READ,
                           FALSE, pid);
    if (!h) {
        wprintf(L"OpenProcess(%lu) failed: %lu\n", (unsigned long)pid, GetLastError());
        return 3;
    }
    size_t bytes = (wcslen(dll) + 1) * sizeof(wchar_t);
    void* remote = VirtualAllocEx(h, NULL, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!remote) {
        wprintf(L"VirtualAllocEx failed: %lu\n", GetLastError());
        CloseHandle(h);
        return 5;
    }
    if (!WriteProcessMemory(h, remote, dll, bytes, NULL)) {
        wprintf(L"WriteProcessMemory failed: %lu\n", GetLastError());
        VirtualFreeEx(h, remote, 0, MEM_RELEASE);
        CloseHandle(h);
        return 6;
    }
    FARPROC loadlib = GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "LoadLibraryW");
    HANDLE t = CreateRemoteThread(h, NULL, 0, (LPTHREAD_START_ROUTINE)loadlib, remote, 0, NULL);
    if (!t) {
        wprintf(L"CreateRemoteThread failed: %lu\n", GetLastError());
        VirtualFreeEx(h, remote, 0, MEM_RELEASE);
        CloseHandle(h);
        return 7;
    }
    DWORD wait = WaitForSingleObject(t, 20000);
    DWORD code = 0;
    GetExitCodeThread(t, &code);
    wprintf(L"wait=%lu exit=0x%lx\n", (unsigned long)wait, (unsigned long)code);
    CloseHandle(t);
    if (wait != WAIT_OBJECT_0)
        VirtualFreeEx(h, remote, 0, MEM_RELEASE);
    CloseHandle(h);
    return (wait == WAIT_OBJECT_0 && code != 0) ? 0 : 8;
}

int wmain(int argc, wchar_t** argv)
{
    if (argc >= 2 && ci_equal(argv[1], L"--list")) {
        list_procs();
        return 0;
    }
    if (argc >= 4 && ci_equal(argv[1], L"--pid")) {
        DWORD pid = (DWORD)wcstoul(argv[2], NULL, 10);
        return inject(pid, argv[3]);
    }
    if (argc >= 4 && ci_equal(argv[1], L"--wait")) {
        /* 后台等目标进程出现，一出现立即注入（避免每次 wine 启动开销） */
        for (int round = 0; round < 6000; round++) {
            DWORD pid = find_by_name(argv[2]);
            if (pid) {
                wprintf(L"[wait] target appeared pid=%lu, injecting\n", (unsigned long)pid);
                int rc = inject(pid, argv[3]);
                wprintf(L"[wait] inject rc=%d\n", rc);
                return rc;
            }
            Sleep(20);
        }
        wprintf(L"[wait] timeout waiting for %ls\n", argv[2]);
        return 9;
    }
    if (argc >= 4 && ci_equal(argv[1], L"--name")) {
        DWORD pid = find_by_name(argv[2]);
        if (!pid) {
            wprintf(L"process not found: %ls\n", argv[2]);
            return 9;
        }
        return inject(pid, argv[3]);
    }
    wprintf(L"usage: inject.exe --pid <pid> <dll> | --name <exe> <dll> | --list\n");
    return 2;
}
