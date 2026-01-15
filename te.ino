/* 包含文件 ------------------------------------------------------------------*/
#include "DEV_Config.h"
#include "EPD.h"
#include "EPD_GDEH042Z96.h"
#include "GUI_Paint.h"
#include "imagedata.h"
#include "fonts.h"
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
#include <memory>
#include <new>
#include <vector>

// 解压 Gzip 数据的辅助函数
bool gzip_decode(uint8_t* input, size_t inputLen, String &output) {
    tinfl_decompressor* inflator = (tinfl_decompressor*)malloc(sizeof(tinfl_decompressor));
    if (!inflator) {
        printf("Failed to allocate inflator\r\n");
        return false;
    }
    tinfl_init(inflator);

    const size_t MAX_OUT = 48000; // 增加缓冲区以容纳较大的预报数据
    uint8_t* outBuf = (uint8_t*)malloc(MAX_OUT);
    if (!outBuf) {
        printf("miniz malloc failed\r\n");
        free(inflator);
        return false;
    }

    size_t in_bytes = inputLen;
    size_t out_bytes = MAX_OUT;
    int flags = TINFL_FLAG_PARSE_ZLIB_HEADER | TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF;

    size_t offset = 0;
    if (inputLen > 10 && input[0] == 0x1F && input[1] == 0x8B) {
        offset = 10; 
        flags = TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF;
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
#define PIN_I2C_SDA 21
#define PIN_I2C_SCL 22
#define SHT4X_I2C_ADDR 0x44

UBYTE *BlackImage = NULL;
UBYTE *RedImage = NULL;

// 存储变量
float last_indoor_temp = -999.0;
int last_indoor_humi = -1;
float realtime_indoor_temp = -999.0;
int realtime_indoor_humi = -1;
unsigned long lastSensorSampleMs = 0;
float lastSensorTemp = 0;
int lastSensorHumi = 0;
double indoorTempWeightedSumMs = 0;
double indoorHumiWeightedSumMs = 0;
double indoorWeightTotalMs = 0;

const float TEMP_UPDATE_THRESHOLD = 0.3f;
const int HUMI_UPDATE_THRESHOLD = 2;
int lastDisplayMinute = -1;
int lastNtpSyncYday = -1;
int lastHaUpdateYday = -1;

// 天气和 HA 数据
struct DailyForecast {
    String date;
    String tempMax;
    String tempMin;
    String iconDay;
    String textDay;
    String precip; // 降水量
};
std::vector<DailyForecast> forecasts;

struct HAData {
    String lunar_year;  // 乙巳蛇年
    String lunar_date;  // 冬月廿七
    String shujiu;      // 三九 第8天
    String birthday_summary; // 滚动显示的生日信息
};
HAData haData;

struct CurrentWeather {
    String temp;
    String icon;
    String text;
    String windDir;
    String windScale;
    String windSpeed;
    String humidity;
    String pressure;
    String feelsLike;
};
CurrentWeather currentKw;

static String strTrim(const String& s) {
    int start = 0;
    int end = (int)s.length() - 1;
    while (start <= end && (s[start] == ' ' || s[start] == '\r' || s[start] == '\n' || s[start] == '\t')) start++;
    while (end >= start && (s[end] == ' ' || s[end] == '\r' || s[end] == '\n' || s[end] == '\t')) end--;
    if (end < start) return "";
    return s.substring(start, end + 1);
}

static String normalizeWindDir(const String& s) {
    if (s.indexOf("无持续风向") >= 0) return "VAR";
    bool hasN = s.indexOf("北") >= 0;
    bool hasS = s.indexOf("南") >= 0;
    bool hasE = s.indexOf("东") >= 0;
    bool hasW = s.indexOf("西") >= 0;
    if (hasN && hasE) return "NE";
    if (hasN && hasW) return "NW";
    if (hasS && hasE) return "SE";
    if (hasS && hasW) return "SW";
    if (hasN) return "N";
    if (hasS) return "S";
    if (hasE) return "E";
    if (hasW) return "W";
    return s;
}

static bool extractValueFromMultiline(const String& text, const String& key, String& out) {
    int pos = text.indexOf(key + ":");
    int colon = -1;
    if (pos >= 0) {
        colon = text.indexOf(':', pos);
    } else {
        pos = text.indexOf(key + "：");
        if (pos >= 0) {
            colon = text.indexOf('：', pos);
        }
    }
    if (pos < 0) return false;
    if (colon < 0) return false;
    int lineEnd = text.indexOf('\n', colon + 1);
    if (lineEnd < 0) lineEnd = (int)text.length();
    out = strTrim(text.substring(colon + 1, lineEnd));
    return out.length() > 0;
}

static bool httpGetBodyToString(HTTPClient& http, String& out) {
    out = "";
    int size = http.getSize();
    WiFiClient* stream = http.getStreamPtr();
    if (!stream) return false;

    if (size > 0) {
        std::unique_ptr<uint8_t[]> buf(new (std::nothrow) uint8_t[size]);
        if (!buf) return false;
        int readn = stream->readBytes(buf.get(), size);
        if (readn <= 0) return false;
        if (readn > 2 && buf[0] == 0x1F && buf[1] == 0x8B) {
            String decoded;
            if (!gzip_decode(buf.get(), (size_t)readn, decoded)) return false;
            out = decoded;
            return true;
        }
        out = String((char*)buf.get(), (size_t)readn);
        return true;
    }

    uint32_t start = millis();
    while (millis() - start < 5000) {
        while (stream->available()) {
            out += (char)stream->read();
        }
        if (!http.connected()) break;
        delay(5);
    }
    if (out.length() == 0) return false;
    if (out.length() > 2 && (uint8_t)out[0] == 0x1F && (uint8_t)out[1] == 0x8B) {
        String decoded;
        if (!gzip_decode((uint8_t*)out.c_str(), (size_t)out.length(), decoded)) return false;
        out = decoded;
    }
    return true;
}

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
    String qweather_location;
    String qweather_lang = "zh";
    String qweather_unit = "m";
    String qweather_forecast_days = "3d"; // 3d, 7d
    int weather_now_refresh_min = 10;
    int weather_forecast_refresh_h = 6;

    String ha_host;
    int ha_port = 8123;
    String ha_token;
    String ha_lunar_entity = "sensor.lunar_calendar";

    int battery_adc_pin = -1;
    float battery_scale = 1.0f;
};

AppConfig CFG;
WiFiClient espClient;
PubSubClient mqttClient(espClient);
unsigned long lastMqttPublishMs = 0;
unsigned long lastNowFetchMs = 0;
unsigned long lastForecastFetchMs = 0;
bool mqttDiscoveryPublished = false;

// 辅助函数：提取 JSON
bool jsonGetNumber(const String& json, const String& key, float* out) {
    int k = json.indexOf("\"" + key + "\"");
    if (k < 0) return false;
    int colon = json.indexOf(':', k);
    if (colon < 0) return false;
    int start = colon + 1;
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

void loadConfig() {
    if (!LittleFS.begin(true)) {
        printf("LittleFS init failed\r\n");
        return;
    }
    File f = LittleFS.open("/conf.json", "r");
    if (!f) {
        printf("conf.json not found\r\n");
        return;
    }
    String json = f.readString();
    f.close();

    // 简单解析 (建议生产环境使用 ArduinoJson，但这里沿用简单解析以保持依赖最小化，
    // 不过后面 fetchHAData 必须用 ArduinoJson 因为结构太复杂)
    String s; float n;
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
    if (jsonGetString(json, "qweather_forecast_days", &s)) CFG.qweather_forecast_days = s;
    if (jsonGetNumber(json, "weather_now_refresh_min", &n)) CFG.weather_now_refresh_min = (int)n;
    if (jsonGetNumber(json, "weather_forecast_refresh_h", &n)) CFG.weather_forecast_refresh_h = (int)n;

    if (jsonGetString(json, "ha_host", &s)) CFG.ha_host = s;
    if (jsonGetNumber(json, "ha_port", &n)) CFG.ha_port = (int)n;
    if (jsonGetString(json, "ha_token", &s)) CFG.ha_token = s;
    if (jsonGetString(json, "ha_lunar_entity", &s)) CFG.ha_lunar_entity = s;

    if (jsonGetNumber(json, "battery_adc_pin", &n)) CFG.battery_adc_pin = (int)n;
    if (jsonGetNumber(json, "battery_scale", &n)) CFG.battery_scale = n;

    printf("Config loaded. HA Host: %s\r\n", CFG.ha_host.c_str());
}

void connectWiFi() {
    if (CFG.wifi_ssid.length() == 0) return;
    printf("Connecting WiFi %s...\r\n", CFG.wifi_ssid.c_str());
    WiFi.mode(WIFI_STA);
    WiFi.begin(CFG.wifi_ssid.c_str(), CFG.wifi_password.c_str());
    int tries = 0;
    while (WiFi.status() != WL_CONNECTED && tries < 30) {
        delay(500);
        printf(".");
        tries++;
    }
    printf("\r\nWiFi Status: %d\r\n", WiFi.status());
}

static bool mqttEnsureConnected() {
    if (CFG.mqtt_host.length() == 0) return false;
    if (!mqttClient.connected()) {
        mqttClient.setKeepAlive(30);
        String cid = "esp32_epaper_" + String((uint32_t)ESP.getEfuseMac(), HEX);
        bool ok;
        if (CFG.mqtt_user.length() > 0) {
            ok = mqttClient.connect(cid.c_str(), CFG.mqtt_user.c_str(), CFG.mqtt_pass.c_str());
        } else {
            ok = mqttClient.connect(cid.c_str());
        }
        if (!ok) return false;
    }
    return true;
}

static void mqttPublishKV(const String& sub, const String& payload) {
    if (CFG.mqtt_base.length() == 0) return;
    String t = CFG.mqtt_base + "/" + sub;
    mqttClient.publish(t.c_str(), payload.c_str(), true);
}

// 传感器读取
bool readSensor(float *temp, int *humi) {
    Wire.beginTransmission(SHT4X_I2C_ADDR);
    Wire.write(0xFD);
    if (Wire.endTransmission() != 0) return false;
    delay(20);
    Wire.requestFrom(SHT4X_I2C_ADDR, 6);
    if (Wire.available() < 6) return false;
    uint8_t rx[6];
    for (int i = 0; i < 6; i++) rx[i] = Wire.read();
    uint16_t t_ticks = (rx[0] << 8) | rx[1];
    uint16_t rh_ticks = (rx[3] << 8) | rx[4];
    *temp = -45 + 175 * ((float)t_ticks / 65535.0);
    float rh = -6 + 125 * ((float)rh_ticks / 65535.0);
    if (rh > 100) rh = 100; if (rh < 0) rh = 0;
    *humi = (int)(rh + 0.5);
    return true;
}

// 获取 HA 数据
bool fetchHAData() {
    if (CFG.ha_host.length() == 0 || CFG.ha_token.length() == 0) return false;

    HTTPClient http;
    String url = "http://" + CFG.ha_host + ":" + String(CFG.ha_port) + "/api/states/" + CFG.ha_lunar_entity;
    
    printf("Fetching HA Data: %s\r\n", url.c_str());
    
    http.begin(url);
    http.addHeader("Authorization", "Bearer " + CFG.ha_token);
    http.addHeader("Content-Type", "application/json");

    int httpCode = http.GET();
    if (httpCode == 200) {
        String payload = http.getString();
        
        JsonDocument doc;
        DeserializationError error = deserializeJson(doc, payload);
        if (error) {
            printf("HA JSON parse failed: %s\r\n", error.c_str());
            http.end();
            return false;
        }

        JsonObject attrs = doc["attributes"];

        haData.lunar_year = "";
        haData.lunar_date = "";
        haData.shujiu = "";

        if (attrs.containsKey("今天的农历日期")) {
            JsonVariant v = attrs["今天的农历日期"];
            if (v.is<JsonObject>()) {
                JsonObject o = v.as<JsonObject>();
                if (o.containsKey("年")) haData.lunar_year = o["年"].as<String>();
                if (o.containsKey("日期")) haData.lunar_date = o["日期"].as<String>();
                if (o.containsKey("数九")) haData.shujiu = o["数九"].as<String>();
            } else if (v.is<const char*>()) {
                String t = v.as<String>();
                extractValueFromMultiline(t, "年", haData.lunar_year);
                extractValueFromMultiline(t, "日期", haData.lunar_date);
                extractValueFromMultiline(t, "数九", haData.shujiu);
            }
        }

        if (haData.lunar_date.length() == 0 || haData.lunar_year.length() == 0) {
            for (JsonPair kv : attrs) {
                String k = kv.key().c_str();
                if (k.indexOf("农历") >= 0 && k.indexOf("日期") >= 0) {
                    JsonVariant v = kv.value();
                    if (v.is<JsonObject>()) {
                        JsonObject o = v.as<JsonObject>();
                        if (haData.lunar_year.length() == 0 && o.containsKey("年")) haData.lunar_year = o["年"].as<String>();
                        if (haData.lunar_date.length() == 0 && o.containsKey("日期")) haData.lunar_date = o["日期"].as<String>();
                        if (haData.shujiu.length() == 0 && o.containsKey("数九")) haData.shujiu = o["数九"].as<String>();
                    } else if (v.is<const char*>()) {
                        String t = v.as<String>();
                        if (haData.lunar_year.length() == 0) extractValueFromMultiline(t, "年", haData.lunar_year);
                        if (haData.lunar_date.length() == 0) extractValueFromMultiline(t, "日期", haData.lunar_date);
                        if (haData.shujiu.length() == 0) extractValueFromMultiline(t, "数九", haData.shujiu);
                    }
                    break;
                }
            }
        }

        if (haData.lunar_year.length() == 0 && attrs.containsKey("年")) haData.lunar_year = attrs["年"].as<String>();
        if (haData.lunar_date.length() == 0 && attrs.containsKey("日期")) haData.lunar_date = attrs["日期"].as<String>();
        if (haData.shujiu.length() == 0 && attrs.containsKey("数九")) haData.shujiu = attrs["数九"].as<String>();

        if (haData.lunar_date.length() == 0 && attrs.containsKey("lunar_date")) haData.lunar_date = attrs["lunar_date"].as<String>();
        if (haData.lunar_year.length() == 0 && attrs.containsKey("lunar_year")) haData.lunar_year = attrs["lunar_year"].as<String>();

        String summary = "";
        if (attrs.containsKey("生日") && attrs["生日"].is<JsonArray>()) {
            JsonArray birthdays = attrs["生日"].as<JsonArray>();
            for (JsonObject b : birthdays) {
                String name = b["名称"].as<String>();
                String daysStr = "";
                if (b.containsKey("阳历天数")) daysStr = String(b["阳历天数"].as<int>());
                else if (b.containsKey("阳历天数说明")) daysStr = b["阳历天数说明"].as<String>();
                if (daysStr.length() > 0) summary += name + " 距离" + daysStr + "天  ";
            }
        }
        haData.birthday_summary = summary;
        
        printf("HA Data: Year=%s Date=%s BD=%s\r\n", haData.lunar_year.c_str(), haData.lunar_date.c_str(), haData.birthday_summary.c_str());
        http.end();
        return true;
    } else {
        printf("HA HTTP Error: %d\r\n", httpCode);
        http.end();
        return false;
    }
}

// 获取天气预报
bool fetchWeatherNow() {
    if (CFG.qweather_token.length() == 0) return false;

    WiFiClientSecure client;
    client.setInsecure();
    HTTPClient http;

    // 获取实时天气
    String urlNow = "https://" + CFG.qweather_host + "/v7/weather/now?location=" + CFG.qweather_location + "&key=" + CFG.qweather_token + "&lang=" + CFG.qweather_lang + "&unit=" + CFG.qweather_unit;
    http.begin(client, urlNow);
    int codeNow = http.GET();
    if (codeNow == 200) {
        String payload;
        if (httpGetBodyToString(http, payload)) {
            JsonDocument doc;
            if (deserializeJson(doc, payload) == DeserializationError::Ok) {
                JsonObject now = doc["now"];
                currentKw.temp = now["temp"].as<String>();
                currentKw.icon = now["icon"].as<String>();
                currentKw.text = now["text"].as<String>();
                currentKw.windDir = now["windDir"].as<String>();
                currentKw.windScale = now["windScale"].as<String>();
                currentKw.windSpeed = now["windSpeed"].as<String>();
                currentKw.humidity = now["humidity"].as<String>();
                currentKw.pressure = now["pressure"].as<String>();
                currentKw.feelsLike = now["feelsLike"].as<String>();
            }
        }
    } else {
        printf("QWeather now HTTP %d\r\n", codeNow);
    }
    http.end();
    return true;
}

bool fetchWeatherForecast() {
    if (CFG.qweather_token.length() == 0) return false;
    WiFiClientSecure client;
    client.setInsecure();
    HTTPClient http;
    // 获取预报
    String urlDaily = "https://" + CFG.qweather_host + "/v7/weather/" + CFG.qweather_forecast_days + "?location=" + CFG.qweather_location + "&key=" + CFG.qweather_token + "&lang=" + CFG.qweather_lang + "&unit=" + CFG.qweather_unit;
    http.begin(client, urlDaily);
    int codeDaily = http.GET();
    if (codeDaily == 200) {
        String payload;
        if (httpGetBodyToString(http, payload)) {
            JsonDocument doc;
            if (deserializeJson(doc, payload) == DeserializationError::Ok) {
                JsonArray daily = doc["daily"];
                forecasts.clear();
                for (JsonObject d : daily) {
                    DailyForecast f;
                    f.date = d["fxDate"].as<String>();
                    f.tempMax = d["tempMax"].as<String>();
                    f.tempMin = d["tempMin"].as<String>();
                    f.iconDay = d["iconDay"].as<String>();
                    f.textDay = d["textDay"].as<String>();
                    f.precip = d["precip"].as<String>();
                    forecasts.push_back(f);
                }
            }
        }
    } else {
        printf("QWeather daily HTTP %d\r\n", codeDaily);
    }
    http.end();
    return true;
}

static int guessSquareIconSize(size_t bytes, int fallback) {
    const int sizes[] = {24, 32, 40, 48, 56, 64};
    for (int s : sizes) {
        size_t wbyte = (size_t)((s + 7) / 8);
        if (wbyte * (size_t)s == bytes) return s;
    }
    return fallback;
}

bool drawIconFromFS(const String& code, int x, int y, int preferredSize) {
    if (!LittleFS.begin(true)) return false;
    String base = (preferredSize <= 24) ? "/icons_bin24/" : "/icons_bin/";
    String path = base + code + ".bin";
    File f = LittleFS.open(path, "r");
    if (!f && preferredSize <= 24) {
        path = "/icons_bin/" + code + ".bin";
        f = LittleFS.open(path, "r");
    }
    if (!f) return false;

    size_t fileSize = f.size();
    int iconSize = guessSquareIconSize(fileSize, preferredSize);
    size_t need = (size_t)((iconSize + 7) / 8) * (size_t)iconSize;
    if (need != fileSize) {
        f.close();
        return false;
    }
    uint8_t* buf = (uint8_t*)malloc(need);
    if (!buf) { f.close(); return false; }
    size_t readn = f.read(buf, need);
    f.close();
    if (readn != need) { free(buf); return false; }
    Paint_DrawImage(buf, x, y, iconSize, iconSize);
    free(buf);
    return true;
}

// 绘制图表 (简单折线图)
void drawChart(int x, int y, int w, int h, const std::vector<float>& values, const char* label) {
    Paint_DrawRectangle(x, y, x + w, y + h, WHITE, DOT_PIXEL_1X1, DRAW_FILL_FULL);
    Paint_DrawRectangle(x, y, x + w, y + h, BLACK, DOT_PIXEL_1X1, DRAW_FILL_EMPTY);
    
    if (values.size() < 2) return;

    float minV = 1000, maxV = -1000;
    for (float v : values) {
        if (v < minV) minV = v;
        if (v > maxV) maxV = v;
    }
    // 留余量
    float range = maxV - minV;
    if (range == 0) range = 1;
    
    // 绘制曲线
    int prevX = 0, prevY = 0;
    float stepX = (float)w / (values.size() - 1);
    
    for (size_t i = 0; i < values.size(); i++) {
        int px = x + (int)(i * stepX);
        // y 轴翻转，数值越大越靠上
        int py = y + h - (int)((values[i] - minV) / range * (h - 10)) - 5; 
        
        if (i > 0) {
            Paint_DrawLine(prevX, prevY, px, py, BLACK, DOT_PIXEL_1X1, LINE_STYLE_SOLID);
        }
        prevX = px;
        prevY = py;
        
        // 绘制点
        Paint_DrawPoint(px, py, BLACK, DOT_PIXEL_2X2, DOT_STYLE_DFT);
    }
    
    // 标签
    Paint_DrawString_CN(x + 2, y + 2, label, &FontCN16, BLACK, WHITE);
}

void updateDisplay(float indoor_temp, int indoor_humi) {
    if (BlackImage == NULL) return;
    Paint_SelectImage(BlackImage);
    Paint_Clear(WHITE);
    Paint_SelectImage(RedImage);
    Paint_Clear(WHITE);
    
    // 切换回 Black 用于主要绘制
    Paint_SelectImage(BlackImage);

    // 1. 左上角：日期区域 (黑底白字) -> 参数 (Fg=WHITE, Bg=BLACK)
    // 区域: 0,0 - 160,100
    Paint_DrawRectangle(0, 0, 160, 100, BLACK, DOT_PIXEL_1X1, DRAW_FILL_FULL);
    
    struct tm timeinfo;
    bool hasTime = getLocalTime(&timeinfo);
    
    char buf[64];
    if (hasTime) {
        const char* weekDays[] = {"星期日", "星期一", "星期二", "星期三", "星期四", "星期五", "星期六"};

        // 三行布局：
        // 1) x月x日（公历） - FontCN24
        snprintf(buf, sizeof(buf), "%d月%d日", timeinfo.tm_mon + 1, timeinfo.tm_mday);
        Paint_DrawString_CN(10, 6, buf, &FontCN24, WHITE, BLACK);

        // 2) 农历xxx年 - 下移至 Y=40
        String lunarYearLine = "农历";
        if (haData.lunar_year.length() > 0) lunarYearLine += haData.lunar_year;
        Paint_DrawString_CN(10, 40, lunarYearLine.c_str(), &FontCN16, WHITE, BLACK);

        // 3) x月x日（农历） 星期x - 下移至 Y=66
        String lunarDateLine = haData.lunar_date;
        if (lunarDateLine.length() == 0) lunarDateLine = " ";
        lunarDateLine += " ";
        lunarDateLine += weekDays[timeinfo.tm_wday];
        Paint_DrawString_CN(10, 66, lunarDateLine.c_str(), &FontCN16, WHITE, BLACK);
    }

    // 2. 右上角：时间与天气区域背景填充为白色
    Paint_DrawRectangle(160, 0, 400, 100, WHITE, DOT_PIXEL_1X1, DRAW_FILL_FULL);

    if (hasTime) {
        sprintf(buf, "%02d:%02d", timeinfo.tm_hour, timeinfo.tm_min);
        Paint_DrawRectangle(160, 0, 260, 40, WHITE, DOT_PIXEL_1X1, DRAW_FILL_FULL);
        Paint_DrawString_CN(170, 12, buf, &FontCN24_Num, BLACK, WHITE);
    }
    
    // 天气图标（强制使用代码 150）
    if (currentKw.icon.length() > 0) {
        drawIconFromFS(currentKw.icon, 260, 4, 48);
    }
    
    // 天气文字
    Paint_DrawString_CN(330, 14, currentKw.text.c_str(), &FontCN32, BLACK, WHITE);
    
    int detailY = 60;
    String windLine = currentKw.windDir + " " + currentKw.windScale + "级 " + currentKw.windSpeed + "km/h";
    Paint_DrawString_CN(170, detailY, windLine.c_str(), &FontCN16, BLACK, WHITE);

    String tempLine = "温度 " + currentKw.temp + "℃ 体感 " + currentKw.feelsLike + "℃";
    Paint_DrawString_CN(170, detailY + 18, tempLine.c_str(), &FontCN16, BLACK, WHITE);

    // 3. 中间区域：室内与预报
    // 分割线
    Paint_DrawLine(0, 100, 400, 100, BLACK, DOT_PIXEL_1X1, LINE_STYLE_SOLID);
    
    // 左侧：室内温湿度 (0,100 - 160,270)
    snprintf(buf, sizeof(buf), "温度: %.1f℃", indoor_temp);
    Paint_DrawString_CN(10, 120, buf, &FontCN16, BLACK, WHITE);
    snprintf(buf, sizeof(buf), "湿度: %d%%", indoor_humi);
    Paint_DrawString_CN(10, 146, buf, &FontCN16, BLACK, WHITE);
    
    Paint_DrawRectangle(160, 100, 400, 270, WHITE, DOT_PIXEL_1X1, DRAW_FILL_FULL);
    Paint_DrawLine(160, 100, 160, 270, BLACK, DOT_PIXEL_1X1, LINE_STYLE_SOLID);
    Paint_DrawLine(0, 100, 400, 100, BLACK, DOT_PIXEL_1X1, LINE_STYLE_SOLID);
    
    int fy = 110;
    int fx = 170;
    // 表头
    // Paint_DrawString_CN(fx, fy, "今日 明日 后天", &FontNewCN, WHITE, BLACK);
    
    // 列表显示
    for (size_t i = 0; i < forecasts.size() && i < 3; i++) {
        const auto& f = forecasts[i];
        // 日期
        String dateStr = f.date.substring(5); // MM-DD
        Paint_DrawString_CN(fx + i*75, fy, dateStr.c_str(), &FontCN12, BLACK, WHITE);
        
        // 图标
        drawIconFromFS(f.iconDay, fx + i*75 + 20, fy + 18, 24);
        
        // 温度
        String tempRange = f.tempMin + "~" + f.tempMax + "℃";
        Paint_DrawString_CN(fx + i*75, fy + 60, tempRange.c_str(), &FontCN12, BLACK, WHITE);
        
        // 文字
        Paint_DrawString_CN(fx + i*75, fy + 80, f.textDay.c_str(), &FontCN16, BLACK, WHITE);
    }

    // 4. 底部：生日信息 (0,270 - 400,300)
    Paint_DrawLine(0, 270, 400, 270, BLACK, DOT_PIXEL_1X1, LINE_STYLE_SOLID);
    
    if (haData.birthday_summary.length() > 0) {
        // 整行静态显示（超出屏幕部分自动裁切）
        Paint_DrawString_CN(5, 275, haData.birthday_summary.c_str(), &FontCN16, BLACK, WHITE);
    } else {
        Paint_DrawString_CN(5, 275, "无生日信息", &FontCN16, BLACK, WHITE);
    }

    EPD_GDEH042Z96_Display(BlackImage, RedImage);
}

void setup() {
    printf("EPD Start\r\n");
    delay(1000);
    DEV_Module_Init();
    loadConfig();
    connectWiFi();
    
    if (WiFi.status() == WL_CONNECTED) {
        configTime(8 * 3600, 0, "ntp.aliyun.com");
        mqttClient.setServer(CFG.mqtt_host.c_str(), CFG.mqtt_port);
        mqttEnsureConnected();
    }
    
    Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL);
    EPD_GDEH042Z96_Init();
    EPD_GDEH042Z96_Clear();
    
    // Allocate Memory
    UWORD Imagesize = ((EPD_GDEH042Z96_WIDTH % 8 == 0)? (EPD_GDEH042Z96_WIDTH / 8 ): (EPD_GDEH042Z96_WIDTH / 8 + 1)) * EPD_GDEH042Z96_HEIGHT;
    BlackImage = (UBYTE *)malloc(Imagesize);
    RedImage = (UBYTE *)malloc(Imagesize);
    Paint_NewImage(BlackImage, EPD_GDEH042Z96_WIDTH, EPD_GDEH042Z96_HEIGHT, 0, WHITE);
    Paint_NewImage(RedImage, EPD_GDEH042Z96_WIDTH, EPD_GDEH042Z96_HEIGHT, 0, WHITE);
    
    // Initial Fetch
    fetchHAData();
    fetchWeatherNow();
    fetchWeatherForecast();
    
    float t; int h;
    readSensor(&t, &h);
    updateDisplay(t, h);
    
    EPD_GDEH042Z96_Sleep();
}

void loop() {
    unsigned long now = millis();
    mqttEnsureConnected();
    mqttClient.loop();
    
    float t; int h;
    if (readSensor(&t, &h)) {
        if (lastSensorSampleMs > 0 && lastSensorTemp > -100.0f) {
            unsigned long dt = now - lastSensorSampleMs;
            indoorTempWeightedSumMs += (double)dt * lastSensorTemp;
            indoorHumiWeightedSumMs += (double)dt * (double)lastSensorHumi;
            indoorWeightTotalMs += (double)dt;
        }
        lastSensorSampleMs = now;
        lastSensorTemp = t;
        lastSensorHumi = h;
        realtime_indoor_temp = t;
        realtime_indoor_humi = h;
    }
    
    if (now - lastMqttPublishMs >= 30000) {
        float avgT = realtime_indoor_temp;
        int avgH = realtime_indoor_humi;
        if (indoorWeightTotalMs > 0.0) {
            avgT = (float)(indoorTempWeightedSumMs / indoorWeightTotalMs);
            avgH = (int)((indoorHumiWeightedSumMs / indoorWeightTotalMs) + 0.5);
        }
        mqttPublishKV("temperature_in", String(avgT, 2));
        mqttPublishKV("humidity_in", String(avgH));
        if (currentKw.temp.length() > 0) {
            mqttPublishKV("temperature_out", currentKw.temp);
        }
        if (currentKw.humidity.length() > 0) {
            mqttPublishKV("humidity_out", currentKw.humidity);
        }
        mqttPublishKV("wifi_rssi", String(WiFi.RSSI()));
        indoorTempWeightedSumMs = 0.0;
        indoorHumiWeightedSumMs = 0.0;
        indoorWeightTotalMs = 0.0;
        lastMqttPublishMs = now;
    }
    
    if (WiFi.status() != WL_CONNECTED) connectWiFi();

    unsigned long nowInterval = (unsigned long)CFG.weather_now_refresh_min * 60000UL;
    unsigned long forecastInterval = (unsigned long)CFG.weather_forecast_refresh_h * 3600000UL;
    if (lastNowFetchMs == 0 || now - lastNowFetchMs >= nowInterval) {
        fetchWeatherNow();
        lastNowFetchMs = now;
    }
    if (lastForecastFetchMs == 0 || now - lastForecastFetchMs >= forecastInterval) {
        fetchWeatherForecast();
        fetchHAData();
        lastForecastFetchMs = now;
    }

    bool needDisplayUpdate = false;
    struct tm timeinfo;
    bool hasTime = getLocalTime(&timeinfo);
    if (hasTime) {
        int curMin = timeinfo.tm_min;
        if (curMin != lastDisplayMinute) {
            needDisplayUpdate = true;
            lastDisplayMinute = curMin;
        }
        if (timeinfo.tm_hour == 0 && timeinfo.tm_min == 0 && lastNtpSyncYday != timeinfo.tm_yday) {
            configTime(8 * 3600, 0, "ntp.aliyun.com");
            lastNtpSyncYday = timeinfo.tm_yday;
        }
        if (timeinfo.tm_hour == 0 && timeinfo.tm_min == 10 && lastHaUpdateYday != timeinfo.tm_yday) {
            fetchHAData();
            lastHaUpdateYday = timeinfo.tm_yday;
        }
    }

    if (last_indoor_temp < -900.0f) {
        last_indoor_temp = realtime_indoor_temp;
        last_indoor_humi = realtime_indoor_humi;
    }

    float dTemp = fabs(realtime_indoor_temp - last_indoor_temp);
    int dHumi = abs(realtime_indoor_humi - last_indoor_humi);
    if (dTemp >= TEMP_UPDATE_THRESHOLD || dHumi >= HUMI_UPDATE_THRESHOLD) {
        needDisplayUpdate = true;
    }

    if (needDisplayUpdate) {
        DEV_Module_Init();
        EPD_GDEH042Z96_Init();
        updateDisplay(realtime_indoor_temp, realtime_indoor_humi);
        EPD_GDEH042Z96_Sleep();
        last_indoor_temp = realtime_indoor_temp;
        last_indoor_humi = realtime_indoor_humi;
    }

    delay(50);
}
