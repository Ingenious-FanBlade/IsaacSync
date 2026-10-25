# GoodTrip 联机传送 desync：证据基线与下一轮调查方案（2026-08-11）

## 1. 目的、范围与当前结论

本文面向 Isaac Sync 与 GoodTrip 的实现、调试人员，固定 2026-08-10 双端样本的证据基线，并规定下一轮插桩、实验和归因标准。文中的 Endpoint A/B 只是去身份化的日志端点，不代表游戏中的玩家实体；`F` 表示原生 desync 报告帧，operation frame 使用 Isaac Sync JSONL 中的 `operationFrame`。

当前结论如下：

1. 九次 desync 中有八次发生在 GoodTrip teleport 后一帧；其中七次同时出现 RNG 分叉和实体“一次 update 偏移”，且落后的物理端点始终是该次 teleport 的 sender。
2. 两端 packet、operation identity 和 Lua handler dispatch 完全匹配。相关窗口没有 ready gate stall、send rejection、runtime failure 或 dispatch failure，因此可以高置信度排除 Isaac Sync 传输、配对和 dispatch 帧不一致。
3. F449 没有自定义同步操作；双方在该局 ACTIVE 之前都因 `a different game lua_State was observed before cleanup` 进入 `FAILED_PRE_ACTIVE`。它是独立的 Lua state 生命周期/普通 RNG desync 问题，不应并入 teleport 根因。
4. 初始首要假设是同步 handler 调用 `StartRoomTransition` 的阶段问题；2026-08-11 的 Trace V2 T0 复现已经把本次清房后传送定位为 H2：双方 validation 对缓存的当前房间 clear 状态得出不同结果。

这些结论是现有证据下的因果定位，不是对 `StartRoomTransition` 内部行为的形式证明。阶段迁移仍不得与 validation 修复混为一个实验。

### 1.1 2026-08-11 T0 复现与修复

Session `8387916150593663141` 的前七次 teleport 双方均产生六个 Trace V2 phase。最后一次 F6101/S0/GID 84 发生于普通怪房清理后：local slot 0 在 game frame 5989 validation 成功并调用 transition，local slot 1 在相同 operation/game frame 以 `CURRENT_ROOM_UNCLEAR` 拒绝；F6102 随即 desync。双方 event 45、event 53 及 handler 数均匹配，排除了该次 operation 的复制或 Native dispatch 丢失。

代码路径表明 validation 使用 floor snapshot 中缓存的 `RoomDescriptor.Clear`。本地打开地图会重建 snapshot，但该本地输入不会使另一端同时刷新。因此修复版在每个合法的 synchronized teleport 正式 validation 前，于双方各调用一次 `floor:rebuild()`；单机命令、policy、prepare、transition 阶段和副作用顺序不变。该修复的回归条件是“清理怪房后立即传送”时双方 validation 均为 `OK`、`planHash` 相同且各记录六个 phase。

## 2. 证据登记

### 2.1 双端日志

下表只保留 basename、大小和摘要，不记录用户名或绝对路径。Endpoint A 在这些共享 session 中是 local slot 0，Endpoint B 是 local slot 1。

| Endpoint | JSONL basename | 字节数 | SHA-256 | 用途 |
|---|---|---:|---|---|
| A | `isaac-sync-1.0.0-10740-20260810-214014-229.jsonl` | 20,853,698 | `47F6D0C4BDC2A1743FFC95E2F1D46F1FDD364D0FEA85B56829D82B3BF9DA8D38` | F2554、F901、F1942、F702、F2657、F6061、F665 |
| B | `isaac-sync-1.0.0-8328-20260810-214008-065.jsonl` | 20,743,702 | `F8108F6954D9B004D5CFC3E9835EF6DFD2CF6AA7C6FE7BD9B50CB0E58C4DE11F` | 与上一行配对 |
| A | `isaac-sync-1.0.0-9620-20260810-212512-197.jsonl` | 8,224,416 | `A7A0D1DB9D5A77194AFBC1902D531156A0C7DA047123A2A3EBB651FF2616C5B5` | F6156、F449 前后的本端生命周期 |
| B | `isaac-sync-1.0.0-13300-20260810-182315-133.jsonl` | 64,139,408 | `A791BB1A1491E3E1F6F3EF37B29AB8B1B5A7E28513786CEBC101A3BBED3ED08B` | 按共享 session 隔离后的远端样本 |

较旧的 Endpoint B 文件包含早于本轮测试的其它 session，不能按整文件时间线与 Endpoint A 一一对应。F6156 只使用双方共同的 session `10354335437172317344`。

### 2.2 Session 与关键 dispatch 行

| Desync | Session token | teleport identity | Endpoint A 行 | Endpoint B 行 |
|---|---:|---|---:|---:|
| F6156 | `10354335437172317344` | F6155 / S1 / opcode 1 / GID 84 | 18478 | 144032 |
| F2554 | `10915570458351663225` | F2553 / S1 / opcode 1 / GID 83 | 7687 | 7666 |
| F901 | `11641968288335397227` | F900 / S0 / opcode 1 / GID 84 | 10452 | 10420 |
| F1942 | `10017098655033004203` | F1941 / S0 / opcode 1 / GID 71 | 16355 | 16307 |
| F702 | `4540874207092249354` | F701 / S0 / opcode 1 / GID 84 | 18501 | 18449 |
| F2657 | `5109100622692283869` | F2656 / S0 / opcode 1 / GID 85 | 26575 | 26498 |
| F6061 | `15712254358375633066` | F6060 / S0 / opcode 1 / GID 83 | 45017 | 44863 |
| F665 | `8892036079545126126` | F664 / S0 / opcode 1 / GID 84 | 47056 | 46897 |

上述 teleport payload 都小于 256，协议高位 flag 全为零，即 `fastTransition=false`、`teleportAnimation=false`、`fairTripTime=false`。当前证据不涉及这些选项的组合差异。

### 2.3 配对不变量复核

复核 key 与正式 group analyzer 一致：packet 使用 `(session, sender, recipient, messageSequence, kind, crc32, frameFirst, recordCount)`；operation 使用 `(session, operationFrame, senderSlot, port, opcode, payloadKind, payloadBits)`。

| 样本 | packet events | packet identities | 配对错误 | 链路 sequence 错误 | 完整 operation identities |
|---|---:|---:|---:|---:|---:|
| 当前七个共享 session | 61,978 | 30,996 | 0 | 0 | 159 |
| 较旧共享 session | 24,538 | 12,270 | 0 | 0 | 26 |

当前两端各有 159 次 `SyncDispatch`，identity 逐条相等；较旧共享 session 两端各有 26 次，也逐条相等。四份日志中的 `SyncReadyStallBegin`、`LuaSendRejected`、`SyncDispatchFailure` 均为 0。

当前样本另有一个不完整 operation：session `8892036079545126126` 的 F680/S0/opcode 2/payload 0 只有一次 `LuaSendAccepted`，没有 sealed、received 或 dispatched。它不计入上表的 159 个完整 operation。

## 3. 九次事件矩阵

“偏移命中”表示 desync analyzer 判定为整整一次实体 update 差异的实体数量。“sender 落后”根据 framestate checksum、本机端点反查和 sender slot 映射共同判定。

| 时间 | Desync | desync 前 GoodTrip 操作 | Sender 物理端点 | RNG | 一次 update 偏移命中 | 观察 |
|---|---:|---|---|---|---:|---|
| 13:25:51 | F6156 | F6154 map-held；F6155 teleport 84 | B / S1 | 分叉 | 1 | sender 落后 |
| 13:36:52 | F449 | 无 | 无 ACTIVE sender | 分叉 | — | 双方均 `FAILED_PRE_ACTIVE` |
| 13:40:49 | F2554 | F2552 map-held；F2553 teleport 83 | B / S1 | 分叉 | 5 | sender 落后 |
| 13:42:59 | F901 | F899 map-held；F900 teleport 84 | A / S0 | 分叉 | 4 | sender 落后 |
| 13:44:23 | F1942 | F1940 map-held；F1941 teleport 71 | A / S0 | 分叉 | 10 | sender 落后 |
| 13:55:14 | F702 | F700 map-held；F701 teleport 84 | A / S0 | 分叉 | 6 | sender 落后 |
| 14:00:43 | F2657 | F2655 map-held；F2656 teleport 85 | A / S0 | 相同 | 0 | 仅亚微量 velocity/checksum 差异 |
| 14:06:10 | F6061 | F6059 map-held；F6060 teleport 83 | A / S0 | 分叉 | 14 | sender 落后 |
| 14:11:21 | F665 | F664 teleport 84；前 16 帧无 map-held | A / S0 | 分叉 | 5 | sender 落后；S0 映射到 player 1 |

由此得到三个约束：

- 落后端跟随 sender 从 B 切换到 A，不跟随固定机器，因此“某一端固定配置或性能异常”不是首要解释。
- F665 没有紧邻的 map-held，且 S0 映射到 player 1，说明问题既不要求 map-held，也不只影响 player 0。
- F2657 说明 teleport 后不一定立即形成 RNG/整帧偏移；它是保留的弱异常和对照样本，不能从主簇中删除。

## 4. 分析器严格失败的解释

### 4.1 当前日志的 F680/S0

正式 group analyzer 报告：

```text
operation F680/S0 的 accepted/sealed 不是 sender 各一次。
```

Endpoint A 在 JSONL 第 47067 行、online frame 668 接受了一条目标为 F680 的 map-held release；第 47068 行检测到原版 roster 缩减，第 47069 行结束 session。Endpoint B 在第 46908 行结束同一 session。该 operation 尚未 sealed 就被 session retirement 取消，目标帧又晚于 F665 desync，因此它是断线/清理的后果，不可能导致 F665。

严格 analyzer 不应把该整份日志改写为 PASS。后续应通过显式 session/frame 分析范围排除因果窗口之后的操作，而不是放宽 accepted/sealed 不变量。

### 4.2 较旧日志的 Lua state failure

较旧整文件分析在解析阶段拒绝 failure event：Endpoint A 第 18494 行、Endpoint B 第 144049 行都记录 `a different game lua_State was observed before cleanup`。这些事件发生在 F6156 的 session 已于 online frame 6159 正常结束之后，并在下一局 F449 之前使双方进入 `FAILED_PRE_ACTIVE`。

按共享 session `10354335437172317344` 隔离后，F6156 前的 12,270 个 packet identity 与 26 个 operation identity 均满足双端不变量。F449 则必须独立处理，因为当时没有 ACTIVE session 或 GoodTrip operation。

## 5. 代码路径与可观测性缺口

当前 GoodTrip 同步路径为：

1. `goodtrip/sync/session.lua:178-179` 调用 `deps.onOperation(command)` 后无条件返回 `{ handled = true }`。
2. `goodtrip/app.lua:95` 调用 `coordinator:executeSynced(command)`，但不读取返回的 `{ ok, reason }`。
3. `goodtrip/teleport/executor.lua:11-24` 可以因 validation/prepare 返回失败；成功时在第 39 行调用 transition effect。
4. `goodtrip/adapters/isaac_teleport.lua:89-127` 根据各端当前 room、target、player、blocking entity 和 route 准备计划；第 156-157 行直接调用 `Game():StartRoomTransition(...)`。
5. Native `native/src/runtime.cpp:2032` 使用 `lua_pcallk(..., 4, 0, ...)`，要求零个返回值。只要 handler 不抛 Lua error，第 2067 行就记录 `Lua handler dispatched`。
6. `native/src/dispatcher.cpp:17-32` 先运行全部自定义 handler，再调用原版 dispatch。相比之下，GoodTrip 普通输入路径在 `goodtrip/runtime/gameplay.lua:399-444` 的 `MC_POST_UPDATE` 中消费队列。

因此，现有 `SyncDispatch` 只能证明“Lua handler 返回且未抛错”，不能证明 validation 通过、plan 相同或双方实际在同一引擎阶段开始房间切换。

## 6. 假设树与证伪条件

| ID | 优先级 | 假设 | 现有支持 | 证伪条件 |
|---|---|---|---|---|
| H1 | 最高 | `StartRoomTransition` 在原版 dispatch 前执行，受 sender/local-player 上下文或重入阶段影响 | 七次落后端都跟随 sender；双端 handler 同帧；普通路径原本属于 `MC_POST_UPDATE` | 双端 transition/new-room 阶段完全一致，迁移到 `MC_POST_UPDATE` 后仍以相同比例复现 sender 偏移 |
| H2 | 高 | endpoint-local validation/prepare 一端静默拒绝 | executor 允许返回失败，调用方和 Native 都不记录结果 | 每次双方 `ok`、`reason` 和 `planHash` 完全一致，且均实际调用 transition |
| H3 | 中 | player ownership、route、伤害、门、诅咒或时间等计划副作用不同 | prepare 读取 player 和 floor 局部对象；sender 相关性可能来自 local ownership | 双端规范化计划及逐项副作用结果完全一致，new-room 后才首次分叉 |
| H4 | 独立 | 新局 Lua state 未清理导致 F449；F449 的 RNG 分叉来自非 sidecar 路径 | 双端均在 ACTIVE 前 error 204，窗口内没有同步操作 | 修复/规避 state transition 后仍在无 GoodTrip operation 的相同阶段稳定复现 |

## 7. GoodTrip 诊断标记契约

### 7.1 输出格式

插桩版本使用 `Isaac.DebugString` 输出单行 ASCII 标记，不改变正式 `Isaac.Sync` API。尚未部署的 V1 契约由 V2 取代；V2 同时保留 operation identity 与实际 Lua callback 帧，并公开规范化计划字段供分析器复算哈希。字段名称、顺序和缺省值固定如下：

```text
GT_SYNC_TRACE_V2 phase=<phase> operationFrame=<u32> gameFrame=<i32> senderSlot=<i32> localSlot=<i32> gid=<i32> flags=<u32> playerIndex=<i32> currentRoom=<i32> targetRoom=<i32> ok=<0|1|na> reason=<TOKEN> planHash=<16HEX> transitionGid=<i32> route=<CSV|-> curseDamageCount=<i32> removeMaze=<0|1|na> addTime=<i32> animationId=<i32> cooldown=<i32> playMusic=<0|1|na>
```

- `operationFrame` 是 handler 收到的 operation frame，所有后续阶段沿用该值作为 identity。
- `gameFrame` 是记录该 phase 时的 `Game():GetFrameCount()`；它只用于比较 callback 时机，不替代 Native online/operation frame，不可用时写 `-1`。
- `currentRoom`、`targetRoom` 均为 GoodTrip floor snapshot 的 `gridIndex`；不可用时写 `-99`。
- `senderSlot`、`localSlot`、`playerIndex` 不可用时写 `-1`。
- `flags` 与同步 payload 高位三项 travel flag 一致。
- `reason` 必须是无空格的大写下划线 token；成功写 `OK`，尚未求值写 `NA`。
- `planHash` 未生成时写 `0000000000000000`，生成后写 16 位大写十六进制。
- 未生成计划时 `transitionGid/curseDamageCount/addTime/animationId/cooldown` 写 `-1`，`route` 写 `-`，三态布尔写 `na`。生成计划后 `route` 按执行顺序用逗号连接；空 route 仍写 `-`，但哈希 canonical string 中对应空字段。
- 标记不得包含用户名、Steam ID、路径或任意自由文本。

### 7.2 阶段

每个 endpoint 对每个 teleport operation 按以下阶段记录：

| phase | 记录时点 | 必填结果 |
|---|---|---|
| `dispatch_begin` | GoodTrip handler 解码并完成 sender→player 映射后 | operationFrame、gameFrame、slot、gid、flags、playerIndex |
| `validate_result` | `validateTeleportCommand` 返回后 | ok、reason、currentRoom、targetRoom |
| `plan_ready` | adapter `prepare` 返回后 | ok、reason、planHash、animationId、cooldown |
| `transition_called` | 调用 `StartRoomTransition` 后立即记录 | planHash、currentRoom、targetRoom |
| `post_update` | 同一 operation 后第一次 `MC_POST_UPDATE` | 当前 room；每个 operation 只记一次 |
| `new_room` | 与该 operation 关联的第一次 `MC_POST_NEW_ROOM` | 到达 room；每个 operation 只记一次 |

validation 或 prepare 失败时仍必须记录对应 result，随后停止该端 operation 的后续阶段；调查构建不得为了制造对称日志而强制继续执行。

所有 trace 公共入口均为 fail-open，观察或写日志失败不得改变 validation、plan 或副作用。若 `StartRoomTransition` 在返回前同步重入 callback，先缓存且去重观察；`transition_called` 输出后按 `post_update`、`new_room` 的固定顺序冲刷已缓存阶段。

### 7.3 `planHash`

`planHash` 使用 64-bit FNV-1a；Lua 实现使用两个 32-bit 分量完成 modulo 2^64 乘法，避免有符号整数溢出差异。分析器必须根据 V2 的公开计划字段复算。canonical string 数值使用十进制、布尔使用 `0/1`、route 使用逗号连接且保持执行顺序：

```text
gid|playerIndex|transitionGid|routeCsv|curseDamageCount|removeMaze|addTime|animationId|cooldown|playMusic
```

任何缺失数值写 `-1`，空 route 生成空字段。哈希只用于比较计划一致性，不作为安全或协议身份。

## 8. Native 诊断事件契约

诊断构建新增事件枚举：

```text
SyncOriginalDispatchCompleted = 53
```

约束如下：

- 只在当前 online frame 至少尝试过一个非 NoOp 自定义 operation 时发布。
- 发布位置在 `IOriginalDispatch::Call` 返回之后、`DispatchHook` 返回之前。
- `onlineFrame` 与 `operationFrame` 都写当前 dispatch frame，`recordCount` 写成功 handler 数；message 固定为 `original dispatch completed after custom operations`。
- 该事件没有 port/opcode/payload identity，不参加 operation accepted/sealed/received/dispatched 计数。
- 只扩展 event type；不改变 `IpcEventV5` 布局、wire protocol、IPC ABI 版本或七项 Lua API。

现有 `SyncDispatch` 与新事件 53 的 JSONL sequence 顺序用于证明 handler 和原版 dispatch 的边界；GoodTrip 的 `post_update/new_room` 标记用于证明游戏 callback 阶段。

## 9. 分析工具建议

为避免当前两类整文件误拒绝遮蔽目标窗口，后续 CLI 设计为：

```text
IssacOnlineSync.exe --analyze-group <logA> <logB> --session <u64> --through-frame <u32>
```

- JSON 解析和文件内 `sequence` 连续性仍检查整份文件。
- roster、packet、failure 和 operation 的语义验证只选择指定 session。
- operation 不变量只纳入 `operationFrame <= through-frame` 的 key；不得把缺失 accepted/sealed/received/dispatched 降级成 warning。
- session 结束后的 `sessionToken=0` failure 不污染已明确选择的旧 session；目标 session、目标帧以内的 failure 仍导致失败。
- 输出必须显示应用的 token/frame 范围，防止范围过滤被误解为完整 session PASS。

双方 T0 构建使用以下入口联合验证 Native JSONL 与游戏日志，参数顺序固定为 A Sync/A game/B Sync/B game：

```text
IssacOnlineSync.exe --analyze-goodtrip-trace <syncA.jsonl> <gameA.log> <syncB.jsonl> <gameB.log> --session <u64> --through-frame <u32> [--format text|json]
```

该入口先执行相同的 scoped group invariants，再按 `(session, operationFrame, senderSlot, gid, flags)` 配对双方 teleport。它验证 `SyncDispatch → SyncOriginalDispatchCompleted` 的 JSONL sequence、event 53 的成功 handler 数、Trace V2 的 local slot、FNV、validation、plan 与 phase。无 finding 返回 0，诊断不一致返回 1，输入/格式错误返回 2；JSON 报告使用独立 schema v1。

GoodTrip trace analyzer 至少区分：

- `GOODTRIP_EXECUTION_OUTCOME_MISMATCH`：同一 operation 的 ok/reason 不同；
- `GOODTRIP_PLAN_MISMATCH`：双方 validation 成功但 planHash 不同；
- `GOODTRIP_TRANSITION_PHASE_MISMATCH`：plan 相同但 transition/new-room 所在帧不同；
- `GOODTRIP_HANDLER_WITHOUT_TRANSITION`：有 Native dispatch 成功但缺少预期 transition marker。

## 10. 实验方案

### 10.1 共同准备

每轮保存双方 Native DLL、注入器、Helper、GoodTrip 文件和配置的 SHA-256。双方必须使用相同游戏版本、Mod manifest、seed、已访问/已清理房间集合和默认 travel flags。每个 trial 保存成对 JSONL、游戏日志、desync 目录；即使没有 desync，也记录 session、sender、GID 和结果。

先使用只插桩、不改变执行阶段的 T0 构建建立基线。T0 不得把失败改成 Lua error，也不得排队或重试 transition。

### 10.2 控制组

- 建立 ACTIVE 双端 session，至少运行 6000 online frames。
- 不触发 GoodTrip teleport；普通移动、攻击和清房可以继续。
- 验收：无 desync、无 GT teleport marker、无 Native event 53、group analyzer 不变量全部通过。

若控制组 desync，先归入普通游戏/其它 Mod 路径，本轮 teleport 实验暂停。

### 10.3 第一轮：保持行为的联合插桩

双方各发起 20 次 teleport：GID 71、83、84、85 各 5 次。每个 GID 的 5 次中，3 次使用产生 map-held→teleport 的地图按键路径，2 次使用不产生紧邻 map-held 的直接选择路径。双方交替充当 sender，所有目标必须在该 trial 中满足同一 eligibility 前提。

每次 operation 检查：

1. packet 和 operation identity 双端一致；
2. 双方 `dispatch_begin` 的 frame/sender/gid/playerIndex 一致；
3. validation ok/reason 一致；
4. planHash 及 canonical plan 字段一致；
5. `transition_called`、Native event 53、`post_update`、`new_room` 的帧和顺序一致；
6. desync analyzer 没有 RNG 分叉或一次 update 偏移。

第一轮不得依据“没有 desync”跳过 trace 比较。

### 10.4 第二轮：只迁移执行阶段

在 T1 构建中，同步 handler 仍在 operation frame 完成 decode、mapping、validation 和 plan 生成，但只保存不可变 plan；实际副作用在同一端随后的第一次 `MC_POST_UPDATE` 按原顺序执行。每端只允许一个与 `(frame, senderSlot)` 绑定的待执行 teleport，重复或过期计划必须显式失败。

使用与第一轮完全相同的 40 次矩阵。除执行阶段迁移外，不改变 eligibility、plan 内容、transition 参数、cooldown 或副作用顺序。

- T0 复现 sender 落后而 T1 不再复现：支持 H1。
- T0/T1 均在 validation 或 plan 阶段分叉：支持 H2/H3，不能把改善或恶化归因于 callback 阶段。
- T0/T1 plan 相同但 new-room 帧仍分叉：继续调查引擎 transition/authority，不进入配置修复。

### 10.5 第三轮：逐层定位副作用

仅在双方 validation 成功且 planHash 一致、T1 仍复现时执行。按以下顺序生成诊断变体，每次只增加一层副作用：

1. 只调用 `StartRoomTransition`；
2. 增加 door reset；
3. 增加 route 中的 `ChangeRoom`；
4. 增加 curse damage 与 maze 处理；
5. 增加 time、music 和 cooldown。

首次重新出现 plan 后状态分叉的层即为下一调查对象。观察到 failure 时保留完整 trial，不通过删除失败样本获得 PASS。

### 10.6 Lua state 生命周期实验

该实验与 teleport 矩阵分开：在不重启游戏进程的情况下完成“进入联机局→退出→进入新联机局”三轮。每轮要求旧 manifest cleanup 完成、新 Lua state 重新绑定并重新形成 ACTIVE session。

验收为双方都不出现 error 204、`FAILED_PRE_ACTIVE` 或“5 秒内没有命中已绑定 state 的安全 lua_pcallk”。失败时只分析 state transition，不执行或解释 GoodTrip teleport。

## 11. 归因判定表

| 观察 | 归类 | 后续动作 |
|---|---|---|
| operation identity 或 Native dispatch 帧不同 | 传输/调度层 | 回到 group analyzer 与 Native runtime；暂停 GoodTrip 归因 |
| Native dispatch 相同，但 validation ok/reason 不同 | validation mismatch / H2 | 比较 room、target、player、blocking entity 和配置输入 |
| validation 相同，但 planHash 不同 | side-effect plan mismatch / H3 | 比较 canonical plan 的首个不同字段 |
| planHash、transition frame 相同，但 new-room frame 不同 | transition phase/authority / H1 | 以 T1 阶段迁移和最小 `StartRoomTransition` 变体继续 |
| new-room 相同，之后才出现 RNG 分叉 | post-transition side effect/RNG | 按第三轮逐层恢复副作用并检查 RNG history |
| sidecar 在 ACTIVE 前失败且没有 operation | lifecycle/独立 RNG / H4 | 只进入 Lua state 生命周期实验 |

## 12. 完成标准

调查阶段完成需要同时满足：

- 每个 operation 两端 validation、planHash、transition 调用帧和 new-room 帧一致；
- Endpoint A、B 各完成至少 20 次 teleport，不再出现 sender 专属的一次 update 偏移或 RNG 分叉；
- 无 teleport 控制组连续运行至少 6000 online frames；
- 相关 packet、accepted、sealed、received、dispatched 不变量错误均为 0；
- 每个失败 trial 都能由第 11 节唯一归为 validation mismatch、plan mismatch、transition phase mismatch、post-transition side effect 或独立 lifecycle/RNG 问题。

在上述条件满足前，不把“双方都有 `Lua handler dispatched`”视为 teleport 执行成功，也不把分析器因范围外事件拒绝等同于目标 desync 的传输根因。
