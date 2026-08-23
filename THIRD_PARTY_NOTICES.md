# Third-party notices

San9 Toolkit 的发布包包含以下第三方二进制或由第三方源代码静态构成的二进制。本文件不改变
San9 Toolkit 自身代码的授权状态。

## Core UI

- 使用版本：`v1.7.0-cdd.1`（基于 Core UI 1.7.0 build 253 的 fork 构建）
- Fork：https://github.com/cdd1037/core-ui
- 上游：https://github.com/ghboke/core-ui
- 许可证：MIT

```text
MIT License

Copyright (c) 2026 Core UI contributors

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

## Core UI 内嵌组件

Core UI 的动态库内嵌 [QuickJS-NG v0.14.0](https://github.com/quickjs-ng/quickjs)、
[LunaSVG](https://github.com/sammycage/lunasvg) 和
[PlutoVG](https://github.com/sammycage/plutovg)。这些组件采用 MIT 许可证；PlutoVG 的部分
FreeType 派生文件同时保留 [FreeType Project License](https://gitlab.freedesktop.org/freetype/freetype/-/blob/master/docs/FTL.TXT)
声明。对应源代码、版权声明和完整许可证文本可在上述项目及 Core UI fork 的
`third_party` 目录中查阅。

QuickJS-NG copyright (c) 2017-2026 Fabrice Bellard; 2017-2024 Charlie Gordon;
2023-2026 Ben Noordhuis; 2023-2026 Saúl Ibarra Corretgé.

LunaSVG and PlutoVG copyright (c) 2020-2026 Samuel Ugochukwu.

```text
Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```
