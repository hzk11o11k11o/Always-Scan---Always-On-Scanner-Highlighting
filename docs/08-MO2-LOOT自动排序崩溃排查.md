# 08 · MO2 自带 LOOT「自动排序」崩溃排查（0xc0000409）

> 排查日期：2026-09-17
> 环境：MO2 **2.5.2**（`D:\Mod Organizer 2`）、Starfield、`libloot`/`loot.dll` **0.23.0**

## 一、现象

点击 MO2 的「排序」按钮后失败，日志窗口：

```
[info] Attempting to create a game handle for game type "Starfield" with game path
       "D:/SteamLibrary/steamapps/common/Starfield" and game local path
       "D:/Mod Organizer 2/starfield_mods/profiles/Default"
[info] Downloading latest masterlist file from
       https://raw.githubusercontent.com/loot/starfield/v0.21/masterlist.yaml
       to C:\Users\huangzhe\AppData\Local\LOOT\games\Starfield\masterlist.yaml
[error] Loot failed. Exit code was: 0xc0000409
```

`0xc0000409` = `STATUS_STACK_BUFFER_OVERRUN`（`-1073740791`）。
崩溃后 `%LOCALAPPDATA%\LOOT\games\Starfield\` 目录**是空的**，
masterlist.yaml 从未落盘 —— 说明下载这一步就挂了。

## 二、根因

**MO2 2.5.x 的「排序」= 启动子进程 `<MO2>\loot\lootcli.exe`**（不是链接 `loot.dll`，
所以崩溃只会让子进程退出，MO2 能拿到「退出码」并报 `Loot failed. Exit code was:`）。

这个 `lootcli.exe` **用 WinHTTP 下载 masterlist**（导入表里有
`WinHttpOpen` / `WinHttpConnect` / `WinHttpCrackUrl` / `WinHttpOpenRequest` /
`WinHttpSendRequest` / `WinHttpReceiveResponse` / `WinHttpReadData`），
而 **WinHTTP 不读取 Windows「系统代理」(WinINET) 设置**。

本机实际情况：

| 通道 | 结果 |
| --- | --- |
| WinINET / IE 系统代理（`HKCU\...\Internet Settings`） | `ProxyEnable=1`，`127.0.0.1:7890`（Clash） |
| `Invoke-WebRequest`（走系统代理） | `https://raw.githubusercontent.com/...` → **200**，23 274 B |
| `netsh winhttp show proxy` | **直接访问（没有代理服务器）** |
| WinHTTP 组件直连同一 URL | **无法解析服务器的名称或地址**（DNS 被污染/直连不通） |

⇒ `lootcli.exe` 走 WinHTTP 直连 → DNS 失败 → **下载失败**；
而它在「下载失败」这条路径上直接崩了（连 `Error downloading masterlist: `
这条本应输出的日志都没来得及打），于是表现为 `0xc0000409`。

## 三、修复

把系统代理同步给 WinHTTP（**必须管理员**）：

```cmd
netsh winhttp import proxy source=ie
```

或显式指定：

```cmd
netsh winhttp set proxy proxy-server="127.0.0.1:7890" ^
      bypass-list="localhost;127.*;192.168.*;10.*;<local>"
```

验证：

```cmd
netsh winhttp show proxy
```

撤销（例如代理软件常关）：

```cmd
netsh winhttp reset proxy
```

**替代方案**：让代理软件开 **TUN / 虚拟网卡模式**（Clash Verge 的「Tun 模式」等），
此时 WinHTTP 的直连流量也会被接管，不依赖 `netsh winhttp`。

## 四、验证方法（不启动 MO2 GUI）

`tools/loot_sort_check.py` 用与 MO2 完全相同的参数直接调用 `lootcli.exe`：

```cmd
python tools\loot_sort_check.py
```

修复前：`退出码: -1073740791 (0xC0000409)`，无输出文件。
修复后：`退出码: 0 (0x00000000)`，排序结果 JSON 写到 `%TEMP%\loot_sorted.json`。

实测到的 `lootcli.exe` 必需参数（CLI11，选项名是**驼峰**，缺一个就报
`Error: argument missing <name>`，按 `game → gamePath → pluginListPath → out` 顺序要求）：

```
lootcli.exe --game Starfield
            --gamePath      <游戏根目录>
            --pluginListPath <profile\plugins.txt>
            --gameLocalPath  <profile 目录>
            --lootDataPath   %LOCALAPPDATA%\LOOT
            --out            <结果 JSON 路径>
```

按此模式即可在命令行复现 / 回归 MO2 的排序。

## 五、踩过的坑（都不管用）

| 尝试 | 结果 |
| --- | --- |
| 手动把 `masterlist.yaml` 放到 `%LOCALAPPDATA%\LOOT\games\Starfield\` | **无效**。`lootcli` 每次排序都会强制重新下载，仍然崩 |
| `--masterlistSource <本地 URL>` | 无效，该选项名不存在（参数被静默忽略，仍走默认 GitHub 地址） |
| `--skipUpdateMasterlist` | 同上，MO2 也不会传这个开关 |

结论：**没法靠「离线预置 masterlist」绕过**，必须让下载本身成功（即修代理）。

## 六、附带发现

`lootcli` 的排序结果里带有 LOOT 的提示讯息，例如
「Starfield does not support the usage of **.esp** plugins ingame, but your load order
contains such plugins」——说明当前 `plugins.txt` 里存在 `.esp`，Starfield 本体不会加载，
值得后续单独清理。
