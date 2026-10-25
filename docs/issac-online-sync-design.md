# IssacOnlineSync 1.0.4 设计

## 目标与公开名称

`IssacOnlineSync` 是 Isaac Sync 1.0.4 的 Rust/GPUI 自动注入器。`Issac` 拼写是固定的公开产品名称，不自动改为 `IsaacOnlineSync`。

| 组件 | 名称与架构 |
| --- | --- |
| 主程序 | `IssacOnlineSync.exe`，x86_64，Windows GUI subsystem |
| Helper | `IssacOnlineSync.Helper.x86.exe`，i686，隐藏 console |
| Native | `IsaacSync.Native.dll`，i386 |
| Rust 主包 | `issac-online-sync` |
| Rust Helper 包 | `issac-online-sync-helper` |
| Rust 共享包 | `issac-online-sync-core` |

三个本地 Cargo package 的版本均继承 workspace `1.0.4`。根 `VERSION` 是产品版本唯一来源，版本工具同步 Cargo package 镜像；MSBuild 输出路径和运行时产品身份必须与其一致。

## 总体架构

```text
IssacOnlineSync.exe
├─ GPUI 主窗口与 Windows 通知
├─ Steam App 250900 定位、联机 Mod 静态补丁检查与确认窗口
├─ 每秒扫描 isaac-ng.exe 进程
├─ Supervisor：目标状态机、重试与多进程协调
├─ IPC reader：IS10 / ABI 5 snapshot 与 event ring
├─ JSONL writer 与 group analyzer
└─ Helper client：JSON line 请求/响应
       │
       ▼
IssacOnlineSync.Helper.x86.exe
├─ 目标架构与 DLL 校验
├─ LoadLibraryW 注入
└─ IsaacSyncShutdown 安全卸载
       │
       ▼
IsaacSync.Native.dll
└─ Local\IsaacSync.1.0.4.<pid>
```

`issac-online-sync-core` 不依赖 GPUI，负责 PE 校验、Helper 私有协议、IPC ABI、typed payload、状态模型、JSONL 和 analyzer。主包只拥有 UI、进程扫描、Supervisor 与 CLI。Helper 包不创建 GUI、不写业务日志，也不持有跨请求状态。

## 联机 Mod 静态补丁

无参数 GUI 启动在取得单实例互斥量后、创建 Supervisor 前执行一次联机 Mod preflight。带参数 CLI 不执行该流程。主程序从当前用户及 32/64 位系统 Steam 注册表取得 Steam 根目录，解析 `steamapps/libraryfolders.vdf` 与 `appmanifest_250900.acf`；最终 canonical `isaac-ng.exe` 必须唯一、位于 manifest 对应 library 的 `steamapps/common` 下，并通过 i386 PE EXE 校验。

补丁状态由两组原始/已补丁完整机器码判定：Mod 门禁中的三条条件跳转替换为六个 NOP，analytics 函数首字节由 `PUSH EBP` 替换为 `RET`。每个 site 必须处于“原始恰好一次”或“已补丁恰好一次”；两处均已补丁时静默继续，两处均原始时为 disabled，一原始一已补丁时为 partial，其余均为 unsupported。disabled/partial 每次启动都询问，拒绝不保存偏好；unsupported 每次启动只报告，不写文件。

确认窗口必须包含“请先关闭游戏，再点击‘是’。”，默认按钮为“否”。用户确认后重新枚举 `isaac-ng.exe` 并重新读取、分类目标文件；游戏仍运行、目标变化、权限不足或特征不唯一时立即停止。提交顺序固定为：保留完整备份、在同目录写入并 `sync_all` 临时文件、`ReplaceFileW(REPLACEFILE_WRITE_THROUGH)` 原子替换、重新读取并验证最终文件与预期补丁字节完全相同。首个备份使用 `isaac-ng.exe.bak`；同名备份内容不同则保留并创建时间戳备份。程序不自动提升权限，也不提供恢复按钮。

## 进程发现与目标状态

主程序每秒枚举 `isaac-ng.exe`，以 `(pid, creationTime)` 作为目标键，避免 PID 复用。每个目标独立经历：

```text
Discovered -> Inspecting -> Injecting -> Monitoring
                         \-> RetryableFailure
                         \-> PermanentFailure
Monitoring -> Exited | Unloading -> Suppressed
```

调和顺序固定为：

1. 验证目标仍存活且创建时间未改变。
2. 检查 PE machine、目标路径和已加载模块。
3. 若已加载 `IsaacSync.Native.dll`，直接尝试连接 1.0.4 IPC。
4. 若未加载，通过同目录 x86 Helper 注入固定 DLL。
5. 等待 `Local\IsaacSync.1.0.4.<pid>` 并验证 magic、ABI、size 和 PID。
6. 进入 monitoring，周期读取 snapshot/event 并写 JSONL。

目标退出、PID 复用、DLL/IPC 不匹配和永久兼容错误必须清除当前监控状态。普通扫描失败使用有界退避；用户请求安全卸载后，该进程在本次生命周期内进入 suppressed，不能被扫描器立即重新注入。

## Helper 私有协议

主程序通过隐藏 stdin/stdout 与 Helper 交换单行 JSON。私有协议版本为 `1`，与产品版本和 wire protocol 无关。

请求：

```json
{
  "protocol": 1,
  "operation": "inject | unload",
  "pid": 1234,
  "dllPath": "C:\\path\\IsaacSync.Native.dll"
}
```

响应：

```json
{
  "protocol": 1,
  "ok": true,
  "code": "ok",
  "message": "..."
}
```

Helper 必须验证：

- 自身为 i686，目标为有效的 32-bit `isaac-ng.exe`；
- DLL 文件名精确为 `IsaacSync.Native.dll`，PE machine 为 i386；
- 注入使用目标进程中的 `LoadLibraryW`，而不是假设本进程函数地址；
- unload 只调用 DLL 导出的 `IsaacSyncShutdown`，不使用远程 `FreeLibrary` 强拆；
- stdout 只输出一行响应 JSON，诊断写 stderr。

同一 PID 的 inject/unload 串行化；多个不同 PID 可以并行。Helper 进程总是由主程序隐藏启动并设置超时。

## IPC ABI 5

1.0.4 IPC 常量：

| 字段 | 值 |
| --- | --- |
| magic | `IS10` / `0x30315349` |
| ABI | `5` |
| mapping | `Local\\IsaacSync.1.0.4.<pid>` |
| mapping size | `1,967,200` |
| event offset | `1,120` |
| event size | `240` |
| event capacity | `8,192` |

reader 每次 snapshot 前后核对 publish sequence，拒绝撕裂读取。event ring 使用单调 sequence；缺口、覆盖、未知 enum、未知 Native error、非零 `reserved_typed_payload` 或非规范 typed payload 都返回明确错误，不能静默跳过。

Native mapping 的所有权属于目标进程内 DLL；注入器只打开和读取。unload 或目标退出后 mapping 必须消失。

## JSONL 与 analyzer

每个目标进程写独立文件：

```text
<app-dir>/logs/isaac-sync-1.0.4-<pid>-<timestamp>.jsonl
```

每条记录包含 endpoint/session、event sequence、runtime state、error code、frame、sender/recipient slot、port/opcode，以及：

```json
{
  "payloadKind": "int32 | float64 | none",
  "payloadValue": 1.5,
  "payloadBits": "3ff8000000000000"
}
```

### 日志导出

主窗口顶部提供“导出日志”按钮，并通过系统保存对话框生成带时间戳的 ZIP。压缩包固定
包含 `<app-dir>/logs` 下全部 `.log`、`.jsonl` 与用户 Documents 目录下
`My Games/Binding of Isaac Repentance+/log.txt`：

```text
injector/<injector-log-name>
game/log.txt
```

游戏日志缺失或注入器没有可用日志时导出失败，不生成不完整的结果。压缩包先写入目标
目录的临时文件，全部输入复制并同步成功后再原子替换用户确认的目标文件。导出过程在
后台执行，按钮暂时禁用，扫描和 IPC polling 不暂停。

`--analyze-group` 接受同一 session 的 2–4 份日志，验证：

- protocol/ABI、endpoint 和 session 唯一性；
- roster、玩家映射、manifest、activation 与 local slot 双射；
- 每条有向链路的 packet sequence、fan-out 和 ACK；
- accepted、sealed、received 与按 slot 分发的 operation identity；
- typed kind/bits、event gap、fatal/ABORT 和 shutdown 完整性。

`--analyze-pair` 保留为双端快捷入口，内部执行同一组分析规则。

`--analyze-group` 可追加 `--session <u64> --through-frame <u32>`。范围模式仍解析整份 JSONL 并检查文件内 event sequence；roster、packet、failure 和 operation 只解释指定 session，且 operation exactly-once key 只纳入 `operationFrame <= through-frame`。范围内 failure 保持硬失败，范围外未 sealed operation 或 sessionToken=0 lifecycle failure 不得污染目标窗口。输出必须回显范围。

T0 调查构建新增 `SyncOriginalDispatchCompleted = 53`：当前 frame 至少尝试一个非 NoOp operation 时，在原版 dispatch 返回后发布，`onlineFrame/operationFrame` 均为 dispatch frame，`recordCount` 为成功 handler 数。该事件无 operation identity，不改变 IPC ABI 5 或 Lua API。

`--analyze-goodtrip-trace <syncA> <gameA> <syncB> <gameB> --session <u64> --through-frame <u32> [--format text|json]` 先执行 scoped group 校验，再配对双方 `GT_SYNC_TRACE_V2`。V2 同时记录 operationFrame 与 `Game():GetFrameCount()`，并公开 canonical plan 字段供分析器复算 64-bit FNV-1a；报告区分 outcome、plan、transition phase 和 handler-without-transition，退出码 0/1/2 分别表示匹配、诊断不一致和输入错误。

### 游戏 desync 离线分析

`--analyze-desync` 读取一个游戏原生 desync 目录；`desync_diff.txt` 是必需输入，framestate、游戏日志和 RNG history 缺失时降级为带 warning 的部分报告。解析器支持 2–4 个端点、非连续实体编号、可选实体字段、科学计数法和 signed zero。framestate checksum 用于反查本机端点，因此报告中的 `Player 0/1` 始终解释为端点快照编号，而不是玩家角色实体。

默认从主程序同目录的 `logs` 查找 JSONL。候选必须由 desync 文件时间区间、online frame 与非零 session token 共同命中；同一进程内多局帧号重置时使用 QPC 相对时间消歧，仍不唯一则报告 `ambiguous`，不运行专用因果规则。分析窗口为 desync 前 16 帧，关注 operation dispatch、custom ready stall 和 dispatch/runtime failure。

通用规则报告 checksum/RNG 分歧、纯 signed-zero 玩家差异、绝对值不超过 `1e-6` 的玩家速度残差，以及严格的“落后端 post 等于领先端 pre”实体一更新偏移。RNG history 中的 `epoxy_*` 只视为未解析地址标签，不能据此归因 OpenGL。游戏日志中的 `LOADED MOD` 与 `Running Lua Script` 分开建模，前者不能作为 Mod 实际执行证据。

内置 GoodTrip profile 解码派生 port `30940`：opcode 1 为 teleport，opcode 2 为 map-held。只有唯一关联 session 同时出现 `F-2 map-held → F-1 teleport → F desync`、GoodTrip Lua 实际执行，且伴随 RNG 或实体更新分歧时，才输出高置信度“可能触发”；缺少任一证据时降为中等置信度，绝不表述为形式证明。

`--analyze-desync-root` 扫描根目录并按主要 finding、最近 operation 帧偏移、RNG 分歧和实体更新偏移聚类。没有 `desync_diff.txt` 的事件目录计入 skipped，`sessions` 等非事件目录忽略。文本报告面向人工排查；`--format json` 输出稳定的 `schemaVersion: 1`，包含 source、desync、endpoints、entities、sync、timeline、findings、warnings 和批量 clusters。默认使用匿名 Endpoint 编号并隐藏绝对路径，`--include-identifiers` 才输出用户名、Steam ID 和原始路径。

## GPUI 行为

窗口标题显示 `IssacOnlineSync 1.0.4`。单实例互斥量保持 `Local\IssacOnlineSync.Gui`，防止重复扫描器。

界面展示：

- 当前发现的 Isaac 进程及其 creation time；
- 注入、IPC、兼容和 session 状态；
- peer/slot/roster、frame、packet、dispatch 和 rejection 统计；
- 最后一次 typed dispatch 的 kind、可读值与精确 bits；
- 最近错误与日志路径。
- 日志导出按钮及导出进度、成功或失败状态。

`manifest_count = 0` 是合法状态。双方无 IsaacSync Mod 时，Native 在兼容 roster 的 2 秒注册窗口后自动冻结空 manifest；GUI 继续通过现有状态、flags 和 event type 21 展示该过程，不增加启动器开关或 IPC 字段。

界面不提供 DLL 浏览、手工 port、Start、Send、payload 或 roster 编辑。DLL、Helper 和主程序必须来自同一 `out/1.0.4/<Configuration>` 目录。

注入成功时发送 Windows 通知；tag 包含 PID 与 creation time，避免 PID 复用覆盖错误通知。最小化隐藏窗口，关闭按钮不等于安全卸载；显式退出时停止扫描器并释放后台任务。

## CLI

```text
IssacOnlineSync.exe --self-test
IssacOnlineSync.exe --test-delay <pid> <1-1000ms>
IssacOnlineSync.exe --analyze-group <log1> <log2> [log3] [log4]
IssacOnlineSync.exe --analyze-pair <logA> <logB>
IssacOnlineSync.exe --analyze-group <logA> <logB> [logC] [logD] --session <u64> --through-frame <u32>
IssacOnlineSync.exe --analyze-goodtrip-trace <syncA.jsonl> <gameA.log> <syncB.jsonl> <gameB.log> --session <u64> --through-frame <u32> [--format text|json]
IssacOnlineSync.exe --analyze-desync <directory> [--sync-log <jsonl> | --sync-log-dir <directory>] [--format text|json] [--include-identifiers]
IssacOnlineSync.exe --analyze-desync-root <directory> [--sync-log-dir <directory>] [--format text|json] [--include-identifiers]
```

`--self-test` 锁定主程序架构、package version、protocol 5、IPC ABI 5、`IS10`、mapping 大小、event 布局、typed payload、sequence wrap、analyzer 和 i686 Helper 自测。

## 构建与交付

`online-sync/build.ps1` 从根 `VERSION` 读取产品版本，校验三个 Cargo package 后分别构建 x86_64 主程序和 i686 Helper。所有路径必须位于 `out/1.0.4`，清理逻辑只允许操作该版本输出根内的精确文件。

完整 solution：

```powershell
& msbuild.exe .\IsaacSync.sln /restore /m /p:Configuration=Debug /p:Platform=Win32
& msbuild.exe .\IsaacSync.sln /restore /m /p:Configuration=Release /p:Platform=Win32
```

每个配置交付：

- `IsaacSync.Native.dll`
- `IsaacSync.Native.Tests.exe`
- `IssacOnlineSync.exe`
- `IssacOnlineSync.Helper.x86.exe`

## 测试门禁

- Cargo fmt、x64/i686 clippy 和全部 Rust tests。
- 日志导出 ZIP 内容、扩展名处理、缺失输入与原子落盘测试。
- 联机 Mod 纯字节状态、partial 补齐、唯一特征、Steam VDF/manifest 定位、备份轮换和原子替换 fixture。
- `IssacOnlineSync.exe --self-test`。
- Win32 fixture：真实 LoadLibrary、typed `IS10` mapping、异构 mapping 隔离、安全卸载和多 host 清理。
- Native.Tests：wire、session、frame queue、manifest、port、typed payload 与 IPC ABI。
- 真实 Steam 2/3/4 peer：自动注入、ACTIVE、typed fan-out、延迟、成员退出、新局重建与 group analyzer。
