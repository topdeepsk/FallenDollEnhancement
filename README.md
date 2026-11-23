# GameEnhancement

一个最小可用的 UE5 外部增强工程：**一次构建产出注入器 exe 与被注入的 dll**，
命令行把 dll 注入指定进程。注入后提供两个功能：

| 功能 | 触发 | 作用 |
|---|---|---|
| **跳跃** | `Q` | 把玩家角色瞬间抬高 180（可配），随后自然下落。用于跳进正常走不到的区域探索未开放地图 |
| **动作放置** | 自动（左键时） | 让动作预览的"红区"（游戏判定为不可放置）也能落位 |

两个功能都不改代码段、不依赖函数地址，只做数据读写；放置功能额外借了引擎自己的提交路径。

面向 `Paralogue-Win64-Shipping.exe`（Fallen Doll / Paralogue）编写，
偏移取自本仓库 `插件项目/dump` 的同版本 dump，不是估计值。

---

## 1. 产物

| 产物 | 说明 |
|---|---|
| `build/bin/ge_inject.exe` | 注入器（x64 控制台程序），`CreateRemoteThread` + `LoadLibraryW` |
| `build/bin/GameEnhancement.dll` | 被注入的模块，导出 `GE_Install` / `GE_Uninstall` / `GE_QueryState` / `GE_AbiVersion` / `GE_JumpNow` / `GE_PreviewState` / `GE_ForcePlaceNow` / `GE_ScriptTraceState` / `GE_TraceDump` / `GE_ForceCommit` |
| `build/bin/GameEnhancement.ini` | 首次注入时自动生成的可调参数文件 |
| `build/bin/GameEnhancement.log` | 运行日志，与 ini 同目录 |

DLL 用 `/MT` 静态链接 CRT，依赖只有 `KERNEL32.dll` 与 `USER32.dll`，
换机器不需要装 VC 运行库。

---

## 2. 构建

```bat
build.cmd
```

`build.cmd` 会自己找 Visual Studio 并加载 `vcvars64`，然后
`cmake -G Ninja -DCMAKE_BUILD_TYPE=Release` + `cmake --build`。
如果已经在 VS 开发者命令提示符里，也可以手动：

```bat
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

要求：CMake ≥ 3.20、Ninja、MSVC x64 工具集（只支持 Windows / x64）。

> 重新构建前请先卸载：DLL 被游戏加载时文件是锁着的，链接会报 `LNK1104`。

---

## 3. 用法

```bat
:: 注入（默认在 exe 同目录找 GameEnhancement.dll）
ge_inject.exe Paralogue-Win64-Shipping.exe

:: 游戏还没起来，最多等 60 秒
ge_inject.exe Paralogue-Win64-Shipping.exe --wait 60

:: 指定 DLL 路径 / 按 PID 注入
ge_inject.exe 12345 --dll D:\tools\GameEnhancement.dll

:: 只卸载，不重新注入
ge_inject.exe Paralogue-Win64-Shipping.exe --eject

:: 不按 Q，直接触发一次跳跃（验证链路）
ge_inject.exe Paralogue-Win64-Shipping.exe --jump

:: 打印动作放置的预览状态（管理器地址 / 三个判定标志 / 幽灵坐标）
ge_inject.exe Paralogue-Win64-Shipping.exe --preview

:: 按当前幽灵位置强制改写一次放置判定（不点鼠标，排错用）
ge_inject.exe Paralogue-Win64-Shipping.exe --place

:: 打印脚本钩子接管状态（vtable 槽位，只读）
ge_inject.exe Paralogue-Win64-Shipping.exe --script

:: 导出最近两次点击抓到的脚本调用序列
ge_inject.exe Paralogue-Win64-Shipping.exe --dump

:: 不点鼠标，直接调用一次提交函数
ge_inject.exe Paralogue-Win64-Shipping.exe --commit
```

**重复注入**：直接再跑一次同样的命令即可。注入器会先远程调用
`GE_Uninstall`（摘钩子、停工作线程），再 `FreeLibrary`，确认模块真的消失后
才重新 `LoadLibrary`。

| 选项 | 作用 |
|---|---|
| `--dll <路径>` | 指定 DLL |
| `--eject` | 只卸载 |
| `--jump` | 让已注入的实例立刻跳一次，不必按 Q（排错用） |
| `--preview` | 打印动作放置的预览状态，只读 |
| `--place` | 按当前幽灵位置强制改写一次放置判定 |
| `--script` | 打印脚本钩子接管状态（只读） |
| `--dump` | 导出最近两次点击抓到的脚本调用序列 |
| `--commit` | 直接调用一次动作提交函数（不经过左键判定） |
| `--no-unload` | 跳过卸载直接注入（调试用，会留下旧实例的钩子） |
| `--skip-uninstall` | 卸载时跳过 `GE_Uninstall`，只 `FreeLibrary`（靠 DllMain 兜底） |
| `--wait <秒>` | 等待进程出现 |

注入成功会打印 `[4/4] 注入成功`，详细状态见 DLL 同目录的 `GameEnhancement.log`。
切回游戏按 `Q` 即触发跳跃；进到动作放置预览后左键即触发放置改写。

> 游戏若以管理员权限运行，注入器也必须以管理员运行，否则 `OpenProcess` 会失败（错误码 5）。

---

## 4. 配置

DLL 同目录的 `GameEnhancement.ini`，改完重新注入即生效：

```ini
GWorldRVA=0x0AFB9F48   ; GWorld 全局指针在模块内的 RVA
WorldZOffset=0x220     ; RootComponent 里 ComponentToWorld.Translation.Z（非反射字段）
JumpHeight=180         ; 一次抬升的高度（UE 单位，约等于厘米）
HoldMs=300             ; 抬升后保持多久，0 = 只写一帧
TickMs=8               ; 保持期间的重写间隔
HotkeyVK=0x51          ; 触发键虚拟键码，0x51 = Q
ZeroVelocity=1         ; 保持期间把竖直速度清零
```

动作放置部分：

```ini
PreviewFix=1               ; 总开关，0 = 不装鼠标钩子
PreviewLogOnly=0           ; 1 = 只把状态写进日志，不改内存
PreviewOnlyUnsnapped=1     ; 只在游戏判定为无效时介入
PreviewSetSnapWall=1       ; 改写 isSnappingWall
PreviewSetFloor=0          ; 改写 FloorPreviewZ
PreviewSetZ=1              ; 把 Z 写成幽灵当前高度
PreviewWriteHitLoc=1       ; 把 wallHitLoaction 写成幽灵位置
PreviewHoldMs=150          ; 点击后持续重写多久
```

`HoldMs` 不是"缓慢上升"：位置在第一帧就已经到顶了，之后的重写只是为了
**不让 CharacterMovement 把它当成悬空立刻拽回地面**。设为 0 会看到瞬移一下
马上落回，通常不可见。

> `HoldMs` / `ZeroVelocity` 只对"本机就是服务端"（`Role == Authority`）那条路径有效。
> 客户端模式下是跨进程写服务端（见 §5.1.2），只写一次、写完立刻进入自由落体，
> 这两个参数不参与。

---

## 5. 实现要点

### 5.1 定位玩家

纯反射链，全部按偏移读，不缓存任何实例指针（场上同时有 11 个 `AvatarPawn_C`，
切场景/换角色后旧指针必然失效）：

```
模块基址 + GWorldRVA            -> UWorld*
  + 0x228  OwningGameInstance   -> UGameInstance*
  + 0x038  LocalPlayers (TArray)-> ULocalPlayer*[0]
  + 0x030  PlayerController     -> APlayerController*
  + 0x2F0  Pawn                 -> APawn*（AvatarPawn_C）
  + 0x1B8  RootComponent        -> UCapsuleComponent*
  + 0x148  RelativeLocation     -> FVector（3 × double）
```

跳跃时写 `RelativeLocation.Z`，**同时**写 `RootComponent + 0x220`
（`ComponentToWorld.Translation.Z`），并可选择把 `CharacterMovement + 0xD8 + 16`
（`Velocity.Z`）清零。

### 5.1.1 为什么必须两个都写（本项目踩过的坑）

只写 `RelativeLocation` 不会调用 `USceneComponent::UpdateComponentToWorld()`，
渲染与物理继续使用缓存的世界变换。现象极具欺骗性：

- 内存里的 `RelativeLocation.Z` 确实变了，日志里 Z 一路涨到 3162；
- 但画面上角色一动不动，相机 Z 也纹丝不动；
- 因为 `ComponentToWorld` 还停在原点，游戏每帧都按它渲染。

实测数据（同一帧）：`RelativeLocation.Z = 1352.2`，而 `ComponentToWorld.Z = 92.2`。
两个都写之后，世界 Z 立刻从 92.2 到 272.2，相机随之升到 356，随后重力接管开始下坠。

`ComponentToWorld` 是非反射成员，dump 里查不到，只能手工定位：
`FTransform` 起始于 `+0x1F0`，`Translation` 三元组在 `+0x210`（X/Y/Z 依次 `+0x210/+0x218/+0x220`）。
换构建后这个偏移要重新找，所以它放在 ini 的 `WorldZOffset` 里而不是写死。

### 5.1.2 位置由服务端权威：跳跃必须写到另一个进程

上面那套读写只对"自己就是服务端"的情况成立。实测这个游戏是**服务端权威**的：
本机跑着两个同名的 `Paralogue-Win64-Shipping.exe`，一个是客户端（你操作的那个），
另一个是服务端。

判断方法：读 `AActor::Role`（`+0x168`，1 字节），
`0=None / 1=SimulatedProxy / 2=AutonomousProxy / 3=Authority`。
客户端里 `PlayerController` 和 `Pawn` 都是 `AutonomousProxy`，服务端里的 `AvatarPawn_C`
才是 `Authority`。

在客户端改 Z 的后果（实测数据）：

- 抬高 180 后，Z 在 **16～22ms（约 1 帧）** 内被写回原值，而且是精确回到原值，
  不是重力下落；
- 同时在服务端进程 `NtSuspendProcess` 挂起后重测，Z **保持 59ms 不动** ——
  证明写回来自服务端的网络位置复制（约 32ms 一次，正好是 30Hz）；
- 顺带说明：`bServerAcceptClientAuthoritativePosition`（`CharacterMovement + 0x565`
  的 bit 2）解决不了这件事，它管的是 `ServerMove` 的校验，管不了"服务端自己模拟出的位置
  往客户端复制"这条链路。

所以 `Q` 按下后的流程是：

```
读本地 pawn 的 Role
├─ 3 (Authority)        → 直接写本地（就是 §5.1 那套）
└─ 其它（客户端）        → PeerJumpUp()：跨进程写到服务端
```

`PeerJump.cpp` 做的事：

1. **找对端进程**：`EnumProcesses` + `QueryFullProcessImageNameW` 按 exe 名匹配（跳过自己），
   能用 `ReadProcessMemory` 读到 `GObjects` 的才算数；
2. **远程反射**：远端 `GObjects`/`GNames` 的 RVA 与本地相同（同一个构建），
   只是基址不同。按 §5.5.1 同样的布局远程遍历 `GUObjectArray`，收集所有 `AvatarPawn_C`；
   类名按类指针缓存，避免每个对象都解一次名字；
3. **认领角色**：用**世界坐标最近邻**匹配（客户端与服务端同一个 pawn 的位置，
   允许几十毫秒同步误差）。要求距离平方 < 400²，并校验该 pawn 的 `Role == Authority`，
   避免误伤 NPC；
4. **写入**：`RootComponent + 0x210 + 16`（世界 Z）加上高度，再把
   `CharacterMovement + 0x241`（`MovementMode`）写成 `3`（`MOVE_Falling`）。

服务端自己按重力下落，客户端在 40ms 内随网络同步跟随。实测数据（服务端抬高后两侧同时采样）：

```
+0   ms  服务端 Z= 72.1   客户端 Z=-107.9
+41  ms  服务端 Z= 71.9   客户端 Z=  71.0
+143 ms  服务端 Z= 61.8   客户端 Z=  61.8   ← 完全一致，自然下落
```

扫描结果缓存 5 秒（`g_scanTick`），避免每次按键都全量遍历几十万个对象。

`PeerJumpShutdown()` 在卸载时关掉持有的进程句柄。

### 5.2 偏移来源

| 字段 | 偏移 | 出自 dump 中的类 |
|---|---|---|
| `GWorld` RVA | `0x0AFB9F48` | dump `meta`（gworld − module_base） |
| `OwningGameInstance` | `0x228` | `World` |
| `LocalPlayers` | `0x038` | `GameInstance` |
| `PlayerController` | `0x030` | `Player` / `LocalPlayer` |
| `Pawn` | `0x2F0` | `Controller` |
| `RootComponent` | `0x1B8` | `Actor` / `Character` |
| `RelativeLocation` | `0x148` | `SceneComponent` |
| `ComponentToWorld.Translation` | `0x210`（Z 在 `0x220`） | **非反射**，手工定位 |
| `CharacterMovement` | `0x338` | `Character` |
| `Velocity` | `0x0D8` | `AvatarMovementComponent` |
| `Actor.Role` | `0x168` | `Actor`（`ENetRole`，1 字节） |
| `MovementMode` | `0x241` | `CharacterMovementComponent`（1 字节，`3` = `MOVE_Falling`） |
| `bJustTeleported` / `bServerAcceptClientAuthoritativePosition` | `0x564` bit5 / `0x565` bit2 | 同上（位域，用 `FBoolProperty` 的 `ByteMask` 判断） |

`FVector` 是 **24 字节**（UE5 LWC，3 × float64），Z 在 `+16`。
用 float 写会直接写坏数据。

### 5.3 钩子

- 键盘：`WH_KEYBOARD_LL` 低级钩子，装在独立线程的消息循环里
  （低级钩子的回调由安装线程的消息循环派发，不能挂在没有消息泵的线程上）。
- 只处理 `WM_KEYDOWN`；屏蔽 `LLKHF_INJECTED`（远程注入器/宏不算，避免别的工具误触发）；
  只在游戏窗口处于前台时触发。
- **不做按键去重**：长按会随系统自动重复连续抬升，每按一次叠加一个 `JumpHeight`。
  这是有意设计——逐段爬高比一次抬到顶更好控制。
- 钩子永远 `CallNextHookEx` 放行，不影响游戏自己的 Q 输入。
- 卸载顺序：先 `UnhookWindowsHookEx` 再停线程，然后才允许 `FreeLibrary`。

### 5.4 卸载与重复注入

`DllMain` 的 `DLL_PROCESS_DETACH`（`reserved == nullptr`，即 `FreeLibrary`）
也会调 `GE_Uninstall`，所以即使注入器用 `--skip-uninstall` 也不会留下悬空钩子。

`GE_Install` / `GE_Uninstall` 都是幂等的，用临界区串行化，避免
"DllMain 的自动安装线程"与"注入器的远程卸载"撞车。

---

### 5.5 动作放置：红区也能落位

游戏在放置动作（H 卡预览）时，幽灵模型是红色就表示"这里不能放"，左键点了只会弹
"请放置在平面上"。判定结果落在 `A_ClientHPreviewManager` 的三个布尔上：

| 属性 | 含义 |
|---|---|
| `isSnappingItem` | 吸附到家具/道具 |
| `isSnappingWall` | 吸附到墙面锚点（`WallBox_C`） |
| `FloorPreviewZ` | 吸附到地面 |

三个都为假 = 红色。左键处理函数 `InpActEvt_LeftMouseButton_K2Node_InputKeyEvent`
读这几个值决定"提交"还是"弹提示"。

做法是在低级鼠标钩子里、**在输入投递给游戏之前**把 `isSnappingWall`、`Z`、
`wallHitLoaction` 改写成"已吸附到幽灵当前位置"，让游戏走它自己的提交路径——
位置、朝向、网络 RPC 都是原生的，不碰代码段，也不依赖任何函数地址。

时序上有个坑：`ReceiveTick` 每帧都会重算并写回这几个标志。所以点击时先同步写一次，
再由工作线程在 `PreviewHoldMs` 内每 5ms 重写一次，盖住那一两帧的重算。日志里的
`预览[保持结束]` 会回读标志，用来判断是"被改回去了"还是"状态保住了但点击函数不认"。

因为 `PreviewOnlyUnsnapped=1` 默认只在红色时介入，绿色（合法）位置仍然走游戏原逻辑，
不会被改成贴墙分支。

### 5.5.1 为什么必须按名字在运行时找

这个游戏的玩法类（`A_DummyActor`、`A_ClientHPreviewManager` …）是 AngelScript 声明的
`ASClass`，dump 里根本没有这些类；dump 里的 `funcs[].addr` 也是 UFunction 对象地址，
不是代码地址。所以 `UeRuntime.cpp` 用 `GObjects` / `GNames` 在活进程里按名字找类、
沿 `ChildProperties` 链找属性偏移：

```
模块基址 + GObjectsRVA(0x0AB0D3D0) -> 块指针表 -> FUObjectItem[24]（对象指针在 +8）
UObject:  +0x10 Class / +0x18 FName / +0x20 Outer
UClass :  +0x40 Super / +0x50 ChildProperties
FProperty: +0x20 Name / +0x34 ElementSize / +0x48 Offset_Internal
```

首次扫描约 100ms，之后只在实例失效（换场景）时重扫，鼠标回调只做常数次读写。

## 6. 已知限制
1. **游戏更新即失效。** `GWorldRVA` 与各字段偏移都和具体构建绑定。
   重新 dump 后改 ini / `src/Common/UeOffsets.h` 即可，不用改代码。
2. **根组件被 attach 时两个坐标不再相等。** 代码会读 `AttachParent` 判断：
   无父级时两份写同一个值；有父级时保持原来的差值。父级若带缩放（极少见），
   实际抬升量会被父级变换缩放。
3. **必须同时改 `MovementMode`。** 只写坐标时组件仍处于 `Walking`，下一帧按贴地逻辑
   把角色拽回地面；跳跃会把 `MovementMode` 写成 `MOVE_Falling`，之后由重力接管。
   若游戏换用别的移动模式（例如能飞的相机 pawn），需要相应调整这个值。
4. **写世界变换跳过了碰撞扫描。** 抬得太高可能穿进几何体，落地位置由物理自行修正。
   这是"跳进不可达区域"的预期副作用，不做穿透修正。
5. **只支持"服务端在本机"的联机形态。** 客户端模式下要跨进程找同名进程写服务端，
   连远程服务器时找不到对端，`Q` 会退回本地写入并被网络同步覆盖（日志有明确提示）。
   带反作弊的会话会直接拦注入。
6. **只支持 x64 目标**，32 位进程会被直接拒绝。
7. **日志要写在 DLL 同目录**，该目录必须可写。若把 DLL 放进 `Program Files`
   这类受保护目录，日志会写不进去（此时只能靠调试器看 `OutputDebugString`）。
8. **放置修复有前提**：左键处理函数必须是"读这三个标志"来决定提交。如果它是自己
   重算一遍几何，改写标志不会生效——日志里会表现为 `预览[改写]` 有记录但点了没反应，
   此时需要改成钩 `ProcessEvent` 拦截左键函数（或直接调 `OnHActionCheckSuccess`）。
9. **红区落位可能穿模**：位置是幽灵当前位置，墙面/家具的碰撞不会替你修正。

---

## 7. 排错

DLL 日志：`GameEnhancement.log`，与 `GameEnhancement.ini` 同在 **GameEnhancement.dll 所在目录**
（注入器会打印完整路径）。这样日志、配置、DLL 三者放在一起，拷走一份就齐全了。

**先跑 `tools/probe.py`**，它只读地走一遍解析链并把每一步地址打出来，
不注入、不改游戏，是判断"到底是偏移过期还是游戏状态不对"最快的办法：

```bat
python tools\probe.py Paralogue-Win64-Shipping.exe
python tools\probe.py Paralogue-Win64-Shipping.exe --bump 20   :: 可逆写测试
```

`--bump 20` 会抬 20 厘米、150ms 后读回、再还原原值，用来确认写 Z 是否
真的推动角色。注意它会同时写 `RelativeLocation` 和 `ComponentToWorld`——
只写前者画面不会动（见 §5.1.1）。

| 现象 | 原因 |
|---|---|
| `OpenProcess 失败（错误码 5）` | 游戏是管理员权限，注入器也要提权 |
| `LoadLibraryW 返回 0` | 位数不符，或 DLL 依赖缺失（本工程应为 0 依赖） |
| 注入成功但按 Q 无反应 | 看日志有没有 `解析玩家失败`：多半在菜单/加载界面，或偏移已过期 |
| 日志里有 `跳跃 #N` 但画面不动 | `WorldZOffset` 过期（写到了错误地址），用 `probe.py --bump` 复测 |
| 按 Q 后角色"瞬间落地"、走不动 | 写的是客户端副本，被服务端每 ~32ms 覆盖。看日志有没有 `退回本地写入（会被网络同步覆盖）`，正常应走 `已交给服务端执行` |
| `跨进程跳跃：找不到同名的服务端进程` | 服务端不在本机（连的远程服务器），这条链路不可用 |
| `跨进程跳跃：对端找到 0 个 AvatarPawn_C` | 远端名字池地址算错：`base + GNamesRVA` **就是** `FNamePool` 结构体，不能再解一次指针；或挑到了错误的同名进程 |
| `跨进程跳跃：最近的 Authority 角色距离 … 超出容忍范围` | 坐标匹配失败（对角/切场景瞬间位置不同步），再按一次通常就好 |
| `FreeLibrary 之后模块仍然存在` | 有别的工具/线程持有引用，重启游戏再试 |
| 链接报 `LNK1104` | DLL 正被游戏加载，先 `--eject` |

---

## 8. 目录结构

```
GameEnhancement/
├── CMakeLists.txt
├── build.cmd                  # 一键构建（自动找 VS）
├── tools/
│   └── probe.py               # 只读验证偏移链（可选 --bump 做可逆写测试）
└── src/
    ├── Common/
    │   ├── UeOffsets.h        # 偏移表 + 来源注释
    │   ├── Win32Util.h/.cpp   # 自读自写内存、日志
    │   └── CMakeLists.txt
    ├── Dll/
    │   ├── dllmain.cpp        # DllMain + 四个导出
    │   ├── Api.h              # C ABI 声明
    │   ├── Config.h/.cpp      # ini 读写
    │   ├── Player.h/.cpp      # 解析玩家 + 跳跃工作线程
    │   ├── PeerJump.h/.cpp    # 客户端模式下跨进程写服务端（远程反射 + 最近邻匹配角色）
    │   ├── UeRuntime.h/.cpp   # 运行时对象表（GObjects/GNames）按名字找类与属性
    │   ├── PreviewBypass.h/.cpp # 动作放置：鼠标钩子 + 判定状态改写
    │   ├── KeyHook.h/.cpp     # 低级键盘钩子安装/卸载
    │   └── CMakeLists.txt
    └── Injector/
        ├── main.cpp           # 命令行
        ├── Injector.h/.cpp    # 注入 / 卸载 / 远程调用
        ├── PeExport.h/.cpp    # 解析导出表取 RVA
        └── CMakeLists.txt
```
