# San9 Toolkit

这是一个面向《三国志 IX》的本地辅助工具仓库。当前仅支持 `SRC-SAN9PK-TC-101`：
由 x64 Core UI 图形主程序管理配置，再通过内部 x86 Bootstrap 挂起目标进程、注入 Runtime DLL，为原版窗口提供等比缩放和
进程内用户配置。仓库后续可以加入其他相互独立的辅助功能，不以窗口缩放作为项目边界。
逆向依据与验证结果保存在上级研究仓库的 `RE-SAN9PK-0062` 与 `RE-SAN9PK-0065`。

## 已知目标版本

- 文件：`San9PK.exe`
- 版本：繁体中文版 1.0.1.0
- 大小：2,636,800 字节
- SHA-256：`D20794AEFF67301EC2BF8C3BECB1E9944C68C6C0588FBFD4BF04E8597F0E5028`

启动器会校验 SHA-256；不匹配时不会启动或注入。恢复游戏主线程后，启动器还会等待
DLL 通过窗口属性报告安装成功；失败或超时不会留下一个未缩放的测试进程继续运行。
启动器还会在目标主线程恢复和 DLL 注入之前，将目标进程设为 system-DPI-aware，避免
GDI 缩放结果再被 Windows DPI 虚拟化二次缩放。GUI 可配置初始 DPI 缩放、窗口标题、
固定尺寸无边框、鼠标锁定键以及 1x～4x 虚拟游戏时钟。加速不读取或改写注册表
`GameSpeed`。

## 原型边界

- 游戏内部仍使用固定的 1024×768 逻辑画布和原有 16 位 DIB 后缓冲。
- 窗口标题使用原 EXE `0x604D64` 处 Big5 字节串的 Unicode 解码结果
  `三國志ⅨPK`，并调用宽字符窗口 API；不依赖机器的系统 ANSI 代码页。
- DLL 先挂钩呈现入口，等目标类窗口首次提交已确认的游戏后缓冲后，才将该窗口改成标准
  可缩放窗口；这避免误改启动阶段短暂出现的同类窗口。`WM_SIZING` 保持客户区 4:3。
- 最大化到非 4:3 屏幕时使用居中黑边。
- DLL 钩住主模块导入表中的 `BitBlt`，只接受选中 1024×768×16 位 `BI_RGB` DIB 的
  内存 DC。确认原作提交后，把 DIB 的 X1R5G5B5 原始行直接复制到
  `B5G5R5A1_UNORM` 动态纹理，由 D3D11 全屏三角形和单 Pass Catmull–Rom shader 完成
  倒置校正与高质量等比缩放；CPU 不再逐像素展开 BGRA8，可见窗口也不再使用
  `StretchBlt`。
  `BitBlt` hook 只记录最新后缓冲；原作整帧和脏矩形提交均在下一次调用立即
  `ReleaseDC`。Runtime 同时代理该导入，先调用原函数释放目标 HDC，再在同一调用栈执行
  待提交的 DXGI `Present`，避免 GDI 与 swap chain 同时占用主窗口。
- 命中后缓存原作后缓冲 DC；换框、缩放和 `WM_PAINT` 会主动重绘整帧，不依赖原作恰好
  再次提交画面。
- DLL 校验并挂钩主游戏消息泵的统一规范化入口 `0x5CC0B0`。硬件鼠标消息在进入 MFC
  消息表和自定义窗口路由前反变换到 1024×768；通过 `MSG.pt` 与客户区坐标的屏幕位置
  一致性识别硬件消息，原作内部 `PostMessage` 产生的逻辑消息不会被二次变换。
- 原作通过 `GetCursorPos` 主动轮询 hover 时，也返回与其无边框窗口坐标约定一致的逻辑
  位置。窗口子类不再改写鼠标消息，消息流和轮询流各自在唯一入口完成一次变换。
- 原作仍可发出整帧或局部脏矩形提交，但 D3D11 presenter 每次都从已确认后缓冲上传并
  呈现完整画布；render target 先清为黑色，再只在居中 4:3 viewport 绘制游戏纹理。
  安装后其他来源到主客户区的 `BitBlt` 被抑制，避免 GDI 和 DXGI 交替覆盖。
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

在 PowerShell 中运行：

```powershell
./build.ps1
```

图形程序使用 x64，Bootstrap 和 Runtime 使用 Win32。发布根目录只有用户入口
`San9Toolkit.exe`；`core-ui.dll` 位于 `bin\x64`，无窗口 Bootstrap 和 Runtime 位于
`bin\x86`。界面结构和样式由发布目录中的 `ui\app.uix` 定义，程序启动时直接读取该文件；
修改 UIX 后无需重新编译。缺少或无法解析 UIX 时程序会明确启动失败，不使用内嵌回退。
Runtime DLL 同样不内嵌、不释放也不缓存。

构建会先运行影片播放器的状态与时序单元测试。若需对本机原作影片做只读的 Media
Foundation 全量解码验证，可构建 `San9Toolkit.MovieProbe.vcxproj`，再把影片目录作为唯一
参数传给生成的 `San9Toolkit.MovieProbe.exe`；探针不复制或修改影片。

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
BorderlessWindow=0
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

已确认的 `Configs` 唯一键名共 22 个：

| 类别 | 键名 |
| --- | --- |
| 常用选项 | `MessageSpeed`、`GameSpeed`、`GameReport`、`GameReportDlg`、`JumpList`、`FullScreen`、`PlayBGM`、`PlaySound`、`PlayMovie`、`SvLdPage` |
| 原作维护的位置 | `YNPositionX`、`YNPositionY`、`KakuninPositionX`、`KakuninPositionY`、`AppPositionX`、`AppPositionY` |
| 原作维护的位集合 | `FlagData`、`ArtData` |
| 原作维护的试用剧情状态 | `TrialStory00`、`TrialStory01`、`TrialStoryEvent`、`TrialStoryClear` |

空 INI 启动时不会主动补齐这些键；原作查询缺值后使用自身默认值，实际保存对应状态时才由
Toolkit 写入。位置、`ArtData` 和剧情进度不作为用户开关，示例文件只用注释登记其名称。
`FlagData` 是包含界面及地图行为等内容的通用打包标志，并非单纯的解锁数据；GUI 仅暴露
已确认的地图滚动位，保存时保留其他位。`ArtData` 的逐位语义仍待确认。

## 安全与恢复

- 原版 EXE 不会被写入。
- 关闭游戏即可卸载所有进程内修改；Runtime 从 Toolkit 发布目录直接加载。
- 注入或恢复主线程失败时，启动器会终止它刚创建且仍处于挂起状态的进程。
- 不要把生成的 EXE 或 staging 文件提交到仓库。
