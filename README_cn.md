
- [English](README.md)

# SIMCom FTP Tool

这是一个 Windows 控制台工具，通过串口（COM 口）向 SIMCom 系列模块发送 AT 指令，利用模块自带的 FTP 功能从远程 FTP 服务器下载文件到本地。它通过 AT 命令序列启动 FTP 服务、登录 FTP、查询文件大小、按偏移分块请求数据（AT+CFTPSGET）并将接收到的二进制数据写入本地文件，同时在控制台打印十六进制的抓包式输出和下载进度。

主要特性：

- 串口读写（使用 Overlapped 异步 I/O）
- 环形缓冲区（线程安全）用于接收数据并供主线程解析
- 单独接收线程持续把串口数据写入环形缓冲区
- 发送 AT 命令并等待特定响应（例如 "OK", "+CFTPSLOGIN: 0" 等）
- 使用 `AT+CFTPSSIZE` 获取远端文件大小
- 使用 `AT+CFTPSGET` 按偏移分块下载并处理 `+CFTPSGET: DATA,<len>` 二进制片段
- 处理 `+CFTPSGET: 14`（针对偏移的重试）和 `+CFTPSGET: 0`（片段完成）等状态
- 在控制台打印十六进制数据视图与下载进度

## 输入 / 输出

### 输入（命令行或交互）

- COM 端口（例如 `COM3`）
- FTP 服务器地址（例如 `117.131.85.140`）
- FTP 端口（例如 `60059`）
- FTP 用户名
- FTP 密码
- 远程文件名（例如 `starline_gen7v2_900-00624.bin`）

### 输出

- 在当前工作目录生成同名文件（以二进制方式写入，若存在则覆盖）
- 控制台会输出 AT 应答、接收到的二进制片段的十六进制视图与下载进度

程序将根据执行结果在控制台打印成功或失败信息；当同一偏移连续收到 `+CFTPSGET: 14` 超过重试限制（默认 5 次）时，程序会中止下载并报错。

## 如何编译

工程已包含 Visual Studio 解决方案（`SIMCom FTP Tool.sln`）和项目文件（`.vcxproj`）。

推荐方式：

1. 在 Windows 上使用 Visual Studio 打开 `SIMCom FTP Tool\SIMCom FTP Tool.sln`。
2. 选择合适的配置（例如 `x64|Debug`），执行 Build → Build Solution。
3. 可执行文件一般位于 `x64\Debug\` 或所选配置的输出目录下。

命令行编译（高级用户）：可在 Developer Command Prompt 下使用 `msbuild` 或 `cl` 编译。

## 如何运行（使用示例）

可通过命令行参数以非交互模式运行。位置参数说明：

```
<程序名> <COM> <FTP_SERVER> <FTP_PORT> <USER> <PASS> <FILENAME>
```

示例（PowerShell）：

```powershell
.\"SIMCom FTP Tool.exe" COM3 117.131.85.140 60059 myuser mypass starline_gen7v2_900-00624.bin
```

如果不提供 6 个命令行参数，程序会进入交互式模式，并按提示依次输入 COM、FTP 地址、端口、用户名、密码和文件名。

## 运行前注意事项

- 确认设备已接好并连接到指定 COM 口，且可以响应基本 AT 命令（例如发送 `AT` 能收到 `OK`）。
- 程序默认串口参数为 115200 波特、8 数据位、无校验、1 停止位（8N1）。
- 程序会尝试列举 `COM1` 至 `COM20` 以供参考。
- 若 FTP 登录或下载失败，可在串口终端（如 PuTTY、Tera Term）手动调试对应 AT 命令以排查问题。

## 常见问题与建议

- 如果遇到频繁的 `+CFTPSGET: 14`，尝试减小每次请求的数据块大小（在源码中将 `packet_size` 从 4096 调小，例如改为 1024 或 512）。
- 如需更好的容错，可考虑增加重试次数或在登录失败时实现重试逻辑。
- 若要减少控制台输出或保存日志，可按需修改源码以支持更灵活的日志级别或输出重定向。

---