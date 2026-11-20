# 第三方许可与依赖

维护者于 2026-11-06 选择 Apache-2.0 用于社区导出形态的自有代码，范围见仓库 `LICENSE-SCOPE.md`。项目许可证不替代第三方许可，不表示安装包再分发已获许可。

| 依赖 | 本地证据／版本 | 当前处理 |
|---|---|---|
| Qt | 最低 6.4，当前内部 6.8.3；所用开放许可须逐模块核对 | 社区源码不附 Qt 二进制；分发安装包时落实 LGPLv3 等相应条件，不能只附项目 Apache 许可证 |
| QtKeychain | 固定提交 875f77d9f61bd97fd84cca47ce3bc71186dfbd09，Modified BSD／BSD-3-Clause | 不导出依赖源码副本；许可全文见 `licenses/third-party/qtkeychain-BSD-3-Clause.txt`；上游源码含 Frank Osterfeld 的版权声明，二进制分发时保留相关声明 |
| libsodium | 固定 1.0.22，ISC | 不导出预编译库；许可全文见 `licenses/third-party/libsodium-ISC.txt` |
| zlib | 本地 MinGW 工具链 1.2.13，zlib License | CMake 静态链接 `ZLIB::ZLIB`；不导出预编译库，保留 `licenses/third-party/zlib.txt` |
| 达梦 DPI／dmdbg | 使用者提供 SDK／调试器 | 不导出供应商文件，安装包再分发待确认 |
| Wine、旧版 Noto Sans 中文回退字体及 Lobby 素材 | 内部打包输入或私有资源 | 原有隔离边界保持不变 |
| Noto Serif CJK SC | 官方 noto-cjk 固定提交 f8d157532fbfaeda587e826d4cd5b21a49186f7c；SIL OFL 1.1 | 应用界面宋体风格；字体、许可与哈希位于 `clients/qt/resources/fonts/`，通过资源加载，不安装系统字体 |
| MesloLGS NF | [Powerlevel10k 字体来源](https://github.com/romkatv/powerlevel10k/blob/master/font.md) | 社区源码不附带此字体。编辑器可使用系统已安装的 MesloLGS NF，或回退到系统等宽字体 |
| Tree-sitter 支持头文件 | 0.26.11，MIT，Copyright (c) 2018 Max Brunsfeld | 社区语法目录包含上游支持头文件，保留 MIT 全文 `licenses/third-party/tree-sitter-MIT.txt`，不被项目 Apache 许可覆盖 |
| Rust 依赖 | 锁文件配合 `cargo metadata --offline --locked` 识别 35 个外部包 | 许可表达式及版本见 `licenses/third-party/rust-dependencies.json`；主要为 MIT／Apache-2.0，另有 Unicode-3.0 等条件，分发二进制时按实际链接内容补齐声明 |
| 应用图标、主题、提示音 | `clients/qt/resources` | 随维护者授权的社区源码发布 |

新增的 LOB 文件格式识别与已有 Lobby ZIP 读取共用 zlib 原始 Deflate 解压，使用项目现有 MinGW 工具链的静态 `libz.a`（当前头文件版本 1.2.13），通过 CMake `ZLIB::ZLIB` 链接。没有新增下载或运行时 DLL；未把它称为当前上游最新版本。上游为 [zlib](https://zlib.net/)，许可文本保存在 `licenses/third-party/zlib.txt`。文件格式探测仅解压有界的小型内容类型成员，不展开整个用户附件；Lobby 归档仍沿用其原有单项及总量预算。

## 供应商文件

达梦 DPI 和 dmdbg 由使用者依其取得的许可自行提供。发布含供应商文件的安装包前，需要核对供应商二进制的再分发权利。

Qt、QtKeychain、libsodium 和 Tree-sitter 等已有开放许可的依赖，通常按相应条款使用与分发即可，并非全部都要另向作者申请书面许可；这不豁免保留版权、许可文本和其他条款义务。

## 已核对的一手依据

- [Apache-2.0 标准条款](https://www.apache.org/licenses/LICENSE-2.0)：专利授权、通知和商标边界。
- [Qt 6.8 许可说明](https://doc.qt.io/qt-6.8/licensing.html)、[Qt 的 LGPL 义务说明](https://www.qt.io/development/open-source-lgpl-obligations)：使用的模块许可及二进制分发条件。
- [QtKeychain 固定提交 COPYING](https://raw.githubusercontent.com/frankosterfeld/qtkeychain/875f77d9f61bd97fd84cca47ce3bc71186dfbd09/COPYING)。
- [libsodium 1.0.22 LICENSE](https://raw.githubusercontent.com/jedisct1/libsodium/1.0.22/LICENSE)。
- [Tree-sitter 0.26.11 LICENSE](https://raw.githubusercontent.com/tree-sitter/tree-sitter/v0.26.11/LICENSE)。

## 界面字体来源

随社区源码发布的 Noto Serif CJK SC 字体及许可已按 `clients/qt/resources/fonts/sources.json` 校验 SHA-256。[Noto CJK 固定提交许可](https://raw.githubusercontent.com/notofonts/noto-cjk/f8d157532fbfaeda587e826d4cd5b21a49186f7c/Serif/LICENSE)
