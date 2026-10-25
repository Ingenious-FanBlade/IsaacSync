# Isaac Sync 1.0.4 Lua State 生命周期契约

## 文档状态

本文定义 Isaac Sync 1.0.4 对游戏 Lua VM 创建、绑定、关闭和重新绑定的正式行为，覆盖 Native、IPC 日志、analyzer 和真实联机验收。

本文是 1.0.4 [正式同步设计](custom-sync-design.md) 的 Lua 生命周期子契约。既有 Lua VM 生命周期路径已完成实现、自动化测试和真实 Steam 双端验收；1.0.4 新增的空 manifest 自动启动已完成自动化门禁，仍须按本文矩阵执行真实双端发布验收。1.0.4 继续使用 wire protocol 5、IPC ABI 5、`IpcEventV5` 布局和七项 Lua API；产品身份与 1.0.2 不兼容。

目标是让同一游戏进程能够安全经历多轮 Lua VM 重建：旧 VM 关闭时同步退役所有 state-scoped 资源，新 VM 在首个可信 simulation callback 前重新获得 `Isaac.Sync`。修复不尝试在原 ACTIVE session 中热接续。

## 问题与根因

1.0.2 使用一个裸 `lua_State*` 保存当前绑定，并把 Lua handler 作为该 state registry 中的引用。`Isaac.Sync` C closure 也只存在于该 state 的全局表中。游戏重建 Lua VM 后，新 VM 的 globals 和 registry 与旧 VM完全独立，因此不会继承 API、handler 或 manifest。

当前迁移逻辑在可信 `lua_pcallk` 观察到另一 `lua_State*` 后设置 `g_lua_cleanup_pending`，但只有旧指针再次命中同一个 simulation pcall 边界时才会清除旧 registry refs、移除 `Isaac.Sync` 并把绑定置空。真实销毁顺序则是：

```text
旧 VM 停止执行 -> lua_close(旧 root) -> 新 VM 开始执行 callback
```

因此 DLL 第一次看到新 state 时，旧 state 通常已经不可能再次进入 `lua_pcallk`。迁移会永久停在 cleanup pending，新 state 无法执行 `RegisterLuaApi`，运行时最终保持 `FAILED_PRE_ACTIVE` 或 `FAILED_FATAL`。历史日志中的 error 204：

```text
a different game lua_State was observed before cleanup
```

与这条不可达清理路径一致。

裸指针比较还存在第二个问题：Lua coroutine 也使用 `lua_State*`，但它与主线程共享同一个 global state 和 registry。把 coroutine 指针与 VM root 指针直接比较，会把同一 VM 内的线程切换误判为 VM 重建。

## 已确认的目标构建依据

当前受支持的 `isaac-ng.exe` 直接从 `Lua5.3.3r.dll` 导入：

- `lua_newstate`；
- `luaL_newstate`；
- `lua_close`；
- `lua_pcallk`。

当前构建的 LuaEngine 创建路径在 `RVA 0x46057B` 调用 `lua_newstate`，或在 `RVA 0x4605A3` 调用 `luaL_newstate`，随后把返回的 root 保存到 `this+0x18`。teardown 在释放 `this+0x10`、`this+0x14` 持有的 registry 引用后，于 `RVA 0x46E5C0` 调用 `lua_close(this+0x18)`，返回后把 `this+0x18` 清零。

`Lua5.3.3r.dll` 同时导出 `lua_tothread`。Lua 5.3 registry 的 `LUA_RIDX_MAINTHREAD` 可从任意同 VM coroutine 解析 canonical main thread，因此无需再以 callback 传入的裸指针代表 VM 身份。

这些地址是当前构建的验证证据，不替代安装前的 PE import、目标镜像和机器码校验。未来游戏构建改变调用点或销毁顺序时，必须重新验证后再更新 fingerprint。

## 生命周期模型

### VM 身份

运行时使用以下身份：

```text
canonicalRoot(L):
    top = lua_gettop(L)
    lua_rawgeti(L, LUA_REGISTRYINDEX, LUA_RIDX_MAINTHREAD)
    root = lua_tothread(L, -1)
    lua_settop(L, top)
    return root
```

解析必须在受支持的主线程 Lua 调用边界进行，成功和失败都必须恢复原 stack top。空值、类型错误、stack 扩容失败或 SEH 异常均视为绑定失败，不能退回裸指针比较。

`canonicalRoot(L)` 相同表示 callback 属于同一 VM generation。所有七项 Lua API 的 state 校验也必须比较 canonical root，而不是要求传入指针精确等于保存的 root；这样同 VM coroutine 可以安全通过身份检查，但仍必须满足游戏主线程约束。

### Generation

只有被可信 simulation pcall 选中的 VM 才获得 generation。`next_generation` 是 DLL 进程生命周期内单调递增的 `uint64`，首次成功绑定取值为 1；每次成功绑定新的 canonical root 时递增。计数溢出时拒绝绑定并 fail closed，不能回绕。

generation 而不是指针是对外诊断身份。内存分配器即使复用相同的 root 地址，新 VM仍会获得新的 generation。创建但从未承载可信 simulation callback 的辅助 VM保持 generation 0。

### 状态机

```text
                    trusted simulation pcall
Unbound  ---------------------------------------------->  Bound(generation)
   ^                                                          |
   |                                                          | lua_close(bound root)
   |                                                          v
   +------------------------------------------------------  Closing
                       original lua_close returned
```

状态含义：

- `Unbound`：没有可调用的 `Isaac.Sync` closure、handler ref 或冻结 manifest；允许可信 pcall 选择新 root。
- `Bound(generation)`：恰好一个 canonical root 拥有 `Isaac.Sync` 和该 generation 的 registry refs。
- `Closing`：已经禁止新的 Lua API、Send、handler dispatch 和 session 启动；只允许对仍存活的 bound root 执行退役清理，然后调用原版 `lua_close`。

任何时刻最多有一个 bound generation。候选辅助 VM不进入这个状态机。

## Hook 契约

### 创建 Hook

主模块 IAT 同时 Hook `lua_newstate` 和 `luaL_newstate`。wrapper 必须先且仅调用一次原函数；返回非空 root 后发布 `LuaStateCreated`。创建事件只用于关联和诊断，不注册 API、不分配 generation，也不改变当前绑定或 session。

如果某个创建实现内部调用另一创建函数，wrapper 以 thread-local create depth 抑制同一次创建的重复事件；只有最外层返回发布一次。当前目标通过主模块 IAT 分别调用二者，但该规则防止未来实现产生双计数。

注入可能发生在游戏 root 已创建之后。因此首个 bound generation 可以没有先行的 `LuaStateCreated`，analyzer 必须允许这一种情况。

### `lua_pcallk` Hook 与绑定

现有 `IsMainLuaCall` 返回地址、主线程 ID 和目标构建验证继续定义“可信 simulation pcall”。只有该调用点能够选择 active root。

处理顺序为：

1. 若生命周期为 `Closing`，不得绑定或调用 generation-scoped 逻辑，直接进入原版 pcall。
2. 计算当前 callback 的 canonical root。
3. `Unbound` 时分配下一 generation，并在调用原版 callback **之前**向该 root 注册七项 `Isaac.Sync` API；这保证新 VM的第一个 Mod callback 已能调用 API。
4. `Bound` 且 canonical root 与 bound root 相同时保持幂等，不重复建表或增加 generation。
5. `Bound` 且 canonical root 不同时，按“未观察到 close 的异常 root 替换”处理，执行 error 204 策略；不得访问旧 root，不得让新 root 接管，也不得进入等待旧 pcall 的 cleanup pending。
6. 原版 pcall 返回后，只有 callback root 和 generation 仍与进入时一致，才允许冻结本轮 Start 请求的 manifest。

Lua API 表仍通过 raw registry globals 和 raw table 操作注册，避免只读 metatable触发未保护错误。绑定失败不能留下半张 `Isaac.Sync` 表、半初始化 generation 或 `IpcFlagLuaBound`。

### `lua_close` Hook

`lua_close` 是 VM 退役的唯一权威边界。wrapper 必须先分类参数：

- 参数等于 bound canonical root：执行完整 bound generation 退役；
- 其他非空 root：作为辅助 VM关闭，只发布生命周期事件；
- 空参数：记录异常并透传，不能修改 active generation。

bound root 的关闭按以下固定顺序执行：

1. 原子地从 `Bound` 转为 `Closing`，发布 `LuaStateClosing`；重复/重入 close 不得再次终止 session 或清理 refs。
2. 关闭 Lua API gate，拒绝新的 `Register`、`Start`、`Send`、查询和 handler dispatch。
3. 按当前 session 阶段执行组内终止策略。
4. 清空 dispatch guard、local/remote frame store、发送队列、pending fanout、roster 和所有 generation-scoped native 状态。
5. 在 root 仍有效时逐个 `luaL_unref` handler，移除 raw `Isaac.Sync`，重置 manifest hash、Start gate 和 invalid latch，并记录实际释放的 ref 数。
6. 清除 `IpcFlagLuaBound`、`IpcFlagStartRequested`、`IpcFlagManifestFrozen`、session flags 和 manifest snapshot。`IpcFlagLuaCleanupPending` 不参与正常 close。
7. 在 native 状态中注销 bound root；之后只保留局部数值用于日志，不允许再次把该地址传给任何 Lua API。
8. 恰好调用一次原版 `lua_close(root)`。
9. 原函数返回后发布 `LuaStateRetired`，从 `Closing` 转为 `Unbound`；若未处于 DLL shutdown，则清除正常 close 引起的 error 204 并回到 `DORMANT`。

正常目标路径要求 close 发生在记录的游戏主线程且没有 handler dispatch 正在执行。若线程、重入深度或 Lua 清理调用不满足约束，运行时必须先终止 session并记录 `LuaCleanupUnsafe`；仍须调用原版 `lua_close`，并在其返回后丢弃全部 native generation 状态。`lua_close` 本身销毁 registry 和 C closure，因此异常清理不得以阻止游戏关闭或继续解引用失效 VM作为补救。

原版 `lua_close` 抛出结构化异常时不得吞掉游戏异常；wrapper 的 finally 路径只负责使 native gate 保持关闭并禁止后续访问该 root。

## Session 终止和恢复

### ACTIVE 或 FAILED_FATAL

bound root 在 ACTIVE 期间关闭会立即使当前 session 不可继续：handler、manifest 和未来帧队列都属于即将销毁的 generation。运行时必须调用幂等的 group abort 路径，向每个其他 slot 至多发送一次 ABORT，使所有 endpoint fail closed。

ABORT 后本地退役 session和 packet tombstone，设置 `require_new_baseline`，不再推进或消费原 session 的 frame，不尝试在新 generation 中恢复旧 session。`lua_close` 完成后本地进入 `DORMANT`；只有新 manager/frame baseline、新 generation 的 `Register/Start` 和完整握手才能再次 ACTIVE。

已经处于 `FAILED_FATAL` 且 ABORT 已发送时不得重复发送。

### DISCOVERING、HELLO 或 WARMUP

尚未 ACTIVE 的 bound session 通过幂等 `EndGracefully` 向组内发送一次 GOODBYE，再以 `notify_remote = false` 退役本地 session。此路径不生成同步 failure；新 generation 仍须重新 `Register/Start`。

### DORMANT 或无 session

只清理 Lua generation，不发送 channel `0xFD` 控制包。正常 close 不是 error 204，也不能留下 `FAILED_PRE_ACTIVE`。

### 未观察到 close 的 root 替换

若可信 pcall 在 `Bound` 状态解析出不同 canonical root：

- 发布一次 latched error 204，detail 明确为 `a different canonical Lua root was observed without lua_close`；
- ACTIVE 时执行一次 ABORT并停止 frame 推进；pre-active 时结束 sidecar，让原版继续；
- 保留旧 generation 身份，但绝不再通过观察到的新 root 操作旧 registry；
- 不设置 `IpcFlagLuaCleanupPending`，不等待旧 root 再次进入 pcall；
- 旧 root 后续若正常进入 `lua_close`，按关闭流程退役，随后允许新 root 的下一次可信 pcall重新绑定；旧 root 永不关闭时保持 fail-closed。

error 204 因而只表示缺失权威 close 边界的生命周期异常，不再用于正常新局重建。

## Generation-scoped 状态

以下内容不得跨 generation 继承：

- handler registry refs、manifest entries/hash 和 collision latch；
- Start requested/frozen 状态；
- `Isaac.Sync` C closure 和 Lua globals 修改；
- local send cursor、sealed/pending records 和 remote frame stores；
- prepared fanout、dispatch guard和最后一次 dispatch identity；
- roster、slot/player mapping、activation、session manager和 session token。

进程级 Hook、Steam context observer、build fingerprint、IPC mapping、累计诊断计数和 `next_generation` 保持存活。新 generation 中存在 IsaacSync Mod 时必须重新执行 `Register/Start`，即使其 manifest 与上一 generation 字节相同；没有注册项时，Native 可在兼容 roster 的 2 秒注册窗口后重新建立空 manifest session。

## IPC、日志和兼容性

### 产品身份

正式产品版本为 1.0.4，并继续使用 wire protocol 5、IPC ABI 5、`IpcEventV5` 大小和七项 Lua API。生命周期修复不改变 packet payload 或共享内存布局。

实现时必须更新 build fingerprint 的 implementation revision，例如：

```text
typed-payload-derived-port-v5-lua-generation-empty-manifest-autostart-r3
```

因此修复前后的 DLL 即使 protocol 和 ABI 相同，也不能完成同一个 group handshake。

### 生命周期事件

在 ABI 5 的空闲 event type 中固定增加：

| type | 名称 | 发布时机 |
| --- | --- | --- |
| 23 | `LuaStateCreated` | 创建 wrapper 返回非空 root 后 |
| 24 | `LuaStateClosing` | 分类完成且 bound gate 关闭后、清理前 |
| 25 | `LuaStateRetired` | 原版 `lua_close` 返回且 native 状态已 Unbound 后 |

复用 `IpcEventV5` 现有字段：

- `value0`：bound generation；尚未绑定的辅助 VM为 0；
- `value1`：32 位 canonical root 地址零扩展为 `uint64`；
- `kind = 1`：由 `lua_newstate` 创建；
- `kind = 2`：由 `luaL_newstate` 创建；
- `kind = 3`：bound root 的 bind/closing/retired 事件；
- `kind = 4`：auxiliary root 的 closing/retired 事件；
- `count`：关闭时实际释放的 handler ref 数，其他事件为 0。

`LuaBindBegin` 和 `LuaBound` 同样写入 `value0 = generation`、`value1 = canonical root`、`kind = 3`。现有 `LuaCleanupComplete` 只表示 DLL 主动卸载期间成功移除仍存活 VM中的 C closure，不再表示正常 state reset。

生命周期事件不得包含 SteamID、用户名、绝对路径或 Lua对象内容。

### Analyzer 规则

analyzer 对单进程事件流验证：

- 非零 generation 严格递增且不复用；
- 同一时刻最多一个 bound generation；
- `LuaBound(g)` 之后只能由同 generation 的 `LuaStateClosing/Retired` 结束；
- 每个正常关闭的 bound generation 恰有一次 Closing 和一次 Retired，顺序严格递增；
- 初始 `LuaBound` 可以没有 Created，后续观察到的创建/关闭应按 root 地址关联；
- 同一地址在 Retired 后可以重新 Created/Bound，但必须获得更大的 generation；
- type 23–25 本身不是 failure。只有事件携带非零 error、error 204/205、ABORT 或失败状态时才按现有严格规则报告失败。

## Hook 安装、回滚和 DLL 卸载

生命周期 Hook 与 frame、dispatch、Steam context Hook 是同一事务。安装 wrapper 时 `hooks_ready` 保持 false；建议顺序为 `lua_close`、两个 creator、`lua_pcallk`、frame、dispatch、Steam observer，全部成功后才发布 ready。任一 import 缺失、IAT slot 非原始目标或 callsite 校验失败时，按逆序恢复所有已安装 patch，不能运行半套生命周期观测。

DLL 主动卸载不调用游戏的 `lua_close`。如果仍有 bound generation：

1. 关闭新的 Lua API和 dispatch入口；
2. 请求在仍存活 root 的下一次可信 simulation pcall 移除 handler refs 和 `Isaac.Sync`；
3. 若游戏在等待期间调用 `lua_close`，close wrapper 的完整退役也满足 cleanup complete；
4. 5 秒内两条路径均未完成则拒绝卸载并保持 DLL 加载；
5. cleanup 完成后恢复 frame、dispatch、Steam、creator 和 `lua_pcallk` 等普通 Hook；
6. 最后恢复 `lua_close` IAT，确保之前所有存活 closure 已移除或其 VM 已关闭；
7. 等待 `g_active_hooks` 连续 100 ms 为零后才允许 `FreeLibraryAndExitThread`。

任一 IAT 已被第三方替换时按现有所有权规则处理：只恢复仍指向本 DLL wrapper 的 slot；无法证明完整恢复时拒绝释放 DLL。

## 测试与验收

### Native 状态机测试

- 首个可信 pcall绑定 generation 1，重复 pcall 不重复注册或递增。
- root 和由其创建的 coroutine 解析为同一 canonical root，不触发 error 204。
- 辅助 VM创建、callback 和 close 不改变 active generation或 manifest。
- bound root close 恰好清理一次 refs、API、session和 flags；重入不重复 ABORT/GOODBYE。
- 原版 `lua_close` 对每次 wrapper 调用恰好执行一次，调用后没有任何 Lua API访问该地址。
- root 地址在 Retired 后复用，下一次绑定获得更大的 generation。
- 不经过 close 的不同 canonical root 被拒绝接管，并只产生一次 latched error 204。
- 创建/注册中途失败不留下半绑定 API、generation 或 IPC flags。
- ACTIVE close 发送一次 ABORT；DISCOVERING/HELLO/WARMUP close 发送一次 GOODBYE；DORMANT close 不发包。
- 正常 close 不设置 `LuaCleanupPending`；主动卸载仍覆盖 pcall cleanup、close-race 和 timeout 拒绝卸载。
- lifecycle event 的 type、value、kind、count 和 ABI 5 序列化保持固定。
- 空 manifest 自动启动门在 1999 ms 保持等待、2000 ms 准时触发，并在 roster 不兼容、注册、Start 或 reset 时清零。

### 真实进程和双端集成

在不重启游戏进程的情况下，双方连续执行三轮：

```text
进入联机局 -> 形成 ACTIVE -> 退出 -> 等待旧 generation Retired -> 进入新联机局
```

每轮要求：

- 旧 generation 有且仅有一组 Closing/Retired，新 generation 单调递增；
- 新 state 的第一个 Mod callback 已能访问 `Isaac.Sync` 并重新 `Register/Start`；
- 新 manager/frame baseline 后能够重新完成握手并进入 ACTIVE；
- 不出现正常 reset 引起的 error 204、永久 `LuaCleanupPending` 或 `FAILED_PRE_ACTIVE`；
- 不继承旧 handler ref、manifest freeze、Send queue、dispatch guard 或 session token；
- 双端 group analyzer 通过，事件序列无 gap。

另设 ACTIVE 强制 reset 用例，确认每个 endpoint 对原 session 至多发送一次 ABORT、原 session 不热接续，并能在下一局建立全新 session。最后执行 DLL 主动卸载，确认 closure 已移除、所有 IAT/call patch 恢复、活动 Hook归零且游戏进程继续运行。

另设不启用任何 Mod 的双端用例，确认没有可信 Lua callback 也能在 2 秒窗口后完成空 manifest handshake、warmup 和 activation；一端为空而另一端注册 Mod 时必须在 ACTIVE 前因 manifest hash 不一致而 fail closed。

## 发布验收

1.0.4 正式契约要求以下发布门禁；真实 Steam 项目必须在发布前单独验收：

- Lua 生命周期 IAT 的静态校验和真实调用顺序完成复验；
- Native、Rust IPC reader、JSONL 和 analyzer 测试全部通过；
- Debug/Release Win32 构建、self-test 和 fixture 通过；
- Steam 双端三轮重建和 ACTIVE reset 验收通过；
- 修复前后 fingerprint 握手不兼容得到验证；
- 自动卸载验证确认不存在悬空 Lua C closure 或未恢复 Hook。

