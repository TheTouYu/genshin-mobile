// msgspy.dll —— 只数消息泵：PeekMessageW/A、GetMessageW/A 调用数 + 指针消息明细
#include <windows.h>
#include <stdio.h>
#include <stdarg.h>
#include "MinHook.h"

typedef BOOL (__attribute__((ms_abi)) *PFn)(LPMSG, HWND, UINT, UINT, UINT);
typedef BOOL (__attribute__((ms_abi)) *GFn)(LPMSG, HWND, UINT, UINT);
static PFn oPeekW, oPeekA; static GFn oGetW, oGetA;
static volatile LONG nPeekW, nPeekA, nGetW, nGetA, nPtr, nMsgs;
static HANDLE g_log;

static void lg(const char* fmt, ...)
{
    if (g_log == INVALID_HANDLE_VALUE) return;
    char b[400]; va_list ap; va_start(ap, fmt); int n = _vsnprintf(b, sizeof(b)-1, fmt, ap); va_end(ap);
    if (n <= 0) return; b[n] = 0;
    char line[460]; int m = _snprintf(line, sizeof(line)-1, "[%6lu] %s\n", (unsigned long)GetTickCount(), b);
    if (m > 0) { DWORD w; WriteFile(g_log, line, (DWORD)m, &w, NULL); }
}
static void note(LPMSG m)
{
    InterlockedIncrement(&nMsgs);
    if (m->message >= 0x0240 && m->message <= 0x0249 && InterlockedIncrement(&nPtr) <= 30)
        lg("PTRMSG %x hwnd=%p wparam=%llx", m->message, (void*)m->hwnd, (unsigned long long)m->wParam);
}
static BOOL __attribute__((ms_abi)) pW(LPMSG m, HWND h, UINT a, UINT b, UINT r)
{ InterlockedIncrement(&nPeekW); BOOL o = oPeekW ? oPeekW(m,h,a,b,r) : 0; if (o) note(m); return o; }
static BOOL __attribute__((ms_abi)) pA(LPMSG m, HWND h, UINT a, UINT b, UINT r)
{ InterlockedIncrement(&nPeekA); BOOL o = oPeekA ? oPeekA(m,h,a,b,r) : 0; if (o) note(m); return o; }
static BOOL __attribute__((ms_abi)) gW(LPMSG m, HWND h, UINT a, UINT b)
{ InterlockedIncrement(&nGetW); BOOL o = oGetW ? oGetW(m,h,a,b) : 0; if (o > 0) note(m); return o; }
static BOOL __attribute__((ms_abi)) gA(LPMSG m, HWND h, UINT a, UINT b)
{ InterlockedIncrement(&nGetA); BOOL o = oGetA ? oGetA(m,h,a,b) : 0; if (o > 0) note(m); return o; }

static DWORD WINAPI rep(LPVOID u)
{
    (void)u;
    for (;;) {
        Sleep(3000);
        lg("STAT peekW=%ld peekA=%ld getW=%ld getA=%ld msgs=%ld ptr=%ld",
           (long)nPeekW, (long)nPeekA, (long)nGetW, (long)nGetA, (long)nMsgs, (long)nPtr);
    }
    return 0;
}

static BOOL WINAPI DllMain(HINSTANCE h, DWORD r, LPVOID x)
{
    (void)x;
    if (r != DLL_PROCESS_ATTACH) return TRUE;
    DisableThreadLibraryCalls(h);
    wchar_t path[MAX_PATH] = {0};
    DWORD nl = GetModuleFileNameW(h, path, MAX_PATH);
    if (nl > 4 && nl < MAX_PATH) {
        wcscpy(path + nl - 4, L".log");
        g_log = CreateFileW(path, FILE_APPEND_DATA, FILE_SHARE_READ|FILE_SHARE_WRITE,
                            NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    }
    lg("=== msgspy attached pid=%lu ===", GetCurrentProcessId());
    if (MH_Initialize() != MH_OK) { lg("minhook init failed"); return TRUE; }
    HMODULE u32 = LoadLibraryW(L"user32.dll");
    struct { const char* n; void* d; void** o; } t[] = {
        {"PeekMessageW", (void*)&pW, (void**)&oPeekW},
        {"PeekMessageA", (void*)&pA, (void**)&oPeekA},
        {"GetMessageW", (void*)&gW, (void**)&oGetW},
        {"GetMessageA", (void*)&gA, (void**)&oGetA},
    };
    int ok = 0;
    for (unsigned i = 0; i < 4; i++) {
        void* p = u32 ? (void*)GetProcAddress(u32, t[i].n) : NULL;
        if (p && MH_CreateHook(p, t[i].d, t[i].o) == MH_OK && MH_EnableHook(p) == MH_OK) ok++;
        else lg("hook failed %s (%p)", t[i].n, p);
    }
    lg("msgspy installed %d/4", ok);
    HANDLE th = CreateThread(NULL, 0, rep, NULL, 0, NULL);
    if (th) CloseHandle(th);
    return TRUE;
}
