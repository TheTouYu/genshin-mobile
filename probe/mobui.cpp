// mobui_probe.dll —— 原神 CN 7.2 移动 UI 实验探针（注入式，非代理 DLL）
//
// 一个 DLL 支撑整场实机实验，不为改判据重启游戏：
//   * 只读观测：定时把 il2cpp 静态量/单例指针/输入模式字段写进日志。
//   * 被动捕获：hook 触控切换 / 键鼠切换 / 分发器，游戏自己调用时记录真实 this。
//   * 受控实验：命令文件逐行投递；命令在 ChangeFOV 钩子内（游戏渲染线程）执行，
//     每条都走 VEH+longjmp 守卫（clang __try 在 mingw 目标下不生效，实测确认），前后各打一行日志。
//
// 命令通道：<dll 同目录>/<dll 主名>.cmds   仅处理新增行，可追加
// 日志：    <dll 同目录>/<dll 主名>.log
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>
#include <setjmp.h>
#include <stdlib.h>
#include "MinHook.h"
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")

// ---- CN 7.2 静态坐标（ImageBase 0x140000000） ----
static const uint64_t kRvaChangeFov = 0x19B0A00;
static const uint64_t kRvaTouch = 0xD484110;
static const uint64_t kRvaKeyboard = 0xD476E00;
static const uint64_t kRvaDispatcher = 0xD47A4A0;
static const uint64_t kRvaRoot = 0x586B600;
static const uint64_t kRvaFlagA = 0x584B655;
static const uint64_t kRvaFlagB = 0x584B722;
static const uint64_t kRvaFlagC = 0x5B7DF90;
static const uint64_t kRvaSingleInput = 0x5890290;
static const uint64_t kRvaMethodInfo = 0x5989820;
static const uint64_t kRvaPlatform = 0x116B850;   // Application::get_platform 薄跳板（0x14099DF60 = mov eax,2; ret）
static const uint32_t kSlot9E60 = 0x9E60;
static const uint32_t kSlot9EC0 = 0x9EC0;
static const uint32_t kSlotA070 = 0xA070;
static const uint32_t kModeField = 0x1AC;

enum { CMD_NONE = 0, CMD_DUMP, CMD_PING, CMD_TOUCH_NULL, CMD_TOUCH_CAPTURED,
       CMD_DISPATCH0, CMD_KB_CAPTURED, CMD_FLIP_A, CMD_FIELDS,
       CMD_FIELD0, CMD_SETTERS, CMD_TOUCH_E60, CMD_TOUCH_E2C0, CMD_SETTER1, CMD_SETTER2,
       CMD_UI0, CMD_TOUCH_A070, CMD_GETTER, CMD_TOUCH_GATE, CMD_TOUCHSEL, CMD_RELEASE,
       CMD_UIPC, CMD_UIAUTO0, CMD_UIAUTO1 };

typedef int32_t(__attribute__((ms_abi)) *FovFn)(void*, float);
typedef void(__attribute__((ms_abi)) *UnaryFn)(void*);
typedef void(__attribute__((ms_abi)) *DispatchFn)(void*, int);

static HMODULE g_self;
static uint8_t* g_base;
static volatile LONG g_frame;
static volatile LONG g_cmd = CMD_NONE;
static volatile LONG g_hook_touch_hits, g_hook_kb_hits, g_hook_disp_hits;
static volatile LONG g_fov_ready, g_fov_hooked;

static void* g_touch_this;
static void* g_kb_this;
static void* g_disp_this;

typedef int32_t(__attribute__((ms_abi)) *ZeroFn)(void);
static ZeroFn g_orig_platform;
typedef int(__attribute__((ms_abi)) *GsmFn)(int);
static GsmFn g_orig_gsm;
static volatile LONG g_gsm_calls;

// ---- WM_POINTER -> WM_TOUCH 桥（Wine 只发指针消息，Unity 只认 WM_TOUCH） ----
// 投递模型：一条 WM_TOUCH = 当前全部接触点的一次快照；接触点状态只由 contact_upsert 维护，
// 状态机 1=DOWN 待发 2=按住 3=UP 待发 0=空闲；emit_touch 发完即推进。所有改 g_contacts 的路径
// 都必须持有 g_touch_lock（net 线程与游戏线程共用）。
#define MAX_CONTACTS 10
#define TI_SLOTS 64                 /* 必须是 2 的幂：取槽用 &(TI_SLOTS-1)，越界在语法上不可能 */
struct Contact { UINT32 id; int x; int y; int ax; int ay; int state; };   // state 1=down 2=move 3=up；ax/ay=按下时的原始位置（增益锚点）
static Contact g_contacts[MAX_CONTACTS];
static CRITICAL_SECTION g_touch_lock;
static TOUCHINPUT g_ti_buf[TI_SLOTS][MAX_CONTACTS];
static volatile LONG g_ti_busy[TI_SLOTS];   // 游戏没读走（CloseTouchInputHandle）前不回收该槽
static volatile LONG g_ti_slot;            // 所有投递路径共用的槽位游标
static volatile LONG g_touch_msg_count, g_ti_called, g_rtw_called;
static volatile LONG g_stat_gti_foreign, g_proto_bad, g_watchdog_lifts, g_net_last_pkt;
static volatile LONG g_ui_auto = 0;      /* 1=检测到游戏把输入模式改回 PC(2) 时自动重新切触摸；默认 0=用户手动控制 */
static volatile LONG g_convert_on = 1;   /* 0=不把系统指针消息转成 WM_TOUCH（用真实触摸通道时关掉，避免幽灵手指） */
static volatile LONG g_look_gain = 100;   /* 触摸位移增益百分比：100=原样；<100 用于"游戏内灵敏度最低仍太快"时细化。cfg 键 lookgain= */
static volatile LONG g_focus_gate = 1;    /* 1=游戏不是前台窗口时不投递（cfg 键 focusgate=） */
static volatile LONG g_focus_blocked = 0; /* 已因失去焦点暂停投递的标记，用于只释放一次 */
static volatile LONG g_screen_xy = 1;    /* 1=TOUCHINPUT.x/y 用屏幕坐标（MS 口径，ClientToScreen 后 ×100）；0=直接用客户区坐标。
                                             窗口原点在 (0,0) 时两者逐字节等价；cfg 键 screenxy= */
static volatile LONG g_primary_mode = 0; /* TOUCHEVENTF_PRIMARY 语义（捏合 A/B 用）：0=first 数组首元素（现状）
                                            1=anchor 主指=该序列第一个 DOWN 的 dwID，直到它抬起
                                            2=none 不置 PRIMARY。cfg 键 primary= / 运行时命令 primary */
static volatile LONG g_primary_id = 0;   /* anchor 模式下的主指 dwID；0=无主指 */
static int g_test_mode = 0;              /* 1=离线验收台模式：只装 user32 导出钩子，不碰游戏地址 */
static void release_all_contacts(const char* why);

typedef BOOL (__attribute__((ms_abi)) *PeekMsgFn)(LPMSG, HWND, UINT, UINT, UINT);
typedef BOOL (__attribute__((ms_abi)) *GetMsgFn)(LPMSG, HWND, UINT, UINT);
typedef BOOL (__attribute__((ms_abi)) *RTWFn)(HWND, ULONG);
typedef BOOL (__attribute__((ms_abi)) *GTIFn)(HTOUCHINPUT, UINT, PTOUCHINPUT, int);
typedef BOOL (__attribute__((ms_abi)) *CTIHFn)(HTOUCHINPUT);
static PeekMsgFn o_PeekMessageW, o_PeekMessageA;
static volatile LONG g_pm_seen, g_get_seen, g_pm_ptr;
static GetMsgFn o_GetMessageW;
static RTWFn o_RegisterTouchWindow;
static GTIFn o_GetTouchInputInfo;
static CTIHFn o_CloseTouchInputHandle;
static FovFn g_orig_fov;
static UnaryFn g_orig_touch, g_orig_kb;
static DispatchFn g_orig_disp;

static volatile LONG g_stat_rx, g_stat_post, g_stat_gti, g_stat_down, g_stat_up;
static HANDLE g_tlog = INVALID_HANDLE_VALUE;
static volatile LONG g_tlog_lines;
static void tlog(const char* fmt, ...)
{
    if (g_tlog == INVALID_HANDLE_VALUE) return;
    char b[512]; va_list ap; va_start(ap, fmt); int n = _vsnprintf(b, sizeof(b)-1, fmt, ap); va_end(ap);
    if (n <= 0) return; b[n] = 0;
    char line[600];
    int m = _snprintf(line, sizeof(line)-1, "%lu t=%6lu %s\n", (unsigned long)GetCurrentThreadId(), (unsigned long)GetTickCount(), b);
    if (m <= 0) return;
    DWORD w; WriteFile(g_tlog, line, (DWORD)m, &w, NULL);
}   /* 0=关 1=鼠标键+光标 2=再加 raw input */
static volatile LONG g_mouse_suppressed_calls;
typedef SHORT (__attribute__((ms_abi)) *GAKSFn)(int);
typedef SHORT (__attribute__((ms_abi)) *GKSFn)(int);
typedef BOOL  (__attribute__((ms_abi)) *GCursorFn)(LPPOINT);
typedef UINT  (__attribute__((ms_abi)) *GRIDFn)(HRAWINPUT, UINT, LPVOID, PUINT, UINT);
static GAKSFn o_GetAsyncKeyState;
static GKSFn  o_GetKeyState;
static GCursorFn o_GetCursorPos;
static GRIDFn o_GetRawInputData;

static int is_mouse_vk(int vk)
{
    return vk == VK_LBUTTON || vk == VK_RBUTTON || vk == VK_MBUTTON ||
           vk == VK_XBUTTON1 || vk == VK_XBUTTON2;
}
static HANDLE g_log = INVALID_HANDLE_VALUE;
static CRITICAL_SECTION g_lock;
static wchar_t g_logpath[MAX_PATH] = {0};
static wchar_t g_cmdpath[MAX_PATH] = {0};
static wchar_t g_touchcmdpath[MAX_PATH] = {0};
static volatile LONG g_touchcmd_lines;
static HWND g_game_hwnd;
static volatile LONG g_map_on;
static int g_joy_cx = 210, g_joy_cy = 790, g_joy_r = 120;
static int g_cam_ax = 1300, g_cam_ay = 480;   /* 镜头手指的锚点（右侧空白区） */
static volatile LONG g_map_sent;
static int g_map_enabled = 0;   /* 默认关，显式 map=1 才开 */
static void map_emit(void);

static DWORD WINAPI mapper(LPVOID u)
{
    (void)u;
    for (;;) {
        if (!InterlockedCompareExchange(&g_map_on, 0, 0)) { Sleep(50); continue; }
        map_emit();
        Sleep(16);
    }
    return 0;
}
static DWORD g_start_tick;
static unsigned g_cmd_lines_done;
static unsigned g_hook_mask = 15;   // bit0 FOV bit1 touch bit2 kb bit3 disp
static int g_use_veh = 1;
static wchar_t g_cfgpath[MAX_PATH] = {0};

static inline uint64_t at(uint64_t rva);   // fwd

// ---- 异常守卫：VEH + longjmp（Wine 实测可捕获 0xc0000005 且线程存活） ----
static jmp_buf g_jb;
static volatile LONG g_armed;
static DWORD g_exc;
static int g_call_kind;   // 1=touch 2=keyboard 3=dispatcher
static void* g_call_self;
static int g_call_mode;
static void* g_getter_ret;
static void* g_gate_ret;
static uint64_t g_gate_mi;

static LONG CALLBACK veh_handler(EXCEPTION_POINTERS* p)
{
    if (InterlockedCompareExchange(&g_armed, 0, 0)) {
        g_exc = p->ExceptionRecord->ExceptionCode;
        InterlockedExchange(&g_armed, 0);
        longjmp(g_jb, 1);
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

static void do_pending_call(void)
{
    switch (g_call_kind) {
    case 1: (g_orig_touch ? g_orig_touch : (UnaryFn)(uintptr_t)at(kRvaTouch))(g_call_self); break;
    case 2: (g_orig_kb ? g_orig_kb : (UnaryFn)(uintptr_t)at(kRvaKeyboard))(g_call_self); break;
    case 3: (g_orig_disp ? g_orig_disp : (DispatchFn)(uintptr_t)at(kRvaDispatcher))(g_call_self, g_call_mode); break;
    case 4: {
        typedef void(__attribute__((ms_abi)) *Fn3)(void*, int, int);
        ((Fn3)(uintptr_t)at(0x14A206F50))(g_call_self, 0, 1);
        break;
    }
    case 7: {
        typedef void*(__attribute__((ms_abi)) *Fn2)(void*, void*);
        g_gate_ret = ((Fn2)(uintptr_t)at(0x1546FB6A0))(g_call_self, (void*)(uintptr_t)g_gate_mi);
        break;
    }
    case 6: {
        typedef void*(__attribute__((ms_abi)) *Fn1)(void*);
        g_getter_ret = ((Fn1)(uintptr_t)at(0x14A9CE8E0))(g_call_self);
        break;
    }
    case 5: {
        typedef void(__attribute__((ms_abi)) *Fn3)(void*, int, int);
        ((Fn3)(uintptr_t)at(0x14C295400))(g_call_self, 0, 0);
        break;
    }
    default: break;
    }
}

// 返回 0=正常返回，否则=异常码
// 只在这个微秒级窗口内注册 VEH：反作弊若按秒级轮询 VEH 链，看不到它
static DWORD guarded_call(int kind, void* self, int mode)
{
    g_call_kind = kind;
    g_call_self = self;
    g_call_mode = mode;
    PVOID h = AddVectoredExceptionHandler(1, veh_handler);
    if (setjmp(g_jb) == 0) {
        InterlockedExchange(&g_armed, 1);
        do_pending_call();
        InterlockedExchange(&g_armed, 0);
        if (h) RemoveVectoredExceptionHandler(h);
        return 0;
    }
    InterlockedExchange(&g_armed, 0);
    if (h) RemoveVectoredExceptionHandler(h);
    return g_exc ? g_exc : 0xFFFFFFFFu;
}

// ---- 日志 / 安全读 ----
static void xlog(const char* fmt, ...)
{
    char body[1024];
    va_list ap;
    va_start(ap, fmt);
    int n = _vsnprintf(body, sizeof(body) - 1, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if (n > (int)sizeof(body) - 1) n = (int)sizeof(body) - 1;
    body[n] = 0;

    char line[1200];
    int m = _snprintf(line, sizeof(line) - 1, "[%6lu] t%-5lu f%-6ld %s\n",
                      (unsigned long)(GetTickCount() - g_start_tick),
                      (unsigned long)GetCurrentThreadId(),
                      (long)InterlockedCompareExchange(&g_frame, 0, 0), body);
    if (m < 0) return;
    if (m > (int)sizeof(line) - 1) m = (int)sizeof(line) - 1;
    line[m] = 0;
    EnterCriticalSection(&g_lock);
    if (g_log != INVALID_HANDLE_VALUE) {
        DWORD written;
        WriteFile(g_log, line, (DWORD)m, &written, NULL);
        FlushFileBuffers(g_log);
    }
    LeaveCriticalSection(&g_lock);
}

static void xflush(void)
{
    EnterCriticalSection(&g_lock);
    if (g_log != INVALID_HANDLE_VALUE) FlushFileBuffers(g_log);
    LeaveCriticalSection(&g_lock);
}

static inline uint64_t at(uint64_t rva) { return (uint64_t)(uintptr_t)(g_base + rva); }

static bool safe_read(uint64_t va, void* out, size_t n)
{
    uint8_t* dst = (uint8_t*)out;
    uint64_t cur = va;
    while (n) {
        MEMORY_BASIC_INFORMATION m;
        if (VirtualQuery((void*)(uintptr_t)cur, &m, sizeof(m)) != sizeof(m)) return false;
        if (m.State != MEM_COMMIT) return false;
        if (m.Protect & (PAGE_NOACCESS | PAGE_GUARD)) return false;
        uint64_t region_end = (uint64_t)(uintptr_t)m.BaseAddress + m.RegionSize;
        if (region_end <= cur) return false;
        size_t avail = (size_t)(region_end - cur);
        size_t take = n < avail ? n : avail;
        memcpy(dst, (void*)(uintptr_t)cur, take);
        dst += take;
        cur += take;
        n -= take;
    }
    return true;
}

static uint64_t rd64(uint64_t va) { uint64_t v = 0; return safe_read(va, &v, 8) ? v : 0; }
static uint32_t rd32(uint64_t va) { uint32_t v = 0; return safe_read(va, &v, 4) ? v : 0; }
static uint8_t rd8(uint64_t va) { uint8_t v = 0xFF; return safe_read(va, &v, 1) ? v : 0xFF; }
static bool bytes_are(uint64_t va, const uint8_t* want, size_t n)
{
    uint8_t buf[32];
    if (n > sizeof(buf)) return false;
    return safe_read(va, buf, n) && memcmp(buf, want, n) == 0;
}

static void dump_fields(void* self, const char* tag)
{
    if (!self) {
        xlog("FIELDS [%s] receiver=null", tag);
        return;
    }
    uint64_t s = (uint64_t)(uintptr_t)self;
    xlog("FIELDS [%s] self=%p +1AC(mode)=%u +340=%p +348=%p +388=%p +3A8=%p +440=%p +48=%p +50=%p",
         tag, self, rd32(s + kModeField),
         (void*)(uintptr_t)rd64(s + 0x340), (void*)(uintptr_t)rd64(s + 0x348),
         (void*)(uintptr_t)rd64(s + 0x388), (void*)(uintptr_t)rd64(s + 0x3A8),
         (void*)(uintptr_t)rd64(s + 0x440), (void*)(uintptr_t)rd64(s + 0x48),
         (void*)(uintptr_t)rd64(s + 0x50));
}

static void dump_state(const char* why)
{
    uint64_t root = rd64(at(kRvaRoot));
    uint64_t s1 = root ? rd64(root + kSlot9E60) : 0;
    uint64_t s2 = root ? rd64(root + kSlot9EC0) : 0;
    uint64_t s3 = root ? rd64(root + kSlotA070) : 0;
    xlog("STATE [%s] root=%p flagA=%02x flagB=%02x flagC=%02x S=%p MI=%p",
         why, (void*)(uintptr_t)root, (unsigned)rd8(at(kRvaFlagA)), (unsigned)rd8(at(kRvaFlagB)),
         (unsigned)rd8(at(kRvaFlagC)), (void*)(uintptr_t)(at(kRvaSingleInput)),
         (void*)(uintptr_t)rd64(at(kRvaMethodInfo)));
    xlog("STATE slot388 9E60=%p 9EC0=%p A070=%p",
         (void*)(uintptr_t)(s1 ? rd64(s1 + 0x388) : 0),
         (void*)(uintptr_t)(s2 ? rd64(s2 + 0x388) : 0),
         (void*)(uintptr_t)(s3 ? rd64(s3 + 0x388) : 0));
    xlog("STATE slots 9E60=%p 9EC0=%p A070=%p mode9EC0=%u mode9E60=%u",
         (void*)(uintptr_t)s1, (void*)(uintptr_t)s2, (void*)(uintptr_t)s3,
         s2 ? rd32(s2 + kModeField) : 0xFFFFFFFFu, s1 ? rd32(s1 + kModeField) : 0xFFFFFFFFu);
    xlog("STATE captured touch=%p kb=%p disp=%p | hits touch=%ld kb=%ld disp=%ld | fov=%ld",
         g_touch_this, g_kb_this, g_disp_this, (long)g_hook_touch_hits, (long)g_hook_kb_hits,
         (long)g_hook_disp_hits, (long)g_fov_ready);
}

// ---- 钩子 ----
static void run_command(LONG cmd, void* self)
{
    (void)self;
    if (g_test_mode) { xlog("TEST MODE: 忽略命令 cmd=%ld（不碰游戏地址）", (long)cmd); return; }
    xlog("EXEC begin cmd=%ld", (long)cmd);
    switch (cmd) {
    case CMD_DUMP: dump_state("on-demand"); break;
    case CMD_PING: xlog("RESULT ping ok (game thread alive, frame=%ld)", (long)g_frame); break;
    case CMD_TOUCH_NULL: {
        /* 幂等保护：已经是触摸模式就别再调（实测重复调用会在游戏里抛访问违例 0xc0000005） */
        {
            uint64_t root0 = rd64(at(kRvaRoot));
            uint64_t mgr0 = root0 ? rd64(root0 + kSlot9EC0) : 0;
            int m0 = mgr0 ? (int)rd32(mgr0 + kModeField) : -1;
            if (m0 == 0) { xlog("RESULT touch_null skipped: already touch mode (mode9EC0=0)"); break; }
        }
        dump_state("before touch_null");
        DWORD e = guarded_call(1, NULL, 0);
        xlog("RESULT touch_null %s%s0x%08lx", e ? "exception=" : "returned normally",
             e ? "" : "", (unsigned long)e);
        dump_state("after touch_null");
        break;
    }
    case CMD_TOUCH_CAPTURED: {
        void* r = g_touch_this ? g_touch_this : (g_kb_this ? g_kb_this : g_disp_this);
        if (!r) { xlog("RESULT touch_captured skipped: no captured receiver"); break; }
        dump_fields(r, "before touch_captured");
        DWORD e = guarded_call(1, r, 0);
        xlog("RESULT touch_captured(%p) %s0x%08lx", r, e ? "exception=" : "returned normally", (unsigned long)e);
        dump_fields(r, "after touch_captured");
        break;
    }
    case CMD_DISPATCH0: {
        void* r = g_disp_this ? g_disp_this : (g_kb_this ? g_kb_this : g_touch_this);
        if (!r) { xlog("RESULT dispatch0 skipped: no captured receiver"); break; }
        dump_state("before dispatch0");
        DWORD e = guarded_call(3, r, 0);
        xlog("RESULT dispatch0(%p,0) %s0x%08lx", r, e ? "exception=" : "returned normally", (unsigned long)e);
        dump_state("after dispatch0");
        break;
    }
    case CMD_KB_CAPTURED: {
        void* r = g_kb_this ? g_kb_this : g_disp_this;
        if (!r) { xlog("RESULT kb_captured skipped: no captured receiver"); break; }
        dump_state("before kb_captured");
        DWORD e = guarded_call(2, r, 0);
        xlog("RESULT kb_captured(%p) %s0x%08lx", r, e ? "exception=" : "returned normally", (unsigned long)e);
        dump_state("after kb_captured");
        break;
    }
    case CMD_FLIP_A: {
        uint8_t before = rd8(at(kRvaFlagA));
        uint8_t after = before;
        if (safe_read(at(kRvaFlagA), &after, 0)) { /* no-op */ }
        DWORD old = 0;
        uint64_t p = at(kRvaFlagA);
        if (VirtualProtect((void*)(uintptr_t)(p & ~0xFFFull), 0x1000, PAGE_READWRITE, &old)) {
            *(volatile uint8_t*)(uintptr_t)p = 1;
            DWORD tmp;
            VirtualProtect((void*)(uintptr_t)(p & ~0xFFFull), 0x1000, old, &tmp);
            xlog("RESULT flipA %02x -> %02x", (unsigned)before, (unsigned)rd8(p));
        } else {
            xlog("RESULT flipA failed: VirtualProtect err=%lu", (unsigned long)GetLastError());
        }
        dump_state("after flipA");
        break;
    }
    case CMD_FIELD0: {
        uint64_t root = rd64(at(kRvaRoot));
        uint64_t mgr = root ? rd64(root + kSlot9EC0) : 0;
        if (!mgr) { xlog("RESULT field0 skipped: manager null (root=%p)", (void*)(uintptr_t)root); break; }
        int before = (int)rd32(mgr + kModeField);
        DWORD old = 0, tmp = 0;
        uint64_t page = (mgr + kModeField) & ~0xFFFull;
        if (VirtualProtect((void*)(uintptr_t)page, 0x1000, PAGE_READWRITE, &old)) {
            *(volatile int*)(uintptr_t)(mgr + kModeField) = 0;
            VirtualProtect((void*)(uintptr_t)page, 0x1000, old, &tmp);
            xlog("RESULT field0 manager=%p mode %d -> %d", (void*)(uintptr_t)mgr, before,
                 (int)rd32(mgr + kModeField));
        } else {
            xlog("RESULT field0 VirtualProtect failed err=%lu", (unsigned long)GetLastError());
        }
        break;
    }
    case CMD_SETTER1:
    case CMD_SETTER2:
    case CMD_SETTERS: {
        uint64_t root = rd64(at(kRvaRoot));
        uint64_t m1 = root ? rd64(root + kSlot9E60) : 0;
        uint64_t m2 = root ? rd64(root + kSlot9EC0) : 0;
        xlog("RESULT setters managers m9E60=%p m9EC0=%p", (void*)(uintptr_t)m1, (void*)(uintptr_t)m2);
        if ((cmd == CMD_SETTER1 || cmd == CMD_SETTERS) && m1) {
            g_call_kind = 4; g_call_self = (void*)(uintptr_t)m1; g_call_mode = 0;
            DWORD e = guarded_call(4, (void*)(uintptr_t)m1, 0);
            xlog("RESULT setter1(m9E60,0,1) %s0x%08lx", e ? "exception=" : "returned normally", (unsigned long)e);
        }
        if ((cmd == CMD_SETTER2 || cmd == CMD_SETTERS) && m2) {
            DWORD e = guarded_call(5, (void*)(uintptr_t)m2, 0);
            xlog("RESULT setter2(m9EC0,0,0) %s0x%08lx", e ? "exception=" : "returned normally", (unsigned long)e);
        }
        dump_state("after setters");
        break;
    }
    case CMD_TOUCH_E60:
    case CMD_TOUCH_E2C0: {
        uint64_t root = rd64(at(kRvaRoot));
        uint64_t mgr = root ? rd64(root + (cmd == CMD_TOUCH_E60 ? kSlot9E60 : kSlot9EC0)) : 0;
        if (!mgr) { xlog("RESULT touch_mgr skipped: manager null"); break; }
        dump_state("before touch_mgr");
        DWORD e = guarded_call(1, (void*)(uintptr_t)mgr, 0);
        xlog("RESULT touch_mgr(%p) %s0x%08lx", (void*)(uintptr_t)mgr, e ? "exception=" : "returned normally", (unsigned long)e);
        dump_state("after touch_mgr");
        break;
    }
    case CMD_GETTER:
    case CMD_UI0: {
        uint64_t root = rd64(at(kRvaRoot));
        uint64_t mgr = root ? rd64(root + kSlotA070) : 0;
        if (!mgr) { xlog("RESULT getter skipped: A070 null"); break; }
        DWORD e = guarded_call(6, (void*)(uintptr_t)mgr, 0);
        xlog("RESULT getter(A070=%p) %s0x%08lx ret=%p", (void*)(uintptr_t)mgr, e ? "exception=" : "returned normally",
             (unsigned long)e, g_getter_ret);
        if (cmd == CMD_UI0 && g_getter_ret) {
            uint8_t before = rd8((uint64_t)(uintptr_t)g_getter_ret + 0x3F4);
            DWORD old = 0, tmp = 0;
            uint64_t page = ((uint64_t)(uintptr_t)g_getter_ret + 0x3F4) & ~0xFFFull;
            if (VirtualProtect((void*)(uintptr_t)page, 0x1000, PAGE_READWRITE, &old)) {
                *(volatile uint8_t*)((uintptr_t)g_getter_ret + 0x3F4) = 0;
                VirtualProtect((void*)(uintptr_t)page, 0x1000, old, &tmp);
                xlog("RESULT ui0 obj=%p flag3F4 %02x -> %02x", g_getter_ret, (unsigned)before,
                     (unsigned)rd8((uint64_t)(uintptr_t)g_getter_ret + 0x3F4));
            } else {
                xlog("RESULT ui0 protect failed err=%lu", (unsigned long)GetLastError());
            }
        }
        break;
    }
    case CMD_TOUCH_A070: {
        uint64_t root = rd64(at(kRvaRoot));
        uint64_t mgr = root ? rd64(root + kSlotA070) : 0;
        if (!mgr) { xlog("RESULT touch_a070 skipped: A070 null"); break; }
        dump_state("before touch_a070");
        DWORD e = guarded_call(1, (void*)(uintptr_t)mgr, 0);
        xlog("RESULT touch_a070(%p) %s0x%08lx", (void*)(uintptr_t)mgr, e ? "exception=" : "returned normally", (unsigned long)e);
        dump_state("after touch_a070");
        break;
    }
    case CMD_TOUCH_GATE: {
        uint64_t root = rd64(at(kRvaRoot));
        uint64_t m9e60 = root ? rd64(root + kSlot9E60) : 0;
        uint64_t mi = rd64(at(kRvaMethodInfo));
        if (!m9e60) { xlog("GATE skip: m9E60 null"); break; }
        typedef void*(__attribute__((ms_abi)) *Fn2)(void*, void*);
        void* rdi = NULL;
        g_gate_mi = rd64(at(kRvaMethodInfo));
        DWORD e = guarded_call(7, (void*)(uintptr_t)m9e60, 0);
        rdi = g_gate_ret;
        xlog("GATE lookup(m9E60=%p, MI=%p) -> rdi=%p exc=0x%08lx", (void*)(uintptr_t)m9e60,
             (void*)(uintptr_t)mi, rdi, (unsigned long)e);
        if (!rdi) { xlog("GATE: receiver lookup null → 不发切换（避免 0xc0000005 空转）"); break; }
        dump_state("before gate-switch");
        DWORD e2 = guarded_call(1, NULL, 0);
        xlog("GATE switch(nullptr) %s0x%08lx", e2 ? "exception=" : "returned normally", (unsigned long)e2);
        dump_state("after gate-switch");
        break;
    }
    case CMD_TOUCHSEL: {
        // 给游戏窗口重新做一次 RegisterTouchWindow（Wine 的 XI2 触摸选择可能只在注册时建立）
        HWND hw = FindWindowW(NULL, L"原神");
        if (!hw) {
            EnumWindows([](HWND w, LPARAM p) -> BOOL {
                DWORD pid = 0; GetWindowThreadProcessId(w, &pid);
                if (pid == GetCurrentProcessId() && IsWindowVisible(w)) { *(HWND*)p = w; return FALSE; }
                return TRUE;
            }, (LPARAM)&hw);
        }
        xlog("TOUCHSEL hwnd=%p", (void*)hw);
        if (hw) {
            if (o_RegisterTouchWindow) { BOOL r = o_RegisterTouchWindow(hw, 0); xlog("TOUCHSEL re-register -> %d err=%lu", r, GetLastError()); }
            else { BOOL r = RegisterTouchWindow(hw, 0); xlog("TOUCHSEL raw register -> %d err=%lu", r, GetLastError()); }
        }
        dump_state("after touchsel");
        break;
    }
    case CMD_RELEASE: {
        release_all_contacts("cmd");
        break;
    }
    case CMD_UIPC: {
        /* 切 PC UI：必须走游戏自己的「应用输入模式」函数（与 touch_null 同一条路径，只是模式值传 2）。
           裸写 kModeField 只会让字段变成 2、不触发 UI 重建——2026-11-19 用户实测：日志显示
           "mode 0 -> 2" 成功，但画面没变。 */
        {
            uint64_t root0 = rd64(at(kRvaRoot));
            uint64_t mgr0 = root0 ? rd64(root0 + kSlot9EC0) : 0;
            int m0 = mgr0 ? (int)rd32(mgr0 + kModeField) : -1;
            if (m0 == 2) { xlog("RESULT uipc skipped: already PC mode (mode9EC0=2)"); break; }
        }
        dump_state("before uipc");
        DWORD e = guarded_call(1, NULL, 2);
        xlog("RESULT uipc apply(mode=2) %s0x%08lx", e ? "exception=" : "returned normally",
             (unsigned long)e);
        dump_state("after uipc");
        break;
    }
    case CMD_UIAUTO0:
    case CMD_UIAUTO1:
        InterlockedExchange(&g_ui_auto, cmd == CMD_UIAUTO1 ? 1 : 0);
        xlog("RESULT uiauto=%ld (1=游戏改回 PC 模式时自动切回触摸)", (long)g_ui_auto);
        break;
    case CMD_FIELDS:
        dump_fields(g_touch_this ? g_touch_this : (g_kb_this ? g_kb_this : g_disp_this), "on-demand");
        break;
    default: xlog("EXEC unknown cmd=%ld", (long)cmd); break;
    }
    xlog("EXEC end cmd=%ld", (long)cmd);
    xflush();
}

static int32_t __attribute__((ms_abi)) hk_fov(void* self, float value)
{
    InterlockedIncrement(&g_frame);
    if (InterlockedCompareExchange(&g_fov_ready, 1, 0) == 0)
        xlog("FOV first call: self=%p value=%.3f", self, (double)value);
    LONG cmd = InterlockedExchange(&g_cmd, CMD_NONE);
    if (cmd != CMD_NONE) run_command(cmd, self);
    return g_orig_fov ? g_orig_fov(self, value) : 0;
}

// ---- 触摸投递核心：接触点状态机 + 槽位分配 + 单条出消息 ----
// 约定：调用者必须持有 g_touch_lock（g_ti_slot / g_ti_busy 内部用 Interlocked，无需锁）。

static int ti_slot_alloc(void)
{
    LONG base = InterlockedIncrement(&g_ti_slot) & (TI_SLOTS - 1);
    for (int i = 0; i < TI_SLOTS; i++) {
        int s = (int)((base + i) & (TI_SLOTS - 1));
        if (InterlockedCompareExchange(&g_ti_busy[s], 1, 0) == 0) return s;
    }
    return (int)base;   /* 全忙（游戏长时间不读）：退回轮转，宁可覆盖也不卡死 */
}

static int ti_slot_index(HTOUCHINPUT h)
{
    const uint8_t* p = (const uint8_t*)(const void*)h;
    const uint8_t* lo = (const uint8_t*)&g_ti_buf[0][0];
    const uint8_t* hi = (const uint8_t*)&g_ti_buf[TI_SLOTS][0];
    if (p < lo || p >= hi) return -1;                       /* 不是我们发的句柄 */
    size_t off = (size_t)(p - lo);
    if (off % sizeof(TOUCHINPUT)) return -1;
    size_t s = off / (sizeof(TOUCHINPUT) * MAX_CONTACTS);
    if (s >= TI_SLOTS) return -1;
    return (int)s;
}

static void ti_slot_free(HTOUCHINPUT h)
{
    int s = ti_slot_index(h);
    if (s >= 0) InterlockedExchange(&g_ti_busy[s], 0);
}

static Contact* contact_find(UINT32 id)
{
    for (int i = 0; i < MAX_CONTACTS; i++)
        if (g_contacts[i].state && g_contacts[i].id == id) return &g_contacts[i];
    return NULL;
}

// active=1：建立或更新接触点；active=0：置为待发 UP。
// 返回 1 = 有需要投递的变化（新建 / 位移 / 抬指），0 = 什么都没变（心跳包走这条，不发消息）。
static int contact_upsert(UINT32 id, int x, int y, int active)
{
    Contact* c = contact_find(id);
    if (c) {
        int moved = (c->x != x || c->y != y);
        if (active) {
            c->x = x; c->y = y;
            if (c->state == 3) { c->state = 2; return 1; }   /* 抬指还没发出去又回来了：按按住+位移处理 */
            return moved ? 1 : 0;
        }
        if (c->state == 3) return 0;
        c->state = 3;
        return 1;
    }
    if (!active) return 0;
    for (int i = 0; i < MAX_CONTACTS; i++) {
        if (!g_contacts[i].state) {
            g_contacts[i].id = id; g_contacts[i].x = x; g_contacts[i].y = y;
            g_contacts[i].ax = x; g_contacts[i].ay = y;   /* 锚点=按下点；新建时 x,y 未缩放即为原始位置 */
            g_contacts[i].state = 1;
            return 1;
        }
    }
    return 0;   /* 10 指占满：丢弃新指（正常手势到不了） */
}

// 把当前全部接触点作为一条 WM_TOUCH 投给游戏。返回投出去的点数（0=没窗口/没接触点）。
// hout 非空时回写这次用的 TOUCHINPUT 数组指针（供 WM_POINTER 桥替换 lParam）。
static UINT emit_touch(const char* tag, int log_detail, HTOUCHINPUT* hout)
{
    if (hout) *hout = NULL;
    if (!g_game_hwnd || !IsWindow(g_game_hwnd)) return 0;
    RECT rc = {0, 0, 0, 0};
    GetClientRect(g_game_hwnd, &rc);
    int w = rc.right > 0 ? rc.right : 1;
    int h = rc.bottom > 0 ? rc.bottom : 1;
    UINT n = 0;
    for (int i = 0; i < MAX_CONTACTS; i++) if (g_contacts[i].state) n++;
    if (!n) return 0;

    int s = ti_slot_alloc();
    TOUCHINPUT* tb = g_ti_buf[s];
    memset(tb, 0, sizeof(g_ti_buf[s]));
    DWORD now = GetTickCount();
    UINT k = 0;
    LONG pmode = InterlockedCompareExchange(&g_primary_mode, 0, 0);
    LONG anchor = InterlockedCompareExchange(&g_primary_id, 0, 0);
    for (int i = 0; i < MAX_CONTACTS; i++) {
        Contact* c = &g_contacts[i];
        if (!c->state) continue;
        int x = c->x < 0 ? 0 : (c->x >= w ? w - 1 : c->x);
        int y = c->y < 0 ? 0 : (c->y >= h ? h - 1 : c->y);
        POINT sp = { x, y };
        /* TOUCHINPUT.x/y 按 MS 口径是**屏幕空间**的百分之一像素（客户端坐标只在窗口位于 0,0 时等价）。
           窗口在 (0,0) 时 ClientToScreen 是恒等变换，逐字节不变；换启动参数/窗口被挪动时不再整体偏移。 */
        if (g_screen_xy && g_game_hwnd) ClientToScreen(g_game_hwnd, &sp);
        TOUCHINPUT* t = &tb[k++];
        t->dwID = c->id;
        t->x = sp.x * 100; t->y = sp.y * 100;         /* 单位 = 0.01 像素 */
        t->dwFlags = (c->state == 1) ? TOUCHEVENTF_DOWN : (c->state == 3 ? TOUCHEVENTF_UP : TOUCHEVENTF_MOVE);
        if (pmode == 2) {
            /* none：完全不置 PRIMARY（mobui26 那种形态） */
        } else if (pmode == 1) {
            if (c->state == 1 && anchor == 0) anchor = (LONG)c->id;   /* 序列第一个 DOWN 定主指 */
            if (anchor != 0 && (LONG)c->id == anchor) t->dwFlags |= TOUCHEVENTF_PRIMARY;
        } else {
            if (k == 1) t->dwFlags |= TOUCHEVENTF_PRIMARY;
        }
        /* dwTime 填的是 GetTickCount（系统时基），所以要显式声明来源 */
        t->dwMask = TOUCHINPUTMASKF_CONTACTAREA | TOUCHINPUTMASKF_TIMEFROMSYSTEM;
        t->cxContact = 8; t->cyContact = 8;
        t->dwTime = now;
        if (c->state == 1) c->state = 2;              /* DOWN 只发一次，之后是 MOVE */
        else if (c->state == 3) {
            c->state = 0;                             /* UP 发完释放接触点 */
            if (anchor != 0 && (LONG)c->id == anchor) anchor = 0;   /* 主指抬起，下个序列重新选 */
        }
    }
    if (pmode == 1) InterlockedExchange(&g_primary_id, anchor);
    BOOL ok = PostMessageW(g_game_hwnd, WM_TOUCH, (WPARAM)k, (LPARAM)(HTOUCHINPUT)tb);
    if (!ok) {
        ti_slot_free((HTOUCHINPUT)tb);
        xlog("TOUCH post[%s] FAILED err=%lu n=%u", tag, (unsigned long)GetLastError(), k);
        return 0;
    }
    if (log_detail) {
        char fl[220]; int off = 0;
        off += _snprintf(fl + off, sizeof(fl) - off, "%s", "");
        for (UINT t = 0; t < k && off < (int)sizeof(fl) - 24; t++)
            off += _snprintf(fl + off, sizeof(fl) - off, " [%lu f=%lx %ld,%ld]", (unsigned long)tb[t].dwID,
                             (unsigned long)tb[t].dwFlags, (long)(tb[t].x / 100), (long)(tb[t].y / 100));
        tlog("POST n=%u ok=1 hwnd=%p%s", k, (void*)g_game_hwnd, fl);
    }
    if (hout) *hout = (HTOUCHINPUT)tb;
    LONG msg = InterlockedIncrement(&g_touch_msg_count);
    if (msg <= 20 || (msg % 200) == 0) xlog("TOUCH post[%s] n=%u slot=%d", tag, k, s);
    return k;
}

static void release_all_contacts(const char* why)
{
    int n = 0;
    EnterCriticalSection(&g_touch_lock);
    for (int i = 0; i < MAX_CONTACTS; i++)
        if (g_contacts[i].state && g_contacts[i].state != 3) { g_contacts[i].state = 3; n++; }
    UINT sent = n ? emit_touch(why, 0, NULL) : 0;
    LeaveCriticalSection(&g_touch_lock);
    xlog("RELEASE[%s]: %d contact(s) -> UP (posted=%u)", why, n, sent);
}

// 把一条 WM_POINTER* 消息翻译成 WM_TOUCH；返回 1 表示已替换 msg
static int translate_pointer_msg(LPMSG msg)
{
    UINT m = msg->message;
    if (m < 0x0245 || m > 0x0249) return 0;
    if (!g_convert_on) return 0;   /* convert=0：不以系统指针消息造手指（net 路径才是真源，避免双源重影） */
    UINT32 id = GET_POINTERID_WPARAM(msg->wParam);
    POINTER_INFO pi;
    if (!GetPointerInfo(id, &pi)) return 0;
    int x = pi.ptPixelLocation.x, y = pi.ptPixelLocation.y;
    int in_contact = (pi.pointerFlags & POINTER_FLAG_INCONTACT) != 0;
    int is_down = (pi.pointerFlags & POINTER_FLAG_DOWN) != 0;
    int is_up = (pi.pointerFlags & POINTER_FLAG_UP) != 0;

    int changed = 0;
    HTOUCHINPUT hh = NULL;
    UINT sent = 0;
    EnterCriticalSection(&g_touch_lock);
    if (m == 0x0246 || is_down) changed = contact_upsert(id, x, y, 1);
    else if (m == 0x0247 || is_up) changed = contact_upsert(id, x, y, 0);
    else if (in_contact) changed = contact_upsert(id, x, y, 1);
    else changed = 0;   /* 悬停(不在接触中)一律不建接触点，否则鼠标一动就有永久的"按住" */
    if (changed) sent = emit_touch("pointer", 0, &hh);
    LeaveCriticalSection(&g_touch_lock);
    if (!changed || !sent) return 0;
    msg->message = WM_TOUCH;            /* 0x0240 */
    msg->wParam = (WPARAM)sent;
    msg->lParam = (LPARAM)hh;
    return 1;
}

static volatile LONG g_pump_on = 0;
static BOOL __attribute__((ms_abi)) hk_PeekMessageW(LPMSG msg, HWND hwnd, UINT min, UINT max, UINT remove)
{
    if (g_pump_on) {
        LONG cmd = InterlockedExchange(&g_cmd, CMD_NONE);
        if (cmd != CMD_NONE) { xlog("PUMP exec cmd=%ld on main thread", (long)cmd); run_command(cmd, NULL); return o_PeekMessageW ? o_PeekMessageW(msg, hwnd, min, max, remove) : 0; }
    }
    BOOL r = o_PeekMessageW ? o_PeekMessageW(msg, hwnd, min, max, remove) : 0;
    LONG n = InterlockedIncrement(&g_pm_seen);
    if (r && msg->message >= 0x0245 && msg->message <= 0x0249) InterlockedIncrement(&g_pm_ptr);
    if (r && n <= 12) xlog("PMW msg=%x hwnd=%p n=%ld", msg->message, (void*)msg->hwnd, (long)n);
    if (r && g_convert_on) translate_pointer_msg(msg);
    return r;
}
static BOOL __attribute__((ms_abi)) hk_PeekMessageA(LPMSG msg, HWND hwnd, UINT min, UINT max, UINT remove)
{
    BOOL r = o_PeekMessageA ? o_PeekMessageA(msg, hwnd, min, max, remove) : 0;
    if (r) translate_pointer_msg(msg);
    return r;
}
static BOOL __attribute__((ms_abi)) hk_GetMessageW(LPMSG msg, HWND hwnd, UINT min, UINT max)
{ InterlockedIncrement(&g_get_seen);
    BOOL r = o_GetMessageW ? o_GetMessageW(msg, hwnd, min, max) : 0;
    if (r > 0) translate_pointer_msg(msg);
    return r;
}
static BOOL __attribute__((ms_abi)) hk_RegisterTouchWindow(HWND hwnd, ULONG flags)
{
    InterlockedIncrement(&g_rtw_called);
    if (g_rtw_called <= 10) xlog("BRIDGE RegisterTouchWindow(hwnd=%p flags=%lx) -> TRUE", hwnd, (unsigned long)flags);
    if (o_RegisterTouchWindow) o_RegisterTouchWindow(hwnd, flags);
    return TRUE;
}
static BOOL __attribute__((ms_abi)) hk_GetTouchInputInfo(HTOUCHINPUT h, UINT count, PTOUCHINPUT out, int size)
{
    InterlockedIncrement(&g_ti_called);
    int s = ti_slot_index(h);
    if (s < 0) {
        /* 不是我们投的句柄（Wine 自己合成的 WM_TOUCH 等）：交回原实现，别把外部句柄当数组读 */
        InterlockedIncrement(&g_stat_gti_foreign);
        if (o_GetTouchInputInfo) return o_GetTouchInputInfo(h, count, out, size);
        return FALSE;
    }
    if (!out) return FALSE;
    if (count > MAX_CONTACTS) count = MAX_CONTACTS;
    TOUCHINPUT* src = g_ti_buf[s];
    for (UINT i = 0; i < count; i++) out[i] = src[i];
    LONG gn = InterlockedIncrement(&g_stat_gti);
    if (gn <= 300 || (gn % 20) == 0) {
        char ids2[160]; int off = 0; off += _snprintf(ids2+off, sizeof(ids2)-off, "%s", "");
        for (UINT t = 0; t < count && off < (int)sizeof(ids2) - 20; t++)
            off += _snprintf(ids2+off, sizeof(ids2)-off, " [%lu f=%lx]", (unsigned long)src[t].dwID, (unsigned long)src[t].dwFlags);
        tlog("GTI count=%u%s", count, ids2);
    }
    if (g_ti_called <= 20) xlog("BRIDGE GetTouchInputInfo(count=%u size=%d) -> TRUE", count, size);
    return TRUE;
}
static BOOL __attribute__((ms_abi)) hk_CloseTouchInputHandle(HTOUCHINPUT h)
{
    int s = ti_slot_index(h);
    if (s < 0) return o_CloseTouchInputHandle ? o_CloseTouchInputHandle(h) : TRUE;
    InterlockedExchange(&g_ti_busy[s], 0);   /* 游戏读完了，槽位可以回收 */
    return TRUE;
}

static volatile LONG g_suppress_mouse;
static int g_suppress_level = 1;
static int g_net_enabled = 0;
static int g_net_port = 51721;
static volatile LONG g_net_pkts, g_net_contacts_sent;
static SHORT __attribute__((ms_abi)) hk_GetAsyncKeyState(int vk)
{
    if (g_suppress_mouse && is_mouse_vk(vk)) { InterlockedIncrement(&g_mouse_suppressed_calls); return 0; }
    return o_GetAsyncKeyState ? o_GetAsyncKeyState(vk) : 0;
}
static SHORT __attribute__((ms_abi)) hk_GetKeyState(int vk)
{
    if (g_suppress_mouse && is_mouse_vk(vk)) return 0;
    return o_GetKeyState ? o_GetKeyState(vk) : 0;
}
static BOOL __attribute__((ms_abi)) hk_GetCursorPos(LPPOINT pt)
{
    if (g_suppress_mouse) { if (pt) { pt->x = 0; pt->y = 0; } return TRUE; }
    return o_GetCursorPos ? o_GetCursorPos(pt) : FALSE;
}
static UINT __attribute__((ms_abi)) hk_GetRawInputData(HRAWINPUT h, UINT cmd, LPVOID data, PUINT size, UINT hdr)
{
    UINT r = o_GetRawInputData ? o_GetRawInputData(h, cmd, data, size, hdr) : (UINT)-1;
    if (!g_suppress_mouse || r == (UINT)-1 || !data) return r;
    if (cmd != RID_INPUT || !size || *size < sizeof(RAWINPUT)) return r;   // RID_HEADER/探测调用一律放过，避免写越界
    RAWINPUT* ri = (RAWINPUT*)data;
    if (ri->header.dwType == RIM_TYPEMOUSE) {
        /* 把鼠标原始包改成"无变化"，Unity 侧就不会产生鼠标输入 */
        if (ri->data.mouse.usFlags & MOUSE_MOVE_ABSOLUTE) { ri->data.mouse.lLastX = 0; ri->data.mouse.lLastY = 0; }
        else { ri->data.mouse.lLastX = 0; ri->data.mouse.lLastY = 0; }
        ri->data.mouse.usButtonFlags = 0;
        ri->data.mouse.ulRawButtons = 0;
    }
    return r;
}
static int __attribute__((ms_abi)) hk_gsm(int idx)
{
    LONG n = InterlockedIncrement(&g_gsm_calls);
    int v;
    if (idx == 94) v = 0xC3;        // SM_DIGITIZER: INTEGRATED_TOUCH|READY|MULTI_INPUT
    else if (idx == 95) v = 10;     // SM_MAXIMUMTOUCHES
    else if (idx == 86) v = 1;      // SM_TABLETPC
    else return g_orig_gsm ? g_orig_gsm(idx) : 0;
    if (n <= 60) xlog("GSM spoof idx=%d -> %d (call #%ld)", idx, v, (long)n);
    return v;
}

static int32_t __attribute__((ms_abi)) hk_platform(void)
{
    InterlockedIncrement(&g_frame);   // 复用同一帧计数当"跳板心跳"
    if (!g_pump_on) {
        LONG cmd = InterlockedExchange(&g_cmd, CMD_NONE);
        if (cmd != CMD_NONE) run_command(cmd, NULL);
    }
    return g_orig_platform ? g_orig_platform() : 2;
}

static void __attribute__((ms_abi)) hk_touch(void* self)
{
    LONG n = InterlockedIncrement(&g_hook_touch_hits);
    if (n <= 8 || (n % 200) == 0)
        xlog("HOOK touch self=%p flagA=%02x hit=%ld", self, (unsigned)rd8(at(kRvaFlagA)), (long)n);
    if (self && !g_touch_this) { g_touch_this = self; xlog("CAPTURE touch this=%p", self); }
    if (g_orig_touch) g_orig_touch(self);
}

static void __attribute__((ms_abi)) hk_kb(void* self)
{
    LONG n = InterlockedIncrement(&g_hook_kb_hits);
    if (n <= 8 || (n % 200) == 0) xlog("HOOK keyboard self=%p hit=%ld", self, (long)n);
    if (self && !g_kb_this) { g_kb_this = self; xlog("CAPTURE keyboard this=%p", self); }
    if (g_orig_kb) g_orig_kb(self);
}

static void __attribute__((ms_abi)) hk_disp(void* self, int mode)
{
    LONG n = InterlockedIncrement(&g_hook_disp_hits);
    if (n <= 12 || (n % 100) == 0) {
        xlog("HOOK dispatcher self=%p mode=%d hit=%ld", self, mode, (long)n);
        if (n <= 12 && self) dump_fields(self, "dispatcher-hit");
    }
    if (self && !g_disp_this) { g_disp_this = self; xlog("CAPTURE dispatcher this=%p mode=%d", self, mode); }
    if (g_orig_disp) g_orig_disp(self, mode);
}

// ---- 配置（<dll>.cfg，注入前可改，免得重编译） ----
static void load_cfg(void)
{
    HANDLE f = CreateFileW(g_cfgpath, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE) { xlog("CONFIG 无 cfg 文件，用默认值"); return; }
    char buf[2048];
    DWORD got = 0;
    if (ReadFile(f, buf, sizeof(buf) - 1, &got, NULL)) {
        buf[got] = 0;
        char* ctx = buf;
        for (char* line = buf; line && *line; line = ctx) {
            char* nl = strchr(line, '\n');
            if (nl) { *nl = 0; ctx = nl + 1; } else ctx = NULL;
            char* eq = strchr(line, '=');
            if (!eq) continue;
            *eq = 0;
            const char* k = line;
            const char* v = eq + 1;
            while (*k == ' ' || *k == '\t') k++;
            while (*v == ' ' || *v == '\t') v++;
            if (!strcmp(k, "hooks")) g_hook_mask = (unsigned)strtoul(v, NULL, 0);
            else if (!strcmp(k, "veh")) g_use_veh = strtoul(v, NULL, 0) ? 1 : 0;
            else if (!strcmp(k, "map")) g_map_enabled = strtoul(v, NULL, 0) ? 1 : 0;
            else if (!strcmp(k, "suppress")) g_suppress_level = (int)strtol(v, NULL, 0);
            else if (!strcmp(k, "net")) g_net_enabled = strtoul(v, NULL, 0) ? 1 : 0;
            else if (!strcmp(k, "test")) g_test_mode = strtoul(v, NULL, 0) ? 1 : 0;
            else if (!strcmp(k, "pump")) g_pump_on = strtoul(v, NULL, 0) ? 1 : 0;
            else if (!strcmp(k, "convert")) g_convert_on = strtoul(v, NULL, 0) ? 1 : 0;
            else if (!strcmp(k, "screenxy")) g_screen_xy = strtoul(v, NULL, 0) ? 1 : 0;
            else if (!strcmp(k, "focusgate")) g_focus_gate = strtoul(v, NULL, 0) ? 1 : 0;
            else if (!strcmp(k, "primary")) g_primary_mode = !strncmp(v, "anchor", 6) ? 1 : (!strncmp(v, "none", 4) ? 2 : 0);
            else if (!strcmp(k, "lookgain")) { long gv = strtol(v, NULL, 0); if (gv < 5) gv = 5; if (gv > 400) gv = 400; g_look_gain = gv; }
            else if (!strcmp(k, "net_port")) g_net_port = (int)strtol(v, NULL, 0);
            else if (!strcmp(k, "joy_cx")) g_joy_cx = (int)strtol(v, NULL, 0);
            else if (!strcmp(k, "joy_cy")) g_joy_cy = (int)strtol(v, NULL, 0);
            else if (!strcmp(k, "joy_r")) g_joy_r = (int)strtol(v, NULL, 0);
            else if (!strcmp(k, "cam_ax")) g_cam_ax = (int)strtol(v, NULL, 0);
            else if (!strcmp(k, "cam_ay")) g_cam_ay = (int)strtol(v, NULL, 0);
        }
    }
    CloseHandle(f);
}

// ---- 命令文件 ----
static LONG cmd_from_token(const char* t)
{
    if (!strcmp(t, "dump")) return CMD_DUMP;
    if (!strcmp(t, "ping")) return CMD_PING;
    if (!strcmp(t, "touch_null")) return CMD_TOUCH_NULL;
    if (!strcmp(t, "touch_c")) return CMD_TOUCH_CAPTURED;
    if (!strcmp(t, "disp0")) return CMD_DISPATCH0;
    if (!strcmp(t, "kb_c")) return CMD_KB_CAPTURED;
    if (!strcmp(t, "flipA")) return CMD_FLIP_A;
    if (!strcmp(t, "fields")) return CMD_FIELDS;
    if (!strcmp(t, "field0")) return CMD_FIELD0;
    if (!strcmp(t, "setters")) return CMD_SETTERS;
    if (!strcmp(t, "setter1")) return CMD_SETTER1;
    if (!strcmp(t, "setter2")) return CMD_SETTER2;
    if (!strcmp(t, "touch_e60")) return CMD_TOUCH_E60;
    if (!strcmp(t, "touch_e2c0")) return CMD_TOUCH_E2C0;
    if (!strcmp(t, "ui0")) return CMD_UI0;
    if (!strcmp(t, "getter")) return CMD_GETTER;
    if (!strcmp(t, "touch_a070")) return CMD_TOUCH_A070;
    if (!strcmp(t, "touch_gate")) return CMD_TOUCH_GATE;
    if (!strcmp(t, "touchsel")) return CMD_TOUCHSEL;
    if (!strcmp(t, "release")) return CMD_RELEASE;
    if (!strcmp(t, "uipc")) return CMD_UIPC;
    if (!strcmp(t, "uiauto0")) return CMD_UIAUTO0;
    if (!strcmp(t, "uiauto1")) return CMD_UIAUTO1;
    return -1;
}

static void poll_cmds(void)
{
    HANDLE f = CreateFileW(g_cmdpath, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE) return;
    char buf[8192];
    DWORD got = 0;
    BOOL ok = ReadFile(f, buf, sizeof(buf) - 1, &got, NULL);
    CloseHandle(f);
    if (!ok) return;
    buf[got] = 0;

    unsigned total = 0;
    char* p = buf;
    while (*p) {
        char* e = strchr(p, '\n');
        if (e) *e = 0;
        total++;
        if (*p && *p != '#') {
            if (total > g_cmd_lines_done) {
                /* 运行时可调参数（不走 g_cmd，直接改原子量，立刻生效） */
                if (!strncmp(p, "gain ", 5)) {
                    long gv = strtol(p + 5, NULL, 0);
                    if (gv < 5) gv = 5; if (gv > 400) gv = 400;
                    InterlockedExchange(&g_look_gain, gv);
                    xlog("CMDFILE gain=%ld%% (运行时生效，无需重启)", gv);
                    goto next_line;
                }
                if (!strncmp(p, "primary ", 8)) {
                    const char* pv = p + 8;
                    LONG pm = !strncmp(pv, "anchor", 6) ? 1 : (!strncmp(pv, "none", 4) ? 2 : 0);
                    InterlockedExchange(&g_primary_mode, pm);
                    InterlockedExchange(&g_primary_id, 0);
                    xlog("CMDFILE primary=%s (运行时生效，无需重启)",
                         pm == 1 ? "anchor" : (pm == 2 ? "none" : "first"));
                    goto next_line;
                }
                {
                LONG code = cmd_from_token(p);
                if (code > 0) {
                    if (InterlockedCompareExchange(&g_cmd, 0, 0) != CMD_NONE)
                        xlog("CMDFILE busy, dropped: %s", p);
                    else {
                        InterlockedExchange(&g_cmd, code);
                        xlog("CMDFILE queued: %s", p);
                    }
                } else {
                    xlog("CMDFILE unknown token: %s", p);
                }
                }
            }
next_line:;
        }
        if (!e) break;
        p = e + 1;
    }
    if (total < g_cmd_lines_done) g_cmd_lines_done = 0;
    else g_cmd_lines_done = total;
}

// ---- 工作线程 ----
// MinHook 初始化（幂等）：install_hooks 与 test 模式都要用，谁先跑谁初始化。
// 教训：原先 MH_Initialize 只写在 install_hooks 里，test 模式绕过它 → 三个 user32 钩子全 FAILED。
static int ensure_mh(void)
{
    static volatile LONG done = 0;
    if (InterlockedCompareExchange(&done, 1, 0) == 0) {
        if (MH_Initialize() != MH_OK) { InterlockedExchange(&done, 0); xlog("MinHook init failed"); return 0; }
    }
    return 1;
}

// user32 侧钩子（指针消息桥 + 触摸 API）：只挂缺省导出，不碰游戏自身地址，
// 因此可以在 test 模式下单独装载（离线验收台就是靠它把 WM_TOUCH 喂给接收端）。
static int install_user32_hooks(void)
{
    if (!ensure_mh()) return 0;
    HMODULE u32 = LoadLibraryW(L"user32.dll");
    static const struct { const char* name; void* det; void** orig; unsigned bit; } tbl[] = {
        {"PeekMessageW", (void*)&hk_PeekMessageW, (void**)&o_PeekMessageW, 0x40},
        {"PeekMessageA", (void*)&hk_PeekMessageA, (void**)&o_PeekMessageA, 0x40},
        {"GetMessageW", (void*)&hk_GetMessageW, (void**)&o_GetMessageW, 0x40},
        {"RegisterTouchWindow", (void*)&hk_RegisterTouchWindow, (void**)&o_RegisterTouchWindow, 0x80},
        {"GetTouchInputInfo", (void*)&hk_GetTouchInputInfo, (void**)&o_GetTouchInputInfo, 0x80},
        {"CloseTouchInputHandle", (void*)&hk_CloseTouchInputHandle, (void**)&o_CloseTouchInputHandle, 0x80},
    };
    int okc = 0;
    for (unsigned i = 0; i < sizeof(tbl)/sizeof(tbl[0]); i++) {
        if (!(g_hook_mask & tbl[i].bit)) continue;
        void* p = u32 ? (void*)GetProcAddress(u32, tbl[i].name) : NULL;
        if (!p) { xlog("BRIDGE missing %s", tbl[i].name); continue; }
        if (MH_CreateHook(p, tbl[i].det, tbl[i].orig) == MH_OK && MH_EnableHook(p) == MH_OK) {
            okc++;
            xlog("BRIDGE hooked %s", tbl[i].name);
        } else {
            xlog("BRIDGE hook FAILED %s", tbl[i].name);
        }
    }
    xlog("BRIDGE touch plumbing ready (%d hooks; mask=0x%x)", okc, (unsigned)g_hook_mask);
    return okc;
}

static int install_hooks(void)
{
    static const uint8_t touch_pro[] = {0x56, 0x57, 0x48, 0x83, 0xEC, 0x28, 0x48, 0x89, 0xCE, 0x80, 0x3D};
    static const uint8_t kb_pro[] = {0x56, 0x57, 0x48, 0x83, 0xEC, 0x28, 0x48, 0x89, 0xCE, 0x80, 0x3D};
    static const uint8_t disp_pro[] = {0x56, 0x57, 0x53, 0x48, 0x83, 0xEC, 0x20, 0x89, 0xD6, 0x48, 0x89, 0xCF};
    static const uint8_t fov_pro[] = {0x40, 0x53, 0x48, 0x83, 0xEC, 0x60};

    xlog("VALIDATE touch    %s", bytes_are(at(kRvaTouch), touch_pro, sizeof(touch_pro)) ? "ok" : "MISMATCH");
    xlog("VALIDATE keyboard %s", bytes_are(at(kRvaKeyboard), kb_pro, sizeof(kb_pro)) ? "ok" : "MISMATCH");
    xlog("VALIDATE dispatch %s", bytes_are(at(kRvaDispatcher), disp_pro, sizeof(disp_pro)) ? "ok" : "MISMATCH");
    xlog("VALIDATE fov      %s", bytes_are(at(kRvaChangeFov), fov_pro, sizeof(fov_pro)) ? "ok" : "MISMATCH");

    xlog("CONFIG hook_mask=0x%x (1=fov 2=touch 4=kb 8=disp) veh=%d", g_hook_mask, g_use_veh);
    if (!ensure_mh()) return 0;
    if (!(g_hook_mask & 1)) { xlog("ChangeFOV hook SKIPPED by config"); }
    else
    if (MH_CreateHook((void*)(uintptr_t)at(kRvaChangeFov), (void*)&hk_fov, (void**)&g_orig_fov) == MH_OK &&
        MH_EnableHook((void*)(uintptr_t)at(kRvaChangeFov)) == MH_OK) {
        InterlockedExchange(&g_fov_hooked, 1);
        xlog("ChangeFOV hook installed at %p", (void*)(uintptr_t)at(kRvaChangeFov));
    } else {
        xlog("ChangeFOV hook FAILED -> commands will run on worker thread (thread affinity risk)");
    }
    if (g_hook_mask & 2) {
    if (MH_CreateHook((void*)(uintptr_t)at(kRvaTouch), (void*)&hk_touch, (void**)&g_orig_touch) == MH_OK &&
        MH_EnableHook((void*)(uintptr_t)at(kRvaTouch)) == MH_OK)
        xlog("touch hook installed");
    else
        xlog("touch hook FAILED");
    } else xlog("touch hook SKIPPED by config");
    if (g_hook_mask & 4) {
    if (MH_CreateHook((void*)(uintptr_t)at(kRvaKeyboard), (void*)&hk_kb, (void**)&g_orig_kb) == MH_OK &&
        MH_EnableHook((void*)(uintptr_t)at(kRvaKeyboard)) == MH_OK)
        xlog("keyboard hook installed");
    else
        xlog("keyboard hook FAILED");
    } else xlog("keyboard hook SKIPPED by config");
    if (g_hook_mask & 0x10) {
        static const uint8_t plat_jmp[] = {0xE9};
        static const uint8_t plat_ret[] = {0xB8, 0x02, 0x00, 0x00, 0x00, 0xC3};
        if (bytes_are(at(kRvaPlatform), plat_jmp, 1) || bytes_are(at(kRvaPlatform), plat_ret, sizeof(plat_ret))) {
            if (MH_CreateHook((void*)(uintptr_t)at(kRvaPlatform), (void*)&hk_platform, (void**)&g_orig_platform) == MH_OK &&
                MH_EnableHook((void*)(uintptr_t)at(kRvaPlatform)) == MH_OK) {
                InterlockedExchange(&g_fov_hooked, 1);   // 有跳板就不走 worker 兜底
                xlog("platform-getter trampoline installed at %p", (void*)(uintptr_t)at(kRvaPlatform));
            } else {
                xlog("platform-getter hook FAILED");
            }
        } else {
            xlog("platform-getter prologue unrecognized: %02x %02x %02x %02x %02x",
                 (unsigned)rd8(at(kRvaPlatform)), (unsigned)rd8(at(kRvaPlatform) + 1),
                 (unsigned)rd8(at(kRvaPlatform) + 2), (unsigned)rd8(at(kRvaPlatform) + 3),
                 (unsigned)rd8(at(kRvaPlatform) + 4));
        }
    } else xlog("platform-getter hook SKIPPED by config");

    if (g_hook_mask & 0x20) {
        HMODULE u32 = LoadLibraryW(L"user32.dll");
        void* gsm = u32 ? (void*)GetProcAddress(u32, "GetSystemMetrics") : NULL;
        if (gsm && MH_CreateHook(gsm, (void*)&hk_gsm, (void**)&g_orig_gsm) == MH_OK &&
            MH_EnableHook(gsm) == MH_OK)
            xlog("GetSystemMetrics spoof installed (SM_DIGITIZER=0xC3, SM_MAXIMUMTOUCHES=10)");
        else
            xlog("GetSystemMetrics spoof FAILED (gsm=%p)", gsm);
    } else xlog("GetSystemMetrics spoof SKIPPED by config");

    if (g_hook_mask & 0x100) {
        HMODULE u32m = LoadLibraryW(L"user32.dll");
        int okm = 0;
        struct { const char* n; void* d; void** o; } mh[] = {
            {"GetAsyncKeyState", (void*)&hk_GetAsyncKeyState, (void**)&o_GetAsyncKeyState},
            {"GetKeyState", (void*)&hk_GetKeyState, (void**)&o_GetKeyState},
            {"GetCursorPos", (void*)&hk_GetCursorPos, (void**)&o_GetCursorPos},
            {"GetRawInputData", (void*)&hk_GetRawInputData, (void**)&o_GetRawInputData},
        };
        for (unsigned i = 0; i < sizeof(mh)/sizeof(mh[0]); i++) {
            if (i == 3 && g_suppress_level < 2) { xlog("MOUSE-SUPPRESS skip GetRawInputData (level<2)"); continue; }
            void* p = u32m ? (void*)GetProcAddress(u32m, mh[i].n) : NULL;
            if (p && MH_CreateHook(p, mh[i].d, mh[i].o) == MH_OK && MH_EnableHook(p) == MH_OK) okm++;
            else xlog("MOUSE-SUPPRESS hook failed %s (%p)", mh[i].n, p);
        }
        InterlockedExchange(&g_suppress_mouse, g_suppress_level > 0 ? 1 : 0);
        xlog("MOUSE-SUPPRESS enabled (%d/4 hooks, level=%d)", okm, g_suppress_level);
    } else xlog("MOUSE-SUPPRESS skipped by config");

    if (g_hook_mask & (0x40 | 0x80)) {
        install_user32_hooks();
    } else xlog("BRIDGE skipped by config");

    if (g_hook_mask & 8) {
    if (MH_CreateHook((void*)(uintptr_t)at(kRvaDispatcher), (void*)&hk_disp, (void**)&g_orig_disp) == MH_OK &&
        MH_EnableHook((void*)(uintptr_t)at(kRvaDispatcher)) == MH_OK)
        xlog("dispatcher hook installed");
    else
        xlog("dispatcher hook FAILED");
    } else xlog("dispatcher hook SKIPPED by config");
    return 1;
}

// ---- 外部触摸注入：读 .tcmds，直接给游戏窗口投 WM_TOUCH（绕开 Wine 触摸投递） ----
static void poll_touch_cmds(void)
{
    HANDLE f = CreateFileW(g_touchcmdpath, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE) return;
    char buf[8192];
    DWORD got = 0;
    ReadFile(f, buf, sizeof(buf) - 1, &got, NULL);
    CloseHandle(f);
    buf[got] = 0;
    unsigned total = 0;
    char* p = buf;
    int changed = 0;
    struct TInj { int kind, id, x, y; } pending[16];
    int pend_n = 0;
    memset(pending, 0, sizeof(pending));
    while (*p) {
        char* e = strchr(p, '\n');
        if (e) *e = 0;
        total++;
        if (total > (unsigned)g_touchcmd_lines && *p && *p != '#') {
            int id = 0, x = 0, y = 0;
            if (sscanf(p, "down %d %d %d", &id, &x, &y) == 3) { pending[pend_n].kind = 1; pending[pend_n].id = id; pending[pend_n].x = x; pending[pend_n].y = y; pend_n++; changed = 1; }
            else if (sscanf(p, "move %d %d %d", &id, &x, &y) == 3) { pending[pend_n].kind = 1; pending[pend_n].id = id; pending[pend_n].x = x; pending[pend_n].y = y; pend_n++; changed = 1; }
            else if (sscanf(p, "up %d", &id) == 1) { pending[pend_n].kind = 0; pending[pend_n].id = id; pending[pend_n].x = 0; pending[pend_n].y = 0; pend_n++; changed = 1; }
            if (pend_n >= 16) break;
        }
        if (!e) break;
        p = e + 1;
    }
    if (total < (unsigned)g_touchcmd_lines) g_touchcmd_lines = 0;
    else g_touchcmd_lines = total;
    if (!changed || !g_game_hwnd) return;

    EnterCriticalSection(&g_touch_lock);
    UINT n = 0;
    for (int i = 0; i < pend_n; i++)
        n += (UINT)contact_upsert((UINT32)pending[i].id, pending[i].x, pending[i].y, pending[i].kind);
    UINT sent = n ? emit_touch("tcmds", 0, NULL) : 0;
    LeaveCriticalSection(&g_touch_lock);
    if (sent && InterlockedIncrement(&g_touch_msg_count) <= 20) xlog("TINJ WM_TOUCH n=%u hwnd=%p", sent, (void*)g_game_hwnd);
}

static int cursor_in_game(int* px, int* py)
{
    POINT pt;
    if (!GetCursorPos(&pt)) return 0;
    if (g_game_hwnd) ScreenToClient(g_game_hwnd, &pt);
    *px = pt.x; *py = pt.y;
    return 1;
}

// 接触点编号约定：1=左键(点按/拖动) 2=镜头(右键) 3=摇杆(WASD) 4/5=双指捏合(Shift+左键)
static int key_down(int vk)   // 用原始 API：我们已把鼠标键屏蔽，键盘照旧
{
    if (is_mouse_vk(vk)) return o_GetAsyncKeyState ? (o_GetAsyncKeyState(vk) & 0x8000) != 0 : 0;
    return o_GetAsyncKeyState ? (o_GetAsyncKeyState(vk) & 0x8000) != 0 : 0;
}
static void get_cursor_orig(int* px, int* py)
{
    POINT pt = {0, 0};
    if (o_GetCursorPos) o_GetCursorPos(&pt);
    if (g_game_hwnd) ScreenToClient(g_game_hwnd, &pt);
    *px = pt.x; *py = pt.y;
}
static void map_emit(void)
{
    int mx = 0, my = 0;
    get_cursor_orig(&mx, &my);
    static int joy_on = 0, cam_on = 0, cam_sx = 0, cam_sy = 0, pinch_on = 0, px0 = 0, py0 = 0, tap_on = 0;
    int changed = 0;

    EnterCriticalSection(&g_touch_lock);

    // 摇杆：WASD（先按在中心，再拖到方向位——真机触屏就是这么用的）
    int dx = 0, dy = 0;
    if (key_down('W')) dy -= 1;
    if (key_down('S')) dy += 1;
    if (key_down('A')) dx -= 1;
    if (key_down('D')) dx += 1;
    if (dx || dy) {
        if (!joy_on) { changed |= contact_upsert(3, g_joy_cx, g_joy_cy, 1); joy_on = 1; }
        changed |= contact_upsert(3, g_joy_cx + dx * g_joy_r, g_joy_cy + dy * g_joy_r, 1);
    } else if (joy_on) {
        changed |= contact_upsert(3, g_joy_cx, g_joy_cy, 0);
        joy_on = 0;
    }

    // 镜头：右键按住 = 右侧锚点 + 位移
    if (key_down(VK_RBUTTON)) {
        if (!cam_on) { cam_sx = mx; cam_sy = my; cam_on = 1; changed |= contact_upsert(2, g_cam_ax, g_cam_ay, 1); }
        changed |= contact_upsert(2, g_cam_ax + (mx - cam_sx), g_cam_ay + (my - cam_sy), 1);
    } else if (cam_on) {
        changed |= contact_upsert(2, g_cam_ax, g_cam_ay, 0);
        cam_on = 0;
    }

    // 双指捏合：Shift + 左键 —— 以按下点为中心，两根手指对称张开
    int shift = key_down(VK_SHIFT) || key_down(VK_LSHIFT) || key_down(VK_RSHIFT);
    if (shift && key_down(VK_LBUTTON)) {
        if (!pinch_on) { px0 = mx; py0 = my; pinch_on = 1; }
        int ddx = mx - px0, ddy = my - py0;
        changed |= contact_upsert(4, px0 - ddx, py0 - ddy, 1);
        changed |= contact_upsert(5, px0 + ddx, py0 + ddy, 1);
    } else if (pinch_on) {
        changed |= contact_upsert(4, px0, py0, 0);
        changed |= contact_upsert(5, px0, py0, 0);
        pinch_on = 0;
    }

    // 单指：左键（不按 Shift 时）—— 点按与拖动都走这条，抬起一定发 UP
    if (key_down(VK_LBUTTON) && !shift) {
        changed |= contact_upsert(1, mx, my, 1);
        tap_on = 1;
    } else if (tap_on) {
        changed |= contact_upsert(1, mx, my, 0);
        tap_on = 0;
    }

    UINT sent = changed ? emit_touch("map", 0, NULL) : 0;
    LeaveCriticalSection(&g_touch_lock);
    if (!sent) return;
    LONG k = InterlockedIncrement(&g_map_sent);
    if (k <= 60) xlog("MAP WM_TOUCH n=%u", sent);
}

// ---- 网络触摸：收 touchd 的 UDP 包，逐包转成 WM_TOUCH（真实触摸，非映射） ----
// 协议 v2：'T'(0x54) | 0x4D 魔数 | cnt | cnt×(u16 id, u16 nx, u16 ny) 小端；总长 3+6*cnt。
// 魔数是为了让"协议不符"变成明确拒收，而不是把偏移错误解析成一堆假手指（v1 的老 bug）。
static void net_apply(const unsigned char* buf, int len)
{
    if (len < 3 || buf[0] != 'T') return;
    if (buf[1] != 0x4D) {
        LONG bad = InterlockedIncrement(&g_proto_bad);
        if (bad <= 3 || (bad % 200) == 0)
            xlog("NET proto reject: magic=0x%02x len=%d (expect 'T',0x4D —— 请重编并重启 tools/touchd)", (unsigned)buf[1], len);
        return;
    }
    int cnt = (int)buf[2];
    if (cnt > MAX_CONTACTS) {
        LONG bad = InterlockedIncrement(&g_proto_bad);
        if (bad <= 3) xlog("NET proto reject: cnt=%d > %d", cnt, MAX_CONTACTS);
        return;
    }
    if (len < 3 + cnt * 6) {
        LONG bad = InterlockedIncrement(&g_proto_bad);
        if (bad <= 3) xlog("NET proto reject: len=%d < %d", len, 3 + cnt * 6);
        return;
    }
    InterlockedExchange(&g_net_last_pkt, (LONG)GetTickCount());

    /* 焦点门闩：游戏不是前台窗口时整包丢弃，并抬一次全部接触点。
       否则在浏览器或别的窗口上的触摸也会打进游戏（2026-11-19 用户实测）。 */
    if (g_focus_gate) {
        HWND fg = GetForegroundWindow();
        DWORD fpid = 0;
        if (fg) GetWindowThreadProcessId(fg, &fpid);
        if (!fg || fpid != GetCurrentProcessId()) {
            if (!InterlockedExchange(&g_focus_blocked, 1)) {
                xlog("FOCUS 游戏不在前台 -> 暂停投递");
                release_all_contacts("focus-lost");
            }
            return;
        }
        if (InterlockedExchange(&g_focus_blocked, 0)) xlog("FOCUS 回到前台 -> 恢复投递");
    }

    RECT rc = {0, 0, 1, 1};
    if (g_game_hwnd) GetClientRect(g_game_hwnd, &rc);
    int w = rc.right - rc.left, h = rc.bottom - rc.top;
    if (w <= 0) w = 1;
    if (h <= 0) h = 1;

    UINT32 ids[MAX_CONTACTS]; int xs[MAX_CONTACTS], ys[MAX_CONTACTS];
    for (int i = 0; i < cnt; i++) {
        unsigned short id, nx, ny;
        memcpy(&id, buf + 3 + i * 6, 2);
        memcpy(&nx, buf + 3 + i * 6 + 2, 2);
        memcpy(&ny, buf + 3 + i * 6 + 4, 2);
        ids[i] = 1000u + (UINT32)(id & 0x3FF);       /* 与映射手指(1..5)分域，永不冲突 */
        xs[i] = (int)((long)nx * w / 65535);
        ys[i] = (int)((long)ny * h / 65535);
    }

    int changed = 0;
    EnterCriticalSection(&g_touch_lock);
    /* 本包里没出现的旧 net 接触点 → 抬指。返回值必须并进 changed，否则"纯抬指包"会被整包丢掉 */
    for (int i = 0; i < MAX_CONTACTS; i++) {
        Contact* c = &g_contacts[i];
        if (!c->state || c->id < 1000) continue;
        int still = 0;
        for (int j = 0; j < cnt; j++) if (ids[j] == c->id) { still = 1; break; }
        if (!still) {
            if (contact_upsert(c->id, c->x, c->y, 0)) { changed = 1; InterlockedIncrement(&g_stat_up); }
        }
    }
    for (int j = 0; j < cnt; j++) {
        Contact* c = contact_find(ids[j]);
        int existed = c != NULL;
        int tx = xs[j], ty = ys[j];
        LONG gain = g_look_gain;
        if (gain != 100 && cnt == 1 && existed && c->state && gain > 0) {
            /* 增益只作用于「单指拖拽」（相机视角/摇杆）。多指手势（双指捏合/旋转）必须原样透传：
               缩放位移后再叠整数截断，会把手指每秒几十像素的慢速相向移动算成 0，
               表现就是「捏合没反应」。2026-11-19 实测：mobui27（无增益）捏合正常，
               mobui28/29（lookgain=50）失效 → 这条是回归根因之一。 */
            /* 增益以「按下点」为锚做绝对缩放：tx = ax + (raw - ax) * gain / 100。
               这里绝不能拿 c->x（上一次发出的位置）当基准——那是递归滤波，每帧只走剩余位移的
               gain%，摇杆方向与弓箭瞄准都会指数滞后，手感表现为「按下就定住、方向改不动」。
               2026-11-19 用户实测：左摇杆按下后方向固定、长按攻击键不能拖动瞄准。 */
            int dx = xs[j] - c->ax, dy = ys[j] - c->ay;
            tx = c->ax + (int)(((long)dx * gain + (dx > 0 ? 50 : -50)) / 100);
            ty = c->ay + (int)(((long)dy * gain + (dy > 0 ? 50 : -50)) / 100);
            if (tx < 0) tx = 0; if (tx > w - 1) tx = w - 1;
            if (ty < 0) ty = 0; if (ty > h - 1) ty = h - 1;
        }
        if (contact_upsert(ids[j], tx, ty, 1)) { changed = 1; if (!existed) InterlockedIncrement(&g_stat_down); }
    }
    UINT sent = changed ? emit_touch("net", 1, NULL) : 0;
    LeaveCriticalSection(&g_touch_lock);
    if (!changed) return;                        /* 心跳包（位置没变）：不发消息也不刷日志 */

    LONG rxn = InterlockedIncrement(&g_stat_rx);
    if (rxn <= 300 || (rxn % 20) == 0) {
        char pts[200]; int off = 0; off += _snprintf(pts+off, sizeof(pts)-off, "%s", "");
        for (int j = 0; j < cnt && off < (int)sizeof(pts) - 24; j++)
            off += _snprintf(pts+off, sizeof(pts)-off, " [%u %d,%d]", ids[j], xs[j], ys[j]);
        tlog("RX n=%d win=%dx%d%s", cnt, w, h, pts);
    }
    if (sent) {
        InterlockedIncrement(&g_stat_post);
        if (InterlockedIncrement(&g_net_pkts) <= 20) xlog("NET WM_TOUCH n=%u (from touchd)", sent);
    }
}

static DWORD WINAPI net_thread(LPVOID u)
{
    (void)u;
    static int wsa_ready = 0;
    if (!wsa_ready) {
        WSADATA wsa;
        int wr = WSAStartup(MAKEWORD(2, 2), &wsa);
        if (wr != 0) { xlog("NET WSAStartup failed err=%d", wr); return 1; }
        wsa_ready = 1;
    }
    SOCKET sk = socket(AF_INET, SOCK_DGRAM, 0);
    if (sk == INVALID_SOCKET) { xlog("NET socket failed err=%d", WSAGetLastError()); return 1; }
    struct sockaddr_in a = {0};
    a.sin_family = AF_INET;
    a.sin_port = htons((u_short)g_net_port);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(sk, (struct sockaddr*)&a, sizeof(a)) != 0) {
        xlog("NET bind failed err=%d", WSAGetLastError()); closesocket(sk); return 1;
    }
    int to = 2000;
    int rb = 1 << 20;
    setsockopt(sk, SOL_SOCKET, SO_RCVTIMEO, (const char*)&to, sizeof(to));
    setsockopt(sk, SOL_SOCKET, SO_RCVBUF, (const char*)&rb, sizeof(rb));
    xlog("NET listening on 127.0.0.1:%d", g_net_port);
    unsigned char buf[512];
    for (;;) {
        int n = recv(sk, (char*)buf, sizeof(buf), 0);
        if (n > 0) net_apply(buf, n);
        else if (n < 0) { int e = WSAGetLastError(); if (e != WSAETIMEDOUT && e != WSAEWOULDBLOCK) xlog("NET recv err=%d", e); }
    }
    return 0;
}

static BOOL CALLBACK find_game_window(HWND w, LPARAM p)
{
    DWORD pid = 0;
    GetWindowThreadProcessId(w, &pid);
    if (pid != GetCurrentProcessId() || !IsWindowVisible(w)) return TRUE;
    /* 进程可能同时有 splash/无边框多窗口：取客户区最大的那个（渲染窗口） */
    RECT rc = {0,0,0,0};
    if (!GetClientRect(w, &rc)) return TRUE;
    LONG area = (rc.right - rc.left) * (rc.bottom - rc.top);
    if (area <= 0) return TRUE;
    LONG* best = (LONG*)p;
    if (area > *best) { *best = area; g_game_hwnd = w; }
    return TRUE;
}

static void run_command(LONG cmd, void* self);
static DWORD WINAPI worker(LPVOID);
static DWORD WINAPI worker(LPVOID)
{
    g_start_tick = GetTickCount();
    if (g_use_veh) AddVectoredExceptionHandler(1, veh_handler);
    IMAGE_DOS_HEADER* dos = (IMAGE_DOS_HEADER*)g_base;
    if (g_log == INVALID_HANDLE_VALUE) return 1;

    load_cfg();
    // 单例守卫：多份探针同时跑会各自发手指，互相打架（症状：角色一直按住不动/一直移动）
    HANDLE once = CreateMutexW(NULL, TRUE, L"mobui_probe_singleton");
    if (once && GetLastError() == ERROR_ALREADY_EXISTS) {
        xlog("SINGLETON: 已有探针实例在工作，本实例只读不动作（hooks/mapper 关闭）");
        g_map_enabled = 0;
        g_hook_mask = 0;
    }
    xlog("=== mobui_probe attached base=%p (expect 0x140000000) test=%d ===", g_base, g_test_mode);
    if (g_test_mode) {
        /* 离线验收台模式：只装 user32 缺省导出钩子（指针消息桥 + 触摸 API），不碰游戏地址、不读状态、不执行命令。
           这样同一份触摸投递代码可以在任意宿主里被 touchrecv.exe 验，而不必拉起整局原神。 */
        g_hook_mask = 0x80 | (g_hook_mask & 0x40);
        g_map_enabled = 0;
        g_pump_on = 0;
        xlog("TEST MODE: user32-only hooks (mask=0x%x), 游戏地址钩子/状态读/命令执行全部跳过", (unsigned)g_hook_mask);
        install_user32_hooks();
    } else {
        dump_state("attach");
        install_hooks();
        dump_state("hooks-installed");
    }

    for (int i = 0; i < 100 && !g_game_hwnd; i++) {
        LONG best = 0;
        g_game_hwnd = NULL;
        EnumWindows(find_game_window, (LPARAM)&best);
        if (!g_game_hwnd) Sleep(100);
    }
    xlog("TINJ game hwnd=%p", (void*)g_game_hwnd);
    if (g_net_enabled) {
        wchar_t tp[MAX_PATH]; wcscpy(tp, g_logpath);
        { wchar_t* d3 = wcsrchr(tp, L'.'); if (d3) wcscpy(d3, L".touch.log"); }
        g_tlog = CreateFileW(tp, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                             NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        tlog("=== touch flow log start (hwnd=%p) ===", (void*)g_game_hwnd);
        InterlockedExchange(&g_net_last_pkt, (LONG)GetTickCount());
        HANDLE nt = CreateThread(NULL, 0, net_thread, NULL, 0, NULL);
        if (nt) CloseHandle(nt);
    }
    InterlockedExchange(&g_map_on, g_map_enabled);
    if (g_map_enabled) {
        HANDLE mh = CreateThread(NULL, 0, mapper, NULL, 0, NULL);
        if (mh) CloseHandle(mh);
        xlog("MAP enabled (joy=%d,%d r=%d)", g_joy_cx, g_joy_cy, g_joy_r);
    } else xlog("MAP disabled by config");

    unsigned tick = 0;
    for (;;) {
        poll_cmds();
        poll_touch_cmds();
        /* net 看门狗：touchd 死了/丢包时把残留接触点抬起，避免角色"一直按住不放"。
           正常按住不动的心跳包每 250ms 一次，所以 1.5s 静默 = 对端确实不在了。 */
        if (g_net_enabled) {
            LONG last = InterlockedCompareExchange(&g_net_last_pkt, 0, 0);
            LONG now = (LONG)GetTickCount();
            if (last && (now - last) > 1500) {
                int n = 0;
                EnterCriticalSection(&g_touch_lock);
                for (int i = 0; i < MAX_CONTACTS; i++)
                    if (g_contacts[i].state && g_contacts[i].state != 3) { g_contacts[i].state = 3; n++; }
                UINT sent = n ? emit_touch("watchdog", 0, NULL) : 0;
                LeaveCriticalSection(&g_touch_lock);
                if (n) {
                    InterlockedIncrement(&g_watchdog_lifts);
                    xlog("NET WATCHDOG: %d contact(s) lifted after %ldms silence (posted=%u)",
                         n, (long)(now - last), sent);
                }
                InterlockedExchange(&g_net_last_pkt, now);   /* 只报一次，别每 500ms 刷屏 */
            }
        }
        tick++;
        /* 模式看门狗：游戏自己把输入模式改回 PC(2) 时（切场景/进出千星奇域），按开关自动切回触摸(0)。
           默认 g_ui_auto=0（用户手动控制），命令 uiauto1/uiauto0 切换。 */
        if (g_ui_auto && !g_test_mode && g_game_hwnd) {
            uint64_t root = rd64(at(kRvaRoot));
            uint64_t mgr = root ? rd64(root + kSlot9EC0) : 0;
            int m = mgr ? (int)rd32(mgr + kModeField) : -1;
            if (m == 2 && InterlockedCompareExchange(&g_cmd, CMD_NONE, CMD_NONE) == CMD_NONE) {
                InterlockedExchange(&g_cmd, CMD_TOUCH_NULL);
                xlog("UI auto-reapply: saw mode9EC0=2, queued touch_null");
            }
        }
        if ((tick % 4) == 0) {
            if (!g_test_mode) dump_state("periodic");
            if (g_tlog != INVALID_HANDLE_VALUE) {
                RECT rc2 = {0,0,0,0}; if (g_game_hwnd) GetClientRect(g_game_hwnd, &rc2);
                tlog("STAT rx=%ld post=%ld gti=%ld downs=%ld ups=%ld foreign=%ld bad=%ld wd=%ld win=%ldx%ld",
                     (long)g_stat_rx, (long)g_stat_post, (long)g_stat_gti,
                     (long)g_stat_down, (long)g_stat_up, (long)g_stat_gti_foreign,
                     (long)g_proto_bad, (long)g_watchdog_lifts,
                     (long)(rc2.right - rc2.left), (long)(rc2.bottom - rc2.top));
            }
        }
        else xlog("HEARTBEAT tick=%u fps_frames=%ld", tick, (long)InterlockedCompareExchange(&g_frame, 0, 0));
        if (!g_test_mode && !g_pump_on && !InterlockedCompareExchange(&g_fov_hooked, 0, 0)) {
            // 没有游戏线程通道时的兜底：在 worker 线程执行（有线程亲和风险，故日志标注）
            LONG cmd = InterlockedExchange(&g_cmd, CMD_NONE);
            if (cmd != CMD_NONE) {
                xlog("WARN executing cmd=%ld on worker thread (no FOV trampoline)", (long)cmd);
                run_command(cmd, NULL);
            }
        }
        Sleep(500);
    }
    return 0;
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved)
{
    (void)reserved;
    if (reason != DLL_PROCESS_ATTACH) return TRUE;
    g_self = inst;
    g_base = (uint8_t*)GetModuleHandleW(NULL);
    InitializeCriticalSection(&g_lock);
    InitializeCriticalSection(&g_touch_lock);   /* 必须在 worker 建 net 线程前初始化 */

    DWORD n = GetModuleFileNameW(inst, g_logpath, MAX_PATH);
    if (!n || n >= MAX_PATH) return TRUE;
    wcscpy(g_cmdpath, g_logpath);
    wcscpy(g_cfgpath, g_logpath);
    wchar_t* dot = wcsrchr(g_logpath, L'.');
    wchar_t* dotc = wcsrchr(g_cmdpath, L'.');
    wchar_t* dotf = wcsrchr(g_cfgpath, L'.');
    if (dot) wcscpy(dot, L".log");
    if (dotc) wcscpy(dotc, L".cmds");
    wcscpy(g_touchcmdpath, g_logpath);
    { wchar_t* d2 = wcsrchr(g_touchcmdpath, L'.'); if (d2) wcscpy(d2, L".tcmds"); }
    if (dotf) wcscpy(dotf, L".cfg");

    g_log = CreateFileW(g_logpath, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                        NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (g_log == INVALID_HANDLE_VALUE) return TRUE;

    HANDLE t = CreateThread(NULL, 0, worker, NULL, 0, NULL);
    if (t) CloseHandle(t);
    return TRUE;
}
