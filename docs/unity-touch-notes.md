# Unity WindowsPlayer 的 WM_TOUCH 消费语义（证据版）

> **分工**：怎么运行/怎么验证/已知偏差清单 → `HANDOFF-touch-2026-11-19.md`；本文件只写**Unity 怎么消费 WM_TOUCH**、**每条判据的出处**、以及**四条已知偏差的失效条件与判据**。
> 取证时间 2026-11-19 15:0x–15:3x。代码基准 `probe/mobui.cpp`（15:13）；行为基准 `build/mobui27.*`（15:19–15:24 真实运行）；对照基准 `build/mobui26.*`（修复前）。
> 标注纪律：**【实测】**= 本机日志/二进制/系统度量直接读到；**【文档】**= Microsoft Learn 或 Wine 源码明写；**【推断】**= 由前两者推出但无直接观测；**【未取证】**= 没有证据，不当结论用。

---

## 0. 一页结论

### 0.1 Unity 的消费语义（四条判定）

| # | 问题 | 判定 | 证据强度 |
|---|---|---|---|
| 1 | 数组是「全量快照」还是「仅变化点」？ | **全量快照**：每条消息带当前全部接触点（含本帧抬起的那一个）。「仅变化点」在文档上不成立，在实现上也没必要 | 【文档】+【实测】间接（`n=2` 1811 条 / `n=3` 165 条消息被正常消费） |
| 2 | `dwID` 是不是唯一身份？ | **是**。Unity 不读 lParam，只能经 `GetTouchInputInfo` 拿数组；数组里能跨消息关联同一根手指的字段只有 `dwID`（x/y/flags 都不是身份）。我方 `dwID` 非 0 且一个序列内稳定 | 【实测】导入表/调用链 +【文档】官方样例按 dwID 建表 |
| 3 | `TOUCHEVENTF_PRIMARY` 被 Unity 使用吗？ | **【未取证】**。我们能证明它不影响「消息是否被读」（有/无 PRIMARY 两种构建都被消费），不能证明 Unity 拿它做主指判定 | 【未取证】 |
| 4 | 依赖 `SM_DIGITIZER` / `SM_MAXIMUMTOUCHES` 才启用触摸吗？ | **不依赖**。Wine 下两者恒为 0，Unity 仍持续调用 GTI（并在该环境打印 `Initialized touch support.`） | 【实测】系统度量 + 探针计数 + 游戏日志（`output_log.txt:26` 命中 `Initialized touch support.`） |

另有一条由实测确定的事实：**Unity 只走「WM_TOUCH → GetTouchInputInfo → CloseTouchInputHandle」这条三元组**，不读 lParam、不用指针/手势 API（【实测】`YuanShen.exe` 导入名字符串：GTI/CTH/RTW 各 1，IsTouchWindow / GetPointerInfo / GetPointerTouchInfo / EnableMouseInPointer / InitializeTouchInjection / InjectTouchInput / GetGestureInfo 全 0）。

### 0.2 四条已知偏差（各自「什么时候会坏、怎么判」）

| # | 偏差 | 失效条件（什么时候会坏） | 怎么判 | 严重度 |
|---|---|---|---|---|
| 1 | PRIMARY 挂在**数组首元素**，不是「主指固定到全抬起」 | 多指场景里主指抬起后，PRIMARY 会被移交给另一根手指；**只有当消费方按 PRIMARY 认定主指**时才有后果 | Unity 侧【未取证】：改前改后各跑一次单指点击 + 双指手势，看点击/长按归属是否变化；日志侧看 `f=12/11/14`（带 PRIMARY）与 `f=2/1/4`（不带）的归属是否随手指抬起跳变 | 低（观察中未见影响） |
| 2 | `TOUCHINPUT.x/y` 按**客户区**给（MS 口径是屏幕空间） | **窗口客户区原点不在屏幕 (0,0) 时整体偏移**（窗口化、居中全屏、多显示器、带边框）。当前 `-popupwindow` 全屏 2560x1440 且客户区 1:1 → 恰好等价 | 把游戏改成窗口化（或有偏移）后点屏幕左上角 vs 右下角，触点会系统性偏移一个窗口原点；判据：偏移量 == 客户区原点 | **中**（换个启动参数就暴雷） |
| 3 | 客户端断线时 **uinput 残留接触点不自动释放** | 平板/Moonlight 端崩溃或断网，**但 touchd 还活着**并继续发旧接触点（uinput 设备仍报按下）→ 游戏里角色/镜头一直朝一个方向走、按钮像被按住 | 日志侧：同一 `dwID` 长时间只有 `f=1`(MOVE) 而**没有** `f=14`(UP)，且 `wd=0`（看门狗没触发——它只管「touchd 静默 >1.5s」，不管「客户端死但 touchd 仍在发」）。现象侧：松手后动作不停止 | **中** |
| 4 | `TOUCHINPUTMASKF_TIMEFROMSYSTEM` 未置位（`dwTime` 已填 `GetTickCount()`） | 消费方若用 `dwTime` 算长按/双击间隔、且信任「系统时间戳」语义时会算错。Unity 侧【未取证】（更可能用引擎自己的时钟） | 无 A/B 无法判；改动本身零风险，属「语义自洽」问题 | 极低（当前仅日志用） |

### 0.3 一条设计取舍（touch-verify 实测，别去掉）

**全屏窗口会持续收到 ~100Hz 的 `WM_POINTERUPDATE (0x245)`**（Wine 把 X11 指针/触摸事件翻成 WM_POINTER 的那条路，与本桥无关）。所以探针**必须有 `convert` 开关且生产态置 0**：若「收到指针消息就转成手指」，这 100Hz 洪水会不停造出/移动幽灵手指，把真实触摸淹没。判据：`convert=1` 时日志里 `POST[pointer]` 会以 ~100/s 增长且 `dwID` 与真实 tracking id 对不上；`convert=0` 时 `POST[net]` 只在真有触摸包时出现（mobui27 实测 `rx=post=gti=5290`，`n=3` 最多三指）。

---

## 1. Unity 到底怎么消费 WM_TOUCH（逐条证据）

### 1.1 只走 GTI 三元组，不读 lParam

- 【实测】`/home/h/.ys/YuanShen.exe` 导入名字符串计数：`GetTouchInputInfo=1`、`CloseTouchInputHandle=1`、`RegisterTouchWindow=1`；`IsTouchWindow`、`GetPointerInfo`、`GetPointerTouchInfo`、`EnableMouseInPointer`、`InitializeTouchInjection`、`InjectTouchInput`、`GetGestureInfo` **全部 = 0**。
- 【实测】`build/mobui27.touch.log`：每条 `POST` 之后都有对应的 `GTI count=...`（`post=5290 gti=5290`），说明游戏确实在**读**我们投的数组，而不是拿 lParam 当地址解引用。
- 【实测】`/home/h/.ys/YuanShen.exe` 内含 Unity 自己的日志串 `Initialized touch support`（grep 命中）→ 说明 Unity 原生触摸初始化路径带这条自检日志；15:24 运行里它被打印出来。**【实测·已复核】lead 给出的游戏日志 `/home/h/.local/share/anime-game-launcher/prefix/drive_c/users/h/AppData/LocalLow/miHoYo/原神/output_log.txt` 第 26 行命中 `<RI> Initialized touch support.`**（2026-11-19 15:2x 复核；此前作者未在该路径下定位到，故原文降级为转述）。
- 【文档】[WM_TOUCH](https://learn.microsoft.com/en-us/windows/win32/wintouch/wm-touchdown)：lParam 是「touch input handle that can be used in a call to GetTouchInputInfo」，进程内有效、`CloseTouchInputHandle` 释放、经 `DefWindowProc`/`PostMessage` 转发后失效。

**推论（决定我方方案合法性）**：既然消费方只经 GTI 读数据，那么「lParam 放我们自己的数组指针 + hook GTI/CTH 两端」在**进程内**是自洽的：句柄的不透明性由我们两端兜住。**边界**：同一份代码在真 Windows 上会因 `ERROR_INVALID_HANDLE` 失效（[GTI 文档](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-gettouchinputinfo)明确「handle 无效则失败」），所以这套是 Wine 专用设计，换环境必须重测。

### 1.2 数组是「全量快照」

- 【文档】[WM_TOUCH](https://learn.microsoft.com/en-us/windows/win32/wintouch/wm-touchdown)：`wParam` 低字 = 「the number of touch points **associated with this message**」；`TOUCHEVENTF_UP` = 「A touch point was removed」，即「移除」是一个**显式事件**而不是「缺席」。
- 【文档】官方样例 [Detecting and Tracking Multiple Touch Points](https://learn.microsoft.com/en-us/windows/win32/wintouch/detecting-and-tracking-multiple-touch-points)：每条消息把整个 `cInputs` 数组扫一遍、按 `dwID` 查表，**只在 `TOUCHEVENTF_UP` 时**把该点移出跟踪表（`points[index] = -1`）。也就是「一帧 = 一份当前状态」。
- 【实测】`build/mobui27.touch.log` 的 `POST n=` 分布：`n=1` 3312 条、`n=2` 1811 条、`n=3` 165 条；同一时刻多指共存的消息被正常消费，且 `downs=104 / ups=104` 严格配平。
- 【推断】如果 Unity 期待「仅变化点」，`n≥2` 消息里那些**没变化**的第二、第三指会被当作新事件重复处理，多点手势会立刻错乱——实测未出现（多指点击在 15:24 的会话里正常）。**未做 A/B**，所以标【推断】不标【实测】。

**结论**：保持「每条消息投全部活跃接触点（含待抬起的那个）」这条设计，不要为省字节改成「只发变化点」。

### 1.3 `dwID` 是唯一身份

- 【实测】Unity 只看 GTI 返回的数组；数组字段里能跨消息标识「同一根手指」的只有 `dwID`（`x/y` 会变、`flags` 每帧不同、`dwTime` 单调、`hSource` 我方恒 0）。
- 【实测】我方 `dwID = 1000 + (ABS_MT_TRACKING_ID & 0x3FF)`（真机 tracking id 472 → dwID 1472），一个触控序列内稳定，且永不为 0。
- 【文档】官方样例用 `if (ti.dwID != 0 && index < MAXPOINTS)` 过滤 → **`dwID == 0` 被视为无效点**，我方不会产出 0。
- 【文档】[TOUCHINPUT](https://learn.microsoft.com/en-us/windows/win32/api/winuser/ns-winuser-touchinput)：`dwID` 在一个接触点生命周期内稳定，之后可被复用。
- 推论：**`dwID` 必须一个序列一个值、不能每帧重置**。我方现在是「evdev tracking id + 1000」，符合；若将来改成自增计数器，必须保证「同一根手指不变、抬起后才允许复用」。

### 1.4 PRIMARY 是否被 Unity 使用：【未取证】

能证明的：
- 【实测】无 PRIMARY 的构建（`mobui26`：40 条消息 `f=1/2/4`，0 个 0x10）与有 PRIMARY 的构建（`mobui27`：`f=11` 5680、`f=12` 76、`f=14` 83，每消息恰一个）**都能被 Unity 消费**（GTI 计数 == POST 计数）。
- 【文档】[TOUCHINPUT](https://learn.microsoft.com/en-us/windows/win32/api/winuser/ns-winuser-touchinput)给 PRIMARY 的语义是「从**无**接触点→**有**」时指定一次，之后该点全程带 PRIMARY，直到它抬起；并且「系统鼠标位置跟随主指，并额外合成 `WM_LBUTTONDOWN`/`WM_MOUSEMOVE`/`WM_LBUTTONUP`」。
- 【推断】最后那句「合成鼠标」是**系统触摸栈**的行为，我们 `PostMessage` 绕过了栈 → 无论置不置 PRIMARY 都不会有合成鼠标事件。所以 PRIMARY 对 Unity 的唯一可能用途是「自己判主指」。

不能证明的：Unity 是否按 PRIMARY 选主指、是否用主指驱动 `simulateMouseWithTouches`。**没读过 Unity 源码，也没做 A/B**。
可做的实验（10 分钟）：把 PRIMARY 改成「固定到该序列第一个 DOWN 的 dwID、抬起才清」，与现状各跑一次「单指点击 / 双指缩放 / 长按」，比较 UI 命中是否变化；若无变化，这条永久降级为「语义洁癖」。

### 1.5 不依赖 `SM_DIGITIZER` / `SM_MAXIMUMTOUCHES`

- 【实测】Wine 11.14 的 `win32u/sysparams.c` 没有 `SM_DIGITIZER`(94)/`SM_MAXIMUMTOUCHES`(95) 分支，落到 `default: return 0`；本机读数 `GetSystemMetrics(94)=0, (95)=0`（`build/sysmetrics.log`）。
- 【实测】`mobui27` 那次运行 `hooks=0x90`（**没有** GSM 伪装），游戏在这个「数字化仪=0、最大触点数=0」的环境里：`RegisterTouchWindow` 成功 → 连续调用 `GetTouchInputInfo` 5290 次（探针日志硬读数）；同时游戏日志 `.../miHoYo/原神/output_log.txt:26` 命中 `Initialized touch support.`（**已复核，硬读数**）。
- 【文档】[GetSystemMetrics](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-getsystemmetrics) 里这两个索引只是**能力查询**，与「消息是否合法」无关；`NID_*` 位表见 [Getting Started](https://learn.microsoft.com/en-us/windows/win32/wintouch/getting-started-with-multi-touch-messages)。
- 结论：**依赖不成立**。探针里的 GSM 伪装（`hooks=0x20`）留作冷路径保险即可，默认不开。

### 1.6 消息到达窗口的要求

- 【文档】[RegisterTouchWindow](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-registertouchwindow)：必须对**每一个**要用触摸的窗口调用；线程不拥有窗口则 `ERROR_ACCESS_DENIED`；`TWF_FINETOUCH`/`TWF_WANTPALM` 只是精度/延迟取舍。
- 【文档】[Getting Started](https://learn.microsoft.com/en-us/windows/win32/wintouch/getting-started-with-multi-touch-messages)：WM_TOUCH 是「贪吃」的——某窗口收到第一条后所有触摸都归它，直到别的窗口获焦；并且注册触摸后不再收 `WM_GESTURE`。
- 【实测】游戏在 `hwnd=0x1005E` 调 RTW（flags=0），我们 POST 到同一 hwnd，GTI 一一对应 → **当前唯一窗口=注册窗口=投递窗口**。
- **残留风险**：投递目标由「找进程中第一个可见顶层窗口」推出，不是从 RTW 钩子记下来的。当前只有一个窗口所以等价；若将来出现启动画面/覆盖窗口，可能投错。最小加固见 §3-F5。
- 子窗口 / `SetCapture`：游戏是单顶层窗口，未观察到子窗口收输入 →【未取证】但不影响当前路径。

---

## 2. 四条偏差的判据卡（展开版）

### 偏差 1：PRIMARY 挂数组首元素

- **正确口径**【文档】：主指 = 从「无接触点」→「有」时的第一个点；PRIMARY 跟定它直到它抬起，之后才重新指定。
- **我方现状**【实测】：`emit_touch()` 里 `if (k == 1) t->dwFlags |= TOUCHEVENTF_PRIMARY;` —— 每帧把「数组第 0 个元素」当主指。mobui27 的 `f=11/12/14` 与 `f=1/2/4` 分布证实「每消息一个 PRIMARY，位置固定在数组首元素」。
- **失效条件**：双指/三指时主指先抬起 → 下一帧另一根手指顶上成为 PRIMARY。**只有当消费方按 PRIMARY 认主指**时才可观察。
- **判据**：`f=12`（带主指的 DOWN）出现在「另一根手指仍在按」的帧里；或 UI 点击归属在抬手瞬间跳变。
- **改法**：维护 `static UINT32 g_primary_id`；DOWN 且无主指时置位，仅在主指点抬起（state 3）时清零；置位条件从 `k == 1` 改成 `c->id == g_primary_id`（见 §3-F1）。
- **风险**：只改 `dwFlags` 内容，不动 GTI/CTH；但若 Unity 真用 PRIMARY 选主指，改完**会改变**点击归属 → 必须在已通的单指点击上回归。

### 偏差 2：x/y 是客户区坐标

- **正确口径**【文档】：`TOUCHINPUT.x/y` 是**屏幕**空间（百分之一像素）；官方样例是 `TOUCH_COORD_TO_PIXEL(ti.x)` 之后**自己** `ScreenToClient(hWnd, &ptInput)` —— 这行就是「系统给屏幕坐标」的直接证据。
- **我方现状**【实测】：`t->x = x * 100`，x 来自 `GetClientRect` 域（客户端坐标，且被 clamp 到客户区）。
- **为什么现在没坏**：`-popupwindow` 全屏、客户区 2560x1440、窗口原点 (0,0) → 客户区坐标 ≡ 屏幕坐标。
- **失效条件**：窗口化/居中全屏/多显示器/带边框，客户区原点 ≠ (0,0)。
- **判据**：点屏幕左上角，触点出现在 (ox, oy) 而不是 (0,0)；偏移量恰好等于客户区原点。
- **改法**：`POINT sp = {x, y}; ClientToScreen(g_game_hwnd, &sp); t->x = sp.x*100; t->y = sp.y*100;`（§3-F2，已在 `build/mobui28.dll` 落地）。
- **开关**：cfg 键 `screenxy=1`（默认，屏幕坐标）/ `screenxy=0`（客户区坐标，即 mobui27 的老行为）。窗口化下先单测再决定用哪个。
- **风险**：窗口在 (0,0) 时**逐字节等价**，当前场景零变化；窗口化时按文档方向修正。若 Unity 反其道按客户端解释（与官方样例矛盾，不太可能），`screenxy=0` 可一键退回。

### 偏差 3：客户端断线时 uinput 残留接触点不释放

- **机制**【实测源码】：探针的 `NET WATCHDOG` 只在「**touchd 静默 > 1500ms**」时把残留接触点抬起（正常心跳 250ms，1.5s 静默 = 对端确实不在了）。它管不到「touchd 还活着、uinput 设备仍旧报着按下」。
- **失效条件**：平板端 Moonlight 崩溃/断网/切后台 → uinput 侧没有 lift 事件 → touchd 继续按旧状态发包（或持续心跳） → 游戏里「一直按住」。
- **判据**：同一 `dwID` 长期只有 `f=1`（MOVE）没有 `f=14`（UP）、`wd=0`；现象上松手后动作不停。
- **改法**：touchd 在检测到「发送端静默/会话结束」时发一个空接触点包（协议 v2 的 `cnt=0`），探针把它当 `release_all_contacts("client-gone")`（该函数已存在）；或探针加「同一 dwID 的 MOVE 位置 N 秒不变」的启发式（不推荐，会误伤长按）。
- **风险**：只在断线路径生效，正常路径零影响。

### 偏差 4：`TOUCHINPUTMASKF_TIMEFROMSYSTEM` 未置位

- **正确口径**【文档】：不置该位时，`dwTime` 「系统不做校验，精度/顺序取决于提供者」；置位表示「时间戳来自系统」。
- **我方现状**【实测】：`dwTime = GetTickCount()`（毫秒，单调），而 `dwMask` 只有 `TOUCHINPUTMASKF_CONTACTAREA`。
- **失效条件**：消费方用 `dwTime` 算长按/双击间隔且要求系统口径。Unity 侧【未取证】。
- **改法**：`t->dwMask = TOUCHINPUTMASKF_CONTACTAREA | TOUCHINPUTMASKF_TIMEFROMSYSTEM;`（§3-F3）。
- **风险**：无（纯字段语义）。

---

## 3. 最小改法（按「是否值得现在做」排序）

> 行号基准 `probe/mobui.cpp`（15:13，1481 行）；代码在并行改写中，按**代码串**定位更稳。风险栏只答一事：**会不会破坏已跑通的 GTI 调用链**。

| 编号 | 现状写法 | 改成 | 值得做吗 / 风险 |
|---|---|---|---|
| **F1** | `:630 if (k == 1) t->dwFlags |= TOUCHEVENTF_PRIMARY;` | 维护主指 ID（DOWN 且无主指 → 记下；仅该 ID 带 PRIMARY；该 ID 抬起 → 清零；加「数组里已无该 ID 则清零」兜底） | 想彻底对齐文档时做。风险低（只改 flags），但可能改变点击归属 → 需单指回归 |
| **F2** | `:628 t->x = x * 100; t->y = y * 100;` | `POINT sp={x,y}; ClientToScreen(g_game_hwnd,&sp); t->x=sp.x*100; t->y=sp.y*100;`（cfg `screenxy=1`，默认开） | **已做**（mobui28）。当前场景逐字节等价；窗口化下可用 `screenxy=0` 退回 |
| **F3** | `:631 t->dwMask = TOUCHINPUTMASKF_CONTACTAREA;` | `\| TOUCHINPUTMASKF_TIMEFROMSYSTEM` | 零风险，顺手做 |
| **F4** | `:632 t->cxContact = 8; t->cyContact = 8;`（= 0.08 px） | 8 px → `800/800`；或去掉 CONTACTAREA 位与两字段 | 零风险，语义洁癖 |
| **F5** | `find_game_window()` 取「第一个可见顶层窗口」当 `g_game_hwnd` | `hk_RegisterTouchWindow()` 里直接 `g_game_hwnd = hwnd;` | 低风险；把「投递窗口」从猜测改成证据 |
| **F6** | GTI 无条件 `return TRUE` | 开头校验 `size != sizeof(TOUCHINPUT)` → `SetLastError(ERROR_INVALID_PARAMETER); return FALSE;` | 可选；Unity 实测传 48 所以行为不变，但会让「传 0 的调用方」由假成功变真失败，先打日志观察 |
| **F7** | `emit_touch()` 在 `PostMessageW` 成功之前就推进 `state 1→2 / 3→0` | 状态推进挪到 `ok` 分支之后 | 零风险（只影响投递失败路径） |

---

## 4. 附录 A：判据全表

> 原始 7 问的落点：Q1（lParam/句柄）→ §1.1；Q2（全量 vs 变化点）→ §1.2；Q3（DOWN/UP/PRIMARY）→ §2-偏差1 + 下表 `DOWN`/`UP`/`UP 后补 MOVE` 行；Q4（dwTime/hSource/dwExtraInfo/CONTACTAREA）→ §2-偏差4 + 下表 `dwTime`/`hSource`/`dwExtraInfo`/`cxContact` 行；Q5（窗口要求）→ §1.6；Q6（SM_DIGITIZER）→ §1.5；Q7（Wine 合成与官方通道）→ 附录 B。

判定口径：**OK** = 与文档/官方样例一致；**SUSPECT** = 有偏差或未验证；**UNKNOWN** = 无证据。

| 项 | 正确做法 | 我方现状 | 判定 | 出处 |
|---|---|---|---|---|
| wParam | 低字 = 本条消息触点数；高字保留（0） | `(WPARAM)k`，k≤10 | OK | [WM_TOUCH](https://learn.microsoft.com/en-us/windows/win32/wintouch/wm-touchdown) |
| lParam 类型 | 不透明 HTOUCHINPUT 句柄，进程内有效 | 数组指针 + 双端 hook | OK（Wine 限定，§1.1） | 同上 |
| 句柄生命周期 | CTH / DefWindowProc / 转发后失效 | 槽位 busy 位 + CTH 释放 | OK | [WM_TOUCH](https://learn.microsoft.com/en-us/windows/win32/wintouch/wm-touchdown) / [GTI](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-gettouchinputinfo) |
| 消息内容 | 全部当前接触点（含本帧 UP 的点） | 遍历全部活跃槽 | OK | [官方样例](https://learn.microsoft.com/en-us/windows/win32/wintouch/detecting-and-tracking-multiple-touch-points) |
| DOWN | 只出现一次；不得与 MOVE/UP 组合 | state 1→2 | OK | [TOUCHINPUT](https://learn.microsoft.com/en-us/windows/win32/api/winuser/ns-winuser-touchinput) |
| UP | 每个被移除点一次；之后不再出现 | state 3→0 | OK（mobui27 `downs=104/ups=104`） | 同上 |
| UP 后补 MOVE | 不需要 | 不补 | OK | 官方样例 |
| PRIMARY 数量 | 每条消息至多一个，且序列内固定 | 每帧挂在数组首元素 | SUSPECT（§2-偏差1） | TOUCHINPUT |
| PRIMARY 缺失 | 无主指 → 系统鼠标不跟随 | mobui26 曾 0 个（已随构建更新消失） | 已消除（旧构建）/ 待定（Unity 用途未取证） | 同上 |
| dwID | 非 0、序列内稳定、可复用 | 1000+(tracking&0x3FF) | OK（§1.3） | 样例 `dwID != 0` + TOUCHINPUT |
| x/y 单位 | 百分之一像素 | ×100 | OK | TOUCHINPUT |
| x/y 坐标系 | 屏幕（物理）坐标 | 客户端坐标 | SUSPECT（§2-偏差2） | 官方样例 `ScreenToClient` |
| dwTime | 毫秒；填了就置 TIMEFROMSYSTEM | GetTickCount，未置位 | SUSPECT（§2-偏差4） | TOUCHINPUT |
| dwMask | 只置真实有效的位 | 只置 CONTACTAREA | OK（但见下） | 同上 |
| cxContact/cyContact | 百分之一像素；无则别置位 | 8/8 = 0.08px | SUSPECT（F4） | 同上 |
| hSource | 设备句柄；无设备可 0 | 0 | UNKNOWN（可接受） | 同上 |
| dwExtraInfo | 可选，置 EXTRAINFO 位才有意义 | 0，未置位 | OK | 同上 |
| INRANGE/NOCOALESCE | 可选 | 从不置 | OK | 同上 |
| 窗口注册 | 每个触摸窗口都要 RTW | 游戏自调，hwnd 0x1005E | OK | [RegisterTouchWindow](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-registertouchwindow) |
| 消息目标 | 投给已注册窗口（WM_TOUCH 是「贪吃」的） | POST 目标 == RTW 的 hwnd | OK（目标靠推断，F5 加固） | [Getting Started](https://learn.microsoft.com/en-us/windows/win32/wintouch/getting-started-with-multi-touch-messages) |
| 子窗口 | 各自 RTW | 单顶层窗口 | UNKNOWN（低风险） | RegisterTouchWindow |
| IsTouchWindow | 可选 | 游戏不导入；Wine 返回 FALSE | OK | 导入表 + Wine 源码 |
| SM_DIGITIZER | 仅能力查询 | Wine=0，游戏照常初始化 | OK（§1.5） | 实测 + [GetSystemMetrics](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-getsystemmetrics) |
| SM_MAXIMUMTOUCHES | 同上 | Wine=0 | OK | 同上 |
| GTI cbSize | 必须 = sizeof(TOUCHINPUT)=48 | 不校验 | 可选（F6） | [GTI](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-gettouchinputinfo) |
| GTI count | ≥ 实际点数；不足时只填 count 条仍成功 | 钳到 10 | OK | 同上 |
| Wine 生产者 | Wine 不发 WM_TOUCH | 自造是唯一路 | OK（§4 附录B） | Wine 源码 |
| 官方注入通道 | InitializeTouchInjection + InjectTouchInput | Wine 全 stub，实测失败 | 不可用 | [InjectTouchInput](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-injecttouchinput) + `win32u.spec` |

## 5. 附录 B：Wine 侧为什么没有「官方通道」

- 【实测源码】`dlls/user32/input.c:645-690`：`CloseTouchInputHandle`/`GetTouchInputInfo` 是 `FIXME stub`（`ERROR_CALL_NOT_IMPLEMENTED` + `FALSE`）；`IsTouchWindow` 返回 FALSE；`RegisterTouchWindow`/`UnregisterTouchWindow` 是空实现返回 TRUE。
- 【实测源码】`dlls/win32u/win32u.spec`：`NtUserGetTouchInputInfo`(:1107)、`NtUserInjectTouchInput`(:1155) 都是 `@ stub -syscall`；`NtUserInitializeTouchInjection`(:1153) 是 FIXME stub。实测 `InjectTouchInput` 返回 0（失败）。
- 【实测源码】`dlls/winex11.drv/mouse.c` 的 `X11DRV_TouchEvent`：XI2 `XI_TouchBegin/Update/End` → **WM_POINTERDOWN/UPDATE/UP**（`NtUserSendHardwareInput`，id = `event->detail`，坐标归一化 0..65535）；XI2 触摸在窗口创建时就无条件选上，与 RTW 无关。
- 【实测】全树无 WM_TOUCH(0x0240) 生产者；本机运行也印证：`foreign=0`（从没有「不是我们投的句柄」进过 GTI）。
- 结论：**Wine 下没有比「进程内 PostMessage + hook GTI/CTH」更稳的通道**。真要走官方路线只有两条：给 Wine 实现 `NtUserGetTouchInputInfo` + WM_TOUCH 合成（上游量级），或换真 Windows 用 `InjectTouchInput`（要求先 `InitializeTouchInjection`，按「桌面全部接触点」数组喂）。都不是本周选项——**当前方案不是权宜之计，是 Wine 下的唯一解**。

## 6. 未验证 / 不能证明

1. 没有在真 Windows 上验证；本文件的「正确做法」均来自文档/样例。
2. 没读 Unity 源码：`dwTime`/`hSource`/PRIMARY 的**实际用途**属外推，PRIMARY 标【未取证】。
3. 「全量快照 vs 仅变化点」是文档推断 + `n≥2` 消息被消费的间接实测，**未做 A/B**。
4. README 级旧文档 `KNOWLEDGE.md` 里「游戏导入表没有任何触摸 API」**是错的**（见 §1.1 的 grep 读数），引用处需同步修正。

## 7. 出处

**Microsoft Learn**：WM_TOUCH（wm-touchdown）· TOUCHINPUT（ns-winuser-touchinput）· GetTouchInputInfo · CloseTouchInputHandle · RegisterTouchWindow · Detecting and Tracking Multiple Touch Points · Getting Started with Windows Touch Messages · GetSystemMetrics · InjectTouchInput / InitializeTouchInjection。URL 见正文每处链接。

**本机**：`probe/mobui.cpp`（emit_touch 604-655 / GTI 732+ / net_apply 1281 / watchdog 1416）· `build/mobui26.touch.log`（修复前反例）· `build/mobui27.touch.log`（`rx=post=gti=5290`、`n=1/2/3` 分布、`downs=ups=104`）· `build/mobui27.log`（RTW hwnd 0x1005E）· `build/sysmetrics.log`（94=0/95=0）· `/home/h/.ys/YuanShen.exe`（导入名字符串）· `/usr/x86_64-w64-mingw32/include/winuser.h:2629-2640,3169-3170,6677-6682`。

**Wine 11.14**（github.com/wine-mirror/wine）：`dlls/user32/input.c:645-690` · `dlls/win32u/win32u.spec:1107,1153,1155` · `dlls/winex11.drv/mouse.c`（X11DRV_TouchEvent）· `dlls/win32u/sysparams.c`（无 SM_DIGITIZER 分支）。
