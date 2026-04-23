# Quota Dashboard

本工程包含两部分：

- `quota_server.py`
  - 在本机读取 `Antigravity Tools` 本地接口
  - 在本机读取 `Codex Tools` 的 `accounts.json`
  - 输出给 ESP32 更容易解析的文本接口
- `esp32/QuotaDashboard`
  - 微雪 `ESP32-S3-RLCD-4.2` 屏幕程序
  - 动态显示时间、电池、账号额度、额度刷新时间
  - 账号数变化时自动分页

## 本机服务

启动：

```powershell
f:\esp32\.venv\Scripts\python.exe .\quota_server.py
```

接口：

- `http://127.0.0.1:8765/health`
- `http://127.0.0.1:8765/api/summary.json`
- `http://127.0.0.1:8765/api/quota.txt`
- `http://127.0.0.1:8765/api/weather.txt`
- `http://127.0.0.1:8765/api/esp.txt`

服务默认监听 `0.0.0.0:8765`，局域网访问请使用本机 IP。

`/api/esp.txt` 示例：

```text
META|1775655600|2026-04-08 21:40:00|6
ROW|AG|caoyanping4365@gmail.com|Gem 100%|Cld 100%|04-15 20:59
ROW|CX|leilei4365@gmail.com|5H 21%|1W 21%|04-10 18:01
```

## ESP32 配置

修改 [config.h](D:\codex\quota_dashboard\esp32\QuotaDashboard\config.h)：

- `WIFI_SSID`
- `WIFI_PASSWORD`
- `QUOTA_URL`
- `WEATHER_URL`

如果你的电脑局域网 IP 是 `192.168.1.100`，则：

```cpp
static const char *QUOTA_URL = "http://192.168.1.100:8765/api/quota.txt";
static const char *WEATHER_URL = "http://192.168.1.100:8765/api/weather.txt";
```

默认刷新周期为 5 分钟，可在 `config.h` 中修改 `FETCH_INTERVAL_MS`。

## Arduino CLI 编译与烧录

先安装并配置 `arduino-cli`（已安装 ESP32 core）：

```powershell
arduino-cli board list
```

编译：

```powershell
arduino-cli compile --fqbn esp32:esp32:esp32s3 .\esp32\QuotaDashboard
```

烧录（将 `COM7` 替换为你的串口）：

```powershell
arduino-cli upload -p COM7 --fqbn esp32:esp32:esp32s3 .\esp32\QuotaDashboard
```

## Windows 防火墙（仅局域网放行）

以管理员身份运行 PowerShell，按你的网段放行 8765 端口（示例为 `192.168.1.0/24`）：

```powershell
netsh advfirewall firewall add rule name="quota-dashboard" dir=in action=allow protocol=TCP localport=8765 remoteip=192.168.1.0/24 enable=yes
```

验证规则：

```powershell
netsh advfirewall firewall show rule name="quota-dashboard"
```

局域网验证：

```powershell
curl http://<你的电脑IP>:8765/health
```

## 开机自启动（Windows）

仓库已提供“守护进程 + 单一登录自启动”脚本，不依赖任务计划程序。

安装后只保留一条登录自启动入口：

- `HKCU\\Software\\Microsoft\\Windows\\CurrentVersion\\Run` 下的 `QuotaDashboardServer`

启动目标不是直接运行服务脚本，而是运行 `scripts/quota_server_supervisor.ps1`。这样可以避免多个启动项同时拉起同一服务。守护脚本会：

- 保证同一会话只有一个守护实例
- 监控 `quota_server.py` 进程
- 进程退出后 3 秒自动拉起

- 安装并立即启动：

```powershell
powershell -ExecutionPolicy Bypass -File .\scripts\install_autostart.ps1
```

- 移除自启动：

```powershell
powershell -ExecutionPolicy Bypass -File .\scripts\remove_autostart.ps1
```

验证方式：

```powershell
curl http://127.0.0.1:8765/health
```

自动重启验证（可选）：

```powershell
Get-CimInstance Win32_Process | Where-Object { $_.Name -eq 'python.exe' -and $_.CommandLine -match 'quota_server.py' } | Select-Object ProcessId,CommandLine
Stop-Process -Id <上一步进程ID> -Force
Start-Sleep -Seconds 5
curl http://127.0.0.1:8765/health
```

如果你想手动重启服务，可直接运行：

```powershell
f:\esp32\.venv\Scripts\python.exe .\quota_server.py
```

如果你要排查启动问题，建议临时改用上面的前台命令运行，这样能直接看到异常输出。

## Arduino 推荐参数

- Board: `ESP32S3 Dev Module`
- Port: 开发板对应串口
- USB CDC On Boot: `Enabled`
- Flash Mode: `QIO 80MHz`
- Flash Size: `16MB`
- Partition Scheme: `16M Flash (3MB APP/9.9MB FATFS)`
- PSRAM: `OPI PSRAM`
- Upload Mode: `UART0 / Hardware CDC`
- USB Mode: `Hardware CDC and JTAG`
