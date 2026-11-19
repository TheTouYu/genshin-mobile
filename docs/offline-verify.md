# 离线触摸验收台（verify-offline.sh）运行手册

不启动原神、不占真机的端到端验收：**真窗口 + 真 `RegisterTouchWindow` + 真 `GetTouchInputInfo` 钩子 + 真 `WM_TOUCH` 投递**，
只是触摸数据由 `tools/txsim.py` 按协议 v2 合成，坐标/时序完全脚本化，所以每条断言都能落到确定数字上。

配套语义文档：`docs/unity-touch-notes.md`（Unity 怎么消费 WM_TOUCH）与 `HANDOFF-touch-2026-11-19.md`（交付状态）。

## 先决条件（三条闸门，脚本自己会挡）

| 闸门 | 判据 | 为什么 |
|---|---|---|
| 原神没在跑 | `pgrep -x YuanShen.exe` 为空 | 探针单例 `mobui_probe_singleton` 是**整个 WINEPREFIX 共享**的；游戏里那份探针活着时，验收拷贝注入后命中 `ERROR_ALREADY_EXISTS` → `g_hook_mask=0` → 三个 user32 钩子不装 → 全表假 FAIL |
| 没有别的 `touchrecv.exe` | `pgrep -x touchrecv.exe` 为空 | 同上单例互斥；也避免两份接收端抢同一个 UDP 端口 |
| 端口没被占 | `ss -lunp` 里没有 `:<port>` | 默认 `51799`。**不要用 51721**：生产态 `mobui-touchd` 每 250ms 往 51721 发 `cnt=0` 心跳，会把接触点提前抬起（症状：MOVE 变成 UP+DOWN） |

XWayland 的 cookie 文件名每次登录都会变（`/run/user/1000/xauth_XXXXXX`）。脚本在 `XAUTHORITY` 未设置时自动认领；
手工跑 wine 时要自己给，否则报 `Authorization required, but no authorization protocol specified`。

## 用法

```bash
cd /home/h/dsh-plugins/.scratch/genshin-mobile

bash verify-offline.sh                        # 正式：从 build/mobui27.dll 造 test 拷贝再验，约 6-10s
bash verify-offline.sh build/foo.dll          # 换源 DLL
bash verify-offline.sh --selftest             # 冒烟：隐藏窗口 + 自投假 WM_TOUCH（零屏幕干扰）
bash verify-offline.sh --replay <recv.log> <txsim.sent> [probe.log] [probe_off] [touch.log]
                                             # 只跑断言，不建窗不注入；给全 6 个参数时连跨层检查一起跑
bash verify-offline.sh --equiv build/mobui27.dll build/mobui28.dll
                                             # 同一套包分别喂两个 DLL，再做等价性对照（建两次窗，约 15-20s）
bash verify-offline.sh --equiv-dumps <a.txt> <b.txt>
                                             # 只比较两份已有 MSG dump（不建窗，可离线复核）
TXSIM_PORT=51812 bash verify-offline.sh       # 换端口（同时写进 cfg 的 net_port）
```

退出码：`0` 全 PASS｜`1` 有 FAIL（打印相关日志原文）｜`2` 前置不就绪（DLL 缺、窗口起不来、端口被占、游戏在跑）。

### 正式模式会做什么

1. 把源 DLL 拷贝成 `build/vf_<源DLL名>.dll`，并写 `build/vf_<源DLL名>.cfg`（`test=1` / `hooks=0x80` / `net=1` / `net_port=<port>` / `convert=0`…）。
   探针的日志/cfg 路径跟随 DLL 名，所以拷贝名意味着**不碰** Lead 的 `probe/mobui.cpp`、`build/mobui27.*`。
2. 起 `build/touchrecv.exe`（2560x1440 无边框、标题 `touchrecv`、`WS_EX_TRANSPARENT` 不吃鼠标、`RegisterTouchWindow`）。
   就绪判据是日志出现 `RTW -> 1`。
3. `build/inject.exe --name touchrecv.exe` 注入拷贝；探针就绪判据是 `ss` 看到 `:<port>` 被监听 或 探针日志 `NET listening on 127.0.0.1:<port>`。
4. `tools/txsim.py` 发 4 个场景（S1 单点 / S2 双指捏合 / S3 双指同起同落 / S4 id 复用）。
   **每个场景发完会读日志握手**（等本场景 `MSG` 条数到齐）再写下个 `SCENARIO` 标记。
5. 断言 → 打印 PASS/FAIL 表 + 写 `build/verify-offline.summary`（机器可读：`RESULT=` / `CHECK <id> <PASS|FAIL> …`）。

### `MSG` 行字段

```
MSG n=1 [id=1020 f=12 x=1280 y=720 t=3478560] wt=3478559 mt=3478559 mask=4
```

| 字段 | 含义 | 为什么要有 |
|---|---|---|
| `t` | 本函数里的 `GetTickCount()` | 接收端时刻（排序用） |
| `wt` | `TOUCHINPUT.dwTime` | 投递端填的时刻（探针 `emit_touch` 里 = 它自己的 `GetTickCount`） |
| `mt` | 消息队列时间（`MSG.time`） | `PostMessage` 时刻；与 `wt` 的差可看投递延迟 |
| `mask` | `TOUCHINPUT.dwMask` | `TOUCHINPUTMASKF_CONTACTAREA=0x4`；加 `TIMEFROMSYSTEM` 后是 `0x5` |

### 等价性对照（`--equiv` / `--equiv-dumps`）

同一套 `txsim` 包喂两个 DLL，比较两份 `MSG` dump：

| 项 | 判据 |
|---|---|
| E1 | `MSG` 条数相等 |
| E2 | 去掉时钟字段（`t`/`wt`/`mt` 跨轮必然不同，必须归一化）与 `mask` 后，`MSG` 行**逐字符相等** |
| E3 | `dwMask` 差异符合预期：A=0x4（`CONTACTAREA`）、B=0x5（`+TIMEFROMSYSTEM`） |

E2 是"几何/语义不变"的硬读数：例如把 `emit_touch` 的坐标从客户区改成 `ClientToScreen` 后，
窗口在 `(0,0)` 时它必须是恒等变换，E2 就该 PASS；一旦 E2 出现差异，说明坐标语义真变了。
`--equiv-dumps` 可以拿历史 dump 事后复核（不建窗）。

**E2 FAIL 的判读顺序（不许放宽成"允许整体偏移"）**：

1. 先看检查 `03b`（窗口原点是否 `(0,0)`）。前提不成立 → 先修 rig 的窗口摆放，别把它当成 28 的缺陷。
2. 前提成立仍然 E2 FAIL → 工具会打印一行诊断：
   `诊断: 所有接触点位移恒为 (dx=?, dy=?) → 固定偏移（坐标系语义差）`。
   - **位移恒定**且与窗口原点同量级：说明 B 侧真的把客户端坐标转成了屏幕坐标 → **真语义差**，报给负责人裁定。
   - **位移不恒定**：不是坐标系问题，是几何/映射逻辑真的变了。
3. 两种情况都判 FAIL。`--equiv-dumps` 的人工复核同理。

产物与日志：

| 文件 | 内容 |
|---|---|
| `build/touchrecv.log` | 接收端：`WINDOW`/`RTW`/`WMSG`（前 20 条消息号）/`MSG n=.. [id f x y t]`/`SUMMARY touches/gti_fail/msgs/ptr` |
| `build/vf_<源DLL名>.log` | 探针主日志：`attached`/`TEST MODE`/`NET listening`/`proto reject`/`TOUCH post[..] FAILED`/`SINGLETON` |
| `build/vf_<源DLL名>.touch.log` | 触摸流：`RX n=..`（收到的包）/`POST n=..`（投出的 WM_TOUCH）/`GTI count=..`（游戏侧实际解码） |
| `build/txsim.sent` | 发送账本：每个场景每包每点的归一化坐标（断言据此算期望像素） |
| `build/verify-offline.summary` | 机器可读结果 |
| `build/vf-probe-seg.txt` / `build/vf-touch-seg.txt` | 本轮新增的探针日志段（按注入前的行数切） |

## 三种必然假 FAIL 的机制（后来人最容易踩，先对号入座再怀疑代码）

### A. 游戏开着 → 全表 FAIL（探针单例）

- 现象：01-03 PASS，04/06/07/08b 一路 FAIL，探针日志出现
  `SINGLETON: 已有探针实例在工作，本实例只读不动作（hooks/mapper 关闭）`。
- 机制：`CreateMutexW(NULL, TRUE, L"mobui_probe_singleton")` 命名的互斥体在整个 **WINEPREFIX**
  （同一个 wineserver）里共享。生产态游戏进程里已有一份探针，验收拷贝注入 `touchrecv.exe` 后 `ERROR_ALREADY_EXISTS`
  → `g_hook_mask = 0` → 三个 user32 钩子（PeekMessage/GetTouchInputInfo/CloseTouchInputHandle）全不装
  → 接收端拿到的是 Wine 的原实现（句柄无效）→ `GTI_FAIL` / 没有接触点。
- 处置：脚本 preflight 直接 `exit 2`；**这不是代码坏**，别去改探针。要么关游戏，要么换独立 WINEPREFIX。

### B. 用 51721 跑验收 → 场景错位、多出 UP

- 现象：S1/S2 里出现"DOWN→UP→DOWN"或 MOVE 变成 UP+DOWN；`MSG` 条数比期望多。
- 机制：生产态 `mobui-touchd` 每 250ms 往 **51721** 发 `cnt=0` 心跳（journal 里的 `n=0 hb`）。
  它插进两个脚本包之间时，探针按"全量快照"语义把没出现在包里的接触点抬起。
- 处置：验收台只用 **51799**（脚本把它写进自己那份 `build/vf_<源DLL名>.cfg` 的 `net_port`，并传 `TXSIM_PORT`）。
  改端口时**必须同时改两边**；混用 51721 必然假 FAIL。

### C. 只靠 `sleep` 分隔场景 → 边界错位一条

- 现象：S2 的最后一条 MOVE/UP 落在 `SCENARIO S3` 之后，整段序列移位；`SUMMARY … msgs=` 明显偏大。
- 机制：全屏窗口收到约 100Hz `WM_POINTERUPDATE` 洪水（一轮 3.4s 可到 `msgs=349`），Wine 消息泵滞后约 200ms。
- 处置：接收端对 `0x0245–0x0249` 只计数不 `Dispatch` + `WS_EX_TRANSPARENT`（`SUMMARY … ptr=` 可看洪水条数）；
  `txsim.py` 每场景发完**读日志握手**（等本场景 `MSG` 条数到齐）再写下个标记，因此它必须能读到
  `build/touchrecv.log`（`TXSIM_RECVLOG` 可改）。这两层别拆掉。
- 附注：这份指针洪水在**生产态原神里同样存在**，靠探针 `convert=0` 不让它造手指——是设计取舍，
  不是接收端独有的 bug；`convert=1` 会让 ~100Hz 洪水不停造幽灵手指。

## 断言表：每一行在验什么、FAIL 通常是什么原因

| ID | 检查 | FAIL 常见原因 |
|---|---|---|
| 01 | 窗口 2560x1440 无边框、标题 `touchrecv`、可见 | X 授权失败（wine 建不了窗）；窗口被 WM 改尺寸 |
| 02 | `RegisterTouchWindow(hwnd,0)` 返回 TRUE | Wine 驱动异常 / 没拿到窗口 |
| 03 | 客户区全程不变 | 窗口被合成器缩放（坐标断言会连带失效） |
| 03b | **窗口原点 (0,0)** | 这是 27/28 坐标等价的先决条件（见下）；老日志没有 `origin=` 字段时该行显示 INFO |
| 04 | 注入成功（探针日志有新内容） | 注入器找不到进程；`LoadLibraryW` 失败 |
| 05 | 探针监听端口 == 验收端口 | cfg 里 `net_port` 写错；`net=0` |
| 06 | `WM_TOUCH` 真到了并解码出接触点 | 钩子没装（单例/`MH_Initialize`）；消息没投到 |
| 07 | `GTI_FAIL` 计数为 0 | `GetTouchInputInfo` 钩子没生效（句柄被当成外部句柄回落原实现） |
| 08 | 探针日志无 `proto reject` / `post FAILED` / `hook FAILED` | 协议版本不符（v1 发送端）；投递失败；钩子安装失败 |
| 08b | 探针没报 `SINGLETON` | **游戏开着**（单例被占），本轮注入等于空转 |
| 09 | 每条接触点恰有一个 DOWN/MOVE/UP 位 | flags 组合写错 |
| 10 | DOWN/UP 成对、MOVE 只在按住期间、无重复 DOWN | 状态机把 MOVE 当 DOWN（纯抬指包被丢的历史 bug 会在这里露头） |
| 11 | 逐场景动作序列与脚本**严格**一致 | 少一条/多一条消息 → 边界错位或心跳干扰（见下） |
| 12 | `dwID = 1000+(sim id & 0xFF)` | 映射公式漂移（代码现用 `&0x3FF`，本 rig 只用 id<256，两者同值） |
| 13 | 坐标误差 <3px（按发送顺序） | 客户端尺寸不是 2560x1440；归一化换算除法差异 |
| 14 | S2/S3 出现 n=2 消息且条数对 | 双指被拆成两条单指消息 |
| 15 | S3 一条消息里两条 UP | 抬指被逐条发 |
| 16 | 多指消息只有**首点**带 PRIMARY、每条恰一个 | PRIMARY 挂错位置（挂到每个点/挂到非首点） |
| 17 | S4 同 id 复用重新出现新 DOWN | 复用被并成 MOVE（状态没清） |
| 18 | 跨层一致：探针 `POST` == `GTI` == 接收端 `MSG` 条数 | 投出去但没到（消息泵/队列）；游戏侧没解码 |
| 19 | 消息时间戳单调不减 | 消息乱序（多源干扰） |

`TOUCHEVENTF_*` 常数（容易记反）：`MOVE=0x0001 / DOWN=0x0002 / UP=0x0004 / PRIMARY=0x0010`。
单指正确序列：`0x12(DOWN|PRIMARY) → 0x11(MOVE|PRIMARY) → 0x14(UP|PRIMARY)`。

## 断言器不是空转：负向自测配方

拿一份已知全 PASS 的日志，按下面任一处改动后 `--replay`，应当**非零退出**且只有对应行 FAIL：

| 改法 | 应 FAIL 的行 |
|---|---|
| 某条 MOVE 的 `x=` 改 +40 | 13 |
| 删掉最后一条 UP | 10 / 11 / 17 |
| 给 n=2 消息的**非首点**补 `0x10` | 16 |
| 把 S4 第一条 `f=12` 改成 `f=11` | 10 / 11 / 17 |
| 插入一条多余 `MSG` | 10 / 11 / 19 |
| 把 `build/txsim.sent` 清空 | 12 / 13（"发送账本为空"） |
| 两份 dump 里改一处坐标 | 等价性 E2 |
| 两份 dump 的 `mask` 都写成 4 | 等价性 E3 |

2026-11-19 实测：以上六种改法分别得到 `exit=1` 且失败行与预期完全一致。

## 什么时候不该用它

- 验"真机手感/延迟/手势识别"：合成包给不了这些，只能上平板。
- 验 `convert=1` 的指针桥：本 rig 的 `cfg` 固定 `convert=0`。
- 验真实 evdev → touchd 的路径：那是 `tools/touchd.c` 的事，本 rig 从 UDP 包开始。
