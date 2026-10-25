# Isaac Hook Research

本项目用于学习《以撒的结合》游戏代码逆向与 mod 制作研究，不包含恶意用途。

## 文档导航

为便于查阅，当前结论按主题拆分为以下文档：

- [online-input-delay.md](online-input-delay.md)：`onlineInputDelay`、输入帧队列与 lockstep 推进链。
- [custom-sync-design.md](custom-sync-design.md)：Isaac Sync 1.0.4 的正式协议、数据模型、IPC 与验收契约。
- [issac-online-sync-design.md](issac-online-sync-design.md)：Rust/GPUI 自动注入器、x86 Helper、后台监控与 Windows 通知设计。
- [isaac-sync-lua-api.md](isaac-sync-lua-api.md)：面向 Mod 作者的正式 Lua API 参考。
- [custom-sync-runtime-analysis.md](custom-sync-runtime-analysis.md)：当前运行实例的 RVA、Steam/EOS 后端证据和动态验收条件。

本 README 保留完整的历史分析记录和原始上下文；上述文档是按主题整理的入口，不以旧 VA 作为版本无关定位依据。

## 当前研究目标

搞清楚 `onlineInputDelay` 字段在游戏中的实际作用链路，重点回答两个问题：

1. 哪些代码位置读取或消费了 `onlineInputDelay`。
2. 真正把 `onlineInputDelay` 转化为游戏输入延迟的代码在哪里。

最终希望得到一条可验证的调用/数据流：

```text
onlineInputDelay 的写入来源
  -> g_Manager + 0x2A3F4
  -> 通过特征定位到的消费点
  -> 实际影响输入队列/输入采样/输入应用时机的位置
```

## 已知信息

| 项目 | 当前结论 |
| --- | --- |
| `g_Manager` 全局指针位置 | 不确定，需要通过特征定位 |
| `g_Manager` 定位特征 | `A1 ?? ?? ?? ?? 8B 80 ?? ?? ?? ?? 83 E8 00` |
| 当前版本 `onlineInputDelay` 偏移 | `g_Manager + 0x2A3F4` |
| 当前版本 `onlineInputDelay` 缓存 | `g_Manager + 0x4B3E8` |
| 当前版本 delay dirty flag | `g_Manager + 0x4B3F0` |
| 当前版本 online frame/tick 字段 | `g_Manager + 0x4B3E4` |
| 当前已确认的输入帧查找函数 | `007E1190` |
| 当前已确认的远端输入接收函数 | `007E2010` |
| 当前已确认的本地输入广播函数 | `007E2950` |
| 当前已确认的输入帧消费/帧推进函数 | `007F0980` |
| 关注的消费点地址 | 不确定，需要通过特征定位 |
| 消费点定位特征 | `F4 A3 02 00` |
| 当前已分析到的消费点所在函数 | `sub_007EB150` |
| 当前已分析到的消费指令 | `007EB634: mov eax, dword ptr [edx+2A3F4]` |
| 当前已确认的输入延迟生效链路 | `sub_007E1CB0` 生成未来输入帧，`sub_007F0980` 消费当前帧输入并推进 online frame |

这些地址先按“版本相关、需要重新定位”处理；偏移 `0x2A3F4` 目前仍按当前版本结论记录，后续如果切换版本也需要重新确认。

## 初步判断

通过特征 `F4 A3 02 00` 定位到的代码位置是当前关注的 `onlineInputDelay` 消费点，但它未必就是“真正设置输入延迟”的最终位置。它可能只是：

- 读取配置值；
- 参与网络/在线状态判断；
- 写入另一个输入相关字段；
- 作为输入缓冲长度、tick 偏移或帧延迟参数传递给下游逻辑。

因此下一步不能只停在该读取点，需要继续追踪它读出的值被传到哪里、写到哪里、以及是否影响输入队列或玩家输入采样。

## 已确认函数：`sub_007EB150`

当前通过消费点附近的完整反汇编确认，`onlineInputDelay` 当前关注的消费点位于 `sub_007EB150` 内。

| 项目 | 当前结论 |
| --- | --- |
| 函数入口 | `007EB150` |
| 已确认函数结束 | `007EB8FD` |
| 调用约定 | `thiscall`，入口 `ecx = this`，函数内保存到 `edi` |
| 返回值 | `bool`/`byte`，返回值位于 `al` |
| 当前判断 | 在线/联机状态更新主流程的一部分，包含 lobby/daily join 状态、延迟开始、在线子系统更新、`onlineInputDelay` 本地缓存同步 |

入口处可见 MSVC 带栈对齐和 SEH 的函数序言：

```asm
007EB150  push ebx
007EB151  mov ebx,esp
007EB153  sub esp,8
007EB156  and esp,FFFFFFF8
007EB159  add esp,4
007EB15C  push ebp
...
007EB185  mov fs:[0],eax
007EB18B  mov edi,ecx
```

已确认的返回路径：

```asm
007EB699  mov al,1
          ; epilogue -> ret

007EB8CD  xor al,al
          ; epilogue -> ret

007EB8E3  mov ecx,edi
007EB8E5  call 007F0980
          ; epilogue -> ret，返回值疑似来自 007F0980
```

### `onlineInputDelay` 消费块

当前关键代码块：

```asm
007EB624  mov ecx,edi
007EB626  call isaac-ng.7EE6D0
007EB62B  mov edx,dword ptr ds:[B5169C]
007EB631  mov ecx,dword ptr ds:[edi+10]
007EB634  mov eax,dword ptr ds:[edx+2A3F4]
007EB63A  mov dword ptr ds:[edi+10],eax
007EB63D  cmp eax,ecx
007EB63F  jbe isaac-ng.7EB645
007EB641  mov byte ptr ds:[edi+18],1
```

等价理解：

```text
oldDelay = this->field_10
newDelay = g_Manager->onlineInputDelay
this->field_10 = newDelay

if (newDelay > oldDelay)
    this->field_18 = 1
```

当前结论：

- `this + 0x10` 是 `onlineInputDelay` 的本地缓存字段。
- `this + 0x18` 可能是 delay 增大后触发缓冲重建/状态刷新的标志位。
- 该函数同步并缓存 `onlineInputDelay`，但没有直接体现“按 delay 延后输入应用”的逻辑。
- 真正让输入延迟生效的位置，仍应继续追踪读取 `this + 0x10` 或读取 `this + 0x18` 的代码。

## `sub_007EB150` 的调用者确认

通过查找 `sub_007EB150` 的调用点，确认其中一个高层调用者为 `sub_00834CD0`。

| 项目 | 当前结论 |
| --- | --- |
| 函数入口 | `00834CD0` |
| 已确认函数结束 | `00835599` |
| 调用约定 | 非普通对象 `thiscall`，函数内部直接读取 `g_Manager` |
| 返回形式 | `ret 4`，有一个栈参数 |
| 当前判断 | `g_Manager` 高层 update/tick 函数，负责在线状态、菜单/窗口状态、输入/同步子系统更新等 |

函数开头直接取 `g_Manager`：

```asm
00834CEC  mov edi,dword ptr ds:[B5169C]   ; edi = g_Manager
```

随后调用 `sub_007EB150`：

```asm
008350A5  lea esi,dword ptr ds:[edi+4B3D8]
008350AB  mov ecx,esi
008350AD  call sub_007EB150
008350B2  test al,al
```

因此可以确认：

```text
sub_007EB150::this = g_Manager + 0x4B3D8
sub_007EB150::this + 0x10 = g_Manager + 0x4B3E8
sub_007EB150::this + 0x18 = g_Manager + 0x4B3F0
```

这一步把原先泛化的 `this+0x10` / `this+0x18` 具体化为全局 `g_Manager` 内的固定偏移：

```text
g_Manager + 0x2A3F4 = onlineInputDelay 源字段
g_Manager + 0x4B3E8 = onlineInputDelay 缓存字段
g_Manager + 0x4B3F0 = delay 增大后设置的 dirty/补齐标志
```

`sub_007EB150` 返回 0 时，调用者会在 `g_Manager+8` 状态为 `2` 或 `5` 时进入清理/退出流程；返回非 0 时继续正常 update。因此 `sub_007EB150` 更像在线子系统 update/check，不是最终输入应用点。

## delay 缓存字段搜索过程

确认 `sub_007EB150::this = g_Manager + 0x4B3D8` 后，搜索重点从泛化的 `[reg+10]` 改为直接搜索：

```text
D8 B3 04 00   ; g_Manager + 0x4B3D8，在线子对象基址
E8 B3 04 00   ; g_Manager + 0x4B3E8，onlineInputDelay 缓存
F0 B3 04 00   ; g_Manager + 0x4B3F0，delay dirty flag
```

### 匹配点 1：`sub_0030EEF0`

该匹配点为误命中：

```asm
0030EF78  E8 B3 04 00 00  call isaac-ng.30F430
```

这里的 `E8` 是 `call rel32` 的操作码，`B3 04 00 00` 是相对跳转偏移，不是内存偏移 `+4B3E8`。

该函数实际功能更像图形/几何数据 builder：向两个动态数组追加 8 字节顶点数据和 16-bit index/command 数据。该匹配点已排除，不属于 `onlineInputDelay` 链路。

### 匹配点 2：`sub_005CEF60`

该匹配点是真实读取 `g_Manager + 0x4B3E8`，但不是输入应用点。

关键代码：

```asm
005CF189  mov eax,dword ptr ds:[B5169C]
005CF18E  mov ecx,dword ptr ds:[eax+4B3E8]
005CF194  mov eax,dword ptr ds:[eax+4B3E4]
005CF19A  add ecx,ecx
005CF19C  sub eax,ecx
005CF19E  sub eax,2
005CF1A1  cmp dword ptr ds:[edx+C],eax
005CF1A4  jae isaac-ng.5CF1D2
```

等价理解：

```text
threshold = g_Manager->field_4B3E4 - g_Manager->onlineInputDelayCache * 2 - 2
```

函数前半段生成随机数，后半段记录随机数调用结果、当前 frame/tick 和调用栈，再按 `threshold` 清理旧记录。因此该函数说明 delay 缓存也参与在线回滚/同步相关的历史窗口保留，但不是直接让输入延迟生效的位置。

### 匹配点 3：`sub_007E1CB0`

该匹配点是真实读取 `g_Manager + 0x4B3E8`，并且目前判断为输入延迟生效的核心位置之一。

| 项目 | 当前结论 |
| --- | --- |
| 函数入口 | `007E1CB0` |
| 已确认函数结束 | `007E2001` |
| 调用约定 | `thiscall`，`ecx = this` |
| 返回形式 | `ret 8`，返回 `bool`/`byte` |
| 当前判断 | 在线输入帧队列 update / input frame buffering 函数 |

第一处 delay 用法：

```asm
007E1D2F  mov ecx,dword ptr ds:[edx+4B3E4]
007E1D35  mov eax,dword ptr ds:[edx+4B3E8]
007E1D3B  add eax,dword ptr ds:[edx+4B3E4]
007E1D41  cmp ecx,eax
007E1D43  ja isaac-ng.7E1F1F
```

其中 `edx = g_Manager`。这里的 `ecx` 表示要处理/生成的输入帧编号：

```text
如果输入队列非空：nextInputFrame = lastQueuedInputFrame + 1
如果输入队列为空：nextInputFrame = currentFrame
```

因此等价逻辑为：

```text
currentFrame = g_Manager + 0x4B3E4
delay = g_Manager + 0x4B3E8

if (nextInputFrame <= currentFrame + delay)
    继续构造并入队输入帧
else
    进入清理/检查路径
```

这说明 `onlineInputDelay` 在这里通过 `currentFrame + delay` 控制本地输入帧队列预填到未来多少帧。delay 越大，当前物理输入被打包到越晚的 simulation frame，玩家感受到的输入延迟越大。

函数中间会构造一个输入帧结构，并写入 `this+3A0` / `this+3AC` 两个队列：

```asm
007E1DCB  mov dword ptr ss:[ebp-2C],esi
007E1DD3  mov word ptr ss:[ebp-20],7FFF
007E1DDA  mov dword ptr ss:[ebp-28],7FFF0000
007E1DE1  mov dword ptr ss:[ebp-24],7FFF7FFF
...
007E1E1D  lea ecx,dword ptr ds:[edi+3A0]
007E1E23  call isaac-ng.7E1070
007E1E2B  lea ecx,dword ptr ds:[edi+3AC]
007E1E32  call isaac-ng.7E1070
007E1E37  push esi
007E1E38  mov ecx,edi
007E1E3A  call isaac-ng.7E2950
```

`0x7FFF` 一类常量像输入摇杆/轴的中立值，进一步支持这是输入帧结构而不是普通状态对象。

第二处 delay 用法与 dirty flag 相关：

```asm
007E1F50  cmp byte ptr ds:[edx+4B3F0],0
007E1F57  je isaac-ng.7E1F61
007E1F59  mov ebx,dword ptr ds:[edx+4B3E8]
007E1F5F  jmp isaac-ng.7E1F63
007E1F61  xor ebx,ebx
...
007E1F88  mov edx,dword ptr ds:[edx+4B3E4]
007E1F8E  lea eax,dword ptr ds:[edx+ebx]
007E1F91  cmp ecx,eax
007E1F93  jbe isaac-ng.7E1FC1
```

等价理解：

```text
extra = dirtyFlag ? onlineInputDelayCache : 0

if (nextInputFrame <= currentFrame + extra)
    return false
```

结合 `sub_007EB150` 中只在 `newDelay > oldDelay` 时设置 `g_Manager+4B3F0`，该逻辑很可能用于 delay 增大后的输入队列补齐，避免 delay 突然增大后未来输入帧不足。

该函数还会按 frame 清理输入帧队列：

```asm
007E1F1F  push dword ptr ds:[edx+4B3E4]
007E1F25  lea ecx,dword ptr ds:[edi+3A0]
007E1F2B  call isaac-ng.7E1030

007E1F3B  mov eax,dword ptr ds:[eax+4B3E4]
007E1F41  sub eax,5
007E1F44  push eax
007E1F45  call isaac-ng.7E1030
```

当前结论：`sub_007E1CB0` 是目前最接近“真正设置输入延迟”的位置。它不是等待时间，而是通过 frame/tick 编号把输入绑定到未来帧。

## 已确认的输入帧/同步相关函数

当前围绕 frame input 已确认出四类核心函数：

```text
007E1190  按 frame 查找 input frame
007E1CB0  按 currentFrame + delay 生成/补齐未来 input frame
007E2010  处理网络收到的 input message，并写入输入帧队列
007E2950  从本地输入帧队列取出指定 frame，打包并广播 input message
007F0980  检查输入是否 ready，消费当前 frame 的 input frame，并推进 online frame
```

结合 `sub_007E1CB0`，当前链路可以整理为：

```text
本地输入生成:
  sub_007E1CB0
    -> 构造 frame input
    -> 007E1070 插入 this+3A0 / this+3AC
    -> 007E2950 从 this+3AC 取出该 frame 并广播

远端输入接收:
  007E2010 / NetInputDevice::ProcessInputMessage
    -> 反序列化 receivedFrame 和 input state
    -> 007E1190 查 this+3A0 是否已有该 frame
    -> 007E1070 插入 this+3A0 / this+3AC

输入帧消费/帧推进:
  007F0980
    -> 遍历 NetInputDevice，调用 007E1CB0 准备输入帧
    -> 如果任一设备未 ready，则返回 false，不推进 online frame
    -> 对每个 active device，用 007E1190 从 this+3A0 查 currentFrame
    -> 把 input frame 写入设备当前输入状态
    -> g_Manager+4B3E4 自增，进入下一 online frame
```

### `007E1190`：按 frame 查找 input frame

| 项目 | 当前结论 |
| --- | --- |
| 函数入口 | `007E1190` |
| 已确认函数结束 | `007E129E` |
| 调用约定 | `thiscall`，`ecx = input frame map/queue` |
| 参数 | `frame`，位于栈参数 |
| 返回值 | 找到则返回 input frame value 指针，找不到返回 `0` |
| 当前判断 | 类似 `std::map<uint32_t, InputState>::find(frame)` |

关键逻辑：

```asm
007E11DA  mov eax,dword ptr ss:[ebp-20]
007E11DD  cmp byte ptr ds:[eax+D],0
007E11E1  jne isaac-ng.7E1289
007E11E7  mov edi,dword ptr ds:[ebx+8]
007E11EA  cmp edi,dword ptr ds:[eax+10]
007E11ED  jb isaac-ng.7E1289
...
007E1284  add eax,14
007E1289  xor eax,eax
```

节点结构暂定：

```text
node + 0x0D  is_nil
node + 0x10  frame key
node + 0x14  input frame value
```

`007E1190` 是后续寻找“缺输入导致等待”的关键函数：如果某调用者在 `007E1190` 返回 `0` 时选择不推进 simulation，就可能是 lockstep 等待门控点。

### `007E2010`：处理远端 input message

| 项目 | 当前结论 |
| --- | --- |
| 函数入口 | `007E2010` |
| 已确认函数结束 | `007E2521` |
| 调用约定 | `thiscall`，`ecx = NetInputDevice / 输入同步对象` |
| 返回值 | `al`，表示是否成功处理消息 |
| 当前判断 | `NetInputDevice::ProcessInputMessage`，远端输入包接收/入队路径 |

函数内字符串直接说明语义：

```text
NetInputDevice::ProcessInputMessage - Dropping input packet outside of match scope. Received Frame: %d, Current Frame: %d
NetInputDevice::ProcessInputMessage - Dropping input packet for frame we already have. Frame: %d
NetInputDevice::ProcessInputMessage - CHECKSUM MISMATCH! Expected: %08x, Received: %08x
```

关键流程：

```asm
007E239F  mov eax,dword ptr ds:[B5169C]
007E23A4  mov eax,dword ptr ds:[eax+4B3E4]
007E23AA  cmp esi,eax
007E23AC  jae isaac-ng.7E23BA
```

这里：

```text
esi = receivedFrame
eax = g_Manager + 0x4B3E4，即 currentFrame
```

如果 `receivedFrame < currentFrame`，说明输入包已经晚于当前同步帧，会被丢弃。

随后查重：

```asm
007E23BA  push esi
007E23BB  lea ecx,dword ptr ds:[ebx+3A0]
007E23C1  call isaac-ng.7E1190
007E23C6  test eax,eax
007E23C8  je isaac-ng.7E23DC
```

若 `this+3A0` 已经存在该 frame，则丢弃重复包；若不存在且 checksum 通过，则插入：

```asm
007E2424  lea eax,dword ptr ss:[ebp-28]
007E2428  lea ecx,dword ptr ds:[ebx+3A0]
007E242E  call isaac-ng.7E1070

007E2433  lea eax,dword ptr ss:[ebp-28]
007E2436  lea ecx,dword ptr ds:[ebx+3AC]
007E243D  call isaac-ng.7E1070
```

当前判断：`007E2010` 是远端输入到达后写入本地输入帧队列的入口。它不是等待门控点，因为 `007E1190` 返回非空在这里代表重复包，是错误路径。

### `007E2950`：广播本地 frame input

| 项目 | 当前结论 |
| --- | --- |
| 函数入口 | `007E2950` |
| 已确认函数结束 | `007E2C89` |
| 调用约定 | `thiscall`，`ecx = NetInputDevice / 输入同步对象` |
| 参数 | 要广播的 `frame`，位于栈参数 |
| 当前判断 | `NetInputDevice::broadcast_frame_input(frame)` |

函数内字符串直接说明语义：

```text
NetInputDevice::broadcast_frame_input should only be called on the local player
Attempting to create a message when there is no multiplayer service
```

关键查找：

```asm
007E2A25  mov ecx,dword ptr ss:[ebp-18]
007E2A28  push dword ptr ds:[ebx+8]
007E2A2B  add ecx,3AC
007E2A34  call isaac-ng.7E1190
007E2A3C  test eax,eax
007E2A3E  je isaac-ng.7E2C76
```

等价理解：

```text
input = this->queue_3AC.find(frame)
if (!input)
    return
```

找到输入帧后，函数会序列化 0x10 字节 input frame：

```asm
007E2AD0  mov eax,dword ptr ds:[esi]
007E2ADE  movzx eax,word ptr ds:[esi+4]
007E2AF6  movzx eax,word ptr ds:[esi+6]
007E2B0E  movzx eax,word ptr ds:[esi+8]
007E2B26  movzx eax,word ptr ds:[esi+A]
007E2B3E  movzx eax,word ptr ds:[esi+C]
007E2B56  mov al,byte ptr ds:[esi+E]
007E2B6D  mov al,byte ptr ds:[esi+F]
```

因此 input frame value 结构暂定为：

```text
+0x00  dword
+0x04  word
+0x06  word
+0x08  word
+0x0A  word
+0x0C  word
+0x0E  byte
+0x0F  byte
```

函数还会带上同步/校验数据并计算 checksum：

```asm
007E29E3  mov eax,dword ptr ds:[edx+4B3E4]
007E29E9  dec eax
007E29EA  cmp dword ptr ds:[ecx],eax
...
007E2B84  push C
007E2B8A  lea ecx,dword ptr ss:[ebp-44]
007E2B8D  call isaac-ng.563410
007E2B92  push 10
007E2B94  push esi
007E2B95  call isaac-ng.563410
```

实际创建/广播消息在尾部：

```asm
007E2C1C  call isaac-ng.958160
...
007E2C3B  lea ecx,dword ptr ds:[esi+4B3D8]
007E2C41  call isaac-ng.7EECA0
```

当前判断：`007E2950` 是本地输入广播路径，不是等待门控点。它确认 `this+3AC` 至少保存了本地可广播输入帧。

### `007F0980`：输入帧消费与 online frame 推进

| 项目 | 当前结论 |
| --- | --- |
| 函数入口 | `007F0980` |
| 已确认函数结束 | `007F1117` |
| 调用约定 | `thiscall`，`ecx = g_Manager + 0x4B3D8` |
| 返回值 | `al`，表示当前 online frame 是否成功推进/输入是否 ready |
| 当前判断 | 输入同步管理器 update，负责调用各 `NetInputDevice` 准备输入、等待缺失输入、应用当前帧输入并推进 `g_Manager+0x4B3E4` |

该函数由 `sub_007EB150` 尾部调用：

```asm
007EB8E3  mov ecx,edi
007EB8E5  call isaac-ng.7F0980
```

因为 `sub_007EB150::this = g_Manager + 0x4B3D8`，所以这里可以确认：

```text
sub_007F0980::this = g_Manager + 0x4B3D8
sub_007F0980::this + 0x0C = g_Manager + 0x4B3E4  ; current online frame
sub_007F0980::this + 0x10 = g_Manager + 0x4B3E8  ; onlineInputDelay cache
sub_007F0980::this + 0x18 = g_Manager + 0x4B3F0  ; delay dirty flag
```

函数前半段遍历输入设备列表，并对每个设备调用 `sub_007E1CB0`：

```asm
007F09D4  mov eax,dword ptr ds:[ecx+esi*4]
...
007F0A29  mov ecx,dword ptr ss:[ebp-3C]
007F0A2C  call sub_007E1CB0
007F0A31  test al,al
007F0A33  je isaac-ng.7F0A3F
...
007F0A3F  mov byte ptr ss:[ebp-11],0
```

这里的 `sub_007E1CB0` 负责按照 `currentFrame + delay` 生成/补齐未来输入帧。如果任一设备返回 false，局部 ready 标志会被清为 0。

随后函数检查 ready 状态。如果未 ready，直接返回 false，不进入输入应用和帧推进：

```asm
007F0B47  test cl,cl
007F0B49  je isaac-ng.7F1102
...
007F1102  mov al,cl
007F1117  ret
```

当前判断：这就是 lockstep 等待门控之一。也就是说，只要当前 online frame 所需的输入还没有准备好，函数不会推进 simulation frame。

ready 后，函数会清除 delay dirty flag：

```asm
007F0B4F  mov byte ptr ds:[edi+18],0
```

由于 `edi = g_Manager + 0x4B3D8`，因此该写入等价于：

```text
g_Manager + 0x4B3F0 = 0
```

这解释了 `sub_007EB150` 在 delay 增大时设置的 dirty flag 最终在哪里被清零。

真正按当前帧读取输入并应用的位置如下：

```asm
007F0B83  lea ecx,dword ptr ds:[edi+3A0]
007F0B89  push dword ptr ds:[eax+C]
007F0B8C  call isaac-ng.7E1190
007F0B94  mov dword ptr ss:[ebp-1C],eax
```

其中：

```text
edi = 当前 NetInputDevice
eax = g_Manager + 0x4B3D8
[eax+C] = g_Manager + 0x4B3E4 = currentFrame
edi+3A0 = 该输入设备的 input frame 队列
```

因此该调用等价于：

```cpp
InputFrame* frame = device->queue_3A0.find(manager->currentFrame);
```

找到输入帧后，函数把 input frame 的各字段写入设备当前输入状态：

```asm
007F0BC1  movzx eax,word ptr ds:[eax+4]    ; buttons
...
007F0C17  mov dword ptr ds:[edi+380],esi   ; current buttons low
007F0C1D  mov dword ptr ds:[edi+388],ecx   ; pressed buttons low
007F0C2A  mov dword ptr ds:[edi+38C],eax   ; pressed buttons high
007F0C30  mov dword ptr ds:[edi+384],edx   ; current buttons high
007F0C36  movzx eax,word ptr ds:[ecx+6]    ; axis 0
007F0C51  movzx eax,word ptr ds:[ecx+8]    ; axis 1
007F0C72  movsx eax,word ptr ds:[ecx+A]    ; axis 2, signed path
007F0C87  movsx eax,word ptr ds:[ecx+C]    ; axis 3, signed path
007F0CCC  mov byte ptr ds:[edi+3D4],dl
007F0CD2  movsx eax,byte ptr ds:[ecx+F]
007F0CE4  mov dword ptr ds:[edi+3D0],eax
```

对应的当前输入状态字段暂定为：

```text
NetInputDevice + 0x380  当前按键低位 mask
NetInputDevice + 0x384  当前按键高位 mask
NetInputDevice + 0x388  本帧新按下低位 mask
NetInputDevice + 0x38C  本帧新按下高位 mask
NetInputDevice + 0x390  axis 0
NetInputDevice + 0x394  axis 1
NetInputDevice + 0x398  axis 2
NetInputDevice + 0x39C  axis 3
NetInputDevice + 0x3D0  input flag/type
NetInputDevice + 0x3D4  axis mode flag
```

函数尾部会根据 delay cache 修剪历史，并推进当前 online frame：

```asm
007F0F99  mov eax,dword ptr ds:[edi+10]
007F0F9F  add eax,eax
007F0FA1  cmp esi,eax
...
007F10E8  inc dword ptr ds:[edi+C]
```

其中：

```text
edi+0x10 = g_Manager + 0x4B3E8 = delay cache
edi+0x0C = g_Manager + 0x4B3E4 = currentFrame
```

因此可以确认：

```text
sub_007E1CB0：把当前物理输入打包到 currentFrame + delay 的未来帧
sub_007F0980：在 currentFrame 到达时取出对应 input frame，写入实际输入状态，然后 currentFrame++
```

当前结论：`sub_007F0980` 是目前已确认的输入帧消费点和 online frame 推进点。它和 `sub_007E1CB0` 一起构成 `onlineInputDelay` 真正影响输入延迟的核心链路。

### 相关状态重置

在 `g_Manager + 0x4ABBC` 的 bit 0 为 1 时，函数会遍历 `this` 内部的对象指针数组，并清零每个元素的两个字段：

```asm
007EB65C  test byte ptr ds:[edx+4ABBC],1
007EB663  je isaac-ng.7EB6B0
...
007EB679  mov dword ptr ds:[eax+388],0
007EB683  mov dword ptr ds:[eax+38C],0
```

这可能是输入/同步状态重置，但目前没有证据表明这里直接按 `onlineInputDelay` 应用输入延迟。

### 动态观察

软件断点 `007EB63A` 触发时的 dump：

```text
ExceptionCode = 80000003 (Break instruction exception)
EIP = 007EB63A
007EB63A = CC int 3

eax = 00000003
ecx = 00000003
edx = 0E2AF714
edi = 0E2FAAEC
```

含义：

```text
g_Manager = 0E2AF714
this = 0E2FAAEC
this + 0x10 = 0E2FAAFC
this + 0x18 = 0E2FAB04
onlineInputDelay = 3
old cached delay = 3
```

硬件执行断点 `007EB634` 触发时的 dump：

```text
ExceptionCode = 80000004 (Single step exception)
EIP = 007EB634
007EB634 = 8B82 F4A30200

edx = 0E3A4804
edi = 0E3EFBDC
ecx = 00000003
```

含义：

```text
g_Manager = 0E3A4804
g_Manager + 0x2A3F4 = 0E3CEBF8
this = 0E3EFBDC
this + 0x10 = 0E3EFBEC
this + 0x18 = 0E3EFBF4
old cached delay = 3
```

### 断点注意事项

当前观察到：

- 软件断点会把目标指令改成 `CC`，触发 `80000003`。
- 硬件执行断点会触发 `80000004` 单步异常。
- 两种断点在当前环境下都会导致游戏退出或生成 dump，疑似游戏/崩溃处理器没有让调试器正常接管异常。

后续运行时追踪优先考虑：

- 静态查找所有读取 `this + 0x10` / `this + 0x18` 的代码。
- 如果需要运行时记录，使用非断点式 inline hook / detour 写日志后继续执行。
- 外部轮询读取 `g_Manager + 0x2A3F4` 和已知 `this + 0x10`，避免设置执行/访问断点。

## 下一步分析路线

1. 通过特征定位关键地址。
   - 用 `A1 ?? ?? ?? ?? 8B 80 ?? ?? ?? ?? 83 E8 00` 定位 `g_Manager` 全局指针相关代码。
   - 用 `F4 A3 02 00` 定位当前关注的 `onlineInputDelay` 消费点。
   - 记录实际匹配到的地址、所在模块基址和游戏版本。

2. 确认消费点附近反汇编上下文。
   - 记录读取 `g_Manager + 0x2A3F4` 的指令。
   - 标注读取结果进入哪个寄存器/局部变量。
   - 追踪该值在当前函数内的后续用途。

3. 查找所有 `onlineInputDelay` 读写点。
   - 优先静态查找 `g_Manager + 0x2A3F4`、`g_Manager + 0x4B3E8`、`g_Manager + 0x4B3F0` 的引用。
   - 当前环境下断点会导致异常 dump；如需运行时记录，优先考虑非断点式 hook 或外部轮询。
   - 分别记录 read/write 触发位置。
   - 区分“配置写入”、“同步更新”、“实际消费”。

4. 继续细化 `sub_007E1CB0` 输入帧链路。
   - 重点分析 `007E1070`：向 `this+3A0` / `this+3AC` 插入输入帧。
   - 重点分析 `007E1030`：按 frame/tick 清理输入帧队列。
   - 重点分析 `007E0CA0`：把当前输入状态写入 input frame 结构。
   - 继续寻找剩余 `007E1190` 调用者，重点确认哪个调用者在找不到输入帧时阻止 simulation 推进。

5. 验证输入延迟实际效果。
   - 判断标准不是“读取了 `onlineInputDelay`”，而是它是否改变了输入被应用到游戏逻辑的帧/tick。
   - 当前假设：`onlineInputDelay` 通过 `currentFrame + delay` 控制本地输入帧队列预填到未来多少帧。
   - 需要继续验证：delay 为 0、1、N 时，输入帧编号和实际应用帧是否按该关系变化。

## 记录模板

后续每发现一个相关位置，按下面格式补充：

| 地址 | 类型 | 作用 | 定位方式 | 证据/备注 |
| --- | --- | --- | --- | --- |
| `007EB634` | read/cache | 读取 `g_Manager + 0x2A3F4` 并在后续写入 `this + 0x10` | 特征 `F4 A3 02 00` | 位于 `sub_007EB150` |
| `008350AD` | caller | 调用 `sub_007EB150`，确认 `this = g_Manager + 0x4B3D8` | 查找 `sub_007EB150` 调用者 | 位于 `sub_00834CD0` |
| `005CF18E` | read/window | 读取 `g_Manager + 0x4B3E8`，用于随机调用记录保留窗口 | 搜索 `E8 B3 04 00` 并排除误命中 | 位于 `sub_005CEF60`，非输入应用点 |
| `007E1D35` | read/input-buffer | 读取 `g_Manager + 0x4B3E8`，参与 `currentFrame + delay` 判断 | 搜索 `E8 B3 04 00` 并确认真实内存偏移 | 位于 `sub_007E1CB0`，输入帧队列预填上限 |
| `007E1F50` | read/dirty | 读取 `g_Manager + 0x4B3F0`，决定 delay 增大后是否补齐队列 | 搜索 `F0 B3 04 00` | 位于 `sub_007E1CB0` |
| `007E1F59` | read/input-buffer | dirty flag 为 1 时读取 `g_Manager + 0x4B3E8` | 搜索 `E8 B3 04 00` | 位于 `sub_007E1CB0`，用于 `currentFrame + extraDelay` |
| `007E1190` | find/input-frame | 按 frame 在输入帧 map/queue 中查找 input frame | 查找 `007E1190` 函数和调用者 | 找到返回 value 指针，找不到返回 `0` |
| `007E2010` | recv/input-frame | 处理网络收到的 input message，校验并插入输入帧队列 | `007E1190` 调用者 | `NetInputDevice::ProcessInputMessage`，远端输入接收路径 |
| `007E23C1` | find/duplicate | 在 `this+3A0` 中查找 receivedFrame，存在则丢弃重复包 | `007E2010` 内部调用 | 不是等待点，是接收查重 |
| `007E2950` | send/input-frame | 从 `this+3AC` 查找指定 frame 的本地输入，序列化并广播 | `007E1190` 调用者 | `NetInputDevice::broadcast_frame_input` |
| `007E2A34` | find/send | 在 `this+3AC` 中查找要广播的本地 input frame，找不到则不发送 | `007E2950` 内部调用 | 不是等待点，是发送前取本地输入 |
| `007F0980` | update/apply-frame | 检查输入是否 ready，消费当前 frame 的输入帧并推进 online frame | `sub_007EB150` 尾部调用 | 输入帧消费/lockstep 等待门控 |
| `007F0A2C` | call/input-buffer | 遍历 `NetInputDevice` 并调用 `sub_007E1CB0` 准备输入帧 | `007F0980` 内部调用 | 任一设备返回 false 会导致本帧不推进 |
| `007F0B4F` | write/dirty-clear | 清零 `g_Manager + 0x4B3F0` delay dirty flag | `007F0980` 内部写入 | 解释 dirty flag 的清零位置 |
| `007F0B8C` | find/apply | 从 `NetInputDevice+3A0` 查找 `g_Manager+4B3E4` 对应 input frame | `007F0980` 内部调用 `007E1190` | 找到后写入设备当前输入状态 |
| `007F10E8` | write/frame-advance | `inc [g_Manager+4B3E4]`，推进 online frame | `007F0980` 尾部 | 只有 ready 后才执行 |

## 待确认问题

- 特征 `A1 ?? ?? ?? ?? 8B 80 ?? ?? ?? ?? 83 E8 00` 在当前版本中是否唯一？
- 特征 `F4 A3 02 00` 在当前版本中是否唯一？
- `onlineInputDelay` 是否还有其他读取点？
- 哪个位置写入 `g_Manager + 0x2A3F4`？
- `g_Manager + 0x4B3F0` 是否只在 `007F0B4F` 被清零？
- `sub_007E1CB0` 的调用者是谁，调用频率是否为每帧？
- `007E0CA0` 写入的 input frame 结构字段分别代表什么？
- `007E1070` 插入队列时是否覆盖/排序，`this+3A0` 与 `this+3AC` 的准确职责差异是什么？
- `sub_007F0980` 中 `007E1190` 返回 0 时是否存在隐含崩溃/断言路径，还是由前置 ready 检查保证一定能找到？
- delay 为 0、1、N 时，输入帧编号和实际应用帧是否满足 `currentFrame + delay` 关系？

## 自定义同步操作注入规划（草案）

### 目标

通过注入 DLL 扩展原有 P2P lockstep 输入链，实现自定义操作的收发，并向 Lua Mod 暴露按 simulation frame 同步的接口。

该功能服务于加载同一版本 DLL 和 Mod 的联机参与者。它不以兼容未安装 DLL 的原版客户端为目标。

### 当前确定的实现前提

| 项目 | 当前结论 |
| --- | --- |
| 游戏版本 | `1.9.7.17` |
| 目标进程 | `isaac-ng.exe`，当前分析对象为 x86 代码 |
| 注入/Hook | C++ + MinHook |
| 模块基址 | 注入后用 `GetModuleHandleW(nullptr)` 取得实际加载基址；地址定位优先使用特征扫描和 RVA，不依赖静态 VA |
| 联机兼容性 | 所有参与者加载相同版本 DLL 和相同 Mod |
| 自定义操作上限 | 暂定每玩家每帧最多一条操作 |
| 收发可靠性 | 使用与原版输入消息一致的游戏内网络链路；不使用独立 socket 作为同步依据 |

若已知某函数的 RVA，可用 `runtimeFunctionAddress - functionRva` 交叉验证模块基址。静态反汇编中的地址只有在确认 PE preferred image base 后才能换算为 RVA；运行时仍应以 `GetModuleHandleW(nullptr)` 为准。

### Lua 注册入口

`LuaEngine::RegisterClasses()` 可通过以下特征定位：

```text
55 8B EC 6A FF 68 ?? ?? ?? ?? 64 A1 ?? ?? ?? ?? 50 81 EC 44 01 00 00 53
```

后续需要取得该函数的完整反汇编，以确认：

- `lua_State*` 的来源；
- 具体的 Lua 注册 API；
- 原有 Lua C 函数的注册表格式；
- Hook 在原函数前或后注册扩展函数的合适位置。

MinHook 对 x86 `__thiscall` 目标的 detour 需要显式接收 `ECX` 中的 `this`，通常以 `__fastcall` Hook 形式适配。实际函数签名必须在完整反汇编确认后定义。

### 同步操作模型

原始 `NetInputDevice + 0x3A0` 队列按 `(player, frame)` 保存一个固定 16 字节 `NetInputFrame`。该队列不是泛用事件队列：相同 frame 会被视为重复输入，且发送、接收、校验和和 map value 布局均假设其固定大小。

因此，自定义操作采用与原输入帧并列的 sidecar 队列：

```text
baseFrames[playerSlot][frame]   = NetInputFrame
customFrames[playerSlot][frame] = CustomFrameRecord
```

两个队列不依赖网络到达顺序，而是都以稳定的 `playerSlot` 和 `frame` 为键。`currentFrame` 只在全部玩家的原始输入和自定义记录均完整时才允许推进。

```cpp
bool ready_for_frame(uint32_t frame) {
    for (PlayerSlot player : stablePlayerSlots) {
        if (!baseFrames[player].contains(frame)) return false;
        if (!customFrames[player].contains(frame)) return false;
    }
    return true;
}
```

每个 frame 都必须存在一条自定义记录，即使没有操作也要写入 empty record。否则接收端无法区分“该帧没有自定义操作”和“自定义消息尚未到达或丢失”。

暂定单条操作结构为：

```cpp
struct CustomFrameOp {
    uint16_t port;    // 目标 Mod 的同步命名空间，0 保留为 empty/no-op
    uint16_t opcode;  // Mod 内操作编号
    int32_t payload;  // 固定整数参数
};
```

接收端必须按 `(playerSlot, frame)` 去重；若同一 key 收到内容不同的重传记录，应视为协议错误或 desync 信号。若未来改为一帧多条操作，操作列表必须附带稳定的 `actionSequence`，执行顺序固定为 `playerSlot` 升序、再 `actionSequence` 升序。

### `Sync` Lua API 草案

为保证 Lua 操作恰好在目标 simulation frame 执行，不能只依赖 Mod 在普通回调中轮询接收队列。轮询只能保证操作最终可读，无法保证每个客户端在 frame `F` 的相同阶段读取它。

当前方案改为：Lua 注册本地 handler 并提交本地操作；C++ 在确定性的帧分发点直接调用 Lua handler。仍只导出一个 `Isaac.Sync` Lua C 函数，但按参数类型分为两种模式：

```lua
-- Mod 初始化阶段：仅在本地注册，不发送 closure 到网络。
Isaac.Sync(MY_PORT, function(senderSlot, opcode, payload, frame)
    if opcode == OP_CALL_F then
        f(payload)
    end
end)
```

```lua
-- 游戏逻辑阶段：提交 future frame 操作。
targetFrame = Isaac.Sync(MY_PORT, OP_CALL_F, itemId)
```

网络中只传递确定性数据，不传函数、Lua closure、实体指针或对象内存地址：

```text
(playerSlot, frame, port, opcode, payload)
```

各客户端由相同版本 Mod 为相同 `port` 注册相同 handler。C++ 保存 Lua registry reference，逻辑关系为：

```text
(modId, port) -> local Lua handler reference
```

操作从提交到执行的时序：

```text
frame C：Lua 调用 Sync(port, opcode, payload)
  -> C++ 立即校验并登记 pending 操作
  -> 选择 future frame F，返回 F；不调用 handler

frame C 到 F：
  -> 本地帧生成路径将操作绑定到 F，并通过原生网络链发送
  -> 每个客户端按 (playerSlot, F) 保存 CustomFrameRecord

frame F：
  -> ready gate 确认所有原始输入和自定义记录完整
  -> 所有输入设备的 frame F 输入先被应用
  -> C++ 按稳定顺序分发 frame F 的 CustomFrameOp
  -> C++ 按 port 调用本地 Lua handler，handler 内执行 f(payload)
  -> 继续 frame F 的游戏逻辑并推进 online frame
```

发起者也必须等待 frame `F` 才执行 `f`；本地提交时立即调用 `f` 会比远端提前修改状态并导致 desync。

`Sync` 对 Lua 调用是异步、非阻塞的：它只返回目标帧号，不等待网络，也不会把远端 handler 的返回值作为本次调用结果。请求-响应必须经过两个未来帧：处理请求的 handler 再调用一次 `Sync` 发送响应。

#### C++ 分发和线程约束

`Sync` 的核心逻辑不另开工作线程。Lua VM、`g_Manager`、`NetInputDevice` 和游戏 STL 容器都仅在游戏主线程访问。

若后续确认原生消息分发器运行在后台线程，接收 Hook 只能把自定义记录复制到线程安全的临时缓冲；游戏主线程在 ready gate 前按 frame drain 到 `customFrames`。后台线程不能直接修改游戏帧队列，也不能调用 Lua。

Lua handler 的调用必须使用游戏已确认的 `lua_State*`、错误处理与 Mod 上下文。概念上的 registry 使用方式如下：

```cpp
// 注册时：保存本地 Lua handler。
lua_pushvalue(L, handlerIndex);
handlerRef = luaL_ref(L, LUA_REGISTRYINDEX);

// frame F 分发时：取回并传入确定性参数。
lua_rawgeti(L, LUA_REGISTRYINDEX, handlerRef);
lua_pushinteger(L, senderSlot);
lua_pushinteger(L, opcode);
lua_pushinteger(L, payload);
lua_pushinteger(L, frame);
lua_pcall(L, 4, 0, 0);
```

上面的代码仅描述模型。当前已定位到 `LuaEngine::RegisterClasses()` 的当前会话入口，并确认它经原版包装取得 `lua_State*` 后注册 Lua C 函数；仍应复用原版 LuaEngine 的调用包装。在异常处理和 Mod callback 上下文完成动态验证前，不直接假定裸 `lua_pcall` 可安全使用。

Lua 分发必须在“每个成功推进的 simulation frame 恰好一次”的固定逻辑阶段进行，而不是在 `007F0980` 的单个输入设备循环中进行。当前倾向位置是：所有设备当前帧输入已写入后、frame `F` 的游戏逻辑更新开始前；精确 Hook 点尚待主更新链分析确认。

handler 约束：

- 不允许 yield。
- 仅依赖同步游戏状态和同步参数；不得依赖本地时间、渲染/UI 状态、未同步 RNG 或本地指针。
- Lua 异常不能仅在单个客户端吞掉后继续运行；至少应记录并终止该局的同步扩展路径，避免静默 desync。
- 对局结束、Mod 卸载或 handler 替换时必须 `luaL_unref` 释放 registry reference。
- handler 内再次调用 `Sync` 只能安排新的 future frame，不能修改或重入当前 frame 的已消费操作。

当前“每玩家每帧最多一条”若为全局限制，不同 Mod 在同一 future frame 争用时，后续调用必须返回失败，例如 `nil, "frame already occupied"`。若未来允许多 Mod 在同帧发送，`CustomFrameRecord` 需改为固定上限的有序操作数组。

`port`、`opcode`、`payload` 初版只接受整数，以避免任意 Lua table、string 或 function 的序列化、版本兼容和确定性问题。

### 端口与 Mod 隔离

`port` 是自定义同步协议的 Mod 命名空间，不是实际网络端口。其作用是让不同 Mod 的操作在同一网络通道和同一 frame 时间轴中保持可区分。

端口冲突必须在对局开始前或首次使用前确定性处理。候选方案：

1. 增加显式注册 API，例如 `Isaac.SyncRegisterPort(port)`，并在开始联机时交换 Mod/端口清单。
2. 原生 DLL 按稳定 Mod ID 自动派生端口，并在联机开始时比较 Mod 清单 hash。
3. 首次 `Isaac.Sync(port, ...)` 时隐式注册；实现最少，但端口冲突可能延迟到对局中才暴露。

当前倾向方案 1 或 2。若坚持 Lua 层只暴露 `Sync`，可把端口登记作为原生层隐式行为，但仍需要联机握手阶段的 Mod 清单校验。

### 计划中的 Hook 链路

下表是目标链路，不代表网络消息细节已经确认：

| 阶段 | 目标位置 | 计划职责 | 状态 |
| --- | --- | --- | --- |
| Lua 注册 | `LuaEngine::RegisterClasses()` | 注册 `Isaac.Sync` | 已定位当前入口和既有 C 函数注册形态；尚未实现 |
| 本地帧生成 | `sub_007E1CB0` | 为与原始输入相同的 future frame 生成/附加 `CustomFrameRecord` | 待确定安全 Hook 点 |
| 原始输入发送 | `007E2950` | 使用原生网络服务发送自定义记录或扩展消息 | 已确认原版输入消息的固定布局、校验和覆盖范围和工厂调用；尚未证明可注册自定义类型 |
| 原始输入接收 | `007E2010` | 校验、去重并按 `(playerSlot, frame)` 写入自定义队列 | 已确认硬编码输入消息分支、反序列化和队列写入；尚未实现 |
| ready gate | `007F0980` | 除原始输入外检查当前帧自定义记录是否完整 | 已确认目标函数，待精确插入点 |
| 当前帧消费 | `007F0980` / 主更新链 | 按稳定顺序消费 `CustomFrameRecord`，在固定逻辑阶段由 C++ 直接分发对应 Lua handler | 已确认输入消费函数，待确定安全 Lua 分发点 |

### 当前会话的静态逆向记录

本节只记录 2026-08-01 当前运行会话中、由 x32dbg MCP 的只读模块查询、特征扫描、反汇编和内存读取支持的结论。分析期间调试器始终为 `running`，开始时断点数量为 `0`；未调用暂停、单步、断点、脚本、内存写入或 Hook 接口。

当前目标为 `isaac-ng.exe`，模块基址为 `0x006A0000`。旧记录中的 VA 与本会话相差 `+0x003C0000`，但后续定位一律使用下表中的 RVA 和指令锚点；x32dbg 的函数边界数据库会把部分带 SEH/栈对齐的函数错误切分，不能据其函数名或结束地址作判断。

| 语义锚点 | 当前 VA | RVA | 关键证据 |
| --- | --- | --- | --- |
| `g_Manager` 指针槽 | `0x00F1169C` | `0x87169C` | `0x00BAB62B: mov edx,[0x00F1169C]` |
| delay 源字段消费 | `0x00BAB634` | `0x50B634` | `8B 82 F4 A3 02 00`，即 `mov eax,[edx+0x2A3F4]` |
| 输入帧查找 | `0x00BA1190` | `0x501190` | 由发送、接收和消费路径直接调用 |
| 本地输入广播 | `0x00BA2950` | `0x502950` | 字符串 `NetInputDevice::broadcast_frame_input...` 和输入帧序列化 |
| 远端输入处理 | `0x00BA2010` | `0x502010` | 字符串 `NetInputDevice::ProcessInputMessage...` 和队列插入 |
| 输入帧插入 | `0x00BA1070` | `0x501070` | 接收端连续写入 `this+0x3A0` 与 `this+0x3AC` |
| 输入帧消费/推进 | `0x00BB0980` | `0x510980` | 按当前 frame 查找、写设备状态，最后 `inc [edi+0x0C]` |
| 校验和更新 | `0x00923410` | `0x283410` | 两个显式长度参数，`ret 8`；CRC32 分支使用多项式 `0xEDB88320` |
| 消息工厂 | `0x00D18160` | `0x678160` | `ECX=this`、五个栈参数、`ret 0x14` |
| 同步管理入口 | `0x00BAECA0` | `0x50ECA0` | `ECX=g_Manager+0x4B3D8`、`ret 0x0C` |
| 输入设备按身份查找 | `0x00BAABF0` | `0x50ABF0` | 以 8 字节键遍历输入设备并返回匹配设备 |
| 网络服务取消息 | `0x00D18770` | `0x678770` | 由在线更新路径从 `0x00F19A58` 服务对象调用 |
| Lua 类注册 | `0x00B06960` | `0x466960` | 原始序言、`ECX` 保存为引擎对象，随后直接调用 `lua_pushcclosure` |

#### 原版输入消息：发送、接收和校验

`0x00BA2950` 仅允许本地输入设备发送：`this+0x370` 的虚调用返回假时，会引用字符串 `NetInputDevice::broadcast_frame_input should only be called on the local player`。它先在 `this+0x3AC` 按 frame 查找本地 `NetInputFrame`，再构造输入消息。

静态对照发送端 `0x00BA2A83..0x00BA2B81` 与接收端 `0x00BA211A..0x00BA238B`，得到原版输入 payload 的固定顺序和长度：

| 相对偏移 | 长度 | 状态 | 证据 |
| --- | ---: | --- | --- |
| `+0x00` | 4 | 输入消息校验值占位/期望值 | 发送端先置零后在 `0x00BA2BE3` 写入最终值；接收端先读取并在 `0x00BA2420` 比较 |
| `+0x04` | 12 | 三个 dword 的原版元数据/路由字段 | 发送端连续序列化；接收端先反序列化这 12 字节并参与校验 |
| `+0x10` | 16 | `NetInputFrame` | `dword + 5*word + 2*byte`，接收后写入两个输入帧 map |
| 总计 | `0x20` | 固定原版输入消息 payload | 两端字段序列严格镜像 |

校验覆盖元数据的 `0x0C` 字节和输入帧的 `0x10` 字节：发送端在 `0x00BA2B8D`、`0x00BA2B95` 调用 `0x00923410`，接收端在 `0x00BA21E0`、`0x00BA239A` 以相同长度更新状态。`0x00923410` 的调用约定为 `__thiscall ChecksumState::Update(const void* data, uint32_t size)`：`ECX` 为状态对象，两个栈参数为数据指针与长度，返回 `ret 8`；当 `state+0x0C == 1` 时使用 IEEE CRC32 多项式 `0xEDB88320`。状态的完整初始化和三项元数据字段的业务语义仍不能只由静态代码命名。

接收端 `0x00BA2010` 由 `0x00BAB7BE` 硬编码调用。它要求消息描述符的种类字段为 `1`；不匹配时走 `Specified message is not an input message` 路径。校验通过后，先拒绝小于 `g_Manager+0x4B3E4` 的过期 frame，再拒绝 `this+0x3A0` 已有的 frame，最后在 `0x00BA242E` 与 `0x00BA243D` 将同一 16 字节输入帧插入 `this+0x3A0` 和 `this+0x3AC`。

#### 消息工厂与分发边界

`0x00D18160` 的 ABI 已确认：`ECX` 为工厂对象，接受五个 4 字节栈参数并以 `ret 0x14` 清理；`0x00BA2C1C` 的实参形态为 `(&out, 1, arg3, arg4, 1)`。函数分配并组织消息对象，但 `arg3`/`arg4` 的具体业务语义尚未由静态证据确定，不能命名为 player slot 或消息类型。

`0x00BAECA0` 的 ABI 为 `ECX` 同步管理器加 12 字节栈参数、`ret 0x0C`。发送路径先由 `0x00D18160` 构造对象、经 `0x00B8F1D0` 复制描述符，再在 `0x00BA2C41` 调用该入口；返回后才触及全局回调 `0x00F1F070`。这证明它是原版消息处理链中的边界，但尚不足以把它单独命名为“最终网络发送函数”。

当前静态路径中没有发现可供调用方注册新消息类型的接口。消息服务从 `0x00F19A58` 取出对象后，在 `0x00BAB6C8` / `0x00BAB72E` 调用 `0x00D18770`，将 8 字节身份键交给 `0x00BAABF0` 找到 `NetInputDevice`，随后直接调用 `0x00BA2010`。因此“原生层支持注册自定义消息类型”状态为 **未确认且不能假设成立**；任何未来设计只能在已确认的更低层分发边界增加受控处理，不能把 type `1` 或 `NetInputFrame` 的 16 字节布局当作可扩展容器。

#### 玩家身份、队列生命周期与确定性时序

`0x00BAABF0` 证明输入设备通过一个 8 字节身份键匹配：它遍历同步管理器的设备指针数组，读取 `NetInputDevice+0x374` 所指对象的 `+0x08/+0x0C`，与消息提供的两个 dword 比较。故当前已确认的是 peer/endpoint 级身份键，而非稳定的数值 `playerSlot`；设备数组下标是否能跨重连保持稳定尚未确认。

`0x00BB0980` 的成功路径具备以下顺序：

1. `0x00BB0B47` 仅在全部设备 ready 时继续；否则在 `0x00BB1102` 返回而不推进 frame。
2. 从 `0x00BB0B70` 开始遍历设备，在 `0x00BB0B8C` 从 `device+0x3A0` 查找 `currentFrame`，随后把 frame 数据写入 `+0x380..+0x3D4` 的当前输入状态。
3. 所有设备处理完成后在 `0x00BB0F99` 退出设备循环、修剪历史队列，并在 `0x00BB10AB` 调用后续管理逻辑。
4. `0x00BB10E8` 才执行 `inc [syncManager+0x0C]`，即 `online frame++`。

因此，自定义 sidecar 记录的静态候选消费窗口是“设备循环结束后的 `0x00BB0F99` 之后、`0x00BB10E8` 之前”。这段是旧会话的静态结论；后续无断点样本已把位置收窄到 `RVA 0x5110AB` 原调用之前，并以 601 次 `MC_POST_UPDATE` marker 确认 candidate 更早且位于同一主线程。当前证据见 [custom-sync-runtime-analysis.md](custom-sync-runtime-analysis.md)。对局开始、重连、退出时设备数组和两个 map 的创建/销毁仍需动态验证。

#### Lua 注册与 Mod 身份

`0x00B06960` 是此前宽泛序言特征的有效候选，而不是仅凭地址选中：函数把 `ECX` 保存为引擎对象，沿 `this+0x18 -> 0x00AFDAB0 -> 0x00B47E10` 取得本次注册使用的 `lua_State*`，随后以 `lua_pushcclosure(L, function, 0)` 形态注册现有 C 函数；例如 `0x00B069B3..0x00B069C1` 直接调用 `lua_pushcclosure`，相邻字符串为 `Vector`。这给出了新注册入口和原版 C 函数注册样例，但没有授权或实现 `Isaac.Sync`。

程序映像中存在 `/metadata.xml`、`mods` 和 `LOADED MOD %s` 字符串，说明 Mod 元数据加载链存在；本次静态路径没有把某个 metadata 字段证明为跨客户端稳定的 Mod ID 或版本值。因此端口派生、Mod 清单 hash 和 Lua 调用上下文中的 Mod 身份均为 **需要动态验证**，不得以路径、显示名或 DLL 名代替稳定 ID。

#### 原“仍待补充”项目的处理状态

| 项目 | 状态 | 当前结论或限制 |
| --- | --- | --- |
| 原版发送路径、类型、长度、校验 | 已确认（字段语义部分未命名） | type `1` 输入分支、`0x20` payload、`0x0C + 0x10` 校验覆盖范围已由镜像代码确认 |
| 原版接收路径、分发和反序列化 | 已确认（上游服务内部未完全命名） | 服务取消息 → 8 字节身份键查设备 → `0x00BA2010` 硬编码输入分支 → 两个 map 插入 |
| `007EECA0`、`00958160`、`00563410` ABI | 已确认/部分推断 | 当前 RVA、栈清理和关键参数已记录；消息工厂的两个上下文参数语义仍未知 |
| 原生自定义消息注册 | 未确认 | 追踪分支没有注册接口；不能基于原版 type `1` 推断可扩展性 |
| `playerSlot` / peer ID / local-remote | 部分确认 | local 判断和 8 字节 peer 键已确认；稳定数值 slot 未确认 |
| 对局、重连、退出生命周期 | 需要动态验证 | 静态代码不足以证明对象重建、复用与清理时序 |
| `LuaEngine::RegisterClasses()` 与样例 | 已确认 | 当前入口、`lua_State*` 获取路径和 `lua_pushcclosure` 样例已确认；完整注册表仍无需为 Sync 实现而穷举 |
| 单次 frame 分发点 | 已确认 | `RVA 0x5110AB` 原调用前；601/601 次早于同主线程 `MC_POST_UPDATE` marker，ready stall 与输入应用证据见拆分文档 |
| Mod 稳定 ID / version | 需要动态验证 | 仅确认 metadata 加载存在，未确认可同步身份字段 |
| 双客户端、日志、内存转储 | 需要动态验证 | 见下方无断点验证条件 |

#### 双客户端无断点验证条件

这些条件不是本次会话的实测结果。后续测试必须使用两个相同构建、相同 DLL 和相同 Mod 的客户端；每个客户端先读取自身模块基址并按 RVA 重定位，不能复制本会话 VA。

1. 只读记录 `M = *(moduleBase + 0x87169C)`、`M+0x4B3E4`、`M+0x4B3E8` 和 `M+0x4B3F0`；验证未 ready 时 frame 不增、ready 后恰好递增一次。
2. 使用现有的外部日志或经单独授权的非断点只读观测手段，抓取一个正常原版输入 payload；通过 `0x20` 长度、四字节校验值、`0x0C + 0x10` 覆盖范围和接收端 map 插入验证解析。不要改写 payload，也不要占用 `NetInputFrame+0x0E/+0x0F`。
3. 在对局创建、断线、重连和玩家离开四个阶段记录每个输入设备的 8 字节身份键、设备数组顺序以及 `+0x3A0/+0x3AC` 队列是否重建或清空；只有跨双方稳定后才能把它命名为 `playerSlot`。
4. 在候选窗口前后记录 Lua callback 与 simulation 的可见 frame，确认分发恰好一次且所有客户端次序一致；在此之前不把候选窗口当作安全 Hook 点。
5. 从双方各自加载的 metadata 中取得实际可访问的 ID/version 字段，并比较其规范化值；若无稳定字段，必须在协议层另行定义并握手，不能从文件路径推断。

在上述动态项确认前，不修改 `NetInputFrame` 的 16 字节布局，不占用 `+0x0E`，也不假定 `+0x0F` 的高位或 button mask 中存在可安全复用的保留位。
