#include <M5Unified.h>
#include <Avatar.h>
#include <WiFi.h>
#include <WiFiManager.h>
#include <Preferences.h>
#include <WebSocketsClient.h>
#include <ArduinoJson.h>
#include <AudioFileSourceHTTPStream.h>
#include <AudioGeneratorMP3.h>
#include <AudioOutputI2S.h>

using namespace m5avatar;
Avatar avatar;
WebSocketsClient webSocket;
Preferences prefs;

// 动态存储的服务器域名（不写死在代码中）
char ws_host[128] = "";
const int ws_port = 443;
const char* ws_path = "/";

AudioGeneratorMP3 *mp3;
AudioFileSourceHTTPStream *file;
AudioOutputI2S *out;

void webSocketEvent(WStype_t type, uint8_t * payload, size_t length) {
  switch(type) {
    case WStype_DISCONNECTED:
      Serial.println("[WS] Disconnected!");
      break;
    case WStype_CONNECTED:
      Serial.println("[WS] Connected to Server!");
      avatar.setExpression(Expression::Happy);
      break;
    case WStype_TEXT: {
      Serial.printf("[WS] Received: %s\n", payload);
      DynamicJsonDocument doc(1024);
      deserializeJson(doc, payload);

      const char* action = doc["action"];
      if (strcmp(action, "speak") == 0) {
        const char* expr = doc["expression"];
        const char* audio_url = doc["audio_url"];

        // 切换表情
        if (strcmp(expr, "happy") == 0) avatar.setExpression(Expression::Happy);
        else if (strcmp(expr, "sad") == 0) avatar.setExpression(Expression::Sad);
        else if (strcmp(expr, "angry") == 0) avatar.setExpression(Expression::Angry);
        else if (strcmp(expr, "doubt") == 0) avatar.setExpression(Expression::Doubt);
        else if (strcmp(expr, "sleepy") == 0) avatar.setExpression(Expression::Sleepy);
        else avatar.setExpression(Expression::Neutral);

        // 播放音频
        if (audio_url && strlen(audio_url) > 0) {
          if (mp3->isRunning()) mp3->stop();
          file->open(audio_url);
          mp3->begin(file, out);
        }
      }
      break;
    }
  }
}

void setup() {
  auto cfg = M5.config();
  M5.begin(cfg);
  M5.Speaker.setVolume(150);
  Serial.begin(115200);

  // 从芯片内部存储读取之前保存的服务器域名
  prefs.begin("stackchan", false);
  String saved_host = prefs.getString("server_host", "");
  saved_host.toCharArray(ws_host, 128);

  M5.Display.setTextSize(2);
  M5.Display.setTextColor(TFT_YELLOW, TFT_BLACK);
  M5.Display.println("Starting WiFi...");

  // 在配网页面增加自定义输入框：MCP Server
  WiFiManagerParameter custom_server_host("server", "MCP Server Host (e.g. xxx.onrender.com)", ws_host, 128);

  WiFiManager wm;
  wm.addParameter(&custom_server_host);
  wm.setConfigPortalTimeout(0); // 永不超时

  // 开启配网热点
  if (!wm.autoConnect("StackChan-Setup")) {
    M5.Display.println("Connect to WiFi:");
    M5.Display.println("StackChan-Setup");
  } else {
    M5.Display.println("WiFi Connected!");
  }

  // 保存用户在配网页面输入的服务器地址
  if (strlen(custom_server_host.getValue()) > 0) {
    strcpy(ws_host, custom_server_host.getValue());
    prefs.putString("server_host", ws_host);
  }

  delay(1000);

  // 初始化 Avatar 表情系统
  avatar.init();
  avatar.setExpression(Expression::Neutral);

  // 初始化音频播放器
  out = new AudioOutputI2S();
  out->SetPinout(12, 0, 2); // CoreS3 I2S
  mp3 = new AudioGeneratorMP3();
  file = new AudioFileSourceHTTPStream();

  // 如果有配置服务器地址，则启动 WebSocket 连接
  if (strlen(ws_host) > 0) {
    Serial.printf("Connecting to WebSocket: %s\n", ws_host);
    webSocket.beginSSL(ws_host, ws_port, ws_path);
    webSocket.onEvent(webSocketEvent);
    webSocket.setReconnectInterval(5000);
  }
}

void loop() {
  M5.update();
  webSocket.loop();

  if (mp3->isRunning()) {
    if (!mp3->loop()) {
      mp3->stop();
      avatar.setExpression(Expression::Neutral);
    }
  }
}
