# 本地第三方依赖

本目录保存 San9 Toolkit 的本地第三方源码参考与已构建 SDK，不进入版本库。

- `core-ui-v1.8.0/`：`cdd1037/core-ui` 的 `codex/align-v1.8` 源码仓库。
- `core-ui-sdk-v1.8.0/`：由上述源码的 `release-package` 目标生成的 Windows x64 SDK，也是 `build.ps1` 的默认 Core UI 输入。

SDK 必须至少包含 `include/ui_core.h`、`lib/dynamic/core-ui.lib` 和 `lib/dynamic/core-ui.dll`。

在 Visual Studio 开发环境中可用 Core UI 的 `release-package` CMake 目标重新生成 SDK；GitHub Actions 也从同一分支执行该流程，不下载旧版本二进制。
