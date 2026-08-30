#include <Arduino.h>
#include <M5Unified.h>
#include <M5StackChan.h>
#include <Avatar.h>
#include <WiFi.h>
#include <WiFiManager.h>
#include <Preferences.h>
#include <WebSocketsClient.h>
#include <ArduinoJson.h>

using namespace m5avatar;

Avatar avatar;
WebSocketsClient webSocket;
Preferences prefs;

char ws_host[128] = "";
const int ws_port = 443;
const char* ws_path = "/";

// 动作状态机
enum GestureKind {
  GESTURE_NONE = 0,
  GESTURE_NOD,
  GESTURE_SHAKE
};

GestureKind activeGesture = GESTURE_NONE;
uint8_t gestureStep = 0;
unsigned long nextGestureStepMs = 0;

void startGesture(GestureKind gesture) {
  activeGesture = gesture;
  gestureStep = 0;
  nextGestureStepMs = 0;
}

void cancelGesture() {
  activeGesture = GESTURE_NONE;
  gestureStep = 0;
  nextGestureStepMs = 0;
}

void updateGesture() {
  if (activeGesture == GESTURE_NONE) return;

  unsigned long now = millis();
  if (nextGestureStepMs != 0 && now < nextGestureStepMs) return;

  if (activeGesture == GESTURE_NOD) {
    switch (gestureStep) {
      case 0:
        M5StackChan.Motion.moveY(300, 500); // 30.0°
        nextGestureStepMs = now + 350;
        break;
      case 1:
        M5StackChan.Motion.moveY(50, 600);  // 5.0°
        nextGestureStepMs = now + 350;
        break;
      case 2:
        M5StackChan.Motion.moveY(300, 500); // 30.0°
        nextGestureStepMs = now + 350;
        break;
      case 3:
        M5StackChan.Motion.goHome(500);
        activeGesture = GESTURE_NONE;
        nextGestureStepMs = 0;
        Serial.println("[GESTURE] Nod complete, returned home.");
        break;
    }
  } else if (activeGesture == GESTURE_SHAKE) {
    switch (gestureStep) {
      case 0:
        M5StackChan.Motion.moveX(-300, 500); // -30.0°
        nextGestureStepMs = now + 350;
        break;
      case 1:
        M5StackChan.Motion.moveX(300, 500);  // +30.0°
        nextGestureStepMs = now + 350;
        break;
      case 2:
        M5StackChan.Motion.moveX(-300, 500); // -30.0°
        nextGestureStepMs = now + 350;
        break;
      case 3:
        M5StackChan.Motion.goHome(500);
        activeGesture = GESTURE_NONE;
        nextGestureStepMs = 0;
        Serial.println("[GESTURE] Shake complete, returned home.");
        break;
    }
  }
  gestureStep++;
}

void webSocketEvent(WStype_t type, uint8_t * payload, size_t length) {
  switch(type) {
    case WStype_DISCONNECTED:
      Serial.println("[WS] Disconnected!");
      break;
    case WStype_CONNECTED:
      Serial.println("[WS] Connected to Server!");
      avatar.setExpression(Expression::Happy);
      M5StackChan.Motion.goHome(500);
      break;
    case WStype_TEXT: {
      Serial.printf("[WS] Received: %s\n", payload);
      DynamicJsonDocument doc(1024);
      DeserializationError err = deserializeJson(doc, payload);
      if (err) {
        Serial.println("[WS] JSON parse failed");
        return;
      }

      // 1. 处理表情 (兼容 root 级或 action 级)
      const char* expr = doc["expression"];
      if (expr) {
        if (strcmp(expr, "happy") == 0) avatar.setExpression(Expression::Happy);
        else if (strcmp(expr, "sad") == 0) avatar.setExpression(Expression::Sad);
        else if (strcmp(expr, "angry") == 0) avatar.setExpression(Expression::Angry);
        else if (strcmp(expr, "doubt") == 0) avatar.setExpression(Expression::Doubt);
        else if (strcmp(expr, "sleepy") == 0) avatar.setExpression(Expression::Sleepy);
        else avatar.setExpression(Expression::Neutral);
      }

      // 2. 处理动作 (motion 字段)
      const char* motion = doc["motion"];
      const char* action = doc["action"];
      
      if (motion) {
        if (strcmp(motion, "nod") == 0) {
          Serial.println("[ACTION] Trigger Nod");
          startGesture(GESTURE_NOD);
        } else if (strcmp(motion, "shake") == 0) {
          Serial.println("[ACTION] Trigger Shake");
          startGesture(GESTURE_SHAKE);
        } else if (strcmp(motion, "home") == 0) {
          Serial.println("[ACTION] Trigger Home");
          cancelGesture();
          M5StackChan.Motion.goHome(500);
        }
      } else if (action && strcmp(action, "home") == 0) {
        cancelGesture();
        M5StackChan.Motion.goHome(500);
      }
      break;
    }
  }
}

void setup() {
  Serial.begin(115200);
  delay(1000);

  // 1. 官方 StackChan-BSP 底层初始化
  M5StackChan.begin();
  M5StackChan.Motion.setAutoAngleSyncEnabled(false);
  M5StackChan.Motion.setAutoTorqueReleaseEnabled(true);
  M5StackChan.Motion.goHome(500);

  // 2. 读取保存的 Render 域名配置
  prefs.begin("stackchan", false);
  String saved_host = prefs.getString("server_host", "");
  saved_host.toCharArray(ws_host, 128);

  M5.Display.clear();
  M5.Display.setTextSize(2);
  M5.Display.setTextColor(TFT_YELLOW, TFT_BLACK);
  M5.Display.println("WiFi Connecting...");

  // 3. 配网逻辑
  WiFiManagerParameter custom_server_host("server", "MCP Server Host (e.g. xxx.onrender.com)", ws_host, 128);
  WiFiManager wm;
  wm.addParameter(&custom_server_host);
  wm.setConnectTimeout(15);

  if (!wm.autoConnect("StackChan-Setup")) {
    M5.Display.clear();
    M5.Display.println("WiFi Failed!");
    M5.Display.println("Connect to AP:");
    M5.Display.println("StackChan-Setup");
    M5.Display.println("192.168.4.1");
    wm.startConfigPortal("StackChan-Setup");
  }

  // 保存最新服务器地址
  if (strlen(custom_server_host.getValue()) > 0) {
    strcpy(ws_host, custom_server_host.getValue());
    prefs.putString("server_host", ws_host);
  }

  M5.Display.clear();
  M5.Display.println("WiFi OK!");
  M5.Display.println(WiFi.localIP().toString());
  delay(1200);

  // 4. 启动表情系统
  avatar.init();
  avatar.setExpression(Expression::Neutral);

  // 5. 启动 Render 公网 WebSocket 连接
  if (strlen(ws_host) > 0) {
    webSocket.beginSSL(ws_host, ws_port, ws_path);
    webSocket.onEvent(webSocketEvent);
    webSocket.setReconnectInterval(5000);
  }
}

void loop() {
  M5StackChan.update();
  webSocket.loop();
  updateGesture();
  delay(10);
}
