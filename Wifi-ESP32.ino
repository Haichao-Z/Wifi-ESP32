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

#define MOUNT_POINT "/sdcard"

// 定义SD_OCR_SDHC_CAP
#ifndef SD_OCR_SDHC_CAP
#define SD_OCR_SDHC_CAP (1ULL << 30)
#endif

// WiFi凭据
const char* wifi_ssid = "hai";        // WiFi名称
const char* wifi_password = "99999999"; // WiFi密码

// MQTT服务器设置
const char* mqtt_server = "135.119.219.218"; // MQTT服务器
const int mqtt_port = 1883;
const char* mqtt_user = "rttyobej";          // MQTT用户名
const char* mqtt_password = "BrsJBNVoQBl7";      // MQTT密码
const char* client_id = "ESP32_SDCard_Manager"; // MQTT客户端ID

// MQTT主题
String device_id = ""; // 设备唯一标识符，将在setup中生成
const char* topic_command = "sdcard/command/";  // 接收命令的主题
const char* topic_response = "sdcard/response/"; // 发送响应的主题
const char* topic_data = "sdcard/data/";       // 发送数据的主题

// 创建WiFi和MQTT客户端实例
WiFiClient espClient;
PubSubClient mqtt(espClient);

// Web服务器（仅用于本地访问）
WebServer server(80);

// 当前浏览目录
String currentDir = "/";

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
  // 循环直到连接成功
  while (!mqtt.connected()) {
    Serial.print("尝试连接MQTT服务器...");
    
    // 尝试连接
    if (mqtt.connect(client_id, mqtt_user, mqtt_password)) {
      Serial.println("已连接");
      
      // 订阅命令主题
      String full_command_topic = String(topic_command) + device_id;
      mqtt.subscribe(full_command_topic.c_str());
      Serial.printf("已订阅主题: %s\n", full_command_topic.c_str());
      
      // 发布设备上线消息
      String status_topic = String(topic_response) + device_id + "/status";
      mqtt.publish(status_topic.c_str(), "online");
    } else {
      Serial.print("连接失败，rc=");
      Serial.print(mqtt.state());
      Serial.println(" 5秒后重试...");
      delay(5000);
    }
  }
}

// 连接WiFi网络
void setupWiFi() {
  Serial.println("连接到WiFi网络...");
  WiFi.begin(wifi_ssid, wifi_password);
  
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  
  Serial.println("");
  Serial.println("WiFi已连接");
  Serial.print("IP地址: ");
  Serial.println(WiFi.localIP());
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
  DynamicJsonDocument doc(4096);
  DeserializationError error = deserializeJson(doc, payload);
  
  if (error) {
    Serial.print("JSON解析失败: ");
    Serial.println(error.c_str());
    return;
  }
  
  String command = doc["command"];
  String response_topic = String(topic_response) + device_id;
  
  if (command == "listdir") {
    String path = doc["path"];
    if (path == "") path = "/";
    
    currentDir = path;
    String dirList = getDirListJSON(path);
    mqtt.publish(response_topic.c_str(), dirList.c_str());
    
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

// 处理本地网页根目录请求
void handleRoot() {
  String html = "<html><head>";
  html += "<title>ESP32 SD卡文件管理器</title>";
  html += "<meta charset='UTF-8'>";
  html += "<meta name='viewport' content='width=device-width, initial-scale=1.0'>";
  html += "<style>";
  html += "body { font-family: Arial, sans-serif; margin: 20px; background-color: #f5f5f5; }";
  html += "h1 { color: #333; text-align: center; margin: 20px 0; }";
  html += ".container { max-width: 1000px; margin: 0 auto; background-color: white; padding: 20px; border-radius: 8px; box-shadow: 0 2px 10px rgba(0,0,0,0.1); }";
  html += ".info-box { background-color: #e9f7ef; padding: 15px; border-radius: 8px; margin-bottom: 20px; border: 1px solid #d5f5e3; }";
  html += "code { background-color: #f8f9fa; padding: 4px 8px; border-radius: 4px; font-family: monospace; }";
  html += "pre { background-color: #f8f9fa; padding: 15px; border-radius: 8px; overflow-x: auto; }";
  html += "h2 { color: #2c3e50; margin-top: 30px; }";
  html += ".footer { text-align: center; margin-top: 20px; font-size: 0.8em; color: #777; }";
  html += "</style>";
  html += "</head><body>";
  html += "<div class='container'>";
  html += "<h1>ESP32 SD卡远程文件管理器</h1>";
  
  html += "<div class='info-box'>";
  html += "<h2>远程访问信息</h2>";
  html += "<p>本设备已配置为通过MQTT进行远程访问。请使用MQTT客户端连接以下信息：</p>";
  html += "<ul>";
  html += "<li><strong>MQTT服务器:</strong> " + String(mqtt_server) + ":" + String(mqtt_port) + "</li>";
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
  html += "<p>已连接到WiFi: " + String(wifi_ssid) + "</p>";
  
  html += "<h2>请使用MQTT客户端进行远程访问</h2>";
  html += "<p>本页面仅提供设备状态和使用信息。</p>";
  
  html += "<div class='footer'>ESP32 SD卡远程文件管理器 | 设备ID: " + device_id + "</div>";
  html += "</div></body></html>";
  
  server.send(200, "text/html; charset=utf-8", html);
}

// 设置Web服务器（仅用于本地访问）
void setupWebServer() {
  server.on("/", HTTP_GET, handleRoot);
  
  server.begin();
  Serial.println("Web服务器已启动（仅用于本地信息显示）");
}

void setup() {
  Serial.begin(115200);
  delay(2000);

  Serial.println("\n\n============================");
  Serial.println("ESP32 SD卡远程文件管理器");
  Serial.println("============================");

  // 生成设备ID
  device_id = generateDeviceID();
  Serial.print("设备ID: ");
  Serial.println(device_id);

  // 初始化SD卡
  if (!initSDCard()) {
    Serial.println("SD卡初始化失败，停止执行");
    return;
  }
  
  // 连接WiFi
  setupWiFi();
  
  // 设置MQTT服务器
  mqtt.setServer(mqtt_server, mqtt_port);
  mqtt.setCallback(mqttCallback);
  
  // 连接到MQTT服务器
  reconnectMQTT();
  
  // 设置本地Web服务器
  setupWebServer();
  
  Serial.println("系统已就绪");
  Serial.println("使用MQTT客户端连接以访问文件系统");
}

void loop() {
  // 检查WiFi连接状态
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("WiFi连接断开，重新连接...");
    setupWiFi();
  }
  
  // 检查MQTT连接状态
  if (!mqtt.connected()) {
    Serial.println("MQTT连接断开，重新连接...");
    reconnectMQTT();
  }
  
  // 处理MQTT消息
  mqtt.loop();
  
  // 处理本地Web服务器请求
  server.handleClient();
  
  delay(10);
}