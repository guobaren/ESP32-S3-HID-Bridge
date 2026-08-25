# 示例配置

演示宏与 Lua 脚本的配置格式，可整体复制到程序旁边的 profiles/ 目录使用：

```powershell
Copy-Item -Recurse profiles.example/示例配置 profiles/
```

复制后重启 HidBridge.Host.exe，在「设置」页选择「示例配置」即可看到宏与 Lua 脚本。

## 包含内容

| 文件 | 说明 |
|---|---|
| profile.json | 配置元数据：宏/Lua 文件关联、触发键、模式与启用状态 |
| macros/*.txt | 宏正文，每个宏一个文本文件 |
| lua/main.txt | Lua 正文，配置启动时自动加载 |

## 自定义

- 宏：在 macros/ 下复制 .txt 文件，并在 profile.json 的 macros 中增加 file 关联、触发键和模式。
- Lua：编辑 lua/main.txt，并保持 profile.json 的 lua_script_file 指向它；激活配置时自动运行。
- Lua 页“检查”会在不改变换行的前提下自动对齐缩进，并规范常见运算符、逗号等行内空格；字符串和注释内容保持原样。
- Lua 输入栏左侧会显示随滚动同步的行号；Host 不会额外生成 `press arg=...` / `release arg=...`，脚本内的 `DebugLog(...)` 仍会输出。
- Lua API 提供 `delay(ms)`、`sleep(ms)` 和 `Sleep(ms)` 三个同底层、可取消的毫秒延时接口；`move(x, y)` 支持带小数的相对移动并按累计结果输出整数 HID 位移。
- Lua 检查和运行错误会在状态或日志中标出脚本错误行号。
- 打开旧版只把 Lua 正文写在 profile.json 的配置时，程序会先读取旧字段，再自动转换为 lua/、macros/ 目录下的文本并保存新版 JSON。
- 键名参考：f1-f24、insert、num0、lctrl、mouse_side1 等，宏触发键支持 ctrl/shift/alt 组合（如 ctrl+f1）。
