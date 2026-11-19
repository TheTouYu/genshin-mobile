// touchrecv.exe —— 离线触摸验收台的 Wine 侧接收端。
//
// 建一个可见的 2560x1440 无边框窗口（标题 touchrecv），RegisterTouchWindow(hwnd, 0)；
// 在 WM_TOUCH 处理里【同步】调用 GetTouchInputInfo()，随后立刻 CloseTouchInputHandle()，
// 把每条消息追加写日志（默认 build/touchrecv.log）。
//
// 环境变量：
//   TOUCHRECV_LOG       日志路径（默认 build\touchrecv.log）
//   TOUCHRECV_STOP      收尾哨兵文件；文件出现即退出（验收脚本用它停进程，不 kill）
//   TOUCHRECV_SECS      看门狗秒数（默认 180，0=不限）
//   TOUCHRECV_SELFTEST  1=不发包也能自测：窗口起来后自己 PostMessage 两条假 WM_TOUCH，
//                       用 SELFTOUCH 前缀落日志（只验窗口/消息泵/日志格式；真实验收不吃这种行）
//
// 日志行（验收脚本按这些断言）：
//   START pid=.. tick=.. cwd=..
//   LOG path=..
//   CLASS atom=..
//   WINDOW hwnd=.. title=touchrecv client=2560x1440 style=86000000 borderless=1
//   RTW -> 1 err=..
//   WMSG #1 msg=0000 wp=0 lp=0        （前 20 条 Windows 消息号，判断 WM_TOUCH 是否真到）
//   MSG n=1 [id=1020 f=11 x=1280 y=720 t=12345] wt=12340
//   GTI_FAIL n=.. err=..              （钩子没装/解码失败时的真实读数）
//   SUMMARY touches=.. gti_fail=.. msgs=..
#include <windows.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <stdlib.h>

#define WIN_W 2560
#define WIN_H 1440
#define MAXT  16
#define MSGW  20

static FILE*  g_log;
static WCHAR  g_stop[MAX_PATH * 2];
static DWORD  g_start, g_last_msg_time;
static unsigned g_msgs, g_touches, g_gti_fail, g_ptr_msgs;
static int    g_selftest, g_selftest_done, g_hide;
static DWORD  g_secs = 180;

static void lg(const char* fmt, ...)
{
    if (!g_log) return;
    va_list ap;
    va_start(ap, fmt);
    vfprintf(g_log, fmt, ap);
    va_end(ap);
    fflush(g_log);
}

static void env_str(const WCHAR* name, WCHAR* out, DWORD n, const WCHAR* def)
{
    DWORD got = GetEnvironmentVariableW(name, out, n);
    if (got == 0 || got >= n) {
        wcsncpy(out, def, n - 1);
        out[n - 1] = 0;
    }
}

static DWORD env_num(const WCHAR* name, DWORD def)
{
    WCHAR b[32];
    DWORD got = GetEnvironmentVariableW(name, b, 32);
    if (!got) return def;
    return (DWORD)wcstoul(b, NULL, 10);
}

// 一条 WM_TOUCH 的规范日志行：MSG n=<n> [id= f= x= y= t=]... wt=<TOUCHINPUT.dwTime> mt=<消息队列时间>
//   t  = 本函数里的 GetTickCount（接收端时刻）
//   wt = TOUCHINPUT.dwTime（投递端填的时刻，探针 emit_touch 里 = 它自己的 GetTickCount）
//   mt = 消息队列时间（PostMessage 时刻，Wine 填）
static void log_touch(UINT n, const TOUCHINPUT* ti, DWORD now, DWORD mtime)
{
    g_touches++;
    DWORD ti_time = n ? ti[0].dwTime : 0;
    lg("MSG n=%u", n);
    for (UINT i = 0; i < n; i++) {
        lg(" [id=%u f=%x x=%d y=%d t=%lu]",
           (unsigned)ti[i].dwID, (unsigned)ti[i].dwFlags,
           (int)TOUCH_COORD_TO_PIXEL(ti[i].x), (int)TOUCH_COORD_TO_PIXEL(ti[i].y),
           (unsigned long)now);
    }
    DWORD ti_mask = n ? ti[0].dwMask : 0;
    lg(" wt=%lu mt=%lu mask=%lx\n", (unsigned long)ti_time, (unsigned long)mtime,
       (unsigned long)ti_mask);
}

static TOUCHINPUT g_fake1[1], g_fake2[2];

static void selftest_post(HWND h)
{
    ZeroMemory(g_fake1, sizeof(g_fake1));
    g_fake1[0].dwID = 1000;
    g_fake1[0].x = 100 * 100; g_fake1[0].y = 200 * 100;
    g_fake1[0].dwFlags = TOUCHEVENTF_DOWN | TOUCHEVENTF_PRIMARY;
    g_fake1[0].dwMask = TOUCHINPUTMASKF_CONTACTAREA;
    g_fake1[0].cxContact = 8; g_fake1[0].cyContact = 8;
    PostMessageW(h, WM_TOUCH, (WPARAM)1, (LPARAM)(HTOUCHINPUT)g_fake1);

    ZeroMemory(g_fake2, sizeof(g_fake2));
    g_fake2[0].dwID = 1000; g_fake2[0].x = 100 * 100; g_fake2[0].y = 200 * 100;
    g_fake2[0].dwFlags = TOUCHEVENTF_MOVE;
    g_fake2[1].dwID = 1001; g_fake2[1].x = 300 * 100; g_fake2[1].y = 400 * 100;
    g_fake2[1].dwFlags = TOUCHEVENTF_UP;
    PostMessageW(h, WM_TOUCH, (WPARAM)2, (LPARAM)(HTOUCHINPUT)g_fake2);
    lg("SELFTEST posted 2 fake WM_TOUCH\n");
}

static LRESULT CALLBACK wndproc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_TOUCH: {
        UINT n = LOWORD(wp);
        if (n > MAXT) n = MAXT;
        TOUCHINPUT ti[MAXT];
        ZeroMemory(ti, sizeof(ti));
        DWORD now = GetTickCount();
        BOOL ok = GetTouchInputInfo((HTOUCHINPUT)lp, n, ti, (int)sizeof(TOUCHINPUT));
        DWORD gerr = ok ? 0 : GetLastError();
        CloseTouchInputHandle((HTOUCHINPUT)lp);      // 同步解码后立刻放句柄
        if (ok) {
            log_touch(n, ti, now, g_last_msg_time);
        } else if (g_selftest) {
            // 没有探针钩子时（冒烟）：lParam 就是本进程内的 TOUCHINPUT 数组，直接解码。
            // 日志前缀故意用 SELFTOUCH，验收脚本不认这一种行，避免把"钩子没装"伪装成通过。
            const TOUCHINPUT* src = (const TOUCHINPUT*)(const void*)lp;
            g_touches++;
            lg("SELFTOUCH n=%u", n);
            for (UINT i = 0; i < n; i++)
                lg(" [id=%u f=%x x=%d y=%d]",
                   (unsigned)src[i].dwID, (unsigned)src[i].dwFlags,
                   (int)TOUCH_COORD_TO_PIXEL(src[i].x), (int)TOUCH_COORD_TO_PIXEL(src[i].y));
            lg(" wt=%lu\n", (unsigned long)g_last_msg_time);
        } else {
            g_gti_fail++;
            lg("GTI_FAIL n=%u err=%lu\n", n, (unsigned long)gerr);
        }
        return 0;
    }
    case WM_SIZE: {
        RECT rc;
        GetClientRect(h, &rc);
        lg("RESIZE client=%ldx%ld\n", (long)(rc.right - rc.left), (long)(rc.bottom - rc.top));
        return 0;
    }
    case WM_CLOSE:
        DestroyWindow(h);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE prev, LPWSTR cmd, int show)
{
    (void)prev; (void)cmd; (void)show;
    g_start = GetTickCount();

    WCHAR logpath[MAX_PATH * 2];
    env_str(L"TOUCHRECV_LOG", logpath, MAX_PATH * 2, L"build\\touchrecv.log");
    env_str(L"TOUCHRECV_STOP", g_stop, MAX_PATH * 2, L"build\\touchrecv.stop");
    g_secs = env_num(L"TOUCHRECV_SECS", 180);
    g_selftest = env_num(L"TOUCHRECV_SELFTEST", 0) ? 1 : 0;
    g_hide = env_num(L"TOUCHRECV_HIDE", 0) ? 1 : 0;   /* 1=不映射窗口（冒烟自测用，零屏幕干扰）*/

    g_log = _wfopen(logpath, L"a");
    if (!g_log) return 2;

    WCHAR cwd[MAX_PATH * 2];
    if (!GetCurrentDirectoryW(MAX_PATH * 2, cwd)) cwd[0] = 0;
    lg("START pid=%lu tick=%lu cwd=%ls\n", (unsigned long)GetCurrentProcessId(),
       (unsigned long)g_start, cwd);
    lg("LOG path=%ls\n", logpath);

    WNDCLASSW wc;
    ZeroMemory(&wc, sizeof(wc));
    wc.lpfnWndProc = wndproc;
    wc.hInstance = inst;
    wc.lpszClassName = L"touchrecvwnd";
    ATOM at = RegisterClassW(&wc);

    HWND h = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TRANSPARENT,
                             L"touchrecvwnd", L"touchrecv",
                             WS_POPUP | (g_hide ? 0 : WS_VISIBLE), 0, 0, WIN_W, WIN_H,
                             NULL, NULL, inst, NULL);
    if (!h) {
        lg("CREATE_FAIL err=%lu atom=%u\n", (unsigned long)GetLastError(), (unsigned)at);
        return 3;
    }
    if (!g_hide) {
        ShowWindow(h, SW_SHOWNOACTIVATE);
        SetWindowPos(h, NULL, 0, 0, WIN_W, WIN_H, SWP_NOZORDER | SWP_NOACTIVATE | SWP_SHOWWINDOW);
    }

    RECT rc;
    GetClientRect(h, &rc);
    long cw = (long)(rc.right - rc.left), ch = (long)(rc.bottom - rc.top);
    LONG style = (LONG)GetWindowLongPtrW(h, GWL_STYLE);
    lg("CLASS atom=%u hwnd=%p\n", (unsigned)at, (void*)h);
    LONG exstyle = (LONG)GetWindowLongPtrW(h, GWL_EXSTYLE);
    RECT wr; GetWindowRect(h, &wr);
    /* origin 是 28 版「TOUCHINPUT 按屏幕坐标填」那条改动的前提：窗口原点不在 (0,0) 时，
       ClientToScreen 不再恒等，坐标断言/27vs28 等价性都会出现固定偏移（那是真语义差，不是假 FAIL）*/
    lg("WINDOW hwnd=%p title=touchrecv client=%ldx%ld origin=%ld,%ld style=%08lx borderless=%d visible=%d exstyle=%08lx hidden=%d\n",
       (void*)h, cw, ch, (long)wr.left, (long)wr.top, (unsigned long)(DWORD)style,
       (style & WS_POPUP) && !(style & WS_CAPTION) ? 1 : 0, (int)IsWindowVisible(h),
       (unsigned long)(DWORD)exstyle, g_hide);

    BOOL rtw = RegisterTouchWindow(h, 0);
    lg("RTW -> %d err=%lu\n", (int)rtw, (unsigned long)GetLastError());

    int quit = 0;
    DWORD last_check = GetTickCount();
    while (!quit) {
        MSG m;
        while (PeekMessageW(&m, NULL, 0, 0, PM_REMOVE)) {
            if (m.message == WM_QUIT) { quit = 1; break; }
            g_last_msg_time = m.time;
            if (++g_msgs <= MSGW)
                lg("WMSG #%u msg=%04x wp=%llx lp=%p\n", g_msgs, (unsigned)m.message,
                   (unsigned long long)m.wParam, (void*)m.lParam);
            if (m.message >= 0x0245 && m.message <= 0x0249) {
                /* WM_POINTER* 只计数不 Dispatch：全屏窗口上的高频指针流会把消息泵拖慢，
                   导致 WM_TOUCH 的消息时间滞后上百毫秒（场景边界错位的根因）*/
                g_ptr_msgs++;
                continue;
            }
            TranslateMessage(&m);
            DispatchMessageW(&m);
        }
        if (quit) break;
        DWORD now = GetTickCount();
        if (now - last_check >= 100) {
            last_check = now;
            if (g_stop[0] && GetFileAttributesW(g_stop) != INVALID_FILE_ATTRIBUTES) {
                lg("STOP file=%ls -> exit\n", g_stop);
                break;
            }
            if (g_secs && now - g_start >= g_secs * 1000) {
                lg("WATCHDOG %lu secs -> exit\n", (unsigned long)g_secs);
                break;
            }
            if (g_selftest && !g_selftest_done && now - g_start > 800) {
                selftest_post(h);
                g_selftest_done = 1;
            }
        }
        Sleep(5);
    }

    lg("SUMMARY touches=%u gti_fail=%u msgs=%u ptr=%u\n", g_touches, g_gti_fail, g_msgs, g_ptr_msgs);
    DestroyWindow(h);
    if (g_log) { fclose(g_log); g_log = NULL; }
    return 0;
}
