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
const uint16_t ws_port = 443;
const char* ws_path = "/";

enum GestureKind {
  GESTURE_NONE = 0,
  GESTURE_NOD,
  GESTURE_SHAKE
};

GestureKind activeGesture = GESTURE_NONE;
uint8_t gestureStep = 0;
unsigned long nextGestureStepMs = 0;

void showStatus(const char* line1, const String& line2 = "") {
  M5.Display.clear(TFT_BLACK);
  M5.Display.setTextColor(TFT_YELLOW, TFT_BLACK);
  M5.Display.setTextSize(2);
  M5.Display.setCursor(8, 35);
  M5.Display.println(line1);

  if (line2.length() > 0) {
    M5.Display.setTextColor(TFT_WHITE, TFT_BLACK);
    M5.Display.setTextSize(1);
    M5.Display.setCursor(8, 80);
    M5.Display.println(line2);
  }
}

void setExpressionByName(const char* expr) {
  if (!expr) return;

  if (strcmp(expr, "happy") == 0) {
    avatar.setExpression(Expression::Happy);
  } else if (strcmp(expr, "sad") == 0) {
    avatar.setExpression(Expression::Sad);
  } else if (strcmp(expr, "angry") == 0) {
    avatar.setExpression(Expression::Angry);
  } else if (strcmp(expr, "doubt") == 0) {
    avatar.setExpression(Expression::Doubt);
  } else if (strcmp(expr, "sleepy") == 0) {
    avatar.setExpression(Expression::Sleepy);
  } else {
    avatar.setExpression(Expression::Neutral);
  }
}

void startGesture(GestureKind gesture) {
  if (activeGesture != GESTURE_NONE) {
    Serial.println("[GESTURE] Ignored: another gesture is active");
    return;
  }

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

  const unsigned long now = millis();

  if (nextGestureStepMs != 0 && now < nextGestureStepMs) {
    return;
  }

  if (activeGesture == GESTURE_NOD) {
    switch (gestureStep) {
      case 0:
        M5StackChan.Motion.moveY(300, 500);
        Serial.println("[GESTURE] Nod step 1: Y=30.0");
        nextGestureStepMs = now + 350;
        break;

      case 1:
        M5StackChan.Motion.moveY(50, 600);
        Serial.println("[GESTURE] Nod step 2: Y=5.0");
        nextGestureStepMs = now + 350;
        break;

      case 2:
        M5StackChan.Motion.moveY(300, 500);
        Serial.println("[GESTURE] Nod step 3: Y=30.0");
        nextGestureStepMs = now + 350;
        break;

      case 3:
        M5StackChan.Motion.goHome(500);
        Serial.println("[GESTURE] Nod complete; returned home");
        cancelGesture();
        break;
    }

    gestureStep++;
  }

  else if (activeGesture == GESTURE_SHAKE) {
    switch (gestureStep) {
      case 0:
        M5StackChan.Motion.moveX(-300, 500);
        Serial.println("[GESTURE] Shake step 1: X=-30.0");
        nextGestureStepMs = now + 350;
        break;

      case 1:
        M5StackChan.Motion.moveX(300, 500);
        Serial.println("[GESTURE] Shake step 2: X=30.0");
        nextGestureStepMs = now + 350;
        break;

      case 2:
        M5StackChan.Motion.moveX(-300, 500);
        Serial.println("[GESTURE] Shake step 3: X=-30.0");
        nextGestureStepMs = now + 350;
        break;

      case 3:
        M5StackChan.Motion.goHome(500);
        Serial.println("[GESTURE] Shake complete; returned home");
        cancelGesture();
        break;
    }

    gestureStep++;
  }
}

void webSocketEvent(
  WStype_t type,
  uint8_t* payload,
  size_t length
) {
  switch (type) {
    case WStype_DISCONNECTED:
      Serial.println("[WS] Disconnected");
      showStatus("WiFi connected", "Render WebSocket disconnected");
      break;

    case WStype_CONNECTED:
      Serial.println("[WS] Connected to Render");
      avatar.setExpression(Expression::Happy);
      M5StackChan.Motion.goHome(500);
      showStatus("Render connected", WiFi.localIP().toString());
      break;

    case WStype_TEXT: {
      Serial.printf("[WS] Received %u bytes: %.*s\n",
                    static_cast<unsigned>(length),
                    static_cast<int>(length),
                    reinterpret_cast<const char*>(payload));

      DynamicJsonDocument doc(1024);
      DeserializationError error = deserializeJson(doc, payload, length);

      if (error) {
        Serial.printf("[WS] JSON parse failed: %s\n", error.c_str());
        return;
      }

      const char* expression = doc["expression"];
      const char* motion = doc["motion"];
      const char* action = doc["action"];

      if (expression) {
        Serial.printf("[ACTION] Expression: %s\n", expression);
        setExpressionByName(expression);
      }

      if (motion) {
        Serial.printf("[ACTION] Motion: %s\n", motion);

        if (strcmp(motion, "nod") == 0) {
          startGesture(GESTURE_NOD);
        } else if (strcmp(motion, "shake") == 0) {
          startGesture(GESTURE_SHAKE);
        } else if (strcmp(motion, "home") == 0) {
          cancelGesture();
          M5StackChan.Motion.goHome(500);
        }
      } else if (action && strcmp(action, "home") == 0) {
        cancelGesture();
        M5StackChan.Motion.goHome(500);
      }

      break;
    }

    case WStype_ERROR:
      Serial.println("[WS] Connection error");
      break;

    default:
      break;
  }
}

void setup() {
  Serial.begin(115200);
  delay(1000);

  Serial.println();
  Serial.println("=== StackChan integrated firmware ===");

  // 1. 初始化官方 StackChan BSP
  M5StackChan.begin();

  M5StackChan.Motion.setAutoAngleSyncEnabled(false);
  M5StackChan.Motion.setAutoTorqueReleaseEnabled(true);
  M5StackChan.Motion.goHome(500);

  // 2. 初始化 Avatar
  avatar.init();
  avatar.setExpression(Expression::Neutral);

  // 3. 读取保存的 Render 主机
  prefs.begin("stackchan", false);

  String savedHost = prefs.getString("server_host", "");
  savedHost.toCharArray(ws_host, sizeof(ws_host));

  Serial.printf("[CONFIG] Saved Render host: '%s'\n", ws_host);

  showStatus("Connecting WiFi...", "Please wait");

  // 4. Wi-Fi 配网
  WiFi.mode(WIFI_STA);

  WiFiManager wm;
  WiFiManagerParameter serverParameter(
    "server",
    "Render Server Host",
    ws_host,
    sizeof(ws_host)
  );

  wm.addParameter(&serverParameter);
  wm.setConnectTimeout(15);
  wm.setConfigPortalTimeout(180);

  Serial.println("[WIFI] Trying saved WiFi credentials...");

  bool connected = wm.autoConnect("StackChan-Setup");

  if (!connected) {
    Serial.println("[WIFI] Failed to connect");
    showStatus("WiFi failed", "AP: StackChan-Setup");

    // 继续运行配置门户。
    // 设备会创建 StackChan-Setup 热点。
    wm.startConfigPortal("StackChan-Setup");
  }

  Serial.printf("[WIFI] Status: %d\n", WiFi.status());
  Serial.printf("[WIFI] IP: %s\n", WiFi.localIP().toString().c_str());

  // 5. 保存服务器地址
  const char* enteredHost = serverParameter.getValue();

  if (enteredHost && strlen(enteredHost) > 0) {
    strncpy(ws_host, enteredHost, sizeof(ws_host) - 1);
    ws_host[sizeof(ws_host) - 1] = '\0';
    prefs.putString("server_host", ws_host);
  }

  Serial.printf("[CONFIG] Active Render host: '%s'\n", ws_host);

  if (WiFi.status() != WL_CONNECTED) {
    showStatus("WiFi not connected", "Check configuration");
    Serial.println("[WIFI] Not connected; WebSocket will not start");
    return;
  }

  showStatus("WiFi connected", WiFi.localIP().toString());

  // 6. 连接 Render WebSocket
  if (strlen(ws_host) == 0) {
    Serial.println("[WS] Render host is empty");
    showStatus("WiFi OK", "Render host is empty");
    return;
  }

  Serial.printf("[WS] Connecting to wss://%s:%u%s\n",
                ws_host,
                ws_port,
                ws_path);

  webSocket.beginSSL(ws_host, ws_port, ws_path);
  webSocket.onEvent(webSocketEvent);
  webSocket.setReconnectInterval(5000);
}

void loop() {
  M5StackChan.update();
  webSocket.loop();
  updateGesture();
  delay(10);
}
