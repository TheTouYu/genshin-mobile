# 触摸链路交付状态（2026-11-19 15:25 · 接手先读这份）

## 一句话
PC 端原神 + 移动端 UI + 平板真机触摸已跑通并在线验证：`mobui27.dll` 修复了三处根因，`touchd` 升级到协议 v2 + 心跳，桌面入口已指向 mobui27。

## 当前生产事实（可直接引用）
- 产物：`build/mobui27.dll`（390107B）+ `build/mobui27.cfg`（`hooks=0x90` / `veh=0` / `map=0` / `suppress=0` / `net=1` / `net_port=51721` / `pump=0` / `convert=0`）
- 注入：`./mobui-play.sh`（默认 mobui27.dll）或桌面「原神·手机UI（注入版）」；切 UI 成功判据 = 探针日志 `mode9EC0=0`
- 触摸守护：`systemd-run --user --collect --unit=mobui-touchd --working-directory=<tools> <tools>/touchd auto 51721 1`
  （transient unit，重启不自启；`mobui-play.sh` 现在每次起游戏前会幂等拉起它，`pgrep -x touchd` 判据）
- 真机实测（15:24，已在世界内）：`STAT rx=5280 post=5280 gti=5280 downs=99 ups=99 foreign=0 bad=0 wd=0 win=2560x1440`；游戏日志出现 `Initialized touch support.`；零崩溃
- 真实设备 id 映射：`dwID = 1000 + (ABS_MT_TRACKING_ID & 0x3FF)`（平板 tracking id 472 → dwID 1472）；touchd 心跳在 journal 里是 `n=0 hb`

## 三处根因（已修，别再重复诊断）
1. 闪退：`g_ti_buf[8]` 配 `% 64` 取槽越界 → 现 `TI_SLOTS=64`（2 的幂）+ `&(TI_SLOTS-1)` + `g_ti_busy[]` CAS 占槽 + `CloseTouchInputHandle` 释放
2. 点击无效：`net_apply` 里抬指循环返回值没并进 `changed`，`if(!changed)return;` 把纯抬指包整包丢弃
3. 协议错位：touchd 写 `pkt+3` 却发 `3+k` 字节 → v2 `'T'|0x4D|cnt|cnt×(u16 id,u16 nx,u16 ny)`，魔数/长度不符即拒收（`g_proto_bad`）
   附带修掉的两个初始化缺陷：`MH_Initialize()` 幂等化（`ensure_mh()`）、`WSAStartup` 之前从未调用（离线台 `err=10093` 暴露）

## 断言/验证通道（改触摸代码后先跑这个，别先开游戏）
- 离线验收台：`bash verify-offline.sh`（隔离端口 51799，**必须先关游戏**——探针单例 `mobui_probe_singleton` 是整个 WINEPREFIX 共享的，游戏开着时注入验收拷贝会命中 `ERROR_ALREADY_EXISTS` 而全表假 FAIL）
- 只重放不建窗：`bash verify-offline.sh --replay build/touchrecv27.log build/txsim.sent`
- 2026-11-19 15:15 那次真实产物：接收端 `build/touchrecv27.log`、探针 `build/mobui27t.touch.log`、发送账本 `build/txsim.sent`（16 项断言全 PASS）
- **常数表（踩过坑）**：`TOUCHEVENTF_MOVE=0x0001 / DOWN=0x0002 / UP=0x0004 / PRIMARY=0x0010`（`/usr/x86_64-w64-mingw32/include/winuser.h:2629-2633`）。单指正确序列 `0x12(DOWN|PRIMARY) → 0x11(MOVE|PRIMARY) → 0x14(UP|PRIMARY)`

## 已知偏差（不是已修，遇到症状先对号入座）
1. PRIMARY 挂在数组首元素，不是"主指固定到全抬起"（Unity 用 dwID 认指头，暂未见影响）
2. `TOUCHINPUT.x/y` 按**客户区**给（MS 文档口径是屏幕空间）：窗口不在 0,0 全屏时整体偏移。当前 `-screen-fullscreen 0 -popupwindow` 客户区 2560x1440，1:1
3. 平板端断线时 uinput 残留接触点不会自动释放（touchd 仍在发旧接触点，看门狗只管"touchd 死"不管"客户端死"）
4. `TOUCHINPUTMASKF_TIMEFROMSYSTEM` 未置位（`dwTime` 已填 GetTickCount，仅用于日志）

## 待验证候选（别直接上线）
`build/mobui28.dll`（+ `build/mobui28.cfg` 同 27）= F2+F4 硬化版，已编译、**未过离线台**：
- F2：`emit_touch` 改为 `ClientToScreen` 后 ×100（TOUCHINPUT 按 MS 口径是**屏幕空间**；窗口在 0,0 时恒等，逐字节等价）。**带 cfg 开关 `screenxy=1`(默认，屏幕坐标)/`0`(老行为=客户区坐标)**，窗口化场景先单测再定
- F4：`dwMask` 加 `TOUCHINPUTMASKF_TIMEFROMSYSTEM`（dwTime 填的是 GetTickCount）
`mobui-play.sh` / `mobui-switch.sh` / 桌面入口此刻仍指向 **mobui27**（已验证跑通的那份）。切换判据：`bash verify-offline.sh build/mobui28.dll` 注入版全 PASS 之后，再把三处默认改成 28。

## 下一步（按优先级）
1. 等用户真机反馈：哪个手势手感不对就改动效/坐标/PRIMARY，改完走 `verify-offline.sh` 回归
2. 游戏关掉的空档：`bash verify-offline.sh build/mobui28.dll` 拿注入版 PASS 表 → 切默认到 28
3. `docs/unity-touch-notes.md`（touch-semantics 已交付，205 行）：Unity 语义证据分级 + 四条偏差判据卡 + F1–F7 改法
4. `docs/offline-verify.md`（touch-verify）：验收台运行手册 / 断言表 / 假 FAIL 机制 / 负向配方
5. 仍未做：F1 主指 PRIMARY 固定（touch-semantics 给了 10 分钟 A/B 方案，未做）、客户端断线主动 release

## 环境备注
`memo_write` 本轮落库失败：嵌入服务 openrouter 402 无额度（`embed-http-402`），日记未进河；本文件即替代持久层。

## 文档地图（谁写什么，别重复）
- `HANDOFF-touch-2026-11-19.md`（本文件）：怎么运行 / 怎么验证 / 现在指向哪个产物 / 待办
- `docs/unity-touch-notes.md`（touch-semantics）：Unity 怎么消费 WM_TOUCH（证据分级：实测/文档推断/未取证）、四条偏差判据卡、F1–F7 最小改法、Wine 没有官方 WM_TOUCH 通道的取证
- `docs/offline-verify.md`（touch-verify）：验收台运行手册、19 条断言表、假 FAIL 机制（单例/端口/账本）、负向配方
- `KNOWLEDGE.md`：项目结论史；2026-11-19 已把"多点触控做不到 / 导入表没有触摸 API"两条旧结论标注作废并追加新结论

## 2026-11-19 15:50 真机试玩反馈与实测读数（下轮从这里接）

用户实测结论：滑动✓、边走边转视角的多点触控✓、点击✓；**双指捏合无效**；感觉①一动就掉帧 ②视角滑动"不连续、像几个散乱的点" ③切到千星奇域后鼠标不能操作，退出后游戏自己变回 PC UI。

**已测到的读数（都在 `build/mobui27.touch.log` / 本机 /proc）**
- 投递并不稀疏：手势中消息间隔 `p50=11ms p90=30ms`（≈90Hz），单指位移步长 `p50=1px p90=3px`
- 游戏消费很快：`POST→GTI` 延迟 `p50=5ms p90=14ms p99=37ms`（不是消息积压）
- 捏合**确实投递到了游戏**：日志里有多段两指距离单调变化的片段（如 `t=4025637` 486ms 内 836→219px 共 63 条消息；另有 511 条消息/4.5s 的长两指手势）
- 输入模式会被游戏自己改回去：`mode9EC0` 时间线 = 注入时 **2**（PC）→ t+7.9s **0**（我们切的触摸）→ **t+726s 又变回 2**（用户进出千星奇域期间）。即"切 UI 不粘"，需要模式看门狗或实时命令
- 基线 CPU（用户操作中采样）：YuanShen.exe 总 137%、最忙单线程 ~60%、Sunshine 22%、GPU<30% → 主线程有余量，掉帧若发生应在"移动/转视角"时把主线程顶到 100%
- 输入设备除了 Touchscreen（event27）还有 `libvirtualhid Mouse`（event25）与 `Mouse (Absolute)`（event26）→ **幽灵鼠标输入待排查**（触摸时是否同时发鼠标事件，可用采样器读 event25/26 计数）
- 仪器：`tools/perf-sample.py <秒数> [csv]`（只读，0.5s 一行：游戏总 CPU/最忙线程/Sunshine CPU/触摸事件率/鼠标事件率），已冒烟通过（`137.1,21.9,59.6`）。DXVK 在 prefix 里但**不支持 `DXVK_FRAME_TIMES`**（只有 `DXVK_HUD`）→ 逐帧时间要另找通道（Present 钩子 / MANGOHUD / 人眼看 HUD）

## 用户守则（2026-11-19 16:2x 明令）：减少重启游戏次数
每次重启 = 启动器+登录+注入+切UI ≈ 1-3 分钟，属于用户可感知的成本。**改动要攒批**：
- 凡是"可能要反复调"的参数（灵敏度增益、坐标开关、投递频率…）一律做成**运行时命令**（写 `.cmds`），不要做成启动 cfg 一次性读；
- 一次重启只装一个"打包版本"，装前把已知待改项全列出来；
- 重启前先说明代价与本次要一并验证的清单。
现状：`mobui28.dll`（16:21 装入）已含 lookgain=50 / uipc / uiauto0|1 / screenxy / TIMEFROMSYSTEM / 幂等 touch_null。下一版待加：`gain <pct>` 运行时命令、切换后自动重推灵敏度（待定位字段）。
