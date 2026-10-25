# onlineInputDelay 与输入帧链路

## 目标

本研究确认 `onlineInputDelay` 如何从 `g_Manager` 的配置字段进入输入帧队列，并最终改变物理输入被应用到 simulation frame 的时机。

## 结论

`onlineInputDelay` 不是一个按毫秒等待的计时器。它控制本地输入帧预填到未来的 frame 数：

```text
current online frame + onlineInputDelay
  -> 生成未来的本地 NetInputFrame
  -> 广播并等待各玩家同一 frame 的输入
  -> 消费 current frame 的输入
  -> current online frame++
```

因此 delay 增大时，当前物理输入被绑定到更晚的 simulation frame，玩家感受到输入延迟。

## 字段与对象关系

| 字段 | 含义 |
| --- | --- |
| `g_Manager + 0x2A3F4` | `onlineInputDelay` 源字段 |
| `g_Manager + 0x4B3E4` | 当前 online frame/tick |
| `g_Manager + 0x4B3E8` | delay 本地缓存 |
| `g_Manager + 0x4B3F0` | delay 增大时设置、推进成功后清除的 dirty 标志 |
| `g_Manager + 0x4B3D8` | 在线输入同步管理对象 |

`sub_007EB150` 的 `this` 是 `g_Manager + 0x4B3D8`。它将源字段同步到 `this+0x10`，即 `g_Manager+0x4B3E8`；若新 delay 更大，则置 `this+0x18`，即 dirty 标志。

## 核心函数职责

这些名称是行为名称；地址是版本相关的，必须按 RVA/特征重新定位。

| 行为 | 历史地址 | 职责 |
| --- | --- | --- |
| `FindInputFrame` | `007E1190` | 按 frame 在输入帧 map 中查找；找不到返回 `0` |
| `PrepareFutureInput` | `007E1CB0` | 以 `currentFrame + delay` 为上限生成并补齐未来输入帧 |
| `ProcessInputMessage` | `007E2010` | 校验并接收远端输入帧，写入队列 |
| `BroadcastFrameInput` | `007E2950` | 序列化并广播本地 frame 输入 |
| `UpdateAndApplyInput` | `007F0980` | 仅在所有设备 ready 时应用当前 frame 输入并推进 frame |

## 数据流

### 本地输入

`PrepareFutureInput` 从 `g_Manager+0x4B3E4` 取得当前 frame、从 `+0x4B3E8` 取得缓存 delay，并执行等价判断：

```text
nextInputFrame <= currentFrame + delay
```

满足时构造固定 16 字节 `NetInputFrame`，插入 `NetInputDevice+0x3A0` 与 `+0x3AC`，再调用本地广播路径。dirty 标志置位时，函数会额外以 delay 补齐队列，避免 delay 增大后未来帧不足。

### 远端输入

接收路径先拒绝早于 `currentFrame` 的 frame，再用 `FindInputFrame(this+0x3A0, frame)` 丢弃重复输入。校验通过后将同一帧插入 `+0x3A0` 与 `+0x3AC`。

### 消费与推进

`UpdateAndApplyInput` 对每个设备准备未来输入。任一设备未 ready 时立即返回 false，不应用输入也不递增 frame。全部 ready 后，它从每个 `device+0x3A0` 取出 `currentFrame` 对应的 16 字节输入帧，写入设备当前输入状态，最后递增 online frame。

## 已确认的输入状态映射

`UpdateAndApplyInput` 将已消费输入帧写入下列字段：

| `NetInputDevice` 偏移 | 状态 |
| --- | --- |
| `+0x380`, `+0x384` | 当前按键 mask |
| `+0x388`, `+0x38C` | 本帧新按下 mask |
| `+0x390..+0x39C` | 四个轴值 |
| `+0x3D0`, `+0x3D4` | 输入类型/轴模式标志 |

## 当前运行实例

当前实例的模块基址、RVA 和特征锚点见 [custom-sync-runtime-analysis.md](custom-sync-runtime-analysis.md)。旧记录中的 VA 仅保留作历史分析参考，不能直接用于当前进程。

## 剩余验证

- 在 delay 为 `0`、`1` 与 `N` 时，使用无断点的双客户端观测确认实际应用 frame 是否严格遵循 `currentFrame + delay`。
- 确认输入帧 map 的创建/销毁时机以及 `+0x3A0` 与 `+0x3AC` 的完整职责差异。
- 不修改 `NetInputFrame` 的 16 字节布局，也不复用 `+0x0E/+0x0F` 的任何位。

完整反汇编上下文、早期排除项和历史动态观察保留在 [README.md](../README.md)。
