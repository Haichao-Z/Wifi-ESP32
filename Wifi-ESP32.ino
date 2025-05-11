#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <WebServer.h>
#include <PubSubClient.h> // MQTT客户端库
#include <ArduinoJson.h>  // JSON处理库
#include <stdio.h>
#include <string.h>
#include <dirent.h>
#include <sys/unistd.h>
#include <sys/stat.h>
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "driver/sdmmc_host.h"
#include <Base64.h>
#include <EEPROM.h> // 用于存储WiFi凭据

#define MOUNT_POINT "/sdcard"
#define EEPROM_SIZE 512

// 定义SD_OCR_SDHC_CAP（如果需要）
#ifndef SD_OCR_SDHC_CAP
#define SD_OCR_SDHC_CAP (1ULL << 30)
#endif

// WiFi配置结构体
struct WiFiConfig {
  char ssid[32];
  char password[64];
  char mqtt_server[64];
  int mqtt_port;
  char mqtt_user[32];
  char mqtt_password[64];
  bool configured;
};

// MQTT服务器默认设置（如果用户未配置）
const char* default_mqtt_server = "135.119.219.218";
const int default_mqtt_port = 1883;
const char* default_mqtt_user = "rttyobej";
const char* default_mqtt_password = "BrsJBNVoQBl7";
const char* client_id = "ESP32_SDCard_Manager";

// AP模式设置
const char* ap_ssid = "ESP32_SDManager";
const char* ap_password = "12345678"; // AP模式的密码，至少8个字符

// MQTT主题
String device_id = ""; // 设备唯一标识符，将在setup中生成
const char* topic_command = "sdcard/command/";
const char* topic_response = "sdcard/response/";
const char* topic_data = "sdcard/data/";

// 创建WiFi和MQTT客户端实例
WiFiClient espClient;
PubSubClient mqtt(espClient);

// Web服务器（用于配网和本地访问）
WebServer server(80);

// WiFi配置
WiFiConfig wifiConfig;

// 当前浏览目录
String currentDir = "/";

// 配置模式标志
bool configMode = false;

// LED指示灯引脚（可以根据实际硬件调整）
const int ledPin = 2; // ESP32开发板上的内置LED

// 配置按钮引脚
const int configButtonPin = 0; // 大多数ESP32开发板上的BOOT按钮

// 保存WiFi配置到EEPROM
void saveConfiguration() {
  EEPROM.put(0, wifiConfig);
  EEPROM.commit();
  Serial.println("配置已保存到EEPROM");
}

// 从EEPROM加载WiFi配置
bool loadConfiguration() {
  EEPROM.get(0, wifiConfig);
  if (wifiConfig.configured) {
    Serial.println("从EEPROM加载配置:");
    Serial.print("SSID: ");
    Serial.println(wifiConfig.ssid);
    Serial.print("MQTT服务器: ");
    Serial.println(wifiConfig.mqtt_server);
    return true;
  }
  return false;
}

// 进入配置模式
void enterConfigMode() {
  configMode = true;
  
  // 断开现有WiFi连接
  WiFi.disconnect();
  
  // 设置AP模式
  WiFi.mode(WIFI_AP);
  WiFi.softAP(ap_ssid, ap_password);
  
  Serial.println("进入配置模式");
  Serial.print("AP SSID: ");
  Serial.println(ap_ssid);
  Serial.print("AP 密码: ");
  Serial.println(ap_password);
  Serial.print("IP地址: ");
  Serial.println(WiFi.softAPIP());
  
  // LED快速闪烁表示配置模式
  for (int i = 0; i < 10; i++) {
    digitalWrite(ledPin, HIGH);
    delay(100);
    digitalWrite(ledPin, LOW);
    delay(100);
  }
}

// SD卡初始化函数
bool initSDCard() {
  Serial.println("\n初始化SD卡...");
  
  // 配置SD卡挂载选项
  esp_vfs_fat_sdmmc_mount_config_t mount_config = {
    .format_if_mount_failed = false,
    .max_files = 5,
    .allocation_unit_size = 16 * 1024
  };
  
  sdmmc_card_t *card;
  const char mount_point[] = MOUNT_POINT;

  // 配置SDMMC主机
  sdmmc_host_t host = SDMMC_HOST_DEFAULT();
  
  // 使用1位模式，降低频率以提高兼容性
  host.flags = SDMMC_HOST_FLAG_1BIT;
  host.max_freq_khz = SDMMC_FREQ_PROBING;

  // 配置SD卡槽
  sdmmc_slot_config_t slot_config = SDMMC_SLOT_CONFIG_DEFAULT();
  slot_config.width = 1; // 使用1位总线宽度
  
  // 为ESP32-S3配置GPIO引脚
  slot_config.clk = GPIO_NUM_39;
  slot_config.cmd = GPIO_NUM_38;
  slot_config.d0 = GPIO_NUM_40;
  
  // 使能内部上拉电阻
  slot_config.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;

  Serial.println("挂载文件系统");
  esp_err_t ret = esp_vfs_fat_sdmmc_mount(mount_point, &host, &slot_config, &mount_config, &card);

  if (ret != ESP_OK) {
    if (ret == ESP_FAIL) {
      Serial.println("挂载文件系统失败");
    } else {
      Serial.printf("SD卡初始化失败: %s\n", esp_err_to_name(ret));
    }
    return false;
  }

  Serial.println("文件系统挂载成功");
  
  // 打印SD卡信息
  Serial.printf("SD卡类型: %s\n", (card->ocr & SD_OCR_SDHC_CAP) ? "SDHC/SDXC" : "SDSC");
  Serial.printf("SD卡容量: %lluMB\n", ((uint64_t)card->csd.capacity) * card->csd.sector_size / (1024 * 1024));
  Serial.printf("SD卡名称: %s\n", card->cid.name);
  
  return true;
}

// 获取文件大小的辅助函数
String formatBytes(size_t bytes) {
  if (bytes < 1024) {
    return String(bytes) + " B";
  } else if (bytes < (1024 * 1024)) {
    return String(bytes / 1024.0) + " KB";
  } else if (bytes < (1024 * 1024 * 1024)) {
    return String(bytes / 1024.0 / 1024.0) + " MB";
  } else {
    return String(bytes / 1024.0 / 1024.0 / 1024.0) + " GB";
  }
}

// 连接MQTT服务器
void reconnectMQTT() {
  // 如果在配置模式，不连接MQTT
  if (configMode) {
    return;
  }
  
  // 循环直到连接成功
  int attempts = 0;
  while (!mqtt.connected() && attempts < 5) { // 限制尝试次数
    Serial.print("尝试连接MQTT服务器...");
    
    // 尝试连接
    if (mqtt.connect(client_id, wifiConfig.mqtt_user, wifiConfig.mqtt_password)) {
      Serial.println("已连接");
      
      // 订阅命令主题
      String full_command_topic = String(topic_command) + device_id;
      mqtt.subscribe(full_command_topic.c_str());
      Serial.printf("已订阅主题: %s\n", full_command_topic.c_str());
      
      // 发布设备上线消息
      String status_topic = String(topic_response) + device_id + "/status";
      mqtt.publish(status_topic.c_str(), "online");
      
      // LED常亮表示连接成功
      digitalWrite(ledPin, HIGH);
    } else {
      Serial.print("连接失败，rc=");
      Serial.print(mqtt.state());
      Serial.println(" 5秒后重试...");
      
      // LED闪烁表示连接失败
      for (int i = 0; i < 3; i++) {
        digitalWrite(ledPin, HIGH);
        delay(100);
        digitalWrite(ledPin, LOW);
        delay(100);
      }
      
      delay(5000);
      attempts++;
    }
  }
  
  if (!mqtt.connected() && attempts >= 5) {
    Serial.println("MQTT连接多次失败，考虑进入配置模式");
    // 如果多次尝试失败，可以选择进入配置模式
    // enterConfigMode();
  }
}

// 连接WiFi网络
void connectToWiFi() {
  if (configMode) return;
  
  Serial.println("连接到WiFi网络...");
  Serial.print("SSID: ");
  Serial.println(wifiConfig.ssid);
  
  WiFi.mode(WIFI_STA);
  WiFi.begin(wifiConfig.ssid, wifiConfig.password);
  
  // 等待连接，但设置超时
  int timeout = 0;
  while (WiFi.status() != WL_CONNECTED && timeout < 20) { // 20*500ms = 10秒超时
    delay(500);
    Serial.print(".");
    timeout++;
    
    // LED闪烁表示正在连接
    digitalWrite(ledPin, !digitalRead(ledPin));
  }
  
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("");
    Serial.println("WiFi已连接");
    Serial.print("IP地址: ");
    Serial.println(WiFi.localIP());
    
    // LED常亮表示连接成功
    digitalWrite(ledPin, HIGH);
  } else {
    Serial.println("");
    Serial.println("WiFi连接失败，进入配置模式");
    enterConfigMode();
  }
}

// 读取目录内容并创建JSON响应
String getDirListJSON(String path) {
  String fullPath = MOUNT_POINT + path;
  
  DIR* dir = opendir(fullPath.c_str());
  if (dir == NULL) {
    DynamicJsonDocument doc(256);
    doc["status"] = "error";
    doc["message"] = "无法打开目录";
    
    String jsonResponse;
    serializeJson(doc, jsonResponse);
    return jsonResponse;
  }
  
  // 创建JSON文档
  // 根据目录内容大小调整容量
  DynamicJsonDocument doc(16384);
  doc["status"] = "success";
  doc["path"] = path;
  
  JsonArray files = doc.createNestedArray("files");
  
  struct dirent* entry;
  while ((entry = readdir(dir)) != NULL) {
    String entryName = String(entry->d_name);
    
    // 跳过.和..条目
    if (entryName == "." || entryName == "..") {
      continue;
    }
    
    String entryPath = fullPath + "/" + entryName;
    
    struct stat entry_stat;
    if (stat(entryPath.c_str(), &entry_stat) == -1) {
      continue;
    }
    
    JsonObject fileObj = files.createNestedObject();
    fileObj["name"] = entryName;
    fileObj["isDirectory"] = S_ISDIR(entry_stat.st_mode);
    
    if (!S_ISDIR(entry_stat.st_mode)) {
      fileObj["size"] = entry_stat.st_size;
      fileObj["sizeFormatted"] = formatBytes(entry_stat.st_size);
    }
  }
  
  closedir(dir);
  
  String jsonResponse;
  serializeJson(doc, jsonResponse);
  return jsonResponse;
}

// 处理MQTT接收到的命令
void handleMQTTCommand(String payload) {
  Serial.println("处理命令: " + payload);
  
  DynamicJsonDocument doc(4096);
  DeserializationError error = deserializeJson(doc, payload);
  
  if (error) {
    Serial.print("JSON解析失败: ");
    Serial.println(error.c_str());
    return;
  }
  
  String command = doc["command"];
  String response_topic = String(topic_response) + device_id;
  
  Serial.println("命令类型: " + command);
  
  if (command == "listdir") {
    String path = doc["path"];
    if (path == "") path = "/";
    
    Serial.println("列出目录: " + path);
    currentDir = path;
    String dirList = getDirListJSON(path);
    
    Serial.println("目录列表JSON长度: " + String(dirList.length()));
    Serial.println("发布到主题: " + response_topic);
    
    bool published = mqtt.publish(response_topic.c_str(), dirList.c_str());
    Serial.println("发布结果: " + String(published ? "成功" : "失败"));
    
  } else if (command == "getfile") {
    String filePath = doc["path"];
    if (filePath == "") {
      DynamicJsonDocument errorDoc(256);
      errorDoc["status"] = "error";
      errorDoc["message"] = "未指定文件路径";
      
      String errorResponse;
      serializeJson(errorDoc, errorResponse);
      mqtt.publish(response_topic.c_str(), errorResponse.c_str());
      return;
    }
    
    String fullPath = MOUNT_POINT + filePath;
    FILE* f = fopen(fullPath.c_str(), "rb");
    
    if (!f) {
      DynamicJsonDocument errorDoc(256);
      errorDoc["status"] = "error";
      errorDoc["message"] = "无法打开文件";
      
      String errorResponse;
      serializeJson(errorDoc, errorResponse);
      mqtt.publish(response_topic.c_str(), errorResponse.c_str());
      return;
    }
    
    // 获取文件大小
    struct stat stat_buf;
    stat(fullPath.c_str(), &stat_buf);
    size_t fileSize = stat_buf.st_size;
    
    // 获取文件名
    int lastSlash = filePath.lastIndexOf('/');
    String fileName = (lastSlash >= 0) ? filePath.substring(lastSlash + 1) : filePath;
    
    // 发送文件信息
    DynamicJsonDocument infoDoc(512);
    infoDoc["status"] = "success";
    infoDoc["action"] = "fileinfo";
    infoDoc["filename"] = fileName;
    infoDoc["size"] = fileSize;
    infoDoc["path"] = filePath;
    
    String infoResponse;
    serializeJson(infoDoc, infoResponse);
    mqtt.publish(response_topic.c_str(), infoResponse.c_str());
    
    // 分块发送文件数据
    const size_t bufferSize = 1024; // MQTT消息大小限制，可能需要调整
    uint8_t buffer[bufferSize];
    size_t bytesRead = 0;
    size_t totalSent = 0;
    int chunkIndex = 0;
    
    String data_topic = String(topic_data) + device_id + "/file";
    
    while ((bytesRead = fread(buffer, 1, bufferSize, f)) > 0) {
      // 为每个数据块创建一个Base64编码的消息
      String chunk = base64::encode(buffer, bytesRead);
      
      DynamicJsonDocument chunkDoc(chunk.length() + 256);
      chunkDoc["chunk"] = chunkIndex;
      chunkDoc["data"] = chunk;
      chunkDoc["size"] = bytesRead;
      chunkDoc["total"] = fileSize;
      chunkDoc["filename"] = fileName;
      
      if (totalSent + bytesRead >= fileSize) {
        chunkDoc["last"] = true;
      }
      
      String chunkMsg;
      serializeJson(chunkDoc, chunkMsg);
      
      mqtt.publish(data_topic.c_str(), chunkMsg.c_str());
      
      totalSent += bytesRead;
      chunkIndex++;
      
      // 短暂延迟，避免发送过快
      delay(100);
      
      // 定期向客户端报告进度
      if (chunkIndex % 10 == 0 || totalSent >= fileSize) {
        DynamicJsonDocument progressDoc(256);
        progressDoc["status"] = "progress";
        progressDoc["filename"] = fileName;
        progressDoc["sent"] = totalSent;
        progressDoc["total"] = fileSize;
        progressDoc["percent"] = (totalSent * 100) / fileSize;
        
        String progressResponse;
        serializeJson(progressDoc, progressResponse);
        mqtt.publish(response_topic.c_str(), progressResponse.c_str());
      }
    }
    
    fclose(f);
    
    // 发送完成消息
    DynamicJsonDocument completeDoc(256);
    completeDoc["status"] = "complete";
    completeDoc["filename"] = fileName;
    completeDoc["size"] = fileSize;
    
    String completeResponse;
    serializeJson(completeDoc, completeResponse);
    mqtt.publish(response_topic.c_str(), completeResponse.c_str());
  }
  // 添加一个新的MQTT命令处理函数来下载文件
  else if (command == "downloadfile") {
    String url = doc["url"];
    String savePath = doc["path"];
    
    Serial.println("下载URL: " + url);
    Serial.println("保存路径: " + savePath);
    
    if (url == "" || savePath == "") {
      DynamicJsonDocument errorDoc(256);
      errorDoc["status"] = "error";
      errorDoc["message"] = "URL或保存路径未指定";
      
      String errorResponse;
      serializeJson(errorDoc, errorResponse);
      mqtt.publish(response_topic.c_str(), errorResponse.c_str());
      return;
    }
    
    // 确保路径以/开头
    if (!savePath.startsWith("/")) {
      savePath = "/" + savePath;
    }
    
    String fullSavePath = MOUNT_POINT + savePath;
    Serial.println("完整保存路径: " + fullSavePath);
    
    // 发送开始下载消息
    DynamicJsonDocument startDoc(512);
    startDoc["status"] = "downloading";
    startDoc["url"] = url;
    startDoc["path"] = savePath;
    
    String startResponse;
    serializeJson(startDoc, startResponse);
    mqtt.publish(response_topic.c_str(), startResponse.c_str());
    
    // 创建HTTP客户端
    HTTPClient http;
    http.begin(url);
    
    // 发送HTTP请求
    int httpCode = http.GET();
    Serial.println("HTTP响应码: " + String(httpCode));
    
    if (httpCode > 0) {
      if (httpCode == HTTP_CODE_OK) {
        // 检查文件是否可写
        Serial.println("尝试打开文件进行写入: " + fullSavePath);
        
        // 先尝试打开父目录以检查是否存在和可写
        int lastSlash = fullSavePath.lastIndexOf('/');
        if (lastSlash > 0) {
          String dirPath = fullSavePath.substring(0, lastSlash);
          DIR* dir = opendir(dirPath.c_str());
          if (dir == NULL) {
            Serial.println("父目录不存在或无法访问: " + dirPath);
            mkdir(dirPath.c_str(), 0755);
            dir = opendir(dirPath.c_str());
            if (dir == NULL) {
              Serial.println("无法创建父目录");
            } else {
              closedir(dir);
            }
          } else {
            closedir(dir);
          }
        }
        
        FILE* f = fopen(fullSavePath.c_str(), "wb");
        
        if (!f) {
          Serial.println("无法创建文件: " + fullSavePath);
          Serial.println("errno: " + String(errno));
          
          DynamicJsonDocument errorDoc(256);
          errorDoc["status"] = "error";
          errorDoc["message"] = "无法创建文件";
          errorDoc["path"] = savePath;
          errorDoc["errno"] = errno;
          
          String errorResponse;
          serializeJson(errorDoc, errorResponse);
          mqtt.publish(response_topic.c_str(), errorResponse.c_str());
          http.end();
          return;
        }
        
        // 获取HTTP响应内容长度
        int contentLength = http.getSize();
        
        // 创建缓冲区
        uint8_t buffer[1024];
        WiFiClient* stream = http.getStreamPtr();
        
        // 读取HTTP响应并写入文件
        int totalRead = 0;
        int bytesRead = 0;
        
        while (http.connected() && (contentLength > 0 || contentLength == -1)) {
          // 读取数据
          size_t size = stream->available();
          
          if (size) {
            int c = stream->readBytes(buffer, ((size > sizeof(buffer)) ? sizeof(buffer) : size));
            
            // 写入文件
            fwrite(buffer, 1, c, f);
            
            totalRead += c;
            
            // 发送进度更新
            if (contentLength > 0) {
              DynamicJsonDocument progressDoc(256);
              progressDoc["status"] = "progress";
              progressDoc["url"] = url;
              progressDoc["path"] = savePath;
              progressDoc["downloaded"] = totalRead;
              progressDoc["total"] = contentLength;
              progressDoc["percent"] = (totalRead * 100) / contentLength;
              
              String progressResponse;
              serializeJson(progressDoc, progressResponse);
              mqtt.publish(response_topic.c_str(), progressResponse.c_str());
            }
            
            if (contentLength > 0) {
              contentLength -= c;
            }
          }
          delay(1);
        }
        
        fclose(f);
        
        // 发送下载完成消息
        DynamicJsonDocument completeDoc(512);
        completeDoc["status"] = "complete";
        completeDoc["url"] = url;
        completeDoc["path"] = savePath;
        completeDoc["size"] = totalRead;
        
        String completeResponse;
        serializeJson(completeDoc, completeResponse);
        mqtt.publish(response_topic.c_str(), completeResponse.c_str());
      } else {
        // HTTP请求失败
        DynamicJsonDocument errorDoc(256);
        errorDoc["status"] = "error";
        errorDoc["message"] = "HTTP请求失败，状态码: " + String(httpCode);
        
        String errorResponse;
        serializeJson(errorDoc, errorResponse);
        mqtt.publish(response_topic.c_str(), errorResponse.c_str());
      }
    } else {
      // 连接失败
      DynamicJsonDocument errorDoc(256);
      errorDoc["status"] = "error";
      errorDoc["message"] = "无法连接到服务器: " + http.errorToString(httpCode);
      
      String errorResponse;
      serializeJson(errorDoc, errorResponse);
      mqtt.publish(response_topic.c_str(), errorResponse.c_str());
    }
    
    http.end();
  }
  else if (command == "testwrite") {
    String savePath = doc["path"];
    String content = doc["content"];
    
    String fullPath = MOUNT_POINT + savePath;
    FILE* f = fopen(fullPath.c_str(), "w");
    
    if (!f) {
      DynamicJsonDocument errorDoc(256);
      errorDoc["status"] = "error";
      errorDoc["message"] = "无法创建测试文件";
      errorDoc["errno"] = errno;
      
      String errorResponse;
      serializeJson(errorDoc, errorResponse);
      mqtt.publish(response_topic.c_str(), errorResponse.c_str());
      return;
    }
    
    fwrite(content.c_str(), 1, content.length(), f);
    fclose(f);
    
    DynamicJsonDocument successDoc(256);
    successDoc["status"] = "success";
    successDoc["message"] = "测试文件写入成功";
    
    String successResponse;
    serializeJson(successDoc, successResponse);
    mqtt.publish(response_topic.c_str(), successResponse.c_str());
  }
}

// MQTT回调函数
void mqttCallback(char* topic, byte* payload, unsigned int length) {
  Serial.print("收到消息: [");
  Serial.print(topic);
  Serial.print("] ");
  
  String message;
  for (int i = 0; i < length; i++) {
    message += (char)payload[i];
  }
  
  Serial.println(message);
  
  // 处理命令
  handleMQTTCommand(message);
}

// 生成设备ID
String generateDeviceID() {
  uint64_t chipid = ESP.getEfuseMac(); // 获取ESP32的芯片ID
  String id = String((uint32_t)(chipid >> 32), HEX) + String((uint32_t)chipid, HEX);
  id.toUpperCase();
  return id;
}

// 处理配置页面的HTTP请求
void handleConfigPage() {
  String html = "<html><head>";
  html += "<title>ESP32 SD卡管理器配置</title>";
  html += "<meta charset='UTF-8'>";
  html += "<meta name='viewport' content='width=device-width, initial-scale=1.0'>";
  html += "<style>";
  html += "body { font-family: Arial, sans-serif; margin: 20px; background-color: #f5f5f5; }";
  html += "h1 { color: #333; text-align: center; margin: 20px 0; }";
  html += ".container { max-width: 600px; margin: 0 auto; background-color: white; padding: 20px; border-radius: 8px; box-shadow: 0 2px 10px rgba(0,0,0,0.1); }";
  html += ".form-group { margin-bottom: 15px; }";
  html += "label { display: block; margin-bottom: 5px; font-weight: bold; }";
  html += "input[type=text], input[type=password], input[type=number] { width: 100%; padding: 8px; box-sizing: border-box; border: 1px solid #ddd; border-radius: 4px; }";
  html += "button { background-color: #4CAF50; color: white; border: none; padding: 10px 15px; border-radius: 4px; cursor: pointer; }";
  html += "button:hover { background-color: #45a049; }";
  html += ".toggle-btn { background-color: #2196F3; margin-bottom: 10px; }";
  html += ".advanced { display: none; margin-top: 20px; }";
  html += "</style>";
  html += "</head><body>";
  html += "<div class='container'>";
  html += "<h1>ESP32 SD卡管理器配置</h1>";
  
  html += "<form action='/save' method='post'>";
  html += "<div class='form-group'>";
  html += "<label for='ssid'>WiFi名称:</label>";
  html += "<input type='text' id='ssid' name='ssid' value='" + String(wifiConfig.ssid) + "' required>";
  html += "</div>";
  
  html += "<div class='form-group'>";
  html += "<label for='password'>WiFi密码:</label>";
  html += "<input type='password' id='password' name='password' value='" + String(wifiConfig.password) + "' required>";
  html += "</div>";
  
  html += "<button type='button' class='toggle-btn' onclick='toggleAdvanced()'>高级选项</button>";
  
  html += "<div id='advanced' class='advanced'>";
  html += "<div class='form-group'>";
  html += "<label for='mqtt_server'>MQTT服务器:</label>";
  html += "<input type='text' id='mqtt_server' name='mqtt_server' value='" + String(wifiConfig.mqtt_server[0] ? wifiConfig.mqtt_server : default_mqtt_server) + "'>";
  html += "</div>";
  
  html += "<div class='form-group'>";
  html += "<label for='mqtt_port'>MQTT端口:</label>";
  html += "<input type='number' id='mqtt_port' name='mqtt_port' value='" + String(wifiConfig.mqtt_port ? wifiConfig.mqtt_port : default_mqtt_port) + "'>";
  html += "</div>";
  
  html += "<div class='form-group'>";
  html += "<label for='mqtt_user'>MQTT用户名:</label>";
  html += "<input type='text' id='mqtt_user' name='mqtt_user' value='" + String(wifiConfig.mqtt_user[0] ? wifiConfig.mqtt_user : default_mqtt_user) + "'>";
  html += "</div>";
  
  html += "<div class='form-group'>";
  html += "<label for='mqtt_password'>MQTT密码:</label>";
  html += "<input type='password' id='mqtt_password' name='mqtt_password' value='" + String(wifiConfig.mqtt_password[0] ? wifiConfig.mqtt_password : default_mqtt_password) + "'>";
  html += "</div>";
  html += "</div>";
  
  html += "<div class='form-group'>";
  html += "<button type='submit'>保存配置</button>";
  html += "</div>";
  html += "</form>";
  
  html += "<script>";
  html += "function toggleAdvanced() {";
  html += "  var advancedDiv = document.getElementById('advanced');";
  html += "  if (advancedDiv.style.display === 'block') {";
  html += "    advancedDiv.style.display = 'none';";
  html += "  } else {";
  html += "    advancedDiv.style.display = 'block';";
  html += "  }";
  html += "}";
  html += "</script>";
  
  html += "</div></body></html>";
  
  server.send(200, "text/html", html);
}

// 保存配置
void handleSaveConfig() {
  // 获取表单数据
  String ssid = server.arg("ssid");
  String password = server.arg("password");
  String mqtt_server = server.arg("mqtt_server");
  String mqtt_port_str = server.arg("mqtt_port");
  String mqtt_user = server.arg("mqtt_user");
  String mqtt_password = server.arg("mqtt_password");
  
  int mqtt_port = mqtt_port_str.toInt();
  
  // 保存到配置结构
  strncpy(wifiConfig.ssid, ssid.c_str(), sizeof(wifiConfig.ssid) - 1);
  strncpy(wifiConfig.password, password.c_str(), sizeof(wifiConfig.password) - 1);
  
  if (mqtt_server.length() > 0) {
    strncpy(wifiConfig.mqtt_server, mqtt_server.c_str(), sizeof(wifiConfig.mqtt_server) - 1);
  } else {
    strncpy(wifiConfig.mqtt_server, default_mqtt_server, sizeof(wifiConfig.mqtt_server) - 1);
  }
  
  if (mqtt_port > 0) {
    wifiConfig.mqtt_port = mqtt_port;
  } else {
    wifiConfig.mqtt_port = default_mqtt_port;
  }
  
  if (mqtt_user.length() > 0) {
    strncpy(wifiConfig.mqtt_user, mqtt_user.c_str(), sizeof(wifiConfig.mqtt_user) - 1);
  } else {
    strncpy(wifiConfig.mqtt_user, default_mqtt_user, sizeof(wifiConfig.mqtt_user) - 1);
  }
  
  if (mqtt_password.length() > 0) {
    strncpy(wifiConfig.mqtt_password, mqtt_password.c_str(), sizeof(wifiConfig.mqtt_password) - 1);
  } else {
    strncpy(wifiConfig.mqtt_password, default_mqtt_password, sizeof(wifiConfig.mqtt_password) - 1);
  }
  
  wifiConfig.configured = true;
  
  // 保存配置
  saveConfiguration();
  
  // 响应保存成功页面
  String html = "<html><head>";
  html += "<title>配置已保存</title>";
  html += "<meta charset='UTF-8'>";
  html += "<meta name='viewport' content='width=device-width, initial-scale=1.0'>";
  html += "<style>";
  html += "body { font-family: Arial, sans-serif; margin: 20px; background-color: #f5f5f5; text-align: center; }";
  html += ".container { max-width: 600px; margin: 0 auto; background-color: white; padding: 20px; border-radius: 8px; box-shadow: 0 2px 10px rgba(0,0,0,0.1); }";
  html += "h1 { color: #4CAF50; }";
  html += "p { margin: 20px 0; }";
  html += ".btn { background-color: #2196F3; color: white; border: none; padding: 10px 15px; border-radius: 4px; text-decoration: none; display: inline-block; margin-top: 20px; }";
  html += "</style>";
  html += "</head><body>";
  html += "<div class='container'>";
  html += "<h1>配置已保存</h1>";
  html += "<p>设备将在5秒后重启并尝试连接到WiFi网络。</p>";
  html += "<p>如果连接成功，设备将自动启动并连接到MQTT服务器。</p>";
  html += "<p>您可以按照先前的说明使用MQTT客户端连接到设备。</p>";
  html += "</div>";
  html += "<script>";
  html += "setTimeout(function() { window.location.href = '/'; }, 10000);";
  html += "</script>";
  html += "</body></html>";
  
  server.send(200, "text/html", html);
  
  // 延迟几秒后重启
  delay(5000);
  ESP.restart();
}

// 处理本地网页根目录请求
void handleRoot() {
  // 如果在配置模式下，显示配置页面
  if (configMode) {
    handleConfigPage();
    return;
  }
  
  String html = "<html><head>";
  html += "<title>ESP32 SD卡文件管理器</title>";
  html += "<meta charset='UTF-8'>";
  html += "<meta name='viewport' content='width=device-width, initial-scale=1.0'>";
  html += "<style>";
  html += "body { font-family: Arial, sans-serif; margin: 20px; background-color: #f5f5f5; }";
  html += "h1 { color: #333; text-align: center; margin: 20px 0; }";
  html += ".container { max-width: 1000px; margin: 0 auto; background-color: white; padding: 20px; border-radius: 8px; box-shadow: 0 2px 10px rgba(0,0,0,0.1); }";
  html += ".info-box { background-color: #e9f7ef; padding: 15px; border-radius: 8px; margin-bottom: 20px; border: 1px solid #d5f5e3; }";
  html += ".config-box { background-color: #fff8dc; padding: 15px; border-radius: 8px; margin-bottom: 20px; border: 1px solid #ffeb3b; }";
  html += "code { background-color: #f8f9fa; padding: 4px 8px; border-radius: 4px; font-family: monospace; }";
  html += "pre { background-color: #f8f9fa; padding: 15px; border-radius: 8px; overflow-x: auto; }";
  html += "h2 { color: #2c3e50; margin-top: 30px; }";
  html += ".footer { text-align: center; margin-top: 20px; font-size: 0.8em; color: #777; }";
  html += ".btn { background-color: #2196F3; color: white; border: none; padding: 10px 15px; border-radius: 4px; text-decoration: none; display: inline-block; }";
  html += "</style>";
  html += "</head><body>";
  html += "<div class='container'>";
  html += "<h1>ESP32 SD卡远程文件管理器</h1>";
  
  html += "<div class='config-box'>";
  html += "<h2>配置状态</h2>";
  html += "<p><strong>WiFi网络:</strong> " + String(wifiConfig.ssid) + "</p>";
  html += "<p><strong>MQTT服务器:</strong> " + String(wifiConfig.mqtt_server) + ":" + String(wifiConfig.mqtt_port) + "</p>";
  html += "<p><a href='/config' class='btn'>修改配置</a></p>";
  html += "</div>";
  
  html += "<div class='info-box'>";
  html += "<h2>远程访问信息</h2>";
  html += "<p>本设备已配置为通过MQTT进行远程访问。请使用MQTT客户端连接以下信息：</p>";
  html += "<ul>";
  html += "<li><strong>MQTT服务器:</strong> " + String(wifiConfig.mqtt_server) + ":" + String(wifiConfig.mqtt_port) + "</li>";
  html += "<li><strong>设备ID:</strong> " + device_id + "</li>";
  html += "<li><strong>命令主题:</strong> " + String(topic_command) + device_id + "</li>";
  html += "<li><strong>响应主题:</strong> " + String(topic_response) + device_id + "</li>";
  html += "<li><strong>数据主题:</strong> " + String(topic_data) + device_id + "/file</li>";
  html += "</ul>";
  html += "</div>";
  
  html += "<h2>命令格式示例</h2>";
  html += "<h3>列出目录内容:</h3>";
  html += "<pre>{\"command\": \"listdir\", \"path\": \"/\"}</pre>";
  
  html += "<h3>获取文件:</h3>";
  html += "<pre>{\"command\": \"getfile\", \"path\": \"/example.txt\"}</pre>";
  
  html += "<h2>本地网络信息</h2>";
  html += "<p>设备IP地址: " + WiFi.localIP().toString() + "</p>";
  html += "<p>已连接到WiFi: " + String(wifiConfig.ssid) + "</p>";
  
  html += "<h2>请使用MQTT客户端进行远程访问</h2>";
  html += "<p>本页面仅提供设备状态和使用信息。</p>";
  
  html += "<div class='footer'>ESP32 SD卡远程文件管理器 | 设备ID: " + device_id + "</div>";
  html += "</div></body></html>";
  
  server.send(200, "text/html; charset=utf-8", html);
}

// 检查配置按钮状态
void checkConfigButton() {
  // 如果配置按钮被按下（考虑到GPIO0是低电平触发）
  if (digitalRead(configButtonPin) == LOW) {
    delay(50); // 去抖动
    if (digitalRead(configButtonPin) == LOW) {
      // 等待按钮释放或超时
      unsigned long pressStart = millis();
      while (digitalRead(configButtonPin) == LOW && millis() - pressStart < 3000) {
        delay(10);
      }
      
      // 如果按下时间超过3秒
      if (millis() - pressStart >= 3000) {
        Serial.println("长按配置按钮，进入配置模式");
        enterConfigMode();
      }
    }
  }
}

// 设置Web服务器路由
void setupWebServer() {
  server.on("/", HTTP_GET, handleRoot);
  server.on("/config", HTTP_GET, handleConfigPage);
  server.on("/save", HTTP_POST, handleSaveConfig);
  
  server.begin();
  Serial.println("Web服务器已启动");
}

void setup() {
  Serial.begin(115200);
  delay(2000);

  // 初始化LED和配置按钮
  pinMode(ledPin, OUTPUT);
  pinMode(configButtonPin, INPUT_PULLUP);

  // LED快速闪烁表示启动
  for (int i = 0; i < 5; i++) {
    digitalWrite(ledPin, HIGH);
    delay(100);
    digitalWrite(ledPin, LOW);
    delay(100);
  }

  Serial.println("\n\n============================");
  Serial.println("ESP32 SD卡远程文件管理器");
  Serial.println("============================");

  // 生成设备ID
  device_id = generateDeviceID();
  Serial.print("设备ID: ");
  Serial.println(device_id);

  // 初始化EEPROM
  EEPROM.begin(EEPROM_SIZE);

  // 初始化SD卡
  if (!initSDCard()) {
    Serial.println("SD卡初始化失败，继续运行但SD卡功能将不可用");
    // 不再返回，而是继续执行以便用户可以配置设备
  }
  
  // 检查配置按钮
  checkConfigButton();
  
  // 如果配置按钮没有触发配置模式，尝试从EEPROM加载配置
  if (!configMode) {
    if (loadConfiguration()) {
      // 连接WiFi
      connectToWiFi();
      
      // 如果WiFi连接成功而且不在配置模式
      if (WiFi.status() == WL_CONNECTED && !configMode) {
        // 设置MQTT服务器
        mqtt.setServer(wifiConfig.mqtt_server, wifiConfig.mqtt_port);
        mqtt.setCallback(mqttCallback);
        
        // 连接到MQTT服务器
        reconnectMQTT();
      }
    } else {
      // 如果没有配置，进入配置模式
      Serial.println("未找到有效配置，进入配置模式");
      enterConfigMode();
    }
  }
  
  // 设置Web服务器
  setupWebServer();
  
  Serial.println("系统已就绪");
  if (configMode) {
    Serial.println("设备处于配置模式，请连接到AP: " + String(ap_ssid));
    Serial.println("然后访问 http://192.168.4.1 进行配置");
  } else {
    Serial.println("使用MQTT客户端连接以访问文件系统");
  }
}

void loop() {
  // 处理Web服务器请求
  server.handleClient();
  
  // 如果在正常运行模式
  if (!configMode) {
    // 检查WiFi连接状态
    if (WiFi.status() != WL_CONNECTED) {
      Serial.println("WiFi连接断开，尝试重新连接...");
      connectToWiFi();
      
      // 如果重连失败，可以选择进入配置模式
      if (WiFi.status() != WL_CONNECTED) {
        Serial.println("WiFi重连失败，进入配置模式");
        enterConfigMode();
      }
    }
    
    // 检查MQTT连接状态
    if (!mqtt.connected() && WiFi.status() == WL_CONNECTED) {
      Serial.println("MQTT连接断开，重新连接...");
      reconnectMQTT();
    }
    
    // 处理MQTT消息
    if (mqtt.connected()) {
      mqtt.loop();
    }
  }
  
  // 检查配置按钮
  checkConfigButton();
  
  delay(10);
}