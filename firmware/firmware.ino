#include <M5Unified.h>
#include <Avatar.h>
#include <WiFi.h>
#include <WiFiManager.h>
#include <WebSocketsClient.h>
#include <ArduinoJson.h>
#include <AudioFileSourceHTTPStream.h>
#include <AudioGeneratorMP3.h>
#include <AudioOutputI2S.h>

using namespace m5avatar;
Avatar avatar;
WebSocketsClient webSocket;

// 这里填你的 Render 服务端域名（去掉 https://，只留域名）
// 例如: "my-stackchan-mcp.onrender.com"
const char* ws_host = "YOUR_RENDER_DOMAIN_HERE"; 
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

        // 播放远程音频
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

  avatar.init();
  avatar.setExpression(Expression::Neutral);

  // 手机配网：如果没有连上 WiFi，会自动启动名为 StackChan-AP 的热点
  WiFiManager wm;
  if (!wm.autoConnect("StackChan-Setup")) {
    Serial.println("WiFi 配网失败");
  }

  // 初始化音频播放器
  out = new AudioOutputI2S();
  out->SetPinout(12, 0, 2); // CoreS3 内置 I2S 引脚
  mp3 = new AudioGeneratorMP3();
  file = new AudioFileSourceHTTPStream();

  // 启动 WebSocket 客户端连接到 Render
  webSocket.beginSSL(ws_host, ws_port, ws_path);
  webSocket.onEvent(webSocketEvent);
  webSocket.setReconnectInterval(5000);
}

void loop() {
  M5.update();
  webSocket.loop();
  
  if (mp3->isRunning()) {
    if (!mp3->loop()) {
      mp3->stop();
      avatar.setExpression(Expression::Neutral); // 播完语音恢复正常表情
    }
  }
}
