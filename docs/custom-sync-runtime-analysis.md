# 自定义同步：运行时逆向与无断点探针证据

## 结论摘要

当前分析对象是正在运行的 x86 `isaac-ng.exe`。2026-08-01 最新会话为 PID `26860`，主模块基址 `0x00AD0000`；旧会话的 `0x006A0000`、`0x00C00000` 和文档旧 VA 只保留为历史记录。本文件以模块 RVA、机器码、虚表和数据流作为可复定位依据。

| 问题 | 当前结论 | 状态 |
| --- | --- | --- |
| 自定义消息承载 | 当前后端枚举为 `2`，使用 Steam `ISteamNetworking`/`SteamNetworking006`；独立 channel `0xFD` | 2026-08-03 双机实测通过：两个方向各 1028 个协议包逐包对应，512 个 PING/PONG 无丢失、重复或乱序 |
| 原版 channel | Steam 原版发送、可用性查询和接收均固定 channel `0` | active 窗口 A/B 分别 2929/2929、1977/1977 次成功收发，非零 channel 和 `ISCP` 混入均为 0 |
| 稳定玩家身份 | 当前 Steam 后端的游戏 8 字节 identity 就是 `CSteamID`/SteamID64；`PeerKey=steam:<uint64>` | 会话内设备关联已确认；断线重连恢复 slot 待验证 |
| 精确分发位置 | `RVA 0x5110AB` 调用 `RVA 0x50D640` 之前 | ready stall、输入应用及成功帧相对 `MC_POST_UPDATE` 的主线程顺序均已动态确认 |
| 当前动态结果 | Lua marker 顺序与 Steam `0xFD` 双机传输均完成 | 601/601 次 candidate 早于同主线程 marker；active 双日志共 242790 个事件，sequence 缺口为 0 |

这里的“静态已确认”表示当前版本有 RVA、机器码和控制流/数据流证据；“设计已确定”不等于已完成双客户端实测。

## 分析和运行约束

静态分析只使用 x32dbg MCP 的模块、导入、特征扫描、反汇编和内存读取接口，没有设置断点、暂停、单步或使用 x32dbg 内存写入。动态观测由独立 x86 DLL 完成：它短暂改写三个已验证 `call rel32` 的 4 字节位移，替换七个 IAT 指针，并在命中指定 Steam 上下文后替换 `ISteamNetworking` 的 Send/Available/Read 三个虚表槽；卸载时恢复注入前值。

最新联机采集结束后的状态为：

- x32dbg：`running`；断点 `0`；调试器 patch `0`。
- `isaac-sync-probe.dll`：不在目标模块列表中。
- `RVA 0x50B8E5`：`E8 96 50 00 00`。
- `RVA 0x5110AB`：`E8 90 C5 FF FF`。
- `RVA 0x50B754`：`E8 97 F4 FF FF`。
- EOS GetNext/Receive/Send IAT 分别恢复为 `0x5E64BB50`、`0x5E64BBA0`、`0x5E64BB00`；`lua_pcallk` 恢复为 `0x6CA333A0`；`SteamInternal_ContextInit` IAT 恢复为 `0x6CA7A9D0`。
- 当前 `SteamNetworking006` 虚表 `0x58924080` 的前三槽恢复为注入前的 `0x57F12FF0 / 0x57F124B0 / 0x57F0FA20`，均位于 `steamclient.dll`；本轮没有加载 `tractor_beam_native_hook.dll`，因而同时补上了无该第三方网络 Hook 环境的被动采样。

## 当前地址账本

| 语义 | 当前 VA | RVA | 定位证据 |
| --- | --- | --- | --- |
| `g_Manager` 指针槽 | `0x0134169C` | `0x87169C` | 在线路径直接读取该全局槽 |
| 当前网络后端对象槽 | `0x01349A58` | `0x879A58` | 当前对象虚表 `0x01272440 = base + RVA 0x7A2440`，为 Steam 分支 |
| 输入推进调用点 | `0x00FDB8E5` | `0x50B8E5` | `E8 96 50 00 00`，目标 RVA `0x510980` |
| 8 字节身份匹配调用点 | `0x00FDB754` | `0x50B754` | `E8 97 F4 FF FF`，目标 RVA `0x50ABF0` |
| 精确分发候选 | `0x00FE10AB` | `0x5110AB` | `6A 00 8B CF E8 90 C5 FF FF 8B 8F B0 18 00 00` |
| Steam 接收轮询实现 | `0x01145280` | `0x675280` | Steam 虚表 `RVA 0x7A2440` 的 `+0x08` 槽 |
| Steam 发送实现 | `0x01145410` | `0x675410` | Steam 虚表 `RVA 0x7A2440` 的 `+0x14` 槽 |
| Steam 上下文描述符 | `0x0132C51C` | `0x85C51C` | 初始化器 RVA `0x676090`，版本字符串 `SteamNetworking006` |
| EOS GetNext/Receive/Send | `0x011461E8 / 0x01146241 / 0x0114654D` | `0x6761E8 / 0x676241 / 0x67654D` | EOS 虚表 `RVA 0x7A2530`，仅后端枚举 `3` 使用 |
| `lua_pcallk` IAT | `0x011E83D8` | `0x7183D8` | PE 导入名解析，不依赖运行时函数 VA |
| `MC_POST_UPDATE` pcall | `0x00F429CB` | `0x4729CB` | `FF 15 D8 83 1E 01`；返回地址 RVA `0x4729D1` |
| 输入发送/接收 | `0x00FD2950 / 0x00FD2010` | `0x502950 / 0x502010` | 原版输入消息发送与 map 插入 |
| 输入消费/推进 | `0x00FE0980` | `0x510980` | ready gate、逐设备应用、历史清理与 frame 递增 |

## 消息承载证据

### 当前激活的 Steam 后端

网络后端工厂在 `RVA 0x629370` 根据枚举值构造实现：case `2` 在 `RVA 0x629514` 构造对象并写入 Steam 虚表 `RVA 0x7A2440`；case `3` 在 `RVA 0x629600` 写入 EOS 虚表 `RVA 0x7A2530`。最新运行对象的虚表为 Steam 分支，故先前 EOS IAT 事件为零不是探针漏掉缓存指针，而是会话没有使用 EOS 收发实现。

Steam 实现通过 `SteamInternal_ContextInit` 和字符串 `SteamNetworking006` 取得旧版 `ISteamNetworking` 接口。发送 RVA `0x675410` 调用虚表槽 0，参数按 `SendP2PPacket(CSteamID, data, size, EP2PSend, channel)` 排列，并静态写入 `EP2PSend=1`、`channel=0`。接收 RVA `0x675280` 调用虚表槽 1/2，即 `IsP2PPacketAvailable(&size, 0)` 和 `ReadP2PPacket(..., &remoteSteamId, 0)`。

5 秒双客户端样本 `artifacts/probe-13272-steam-vtable-check.jsonl` 共 3196 个连续事件，sequence 缺口为 0：

- 416 次 Send 全部成功，全部 `channel=0`、发送模式 `1`；长度分布为 40 字节 273 次、16 字节 133 次、24 字节 10 次。
- 713 次 Available 全部请求 `channel=0`，其中 413 次有包；对应 413 次 Receive 全部成功且 `channel=0`。
- 发送和接收的远端 SteamID64 均为同一 peer；文档只保留截断形式 `steam:…592585`。

Steam 官方文档说明 channel 是同一 P2P connection 上的虚拟端口，接收方必须以相同 channel 读取；不同 channel 仍复用底层连接。因此当前 Steam 后端的 sidecar 固定使用 `0xFD`，原版 channel `0` 无需改写筛选；自定义同步记录使用 `k_EP2PSendReliable=2`，不照搬原版 `1`（`k_EP2PSendUnreliableNoDelay`）。参见 [ISteamNetworking::SendP2PPacket](https://partner.steamgames.com/doc/api/ISteamNetworking?l=english)。2026-08-03 的真实双机日志已通过逐包配对分析，故 Steam backend `2` 的该承载方式升级为“双端已实测”；结论不外推到 EOS backend `3`。

### Steam `0xFD` active 验证器

active 模式严格限定 backend `2`。启动时同时验证当前网络对象虚表为 `moduleBase + RVA 0x7A2440`，并要求 `SteamNetworking006` 的 Send/Available/Read 原始三槽都属于 `steamclient.dll`；EOS backend `3`、TractorBeam 或未知虚表 Hook 均 fail closed。下列机制先通过离线状态机测试，并于 2026-08-03 完成真实双机验收。

- disarmed 阶段只观察原版 channel `0`。发现唯一 SteamID64、累计 60 次成功 frame、30 次成功发送和 30 次成功接收后进入 `READY-DISARMED`；在此之前不会调用 `0xFD`。
- 两端必须使用同一 128 位 session 码分别手动 `--arm`。HELLO/ACK 完成后，每个成功推进最多产生一个新 PING；每次 frame update 前最多读取 64 个 `0xFD` 包，PONG 立即发送，因此 ready stall 仍可排空已到达包但不制造新 PING。
- sidecar 直接调用安装时保存的 Steam 原函数，固定 channel `0xFD`、`k_EP2PSendReliable=2`。原版调用继续经过观察 Hook，参数不被改写。
- 60 字节小端包包含 `ISCP`、协议版本、包类型、session、双方 instance、覆盖全部包类型的递增 `txSequence`、PING/PONG correlation、目标计数、发送帧和覆盖前 56 字节的 CRC32。
- peer、session、instance、版本、大小、CRC、txSequence、状态或 correlation 任一异常都会记录 `sidecar_reject`，停止新增 sidecar 流量并回到仅观察原版流量的失败状态。
- 默认每端发送 512 个 PING、回应 512 个远端 PING，再完成 FIN/ACK；之后 disarm 并继续观察至少 60 个成功 frame、30 次 channel `0` 发送和 30 次接收。

控制器接口：

```powershell
$session = .\build\RelWithDebInfo\isaac-sync-probe-injector.exe --new-session

.\build\RelWithDebInfo\isaac-sync-probe-injector.exe `
  --pid <pid> --mode active --peer auto --session $session `
  --count 512 --log .\artifacts\host.jsonl

.\build\RelWithDebInfo\isaac-sync-probe-injector.exe --status <pid>
.\build\RelWithDebInfo\isaac-sync-probe-injector.exe --arm <pid> --session $session
.\build\RelWithDebInfo\isaac-sync-probe-injector.exe --disarm <pid> --session $session
.\build\RelWithDebInfo\isaac-sync-probe-injector.exe --unload <pid>

.\build\RelWithDebInfo\isaac-sync-probe-injector.exe `
  --analyze-pair .\artifacts\host-a.jsonl .\artifacts\host-b.jsonl --session $session
```

active 记录器保持前台，准备阶段最多等待 5 分钟，武装后最多等待 90 秒。双日志分析器不按两机 QPC 排序，而以 session、sender instance、`txSequence`、kind 和 CRC 做逐包交叉对应；同时检查 JSONL sequence 连续、原版 channel `0`、原版 payload 不含 `ISCP`、每个成功 frame 恰好一次 candidate 且退出为 `F+1`，以及每端至少 300 次原版成功发送/接收和目标数量的成功 frame。

### 2026-08-03 Steam 双机实测

原始日志保存在 `build/RelWithDebInfo/host-a.jsonl` 和 `build/RelWithDebInfo/host-b.jsonl`。文档只记录一次性 session 的截断形式 `0c20…fe6d`，不记录完整 SteamID64。文件证据：

| 日志 | 大小 | SHA-256 | JSONL sequence |
| --- | ---: | --- | --- |
| host A | 68912299 字节 | `A0311B601E3C9B54DA125E38B59C6F0171293D4D0FEA575800F3B21DEA9D565E` | `1..141551`，缺口 0 |
| host B | 49269919 字节 | `4275177FAC55DDD537622E0389B20A522BE7236FCC4C40609731248DD68F9380` | `1..101239`，缺口 0 |

两端均依次进入 `DISCOVERING -> READY-DISARMED -> HANDSHAKING -> RUNNING -> FINISHING -> POST-OBSERVE -> PASSED`，最终 `failure=none`、`reject=0`。逐包验证结果：

- 每端发送和接收均为 1028 个 sidecar 包：HELLO 1、HELLO_ACK 1、PING 512、PONG 512、FIN 1、FIN_ACK 1。
- 两个 sender 的 `txSequence` 都严格为 `1..1028`；PING/PONG correlation 都严格为 `1..512`。
- A-send 与 B-receive、B-send 与 A-receive 按 session、sender instance、`txSequence`、kind、correlation 和 CRC 逐包完全相等；两个方向缺失 0、额外 0，sender instance 彼此不同。
- 全部 4112 条 sidecar Send/Receive 事件均为 channel `0xFD`、`k_EP2PSendReliable=2`、长度 60、结果成功；`sidecar_reject=0`。
- active 窗口中，A 的原版 channel `0` 成功发送/接收为 2929/2929，B 为 1977/1977；所有原版 Send/Available/Receive 的非零 channel 数为 0，原版 payload 首部出现 `ISCP` 的次数为 0。原版长度仍只出现 16、24、40 字节。

帧和 candidate 检查也覆盖了实际 stall：

| 主机 | active 到 summary 成功尝试 | stall | candidate/frame 异常 |
| --- | ---: | ---: | ---: |
| A | 851 | 657 | 0 |
| B | 574 | 18 | 0 |

每个成功尝试都恰好一次 candidate 并从 `F` 退出为 `F+1`；每个 stall 都没有 candidate 且保持 `F`。A 比 B 更早武装：等待第一个远端包期间仍有 278 次成功推进和 221 次 stall；随后 PING 阶段有 512 次成功推进和 389 次 stall，但最终 PING 仍恰好 512 个，证明 stall 没有制造重复 PING。FIN/ACK 后两端都停止 `0xFD`，再完成至少 60 个成功 frame 的 channel `0` 后观察。

终端输出显示两端均产生 `probe_started=1`、`probe_stopped=1` 并报告“target bytes restored”。本机 A 另行只读确认 `isaac-ng.exe` PID `32140` 仍正常响应且探针模块不存在。主机 B 的进程/模块状态只由其控制器输出证明；本次没有提供两端 x32dbg 的最终断点和 patch 列表，因此不能把 2026-08-01 的 x32dbg `running / breakpoint 0 / patch 0` 复查冒充为本次 active 会话的独立复查。

`overwritten_events` 的 A/B 值 137455/97143 都恰好等于 `总事件数 - 4096`，表示固定环形槽发生复用；由于外部记录器持续转存后的 JSONL sequence 均完整连续，这不是日志丢失数。

### EOS 备用后端

`RVA 0x67654D` 是当前映像中唯一直接调用 `EOS_P2P_SendPacket` 的位置。其 x86 V3 options 为 36 字节：

| options 偏移 | 字段 | 当前静态赋值 |
| --- | --- | --- |
| `+0x00` | `ApiVersion` | `3` |
| `+0x04` | `LocalUserId` | EOS PUID |
| `+0x08` | `RemoteUserId` | EOS PUID |
| `+0x0C` | `SocketId` | 原版 socket |
| `+0x10` | `Channel` | `0` |
| `+0x14` | `DataLengthBytes` | 调用方长度 |
| `+0x18` | `Data` | payload |
| `+0x1C` | `Reliability` | `1` |
| `+0x20` | `bAllowDelayedDelivery` | `0` |

接收循环构造的 GetNext V2 options 把 `RequestedChannel` 设为 null，因此当前原版会取任意 channel；Receive V2 也沿用同一策略。EOS 接口本身允许通过 `RequestedChannel` 筛选包，参见 [EOS_P2P_GetNextReceivedPacketSizeOptions](https://dev.epicgames.com/docs/zh-Hans/api-ref/structs/eos-p-2-p-get-next-received-packet-size-options)。

若运行时后端枚举为 `3`，承载边界为：

1. 自定义包不复用原版 kind `1` 和固定 `0x20` 输入 payload。
2. 自定义 channel 为 `0xFD`，复用原 Local/Remote PUID 和 SocketId，并使用可靠模式、保持 delayed delivery 为 `0`。
3. active sidecar 必须给原版 GetNext/Receive 传 options 副本，并把 `RequestedChannel` 固定为 `0`，否则原版有机会先消费 `0xFD`。
4. sidecar 在输入推进前单独轮询 `0xFD`，只复制到自己的临时队列。

EOS 分支尚未在本轮动态激活，不能把 Steam 的 416 包扫描结果外推到 EOS；启用该后端时仍需独立完成 channel 冲突和 PUID 关联验证。

## 稳定 `playerSlot` 证据和模型

`RVA 0x50B754` 调用的 RVA `0x50ABF0` 遍历同步管理器开头的 `NetInputDevice*` vector，比较消息传入的两个 dword 与 `device+0x374` identity 对象的 `+0x08/+0x0C`。

Steam Receive 直接输出 `CSteamID`。最新样本 148 次 identity-match 全部满足：

- 最近成功 Receive 的 `steam:…592585` 转为 uint64 后，低/高 dword 是文档截断的 `0x…9509 / 0x01100001`；
- 这两个 dword 与 identity-match 入参逐次相等；
- `device+0x374` 保存的两个 dword 也逐次相等；
- 返回设备固定为 vector index `1`。

因此当前 Steam 后端的游戏 8 字节 identity 已确认就是 SteamID64，而不是只能临时关联的匿名 8 字节值。设计固定为带后端标签的键：

```text
PeerKey = "steam:" + unsigned-decimal SteamID64   // backend 2
PeerKey = "eos:" + canonical ProductUserId string // backend 3，待动态验证
initial roster = all PeerKey values按字节序排序
playerSlot = initial roster中的冻结下标
```

- 玩家离开后保留 tombstone，本局不复用 slot。
- 同一 PeerKey 重连时恢复原 slot。
- `NetInputDevice*` 和临时 vector 下标只作为当前对象关联，不进入协议键。
- 若一个 peer 对应多个输入设备，最终键必须扩展为 `(PeerKey, localPlayerOrdinal)`。在原版三个元数据 dword 中找不到稳定 ordinal 前，明确不支持单 peer 多本地玩家。

SteamID64 在当前会话内的稳定关联已确认；“断线重连后仍恢复同一冻结 slot”和“单 peer 多本地玩家 ordinal”仍需单独动态验证。设备指针和 vector index 仍然只作为本次对象关联，不进入协议键。

## 精确分发候选

### 2026-08-06 双日志失败复盘

一次双端失败并不是 `RVA 0x5110AB` 漏装或 deterministic dispatcher
主动拒绝。真正的首个错误发生在 participant 日志 sequence `1718`：进入 frame `380`
后原版输入仍未 ready，online frame 保持 `380`，因此控制流没有到达 dispatch candidate；
frame wrapper 却报告 `DispatchStateViolation (307)`，错误文本为
`successful frame update did not dispatch exactly once`。另一端随后收到 ABORT，才在
sequence `1721` 以 `FrameStateViolation (126)` 进入 FAILED_FATAL。

根因是目标 `RVA 0x510980` 按 x86 C++ ABI 只在 `AL` 返回 `bool`，`EAX[31:8]` 不属于
返回值且允许保留任意旧数据。早期 Native 与研究 probe 都把函数声明成
`uint32_t (__thiscall*)(void*)`；当正常 stall 返回 `AL=0` 而高 24 位非零时，wrapper 会把
整个 `EAX` 误判为成功，再把“没有 candidate”错误升级为 fatal。修复后的 frame 函数指针
与 hook 都使用 `bool`，JSONL/probe 的 `result` 字段仍保持原布局，但只显式写入 `0` 或 `1`。
wire、IPC ABI、Lua API、frame/dispatch RVA 与分发顺序均不因此改变。历史日志继续作为失败
证据保存，不能通过放宽 analyzer 规则改写为 PASS。

RVA `0x510980` 的成功路径顺序如下：

1. RVA `0x510B47` 检查 ready gate；失败跳到返回路径，不进入候选调用，也不递增 frame。
2. RVA `0x510B70..0x510F93` 遍历设备。对 active device，RVA `0x510B8C` 以 `syncManager+0x0C` 当前 frame 查 `device+0x3A0`，随后更新 `device+0x380..` 的输入状态。
3. 设备循环结束并完成历史队列维护后，到达 RVA `0x5110A7: push 0; mov ecx,edi`。
4. RVA `0x5110AB` 调用 RVA `0x50D640`。
5. RVA `0x5110E8` 才执行 `inc [edi+0x0C]`。

因此精确候选不是宽泛窗口，而是 `RVA 0x5110AB` 的原调用执行前：全部设备输入已经应用，ready gate 已通过，`onlineFrame` 仍是当前 `F`。

探针在三个阶段为每个设备记录 `device+0x380` 开始的 32 字节核心输入状态、CRC32 和设备下标：

- phase `0`：进入 `RVA 0x50B8E5` 包装函数、调用 RVA `0x510980` 前。
- phase `1`：`dispatch_candidate_pre` 后、调用 RVA `0x50D640` 前。
- phase `2`：RVA `0x510980` 返回后。

同时记录 `frame_update_enter/exit`、`dispatch_candidate_pre/post` 和所有 `lua_pcallk` 的线程、返回地址 RVA、事件序号及 `onlineFrame`。只有满足以下条件，才把该候选升级为“动态已确认”：

- ready stall 没有 candidate 事件且 frame 不增长；
- 每个成功 frame 恰好一对 candidate pre/post；
- phase 1 的所有设备状态对应 frame `F` 已应用；
- candidate 时 `onlineFrame==F`，frame update 返回后为 `F+1`；
- candidate 与后续 simulation Lua 调用位于同一主线程，且 candidate 更早。

2026-08-01 的单客户端联机样本覆盖 frame `3870..4320`，451 次调用全部返回 `1`。每次严格出现一次 candidate pre/post，candidate 的 `onlineFrame` 和对象内 `+0x0C` 都等于 `F`，frame update exit 时都变为 `F+1`；451 次顺序检查和 frame 值检查的异常数均为 `0`。enter、candidate 和设备快照全部位于线程 `7852`。

这把候选升级为 **单客户端成功推进路径的帧级已确认**。该样本没有 ready stall、`lua_pcallk` 或按键变化；ready stall 和按键数据流由后续双客户端样本补齐，Lua 顺序由下面的 marker 样本补齐。

### 双客户端 ready stall 与输入变化实测

2026-08-01 的双客户端联机样本覆盖 frame `12030..12621`，共 608 次推进尝试：592 次返回 `1` 并推进，16 次返回 `0`。全部 6608 个 JSONL 事件从 sequence `1` 连续到 `6608`，缺口为 `0`。环形缓冲报告的 `2512` 是容量 4096 之后的槽位复用次数，不是本次日志丢失数。

ready stall 发生在两个 frame：

- frame `12394` 连续 15 次 `enter(F) -> exit(F, result=0)`，约阻塞 `250.78 ms`；随后同一 `F` 成功出现一次 candidate 并推进到 `12395`。
- frame `12396` 出现 1 次相同 stall，约 `17.67 ms`；下一次尝试成功推进到 `12397`。

16 次 stall 全部满足：candidate pre/post 为 `0`、phase 1 快照为 `0`、phase 0/2 各有两个设备且 CRC 不变。592 次成功尝试全部满足：candidate 恰好一次、phase 0/1/2 各有两个设备、exit frame 恰为 `F+1`。所有事件都在主线程 `33764`。

设备关联与按键变化：

- index `0`：`NetInputDevice*=0x0B2B00A4`。phase 0→1 在 45 个 frame 发生实际字节/CRC 变化，共观察到 28 种状态；这是本地按键设备。
- index `1`：`NetInputDevice*=0x0B2AEC7C`。590 次 identity-match 全部返回该设备，故判定为远端设备；identity 键恒为 `0x462C9509 / 0x01100001`，与 `device+0x374` 保存值逐次一致。
- 所有设备 phase 1→2 的 CRC 变化数为 `0`，证明 candidate 前已应用的输入状态在原调用返回前保持稳定。

这完成了 ready stall 与实际输入应用两项验收。该旧样本本身没有 `lua_pcallk`；后续专用 Mod marker 样本独立完成了成功推进时的 Lua 相对顺序验证。

### `MC_POST_UPDATE` 主线程与相对顺序实测

2026-08-01 在 PID `26860`、模块基址 `0x00AD0000` 的单客户端联机中启用只包含 `MC_POST_UPDATE` 的测试 Mod。探针在第一次游戏侧 `lua_pcallk` 前把测试专用 C 函数 `IsaacSyncProbeMark` 注册到同一个 `lua_State*`；Mod 每次回调传入 phase、`Game():GetFrameCount()` 和 Lua 侧递增序号。marker 热路径只写同一共享环形缓冲，不操作游戏状态。卸载前由下一次游戏线程 pcall 把该全局设为 `nil`，确认 `lua_marker_unregistered` 后才恢复 Hook 并释放 DLL。

20 秒原始日志为 [`artifacts/probe-26860-lua-order.jsonl`](../artifacts/probe-26860-lua-order.jsonl)：

- 共 6626 个事件，sequence 从 `1` 连续到 `6626`，缺口 `0`。环形槽复用计数为 2530，即 `6626-4096`；注入器持续转存成功，没有日志事件丢失。
- 记录 602 次 `dispatch_candidate_pre/post`、602 次 `lua_pcallk` 和 601 次 `lua_simulation_marker`。601 个相邻 candidate 区间各有且只有一个 marker；最后一个区间为零，是因为 sequence `6624` 已在游戏线程移除 marker，sequence `6625` 是执行该清理后的 pcall。
- 601/601 个 marker 都直接关联一个更早的 `lua_pcallk` sequence，关联失败为 `0`。全部 pcall 返回地址均为 RVA `0x4729D1`；对应调用指令位于 RVA `0x4729CB`：`call dword ptr [lua_pcallk IAT]`。
- candidate、pcall 和 marker 的线程集合都只有 `{22660}`。`probe_started.value0` 记录的游戏窗口线程和 `value1` 记录的进程最早创建线程也都为 `22660`；因此这里可称为同一主线程，而不只是未命名的 simulation worker。
- 线程不一致、marker/pcall 关联失败和顺序反转均为 `0`。每个 marker 的 `onlineFrame` 都恰好为对应 candidate 的 `F+1`；marker 参数校验结果全部为 `0`，phase 全部为 `1`，Lua game frame/sequence 从 `19144` 连续到 `19744`。

首帧的实际全序为：candidate pre sequence `7`、candidate post `9`、frame exit `10`（`F+1`）、`lua_pcallk` `13`、marker `14`。因此发布版 sidecar 在 RVA `0x5110AB` 原调用前分发，严格早于同一主线程后续的 `MC_POST_UPDATE` simulation callback。该次单客户端样本没有 ready stall；“stall 时不进入 candidate”仍由上面的双客户端 16 次失败尝试证明，没有把两场独立观测伪装成同一场联测。

## 无断点探针实现

源码入口：

- `CMakeLists.txt`
- `src/shared/probe_shared.hpp`
- `src/probe/probe_dll.cpp`
- `src/injector/injector.cpp`

实现特征：

- 只构建 Win32/x86；显式 PID，且目标映像名必须为 `isaac-ng.exe`。
- 安装前同时验证三个 callsite 机器码和 `RVA 0x5110A7` 的 15 字节上下文；不匹配即拒绝修改。
- 三个 `call rel32` 只替换 4 字节 displacement；写入时短暂挂起其他线程，随后立即恢复。
- EOS Send/GetNext/Receive、Lobby member/count、`lua_pcallk` 和 `SteamInternal_ContextInit` 按 PE 导入名替换 IAT，不依赖固定运行时 VA。
- Steam 上下文 Hook 只接受 `RVA 0x85C51C` 且首项初始化器等于 RVA `0x676090` 的描述符；取得 `SteamNetworking006` 后才替换其 Send/Available/Read 三槽。其他 Steam 接口不会被修改。
- Hook 热路径写固定容量 4096 的共享环形缓冲，不分配内存、不写文件；注入器转存 JSONL。
- Lua marker 只注册到第一个实际执行 pcall 的游戏 `lua_State*`；marker 事件携带包含它的 pcall sequence。卸载先要求游戏线程把全局设为 `nil`，清理失败则拒绝释放 DLL，避免留下悬空 Lua C 函数。
- marker 清理完成后恢复 call、IAT 和 Steam 虚表，再等待活动 Hook 连续 100 ms 为零，最后释放 DLL。
- passive 模式不发送包、不改变原版 channel 筛选；active 模式只允许 Steam backend `2`，并以 `READY-DISARMED`、正确 session 手动武装和虚表所有者检查限制启动。EOS backend `3` 明确拒绝 active。

构建和运行：

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A Win32
cmake --build build --config RelWithDebInfo
ctest --test-dir build -C RelWithDebInfo --output-on-failure

.\build\RelWithDebInfo\isaac-sync-probe-injector.exe `
  --pid <pid> --mode passive --log .\artifacts\probe.jsonl --duration 30

.\build\RelWithDebInfo\isaac-sync-probe-injector.exe --unload <pid>
```

JSONL 统一包含 sequence、QPC、线程、RVA、onlineFrame 和关联序号；网络事件还包含 PUID 或 SteamID64、SocketId、channel、发送模式、长度、CRC32 与最多 32 字节前缀。原始日志允许完整 peer ID，进入文档或提交前必须哈希或截断。

## 当前执行结果与限制

| 运行 | 结果 | 可证明内容 |
| --- | --- | --- |
| 中间版本 passive 20 秒 | 只有 `probe_started`，`onlineFrame=0`，无覆盖 | 目标当时空闲；不能证明网络、身份或时序 |
| 最终版本 smoke 3 秒 | `probe_started=1`、`probe_stopped=1`、覆盖 `0` | 最终安装/恢复/卸载路径可用 |
| 单客户端联机 passive 15 秒 | 451 组 enter/exit/pre/post、1353 个设备快照、覆盖 `0` | frame `3870..4320` 连续；每帧 candidate 一次；exit 恰为 `F+1`；约 `29.676 Hz` |
| 双客户端联机 passive 20 秒 | 608 次尝试、592 次成功、16 次 stall、590 次 identity-match | stall 不推进且无 candidate；45 个 frame 有实际按键应用；sequence 缺口 `0` |
| Steam 后端 passive 5 秒 | 416 Send、713 Available、413 Receive、148 identity-match、4 stall | 原版 channel 0 扫描通过；SteamID64 与游戏 identity/设备 148/148 一致；sequence 缺口 `0` |
| 最终竞态加固 smoke 3 秒 | `probe_started=1`、`probe_stopped=1`、91 次成功推进、183 次 Steam 轮询 | 最新 DLL 自动安装、采集、恢复和卸载成功 |
| Lua marker passive 20 秒 | 602 candidate、602 pcall、601 `MC_POST_UPDATE` marker | 601/601 次同主线程且 candidate 更早；窗口线程和最早线程均为 `22660`；sequence 缺口 `0` |
| Steam active 离线自测 | 60 字节布局/CRC、双内存状态机交错 HELLO/PING/PONG/FIN、错误 session/version/size/CRC/peer/sequence 与失败后零发送 | 实现级测试通过；没有注入或产生网络流量，不能替代真实双机验收 |
| Steam active 双机实测 | 两端各 1028 Send/Receive、512 PING/PONG、FIN/ACK；A/B active channel 0 成功收发 2929/2929、1977/1977 | 两方向逐包对应；sequence、correlation、CRC 无缺失/重复/乱序；reject、原版非零 channel、candidate/frame 异常均为 0 |

首次 20 秒运行使用的中间注入器只按未装饰名解析 `ProbeShutdown`，没有处理 x86 导出 `_ProbeShutdown@4`，导致 x32dbg 捕获了错误辅助线程的执行异常并短暂停住目标。这次运行不满足“全程不暂停”的验收条件。故障辅助线程没有执行游戏代码；恢复后修正为同时解析两种导出名，并由最终 smoke run 证明普通 `--unload` 可完成。全程没有新增断点；最终字节、IAT、模块和运行状态已逐项复查。

本次联机样本还给出以下进程内快照；这些 VA 只对 PID `29300` 的本次运行有效：

- `g_Manager = 0x0E221874`，其嵌入同步对象 `g_Manager+0x4B3D8 = 0x0E26CC4C`。
- 设备 vector 只有一个条目：`NetInputDevice*=0x0AF774E4`。
- `device+0x374 = 0x072C109C`，identity 匹配 dword 为 `0x1712D8F8 / 0x01100001`。
- 一次读取时 `onlineFrame=0x286F`、delay 源字段 `1`、delay 缓存 `1`、dirty `0`。
- `device+0x3A0` 和 `device+0x3AC` 的原始头部分别出现候选计数 `2` 和 `7`；没有进一步把它们当作稳定队列长度写入协议。

早期双客户端样本有第二设备和 identity-match，但 EOS/Lobby/Lua IAT 事件为 `0`。最新 x32dbg 静态与实时对象证据已经解释该现象：当前后端是 Steam case `2`，EOS case `3` 的代码没有执行。第一版 Steam 扩展曾把共用 `SteamInternal_ContextInit` 返回的第一个接口误当作 Networking，该次 12 秒日志只有 `steam_interface`，槽位已恢复且样本作废；最终版本增加描述符/初始化器双重校验后才得到上表的真实 P2P 数据。

尚未完成、不得写成实测确认的项目：

- 断线重连后 SteamID64 不变并恢复原 slot。
- 单 Steam peer 多本地玩家的稳定 `localPlayerOrdinal`。
- 若未来实际启用 EOS backend `3`，单独完成 EOS channel 扫描、PUID 与设备关联及重连验证。
- 本次 active 双机结束后的主机 B 进程/模块状态和双方 x32dbg `running / breakpoint 0 / patch 0` 尚未独立读取；只能引用控制器的恢复/卸载成功结果。

这些项目需要断线重连、本地多人、实际 EOS backend `3` 会话，或补充本次双机的运行后只读复查；它们不再阻塞“Steam backend `2`、两 peer、每 peer 单玩家、不支持重连”的首版实现。

最新 Lua marker 采集自动卸载后，x32dbg 确认 PID `26860` 仍为 `running`、断点 `0`、patch `0`、探针模块不存在；三个 callsite 已恢复原机器码，`lua_pcallk`/Steam/EOS IAT 和 `SteamNetworking006` 虚表也均恢复注入前值。更早的 Steam 采集曾在加载 TractorBeam 的环境中验证外层包装与恢复；本次虚表原始目标位于 `steamclient.dll`，补上了无 TractorBeam 环境的采样。
