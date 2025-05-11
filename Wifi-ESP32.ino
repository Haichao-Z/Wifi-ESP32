#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <WebServer.h>  // 添加WebServer库
#include <stdio.h>
#include <string.h>
#include <dirent.h>
#include <sys/unistd.h>
#include <sys/stat.h>
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "driver/sdmmc_host.h"

#define MOUNT_POINT "/sdcard"

// 定义SD_OCR_SDHC_CAP（如果需要）
#ifndef SD_OCR_SDHC_CAP
#define SD_OCR_SDHC_CAP (1ULL << 30)
#endif

// WiFi凭据
const char* ssid = "hai";       // 替换为你的WiFi名称
const char* password = "99999999"; // 替换为你的WiFi密码

// 服务器信息
const char* serverIP = "135.119.219.218";
const int serverPort = 80;  // 如果不是标准HTTP端口，请修改

// 文件下载信息
const char* remoteFilePath = "/test.txt"; // 替换为你要下载的文件路径
const char* localFilePath = MOUNT_POINT "/test/downloaded_file.txt"; // SD卡上的保存路径

// 创建Web服务器对象，使用端口80
WebServer server(80);

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

// 下载文件到SD卡
bool downloadFile(const char* url, const char* filePath) {
  HTTPClient http;
  
  Serial.printf("开始下载文件: %s\n", url);
  Serial.printf("存储到: %s\n", filePath);
  
  // 开始HTTP会话
  http.begin(url);
  
  // 发送GET请求
  int httpCode = http.GET();
  
  if (httpCode == HTTP_CODE_OK) {
    // 如果请求成功
    Serial.printf("HTTP请求成功，状态码: %d\n", httpCode);
    
    // 获取响应长度
    int contentLength = http.getSize();
    Serial.printf("文件大小: %d 字节\n", contentLength);
    
    // 创建文件
    FILE* f = fopen(filePath, "w");
    if (f == NULL) {
      Serial.println("无法创建文件");
      http.end();
      return false;
    }
    
    // 创建缓冲区
    uint8_t buffer[1024];
    WiFiClient* stream = http.getStreamPtr();
    
    // 读取所有数据并写入文件
    int bytesRead = 0;
    int totalBytesRead = 0;
    
    while (http.connected() && (totalBytesRead < contentLength)) {
      // 读取可用数据
      size_t size = stream->available();
      if (size) {
        int c = stream->readBytes(buffer, ((size > sizeof(buffer)) ? sizeof(buffer) : size));
        
        // 写入文件
        fwrite(buffer, 1, c, f);
        
        bytesRead = c;
        totalBytesRead += c;
        
        // 显示下载进度
        if (contentLength > 0) {
          Serial.printf("下载进度: %d%%\n", (totalBytesRead * 100) / contentLength);
        }
      }
      delay(1); // 给服务器一些处理时间
    }
    
    // 关闭文件
    fclose(f);
    
    Serial.printf("文件下载完成，共 %d 字节\n", totalBytesRead);
    http.end();
    return true;
  } else {
    Serial.printf("HTTP请求失败，错误代码: %d\n", httpCode);
    http.end();
    return false;
  }
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

// 处理文件下载请求 - 改进版，修复下载编码问题
void handleDownload() {
  String path = server.arg("file");
  if (path == "") {
    server.send(400, "text/plain; charset=utf-8", "缺少文件路径参数");
    return;
  }
  
  String fullPath = MOUNT_POINT + path;
  Serial.printf("请求下载文件: %s\n", fullPath.c_str());
  
  // 检查文件是否存在
  FILE* f = fopen(fullPath.c_str(), "r");
  if (!f) {
    server.send(404, "text/plain; charset=utf-8", "文件不存在");
    return;
  }
  
  // 获取文件大小
  struct stat stat_buf;
  stat(fullPath.c_str(), &stat_buf);
  size_t fileSize = stat_buf.st_size;
  
  // 获取文件名（用于Content-Disposition头）
  int lastSlash = path.lastIndexOf('/');
  String fileName = (lastSlash >= 0) ? path.substring(lastSlash + 1) : path;
  
  // 直接使用WiFiClient发送HTTP头和文件内容
  WiFiClient client = server.client();
  
  // 发送自定义HTTP头
  client.println("HTTP/1.1 200 OK");
  client.println("Content-Type: application/octet-stream");
  
  // 使用双引号包围文件名，并处理URL编码
  client.print("Content-Disposition: attachment; filename=\"");
  client.print(fileName);
  client.println("\"");
  
  client.print("Content-Length: ");
  client.println(fileSize);
  client.println("Connection: close");
  client.println("Cache-Control: no-cache, no-store, must-revalidate");
  client.println("Pragma: no-cache");
  client.println("Expires: 0");
  client.println(); // 空行标记头部结束
  
  // 使用更大的缓冲区提高性能
  uint8_t buffer[2048];
  size_t bytesRead = 0;
  size_t totalSent = 0;
  
  while ((bytesRead = fread(buffer, 1, sizeof(buffer), f)) > 0) {
    if (client.write(buffer, bytesRead) != bytesRead) {
      Serial.println("发送数据失败");
      break;
    }
    totalSent += bytesRead;
    
    // 显示传输进度
    if (fileSize > 0 && totalSent % 10240 == 0) {
      Serial.printf("下载进度: %d%%\n", (totalSent * 100) / fileSize);
    }
    
    // 给其他任务一些运行时间
    yield();
  }
  
  fclose(f);
  
  // 等待数据发送完成
  delay(1);
  client.stop();
  
  Serial.printf("文件传输完成，已发送 %u 字节\n", totalSent);
}

// 处理根目录请求，显示SD卡目录内容 - 增强版
void handleRoot() {
  String html = "<html><head>";
  html += "<title>ESP32 SD卡文件列表</title>";
  html += "<meta charset='UTF-8'>";
  html += "<meta name='viewport' content='width=device-width, initial-scale=1.0'>";
  html += "<style>";
  html += "body { font-family: Arial, sans-serif; margin: 20px; background-color: #f5f5f5; }";
  html += "h1 { color: #333; text-align: center; margin: 20px 0; }";
  html += ".container { max-width: 1000px; margin: 0 auto; background-color: white; padding: 20px; border-radius: 8px; box-shadow: 0 2px 10px rgba(0,0,0,0.1); }";
  html += "table { border-collapse: collapse; width: 100%; margin-top: 20px; }";
  html += "th, td { text-align: left; padding: 12px; border-bottom: 1px solid #ddd; }";
  html += "tr:hover { background-color: #f9f9f9; }";
  html += "th { background-color: #4CAF50; color: white; position: sticky; top: 0; }";
  html += "a { text-decoration: none; color: #0066cc; }";
  html += "a:hover { text-decoration: underline; }";
  html += ".download { background-color: #4CAF50; color: white; padding: 6px 12px; border-radius: 4px; display: inline-block; }";
  html += ".download:hover { background-color: #45a049; text-decoration: none; }";
  html += ".back { margin-bottom: 15px; display: inline-block; background-color: #f1f1f1; padding: 8px 15px; border-radius: 4px; color: #333; }";
  html += ".back:hover { background-color: #ddd; text-decoration: none; }";
  html += ".path { background-color: #e9e9e9; padding: 10px; border-radius: 4px; margin-bottom: 15px; }";
  html += ".footer { text-align: center; margin-top: 20px; font-size: 0.8em; color: #777; }";
  html += "</style>";
  html += "</head><body>";
  html += "<div class='container'>";
  html += "<h1>ESP32 SD卡文件系统</h1>";
  
  // 获取查询参数，如果有的话
  String path = server.arg("dir");
  if (path == "") {
    path = "/";
  }
  
  String fullPath;
  if (path == "/") {
    fullPath = MOUNT_POINT;
    html += "<div class='path'>当前路径: <strong>/</strong></div>";
  } else {
    fullPath = MOUNT_POINT + path;
    html += "<div class='path'>当前路径: <strong>" + path + "</strong></div>";
    
    // 添加返回上级目录链接
    int lastSlash = path.lastIndexOf('/');
    if (lastSlash > 0) {
      String parentPath = path.substring(0, lastSlash);
      html += "<a href='/?dir=" + parentPath + "' class='back'>← 返回上级目录</a>";
    } else {
      html += "<a href='/' class='back'>← 返回根目录</a>";
    }
  }
  
  html += "<table><tr><th>名称</th><th>类型</th><th>大小</th><th>操作</th></tr>";
  
  DIR* dir = opendir(fullPath.c_str());
  if (dir == NULL) {
    html += "<tr><td colspan='4'>无法打开目录</td></tr>";
  } else {
    struct dirent* entry;
    bool hasEntries = false;
    
    while ((entry = readdir(dir)) != NULL) {
      String entryName = String(entry->d_name);
      
      // 跳过.和..条目
      if (entryName == "." || entryName == "..") {
        continue;
      }
      
      hasEntries = true;
      String entryPath = fullPath + "/" + entryName;
      
      struct stat entry_stat;
      if (stat(entryPath.c_str(), &entry_stat) == -1) {
        continue;
      }
      
      String fileURL = path;
      if (path.endsWith("/")) {
        fileURL += entryName;
      } else {
        fileURL += "/" + entryName;
      }
      
      html += "<tr>";
      
      if (S_ISDIR(entry_stat.st_mode)) {
        // 如果是目录
        html += "<td><a href='/?dir=" + fileURL + "'>" + entryName + "/</a></td>";
        html += "<td>目录</td>";
        html += "<td>-</td>";
        html += "<td>-</td>";
      } else {
        // 如果是文件
        html += "<td>" + entryName + "</td>";
        html += "<td>文件</td>";
        html += "<td>" + formatBytes(entry_stat.st_size) + "</td>";
        html += "<td><a href='/download?file=" + fileURL + "' class='download'>下载</a></td>";
      }
      
      html += "</tr>";
    }
    
    if (!hasEntries) {
      html += "<tr><td colspan='4'><em>目录为空</em></td></tr>";
    }
    
    closedir(dir);
  }
  
  html += "</table>";
  html += "<div class='footer'>ESP32 SD卡文件管理器 | IP: " + WiFi.localIP().toString() + "</div>";
  html += "</div></body></html>";
  
  server.send(200, "text/html; charset=utf-8", html);
}

// 处理404错误 - 美化版
void handleNotFound() {
  String html = "<html><head>";
  html += "<title>404 未找到</title>";
  html += "<meta charset='UTF-8'>";
  html += "<meta name='viewport' content='width=device-width, initial-scale=1.0'>";
  html += "<style>";
  html += "body { font-family: Arial, sans-serif; margin: 0; padding: 0; display: flex; justify-content: center; align-items: center; height: 100vh; background-color: #f5f5f5; }";
  html += ".error-container { text-align: center; background: white; padding: 40px; border-radius: 8px; box-shadow: 0 2px 10px rgba(0,0,0,0.1); max-width: 500px; }";
  html += "h1 { color: #e74c3c; font-size: 60px; margin: 0; }";
  html += "h2 { color: #333; font-size: 24px; margin-top: 10px; }";
  html += "p { color: #666; margin: 20px 0; }";
  html += ".back-btn { display: inline-block; background-color: #4CAF50; color: white; padding: 10px 20px; text-decoration: none; border-radius: 4px; }";
  html += ".back-btn:hover { background-color: #45a049; }";
  html += "</style>";
  html += "</head><body>";
  html += "<div class='error-container'>";
  html += "<h1>404</h1>";
  html += "<h2>找不到请求的资源</h2>";
  html += "<p>您请求的页面或资源不存在。</p>";
  html += "<a href='/' class='back-btn'>返回首页</a>";
  html += "</div></body></html>";
  
  server.send(404, "text/html; charset=utf-8", html);
}

// 启动Web服务器
void startWebServer() {
  // 设置路由处理程序
  server.on("/", HTTP_GET, handleRoot);
  server.on("/download", HTTP_GET, handleDownload);
  server.onNotFound(handleNotFound);
  
  // 启动服务器
  server.begin();
  Serial.println("Web服务器已启动");
  Serial.println("在浏览器中访问 http://" + WiFi.localIP().toString() + "/ 查看SD卡内容");
}

void setup() {
  Serial.begin(115200);
  delay(2000);

  Serial.println("\n\n============================");
  Serial.println("ESP32 SD卡Web服务器");
  Serial.println("============================");

  // 初始化SD卡
  if (!initSDCard()) {
    Serial.println("SD卡初始化失败，停止执行");
    return;
  }
  
  // 连接WiFi
  Serial.printf("连接到WiFi网络: %s\n", ssid);
  WiFi.begin(ssid, password);
  
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  
  Serial.println("");
  Serial.println("WiFi已连接");
  Serial.print("IP地址: ");
  Serial.println(WiFi.localIP());
  
  // 构建完整的URL
  char url[100];
  sprintf(url, "http://%s%s", serverIP, remoteFilePath);
  
  // 下载文件
  if (downloadFile(url, localFilePath)) {
    Serial.println("文件下载并保存到SD卡成功");
    
    // 尝试读取下载的文件，显示其内容（可选）
    FILE* f = fopen(localFilePath, "r");
    if (f) {
      Serial.println("文件内容预览:");
      
      char buffer[101]; // 100字符 + 终止符
      size_t bytesRead = fread(buffer, 1, 100, f); // 只读取前100字节
      buffer[bytesRead] = 0; // 确保字符串正确终止
      
      Serial.println(buffer);
      Serial.println("...");
      
      fclose(f);
    }
  } else {
    Serial.println("文件下载失败");
  }
  
  // 启动Web服务器
  startWebServer();
}

void loop() {
  // 处理Web服务器客户端请求
  server.handleClient();
  delay(2); // 让WiFi处理一下
}