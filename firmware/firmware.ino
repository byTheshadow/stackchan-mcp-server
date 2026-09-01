#include <Arduino.h>
#include <M5Unified.h>
#include <M5StackChan.h>
#include <math.h>


#include <Avatar.h>
#include <Eyeblow.h>
#include <Face.h>
#include <Mouth.h>

#include <WiFi.h>
#include <WiFiManager.h>
#include <Preferences.h>
#include <WebSocketsClient.h>
#include <ArduinoJson.h>
#include <SPI.h>
#include <SD.h>

#include "CustomEye.h"
#include "CustomMouth.h"
#include "CustomEyeblow.h"


using namespace m5avatar;

/*
 * 自定义眼睛效果状态。
 *
 * FaceEffect::None:
 *   使用 m5stack-avatar 原生 Eye。
 *
 * FaceEffect::HeartEyes:
 *   使用自定义爱心眼。
 */
FaceEffectState faceEffectState;
/*
 * RGB 灯光控制。
 *
 * 0~5  ：左侧 6 颗灯
 * 6~11 ：右侧 6 颗灯
 */
struct LedTheme {
  uint8_t r;
  uint8_t g;
  uint8_t b;
  uint8_t maxBrightness;
  uint16_t periodMs;
};

LedTheme currentLedTheme = {
  20, 20, 30,  // 默认冷色
  80,          // 默认最大亮度
  3000         // 呼吸周期
};

unsigned long lastLedUpdateMs = 0;

static constexpr unsigned long LED_UPDATE_INTERVAL_MS = 40;
static constexpr uint8_t LED_COUNT = 12;
/*
 * 头顶触摸：柔和闪烁两次。
 *
 * 一个周期为“亮起 + 回落”，共两个周期。
 */
bool headTouchLightActive = false;
unsigned long headTouchLightStartedMs = 0;

static constexpr unsigned long HEAD_TOUCH_LIGHT_CYCLE_MS = 600;
static constexpr uint8_t HEAD_TOUCH_LIGHT_FLASH_COUNT = 2;

/*
 * 屏幕文字存在时，视为角色正在说话。
 */
static constexpr float SPEAKING_LIGHT_BOOST = 1.22f;




/*
 * Face 构造参数顺序：
 *
 * mouth,
 * right eye,
 * left eye,
 * right eyebrow,
 * left eyebrow
 *
 * Avatar 会负责释放 Face；
 * Face 会负责释放其中的各个 Drawable。
 */
Avatar avatar(
  new Face(
  new CustomMouth(&faceEffectState),
new CustomEye(false, &faceEffectState),  // 右眼
new CustomEye(true, &faceEffectState),   // 左眼
new CustomEyeblow(false, &faceEffectState), // 右眉
new CustomEyeblow(true, &faceEffectState)   // 左眉

  )
);

WebSocketsClient webSocket;
Preferences prefs;

char ws_host[128] = "";

const uint16_t ws_port = 443;
const char* ws_path = "/";

/*
 * M5Stack CoreS3 官方 microSD SPI 引脚定义。
 */
static constexpr int SD_SPI_CS_PIN = 4;
static constexpr int SD_SPI_SCK_PIN = 36;
static constexpr int SD_SPI_MISO_PIN = 35;
static constexpr int SD_SPI_MOSI_PIN = 37;

bool sdCardReady = false;

/*
 * WAV 流式读取缓冲。
 */
static constexpr size_t WAV_BUFFER_COUNT = 3;
static constexpr size_t WAV_BUFFER_SIZE = 1024;

uint8_t wavData[WAV_BUFFER_COUNT][WAV_BUFFER_SIZE];

enum GestureKind {
  GESTURE_NONE = 0,
  GESTURE_NOD
};

GestureKind activeGesture = GESTURE_NONE;
uint8_t gestureStep = 0;
unsigned long nextGestureStepMs = 0;

/*
 * 屏幕文字状态。
 */
String activeSpeechText = "";
bool speechClearScheduled = false;
unsigned long speechClearAtMs = 0;

const unsigned long DEFAULT_DISPLAY_DURATION_MS = 5000;
const unsigned long MIN_DISPLAY_DURATION_MS = 1000;
const unsigned long MAX_DISPLAY_DURATION_MS = 10000;

/*
 * 屏幕触摸防抖。
 */
const unsigned long TOUCH_EVENT_DEBOUNCE_MS = 700;
unsigned long lastTouchEventMs = 0;

/*
 * 头顶触摸防抖。
 */
const unsigned long HEAD_TOUCH_DEBOUNCE_MS = 700;
unsigned long lastHeadTouchEventMs = 0;

struct __attribute__((packed)) WavHeader {
  char riff[4];
  uint32_t chunkSize;
  char waveFmt[8];
  uint32_t fmtChunkSize;
  uint16_t audioFormat;
  uint16_t channels;
  uint32_t sampleRate;
  uint32_t bytesPerSecond;
  uint16_t blockAlign;
  uint16_t bitsPerSample;
};

struct __attribute__((packed)) WavSubChunk {
  char identifier[4];
  uint32_t chunkSize;
};

/*
 * 从 SD 卡读取并播放 PCM WAV。
 *
 * 支持：
 * - PCM；
 * - 8-bit 或 16-bit；
 * - 单声道或立体声。
 */
bool playSdWav(const char* filename) {
  if (!sdCardReady) {
    Serial.println("[AUDIO] SD card is not ready");
    return false;
  }

  if (filename == nullptr || !SD.exists(filename)) {
    Serial.printf(
      "[AUDIO] WAV file not found: %s\n",
      filename == nullptr ? "(null)" : filename
    );
    return false;
  }

  File file = SD.open(filename, FILE_READ);

  if (!file) {
    Serial.printf("[AUDIO] Failed to open: %s\n", filename);
    return false;
  }

  WavHeader header = {};

  if (
    file.read(
      reinterpret_cast<uint8_t*>(&header),
      sizeof(WavHeader)
    ) != sizeof(WavHeader)
  ) {
    Serial.printf("[AUDIO] WAV header read failed: %s\n", filename);
    file.close();
    return false;
  }

  Serial.printf(
    "[AUDIO] WAV %s: format=%u, channels=%u, rate=%lu, bits=%u\n",
    filename,
    static_cast<unsigned>(header.audioFormat),
    static_cast<unsigned>(header.channels),
    static_cast<unsigned long>(header.sampleRate),
    static_cast<unsigned>(header.bitsPerSample)
  );

  if (
    memcmp(header.riff, "RIFF", 4) != 0 ||
    memcmp(header.waveFmt, "WAVEfmt ", 8) != 0 ||
    header.audioFormat != 1 ||
    header.bitsPerSample < 8 ||
    header.bitsPerSample > 16 ||
    header.channels == 0 ||
    header.channels > 2 ||
    header.sampleRate == 0
  ) {
    Serial.println(
      "[AUDIO] Unsupported WAV: require PCM, 8/16-bit, mono/stereo"
    );
    file.close();
    return false;
  }

  /*
   * 跳过 fmt chunk 其余内容，并查找 data chunk。
   */
  const uint32_t afterFmtOffset =
    offsetof(WavHeader, audioFormat) + header.fmtChunkSize;

  if (!file.seek(afterFmtOffset)) {
    Serial.println("[AUDIO] Failed to seek after fmt chunk");
    file.close();
    return false;
  }

  WavSubChunk subChunk = {};
  bool dataChunkFound = false;

  while (
    file.available() >= static_cast<int>(sizeof(WavSubChunk)) &&
    file.read(
      reinterpret_cast<uint8_t*>(&subChunk),
      sizeof(WavSubChunk)
    ) == sizeof(WavSubChunk)
  ) {
    if (memcmp(subChunk.identifier, "data", 4) == 0) {
      dataChunkFound = true;
      break;
    }

    if (!file.seek(file.position() + subChunk.chunkSize)) {
      break;
    }
  }

  if (!dataChunkFound) {
    Serial.println("[AUDIO] WAV data chunk not found");
    file.close();
    return false;
  }

  uint32_t remaining = subChunk.chunkSize;
  size_t bufferIndex = 0;

  const bool stereo = header.channels > 1;
  const bool is16Bit = header.bitsPerSample == 16;

  Serial.printf(
    "[AUDIO] Playing %s (%lu bytes PCM)\n",
    filename,
    static_cast<unsigned long>(remaining)
  );

  while (remaining > 0) {
    const size_t requested =
      remaining < WAV_BUFFER_SIZE
        ? remaining
        : WAV_BUFFER_SIZE;

    const size_t bytesRead = file.read(
      wavData[bufferIndex],
      requested
    );

    if (bytesRead == 0) {
      Serial.println("[AUDIO] Unexpected end of WAV data");
      break;
    }

    remaining -= bytesRead;

    if (is16Bit) {
      M5.Speaker.playRaw(
        reinterpret_cast<const int16_t*>(wavData[bufferIndex]),
        bytesRead >> 1,
        header.sampleRate,
        stereo,
        1,
        0
      );
    } else {
      M5.Speaker.playRaw(
        reinterpret_cast<const uint8_t*>(wavData[bufferIndex]),
        bytesRead,
        header.sampleRate,
        stereo,
        1,
        0
      );
    }

    bufferIndex =
      bufferIndex < (WAV_BUFFER_COUNT - 1)
        ? bufferIndex + 1
        : 0;
  }

  file.close();

  Serial.println("[AUDIO] WAV data queued");
  return true;
}

void playSoundByName(const char* sound) {
  if (sound == nullptr || strcmp(sound, "none") == 0) {
    return;
  }

  if (strcmp(sound, "message") == 0) {
    playSdWav("/message.wav");
    return;
  }

  if (strcmp(sound, "emotion") == 0) {
    playSdWav("/emotion.wav");
    return;
  }

  Serial.printf("[AUDIO] Unknown sound ignored: %s\n", sound);
}

void showBootMessage(
  const char* line1,
  const char* line2 = nullptr
) {
  /*
   * 只能在 avatar.init() 之前调用。
   */
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
  if (expr == nullptr) {
    return;
  }

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
void setFaceEffectByName(const char* effect) {
  if (effect == nullptr) {
    return;
  }

  if (strcmp(effect, "none") == 0) {
    faceEffectState.set(FaceEffect::None);

  } else if (strcmp(effect, "heart_eyes") == 0) {
    faceEffectState.set(FaceEffect::HeartEyes);

  } else if (strcmp(effect, "sparkle_eyes") == 0) {
    faceEffectState.set(FaceEffect::SparkleEyes);

  } else if (strcmp(effect, "dizzy_eyes") == 0) {
    faceEffectState.set(FaceEffect::DizzyEyes);

  } else if (strcmp(effect, "tear_eyes") == 0) {
    faceEffectState.set(FaceEffect::TearEyes);

  } else if (strcmp(effect, "surprised_face") == 0) {
    faceEffectState.set(FaceEffect::SurprisedFace);

  } else if (strcmp(effect, "pout_face") == 0) {
    faceEffectState.set(FaceEffect::PoutFace);

  } else if (strcmp(effect, "shy_face") == 0) {
    faceEffectState.set(FaceEffect::ShyFace);

  } else if (strcmp(effect, "smug_face") == 0) {
    faceEffectState.set(FaceEffect::SmugFace);

  } else if (strcmp(effect, "confused_face") == 0) {
    faceEffectState.set(FaceEffect::ConfusedFace);

  } else if (strcmp(effect, "laugh_face") == 0) {
    faceEffectState.set(FaceEffect::LaughFace);

  } else if (strcmp(effect, "kiss_face") == 0) {
    faceEffectState.set(FaceEffect::KissFace);

  } else if (strcmp(effect, "nervous_face") == 0) {
    faceEffectState.set(FaceEffect::NervousFace);

  } else if (strcmp(effect, "relieved_face") == 0) {
    faceEffectState.set(FaceEffect::RelievedFace);

  } else if (strcmp(effect, "determined_face") == 0) {
    faceEffectState.set(FaceEffect::DeterminedFace);

  } else {
    faceEffectState.set(FaceEffect::None);

    Serial.printf(
        "[FACE] Unknown effect '%s', fallback to none\n",
        effect
    );
    return;
  }

  Serial.printf("[FACE] Effect: %s\n", effect);
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
   * 空文字立即清除，并取消旧的自动清除计时。
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
   * 使用有符号差值，正确处理 millis() 溢出。
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
      return;

    default:
      cancelGesture();
      return;
  }

  gestureStep++;
}

/*
 * 将屏幕触摸事件通过 WebSocket 上报至 Render。
 */
void sendTouchTapEvent(int32_t x, int32_t y) {
  if (!webSocket.isConnected()) {
    Serial.println(
      "[TOUCH] Tap detected, but WebSocket is disconnected"
    );
    return;
  }

  StaticJsonDocument<192> eventDoc;

  eventDoc["type"] = "robot_event";
  eventDoc["event"] = "touch_tap";
  eventDoc["x"] = x;
  eventDoc["y"] = y;
  eventDoc["at_ms"] = millis();

  String eventJson;
  serializeJson(eventDoc, eventJson);

  webSocket.sendTXT(eventJson);

  Serial.printf(
    "[TOUCH] touch_tap sent: x=%ld, y=%ld\n",
    static_cast<long>(x),
    static_cast<long>(y)
  );
}

void sendHeadTouchEvent() {
  if (!webSocket.isConnected()) {
    Serial.println(
      "[HEAD] Touch detected, but WebSocket is disconnected"
    );
    return;
  }

  StaticJsonDocument<128> eventDoc;

  eventDoc["type"] = "robot_event";
  eventDoc["event"] = "head_touch";
  eventDoc["at_ms"] = millis();

  String eventJson;
  serializeJson(eventDoc, eventJson);

  webSocket.sendTXT(eventJson);

  Serial.println("[HEAD] head_touch sent");
}

void updateHeadTouchInput() {
  /*
   * intensities[0] = Front
   * intensities[1] = Middle
   * intensities[2] = Back
   */
  const auto& intensities =
    M5StackChan.TouchSensor.getIntensities();

  const bool isTouched =
    intensities[0] > 0 ||
    intensities[1] > 0 ||
    intensities[2] > 0;

  /*
   * 仅在“未触摸 → 触摸”的边缘上报一次。
   */
  static bool wasTouched = false;

  if (!isTouched) {
    wasTouched = false;
    return;
  }

  if (wasTouched) {
    return;
  }

  wasTouched = true;

  const unsigned long now = millis();

  if (
    lastHeadTouchEventMs != 0 &&
    now - lastHeadTouchEventMs < HEAD_TOUCH_DEBOUNCE_MS
  ) {
    Serial.println("[HEAD] Ignored by debounce");
    return;
  }

lastHeadTouchEventMs = now;

Serial.printf(
  "[HEAD] Raw touch detected: front=%u, middle=%u, back=%u\n",
  static_cast<unsigned>(intensities[0]),
  static_cast<unsigned>(intensities[1]),
  static_cast<unsigned>(intensities[2])
);

/*
 * 仅在有效的“未触摸 -> 触摸”边缘事件触发，
 * 不会在持续触摸期间反复重启闪烁。
 */
startHeadTouchLightEffect();

sendHeadTouchEvent();
}

/*
 * 检查屏幕的新一次按下。
 *
 * M5StackChan.update() 已经在 loop() 开头执行，
 * 所以这里不再次调用 M5.update()。
 */
void updateTouchInput() {
  auto touch = M5.Touch.getDetail();

  if (!touch.wasPressed()) {
    return;
  }

  const unsigned long now = millis();

  if (
    lastTouchEventMs != 0 &&
    now - lastTouchEventMs < TOUCH_EVENT_DEBOUNCE_MS
  ) {
    Serial.println("[TOUCH] Ignored by debounce");
    return;
  }

  lastTouchEventMs = now;

  Serial.printf(
    "[TOUCH] Pressed: x=%ld, y=%ld\n",
    static_cast<long>(touch.x),
    static_cast<long>(touch.y)
  );

  sendTouchTapEvent(touch.x, touch.y);
}

void webSocketEvent(
  WStype_t type,
  uint8_t* payload,
  size_t length
) {
  switch (type) {
    case WStype_CONNECTED:
      Serial.println("[WS] Connected to Render");

      /*
       * 连接成功时只设置基础表情。
       * 当前 face_effect 状态不强制修改。
       */
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
      DeserializationError error =
        deserializeJson(doc, payload, length);

      if (error) {
        Serial.printf(
          "[WS] JSON parse failed: %s\n",
          error.c_str()
        );
        return;
      }

      const char* expression = doc["expression"];
      const char* faceEffect = doc["face_effect"];
      const char* motion = doc["motion"];
      const char* sound = doc["sound"];
      const char* action = doc["action"];

      /*
       * 只有 Render 明确发送 text_to_display 时才更新文字，
       * 避免旧版 expression + motion 调用清除已有文字。
       */
      JsonVariantConst textToDisplay =
        doc["text_to_display"];

      if (
        !textToDisplay.isNull() &&
        textToDisplay.is<const char*>()
      ) {
        const char* text =
          textToDisplay.as<const char*>();

        unsigned long durationMs =
          doc["display_duration_ms"] |
          DEFAULT_DISPLAY_DURATION_MS;

        setSpeechTextForDuration(text, durationMs);
      }

      if (expression != nullptr) {
        Serial.printf(
          "[ACTION] Expression: %s\n",
          expression
        );
        setExpressionByName(expression);
      }

      if (faceEffect != nullptr) {
        Serial.printf(
          "[ACTION] Face effect: %s\n",
          faceEffect
        );
        setFaceEffectByName(faceEffect);
      }

      if (motion != nullptr) {
        Serial.printf(
          "[ACTION] Motion: %s\n",
          motion
        );

        if (strcmp(motion, "nod") == 0) {
          startNod();

        } else if (strcmp(motion, "home") == 0) {
          cancelGesture();
          M5StackChan.Motion.goHome(500);
          Serial.println("[GESTURE] Home sent");

        } else if (strcmp(motion, "none") == 0) {
          Serial.println(
            "[GESTURE] No motion requested"
          );

        } else if (strcmp(motion, "shake") == 0) {
          /*
           * 暂不执行：尚未在实体设备验证安全范围。
           */
          Serial.println(
            "[GESTURE] Shake ignored: "
            "not hardware-tested yet"
          );
        }

      } else if (
        action != nullptr &&
        strcmp(action, "home") == 0
      ) {
        cancelGesture();
        M5StackChan.Motion.goHome(500);
        Serial.println("[GESTURE] Home sent");
      }

      if (sound != nullptr) {
        Serial.printf(
          "[ACTION] Sound: %s\n",
          sound
        );
        playSoundByName(sound);
      }

      break;
    }

    default:
      break;
  }
}


LedTheme getLedTheme() {
  FaceEffect effect = faceEffectState.get();

  /*
   * 自定义 face_effect 的优先级高于 expression。
   */
  switch (effect) {
    case FaceEffect::HeartEyes:
    case FaceEffect::KissFace:
      return {255, 35, 130, 180, 2400};

    case FaceEffect::SparkleEyes:
    case FaceEffect::LaughFace:
      return {255, 180, 20, 210, 1600};

    case FaceEffect::DizzyEyes:
    case FaceEffect::ConfusedFace:
      return {100, 30, 255, 150, 2200};

    case FaceEffect::TearEyes:
      return {20, 100, 255, 130, 3500};

    case FaceEffect::SurprisedFace:
      return {255, 255, 255, 230, 1000};

    case FaceEffect::PoutFace:
      return {255, 80, 30, 150, 2600};

    case FaceEffect::ShyFace:
      return {255, 70, 140, 120, 3200};

    case FaceEffect::SmugFace:
      return {150, 40, 220, 160, 2200};

    case FaceEffect::NervousFace:
      return {255, 100, 20, 120, 1100};

    case FaceEffect::RelievedFace:
      return {30, 220, 100, 140, 3600};

    case FaceEffect::DeterminedFace:
      return {20, 210, 180, 180, 1800};

    case FaceEffect::None:
    default:
      break;
  }

  /*
   * 没有 face_effect 时，使用原生 expression 的主题。
   */
  switch (avatar.getExpression()) {
    case Expression::Happy:
      return {255, 180, 0, 170, 2200};

    case Expression::Sad:
      return {0, 80, 255, 120, 3800};

    case Expression::Angry:
      return {255, 0, 0, 190, 900};

    case Expression::Doubt:
      return {150, 40, 220, 130, 2400};

    case Expression::Sleepy:
      return {30, 20, 120, 80, 5000};

    case Expression::Neutral:
    default:
      return {20, 30, 50, 90, 3200};
  }
}


void updateLedBreath() {
  const unsigned long now = millis();

  if (now - lastLedUpdateMs < LED_UPDATE_INTERVAL_MS) {
    return;
  }

  lastLedUpdateMs = now;

  const LedTheme theme = getLedTheme();

  /*
   * wave 的范围是 0.0 ~ 1.0。
   */
  const float phase =
      static_cast<float>(now % theme.periodMs) /
      static_cast<float>(theme.periodMs);

  const float wave =
      (sinf(phase * 6.2831853f) + 1.0f) * 0.5f;

  /*
   * 基础亮度范围：
   *
   * - 最低：该主题最大亮度的 15%
   * - 最高：该主题最大亮度的 100%
   *
   * 以 theme.maxBrightness / 255.0f 为上限，
   * 避免计算值超过 1.0 而造成 uint8_t 溢出。
   */
  const float maxBrightness =
      static_cast<float>(theme.maxBrightness) / 255.0f;

  const float minBrightness = maxBrightness * 0.15f;

  float brightness =
      minBrightness +
      (maxBrightness - minBrightness) * wave;

  /*
   * 屏幕有文字时，同色系亮度增强。
   *
   * 若文字被 updateSpeechText() 清除，activeSpeechText 会成为空字符串，
   * 本轮即可自然恢复为普通呼吸灯。
   */
  if (activeSpeechText.length() > 0) {
    brightness *= SPEAKING_LIGHT_BOOST;
  }
  /*
   * 摸头临时灯效具有最高优先级。
   *
   * 两次粉色柔和闪烁：
   * 亮起 -> 回落 -> 再亮起 -> 再回落。
   */
  bool useHeadTouchPink = false;

  if (headTouchLightActive) {
    const unsigned long totalDuration =
        HEAD_TOUCH_LIGHT_CYCLE_MS * HEAD_TOUCH_LIGHT_FLASH_COUNT;

    const unsigned long elapsed =
        now - headTouchLightStartedMs;

    if (elapsed >= totalDuration) {
      headTouchLightActive = false;

      Serial.println("[LED] Head-touch pink flash finished");
    } else {
      const unsigned long cycleElapsed =
          elapsed % HEAD_TOUCH_LIGHT_CYCLE_MS;

      const float cyclePhase =
          static_cast<float>(cycleElapsed) /
          static_cast<float>(HEAD_TOUCH_LIGHT_CYCLE_MS);

      /*
       * sin(0 -> PI) 的范围为 0 -> 1 -> 0。
       */
      const float flashWave =
          sinf(cyclePhase * 3.14159265f);

      /*
       * 摸头时使用固定粉色，并且不再使用当前表情主题色。
       *
       * 粉色 RGB：
       * R = 255
       * G = 45
       * B = 150
       */
      useHeadTouchPink = true;

      /*
       * 粉色灯效使用独立亮度，避免被当前主题的
       * maxBrightness 限制得太暗。
       */
      brightness =
          (220.0f / 255.0f) *
          (0.35f + flashWave * 0.65f);
    }
  }

  /*
   * 防止亮度超过 RGB 可用范围。
   */
  if (brightness > 1.0f) {
    brightness = 1.0f;
  }

  uint8_t r;
  uint8_t g;
  uint8_t b;

  if (useHeadTouchPink) {
    /*
     * 固定粉色摸头灯效。
     */
    r = static_cast<uint8_t>(255.0f * brightness);
    g = static_cast<uint8_t>(45.0f * brightness);
    b = static_cast<uint8_t>(150.0f * brightness);
  } else {
    /*
     * 非摸头期间，恢复原来的 expression /
     * face_effect 主题色呼吸灯。
     */
    r = static_cast<uint8_t>(theme.r * brightness);
    g = static_cast<uint8_t>(theme.g * brightness);
    b = static_cast<uint8_t>(theme.b * brightness);
  }

  for (uint8_t i = 0; i < LED_COUNT; ++i) {
    M5StackChan.setRgbColor(i, r, g, b);
  }

  M5StackChan.refreshRgb();
}



void startHeadTouchLightEffect() {
  headTouchLightActive = true;
  headTouchLightStartedMs = millis();

  Serial.println("[LED] Head-touch soft flash started");
}



void setup() {
  Serial.begin(115200);
  delay(1000);

  Serial.println();
  Serial.println(
    "=== StackChan WiFi + Render + Servo + Display + Touch firmware ==="
  );

  /*
   * StackChan-BSP 负责设备、舵机供电和舵机总线初始化。
   */
  M5StackChan.begin();

  /*
 * 让首次 updateLedBreath() 不受 40ms 刷新间隔限制。
 */
lastLedUpdateMs = millis() - LED_UPDATE_INTERVAL_MS;
updateLedBreath();


  /*
   * CoreS3 官方 SD 卡初始化。
   */
  SPI.begin(
    SD_SPI_SCK_PIN,
    SD_SPI_MISO_PIN,
    SD_SPI_MOSI_PIN,
    SD_SPI_CS_PIN
  );

  sdCardReady = SD.begin(
    SD_SPI_CS_PIN,
    SPI,
    25000000
  );

  if (sdCardReady) {
    Serial.println("[SD] Card mounted");

    Serial.printf(
      "[SD] /message.wav: %s\n",
      SD.exists("/message.wav")
        ? "found"
        : "missing"
    );

    Serial.printf(
      "[SD] /emotion.wav: %s\n",
      SD.exists("/emotion.wav")
        ? "found"
        : "missing"
    );
  } else {
    Serial.println("[SD] Card mount failed");
  }

  /*
   * M5Unified 音量范围为 0 至 255。
   */
  M5.Speaker.setVolume(80);

  M5StackChan.Motion.setAutoAngleSyncEnabled(false);
  M5StackChan.Motion.setAutoTorqueReleaseEnabled(true);
  M5StackChan.Motion.goHome(500);

  /*
   * Avatar 尚未启动，因此这里可以安全操作 M5.Display。
   */
  showBootMessage(
    "WiFi starting...",
    "Please wait"
  );

  prefs.begin("stackchan", false);

  String savedHost =
    prefs.getString("server_host", "");

  savedHost.toCharArray(
    ws_host,
    sizeof(ws_host)
  );

  Serial.printf(
    "[CONFIG] Saved Render host: '%s'\n",
    ws_host
  );

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

  Serial.println(
    "[WIFI] Trying saved WiFi credentials..."
  );

  if (!wm.autoConnect("StackChan-Setup")) {
    Serial.println(
      "[WIFI] Could not join saved WiFi."
    );

    Serial.println(
      "[WIFI] Starting config portal: "
      "StackChan-Setup"
    );

    showBootMessage(
      "WiFi setup",
      "AP: StackChan-Setup\n"
      "Open: 192.168.4.1"
    );

    /*
     * 不设置门户超时，等待用户完成配网。
     */
    wm.startConfigPortal("StackChan-Setup");
  }

  Serial.printf(
    "[WIFI] Status: %d\n",
    WiFi.status()
  );

  Serial.printf(
    "[WIFI] IP: %s\n",
    WiFi.localIP().toString().c_str()
  );

  const char* enteredHost =
    serverParameter.getValue();

  if (
    enteredHost != nullptr &&
    strlen(enteredHost) > 0
  ) {
    strncpy(
      ws_host,
      enteredHost,
      sizeof(ws_host) - 1
    );

    ws_host[sizeof(ws_host) - 1] = '\0';

    prefs.putString(
      "server_host",
      ws_host
    );
  }

  Serial.printf(
    "[CONFIG] Active Render host: '%s'\n",
    ws_host
  );

  /*
   * Wi-Fi 配置完成后启动 Avatar。
   */
  avatar.init();

  /*
   * 让 Avatar 的 Balloon 使用 M5GFX 内置中文点阵字体。
   */
  avatar.setSpeechFont(&fonts::efontCN_10);

  /*
   * 默认状态：
   * - 原生 neutral 表情；
   * - 原生眼睛；
   * - 无文字。
   */
  faceEffectState.set(FaceEffect::None);
  avatar.setExpression(Expression::Neutral);
  avatar.setSpeechText("");

  if (WiFi.status() != WL_CONNECTED) {
    Serial.println(
      "[WIFI] Not connected; WebSocket will not start"
    );
    return;
  }

  if (strlen(ws_host) == 0) {
    Serial.println("[WS] Render host is empty.");

    Serial.println(
      "[WS] Open StackChan-Setup and fill "
      "Render Server Host."
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
  webSocket.beginSSL(
    ws_host,
    ws_port,
    ws_path
  );
}

void loop() {
  M5StackChan.update();
  webSocket.loop();

  updateGesture();
  updateSpeechText();
  updateTouchInput();
  updateHeadTouchInput();

  updateLedBreath();

  delay(10);
}
