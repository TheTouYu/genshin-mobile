# genshin-mobile —— 原神 PC 版跑出移动端 UI（触摸投递）

把 Windows 版原神（wine）的运行期改成**移动端 UI**，并把**平板/串流的原生多点触控**送进游戏：
在游戏进程内注入探针，把触摸数据以 `WM_TOUCH`（`GetTouchInputInfo` 三元组）直接 `PostMessage` 给 Unity 窗口。

## 当前状态（2026-11-19）

| 探针 | 状态 | 说明 |
|---|---|---|
| **mobui31** | **已验通过（默认）** | = 30 + 捏合修复。实测 1–3 指正常、捏合恢复，`rx=post=gti=4168 bad=0`、`downs=ups=36` |
| **mobui27** | 已知良好（回退位） | 实测 1–6 指、捏合正常。**源码已丢失，只有二进制**，见下 |
| mobui28 / 29 / 30 | 有回归 | 捏合失效。28 引入 `lookgain` 滞后滤波 + 整数截断，慢速位移被吞成 0 |

### 捏合回归的根因与修法（2026-11-19）

`lookgain` 把每个接触点的位移按增益缩放后再整数截断：

```c
// 旧（28/29/30）：gain=35 时，位移 1~2px → 2*35/100 = 0 → 位置不动
tx = c->x + (int)((long)(xs[j] - c->x) * gain / 100);
```

双指捏合时两根手指相向慢移，每帧位移正是 1~2px 量级 → 被整体吞掉，表现就是「捏合没反应」。
mobui27 的 cfg 里没有这个键，所以那版手势是原样透传的。

修法（mobui31）：**增益只作用于单指拖拽**（相机视角/摇杆），多指手势原样透传，并且改成四舍五入：

```c
if (gain != 100 && cnt == 1 && existed && c->state && gain > 0) { ... }
```

判据：`n≥3` 消息出现、`gti == post`、单指 `POST` 坐标逐帧 1px 级推进（说明增益在单指上仍生效）。

⚠️ **mobui27 的源码没有留存**（`probe/mobui.cpp` 已被后续版本覆盖）。`build/mobui27.dll` 是不可再生的
已知良好版本，已入库，别删。

## 快速开始

```bash
# 桌面图标「原神·手机模式」→ bin/genshin-mobile-launch（唯一入口，默认注入 mobui27）
bin/genshin-mobile-launch --dry-run         # 只打印将执行的命令
bin/genshin-mobile-launch --status          # 游戏在跑时看当前 UI 模式
bin/genshin-mobile-launch --dll=mobui31.dll # 指定探针版本
bin/genshin-mobile-launch --native          # 原生 mobile platform 路线（无触摸注入）

# 运行时调参（不用重启游戏；自动认领当前注入的探针）
./mobui-ui.sh status | touch | pc
./mobui-gain.sh 35                          # 灵敏度增益（仅 mobui28+）
printf 'primary anchor\n' >> build/mobui31.cmds   # PRIMARY 语义 A/B（仅 mobui30+）

# 触摸注入服务（游戏每局启动时由 mobui-play.sh 自动拉起）
systemd-run --user --collect --unit=mobui-touchd \
  --working-directory=$PWD/tools $PWD/tools/touchd auto 51721 1
```

## 目录

| 路径 | 作用 |
|---|---|
| `probe/mobui.cpp` | 探针本体：DLL 注入、user32 钩子、`WM_TOUCH` 投递、UI 模式切换 |
| `tools/touchd.c` | 读 Linux evdev 触摸设备 → UDP 51721 → 探针 |
| `tools/txsim.py` | 触摸包模拟器（离线验收与排障用） |
| `bin/genshin-mobile-launch` | 桌面图标唯一入口（`~/.local/bin` 里是软链） |
| `mobui-play.sh` / `mobui-switch.sh` | 起游戏 → 等登录页 → 注入 → 切移动端 UI |
| `mobui-ui.sh` | 运行时切 UI / 查模式（自动认领在跑的探针） |
| `mobui-gain.sh` | 运行时调灵敏度 |
| `verify-offline.sh` | 离线验收台：`--selftest` / `--replay` / full |
| `docs/unity-touch-notes.md` | Unity 消费 `WM_TOUCH` 的证据版语义分析（F1–F7 改法表） |
| `docs/offline-verify.md` | 验收台运行手册、断言表、假 FAIL 机制 |
| `HANDOFF-touch-2026-11-19.md` | 交接记录 |

## 判据（怎么算对）

- 投递：`build/<dll>.touch.log` 末尾 `STAT` 行 `rx == post == gti`、`bad=0`、`downs ≈ ups`
- 多点触控：同日志里出现 `POST n=3` 及以上（只到 `n=2` 说明多指被吞）
- UI 模式：`build/<dll>.log` 的 `STATE slots ... mode9EC0=0` = 移动端 UI，`2` = PC UI
- 单指语义：`f=12`(DOWN|PRIMARY) → `f=11`/`f=1`(MOVE) → `f=14`(UP|PRIMARY)，每条消息恰一个 PRIMARY

## 已知坑

1. **探针版本漂移**：`build/<dll>.cmds` 是每个 DLL 独立的一份。写死文件名会让命令静默无效 ——
   所有脚本按 `build/mobui<数字>.log` 的 mtime 自动认领在跑的探针。
2. **单例互斥** `mobui_probe_singleton`：游戏开着时跑 `verify-offline.sh` full 模式必然假 FAIL。
3. **虚拟触摸设备会重建**：`event27 libvirtualhid Touchscreen` 被重启后，touchd 拿着旧句柄会
   报 `read: No such device` 退出 → 触摸全无。判据是 `systemctl --user is-active mobui-touchd`。
4. **`--replay` 要配 full-run 接收端日志**（含 `SUMMARY touches=N`），喂 selftest 日志会稳定 FAIL。

## 配套 GUI（命令管理器）

日常操作（启动+注入+切 UI、运行中切移动端/PC UI、调灵敏度、离线验收、性能采样）已经做成可视化命令面板：

- [TheTouYu/dsh-cli-manager](https://github.com/TheTouYu/dsh-cli-manager) —— 通用 CLI 命令管理 GUI，首个模块就是本项目的 10 条命令；每条命令原样显示、可复制、流式回显输出。
- 模块定义：`modules/genshin-touch.json`（`cwd` 指向本仓库目录）。
- Win11 迁移计划见该仓库的 `docs/win11-plan.md`。
