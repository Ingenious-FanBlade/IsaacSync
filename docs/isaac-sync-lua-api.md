# Isaac Sync 1.0.4 Lua API 参考

## 使用前提

所有 2–4 个 endpoint 必须同时部署字节相同的 `IsaacSync.Native.dll`、`IssacOnlineSync.exe`、x86 Helper 和 Mod 文件。1.0.4 使用 wire protocol 5、IPC ABI 5、派生 port 和 `Int32`/`Float64` typed payload。

不使用 IsaacSync Mod 时无需调用任何 Lua API：Steam roster 连续兼容 2 秒后，Native 会自动冻结空 manifest 并启动。需要发送自定义操作的 Mod 必须在 simulation callback 中等待 `Isaac.Sync` 可用，再完成注册和启动。所有 endpoint 必须形成相同的 canonical manifest；任一构建、roster、玩家映射、capability 或 manifest 不一致都会 fail closed。

## API 一览

```lua
Isaac.Sync.Register(syncId, version, handler)  -- port | nil, error
Isaac.Sync.Start()                             -- true | nil, error
Isaac.Sync.IsActive()                          -- boolean
Isaac.Sync.GetLocalSlot()                      -- 0..peerCount-1 | nil
Isaac.Sync.GetPeerCount()                      -- 2..4 | nil
Isaac.Sync.GetPlayerIndexForSlot(senderSlot)   -- playerIndex | nil[, error]
Isaac.Sync.Send(port, opcode, payload)          -- targetFrame | nil, error
```

## `Register`

```lua
local port, err = Isaac.Sync.Register(syncId, version, handler)
```

- `syncId`：1–96 bytes 的合法 UTF-8，不能包含 NUL。
- `version`：正整数，必须能无损表示为 `uint32`。
- `handler`：不可 yield 的 Lua 函数。
- 成功返回 `1..65535` 的派生 port；失败返回 `nil, error`。

port 只由 `syncId` 决定。Mod 必须保存本次 `Register` 的返回值，并把它传给 `Send`；不得硬编码或自行重算 port。两个不同 ID 派生碰撞时注册失败。

handler 签名：

```lua
local function handler(senderSlot, opcode, payload, frame)
    -- 每个 endpoint 在同一 frame 按 senderSlot 升序调用
end
```

`payload` 的 Lua 数值子类型由 wire kind 保留：Int32 到达为 integer，Float64 到达为 float。即使数值相等，`1` 与 `1.0` 也不能视为相同类型。

## `Start`

```lua
local ok, err = Isaac.Sync.Start()
```

`Start` 无参数且幂等，空 manifest 也合法。第一次成功调用只记录启动意图；Native 会在包含该调用的完整 simulation Lua callback 成功返回后冻结 manifest。同一 callback sweep 中后执行的其它 Mod 仍可执行 `Register` 和 `Start`。对空 manifest 调用 `Start` 可以跳过 2 秒自动等待。

manifest 真正冻结后，晚到注册返回 `manifest frozen`。callback 抛错时本次 sweep 不冻结，等待下一次成功边界。

若始终没有成功的 `Register` 或 `Start`，Native 会在兼容 roster 保持 2 秒后自动冻结空 manifest。计时期间 roster 失效会重新计时；自动冻结后再调用 `Register` 同样返回 `manifest frozen`。

## 状态和玩家映射

```lua
local active = Isaac.Sync.IsActive()
local localSlot = Isaac.Sync.GetLocalSlot()
local peerCount = Isaac.Sync.GetPeerCount()
local playerIndex, err = Isaac.Sync.GetPlayerIndexForSlot(senderSlot)
```

- `IsActive` 只在 group session 完成握手、warmup 和 activation barrier 后返回 `true`。
- `GetLocalSlot` 在 roster 可用时返回本地 canonical slot。
- `GetPeerCount` 返回 `2..4`。
- `GetPlayerIndexForSlot` 返回游戏内玩家顺序索引。

sender slot 是协议身份，不是 `Isaac.GetPlayer` 下标。正确写法：

```lua
local playerIndex, err = Isaac.Sync.GetPlayerIndexForSlot(senderSlot)
if playerIndex == nil then
    error(err or "player mapping unavailable")
end
local player = Isaac.GetPlayer(playerIndex)
```

## `Send`

```lua
local targetFrame, err = Isaac.Sync.Send(port, opcode, payload)
```

- `port` 必须是当前 manifest 中 `Register` 返回的 port。
- `opcode` 必须是 `0..65535` 的整数。
- integer payload 必须在 `-2147483648..2147483647`。
- float payload 必须有限；NaN 和正负 Infinity 被拒绝；`-0.0` 规范化为 `+0.0`。

成功只表示 operation 已分配 future target，不会立即调用本地 handler。同一 simulation callback 中的多次合法调用按 Lua 实际顺序获得连续 target。future window 最多 1024 frame；满时返回 `nil, "send queue full"`。

任何参数、范围、queue full 或 overflow 失败都不会占用 target、推进 queue/sequence 或污染下一次合法发送。

## 推荐模板

```lua
local mod = RegisterMod("My Synchronized Mod", 1)

local SYNC_ID = "com.example.my-sync-mod"
local SYNC_VERSION = 1
local OPCODE_ACTION = 1

local syncPort = nil
local startRequested = false
local initializationFailed = false

local function onOperation(senderSlot, opcode, payload, frame)
    if opcode ~= OPCODE_ACTION then
        return
    end

    local playerIndex, err = Isaac.Sync.GetPlayerIndexForSlot(senderSlot)
    if playerIndex == nil then
        error(err or "player mapping unavailable")
    end

    local player = Isaac.GetPlayer(playerIndex)
    -- 只使用同步参数和确定性游戏状态。
    Isaac.DebugString(string.format(
        "sync frame=%d slot=%d player=%d payload=%s",
        frame, senderSlot, playerIndex, tostring(payload)))
end

local function ensureStarted()
    local sync = Isaac.Sync
    if type(sync) ~= "table" or type(sync.Register) ~= "function" then
        return nil
    end
    if initializationFailed then
        return nil
    end

    if syncPort == nil then
        local port, err = sync.Register(SYNC_ID, SYNC_VERSION, onOperation)
        if port == nil then
            initializationFailed = true
            Isaac.DebugString("Register failed: " .. tostring(err))
            return nil
        end
        syncPort = port
    end

    if not startRequested then
        local ok, err = sync.Start()
        if not ok then
            initializationFailed = true
            Isaac.DebugString("Start failed: " .. tostring(err))
            return nil
        end
        startRequested = true
    end

    return sync
end

function mod:PostUpdate()
    local sync = ensureStarted()
    if sync == nil or not sync.IsActive() then
        return
    end

    if Input.IsButtonTriggered(Keyboard.KEY_K, 0) then
        local target, err = sync.Send(syncPort, OPCODE_ACTION, 1)
        if target == nil then
            Isaac.DebugString("Send failed: " .. tostring(err))
        end
    end
end

mod:AddCallback(ModCallbacks.MC_POST_UPDATE, mod.PostUpdate)
```

## 确定性约束

- handler 不得读取本地时间、随机数、未同步输入、进程地址或只存在于单端的对象。
- handler 不得 yield；未处理异常会终止整个 group session。
- 不要在 handler 中根据客户端角色分支出不同游戏状态。
- 所有端必须启用相同 Mod 集和相同 manifest version。
- 同一 sender/frame 在 wire 上只有一条 operation；需要多次操作时让 `Send` 分配连续 future frame。
- 当前不支持 Float32、字符串、table、blob、数组、RPC 或动态 roster。

## 常见错误

| 错误 | 原因 | 处理 |
| --- | --- | --- |
| `Isaac.Sync` 为 `nil` | Native 尚未绑定 Lua API或运行环境不兼容 | 在后续 simulation callback 重试，并检查注入器状态 |
| `manifest frozen` | 注册发生在成功冻结边界之后 | 把所有注册放到稳定的初始化 callback |
| `port not registered` | 使用了手工 port 或其它 entry 的 port | 只使用 `Register` 的实际返回值 |
| `send queue full` | 本地 future window 达到 1024 frame | 降低提交频率，等待队列推进 |
| `payload must be finite` | float 是 NaN 或 Infinity | 在调用前验证数值 |
| session 进入 fatal | handler、wire、frame 或 peer 状态不一致 | 收集该组所有 JSONL，用 group analyzer 定位首个错误 |

## 日志与组分析

日志文件名为 `isaac-sync-1.0.4-<pid>-<timestamp>.jsonl`。typed operation 同时记录 `payloadKind`、可读 `payloadValue` 和精确 `payloadBits`。

```powershell
& .\out\1.0.4\Release\IssacOnlineSync.exe `
    --analyze-group .\peerA.jsonl .\peerB.jsonl
```

analyzer 需要同一 session 的 2–4 份完整日志，并验证 roster、slot、packet sequence、fan-out、all-slot dispatch、event gap 与 kind/bits。离线测试不能替代真实 Steam 多端验收。

权威示例：

- `test-mod/isaac-sync-typed-payload-smoke`
- `test-mod/isaac-k-to-treasure-room`
- `test-mod/isaac-k-toggle-debug-10`
