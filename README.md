
- [Chinese](README_cn.md)

# SIMCom FTP Tool

`SIMCom FTP Tool` is a Windows console utility that communicates with SIMCom-series modules over a serial (COM) port using AT commands, and uses the module's built-in FTP capabilities to download files from a remote FTP server to the local machine. The tool issues a sequence of AT commands to start the FTP service, log in, query the remote file size, and request file data in offset-based chunks (`AT+CFTPSGET`). Received binary chunks are written to a local file while a hex dump and progress information are printed to the console.

## Key features

- Serial I/O using overlapped (asynchronous) read/write
- Thread-safe ring buffer for received data
- Separate receiver thread that pushes incoming serial data into the ring buffer
- AT command request/response handling (e.g. `OK`, `+CFTPSLOGIN: 0`)
- `AT+CFTPSSIZE` to obtain the remote file size
- `AT+CFTPSGET` to download file data in offset-based chunks and handle `+CFTPSGET: DATA,<len>` binary frames
- Handles `+CFTPSGET: 14` (retry same offset) and `+CFTPSGET: 0` (chunk complete)
- Prints hex view of received data and download progress to console

## Inputs / Outputs

### Inputs (CLI or interactive)

- COM port (for example, `COM3`)
- FTP server address (for example, `117.131.85.140`)
- FTP server port (for example, `60059`)
- FTP username
- FTP password
- Remote filename (for example, `starline_gen7v2_900-00624.bin`)

### Outputs

- Creates a file with the same name in the current working directory (binary write; overwrites existing files)
- Console output including AT responses, hex view of binary chunks, and download progress

The program prints success/failure messages to the console. If the module repeatedly returns `+CFTPSGET: 14` for the same offset more than the configured retry limit (default 5), the download is aborted and an error is reported.

## Building

The repository includes a Visual Studio solution file (`SIMCom FTP Tool.sln`) and a project file (`.vcxproj`).

Recommended (GUI):

1. Open `SIMCom FTP Tool\\SIMCom FTP Tool.sln` in Visual Studio on Windows.
2. Choose an appropriate configuration (e.g. `x64|Debug`) and select Build → Build Solution.
3. The executable is typically emitted to `x64\\Debug\\` (or the output directory for your chosen configuration).

Advanced (CLI):

Use the Developer Command Prompt and build with `msbuild` or `cl`.

## Usage

The program supports non-interactive (positional argument) and interactive modes. Positional arguments (non-interactive):

```text
<program> <COM> <FTP_SERVER> <FTP_PORT> <USER> <PASS> <FILENAME>
```

Example (PowerShell):

```powershell
.\\"SIMCom FTP Tool.exe" COM3 117.131.85.140 60059 myuser mypass starline_gen7v2_900-00624.bin
```

If fewer than 6 command-line arguments are provided, the program falls back to interactive prompts for COM port, FTP server, port, username, password and filename.

## Pre-run notes

- Make sure the device is connected to the specified COM port and responds to basic AT commands (e.g. sending `AT` should return `OK`).
- Default serial parameters used by the program: 115200 baud, 8 data bits, no parity, 1 stop bit (8N1).
- The program enumerates `COM1` through `COM20` for convenience.
- If FTP login or file download fails, consider using a serial terminal (e.g. PuTTY, Tera Term) to manually test the AT command sequence for troubleshooting.

## Troubleshooting and recommendations

- If you frequently receive `+CFTPSGET: 14`, try reducing the per-request packet size in source (change `packet_size` from 4096 to 1024 or 512).
- For improved resilience, add retries for login and other transient errors.
- To reduce console output or capture logs, add configurable logging levels or redirect console output in the source.

---

If you want, I can:

- Add command-line options (e.g. `--baud`, `--packet-size`, `--help`) and implement a small CLI parser.
- Generate a more detailed README with examples and troubleshooting steps in both English and Chinese.
- Make some runtime constants (e.g. `packet_size`, `RING_BUFFER_SIZE`) configurable via command-line options or a simple config file.
