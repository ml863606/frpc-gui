# frpc-gui

极轻量 Windows frp 客户端。界面使用 Slint 1.16 + C++，运行 frp 官方 `frpc.exe` 子进程，不引入 Electron、Tauri、WebView、Qt、MFC 或 .NET。

## 功能

- 中文 Win32 主窗口
- Slint 左侧菜单 + 右侧页面布局
- 运行状态页：启动/停止 frp 代理服务
- 代理设置页：多个 TCP 代理卡片展示，点击“新增代理”弹出新增窗口
- frp版本管理页：以卡片文本形式列出版本
- frps设置页：选择 frp 版本、公网 IP、端口、验证方式
- 日志界面：查看连接 frps 时的 stdout/stderr 日志
- 保存 `config.json`
- 生成 `frpc.toml`
- 程序启动时不自动下载 `frpc.exe`
- 在 frp版本管理页同步 GitHub releases，并手动下载当前选择版本
- 下载镜像下拉框默认 `https://gh.zwy.one`
- 启动/停止 `frpc.exe`
- 读取 stdout/stderr 日志，界面最多保留 1000 行
- 最小化到托盘，托盘启动/停止/退出

## 运行数据目录

程序运行时只写入当前 `frpc-gui.exe` 所在目录下的文件和子目录：

```text
<frpc-gui.exe 所在目录>\
```

其中：

- `config.json`：界面配置
- `frpc.toml`：生成给 frpc 使用的配置
- `versions\版本号.json`：每个 frp 版本独立维护的版本配置
- `bin\版本号\frpc.exe`：每个版本独立安装的 frpc
- `downloads\`：下载和临时解压目录

## GitHub 版本同步

点击 `同步GitHub版本` 后，程序会读取：

```text
https://api.github.com/repos/fatedier/frp/releases?per_page=100&page=N
```

然后筛选每个 release 下的 `windows_amd64.zip`，并写成独立版本文件。

## 镜像下载地址

默认镜像站点：

```text
https://gh.zwy.one
```

下载时拼接规则：

```text
<镜像站点>/github.com/fatedier/frp/releases/download/v<版本号>/<压缩包名>
```

例如：

```text
https://gh.zwy.one/github.com/fatedier/frp/releases/download/v0.69.1/frp_0.69.1_windows_amd64.zip
```

默认版本文件：

```text
<frpc-gui.exe 所在目录>\versions\0.69.1.json
```

## 构建

需要 Visual Studio C++ 工具链和 CMake。本机已用 Visual Studio 18 2026 验证通过；如果使用 VS2022，把 generator 改成 `Visual Studio 17 2022`。

项目使用 Slint C++ `1.16.0` 的 Windows MSVC AMD64 预编译包：

```text
third_party\slint\1.16.0\
```

构建后会把 `slint_cpp.dll` 复制到输出目录，运行时需要和 `frpc-gui.exe` 放在同级。

## UI 模块结构

Slint 页面已按页面级别拆分：

```text
ui\app.slint
ui\components\nav_button.slint
ui\components\card.slint
ui\pages\status_page.slint
ui\pages\proxy_page.slint
ui\pages\version_page.slint
ui\pages\frps_page.slint
ui\pages\log_page.slint
```

`app.slint` 只负责窗口状态、左侧导航和页面组装；各页面独立维护。

```powershell
cmake -S . -B build -G "Visual Studio 18 2026" -A x64
cmake --build build --config Release
```

输出：

```text
build\Release\frpc-gui.exe
```

## 手动放置 frpc

如果下载失败，可以手动下载 frp Windows amd64 包，解压后把 `frpc.exe` 放到：

```text
<frpc-gui.exe 所在目录>\bin\0.69.1\frpc.exe
```
