// 触摸注入测试工具：mtouch.exe <x> <y> [hold_ms] [x2 y2]
// 走 Wine 实现的 InitializeTouchInjection/InjectTouchInput（同 wine session 内即可，不必注入游戏）
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

static FILE* g_log;
static void lg(const char* fmt, ...) {
    va_list ap; va_start(ap, fmt);
    if (g_log) { vfprintf(g_log, fmt, ap); fflush(g_log); }
    va_end(ap);
}

typedef BOOL (WINAPI *InitFn)(DWORD, DWORD);
typedef BOOL (WINAPI *InjectFn)(DWORD, const POINTER_TOUCH_INFO*);

int main(int argc, char** argv)
{
    g_log = fopen("mtouch.log", "a");
    if (g_log) lg("=== mtouch start ===\n");
    if (argc < 3) { lg("usage: mtouch.exe <x> <y> [hold_ms] [x2 y2]\n"); return 2; }
    int x = atoi(argv[1]), y = atoi(argv[2]);
    int hold = (argc > 3) ? atoi(argv[3]) : 120;
    int x2 = (argc > 5) ? atoi(argv[4]) : -1;
    int y2 = (argc > 5) ? atoi(argv[5]) : -1;

    HMODULE u32 = LoadLibraryW(L"user32.dll");
    lg("user32 handle=%p\n", (void*)u32);
    InitFn init = (InitFn)(void*)GetProcAddress(u32, "InitializeTouchInjection");
    InjectFn inject = (InjectFn)(void*)GetProcAddress(u32, "InjectTouchInput");
    HMODULE w32u = LoadLibraryW(L"win32u.dll");
    if (!inject && w32u) inject = (InjectFn)(void*)GetProcAddress(w32u, "NtUserInjectTouchInput");
    lg("win32u=%p NtUserInjectTouchInput=%p\n", (void*)w32u, (void*)inject);
    lg("InitializeTouchInjection=%p InjectTouchInput=%p\n", (void*)init, (void*)inject);
    if (!init || !inject) {
        lg("FAIL: touch API not exported init=%p inject=%p\n", (void*)init, (void*)inject);
        return 3;
    }
    if (!init(10, 0 /*TOUCH_FEEDBACK_NONE*/)) { lg("FAIL: InitializeTouchInjection err=%lu\n", GetLastError()); return 4; }

    POINTER_TOUCH_INFO t[2];
    memset(t, 0, sizeof(t));
    for (int i = 0; i < 2; i++) {
        t[i].pointerInfo.pointerType = 2 /*PT_TOUCH*/;
        t[i].pointerInfo.pointerId = (UINT32)(i + 1);
        t[i].touchFlags = TOUCH_FLAG_NONE;
        t[i].touchMask = TOUCH_MASK_CONTACTAREA | TOUCH_MASK_ORIENTATION | TOUCH_MASK_PRESSURE;
        t[i].orientation = 90;
        t[i].pressure = 32000;
        t[i].rcContact.left = 0; t[i].rcContact.right = 2; t[i].rcContact.top = 0; t[i].rcContact.bottom = 2;
    }
    int n = (x2 >= 0) ? 2 : 1;
    t[0].pointerInfo.ptPixelLocation.x = x;  t[0].pointerInfo.ptPixelLocation.y = y;
    t[1].pointerInfo.ptPixelLocation.x = x2; t[1].pointerInfo.ptPixelLocation.y = y2;
    for (int i = 0; i < n; i++) {
        t[i].pointerInfo.pointerFlags = POINTER_FLAG_DOWN | POINTER_FLAG_INRANGE | POINTER_FLAG_INCONTACT;
    }
    BOOL ok1 = inject(n, t);
    lg("inject down(%d pts @ %d,%d %d,%d) -> %d err=%lu\n", n, x, y, x2, y2, ok1, GetLastError());
    Sleep(30);
    for (int i = 0; i < n; i++) {
        t[i].pointerInfo.pointerFlags = POINTER_FLAG_UPDATE | POINTER_FLAG_INRANGE | POINTER_FLAG_INCONTACT;
        t[i].pointerInfo.ptPixelLocation.x += 4;
    }
    BOOL ok2 = inject(n, t);
    lg("inject update -> %d err=%lu\n", ok2, GetLastError());
    Sleep((DWORD)(hold > 0 ? hold : 0));
    for (int i = 0; i < n; i++) t[i].pointerInfo.pointerFlags = POINTER_FLAG_UP;
    BOOL ok3 = inject(n, t);
    lg("inject up -> %d err=%lu\n", ok3, GetLastError());
    return 0;
}
