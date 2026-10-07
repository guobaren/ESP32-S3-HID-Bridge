# 开发环境与依赖

本文说明各条工作流所需的软件、官方来源、安装位置和用途。只需安装自己要使用的部分。命令中的仓库路径默认相对于项目根目录；Markdown 链接相对于当前文档。ESP-IDF 和工具的缓存目录可移到用户选择的位置，但本项目的激活脚本及原生逻辑测试有明确的项目内路径约定。

## Windows Host 与自动化

| 依赖 | 获取来源 | 安装位置与用途 |
|---|---|---|
| Git for Windows | [官方 Windows 下载页](https://git-scm.com/downloads/win) | 安装在 Windows 任意位置并加入 PATH；用于仓库工作流和克隆带子模块的 ESP-IDF。 |
| .NET 8 SDK | [Microsoft .NET 8 下载页](https://dotnet.microsoft.com/en-us/download/dotnet/8.0) | 安装在开发电脑；用于构建 Host 和运行检查项目。构建时 NuGet 会自动恢复项目引用。 |
| .NET 8 Windows Desktop Runtime x64 | 同一 [Microsoft .NET 8 下载页](https://dotnet.microsoft.com/en-us/download/dotnet/8.0) 的 Desktop Runtime | 安装在运行 Host 的 Windows 电脑。Host 发布为 `self-contained=false` 单文件，仍需要匹配的 .NET Desktop Runtime。 |
| MoonSharp 2.0.0、System.IO.Ports 10.0.0 | [NuGet: MoonSharp](https://www.nuget.org/packages/MoonSharp/2.0.0)、[NuGet: System.IO.Ports](https://www.nuget.org/packages/System.IO.Ports/10.0.0) | 由 `host/HidBridge.Host/HidBridge.Host.csproj` 声明，`dotnet restore` 下载到用户 NuGet 缓存；无需手工复制到仓库。前者用于 Lua，后者用于串口。 |

## ESP-IDF 固件构建

推荐使用 Python 3.12 完整 Windows 安装版，并将 `python.exe` 加入 PATH；从 [Python 官方 Windows 下载页](https://www.python.org/downloads/windows/)获取。不要把 embeddable ZIP 当作常规 Python 安装。ESP-IDF 的 `install.ps1` 会在 IDF 工具目录中创建其构建用 Python 环境；本项目的 `idf-tools/python` 若被 `build-embedded-esptool.ps1` 选中，是另行放置的可选便携 Python，并非 `install.ps1` 自动创建。

双板主线使用 Espressif [ESP-IDF v6.0.2 官方源码标签](https://github.com/espressif/esp-idf/tree/v6.0.2)。若要使用项目自带的激活脚本，请从项目根目录运行以下命令。ESP-IDF 必须用递归克隆取得 Git 子模块；GitHub 的源码 ZIP 不含这些子模块。

```powershell
New-Item -ItemType Directory -Force .\.esp-idf\environment\v6.0.2 | Out-Null
git clone --recursive --branch v6.0.2 https://github.com/espressif/esp-idf.git .\.esp-idf\environment\v6.0.2\esp-idf
$env:IDF_TOOLS_PATH = Join-Path (Get-Location) '.esp-idf\environment\idf-tools'
& .\.esp-idf\environment\v6.0.2\esp-idf\install.ps1 esp32s3
. .\scripts\Enter-EspIdf.ps1
```

`install.ps1` 会下载 ESP32-S3 所需工具链、CMake/Ninja 和 Python 虚拟环境到 `IDF_TOOLS_PATH`。项目的 `scripts/Enter-EspIdf.ps1` 固定查找 `.esp-idf/environment/v6.0.2/esp-idf/export.ps1`，并设置 `.esp-idf/environment/idf-tools`；因此只把 IDF 装在任意外部目录时，这个脚本无法激活它。固件入口和构建命令见 [README 的双板构建说明](../README.md)。

ESP-IDF 官方也提供 [Espressif Installation Manager（EIM）](https://dl.espressif.com/dl/eim/) 和 [Windows 安装指南](https://docs.espressif.com/projects/esp-idf/en/stable/esp32/get-started/windows-setup.html)。EIM 是可选安装途径；安装时仍需选择 ESP-IDF **v6.0.2**，不要随 stable 文档升级到其他版本。在 EIM 生成的 ESP-IDF 激活终端中可以运行本仓库的 `idf.py` 构建；它不会自动创建上述项目固定目录，不能直接替代 `Enter-EspIdf.ps1`。`scripts/Test-DualProxyLogic.ps1` 还要求项目内 `.esp-idf` 头文件和 `firmware/dual_proxy/build/config/sdkconfig.h`，所以该脚本需要按项目固定布局安装并先构建双板工程。

双板依赖由 `firmware/dual_proxy/main/idf_component.yml` 和 `firmware/dual_proxy/dependencies.lock` 管理。当前锁文件对应 ESP-IDF 6.0.2、`esp_tinyusb` 2.3.0、`tinyusb` 0.21.0~2、`led_strip` 3.0.3、`usb` 1.5.0；`usb_host_hid` 解析为仓库本地 `firmware/dual_proxy/components/espressif__usb_host_hid` 1.2.1。运行 `idf.py build` 时组件管理器按清单和锁文件从 [Espressif Component Registry](https://components.espressif.com/) 取得注册组件，放入 `firmware/dual_proxy/managed_components`；`usb_host_hid` 使用仓库已包含的本地副本，不要再手工下载覆盖。旧单板、连接验证和吞吐探针各自有单独的锁文件，不能把它们的组件版本当作双板主线版本。

## 可选的独立刷写工具

Host 可将独立 `esptool.exe` 嵌入单文件程序，使目标电脑不需要 Python 或 ESP-IDF。需要生成该资源时，从项目根目录运行：

```powershell
powershell -ExecutionPolicy Bypass -File .\scripts\build-embedded-esptool.ps1
```

脚本优先使用 `.esp-idf/environment/idf-tools/python/python.exe`（如果用户自行放置），否则使用 PATH 中的 `python`；它在 `.build-tools/esptool-build/venv` 创建隔离环境，通过 pip 从 PyPI 安装当时可用的 [`esptool`](https://pypi.org/project/esptool/) 和 [`pyinstaller`](https://pypi.org/project/pyinstaller/) 包（项目文档：[esptool](https://docs.espressif.com/projects/esptool/en/latest/esp32/)、[PyInstaller](https://pyinstaller.org/en/stable/)），然后生成 `host/HidBridge.Host/EmbeddedAssets/esptool.exe`。无需把 Python 包安装到全局环境，也不应把生成的 EXE 手工放到其他目录。构建 Host 时该文件存在才会嵌入；文件被 `.gitignore` 排除，不随源码自动提供。

若程序未内置该资源，Host 会尝试从项目 ESP-IDF Python 环境中查找 `esptool.exe`；两处都没有时刷写会报缺少 esptool。无论 esptool 来自哪里，刷写都从设置页或 API 指定的 `flasher_args.json` 读取镜像，镜像不嵌入 Host EXE。首次调用内置工具时，Host 将其解压到 `%LOCALAPPDATA%\HidBridge\embedded\esptool\esptool.exe`。

## 可选的串口与截图工具

多数仓库 Python 工具仅使用 Python 标准库。需要运行下列工具时，才安装额外包；下面把虚拟环境放在被忽略的项目工具目录中。激活前，直接使用普通 `python` 不会自动选择此环境，应使用变量 `$python` 调用工具：

```powershell
$venv = Join-Path (Get-Location) '.build-tools\python-tools\venv'
python -m venv $venv
$python = Join-Path $venv 'Scripts\python.exe'
& $python -m pip install pyserial pillow
& $python .\tools\dual_uart_inspect.py --help
```

`pyserial`（[PyPI 项目页](https://pypi.org/project/pyserial/)）用于 `tools/dual_uart_inspect.py`、串口捕获/统计/注入脚本、硬件检查和 USB CDC 吞吐测试。`Pillow`（[PyPI 项目页](https://pypi.org/project/pillow/)）只用于 `tools/capture_window_winapi.py` 的窗口截图。Windows 原生 WinUSB 探针使用系统 WinUSB API，不需要额外的 USB Python 包。

USB 串口适配器缺少驱动时，仓库已经包含 WCH CH340/CH341 驱动包，位置为 `drivers/wch-ch341ser/`；官方来源和手动安装步骤见 [驱动说明](../drivers/wch-ch341ser/README.md) 和 [WCH 官方下载页](https://www.wch-ic.com/downloads/CH341SER_ZIP.html)。Host 的启动诊断会在程序目录旁查找这个驱动目录，并且仅在明确需要时询问用户是否安装；发布包应将该目录放在 EXE 旁。驱动安装可能需要管理员权限，设备的 COM 枚举仍需在目标电脑单独确认。

## 可选的视频探针与原生逻辑测试

以下仅记录本地可选 USB 带宽实验的依赖；`firmware/usb_bandwidth_probe/` 未随双板 v0.1.1 源码发布，不是运行 Host 或双板鼠标所需的工程。该实验的 `benchmark_video_sender.py` 需要一个可执行的 FFmpeg，并通过 `--ffmpeg` 参数明确传入位置。FFmpeg [官方页面](https://ffmpeg.org/download.html)提供源码和指向 Windows 构建提供者的链接；Windows 预编译文件来自页面列出的第三方，不是 FFmpeg 官方构建。可将所选 `ffmpeg.exe` 放在被忽略的项目目录 `.build-tools/ffmpeg/bin/ffmpeg.exe`，并传入 `--ffmpeg ./.build-tools/ffmpeg/bin/ffmpeg.exe`；也可自行选目录并提供相应路径，不需把文件加入源码。CDC 传输还需前述 `pyserial`；WinUSB 传输使用 Windows 自带 API。当前 USB 探针锁文件将 TinyUSB 指向 `firmware/dual_proxy/managed_components` 的本地组件目录，首次单独构建探针前需先按双板工程的锁文件恢复组件。

若要运行 `scripts/Test-DualProxyLogic.ps1`，除项目布局下的 ESP-IDF 环境和一次 `firmware/dual_proxy` 构建外，还需将 Clang C 编译器加入 PATH。可从 [LLVM 官方发布页](https://github.com/llvm/llvm-project/releases)获取 LLVM 工具链。Windows 默认的 MSVC 目标还需要 Microsoft C++ 编译工具链与 Windows SDK；可从 [Visual Studio C++ Build Tools 官方页](https://visualstudio.microsoft.com/visual-cpp-build-tools/)安装 C++ 构建工具，也可复用已有的 Visual Studio C++ 工作负载。使用官方 Clang Windows 包时，请在 Developer PowerShell/命令提示符中从项目根目录运行脚本，以便找到 MSVC 标准库和链接器。脚本以 `clang -Wall -Wextra -Werror` 编译纯逻辑用例；它不是 ESP-IDF 固件构建，也不触碰硬件。
