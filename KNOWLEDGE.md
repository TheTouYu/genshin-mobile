# 原神 7.2 国服 Wine 移动端 UI —— 实机成功方法（2026-11-19）

## 结论
外部注入器把探针 DLL 打进行运行中的 YuanShen.exe，hook 平台 getter `0x14116B850` 当**游戏线程跳板**，
在跳板里调 `SwitchInputDeviceToTouchScreen`（VA `0x14D484110`）并传 **nullptr** →
游戏抛 C++ 异常 `0xE06D7363`（null-this），但异常前的副作用已把 UI 切成移动端；
异常由**瞬时 VEH**（AddVectoredExceptionHandler → longjmp → RemoveVectoredExceptionHandler）吞掉，进程存活。
用户目视确认：「切换成功了，是移动端 UI」。

## 工具（/home/h/dsh-plugins/.scratch/genshin-mobile/）
- `mobui-play.sh` —— 一键：无游戏则 launcher 启动 → 注入 → 切 UI。桌面项 `~/.local/share/applications/genshin-mobile-ui.desktop`
- `mobui-switch.sh [dll]` —— 幂等：探针没进就注入，已进就只补发切换命令（缺省 mobui5.dll）
- `build/inject.exe --name <exe> <dll> | --pid <windows-pid> | --list` —— x64 注入器（Wine 下 Linux pid ≠ Windows pid）
- `probe/mobui.cpp` → `build/mobui5.dll` —— 观测/实验探针：`.cfg` hook 位 + `.cmds` 命令队列 + 同目录 `.log`
- `probe-run.sh` / `late-inject.sh` / `matrix.sh` —— 实机实验台（重启注入、晚注入、hook 位矩阵）
- `vts/vts.c` —— uinput 虚拟多点触控屏（MT protocol B）

## 地址与状态（CN 7.2，ImageBase 0x140000000）
| 项 | 地址/值 |
|---|---|
| 全局根对象指针 | `[0x14586B600]` |
| 输入管理单例 | `[G+0x9E60]`、`[G+0x9EC0]` |
| UI 管理单例 | `[G+0xA070]` |
| 输入模式字段 | `[[G+0x9EC0]+0x1AC]`（PC=2 键鼠；改 0 能黏住但画面不变） |
| touch 切换 | `0x14D484110`（序言 `56 57 48 83 EC 28 48 89 CE 80 3D`） |
| 键盘切换 | `0xD476E00` |
| 分发器 | `0xD47A4A0`（native 0x14D47A710） |
| 平台 getter（跳板） | `0x14116B850`（jmp 0x14099DF60 = `mov eax,2;ret`） |
| ChangeFOV | `0x19B0A00` |
| flagA / flagB / flagC | `0x584B655` / `0x584B722` / `0x5B7DF90` |

touch flagA==0 路径关键调用序：`0x1546fb6a0(m9E60, MI 0x5989820)` → `0x14a206f50(m9E60,0,1)`
→ `0x14c295400(m9EC0,0,0)` → `0x148a4da00(rdi,eax)` → **`mov 0x388(%rsi),%rcx`（null-this 炸点）**
→ `0x150e08b50` → `0x14a9ce8e0([G+0xA070])` → `movb $0,0x3F4(%rax)`（真正隐藏 PC UI 在最后）。

## 反作弊判据
1. **常驻 VEH 必死**：注册后 3–4s 游戏 `NtRaiseHardError 0x50000018` 自杀 → 一律用瞬时 VEH。
2. **代码 hook 时好时坏**：launcher 正常启动的实例上 FOV+平台 getter 两 hook 跑了 35 万次调用未死；直接 `wine YuanShen.exe` 起的实例同类 hook 3s 就死。**别用直起实例下结论**，直起还会自己十几秒后退出。
3. clang `__try/__except` 在 `--target=x86_64-w64-mingw32` 下编译通过但**不捕获**；捕获只用 VEH+setjmp/longjmp（实测能吞 0xc0000005 与 0xE06D7363）。

## launcher 沙箱坑
`sandbox.enabled=true` 时 bwrap 带 `--tmpfs /home --tmpfs /home/h`：/home/h 全被抹掉、DLL 路径不存在、
沙箱内 wineserver 与宿主不同（`/tmp/.wine-1000/server-<dev>-<hash>`）。
已关闭 sandbox：`/home/h/.local/share/anime-game-launcher/config.json`，备份 `config.json.bak-mobui-20261119-044846`。
launcher 启动必须带 DBus：`DBUS_SESSION_BUS_ADDRESS=unix:path=/run/user/1000/bus`，否则静默退出。

## 多点触控（未解决，当前断点）
- 游戏只收到鼠标模拟的单点触摸（Unity simulateMouseWithTouches）→ 用户反馈「不是多点触控」。
- Wine `NtUserInjectTouchInput` 是 **unimplemented stub**（win32u abort），user32 不导出 `InjectTouchInput` → 注入 API 走不通。
- `winex11.so` 有 XI2 touch（XI_TouchBegin/Update/End），`winewayland.so` 没有。
- uinput 虚拟触控屏（vts）能创建、KWin 收得到，但 Xwayland 报 `xi/exevents.c:1396 RetrieveTouchDeliveryData` BUG；
  且**带触控设备启动游戏会崩**（启动阶段，疑似 Unity 触摸初始化）。
- 下一步候选：① 让虚拟触控屏在 Xwayland 启动前就存在并确认窗口 RegisterTouchWindow；
  ② 换 Xwayland 版本/编译 TODO；③ 在进程内 hook Unity 触摸读取函数伪造多指；④ 接受单点。

## 触发条件→动作
- 下次接手：`pgrep -x YuanShen.exe` → 没跑就 `./mobui-play.sh`；要重切就 `./mobui-switch.sh`。
- 要看画面：`DISPLAY=:1 XDG_RUNTIME_DIR=/run/user/1000 spectacle --background --nonotify --output /tmp/s.png`
  （游戏窗口须在最前；抬窗口必须先拿屏幕授权走 ys-gui-slot）。
- 若发现游戏在注入后 3s 内死 → 先查是否又用了常驻 VEH 或 hook 了非 launcher 启动实例。

## 多点触控：2026-11-19 更正——**这个 build 上做不到**（此前 D2 结论错误，已废止）

### 三层取证（都是行为读数）
| 层 | 探针 | 读数 |
|---|---|---|
| KWin→Xwayland→X 客户 | `xtouch/xtouch --overlay`（X11 XI2 触摸接收器） | 收到 **2 个不同 touchid** 的 XI_TouchBegin/Update/End（639 与 640），坐标正确 |
| Wine 投递给 Win32 应用 | `build/wintouch.exe`（TOP MOST 窗口 + RegisterTouchWindow） | 收到 **2 个 pointerId 的 WM_POINTERDOWN，pointerType=2(PT_TOUCH)**，坐标 298,298 与对应第二指 |
| 游戏本体 | `objdump -p YuanShen.exe` 导入表 | ⚠️ **本节旧结论已被推翻（2026-11-19 15:2x，见 `docs/unity-touch-notes.md` §1.1/§6）**：USER32 导入里**确实有** `RegisterTouchWindow` / `GetTouchInputInfo` / `CloseTouchInputHandle` 各 1 个（无 GetPointerInfo/EnableMouseInPointer/InitializeTouchInjection），即 Unity 走的就是 WM_TOUCH→GTI→CTH 三元组。旧写法"没有任何触摸/指针 API"是 grep 口径错（只查了 Pointer* 名字）。 |

### 结论
链路本身通（X11 层与 Wine 层都能拿到两指），**瓶颈在游戏**：WindowsPlayer 这个 build 没编触摸输入通道，系统把触摸降级成鼠标单点，所以双指捏合只会被当成单指拖动（用户实测：地图只平移、左栏缩放条不动）。
排除项：`NtUserInjectTouchInput` 是 unimplemented stub（走 API 注入不通）；`winewayland` 无 XI2 触摸，`winex11` 有。

### 想继续时的三条路
1. **先确认键盘可用性**：mobile UI 下 WASD 是否还能让角色移动（游戏导入了 GetAsyncKeyState/raw input）。能用 → 键鼠体验成立（鼠标单指＝镜头/按钮，键盘＝移动），不依赖多点触控。
2. **深度 patch 输入**：在进程内把触摸桥接成游戏内部输入（要动 il2cpp/Unity 输入层），成本高、未验证。
3. **接受单指**：点按/拖拽都能用，摇杆+镜头不能同时。

## 2026-11-19 15:2x 后续：多点触控**已做通**（本文件上面的"做不到"结论作废）

- 通道：进程内注入探针，`PostMessage(WM_TOUCH)` + hook `GetTouchInputInfo/CloseTouchInputHandle`（Wine 11.14 里这两者是 FIXME stub，winex11 只把 XI2 触摸翻成 WM_POINTER，所以**没有官方 WM_TOUCH 通道**，PostMessage 是唯一解）。取证与语义见 `docs/unity-touch-notes.md`。
- 触源：平板 → Sunshine → `/dev/input/event27`（libvirtualhid Touchscreen）→ `tools/touchd`（evdev MT protocol B，协议 v2 + 250ms 心跳）→ UDP 127.0.0.1:51721 → 探针。
- 生产读数（真机、已在世界内）：`rx=8067 post=8067 gti=8065 downs=171 ups=169 foreign=0 bad=0 wd=0 win=2560x1440`，`mode9EC0=0`，游戏日志 `Initialized touch support.`，零崩溃。
- 使用与验证：`./mobui-play.sh`（起游戏/起 touchd/注入/切 UI）；改触摸代码先跑 `bash verify-offline.sh`（**必须先关游戏**，探针单例全 WINEPREFIX 共享）。
- 交接与已知偏差：`HANDOFF-touch-2026-11-19.md`。
