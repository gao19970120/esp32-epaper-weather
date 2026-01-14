/* 包含文件 ------------------------------------------------------------------*/
#include "DEV_Config.h"
#include "EPD.h"
#include "GUI_Paint.h"
#include "imagedata.h"
#include "font_new_cn.h"
#include <time.h>
#include <stdlib.h>
#include <stdio.h>
#include <Wire.h>
#include <math.h>
#include <string.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <ArduinoOTA.h>
#include <LittleFS.h>
#include <FS.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <miniz.h>

// 解压 Gzip 数据的辅助函数
bool gzip_decode(uint8_t* input, size_t inputLen, String &output) {
    // 在堆上分配解压器以避免栈溢出（结构体较大 ~10KB）
    tinfl_decompressor* inflator = (tinfl_decompressor*)malloc(sizeof(tinfl_decompressor));
    if (!inflator) {
        printf("Failed to allocate inflator\r\n");
        return false;
    }
    tinfl_init(inflator);

    // 输出缓冲区
    const size_t MAX_OUT = 32768; // 增加到 32KB 以确保安全
    uint8_t* outBuf = (uint8_t*)malloc(MAX_OUT);
    if (!outBuf) {
        printf("miniz malloc failed\r\n");
        free(inflator);
        return false;
    }

    size_t in_bytes = inputLen;
    size_t out_bytes = MAX_OUT;
    int flags = TINFL_FLAG_PARSE_ZLIB_HEADER | TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF; // 尝试解析 zlib 头

    // 如果是标准 gzip，手动跳过 gzip 头（10字节）
    // Gzip 头信息：ID1(1f) ID2(8b) CM(08) FLG MTIME(4) XFL OS
    size_t offset = 0;
    if (inputLen > 10 && input[0] == 0x1F && input[1] == 0x8B) {
        offset = 10; 
        flags = TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF; // 头之后的原始 deflate 流
    }

    int status = tinfl_decompress(inflator, (const uint8_t*)(input + offset), &in_bytes, outBuf, (uint8_t*)outBuf, &out_bytes, flags);

    if (status != TINFL_STATUS_FAILED) {
        output = String((char*)outBuf, out_bytes);
        free(outBuf);
        free(inflator);
        return true;
    }
    
    printf("miniz inflate error: %d\r\n", status);
    free(outBuf);
    free(inflator);
    return false;
}

// I2C 引脚
// 使用标准 ESP32 I2C 引脚
#define PIN_I2C_SDA 21
#define PIN_I2C_SCL 22

// 传感器地址（SHT4x 默认为 0x44）
#define SHT4X_I2C_ADDR 0x44

UBYTE *BlackImage = NULL;
float last_indoor_temp = -999.0;
int last_indoor_humi = -1;
// MQTT 的实时值（变化时立即更新）
float realtime_indoor_temp = -999.0;
int realtime_indoor_humi = -1;
unsigned long lastSensorSampleMs = 0;
float lastSensorTemp = 0;
int lastSensorHumi = 0;
double indoorTempWeightedSumMs = 0;
double indoorHumiWeightedSumMs = 0;
double indoorWeightTotalMs = 0;
float last_outdoor_temp = -999.0;
int last_outdoor_humi = -1;
String last_weather_text = "晴";
String last_weather_icon = "100";

// 配置结构体
struct AppConfig {
    String wifi_ssid;
    String wifi_password;

    String mqtt_host;
    int    mqtt_port = 1883;
    String mqtt_user;
    String mqtt_pass;
    String mqtt_base = "home/esp32_epaper";
    String ota_password;

    String qweather_host;
    String qweather_token;
    String qweather_location; // 例如："101010100" 或 "116.41,39.92"
    String qweather_lang = "zh";
    String qweather_unit = "m";

    int battery_adc_pin = -1;   // 可选 ADC 引脚
    float battery_scale = 1.0f; // 缩放到百分比
};

AppConfig CFG;
WiFiClient espClient;
PubSubClient mqttClient(espClient);
unsigned long lastMqttPublishMs = 0;
unsigned long lastWeatherFetchMs = 0;
bool mqttDiscoveryPublished = false;

// 如果读取成功返回 true
bool readSensor(float *temp, int *humi) {
    // 开始测量（高精度）
    Wire.beginTransmission(SHT4X_I2C_ADDR);
    Wire.write(0xFD);
    if (Wire.endTransmission() != 0) {
        printf("Sensor I2C Error or Not Found\r\n");
        return false;
    }
    
    delay(20); // 等待测量（SHT4x 高精度最大 8.2ms，安全余量）

    Wire.requestFrom(SHT4X_I2C_ADDR, 6);
    if (Wire.available() < 6) {
        printf("Sensor Data Error\r\n");
        return false;
    }

    uint8_t rx_bytes[6];
    for (int i = 0; i < 6; i++) {
        rx_bytes[i] = Wire.read();
    }

    // 转换数据
    uint16_t t_ticks = (rx_bytes[0] << 8) | rx_bytes[1];
    uint16_t rh_ticks = (rx_bytes[3] << 8) | rx_bytes[4];

    // 演示中略过校验和验证以简化代码
    
    *temp = -45 + 175 * ((float)t_ticks / 65535.0);
    float rh = -6 + 125 * ((float)rh_ticks / 65535.0);
    if (rh > 100) rh = 100;
    if (rh < 0) rh = 0;
    *humi = (int)(rh + 0.5); // 四舍五入到最近的整数
    
    // printf("Read Sensor: %.2f C, %d %%\r\n", *temp, *humi);
    return true;
}

// 用于简单键（如 "temp": "24"）的最小 JSON 值提取器
bool jsonGetNumber(const String& json, const String& key, float* out) {
    int k = json.indexOf("\"" + key + "\"");
    if (k < 0) return false;
    int colon = json.indexOf(':', k);
    if (colon < 0) return false;
    int start = colon + 1;
    // 跳过空格和引号
    while (start < (int)json.length() && (json[start] == ' ' || json[start] == '\"')) start++;
    int end = start;
    while (end < (int)json.length() && (isdigit(json[end]) || json[end]=='.' || json[end]=='-')) end++;
    if (end <= start) return false;
    String numStr = json.substring(start, end);
    *out = numStr.toFloat();
    return true;
}

bool jsonGetString(const String& json, const String& key, String* out) {
    int k = json.indexOf("\"" + key + "\"");
    if (k < 0) return false;
    int colon = json.indexOf(':', k);
    if (colon < 0) return false;
    int quote1 = json.indexOf('\"', colon + 1);
    if (quote1 < 0) return false;
    int quote2 = json.indexOf('\"', quote1 + 1);
    if (quote2 < 0) return false;
    *out = json.substring(quote1 + 1, quote2);
    return true;
}

// 从 LittleFS 读取 conf.json
void loadConfig() {
    if (!LittleFS.begin(true)) {
        printf("LittleFS init failed, using defaults\r\n");
        return;
    }
    File f = LittleFS.open("/conf.json", "r");
    if (!f) {
        printf("conf.json not found, using defaults\r\n");
        return;
    }
    String json = f.readString();
    f.close();

    // 通过扫描键进行简单解析
    String s;
    float n;
    if (jsonGetString(json, "wifi_ssid", &s)) CFG.wifi_ssid = s;
    if (jsonGetString(json, "wifi_password", &s)) CFG.wifi_password = s;
    if (jsonGetString(json, "mqtt_host", &s)) CFG.mqtt_host = s;
    if (jsonGetNumber(json, "mqtt_port", &n)) CFG.mqtt_port = (int)n;
    if (jsonGetString(json, "mqtt_user", &s)) CFG.mqtt_user = s;
    if (jsonGetString(json, "mqtt_pass", &s)) CFG.mqtt_pass = s;
    if (jsonGetString(json, "mqtt_base", &s)) CFG.mqtt_base = s;
    if (jsonGetString(json, "ota_password", &s)) CFG.ota_password = s;

    if (jsonGetString(json, "qweather_host", &s)) CFG.qweather_host = s;
    if (jsonGetString(json, "qweather_token", &s)) CFG.qweather_token = s;
    if (jsonGetString(json, "qweather_location", &s)) CFG.qweather_location = s;
    if (jsonGetString(json, "qweather_lang", &s)) CFG.qweather_lang = s;
    if (jsonGetString(json, "qweather_unit", &s)) CFG.qweather_unit = s;

    if (jsonGetNumber(json, "battery_adc_pin", &n)) CFG.battery_adc_pin = (int)n;
    if (jsonGetNumber(json, "battery_scale", &n)) CFG.battery_scale = n;

    printf("Config loaded\r\n");
}

void connectWiFi() {
    if (CFG.wifi_ssid.length() == 0) {
        printf("WiFi SSID missing in conf.json\r\n");
        return;
    }
    printf("Connecting WiFi %s...\r\n", CFG.wifi_ssid.c_str());
    WiFi.mode(WIFI_STA);
    WiFi.begin(CFG.wifi_ssid.c_str(), CFG.wifi_password.c_str());
    int tries = 0;
    while (WiFi.status() != WL_CONNECTED && tries < 30) {
        delay(500);
        printf(".");
        tries++;
    }
    printf("\r\n");
    if (WiFi.status() == WL_CONNECTED) {
        printf("WiFi connected, IP: %s\r\n", WiFi.localIP().toString().c_str());
    } else {
        printf("WiFi connect failed\r\n");
    }
}

void delayWithOTA(unsigned long ms) {
    unsigned long start = millis();
    while (millis() - start < ms) {
        ArduinoOTA.handle();
        delay(10);
    }
}

void mqttSetup() {
    if (CFG.mqtt_host.length() == 0) {
        printf("MQTT host missing, skip MQTT\r\n");
        return;
    }
    mqttClient.setServer(CFG.mqtt_host.c_str(), CFG.mqtt_port);
}

void mqttEnsureConnected() {
    if (CFG.mqtt_host.length() == 0) return;
    while (!mqttClient.connected()) {
        String clientId = "esp32_epaper_" + String((uint32_t)ESP.getEfuseMac(), HEX);
        printf("MQTT connecting...\r\n");
        bool ok;
        if (CFG.mqtt_user.length() > 0) {
            ok = mqttClient.connect(clientId.c_str(), CFG.mqtt_user.c_str(), CFG.mqtt_pass.c_str());
        } else {
            ok = mqttClient.connect(clientId.c_str());
        }
        if (ok) {
            printf("MQTT connected\r\n");
        } else {
            printf("MQTT connect failed, rc=%d; retry in 2s\r\n", mqttClient.state());
            delay(2000);
        }
    }
}

String device_class_to_topic_suffix(const char* unique_id) {
    if (strcmp(unique_id, "epaper_temperature_in") == 0) return "temperature_in";
    if (strcmp(unique_id, "epaper_humidity_in") == 0) return "humidity_in";
    if (strcmp(unique_id, "epaper_temperature_out") == 0) return "temperature_out";
    if (strcmp(unique_id, "epaper_humidity_out") == 0) return "humidity_out";
    if (strcmp(unique_id, "epaper_wifi_rssi") == 0) return "wifi_rssi";
    if (strcmp(unique_id, "epaper_battery_percent") == 0) return "battery";
    return "";
}

void mqttPublishDiscovery(const char* unique_id, const char* name, const char* device_class, const char* unit_of_measurement) {
    if (!mqttClient.connected()) return;

    // 使用标准 Home Assistant 发现前缀 "homeassistant"
    String topic = String("homeassistant") + "/sensor/" + unique_id + "/config";

    JsonDocument doc;
    doc["name"] = name;
    doc["uniq_id"] = unique_id;
    doc["stat_t"] = String(CFG.mqtt_base) + "/" + device_class_to_topic_suffix(unique_id);
    if (unit_of_measurement) {
        doc["unit_of_meas"] = unit_of_measurement;
    }
    if (device_class) {
        doc["dev_cla"] = device_class;
    }
    
    // 添加设备信息
    JsonObject device = doc["dev"].to<JsonObject>();
    device["ids"] = "esp32_epaper_v1";
    device["name"] = "ESP32 E-Paper Display";
    device["mf"] = "Espressif";
    device["mdl"] = "ESP32";
    device["sw"] = "1.0";

    String payload;
    serializeJson(doc, payload);

    // 发布时设置 retain 标志为 true，以便 HA 重启后能获取到
    mqttClient.publish(topic.c_str(), payload.c_str(), true);
}

float readBatteryPercent() {
    if (CFG.battery_adc_pin < 0) return -1.0f;
    int raw = analogRead(CFG.battery_adc_pin);
    float voltage = (float)raw / 4095.0f * 3.3f; // 简单计算
    float percent = voltage * CFG.battery_scale; // 用户定义的比例
    if (percent > 100) percent = 100;
    if (percent < 0) percent = 0;
    return percent;
}

void publishMetrics(float indoorTemp, int indoorHumi, float outdoorTemp, int outdoorHumi) {
    if (CFG.mqtt_host.length() == 0) return;
    mqttEnsureConnected();

    char buf[64];
    String base = CFG.mqtt_base;

    dtostrf(indoorTemp, 0, 2, buf);
    mqttClient.publish((base + "/temperature_in").c_str(), buf, true);

    snprintf(buf, sizeof(buf), "%d", indoorHumi);
    mqttClient.publish((base + "/humidity_in").c_str(), buf, true);

    dtostrf(outdoorTemp, 0, 2, buf);
    mqttClient.publish((base + "/temperature_out").c_str(), buf, true);

    snprintf(buf, sizeof(buf), "%d", outdoorHumi);
    mqttClient.publish((base + "/humidity_out").c_str(), buf, true);

    // WiFi 信号强度
    snprintf(buf, sizeof(buf), "%d", WiFi.RSSI());
    mqttClient.publish((base + "/wifi_rssi").c_str(), buf, true);

    // 电池
    float batt = readBatteryPercent();
    if (batt >= 0) {
        dtostrf(batt, 0, 1, buf);
        mqttClient.publish((base + "/battery").c_str(), buf, true);
    }
}

bool fetchOutdoorWeather(float* tempOut, int* humiOut, String* textOut) {
    if (CFG.qweather_host.length() == 0 || CFG.qweather_token.length() == 0 || CFG.qweather_location.length() == 0) {
        return false;
    }
    if (WiFi.status() != WL_CONNECTED) return false;

    String url = String("https://") + CFG.qweather_host + "/v7/weather/now?location=" + CFG.qweather_location + "&lang=" + CFG.qweather_lang + "&unit=" + CFG.qweather_unit;
    HTTPClient http;
    
    // 在 if 块外部声明 client 以在 http.GET() 期间保持存活
    WiFiClientSecure client;
    client.setInsecure();

    if (url.startsWith("https://")) {
        http.begin(client, url);
    } else {
        http.begin(url);
    }
    // http.addHeader("Authorization", String("Bearer ") + CFG.qweather_token);
    http.addHeader("X-QW-Api-Key", CFG.qweather_token);
    // 显式接受 gzip
    http.addHeader("Accept-Encoding", "gzip");
    
    int code = http.GET();
    String payload = "";
    
    if (code == 200) {
        // 读取原始流
        int size = http.getSize();
        WiFiClient *stream = http.getStreamPtr();
        
        if (size > 0) {
            // 读取到缓冲区
            uint8_t* buff = (uint8_t*)malloc(size);
            if (buff) {
                int len = stream->readBytes(buff, size);
                
                // 检查 Gzip 魔数头：0x1F 0x8B
                if (len > 2 && buff[0] == 0x1F && buff[1] == 0x8B) {
                    printf("Detected Gzip content, decompressing...\r\n");
                    if (!gzip_decode(buff, len, payload)) {
                        printf("Gzip decode failed\r\n");
                    }
                } else {
                    // 不是 gzip，当作字符串处理
                    payload = String((char*)buff, len);
                }
                free(buff);
            } else {
                printf("Malloc failed for HTTP body\r\n");
            }
        }
    } else {
        payload = http.getString(); // Get error message if any
    }
    
    // 调试：打印 payload 的前100个字符以检查是否为明文
    printf("HTTP %d, Payload Preview: %.100s\r\n", code, payload.c_str());

    if (code != 200) {
        http.end();
        printf("QWeather HTTP error: %d\r\n", code);
        printf("Response Payload: %s\r\n", payload.c_str());
        return false;
    }
    http.end();

    // 解析 now.temp, now.humidity, now.text
    float t = 0;
    float h = 0;
    String txt;
    String iconCode;
    bool okT = false, okH = false, okX = false, okI = false;

    // 找到 "now": { ... }，然后查找键
    int nowIdx = payload.indexOf("\"now\"");
    if (nowIdx >= 0) {
        int brace = payload.indexOf('{', nowIdx);
        int endBrace = payload.indexOf('}', brace);
        String nowObj = payload.substring(brace, endBrace + 1);
        okT = jsonGetNumber(nowObj, "temp", &t);
        okH = jsonGetNumber(nowObj, "humidity", &h);
        okX = jsonGetString(nowObj, "text", &txt);
        okI = jsonGetString(nowObj, "icon", &iconCode);
    }
    if (okT) *tempOut = t;
    if (okH) *humiOut = (int)(h + 0.5f);
    if (okX) *textOut = txt;
    if (okI) {
        last_weather_icon = iconCode;
        printf("Weather Icon Code: %s\r\n", iconCode.c_str());
    }
    return okT || okH || okX || okI;
}

bool drawIconFromFS(const String& code, int x, int y, int w, int h) {
    if (!LittleFS.begin(true)) return false;
    String path = "/icons_bin/" + code + ".bin";
    printf("Loading icon from: %s\r\n", path.c_str());
    File f = LittleFS.open(path, "r");
    if (!f) {
        printf("Icon file not found: %s\r\n", path.c_str());
        return false;
    }
    size_t wbyte = (w % 8) ? (w / 8) + 1 : (w / 8);
    size_t need = wbyte * h;
    uint8_t* buf = (uint8_t*)malloc(need);
    if (!buf) {
        f.close();
        return false;
    }
    size_t readn = f.read(buf, need);
    f.close();
    if (readn != need) {
        free(buf);
        return false;
    }
    // 图标 bin 格式：1=白，0=黑
    Paint_DrawImage(buf, x, y, w, h);
    free(buf);
    return true;
}

void updateDisplay(float indoor_temp, int indoor_humi) {
    if (BlackImage == NULL) return;

    printf("Updating Display: In=%.2fC, %d%%\r\n", indoor_temp, indoor_humi);

    Paint_SelectImage(BlackImage);
    Paint_Clear(WHITE);

    // 数据
    float outdoor_temp = 28.0; // 虚拟室外数据
    int outdoor_humi = 45;     // 虚拟室外数据

    char buf[50];

    // 1. 头部（黑底白字）
    Paint_DrawRectangle(0, 0, 400, 50, BLACK, DOT_PIXEL_1X1, DRAW_FILL_FULL);
    
    // 绘制日期
    struct tm timeinfo;
    if(!getLocalTime(&timeinfo)){
        printf("Failed to obtain time\r\n");
        sprintf(buf, "2024年1月1日");
    } else {
        sprintf(buf, "%d年%d月%d日", timeinfo.tm_year + 1900, timeinfo.tm_mon + 1, timeinfo.tm_mday);
    }
    // 粗略的居中计算（假设平均每个字符/数字约 32px）
    // 9 chars * 32 = 288. Center ~56.
    Paint_DrawString_CN(56, 9, buf, &FontNewCN, WHITE, BLACK);

    // 2. 垂直分割线
    Paint_DrawLine(200, 50, 200, 300, BLACK, DOT_PIXEL_1X1, LINE_STYLE_SOLID);

    // 3. 左侧区域（室外）
    Paint_DrawString_CN(68, 60, "室外", &FontNewCN, BLACK, WHITE);

    if (!last_weather_icon.length() || !drawIconFromFS(last_weather_icon, 72, 104, 48, 48)) {
        Paint_DrawString_CN(84, 150, last_weather_text.c_str(), &FontNewCN, BLACK, WHITE);
    }

    // 室外温度
    Paint_DrawString_CN(29, 190 - 4, "温度", &FontNewCN, BLACK, WHITE);
    sprintf(buf, "%.1f", last_outdoor_temp > -900 ? last_outdoor_temp : outdoor_temp);
    int numlen = strlen(buf);
    int label_x_out = 29;
    int label_w_out = FontNewCN.Width * 2;
    int gap_out = 6;
    int max_right_out = 200 - 4;
    int total_w_out = Font24.Width * numlen + FontNewCN.Width;
    int temp_x_out = label_x_out + label_w_out + gap_out;
    int min_x_out = max_right_out - total_w_out;
    if (temp_x_out > min_x_out) temp_x_out = min_x_out;
    Paint_DrawString_EN(temp_x_out, 190, buf, &Font24, WHITE, BLACK);
    Paint_DrawString_CN(temp_x_out + Font24.Width * numlen, 190 - 4, "℃", &FontNewCN, BLACK, WHITE);

    // 室外湿度
    Paint_DrawString_CN(59, 230, "湿度", &FontNewCN, BLACK, WHITE);
    sprintf(buf, "%d", last_outdoor_humi > -1 ? last_outdoor_humi : outdoor_humi);
    int hum_x_out = 59 + FontNewCN.Width * 2 + 8;
    Paint_DrawString_EN(hum_x_out, 230 + 4, buf, &Font24, WHITE, BLACK);
    int humlen = strlen(buf);
    Paint_DrawString_CN(hum_x_out + Font24.Width * humlen, 230, "%", &FontNewCN, BLACK, WHITE);


    // 4. 右侧区域（室内）
    Paint_DrawString_CN(268, 60, "室内", &FontNewCN, BLACK, WHITE);

    // 室内温度
    Paint_DrawString_CN(229, 120 - 4, "温度", &FontNewCN, BLACK, WHITE);
    sprintf(buf, "%.1f", indoor_temp);
    int temp_x_in = 229 + FontNewCN.Width * 2 + 8;
    Paint_DrawString_EN(temp_x_in, 120 , buf, &Font24, WHITE, BLACK);
    numlen = strlen(buf);
    Paint_DrawString_CN(temp_x_in + Font24.Width * numlen, 120 - 4, "℃", &FontNewCN, BLACK, WHITE);

    // 室内湿度
    Paint_DrawString_CN(259, 160, "湿度", &FontNewCN, BLACK, WHITE);
    sprintf(buf, "%d", indoor_humi);
    int hum_x_in = 259 + FontNewCN.Width * 2 + 8;
    Paint_DrawString_EN(hum_x_in, 160 + 4, buf, &Font24, WHITE, BLACK);
    humlen = strlen(buf);
    Paint_DrawString_CN(hum_x_in + Font24.Width * humlen, 160, "%", &FontNewCN, BLACK, WHITE);

    // WiFi 状态（右下角）
    int wifi_y = 270;
    if (WiFi.status() == WL_CONNECTED) {
        String ip = WiFi.localIP().toString();
        sprintf(buf, "IP %s", ip.c_str());
        // 计算宽度以右对齐
        // int str_width = strlen(buf) * Font12.Width;
        // int wifi_x = 400 - str_width - 10; // 右对齐，保留 10px 边距
        
        // 硬编码以确保按要求右对齐
        int wifi_x = 280; 
        
        Paint_DrawString_EN(wifi_x, wifi_y, buf, &Font12, WHITE, BLACK);
    } else {
        int wifi_x = 10;
        Paint_DrawString_EN(wifi_x, wifi_y, "WiFi not connected", &Font12, WHITE, BLACK);
    }

    // 显示
    EPD_4IN2_V2_Display(BlackImage);
}

 /* 入口点 ----------------------------------------------------------------*/
 void setup()
{
    printf("EPD_4IN2_V2 Weather Station Demo\r\n");
    
    // 1. 启动延迟 5 秒
    printf("Startup Delay 5s...\r\n");
    delayWithOTA(5000);
 
    DEV_Module_Init();
 
    // 加载配置
   loadConfig();
 
     // WiFi 连接
    connectWiFi();
    
    // 进行任何网络调用前等待 3 秒以确保 WiFi 稳定
    if (WiFi.status() == WL_CONNECTED) {
        printf("WiFi Connected. Syncing time...\r\n");
        configTime(8 * 3600, 0, "ntp.aliyun.com", "pool.ntp.org", "cn.ntp.org.cn");
        ArduinoOTA.setHostname("esp32_epaper");
        if (CFG.ota_password.length() > 0) {
            ArduinoOTA.setPassword(CFG.ota_password.c_str());
        }
        ArduinoOTA.onStart([]() { printf("OTA Start\r\n"); });
        ArduinoOTA.onEnd([]() { printf("OTA End\r\n"); });
        ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) { printf("OTA %u%%\r\n", (progress * 100) / total); });
        ArduinoOTA.onError([](ota_error_t error) { printf("OTA Error %d\r\n", error); });
        ArduinoOTA.begin();
        printf("Waiting 3s before API access...\r\n");
        delayWithOTA(3000);
    }

    // MQTT 设置
    mqttSetup();
 
     // 初始化传感器 I2C
     Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL);
     
     // 2. 等待 SHT45 有效数据
    float temp = 0;
    int humi = 0;
    printf("Waiting for SHT45...\r\n");
    while(!readSensor(&temp, &humi)) {
        printf("SHT45 Not Ready, retrying...\r\n");
        delayWithOTA(1000);
    }
    printf("SHT45 Ready: %.2f C, %d %%\r\n", temp, humi);

    // 初始化电子墨水屏
    printf("e-Paper Init...\r\n");
    EPD_4IN2_V2_Init();
    EPD_4IN2_V2_Clear();
    DEV_Delay_ms(500);

    // 分配内存
    UWORD Imagesize = ((EPD_4IN2_V2_WIDTH % 8 == 0)? (EPD_4IN2_V2_WIDTH / 8 ): (EPD_4IN2_V2_WIDTH / 8 + 1)) * EPD_4IN2_V2_HEIGHT;
    if((BlackImage = (UBYTE *)malloc(Imagesize)) == NULL) {
        printf("Failed to apply for black memory...\r\n");
        while (1);
    }
     Paint_NewImage(BlackImage, EPD_4IN2_V2_WIDTH, EPD_4IN2_V2_HEIGHT, 0, WHITE);
 
     // 初始获取一次室外天气
     float tOut = 0; int hOut = 0; String txtOut = "晴";
     if (fetchOutdoorWeather(&tOut, &hOut, &txtOut)) {
         last_outdoor_temp = tOut;
         last_outdoor_humi = hOut;
         last_weather_text = txtOut;
     }
 
     // 首次显示
     updateDisplay(temp, humi);
     
     // 存储上次的值
     last_indoor_temp = temp;
     last_indoor_humi = humi;

     // 初始化实时值
     realtime_indoor_temp = temp;
     realtime_indoor_humi = humi;
    lastSensorTemp = temp;
    lastSensorHumi = humi;
    lastSensorSampleMs = millis();
 
     // 电子墨水屏休眠以省电
     printf("Goto Sleep...\r\n");
     EPD_4IN2_V2_Sleep();
 }
 
/* 主循环 -------------------------------------------------------------*/
 void loop()
{
    delayWithOTA(2000);
 
    float current_temp = 0;
    int current_humi = 0;
 
    if (readSensor(&current_temp, &current_humi)) {
       // 检查是否有显著变化以防止抖动
       // 阈值：温度 0.2°C，湿度 2%
       
       bool changed = false;
       
       // 过滤无效读数（SHT4x -45 意味着原始值 0，可能是错误）
       if (current_temp < -40 || current_temp > 85) {
           Serial.printf("Ignored invalid temp: %.2f\n", current_temp);
       } else {
           // 更新 MQTT 的实时值
           realtime_indoor_temp = current_temp;
           realtime_indoor_humi = current_humi;

            unsigned long sampleMs = millis();
            if (lastSensorSampleMs != 0) {
                unsigned long dtMs = sampleMs - lastSensorSampleMs;
                if (dtMs > 0) {
                    float segTemp = (lastSensorTemp + current_temp) * 0.5f;
                    float segHumi = ((float)lastSensorHumi + (float)current_humi) * 0.5f;
                    indoorTempWeightedSumMs += (double)segTemp * (double)dtMs;
                    indoorHumiWeightedSumMs += (double)segHumi * (double)dtMs;
                    indoorWeightTotalMs += (double)dtMs;
                }
            }
            lastSensorSampleMs = sampleMs;
            lastSensorTemp = current_temp;
            lastSensorHumi = current_humi;

           if (abs(current_temp - last_indoor_temp) >= 0.2) {
               Serial.printf("Temp Changed: Old=%.2f New=%.2f\n", last_indoor_temp, current_temp);
               changed = true;
           }
           
           if (abs(current_humi - last_indoor_humi) >= 2) {
               Serial.printf("Humi Changed: Old=%d New=%d\n", last_indoor_humi, current_humi);
               changed = true;
           }
       }

       if (changed) {
           printf("Change detected! Updating EPD...\r\n");
           
           // 唤醒
           DEV_Module_Init(); // Ensure SPI/GPIO active
           EPD_4IN2_V2_Init();
           
           // 更新
           updateDisplay(current_temp, current_humi);
           
           // 休眠
           EPD_4IN2_V2_Sleep();
           
           // 更新上次的值
           last_indoor_temp = current_temp;
           last_indoor_humi = current_humi;
       } else {
           // 调试打印以显示为什么没有更新
           // printf("No Change: T=%.2f(%.1f) vs %.2f(%.1f), H=%d vs %d\r\n", current_temp, (float)t1/10.0, last_indoor_temp, (float)t2/10.0, current_humi, last_indoor_humi);
       }
    }
 
    // MQTT 每 3 秒发布一次，保持精度
    mqttClient.loop();
    unsigned long nowMs = millis();
    if (nowMs - lastMqttPublishMs >= 30000) {
        if (!mqttDiscoveryPublished) {
            mqttPublishDiscovery("epaper_temperature_in", "主卧室内温度", "temperature", "°C");
            mqttPublishDiscovery("epaper_humidity_in", "主卧室内湿度", "humidity", "%");
            mqttPublishDiscovery("epaper_temperature_out", "EPaper 室外温度", "temperature", "°C");
            mqttPublishDiscovery("epaper_humidity_out", "EPaper 室外湿度", "humidity", "%");
            mqttPublishDiscovery("epaper_wifi_rssi", "EPaper WiFi RSSI", "signal_strength", "dBm");
            mqttPublishDiscovery("epaper_battery_percent", "EPaper 电池", "battery", "%");
            mqttDiscoveryPublished = true;
        }

        if (lastSensorSampleMs != 0) {
            unsigned long dtTail = nowMs - lastSensorSampleMs;
            if (dtTail > 0 && realtime_indoor_temp > -40 && realtime_indoor_temp < 85) {
                float segTemp = realtime_indoor_temp;
                float segHumi = realtime_indoor_humi;
                indoorTempWeightedSumMs += (double)segTemp * (double)dtTail;
                indoorHumiWeightedSumMs += (double)segHumi * (double)dtTail;
                indoorWeightTotalMs += (double)dtTail;
            }
        }

        float avgIndoorTemp;
        int avgIndoorHumi;
        if (indoorWeightTotalMs > 0) {
            float avgTemp = (float)(indoorTempWeightedSumMs / indoorWeightTotalMs);
            float avgHumi = (float)(indoorHumiWeightedSumMs / indoorWeightTotalMs);
            avgIndoorTemp = avgTemp;
            avgIndoorHumi = (int)(avgHumi + 0.5f);
        } else {
            avgIndoorTemp = realtime_indoor_temp;
            avgIndoorHumi = realtime_indoor_humi;
        }

        publishMetrics(avgIndoorTemp, avgIndoorHumi,
                       last_outdoor_temp,
                       last_outdoor_humi);
        lastMqttPublishMs = nowMs;
        indoorTempWeightedSumMs = 0;
        indoorHumiWeightedSumMs = 0;
        indoorWeightTotalMs = 0;
        lastSensorSampleMs = nowMs;
    }
 
    if (WiFi.status() == WL_CONNECTED && nowMs - lastWeatherFetchMs >= 600000) {
        float tOut = 0; int hOut = 0; String txtOut = last_weather_text;
        if (fetchOutdoorWeather(&tOut, &hOut, &txtOut)) {
            last_outdoor_temp = tOut;
            last_outdoor_humi = hOut;
            last_weather_text = txtOut;
        }
        lastWeatherFetchMs = nowMs;
    }
}
