# ESP32 E-Paper Weather Station

基于 ESP32 + 4.2 寸墨水屏的室内外温湿度与天气显示项目，并通过 MQTT 与 Home Assistant 集成。

核心特性：
- 室内温湿度使用 SHT4x 传感器；
- 支持和风天气实时获取室外天气与图标；
- 支持 MQTT 数据上报（带 Home Assistant 自动发现）；
- 支持 OTA 固件升级；
- 室内温湿度上传到 MQTT 时，使用 30 秒内**按时间加权平均值**，降低抖动。

---

## 一、工程结构概览

工程根目录（`D:\esp32\temp+epaper\te`）中的重要文件：

- [te.ino](file:///d%3A/esp32/temp%2Bepaper/te/te.ino)：主程序，包含传感器读取、天气获取、屏幕绘制、MQTT、OTA 等逻辑。
- `data/conf.json`：运行时配置文件（WiFi、MQTT、和风天气、OTA 密码等），**不会被上传到 Git 仓库**。
- `data/icons_bin/`：天气图标二进制文件。
- `build/`：Arduino/ESP32 编译输出目录（`.gitignore` 已忽略）。
- [ota_upload.ps1](file:///d%3A/esp32/temp%2Bepaper/te/ota_upload.ps1)：在本机通过 IP 对设备进行 OTA 升级的辅助脚本。

---

## 二、conf.json 配置说明

`conf.json` 位于项目的 `data/` 目录下，**不会进入版本控制**，需要在每台设备/环境中自行创建与修改。

示例（仅为结构示意，请用你自己的实际参数替换所有占位符）：

```json
{
  "wifi_ssid": "YOUR_WIFI_SSID",
  "wifi_password": "YOUR_WIFI_PASSWORD",

  "mqtt_host": "YOUR_MQTT_HOST",
  "mqtt_port": 1883,
  "mqtt_user": "YOUR_MQTT_USER",
  "mqtt_pass": "YOUR_MQTT_PASS",
  "mqtt_base": "YOUR/MQTT/BASE_TOPIC",

  "qweather_host": "YOUR_QWEATHER_HOST",
  "qweather_token": "YOUR_QWEATHER_TOKEN",
  "qweather_location": "YOUR_LOCATION_ID_OR_LON_LAT",
  "qweather_lang": "zh",
  "qweather_unit": "m",

  "qweather_forecast_days": "3d",
  "weather_now_refresh_min": 15,
  "weather_forecast_refresh_h": 2,

  "ha_host": "YOUR_HA_HOST",
  "ha_port": 8123,
  "ha_token": "YOUR_LONG_LIVED_ACCESS_TOKEN",
  "ha_lunar_entity": "sensor.your_lunar_sensor",

  "battery_adc_pin": -1,
  "battery_scale": 30.0,

  "ota_password": "YOUR_OTA_PASSWORD"
}
```

字段说明：

- `wifi_ssid` / `wifi_password`  
  - 连接的 2.4GHz WiFi 名称和密码。  
  - 示例：`"wifi_ssid": "MyHomeWiFi"`，`"wifi_password": "MyStrongPassword"`。

- `mqtt_host`  
  - MQTT 服务器地址，可以是 IP（如 `192.168.1.10`）或域名（如 `mqtt.example.com`）。

- `mqtt_port`  
  - MQTT 端口号，一般为 `1883`（未加密）或 `8883`（TLS 加密）。

- `mqtt_user` / `mqtt_pass`  
  - MQTT 登录用户名和密码。  
  - 如果你的 MQTT 不需要账号密码，可以设置为空字符串 `""`。

- `mqtt_base`  
  - 本设备在 MQTT 中的基础主题前缀，建议使用层级结构，如：`"home/esp32_epaper"`。  
  - 程序会在此前缀下发布以下主题：
    - `<mqtt_base>/temperature_in`：室内温度
    - `<mqtt_base>/humidity_in`：室内湿度
    - `<mqtt_base>/temperature_out`：室外温度
    - `<mqtt_base>/humidity_out`：室外湿度
    - `<mqtt_base>/wifi_rssi`：WiFi 信号强度
    - `<mqtt_base>/battery`：电池电量百分比（若启用）

- `qweather_host`  
  - 和风天气 API 的域名，例如官方的 `devapi.qweather.com`，或者控制台分配给你的专用域名。

- `qweather_token`  
  - 和风天气申请到的 API Key/Token，用于身份认证。

- `qweather_location`  
  - 和风天气的地理位置标识，可以是：  
    - 城市 ID（如 `"101010100"`），或  
    - 经纬度字符串 `"经度,纬度"`（如 `"116.99,32.60"`）。

- `qweather_lang`  
  - 返回语言，常用为 `"zh"`（中文）。

- `qweather_unit`  
  - 单位系统，`"m"` 表示公制单位（温度用摄氏度、能见度用公里等）。

- `qweather_forecast_days`  
  - 控制获取的和风天气预报天数。  
  - 例如 `"3d"` 表示获取未来 3 天预报，`"7d"` 表示 7 天预报（需与和风账户权限匹配）。

- `weather_now_refresh_min`  
  - 当前天气（实况）刷新间隔，单位为分钟。  
  - 示例：`15` 表示每 15 分钟更新一次当前天气数据。

- `weather_forecast_refresh_h`  
  - 天气预报刷新间隔，单位为小时。  
  - 示例：`2` 表示每 2 小时更新一次未来预报。

- `ha_host`  
  - Home Assistant 的访问地址，可以是 IP 或域名，例如 `192.168.1.20` 或 `ha.local`。

- `ha_port`  
  - Home Assistant 的 HTTP 端口，默认通常为 `8123`。

- `ha_token`  
  - Home Assistant 的长期访问令牌（Long-Lived Access Token），用于通过 REST API 读取实体数据。  
  - 在 HA 的用户配置中创建，具有较高权限，请妥善保管。

- `ha_lunar_entity`  
  - Home Assistant 中用于提供农历信息的实体 ID，例如：`"sensor.chinese_lunar"`。  
  - 程序会从这个实体中拉取农历日期等信息，用于在墨水屏上显示。

- `battery_adc_pin`  
  - 用于测量电池电压的 ADC 引脚编号；  
  - 如果不使用电池监测，设置为 `-1`，程序会跳过电池上报。

- `battery_scale`  
  - 将 ADC 电压换算为电量百分比时使用的比例系数。  
  - 示例：根据你的分压电路和电池特性调节到合适的值（如 `30.0`），具体可通过校准确定。

- `ota_password`  
  - OTA 升级密码，上传固件时需要填写相同的密码。  
  - 与代码中 `ArduinoOTA.setPassword` 对应，用于防止设备被未授权升级。

> 注意：`conf.json` 中包含敏感信息（WiFi/MQTT/Token），**不要提交到 GitHub**。本仓库的 `.gitignore` 已经包含 `data/conf.json`，正常使用 `git add .` 时不会意外提交。

---

## 三、上传 data 目录（LittleFS 文件系统）

本项目在代码中明确使用的是 **LittleFS**：
- 通过 `#include <LittleFS.h>` 和 `LittleFS.begin(true)` 挂载文件系统；
- 通过 `LittleFS.open("/conf.json", "r")` 读取配置；
- 通过 `LittleFS.open("/icons_bin/xxx.bin", "r")` 读取图标。

因此，**必须把 `data/` 目录上传到 LittleFS 分区**，否则程序在运行时找不到配置和图标文件。

需要上传的内容包括：
- `data/conf.json`：配置文件（WiFi、MQTT、和风、OTA 等）；
- `data/icons_bin/`：天气图标二进制文件。

它们不会自动随固件一起上传，需要**单独执行一次“文件系统上传”**。一般在以下情况下需要重新上传：
- 第一次烧录这块板子；
- 修改了 `conf.json`；
- 替换/更新了 `icons_bin` 中的图标。

### 3.1 在 Arduino IDE 2.x 中通过 arduino-littlefs-upload.vsix 上传（推荐）

本项目推荐的方式是：使用 **Arduino IDE 2.x**，配合 `arduino-littlefs-upload.vsix` 插件来上传 LittleFS 文件系统。

1. 安装 Arduino IDE 2.x，并安装好 ESP32 开发板支持包。  
2. 将 `arduino-littlefs-upload.vsix` 放到 Arduino IDE 2 的 `plugins` 目录下（或通过 Arduino IDE 的扩展管理功能从 VSIX 安装该插件），然后重启 Arduino IDE，确保插件被正确加载。  
3. 在本仓库根目录下准备好 `data/` 目录，内容包括：  
   - 已配置好的 `data/conf.json`；  
   - `data/icons_bin/` 及其中的图标文件。  
4. 在 Arduino IDE 中打开本工程，连接好 ESP32（串口已可用），开发板选择为 `ESP32 Dev Module`（或与你实际板子对应的型号）。  
5. 在 Arduino IDE 中按下 `Ctrl+Shift+P` 打开命令面板，在搜索框中输入 `LittleFS`，选择插件提供的 LittleFS 上传命令（例如 `LittleFS: Upload` 等，具体名称以 arduino-littlefs-upload.vsix 实际显示为准）。  
6. 按提示选择串口/开发板后，等待上传过程完成，此时 `data/` 目录中的内容会被打包并写入 ESP32 的 LittleFS 分区。

> 提示：以后如果只改了代码（.ino），可以仅上传固件而不必每次都重传 LittleFS；  
> 如果修改了 `conf.json` 或 `icons_bin`，则需要重新执行一次上述“文件系统上传”步骤。

### 3.2 使用 PlatformIO（可选）

如果你在使用 PlatformIO 开发，同样可以把 `data/` 目录打包上传到 LittleFS 分区，但需要确保文件系统类型配置为 LittleFS，例如在 `platformio.ini` 中设置：

```ini
board_build.filesystem = littlefs
```

之后即可：
- 在工程根目录执行：`pio run -t uploadfs`；  
- 或在 PlatformIO 的界面中点击 “Upload File System Image”。  

具体细节以你的 PlatformIO 配置为准，核心要求是：**文件系统必须是 LittleFS**。

---

## 四、编译与上传（串口方式）

1. 打开 Arduino IDE。
2. 选择开发板：
   - `ESP32 Dev Module`（或与你实际板子对应的型号）。
3. 安装依赖库：
   - `PubSubClient`
   - `ArduinoJson`
   - `ArduinoOTA`
   - `LittleFS` 支持等（通常随 ESP32 Core 一起安装）。
4. 保证 `data/conf.json` 已按上文配置好，并且已经通过上一节的 LittleFS 上传到了设备。
5. 使用 USB 连接 ESP32，选择对应串口。
6. 点击“上传”进行串口烧录。

首次烧录建议使用串口方式，确认一切正常后，再使用 OTA 升级。

---

## 五、导出固件并通过 OTA 升级

项目已经内置 OTA 功能，只要设备连上 WiFi 并读取到 `ota_password`，即可通过网络升级固件。

### 5.1 导出编译后的固件（.bin）

在 Arduino IDE 中：

1. 菜单选择：`草图(Sketch)` → `导出已编译的二进制文件(Export compiled Binary)`。
2. 编译完成后，会在项目目录的 `build/esp32.esp32.esp32/` 中生成：
   - `te.ino.bin`（主固件文件）

你也可以在 VSCode/其它 IDE 中找到同样的 `te.ino.bin` 文件，这个就是 OTA 上传用的固件。

### 5.2 在局域网内直接使用 espota.py 上传

如果你有一台和 ESP32 **在同一局域网** 的 Linux/群晖/树莓派/PC，可以直接使用 `espota.py`：

1. 将以下两个文件放到同一目录：
   - `te.ino.bin`：编译好的固件；
   - `espota.py`：ESP32 Core 自带的 OTA 工具脚本（在本机 `Arduino15/packages/esp32/.../tools/espota.py` 中，可以复制出来）。

2. 在该目录执行：

```bash
python espota.py -i <ESP32_IP> -f te.ino.bin --auth=<ota_password>
```

例如：

```bash
python espota.py -i 192.168.2.105 -f te.ino.bin --auth=123456
```

过程输出中如果出现：
- `Sending invitation to ...`
- `Authenticating...OK`
- 一串 `Uploading..............`

即表示升级过程已经正常进行，上传完成后 ESP32 会自动重启。

> 提示：某些网络环境下，即使没有明确打印 `Success` 字样，只要上传过程完成且设备重启，固件一般已经升级成功，可以通过 MQTT 行为或屏幕显示变化进行验证。

---

## 六、通过 PowerShell 脚本 ota_upload.ps1 升级（在本机侧）

项目中还提供了一个辅助脚本 [ota_upload.ps1](file:///d%3A/esp32/temp%2Bepaper/te/ota_upload.ps1)，用于在 Windows / PowerShell 环境下快速进行 OTA 升级。

使用方式：

1. 确保已经导出 `te.ino.bin`（见上文 5.1）。
2. 在 PowerShell 中进入项目目录：

```powershell
cd "D:\esp32\temp+epaper\te"
```

3. 以绕过策略的方式运行脚本：

```powershell
powershell -ExecutionPolicy Bypass -File .\ota_upload.ps1
```

4. 按提示输入 ESP32 的 IP 地址（如 `192.168.2.105`）。
5. 脚本会自动：
   - 搜索最新的 `te.ino.bin`；
   - 读取 `data/conf.json` 中的 `ota_password`；
   - 调用系统中的 `python` 和 `espota.py`，向 ESP32 推送固件。

该脚本主要适合在 **本机与 ESP32 在同一网段** 且无复杂 VPN / 路由环境时使用。

---

## 七、MQTT 数据与 Home Assistant 集成说明

程序在运行时，会：

- 每 30 秒向 MQTT 服务器发布一次室内温湿度数据；
  - 使用 30 秒内传感器所有采样点的**按时间加权平均值**作为上报数据；
  - 有效降低传感器自身抖动带来的曲线噪声；
- 定期刷新室外天气温湿度、天气描述与图标；
- 在启动后通过 Home Assistant Discovery 自动注册多个 `sensor` 实体。

主题前缀由 `mqtt_base` 决定，例如 `home/esp32_epaper`，你可以在 Home Assistant 中看到对应实体自动出现。
