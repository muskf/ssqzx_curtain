#include <ESP8266WiFi.h>
#include <ESP8266HTTPClient.h>
#include <WiFiClient.h>
#include <ArduinoJson.h>
#include <RCSwitch.h>

const char* ssid = "SSQZX-B";
const char* password = "ssqzx888";
const char* serverUrl = "http://hostname/api/status";
const char* logUrl = "http://hostname/api/log";
const char* deviceStatusUrl = "http://hostname/api/device-status";

// 433MHz configuration
RCSwitch mySwitch = RCSwitch();
const unsigned long CMD_UP = 1529603;      // 上升
const unsigned long CMD_DOWN = 1529792;    // 下降  
const unsigned long CMD_LOCK = 1529612;    // 锁门
const unsigned long CMD_PAUSE = 1529648;   // 暂停/解锁

// Hardware pins
const int TRANSMIT_PIN = D1;    // 发送模块DATA引脚
const int RECEIVE_PIN = D2;     // 接收模块DATA引脚
const int STATUS_LED = LED_BUILTIN;

const unsigned long STATUS_CHECK_INTERVAL = 3000; // 3秒检查服务器命令
const unsigned long HEARTBEAT_INTERVAL = 30000;   // 30秒发送一次心跳
const unsigned long SIGNAL_REPEAT_DELAY = 20;     // 信号重复间隔
const unsigned long LED_BLINK_DELAY = 200;        // LED闪烁延迟
const unsigned long STATE_CHANGE_DELAY = 2000;    // 状态变化后等待时间（模拟运动完成）

String currentStatus = "closed";  // closed, open, locked, stopped, moving_up, moving_down
unsigned long lastStatusCheck = 0;
unsigned long lastHeartbeat = 0;
unsigned long lastStateChange = 0;  // Track last state change for delays
bool heartbeatWorking = false;

void setup() {
  Serial.begin(115200);
  
  // Initialize 433MHz module
  mySwitch.enableTransmit(TRANSMIT_PIN);  // 发送引脚
  mySwitch.enableReceive(RECEIVE_PIN);    // 接收引脚
  mySwitch.setProtocol(1);                // 设置协议（根据信号）
  mySwitch.setPulseLength(350);           // 脉冲长度（需要调整）
  
  // Initialize LED
  pinMode(STATUS_LED, OUTPUT);
  digitalWrite(STATUS_LED, HIGH);
  
  // Connect to WiFi
  WiFi.begin(ssid, password);
  Serial.print("🔌 连接WiFi");
  while (WiFi.status() != WL_CONNECTED) {
    delay(1000);
    Serial.print(".");
    digitalWrite(STATUS_LED, !digitalRead(STATUS_LED)); // Blink LED
  }
  Serial.println("\n✅ WiFi连接成功!");
  Serial.print("📡 IP地址: ");
  Serial.println(WiFi.localIP());
  
  digitalWrite(STATUS_LED, LOW);
  
  sendDeviceStatus("closed", WiFi.localIP().toString());
  sendLog("closed", "设备上线 - 默认状态: 关闭");
  
  Serial.println("🛡️ 系统就绪 - 等待命令...");
  Serial.println("可用命令: up, down, stop, lock");
  heartbeatWorking = true;
}

void loop() {
  check433Receiver();
  
  if (millis() - lastStatusCheck > STATUS_CHECK_INTERVAL) {
    checkServerCommands();
    lastStatusCheck = millis();
  }
  
  if (millis() - lastHeartbeat > HEARTBEAT_INTERVAL) {
    sendHeartbeat();
    lastHeartbeat = millis();
  }
  
  if (currentStatus.indexOf("moving") >= 0 && millis() - lastStateChange > STATE_CHANGE_DELAY) {
    if (currentStatus == "moving_up") {
      currentStatus = "open";
    } else if (currentStatus == "moving_down") {
      currentStatus = "closed";
    }
    sendDeviceStatus(currentStatus, WiFi.localIP().toString());
    sendLog(currentStatus, String("状态自动更新: ") + currentStatus);
    lastStateChange = millis();
  }
  
  delay(50);
}

void check433Receiver() {
  if (mySwitch.available()) {
    unsigned long value = mySwitch.getReceivedValue();
    unsigned int bitLength = mySwitch.getReceivedBitlength();
    unsigned int protocol = mySwitch.getReceivedProtocol();
    
    Serial.println("=== 📡 接收到433MHz信号 ===");
    Serial.print("数值: ");
    Serial.println(value);
    Serial.print("比特长度: ");
    Serial.println(bitLength);
    Serial.print("协议: ");
    Serial.println(protocol);
    
    String command = "";
    String message = "";
    
    if (value == CMD_UP) {
      command = "up";
      message = "遥控器 - 卷帘门上升";
    } else if (value == CMD_DOWN) {
      command = "down";
      message = "遥控器 - 卷帘门下降";
    } else if (value == CMD_LOCK) {
      command = "lock";
      message = "遥控器 - 卷帘门锁定";
    } else if (value == CMD_PAUSE) {
      command = "stop";
      message = "遥控器 - 卷帘门停止";
    } else {
      command = "unknown";
      message = "未知遥控信号: " + String(value);
    }
    
    if (command != "unknown") {
      executeRemoteCommand(command, message);
    }
    
    Serial.println("================================");
    mySwitch.resetAvailable();
  }
}

void checkServerCommands() {
  if (WiFi.status() == WL_CONNECTED) {
    WiFiClient client;
    HTTPClient http;
    
    http.begin(client, serverUrl);
    http.addHeader("Content-Type", "application/json");
    
    int httpCode = http.GET();
    
    if (httpCode == 200) {
      String payload = http.getString();
      DynamicJsonDocument doc(1024);
      deserializeJson(doc, payload);
      
      String command = doc["command"];
      if (command != "" && command != currentStatus) {
        String message = "服务器 - 卷帘门" + getActionDesc(command);
        executeRemoteCommand(command, message);
      }
    } else {
      Serial.println("❌ HTTP请求失败: " + String(httpCode));
      heartbeatWorking = false;
    }
    
    http.end();
  }
}

String getActionDesc(String command) {
  if (command == "up") return "上升";
  if (command == "down") return "下降";
  if (command == "stop") return "停止";
  if (command == "lock") return "锁定";
  return command;
}

void executeRemoteCommand(String command, String message) {
  if (command == "up") {
    if (currentStatus == "locked") {
      // Unlock first
      send433Signal(CMD_PAUSE);
      sendLog("stop", "自动解锁 - 准备上升");
      delay(500);
      currentStatus = "stopped";
    }
    if (currentStatus != "open") {
      send433Signal(CMD_UP);
      currentStatus = "moving_up";
      lastStateChange = millis();
      sendLog("moving_up", message);
      blinkLED(2, 150);
    } else {
      sendLog("open", "已打开 - 忽略上升命令");
    }
  } else if (command == "down") {
    if (currentStatus == "locked") {
      // Unlock first
      send433Signal(CMD_PAUSE);
      sendLog("stop", "自动解锁 - 准备下降");
      delay(500);
      currentStatus = "stopped";
    }
    if (currentStatus != "closed") {
      send433Signal(CMD_DOWN);
      currentStatus = "moving_down";
      lastStateChange = millis();
      sendLog("moving_down", message);
      blinkLED(2, 150);
    } else {
      sendLog("closed", "已关闭 - 忽略下降命令");
    }
  } else if (command == "stop") {
    send433Signal(CMD_PAUSE);
    if (currentStatus.indexOf("moving") >= 0) {
      currentStatus = "stopped";
    } else if (currentStatus == "locked") {
      currentStatus = "stopped"; // Unlocked
    }
    sendLog("stopped", message);
    blinkLED(1, LED_BLINK_DELAY);
  } else if (command == "lock") {
    if (currentStatus.indexOf("moving") >= 0) {
      // Stop first
      send433Signal(CMD_PAUSE);
      delay(500);
      currentStatus = "stopped";
      sendLog("stopped", "自动停止 - 准备锁定");
    }
    send433Signal(CMD_LOCK);
    currentStatus = "locked";
    sendLog("locked", message);
    blinkLED(3, 100);
  }
  
  sendDeviceStatus(currentStatus, WiFi.localIP().toString());
}

void send433Signal(unsigned long code) {
  Serial.print("📡 发送433MHz信号: ");
  Serial.println(code);
  
  for (int i = 0; i < 5; i++) {
    mySwitch.send(code, 24);
    delay(SIGNAL_REPEAT_DELAY);
  }
  
  Serial.println("✅ 信号发送完成");
}

void sendDeviceStatus(String status, String ip) {
  if (WiFi.status() == WL_CONNECTED) {
    WiFiClient client;
    HTTPClient http;
    
    http.begin(client, deviceStatusUrl);
    http.addHeader("Content-Type", "application/json");
    
    DynamicJsonDocument doc(512);
    doc["status"] = status;
    doc["ip"] = ip;
    
    String jsonString;
    serializeJson(doc, jsonString);
    
    int httpCode = http.POST(jsonString);
    Serial.print("📊 设备状态发送结果: ");
    Serial.println(httpCode);
    
    http.end();
    heartbeatWorking = (httpCode == 200);
  }
}

void sendLog(String status, String message) {
  if (WiFi.status() == WL_CONNECTED) {
    WiFiClient client;
    HTTPClient http;
    
    http.begin(client, logUrl);
    http.addHeader("Content-Type", "application/json");
    
    DynamicJsonDocument doc(512);
    doc["status"] = status;
    doc["message"] = message;
    doc["timestamp"] = millis() / 1000;
    
    String jsonString;
    serializeJson(doc, jsonString);
    
    int httpCode = http.POST(jsonString);
    Serial.print("📝 日志发送结果: ");
    Serial.println(httpCode);
    
    http.end();
  }
}

void sendHeartbeat() {
  String hbMessage = heartbeatWorking ? "心跳检测 - 系统运行正常" : "心跳检测 - 警告 - 连接异常";
  sendLog(currentStatus, hbMessage);
  sendDeviceStatus(currentStatus, WiFi.localIP().toString());
  Serial.println("💓 发送心跳 - 状态: " + currentStatus);
}

void blinkLED(int times, unsigned long delayMs) {
  for (int i = 0; i < times; i++) {
    digitalWrite(STATUS_LED, HIGH);
    delay(delayMs);
    digitalWrite(STATUS_LED, LOW);
    delay(delayMs);
  }
}
