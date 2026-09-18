# 本地第三方依赖

本目录保存 San9 Toolkit 的本地第三方源码参考与已构建 SDK，不进入版本库。

- `core-ui-v1.8.0/`：`cdd1037/core-ui` 的 `codex/align-v1.8` 源码仓库。
- `core-ui-sdk-v1.8.0/`：从 `cdd1037/core-ui` 的 `v1.8.0-cdd.1` Release 下载并校验 SHA-256 的 Windows x64 静态 CRT SDK，也是 `build.ps1` 的默认 Core UI 输入。

SDK 必须至少包含 `include/ui_core.h`、`lib/dynamic/core-ui.lib` 和 `lib/dynamic/core-ui.dll`。

GitHub Actions 下载同一 Release 的预构建 SDK 并校验 SHA-256。本地参考源码不作为已发布 SDK 的版本依据。
