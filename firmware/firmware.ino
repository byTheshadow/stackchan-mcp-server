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
  GESTURE_NOD
};

GestureKind activeGesture = GESTURE_NONE;
uint8_t gestureStep = 0;
unsigned long nextGestureStepMs = 0;

/*
 * 屏幕文字状态。
 *
 * 注意：
 * Avatar 启动后，必须通过 avatar.setSpeechText() 显示文字，
 * 不能直接操作 M5.Display，否则可能与 Avatar 的显示任务冲突。
 */
String activeSpeechText = "";
bool speechClearScheduled = false;
unsigned long speechClearAtMs = 0;

const unsigned long DEFAULT_DISPLAY_DURATION_MS = 5000;
const unsigned long MIN_DISPLAY_DURATION_MS = 1000;
const unsigned long MAX_DISPLAY_DURATION_MS = 10000;

void showBootMessage(const char* line1, const char* line2 = nullptr) {
  // 只能在 avatar.init() 之前调用。
  M5.Display.clear(TFT_BLACK);
  M5.Display.setTextColor(TFT_YELLOW, TFT_BLACK);
  M5.Display.setTextSize(2);
  M5.Display.setCursor(8, 35);
  M5.Display.println(line1);

  if (line2 != nullptr) {
    M5.Display.setTextColor(TFT_WHITE, TFT_BLACK);
    M5.Display.setTextSize(1);
    M5.Display.setCursor(8, 80);
    M5.Display.println(line2);
  }
}

void setExpressionByName(const char* expr) {
  if (expr == nullptr) return;

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

unsigned long clampDisplayDuration(unsigned long durationMs) {
  if (durationMs < MIN_DISPLAY_DURATION_MS) {
    return MIN_DISPLAY_DURATION_MS;
  }

  if (durationMs > MAX_DISPLAY_DURATION_MS) {
    return MAX_DISPLAY_DURATION_MS;
  }

  return durationMs;
}

void setSpeechTextForDuration(
  const char* text,
  unsigned long durationMs
) {
  if (text == nullptr) {
    return;
  }

  activeSpeechText = text;
  avatar.setSpeechText(activeSpeechText.c_str());

  /*
   * 空文字有明确含义：立即清除屏幕文字，
   * 同时取消之前的自动清除计时。
   */
  if (activeSpeechText.length() == 0) {
    speechClearScheduled = false;
    speechClearAtMs = 0;
    Serial.println("[DISPLAY] Speech text cleared");
    return;
  }

  durationMs = clampDisplayDuration(durationMs);
  speechClearAtMs = millis() + durationMs;
  speechClearScheduled = true;

  Serial.printf(
    "[DISPLAY] Speech text set: \"%s\" (%lu ms)\n",
    activeSpeechText.c_str(),
    durationMs
  );
}

void updateSpeechText() {
  if (!speechClearScheduled) {
    return;
  }

  const unsigned long now = millis();

  /*
   * 使用有符号差值，能安全处理 millis() 溢出。
   */
  if (static_cast<long>(now - speechClearAtMs) < 0) {
    return;
  }

  avatar.setSpeechText("");
  activeSpeechText = "";
  speechClearScheduled = false;
  speechClearAtMs = 0;

  Serial.println("[DISPLAY] Speech text auto-cleared");
}

void cancelGesture() {
  activeGesture = GESTURE_NONE;
  gestureStep = 0;
  nextGestureStepMs = 0;
}

void startNod() {
  if (activeGesture != GESTURE_NONE) {
    Serial.println("[GESTURE] Ignored nod: another gesture is active");
    return;
  }

  activeGesture = GESTURE_NOD;
  gestureStep = 0;
  nextGestureStepMs = 0;
  Serial.println("[GESTURE] Nod started");
}

void updateGesture() {
  if (activeGesture != GESTURE_NOD) {
    return;
  }

  const unsigned long now = millis();

  if (nextGestureStepMs != 0 && now < nextGestureStepMs) {
    return;
  }

  switch (gestureStep) {
    case 0:
      M5StackChan.Motion.moveY(300, 500);  // 已验证：30.0°
      Serial.println("[GESTURE] Nod step 1: Y=30.0");
      nextGestureStepMs = now + 350;
      break;

    case 1:
      M5StackChan.Motion.moveY(50, 600);   // 已验证：5.0°
      Serial.println("[GESTURE] Nod step 2: Y=5.0");
      nextGestureStepMs = now + 350;
      break;

    case 2:
      M5StackChan.Motion.moveY(300, 500);  // 已验证：30.0°
      Serial.println("[GESTURE] Nod step 3: Y=30.0");
      nextGestureStepMs = now + 350;
      break;

    case 3:
      M5StackChan.Motion.goHome(500);
      Serial.println("[GESTURE] Nod complete; returned home");
      cancelGesture();
      return;

    default:
      cancelGesture();
      return;
  }

  gestureStep++;
}

void webSocketEvent(WStype_t type, uint8_t* payload, size_t length) {
  switch (type) {
    case WStype_CONNECTED:
      Serial.println("[WS] Connected to Render");
      avatar.setExpression(Expression::Happy);
      M5StackChan.Motion.goHome(500);
      break;

    case WStype_DISCONNECTED:
      Serial.println("[WS] Disconnected from Render");
      break;

    case WStype_ERROR:
      Serial.println("[WS] WebSocket error");
      break;

    case WStype_TEXT: {
      Serial.printf(
        "[WS] Received %u bytes: %.*s\n",
        static_cast<unsigned>(length),
        static_cast<int>(length),
        reinterpret_cast<const char*>(payload)
      );

      DynamicJsonDocument doc(1024);
      DeserializationError error = deserializeJson(doc, payload, length);

      if (error) {
        Serial.printf("[WS] JSON parse failed: %s\n", error.c_str());
        return;
      }

      const char* expression = doc["expression"];
      const char* motion = doc["motion"];
      const char* action = doc["action"];

      /*
       * 只有 Render 明确发送 text_to_display 时才更新文字。
       * 因此旧版只含 expression + motion 的指令，
       * 不会意外清除当前正在显示的文字。
       */
      JsonVariantConst textToDisplay = doc["text_to_display"];

      if (!textToDisplay.isNull() && textToDisplay.is<const char*>()) {
        const char* text = textToDisplay.as<const char*>();

        unsigned long durationMs =
          doc["display_duration_ms"] | DEFAULT_DISPLAY_DURATION_MS;

        setSpeechTextForDuration(text, durationMs);
      }

      if (expression != nullptr) {
        Serial.printf("[ACTION] Expression: %s\n", expression);
        setExpressionByName(expression);
      }

      if (motion != nullptr) {
        Serial.printf("[ACTION] Motion: %s\n", motion);

        if (strcmp(motion, "nod") == 0) {
          startNod();
        } else if (strcmp(motion, "home") == 0) {
          cancelGesture();
          M5StackChan.Motion.goHome(500);
          Serial.println("[GESTURE] Home sent");
        } else if (strcmp(motion, "none") == 0) {
          Serial.println("[GESTURE] No motion requested");
        } else if (strcmp(motion, "shake") == 0) {
          // 暂不执行：尚未在实体设备验证安全范围。
          Serial.println("[GESTURE] Shake ignored: not hardware-tested yet");
        }
      } else if (action != nullptr && strcmp(action, "home") == 0) {
        cancelGesture();
        M5StackChan.Motion.goHome(500);
        Serial.println("[GESTURE] Home sent");
      }

      break;
    }

    default:
      break;
  }
}

void setup() {
  Serial.begin(115200);
  delay(1000);

  Serial.println();
  Serial.println(
    "=== StackChan WiFi + Render + Servo + Display firmware ==="
  );

  // StackChan-BSP 负责设备、舵机供电和舵机总线的官方初始化。
  M5StackChan.begin();

  M5StackChan.Motion.setAutoAngleSyncEnabled(false);
  M5StackChan.Motion.setAutoTorqueReleaseEnabled(true);
  M5StackChan.Motion.goHome(500);

  // 注意：Avatar 还未启动，因此此阶段可以安全直接显示文字。
  showBootMessage("WiFi starting...", "Please wait");

  prefs.begin("stackchan", false);

  String savedHost = prefs.getString("server_host", "");
  savedHost.toCharArray(ws_host, sizeof(ws_host));

  Serial.printf("[CONFIG] Saved Render host: '%s'\n", ws_host);

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

  Serial.println("[WIFI] Trying saved WiFi credentials...");

  if (!wm.autoConnect("StackChan-Setup")) {
    Serial.println("[WIFI] Could not join saved WiFi.");
    Serial.println("[WIFI] Starting config portal: StackChan-Setup");

    showBootMessage(
      "WiFi setup",
      "AP: StackChan-Setup\nOpen: 192.168.4.1"
    );

    // 不设置门户超时：等用户完成配网。
    wm.startConfigPortal("StackChan-Setup");
  }

  Serial.printf("[WIFI] Status: %d\n", WiFi.status());
  Serial.printf("[WIFI] IP: %s\n", WiFi.localIP().toString().c_str());

  const char* enteredHost = serverParameter.getValue();

  if (enteredHost != nullptr && strlen(enteredHost) > 0) {
    strncpy(ws_host, enteredHost, sizeof(ws_host) - 1);
    ws_host[sizeof(ws_host) - 1] = '\0';
    prefs.putString("server_host", ws_host);
  }

  Serial.printf("[CONFIG] Active Render host: '%s'\n", ws_host);

  // Wi‑Fi 配置完成后才启动 Avatar。
  // 从这里开始，不再直接操作 M5.Display。
  avatar.init();
  avatar.setExpression(Expression::Neutral);
  avatar.setSpeechText("");

  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[WIFI] Not connected; WebSocket will not start");
    return;
  }

  if (strlen(ws_host) == 0) {
    Serial.println("[WS] Render host is empty.");
    Serial.println(
      "[WS] Open StackChan-Setup and fill Render Server Host."
    );
    return;
  }

  Serial.printf(
    "[WS] Connecting to wss://%s:%u%s\n",
    ws_host,
    ws_port,
    ws_path
  );

  webSocket.onEvent(webSocketEvent);
  webSocket.setReconnectInterval(5000);
  webSocket.beginSSL(ws_host, ws_port, ws_path);
}

void loop() {
  M5StackChan.update();
  webSocket.loop();
  updateGesture();
  updateSpeechText();

  delay(10);
}

