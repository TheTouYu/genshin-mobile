// wintouch.exe —— Wine 侧触摸投递探针：Win32 窗口 + WM_TOUCH + WM_POINTER，全部记日志
#include <windows.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>

static FILE* g;
static void lg(const char* fmt, ...) { va_list ap; va_start(ap, fmt); if (g) { vfprintf(g, fmt, ap); fflush(g); } va_end(ap); }

typedef BOOL (WINAPI *EnableMouseInPointerFn)(BOOL);
typedef BOOL (WINAPI *GetPointerTypeFn)(UINT32, int*);
typedef BOOL (WINAPI *GetPointerTouchInfoFn)(UINT32, void*);

static int g_touch_msgs, g_pointer_msgs;
static UINT g_max_contacts;

static LRESULT CALLBACK wndproc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_TOUCH: {
        g_touch_msgs++;
        UINT n = LOWORD(wp);
        TOUCHINPUT ti[10];
        if (GetTouchInputInfo((HTOUCHINPUT)lp, n, ti, sizeof(TOUCHINPUT))) {
            lg("WM_TOUCH n=%u msgs=%d\n", n, g_touch_msgs);
            for (UINT i = 0; i < n; i++)
                lg("   touch id=%d x=%d y=%d flags=%x\n", (int)ti[i].dwID, (int)TOUCH_COORD_TO_PIXEL(ti[i].x),
                   (int)TOUCH_COORD_TO_PIXEL(ti[i].y), (unsigned)ti[i].dwFlags);
            CloseTouchInputHandle((HTOUCHINPUT)lp);
        } else lg("WM_TOUCH n=%u but GetTouchInputInfo failed (err=%lu)\n", n, GetLastError());
        return 0;
    }
    case WM_POINTERDOWN: case WM_POINTERUPDATE: case WM_POINTERUP: {
        g_pointer_msgs++;
        UINT32 id = GET_POINTERID_WPARAM(wp);
        if (msg != WM_POINTERUPDATE || (g_pointer_msgs % 100) == 0 || id != 557) {
            POINTER_INFO pi; memset(&pi, 0, sizeof(pi));
            BOOL ok = GetPointerInfo(id, &pi);
            lg("WM_POINTER msg=%x id=%u type=%d ok=%d at %ld,%ld flags=%x msgs=%d\n",
               msg, id, ok ? pi.pointerType : -1, ok, ok ? (long)pi.ptPixelLocation.x : -1,
               ok ? (long)pi.ptPixelLocation.y : -1, ok ? pi.pointerFlags : 0,
               g_pointer_msgs);
        }
        return 0;
    }
    case WM_POINTERENTER: case WM_POINTERLEAVE:
    case WM_POINTERCAPTURECHANGED: return 0;
    case WM_DESTROY: PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE prev, PWSTR cmd, int show)
{
    (void)prev; (void)cmd; (void)show;
    int secs = 10;
    g = fopen("wintouch.log", "a");
    lg("=== wintouch start (pid %lu) ===\n", GetCurrentProcessId());

    WNDCLASSW wc; memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc = wndproc;
    wc.hInstance = inst;
    wc.lpszClassName = L"wintouchwnd";
    ATOM at = RegisterClassW(&wc);
    lg("RegisterClassW -> %u err=%lu\n", (unsigned)at, GetLastError());
    int sw = 1000, sh = 800;
    HWND h = CreateWindowExW(WS_EX_TOPMOST, L"wintouchwnd", L"wintouch", WS_POPUP | WS_VISIBLE,
                             0, 0, sw, sh, NULL, NULL, inst, NULL);
    if (!h) { lg("CreateWindow failed %lu (class atom=%u)\n", GetLastError(), (unsigned)at); return 1; }
    ShowWindow(h, SW_SHOW);
    SetWindowPos(h, HWND_TOPMOST, 100, 100, sw, sh, SWP_SHOWWINDOW);
    lg("window %dx%d topmost\n", sw, sh);

    BOOL reg = RegisterTouchWindow(h, TWF_WANTPALM);
    lg("RegisterTouchWindow -> %d err=%lu\n", reg, GetLastError());

    HMODULE u32 = LoadLibraryW(L"user32.dll");
    EnableMouseInPointerFn emip = (EnableMouseInPointerFn)(void*)GetProcAddress(u32, "EnableMouseInPointer");
    int use_pointer = GetEnvironmentVariableW(L"WTOUCH_POINTER", NULL, 0) > 0;
    lg("EnableMouseInPointer -> %d (use_pointer=%d)\n", (use_pointer && emip) ? emip(TRUE) : -1, use_pointer);

    DWORD t0 = GetTickCount();
    while (GetTickCount() - t0 < (DWORD)secs * 1000) {
        MSG m;
        while (PeekMessageW(&m, NULL, 0, 0, PM_REMOVE)) { TranslateMessage(&m); DispatchMessageW(&m); }
        Sleep(10);
    }
    lg("summary: touch_msgs=%d pointer_msgs=%d\n", g_touch_msgs, g_pointer_msgs);
    DestroyWindow(h);
    return 0;
}
