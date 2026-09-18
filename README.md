# San9 Toolkit

这是一个面向《三国志 IX》的现代化本地工具箱。当前仅支持繁体中文版威力加强版 1.0.1.0：
由 x64 Core UI 图形主程序管理配置，再通过内部 x86 Bootstrap 挂起目标进程、注入 Runtime DLL，为原版窗口提供等比缩放和
进程内用户配置。仓库后续可以加入其他相互独立的辅助功能，不以窗口缩放作为项目边界。

## 下载

普通玩家请从 [GitHub Releases](https://github.com/cdd1037/san9-toolkit/releases/latest) 下载
`San9Toolkit-*-windows.zip`，完整解压后运行 `San9Toolkit.exe`。不要只取出主程序；
`bin` 和 `ui` 目录也是运行所必需的。工具箱不包含游戏本体、影片、存档或其他原版资源。

系统要求为 Windows 10/11 x64。当前只接受下列目标程序，其他版本会在启动前被拒绝，
不会修改原版 EXE。

## 已知目标版本

- 文件：`San9PK.exe`
- 版本：繁体中文版 1.0.1.0
- 大小：2,636,800 字节
- SHA-256：`D20794AEFF67301EC2BF8C3BECB1E9944C68C6C0588FBFD4BF04E8597F0E5028`

启动器会校验 SHA-256；不匹配时不会启动或注入。恢复游戏主线程后，启动器还会等待
DLL 通过窗口属性报告安装成功；失败或超时不会留下一个未缩放的测试进程继续运行。
启动器还会在目标主线程恢复和 DLL 注入之前，将目标进程设为 system-DPI-aware，避免
GDI 缩放结果再被 Windows DPI 虚拟化二次缩放。GUI 可配置初始 DPI 缩放、窗口标题、
无边框全屏、鼠标锁定键以及 1x～4x 虚拟游戏时钟。加速不读取或改写注册表
`GameSpeed`。

## 功能与边界

- 游戏内部仍使用固定的 1024×768 逻辑画布和原有 16 位 DIB 后缓冲。
- 窗口标题使用原 EXE `0x604D64` 处 Big5 字节串的 Unicode 解码结果
  `三國志ⅨPK`，并调用宽字符窗口 API；不依赖机器的系统 ANSI 代码页。
- DLL 先挂钩呈现入口，等目标类窗口首次提交已确认的游戏后缓冲后，才将该窗口改成标准
  可缩放窗口；这避免误改启动阶段短暂出现的同类窗口。`WM_SIZING` 保持客户区 4:3。
- 有边框窗口最大化时仍处于 Windows 原生最大化状态，但客户区限制为工作区内水平居中、
  底部贴齐的最大整数 4:3；取整余量留在上方，窗口外侧保留桌面，不在客户区内补黑边。
- 无边框全屏隐藏标题栏和边框，在上次使用的显示器工作区内采用同一最大 4:3 底部贴齐几何，
  不覆盖任务栏。该模式不读取或覆盖已保存的普通窗口位置、尺寸和最大化状态；关闭后
  恢复先前的有边框窗口 placement。“自动调整窗口大小”在此模式下不参与尺寸计算。
- Toolkit 独占主窗口几何状态：以 96-DPI 逻辑单位保存正常窗口相对显示器工作区的位置、
  客户区宽度和最大化状态，高度由 4:3 推导。重启时优先恢复到原显示器；显示器缺失或
  工作区缩小时回退到当前主显示器并把完整窗口约束在可见工作区内。最小化不会覆盖正常
  窗口状态。原作 `AppPositionX/Y` 的读取按缺失处理、写入仅确认成功而不再持久化。
- DLL 钩住主模块导入表中的 `BitBlt`，只接受选中 1024×768×16 位 `BI_RGB` DIB 的
  内存 DC。更新后的 RGB555 画面上传到纹理，以一次双线性采样完成倒置校正及颜色格式转换，
  写入固定 1024×768 的 BGRA8 后缓冲；DXGI Flip Model 交换链通过
  `DXGI_SCALING_STRETCH` 交给 Windows 显示链路适配实际窗口尺寸。窗口变大不会增大
  Toolkit 的后缓冲或着色器工作量，也不再运行原来的 16 次采样 Catmull–Rom 缩放。
  系统缩放的画质与原来的滤波不同，文字锐度需实际验收。
- `BitBlt` 仅标记最新画面，`ReleaseDC` 先释放原作目标 HDC，再尝试显示。连续更新合并，
  上传和呈现最多每秒 60 次；交换链最多排队一帧，以非阻塞方式检查可提交状态，并使用
  同步呈现。未到提交时间或队列忙时由窗口线程定时器继续处理，保留最后一次更新。
  不在游戏线程中睡眠、等待垂直同步或让工作线程读取原作正在修改的 DIB。
- 窗口重绘和调整大小复用已上传纹理，仅在原作提交新画面时重新上传。画面无更新且没有
  重绘请求时停止定时器；隐藏或最小化时不上传、不绘制，恢复后处理保留的更新。
- DLL 校验并挂钩主游戏消息泵的统一规范化入口 `0x5CC0B0`。硬件鼠标消息在进入 MFC
  消息表和自定义窗口路由前反变换到 1024×768；通过 `MSG.pt` 与客户区坐标的屏幕位置
  一致性识别硬件消息，原作内部 `PostMessage` 产生的逻辑消息不会被二次变换。
- 原作通过 `GetCursorPos` 主动轮询 hover 时，也返回与其无边框窗口坐标约定一致的逻辑
  位置。窗口子类不再改写鼠标消息，消息流和轮询流各自在唯一入口完成一次变换。
- 原作仍可发出整帧或局部脏矩形提交；同一显示周期内合并为一次完整画面更新。
  普通窗口仍保留居中的 4:3 视口及必要黑边，鼠标映射使用同一视口；最大化与无边框全屏
  的客户区本身为 4:3。安装后其他来源到主客户区的 `BitBlt` 被抑制，避免 GDI 和 DXGI 交替覆盖。
- 默认按 F12（可在 GUI 中改绑单个键）切换鼠标锁定；按下后由鼠标穿透、不可激活的透明覆盖窗口在画面中央短暂显示
  “鼠标已锁定在游戏画面内”或“鼠标锁定已解除”。覆盖窗口不参与原作逐帧 GDI 提交，
  地图连续滚动时不会被游戏帧交替覆盖。快捷键在原作统一消息规范化入口处理，因为注册
  到原作内部窗口路由的键盘消息不会经过后来安装的 Win32 子类窗口过程。锁定范围是当前居中的 4:3
  游戏视口，不包含标题栏、边框或黑边；鼠标仍能进入原作的地图边缘滚动区。窗口失焦、
  最小化或正在移动和缩放时会
  临时解除，恢复操作后自动重新锁定；再次按 F12 则彻底解除。原作 F8 的地图卷动模式
  和 F10 的暂停快捷键保持不变。
- Runtime 为 `HKCU\Software\KOEI\San9 Tc` 与 `HKCU\Software\KOEI\San9PK Tc` 两个
  本作 profile 根提供进程内虚拟注册表。原作要求的 `Install\InstallInfo` 由当前
  `San9PK.exe` 目录生成；两处 `Configs` 下的 DWORD 设置读取、修改和删除都映射到
  `data\San9Toolkit.ini`，不要求用户预先创建游戏注册表键。
  该隔离只覆盖本作用户配置；系统设备枚举、Windows 卸载信息及其他安装/授权检查继续
  调用原生注册表 API。空 INI 时除 `FullScreen=0` 外均使用原作编译默认值。
- 2x～4x 加速只替换主游戏模块通过导入表调用的 `timeGetTime`；1x 不安装该钩子。首次调用以真实毫秒值为锚点，
  此后按相邻真实采样的无符号差值累加所选倍数，切入时不会产生时间跳变，也保留 WinMM
  `DWORD` 计时回绕语义。Windows 系统时间、注册表 `GameSpeed`、音频设备时钟和其他进程
  不受影响。本轮验收重点是地图单位移动、通用 motion 动画及原作等待循环是否约为 2×；
  影片同步、所有事件演出和长时间运行仍需实机验证。
- 启动器以窗口属性 `San9Toolkit.RuntimeStatus` 等待 DLL 完成安装；该属性只用于启动握手，
  不参与缩放或输入行为。

它暂不处理触摸、数位笔、原始输入、跨不同 DPI 显示器迁移或全屏模式切换。第一版只应在
原作设置为窗口模式时使用。

## 构建

头像 PNG 绕过原作 256 色调色板的研究性 RGB555 方案、地址边界、裁剪规则和已知透明限制，见
[`docs/portrait-palette-bypass.md`](docs/portrait-palette-bypass.md)。该原型目前只覆盖头像 ID 9 的
L 头像及调色板变体 0，不是已完成的通用头像功能；配套实现仅保存在实验分支
`codex/portrait-palette-bypass`，不属于主分支产品代码。
供不了解本项目的外部开发者单独阅读和转发的完整实验指南，见
[`docs/san9-png-portrait-hook-standalone.md`](docs/san9-png-portrait-hook-standalone.md)。

GUI 依赖上游 [ghboke/core-ui](https://github.com/ghboke/core-ui) 的
[`v1.8.0`](https://github.com/ghboke/core-ui/releases/tag/v1.8.0)，并在该版本上保留本项目所需的
两处修复（ScrollView padding 视口和 `.uix` 原生导航控件）。本地参考源码放在
`third_party/core-ui-v1.8.0/`，使用静态 MSVC 运行库的 Windows x64 SDK 从
[`v1.8.0-cdd.1`](https://github.com/cdd1037/core-ui/releases/tag/v1.8.0-cdd.1) 下载并校验 SHA-256 后放在
`third_party/core-ui-sdk-v1.8.0/`；两者不进入本仓库版本历史。默认构建直接使用后者，也可通过
参数明确指定其他 SDK 路径。构建需要安装带 C++ 桌面工具链的 Visual Studio 和 Windows 10 SDK。

```powershell
./build.ps1
./build.ps1 -CoreUiRoot C:\SDK\core-ui-sdk-v1.8.0
```

测试使用独立入口，不会读取游戏、影片或其他原版资源：

```powershell
./test.ps1
```

带图形桌面和 D3D11 硬件的本机可额外运行呈现集成测试；测试短暂打开合成画面窗口，不启动游戏：

```powershell
./test.ps1 -Presenter
```

该测试覆盖高频更新合并、最后一帧延迟提交、空闲停绘、缩放重绘复用纹理、隐藏恢复和影片切换。
真实游戏的鼠标命中、缩放画质、地图动画、影片音画同步和噪音改善仍需玩家实机验收。


生成可发布 ZIP 和 SHA-256 校验文件：

```powershell
./package.ps1 -CoreUiRoot C:\SDK\core-ui-sdk-v1.8.0
```

图形程序使用 x64，Bootstrap 和 Runtime 使用 Win32。正式 Windows 玩家包中的
`San9Toolkit.exe`、`core-ui.dll`、Bootstrap 和 Runtime 均静态链接 MSVC C/C++ 运行库（`/MT`），
不依赖 `MSVCP140.dll`、`VCRUNTIME140*.dll` 或 `CONCRT140.dll`，因此玩家无需另行安装
x86/x64 Visual C++ Redistributable。发布根目录只有用户入口 `San9Toolkit.exe`；
`core-ui.dll` 仍作为项目自带动态库放在 `bin\x64`，无窗口 Bootstrap 和 Runtime 位于
`bin\x86`。界面结构和样式由发布目录中的 `ui\app.uix` 定义，程序启动时直接读取该文件；
修改 UIX 后无需重新编译。缺少或无法解析 UIX 时程序会明确启动失败，不使用内嵌回退。
Runtime DLL 同样不内嵌、不释放也不缓存。

官方 CI 固定下载 `cdd1037/core-ui` 的 `v1.8.0-cdd.1` Windows x64 SDK；该 SDK 已以
静态 CRT（`/MT`）构建，因此无需在每次 Toolkit 构建时重新编译 Core UI。随后 CI 会运行
Toolkit 测试、打包，并使用 `dumpbin /dependents` 检查四个发布二进制，若仍导入上述
VC Runtime DLL 则构建失败。
本地通过预编译 Core UI SDK 构建时，`core-ui.dll` 的 CRT 模式取决于该 SDK 本身。

CI 会先运行不依赖游戏资源的状态、缓冲和时序逻辑测试，再单独构建和打包。若需对本机原作
影片做只读的 Media Foundation 全量解码验证，可构建 `San9Toolkit.MovieProbe.vcxproj`，
再把影片目录作为唯一参数传给生成的 `San9Toolkit.MovieProbe.exe`；探针不复制或修改影片。

## 使用

保持发布包目录结构，直接运行：

```powershell
./San9Toolkit.exe
```

首次打开时会自动识别主程序同目录的 `San9PK.exe`；也可在 GUI 中直接选择其他位置的
`San9PK.exe`。选择后会校验文件名和已支持版本的 SHA-256。配置保存到
`data\San9Toolkit.ini`。启动完成后 GUI 自动隐藏，游戏退出后恢复并重读原作可能写回的设置。

GUI 会管理以下配置：

```ini
[Toolkit]
GameExecutable=D:\Games\San9\San9PK.exe
DocumentsRoot=D:\Games\San9Documents
CursorLockVirtualKey=123
ScaleInitialWindowForSystemDpi=1
WindowTitle=三國志ⅨPK
BorderlessFullscreen=0
GameSpeedMultiplier=2

[Configs]
MessageSpeed=1
GameReport=1
PlayBGM=1
PlaySound=1
PlayMovie=1
FlagData=7681
FullScreen=0
```

“存档与用户数据”功能用于把原作默认写入 Windows“文档”的用户文件集中到指定磁盘，
便于备份、便携管理或隔离不同环境。`DocumentsRoot` 会在游戏进程内替换该系统位置；原作继续自行追加
`Koei\San9 Tc` 和 `Koei\San9PK Tc` 并按原有行为创建目录。Toolkit 不迁移或复制旧位置的
存档、登录武将等文件；需要沿用时应由用户自行复制。路径必须是目标游戏当前 ANSI 代码页
可表示的绝对路径，并为原作追加的子目录保留在 `MAX_PATH` 内。`GameReportDlg` 和
`JumpList` 在 GUI 中仅作禁用占位符。

已由原作界面标签及按钮处理函数共同确认：`MessageSpeed` 为 `0=快`、`1=普通`、
`2=暫停`；`GameReport` 对应界面“進行記錄”，为 `0=OFF`、`1=要約`、`2=詳細`。
当前目标版本只加载、范围夹取和保存 `GameSpeed`，未发现运行时消费者；Toolkit 的 2×
虚拟时钟不读取该项。`GameReportDlg` 和 `JumpList` 的三档标签仍待确认。

“地图滚动方式”对应原作 F8：`FlagData` 的 `0x1000` 位设置时为边缘悬停滚动，清除时为
按住左键滚动。GUI 修改该位时会保留 `FlagData` 的其他位；缺少此项时采用原作编译默认值
`7681 (0x1E01)`，即边缘悬停。

影片播放由 Toolkit 在游戏窗口内完成：Media Foundation 解码原作实际为 AVI 的
WMV3/MP3 文件，XAudio2 提供独立于游戏倍率的音频主时钟，画面复用游戏现有 D3D11
交换链。播放期间左键释放或 Esc 可跳过；失焦、最小化时暂停。格式或解码失败会恢复游戏
画面并提示“影片无法播放，已跳过”，不会回退到旧式影片窗口。

每次开始播放影片时，Toolkit 会在配置文件同目录创建 `San9Toolkit.movie.log`，记录解码
进度、音视频队列、音频时钟、最后交给呈现器的影片帧和结束条件，并至少每秒强制刷盘一次。若游戏在
影片期间卡住或被强制结束，请保留该文件用于诊断。下一次影片开始时，旧记录会轮换为
`San9Toolkit.movie.previous.log`；单个记录文件最大 1 MiB，不包含影片完整路径或媒体内容。

已确认的 `Configs` 唯一键名共 22 个：

| 类别 | 键名 |
| --- | --- |
| 常用选项 | `MessageSpeed`、`GameSpeed`、`GameReport`、`GameReportDlg`、`JumpList`、`FullScreen`、`PlayBGM`、`PlaySound`、`PlayMovie`、`SvLdPage` |
| 原作维护的对话框位置 | `YNPositionX`、`YNPositionY`、`KakuninPositionX`、`KakuninPositionY` |
| Toolkit 接管、原作不再持久化 | `AppPositionX`、`AppPositionY` |
| 原作维护的位集合 | `FlagData`、`ArtData` |
| 原作维护的试用剧情状态 | `TrialStory00`、`TrialStory01`、`TrialStoryEvent`、`TrialStoryClear` |

空 INI 启动时不会主动补齐这些原作键；原作查询缺值后使用自身默认值，实际保存对应状态时
才由 Toolkit 写入。对话框位置、`ArtData` 和剧情进度不作为用户开关，示例文件只用注释
登记其名称。主窗口状态由 `[WindowState]` 的单一版本化 `Placement` 值维护，删除该值即可
恢复默认几何。
`FlagData` 是包含界面及地图行为等内容的通用打包标志，并非单纯的解锁数据；GUI 仅暴露
已确认的地图滚动位，保存时保留其他位。`ArtData` 的逐位语义仍待确认。

## 安全与恢复

- 原版 EXE 不会被写入。
- 关闭游戏即可卸载所有进程内修改；Runtime 从 Toolkit 发布目录直接加载。
- 注入或恢复主线程失败时，启动器会终止它刚创建且仍处于挂起状态的进程。
- 不要把生成的 EXE 或 staging 文件提交到仓库。

## 第三方组件

发布包携带上述 fork 构建的 `core-ui.dll`。Core UI 及其内嵌组件的来源和许可证见
[`THIRD_PARTY_NOTICES.md`](THIRD_PARTY_NOTICES.md)。本仓库未声明项目自身的开放源代码
许可证；第三方许可证只适用于各自组件。
