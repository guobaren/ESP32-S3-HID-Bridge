# 示例配置

演示宏与 Lua 脚本的配置格式，可整体复制到程序旁边的 profiles/ 目录使用：

```powershell
Copy-Item -Recurse profiles.example/示例配置 profiles/
```

复制后重启 HidBridge.Host.exe，在「设置」页选择「示例配置」即可看到宏与 Lua 脚本。

## 包含内容

| 文件 | 说明 |
|---|---|
| profile.json | 配置元数据：三个宏（触发键/模式/启用状态）与一段 Lua 脚本 |
| 连点.txt | once 模式示例：F13 触发，执行一次左键点按 |
| 按住连射.txt | hold_loop 模式示例：按住鼠标侧键期间循环连点 |
| 三段示例.txt | staged 模式示例：ctrl+f14 按下/按住/松开三段脚本 |

## 自定义

- 宏：复制 .txt 文件改名即可新增宏；触发键与模式在 profile.json 的 macros 中配置。
- Lua：编辑 profile.json 的 lua_script_text（单行或标准 JSON 换行转义均可），激活配置时自动运行。
- 键名参考：f1-f24、insert、num0、lctrl、mouse_side1 等，宏触发键支持 ctrl/shift/alt 组合（如 ctrl+f1）。
