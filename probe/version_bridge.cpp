// version.dll 代理：进程启动即加载（loader 载入，非远程注入），在 Unity 初始化输入之前
// 安装 (a) GetSystemMetrics 数字化仪伪装 (b) WM_POINTER -> WM_TOUCH 桥
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "MinHook.h"

#define MAX_CONTACTS 10
struct Contact { UINT32 id; int x, y, state; };

static Contact g_contacts[MAX_CONTACTS];
static TOUCHINPUT g_ti_buf[8][MAX_CONTACTS];
static volatile LONG g_ti_slot, g_rtw_n, g_gti_n, g_touch_n;
static volatile LONG g_peekw_n, g_peeka_n, g_getw_n, g_geta_n, g_ptr_n, g_emip_n;

typedef BOOL (__attribute__((ms_abi)) *PeekMsgFn)(LPMSG, HWND, UINT, UINT, UINT);
typedef BOOL (__attribute__((ms_abi)) *GetMsgFn)(LPMSG, HWND, UINT, UINT);
typedef BOOL (__attribute__((ms_abi)) *RTWFn)(HWND, ULONG);
typedef BOOL (__attribute__((ms_abi)) *GTIFn)(HTOUCHINPUT, UINT, PTOUCHINPUT, int);
typedef BOOL (__attribute__((ms_abi)) *CTIHFn)(HTOUCHINPUT);
typedef int  (__attribute__((ms_abi)) *GsmFn)(int);
static PeekMsgFn o_PeekW, o_PeekA;
static GetMsgFn o_GetW, o_GetA;
static RTWFn o_RTW; static GTIFn o_GTI; static CTIHFn o_CTIH; static GsmFn o_GSM;

static HANDLE g_log = INVALID_HANDLE_VALUE;
static void lg(const char* fmt, ...)
{
    char b[512]; va_list ap; va_start(ap, fmt);
    int n = _vsnprintf(b, sizeof(b) - 1, fmt, ap); va_end(ap);
    if (n <= 0) return; b[n] = 0;
    char line[600];
    int m = _snprintf(line, sizeof(line) - 1, "[%6lu] %s\n", (unsigned long)GetTickCount(), b);
    if (m > 0 && g_log != INVALID_HANDLE_VALUE) { DWORD w; WriteFile(g_log, line, (DWORD)m, &w, NULL); }
}

static void contacts_update(UINT32 id, int x, int y, int down, int up)
{
    for (int i = 0; i < MAX_CONTACTS; i++)
        if (g_contacts[i].state && g_contacts[i].id == id) {
            g_contacts[i].x = x; g_contacts[i].y = y; g_contacts[i].state = up ? 3 : 2; return;
        }
    if (down)
        for (int i = 0; i < MAX_CONTACTS; i++)
            if (!g_contacts[i].state) { g_contacts[i].id = id; g_contacts[i].x = x; g_contacts[i].y = y; g_contacts[i].state = 1; return; }
}

static int translate(LPMSG msg)
{
    UINT m = msg->message;
    if (m < 0x0245 || m > 0x0249) return 0;
    UINT32 id = GET_POINTERID_WPARAM(msg->wParam);
    POINTER_INFO pi;
    if (!GetPointerInfo(id, &pi)) return 0;
    int x = pi.ptPixelLocation.x, y = pi.ptPixelLocation.y;
    if (m == 0x0246) contacts_update(id, x, y, 1, 0);
    else if (m == 0x0247) contacts_update(id, x, y, 0, 1);
    else if (m == 0x0245) contacts_update(id, x, y, 0, 0);
    else return 0;

    if (InterlockedIncrement(&g_ptr_n) <= 40)
        lg("PTR msg=%x id=%u type=%d at %d,%d flags=%x", m, id, (int)pi.pointerType, x, y, pi.pointerFlags);
    LONG slot = InterlockedIncrement(&g_ti_slot) % 8;
    TOUCHINPUT* buf = g_ti_buf[slot];
    UINT n = 0;
    for (int i = 0; i < MAX_CONTACTS; i++) {
        Contact* c = &g_contacts[i];
        if (!c->state) continue;
        TOUCHINPUT* t = &buf[n++];
        t->dwID = (DWORD)c->id;
        t->x = c->x * 100; t->y = c->y * 100;
        t->dwFlags = (c->state == 1 ? TOUCHEVENTF_DOWN : (c->state == 3 ? TOUCHEVENTF_UP : TOUCHEVENTF_MOVE));
        if (n == 1) t->dwFlags |= TOUCHEVENTF_PRIMARY;
        t->dwMask = TOUCHINPUTMASKF_CONTACTAREA;
        t->cxContact = 8; t->cyContact = 8;
    }
    if (!n) return 0;
    if (InterlockedIncrement(&g_touch_n) <= 15) lg("WM_TOUCH n=%u (from %x)", n, m);
    msg->message = WM_TOUCH; msg->wParam = (WPARAM)n; msg->lParam = (LPARAM)(HTOUCHINPUT)buf;
    return 1;
}

static BOOL __attribute__((ms_abi)) hkPeekW(LPMSG m, HWND h, UINT a, UINT b, UINT r)
{ InterlockedIncrement(&g_peekw_n); BOOL ok = o_PeekW ? o_PeekW(m, h, a, b, r) : 0; if (ok) translate(m); return ok; }
static BOOL __attribute__((ms_abi)) hkPeekA(LPMSG m, HWND h, UINT a, UINT b, UINT r)
{ InterlockedIncrement(&g_peeka_n); BOOL ok = o_PeekA ? o_PeekA(m, h, a, b, r) : 0; if (ok) translate(m); return ok; }
static BOOL __attribute__((ms_abi)) hkGetW(LPMSG m, HWND h, UINT a, UINT b)
{ InterlockedIncrement(&g_getw_n); BOOL ok = o_GetW ? o_GetW(m, h, a, b) : 0; if (ok > 0) translate(m); return ok; }
static BOOL __attribute__((ms_abi)) hkGetA(LPMSG m, HWND h, UINT a, UINT b)
{ InterlockedIncrement(&g_geta_n); BOOL ok = o_GetA ? o_GetA(m, h, a, b) : 0; if (ok > 0) translate(m); return ok; }
static BOOL __attribute__((ms_abi)) hkRTW(HWND h, ULONG f)
{ LONG n = InterlockedIncrement(&g_rtw_n); if (n <= 5) lg("RegisterTouchWindow(%p,%lx) -> TRUE", h, (unsigned long)f); if (o_RTW) o_RTW(h, f); return TRUE; }
static BOOL __attribute__((ms_abi)) hkGTI(HTOUCHINPUT h, UINT c, PTOUCHINPUT o, int s)
{ TOUCHINPUT* src = (TOUCHINPUT*)(void*)h; if (!src) return FALSE; if (c > MAX_CONTACTS) c = MAX_CONTACTS;
  for (UINT i = 0; i < c; i++) if (o) o[i] = src[i];
  if (InterlockedIncrement(&g_gti_n) <= 15) lg("GetTouchInputInfo(count=%u)", c); return TRUE; }
static BOOL __attribute__((ms_abi)) hkCTIH(HTOUCHINPUT h) { (void)h; return TRUE; }
static int __attribute__((ms_abi)) hkGSM(int i)
{
    if (i == 94) return 0xC3;
    if (i == 95) return 10;
    if (i == 86) return 1;
    return o_GSM ? o_GSM(i) : 0;
}

static DWORD WINAPI reporter(LPVOID u)
{
    (void)u;
    LONG lp = -1, lpw = -1, lga = -1, lptr = -1, ltouch = -1;
    for (;;) {
        Sleep(5000);
        LONG pw = InterlockedCompareExchange(&g_peekw_n, 0, 0), pa = InterlockedCompareExchange(&g_peeka_n, 0, 0);
        LONG gw = InterlockedCompareExchange(&g_getw_n, 0, 0), ga = InterlockedCompareExchange(&g_geta_n, 0, 0);
        LONG ptr = InterlockedCompareExchange(&g_ptr_n, 0, 0), tc = InterlockedCompareExchange(&g_touch_n, 0, 0);
        LONG gti = InterlockedCompareExchange(&g_gti_n, 0, 0), emip = InterlockedCompareExchange(&g_emip_n, 0, 0);
        if (pw != lpw || pa != lga || gw != lga || ga != lga || ptr != lptr || tc != ltouch) {
            lg("STAT peekW=%ld peekA=%ld getW=%ld getA=%ld ptr=%ld touch=%ld gti=%ld emip=%ld",
               (long)pw, (long)pa, (long)gw, (long)ga, (long)ptr, (long)tc, (long)gti, (long)emip);
            lpw = pw; lga = pa; lptr = ptr; ltouch = tc;
        }
    }
    return 0;
}

typedef BOOL (__attribute__((ms_abi)) *EMIPFn)(BOOL);
static EMIPFn o_EMIP;
static BOOL __attribute__((ms_abi)) hkEMIP(BOOL on)
{
    InterlockedIncrement(&g_emip_n);
    lg("EnableMouseInPointer(%d)", (int)on);
    return o_EMIP ? o_EMIP(on) : FALSE;
}

static void install(void)
{
    HMODULE u32 = LoadLibraryW(L"user32.dll");
    if (MH_Initialize() != MH_OK) { lg("MinHook init failed"); return; }
    struct { const char* n; void* d; void** o; } t[] = {
        {"PeekMessageW", (void*)&hkPeekW, (void**)&o_PeekW},
        {"PeekMessageA", (void*)&hkPeekA, (void**)&o_PeekA},
        {"GetMessageW", (void*)&hkGetW, (void**)&o_GetW},
        {"GetMessageA", (void*)&hkGetA, (void**)&o_GetA},
        {"RegisterTouchWindow", (void*)&hkRTW, (void**)&o_RTW},
        {"GetTouchInputInfo", (void*)&hkGTI, (void**)&o_GTI},
        {"CloseTouchInputHandle", (void*)&hkCTIH, (void**)&o_CTIH},
        {"GetSystemMetrics", (void*)&hkGSM, (void**)&o_GSM},
    };
    int ok = 0;
    for (unsigned i = 0; i < sizeof(t)/sizeof(t[0]); i++) {
        void* p = u32 ? (void*)GetProcAddress(u32, t[i].n) : NULL;
        if (p && MH_CreateHook(p, t[i].d, t[i].o) == MH_OK && MH_EnableHook(p) == MH_OK) ok++;
        else lg("hook failed: %s (%p)", t[i].n, p);
    }
    lg("bridge installed %d/8", ok);
}

static DWORD WINAPI worker(LPVOID u)
{
    (void)u;
    Sleep(150);
    install();
    HANDLE r = CreateThread(NULL, 0, reporter, NULL, 0, NULL);
    if (r) CloseHandle(r);
    return 0;
}

// ---- version.dll 导出转发 ----
static HMODULE real_ver(void) { static HMODULE h; if (!h) h = LoadLibraryA("C:\\windows\\system32\\version.dll"); return h; }

extern "C" {
DWORD WINAPI GetFileVersionInfoSizeA(LPCSTR a, LPDWORD b)
{ typedef DWORD (WINAPI*F)(LPCSTR, LPDWORD); static F f; if (!f) f = (F)GetProcAddress(real_ver(), "GetFileVersionInfoSizeA"); return f ? f(a, b) : 0; }
DWORD WINAPI GetFileVersionInfoSizeW(LPCWSTR a, LPDWORD b)
{ typedef DWORD (WINAPI*F)(LPCWSTR, LPDWORD); static F f; if (!f) f = (F)GetProcAddress(real_ver(), "GetFileVersionInfoSizeW"); return f ? f(a, b) : 0; }
BOOL WINAPI GetFileVersionInfoA(LPCSTR a, DWORD b, DWORD c, LPVOID d)
{ typedef BOOL (WINAPI*F)(LPCSTR, DWORD, DWORD, LPVOID); static F f; if (!f) f = (F)GetProcAddress(real_ver(), "GetFileVersionInfoA"); return f ? f(a, b, c, d) : 0; }
BOOL WINAPI GetFileVersionInfoW(LPCWSTR a, DWORD b, DWORD c, LPVOID d)
{ typedef BOOL (WINAPI*F)(LPCWSTR, DWORD, DWORD, LPVOID); static F f; if (!f) f = (F)GetProcAddress(real_ver(), "GetFileVersionInfoW"); return f ? f(a, b, c, d) : 0; }
BOOL WINAPI VerQueryValueA(LPCVOID a, LPCSTR b, LPVOID* c, PUINT d)
{ typedef BOOL (WINAPI*F)(LPCVOID, LPCSTR, LPVOID*, PUINT); static F f; if (!f) f = (F)GetProcAddress(real_ver(), "VerQueryValueA"); return f ? f(a, b, c, d) : 0; }
BOOL WINAPI VerQueryValueW(LPCVOID a, LPCWSTR b, LPVOID* c, PUINT d)
{ typedef BOOL (WINAPI*F)(LPCVOID, LPCWSTR, LPVOID*, PUINT); static F f; if (!f) f = (F)GetProcAddress(real_ver(), "VerQueryValueW"); return f ? f(a, b, c, d) : 0; }
}

BOOL WINAPI DllMain(HINSTANCE h, DWORD reason, LPVOID r)
{
    (void)r;
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(h);
        g_log = CreateFileW(L"Z:\\home\\h\\.ys\\mobui-bridge.log", FILE_APPEND_DATA,
                            FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        lg("=== version.dll bridge attached (pid %lu) ===", GetCurrentProcessId());
        HANDLE t = CreateThread(NULL, 0, worker, NULL, 0, NULL);
        if (t) CloseHandle(t);
    }
    return TRUE;
}
