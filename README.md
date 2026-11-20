# GzydDmTools Community

面向达梦数据库的 Windows x64 工作台与命令行客户端。社区源码保留通用数据库功能，剔除私有业务实现、内部更新服务及原仓库历史。

[操作手册](shared/help/user-guide.md) · [第三方说明](docs/third-party-notices.md) · [许可证范围](LICENSE-SCOPE.md)

## 应用截图

以下五张截图取自 2026-11-13 构建的 1.4.49 社区版 Windows 工作台，在独立 Wine 11.0 环境运行，由 Qt 控件直接截取。连接、SQL 和结果均为离线示例。

### 启动页

![社区版启动页](shared/help/images/startup.png)

### 首页工作台

![首页工作台：窗口导航、SQL 编辑器与代码片段](shared/help/images/home.png)

### 连接管理

![连接管理：直连配置示例，不含密码](shared/help/images/connections.png)

### SQL 编辑与结果

![SQL 编辑与结果：8 行离线示例](shared/help/images/sql-results.png)

### 操作日志

![操作日志：SQL、连接名和时间优先展示](shared/help/images/operation-log.png)

## 可以做什么

- 管理直连和集群连接，区分应用主连接与各工作窗口的独立会话。
- 编辑和执行 SQL，查看结果／计划／诊断，按条件编辑数据、导入和导出。
- 浏览对象、开发规范与包体、编译无效对象，查看本地包体恢复材料。
- 使用 Command 发布脚本、控制并行和遇错策略，查看完整自动日志。
- 使用 Test Window 输入参数、执行程序并在具备依赖和权限时调试。
- 查看会话、锁等待、参数、存储和数据库信息。
- 通过独立 CLI 核对连接、执行明确的文件任务和对象诊断。

编辑器提供执行全文历史、草稿恢复、命名转换和带输入占位符的代码片段。操作手册按真实入口说明查询、写入、编译、提交、恢复及取消之间的区别。

## 源码目录

| 路径 | 内容 |
| --- | --- |
| `clients/qt` | C++17／Qt 工作台、CLI 和社区功能边界检查 |
| `language-server` | 独立 Rust 语言服务与 SQL 语法 |
| `shared/help/user-guide.md` | 社区操作手册，同时作为应用内帮助 |
| `shared/examples` | CLI 单文件／批量任务模板，全部为虚构值 |
| `editions/community.json` | 固定关闭私有业务与内部更新功能 |
| `licenses` | 第三方许可及依赖清单 |

本目录不包含数据库 SDK、驱动 DLL、调试器 EXE、Qt／Wine 预编译运行环境、个人配置、客户数据或原 `.git`。界面字体资源附带固定来源、SHA-256 和许可证；它们不被项目自有代码许可替代。

社区版随源码提供 Noto Serif CJK SC。编辑器可使用系统已安装的 MesloLGS NF，或回退到系统等宽字体。历史截图使用当时构建内置的编辑器字体。

## 在 Windows 构建

### 准备依赖

使用 x64 且彼此匹配的编译器和库：

- CMake 3.25+ 和 Ninja，或其他与本机 Qt 一致的 CMake 生成器。
- Qt 6.4 或更高版本，需要 Concurrent、Network、Sql、Widgets 及相应运行插件。历史截图构建使用 Qt 6.8.3。
- 自行合法取得的达梦 DPI SDK，目录中应包含 `drivers/dpi/include/DPI.h` 及可链接的 `dmdpi` 库。
- libsodium 1.0.22，通过 `Sodium_ROOT` 指定。
- zlib 1.2.13 或更新的兼容版本，提供与编译器匹配的静态库，通过 `ZLIB_ROOT` 指定；用于文件类型探测中的小型 ZIP 成员解压。
- QtKeychain 由 CMake 固定到特定提交，首次配置需要访问上游；离线构建可通过 CMake 的 FetchContent 源目录配置提供同一版本。
- Rust 按 `language-server/Cargo.toml` 声明的工具链要求准备，依赖由 `Cargo.lock` 固定。

Qt、SDK、libsodium 和 zlib 都必须与当前编译器 ABI 匹配。不要直接混用不同工具链生成的库。

### 配置和编译

在已准备好编译器及 Qt 环境的 PowerShell 中，从源码根目录执行；替换示例路径：

```powershell
cmake -S clients/qt -B build/qt -G Ninja `
  -DGZYD_EDITION=community `
  -DCMAKE_BUILD_TYPE=Release `
  -DCMAKE_PREFIX_PATH="C:/Qt/6.8.3/mingw_64" `
  -DDM_CLIENT_DIR="C:/sdk/dm-client" `
  -DSodium_ROOT="C:/sdk/libsodium-1.0.22" `
  -DZLIB_ROOT="C:/sdk/zlib"
cmake --build build/qt --parallel
cargo build --manifest-path language-server/Cargo.toml --release --locked
```

生成的程序包括 `GzydDmTools.exe`、`GzydDmToolsCli.exe` 和独立的 `GzydDmToolsLanguageServer.exe`。把语言服务放在客户端能够找到的位置，通常是工作台 EXE 同目录。

使用与构建相同 Qt 的部署工具准备插件和运行依赖，并补齐 DPI、libsodium、QtKeychain 等实际链接依赖。生成 EXE 不等于得到完整安装包；不要把任意一份个人运行目录直接上传。社区源码不依赖内部服务地址或内部密钥。

### 验证自己的构建

```powershell
ctest --test-dir build/qt --output-on-failure
.\build\qt\GzydDmToolsCli.exe --help
.\build\qt\GzydDmToolsCli.exe --version
```

上述是供维护者执行的命令，不表示本次源码已经通过它们。CMake 注册的社区边界检查应确认私有功能、业务片段和内部更新凭据不可用。自动检查不能代替真实数据库权限、事务、主备、输入法、窗口切换和缩放验收。

## 开始使用

1. 先阅读[运行准备与第一次连接](shared/help/user-guide.md)，准备匹配的运行依赖。
2. 在连接管理中保存测试连接和本机安全凭据。
3. 新建 SQL Window，核对该窗口连接，执行 `SELECT 1 FROM DUAL;`。
4. 再按手册使用对象开发、数据编辑、Command 和 CLI；发送写入、编译、权限或恢复操作前确认对应目标与范围。

CLI 任务示例中的连接 ID、路径和全零摘要必须替换，不能直接发布。社区版不接受私有业务会话初始化。

macOS 采用 Windows 运行目录加 Wine 的路线，本目录没有可直接安装的 Mac 包或原生 macOS 客户端；需要自行准备并验证运行环境。

## 更新与本次验证状态

当前源码版本为 1.4.49，社区更新见 [更新说明](release-notes.json)。截图展示 2026-11-13 的工作台构建。当前源码快照已检查本地头文件、CMake 输入和 Qt 资源引用，构建、全量回归、Windows 实机和真实数据库验收待验证。

自有代码采用 Apache-2.0 许可证，第三方资源沿用各自的许可。详见 [许可证范围](LICENSE-SCOPE.md) 和 [第三方说明](docs/third-party-notices.md)。

## 反馈问题

提供版本、平台、复现步骤、预期和实际结果、脱敏错误及日志。未响应问题尽量保留现场和主线程调用栈；不要上传连接密码、完整用户数据目录或未脱敏转储。
