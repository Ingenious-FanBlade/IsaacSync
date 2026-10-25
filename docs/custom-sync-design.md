# Isaac Sync 1.0.4 正式同步设计

## 文档状态

本文是 Isaac Sync 1.0.4 的权威产品契约，覆盖 Native、wire protocol、Lua API、IPC、`IssacOnlineSync`、日志和验收规则。实现与本文冲突时，以当前代码中的 fail-closed 安全边界和本文列出的固定常量为准。

1.0.4 的功能基线是：Steam 2–4 peer full mesh、确定性 frame operation、`Int32`/`Float64` typed payload、由 `syncId` 派生的 port、冻结玩家映射、IPC ABI 5、group analyzer，以及 Lua VM 重建后的安全退役与重新绑定。

Lua VM 生命周期以 [1.0.4 Lua State 生命周期契约](lua-state-lifecycle-design.md) 为准：`lua_close` 是 state-scoped 资源的权威退役边界，新 VM 在首个可信 simulation callback 前重新获得 `Isaac.Sync`，但不会热接续旧 ACTIVE session。

## 支持矩阵与正式身份

| 项目 | 1.0.4 契约 |
| --- | --- |
| 产品版本 | 根目录 `VERSION`，值为 `1.0.4` |
| Native | `IsaacSync.Native.dll`，Win32/x86 |
| 注入器 | `IssacOnlineSync.exe`，x86_64 |
| Helper | `IssacOnlineSync.Helper.x86.exe`，i686 |
| 输出 | `out/1.0.4/<Configuration>` |
| 网络后端 | backend `2` / `SteamNetworking006` |
| topology | 2–4 peer full mesh；slot 0 只协调握手，不中继 frame batch |
| wire | magic `ISCS`，protocol `5`，channel `0xFD` |
| IPC | magic `IS10`，ABI `5`，`Local\\IsaacSync.1.0.4.<pid>` |
| 日志 | `isaac-sync-1.0.4-<pid>-<timestamp>.jsonl` |
| Lua | `Isaac.Sync` 七项 API |

构建 fingerprint 纳入产品标识 `IsaacSync-1.0.4`、protocol、IPC ABI、channel、operation 大小、payload kind mask、最大 packet、最大 peer 数、topology、capability mask、Hook RVA 与目标镜像 `.text`。任何 endpoint 的 fingerprint、manifest、roster 或 capability 不一致都必须在 ACTIVE 前失败。

1.0.4 不与任何旧版或预发布产品身份协商或降级。protocol 1–4 的 golden vectors只用于验证生产 decoder 能稳定拒绝旧 packet。

## 传输、roster 与 Hook 时序

- 自定义数据复用游戏的 Steam P2P connection，固定使用可靠模式和 channel `0xFD`；原版 channel `0` 的发送、接收和 payload 不得修改。
- 所有 endpoint 从同一联机会话读取 2–4 个不同的 SteamID64，排序后冻结为 canonical sender slot。
- 玩家游戏内顺序单独冻结为 `senderSlot -> Isaac.GetPlayer(index)` 双射，并进入 roster/handshake hash。
- roster 冻结后不支持中途加入、缩容或 coordinator 选举。任一成员离开会结束整个 group session；只有新的 manager/frame baseline 才能建立新 session。
- 每条有向 peer 链路独立维护 sequence、ACK/重试和 session tombstone。迟到的旧 session packet 只能丢弃，不能重新激活会话。
- sidecar 接收和确定性分发发生在 `RVA 0x5110AB` 原调用之前：原版 frame 输入已应用、ready gate 已通过、`onlineFrame` 尚未递增。Lua 和游戏对象只允许在主线程访问。

Steam 虚表、关键 callsite、目标镜像或 Lua export 不匹配时不安装半套 Hook。第三方 Hook 链、EOS backend、多本地玩家和动态 roster 不属于 1.0.4 支持范围。

## `syncId`、派生 port 与 manifest

`syncId` 必须是 1–96 bytes 的合法 UTF-8，不能包含 NUL。port 算法固定为：

```text
input = UTF8("IsaacSync/port-v1")
      || LE16(syncIdByteLength)
      || UTF8(syncId)
digest = SHA256(input)
value  = LE32(digest[0..4])
port   = 1 + (value mod 65535)
```

相同 `syncId` 必须永远得到相同 port。两个不同 ID 派生到同一 port 时，注册直接返回 collision，不做线性探测、salt 或动态分配。

manifest entry 是 `(syncId bytes, version:u32, derivedPort:u16)`。canonical manifest 按 `syncId` 字节序排序后哈希，因此 Mod 加载顺序不影响结果。重复 ID、不同 handler 的重复注册、派生碰撞、非法 version 或 manifest 已冻结均返回错误且不改变现有状态。

canonical 空 manifest 合法。Steam roster 连续兼容且 manifest 始终为空时，Native 保留 2 秒单调时钟注册窗口；窗口到期后无需 Lua callback，直接在 frame 主线程边界自动冻结空 manifest 并启动握手。兼容状态中断会重置计时；窗口内成功注册首个 entry 会取消自动路径并恢复显式 `Start` 契约。

`Start` 在空或非空 manifest 上都合法。第一次成功调用只记录启动意图；Native 在包含该调用的完整 simulation Lua callback 成功返回后冻结 manifest，同一 callback sweep 中后执行的 Mod 仍可注册。callback 失败时保持收集状态；真正冻结后拒绝晚到注册。

## Protocol 5 wire 契约

所有多字节字段使用小端。sidecar header、控制包和 frame batch 均纳入 CRC 与严格长度校验。生产 decoder 要求：

- magic 为 `ISCS`，protocol version 精确为 `5`；
- message kind、flags、header size 和 payload size 均为已知规范值；
- required capability mask 为 `0b1111`；
- 最大 wire packet 为 `312` bytes；
- `FrameBatchPrefixV1.record_size` 精确为 `16`；
- 未知版本、kind、flag、payload kind、非规范 padding 或尾随字节全部拒绝。

`CustomFrameOpV2` 固定为 16 bytes：

```cpp
struct CustomFrameOpV2 {
    uint16_t port;
    uint16_t opcode;
    uint32_t payload_kind;
    uint64_t payload_bits;
};
```

payload 规范：

| kind | wire 规则 | Lua 类型 |
| --- | --- | --- |
| None | 四个字段全零，只表示 no-op | 不分发 |
| Int32 | `payload_bits[31:0]` 为二补码，高 32 位必须为零 | integer |
| Float64 | IEEE-754 binary64；只允许有限值；`-0.0` 规范化为 `+0.0` | float |

NaN、Infinity、非规范负零、未知 kind 和不规范高位必须在入队前或 decode 时拒绝。duplicate/conflict 比较使用 canonical kind/bits，不能按浮点近似值比较。

每个 sender/frame 在 wire 上至多一条 operation。FRAME_BATCH 包含连续 frame 的 1–19 条记录；没有业务操作的 frame 仍发送 no-op，以保证 all-slot ready gate 能确定推进。

## Session、握手与确定性推进

正式 hash domain 固定为：

- `IsaacSync/1.0.4/handshake-v5`
- `IsaacSync/1.0.4/roster-v5`
- `IsaacSync/1.0.4/peer-token`

握手覆盖 session、排序 Steam roster、玩家顺序映射、build hash、manifest hash、activation frame、lead frames、batch 上限、capability、peer 上限和 topology。

状态流为 `DORMANT -> DISCOVERING -> HELLO -> WARMUP -> ACTIVE`。只有所有 slot 的握手、manifest、roster、warmup 和 activation barrier 一致时才能进入 ACTIVE。

对 frame `F`，只有每个冻结 slot 都存在 custom record 时才能原子 Consume。分发严格按 sender slot 升序，每条非空 operation 恰好调用一次 handler；任意缺失、冲突、重复分发、handler 异常或 frame 状态错误会向全组传播 ABORT 并 fail closed。

## Lua API 与队列

```lua
Isaac.Sync.Register(syncId, version, handler)  -- port | nil, error
Isaac.Sync.Start()                             -- true | nil, error
Isaac.Sync.IsActive()                          -- boolean
Isaac.Sync.GetLocalSlot()                      -- 0..peerCount-1 | nil
Isaac.Sync.GetPeerCount()                      -- 2..4 | nil
Isaac.Sync.GetPlayerIndexForSlot(senderSlot)   -- playerIndex | nil[, error]
Isaac.Sync.Send(port, opcode, payload)          -- targetFrame | nil, error
```

handler 签名为 `(senderSlot, opcode, payload, frame)`。handler 不允许 yield；Lua registry、stack、pcall 或类型错误都视为 group fatal。Mod 必须通过 `GetPlayerIndexForSlot` 转换 sender slot，不能直接把 slot 当玩家下标。

`Send` 仅在 ACTIVE 接受。integer 必须落在 int32 范围，float 必须有限。合法调用按实际 Lua 调用顺序进入本地单调队列：

```text
target = max(onlineFrame + leadFrames, nextUnsealedFrame, queueCursor)
```

成功后 cursor 前进到下一 frame；同一 simulation callback 内多个 Mod 因而获得连续 target。future window 固定为 1024 frame。参数错误、queue full 或 target overflow 不得占用 frame、推进 cursor/sequence 或增加 accepted 计数。

## IPC ABI 5、GUI 与 JSONL

共享内存固定为：

```text
magic          = 0x30315349  // "IS10"
abi            = 5
mappingPrefix  = "Local\\IsaacSync.1.0.4."
mappingSize    = 1,967,200
eventOffset    = 1,120
eventSize      = 240
eventCapacity  = 8,192
```

typed dispatch snapshot 位于 offset `1104..1119`，包含 payload kind、必须为零的 `reserved_typed_payload` 和 payload bits。event 中对应字段位于 offset `96`、`100` 和 `104`。C++ static assertion、Rust reader 和 fixture 必须共同锁定所有 offset、size、padding 和 publish sequence。

JSONL 每条 typed operation 使用：

```json
{
  "payloadKind": "int32 | float64 | none",
  "payloadValue": 1.5,
  "payloadBits": "3ff8000000000000"
}
```

GUI 只展示状态、roster、mapping、统计和最后一次 typed dispatch，不提供手工 port、Send 或 roster 控件。analyzer 只接受同一 2–4 endpoint session 的 protocol 5 / ABI 5 日志，并验证 sequence、manifest、roster、fan-out、all-slot dispatch、kind/bits 和 event gap。

typed smoke 每个 session 首次 ACTIVE 时故意发送 NaN、正 Infinity 和负 Infinity 三个非法参数探针。analyzer 只豁免这些没有 operation identity 且没有 queue/sequence 副作用的 `LuaSendArgument` 事件。

## 构建、测试与发布门禁

- 根 `VERSION` 是产品版本唯一来源；版本工具的 `-Check` 必须确认 Cargo workspace、本地 lock 条目、MSBuild 输出和运行时产品身份一致。
- Debug/Release Win32 solution 构建只能写入 `out/1.0.4`。
- Native.Tests 覆盖 protocol 1–4 拒绝、protocol 5 golden vectors、typed payload、manifest/port、2/3/4 endpoint、frame queue、session 和 IPC ABI。
- Native.Tests 还覆盖空 manifest hash、2 秒自动启动门、空会话 activation，以及空/非空端点在 ACTIVE 前拒绝握手。
- Rust 执行 fmt、x64/i686 clippy、core/main/helper tests、`--self-test` 和 Win32 fixture。
- fixture 必须验证真实 LoadLibrary/unload、`IS10` mapping、typed event、异构 mapping 隔离和多 host 清理。
- 发布前在真实 Steam 2/3/4 peer 上运行相同 Release DLL、注入器、Helper 和 smoke Mod，覆盖延迟、成员退出、新局重建和完整 group analyzer。

不在 1.0.4 范围内：EOS、动态 roster、单 peer 多本地玩家、RPC、字符串/table/blob payload、同 sender/frame 多 operation、热升级或跨产品身份协商。

## 1.0.4 Lua State 生命周期契约

1.0.2 在观察到新的 `lua_State*` 后等待旧 state 再次进入 simulation `lua_pcallk` 才清理 registry refs；旧 VM 已经由游戏关闭时，这个条件不可达，会使新 VM无法重新绑定 `Isaac.Sync`。1.0.4 以 `lua_close` 为权威退役边界，以 `LUA_RIDX_MAINTHREAD` canonical root 和单调 generation 标识 VM，在新 state 的首个可信 callback 前重新注册 API。

完整状态机、ACTIVE reset 的 ABORT 语义、IPC 事件复用、Hook 安装/卸载顺序以及验收矩阵见 [Lua State 生命周期契约](lua-state-lifecycle-design.md)。该契约已纳入 1.0.4 正式基线；wire protocol 5、IPC ABI 5、`IpcEventV5` 布局和七项 Lua API 保持不变。
