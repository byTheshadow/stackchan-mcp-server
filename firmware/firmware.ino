#include <Arduino.h>
#include <M5Unified.h>
#include <M5StackChan.h>

#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

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
 * ============================================================
 * 基础状态
 * ============================================================
 */

FaceEffectState faceEffectState;


/*
 * ============================================================
 * Avatar
 * ============================================================
 *
 * Face 构造参数顺序：
 *
 * mouth,
 * right eye,
 * left eye,
 * right eyebrow,
 * left eyebrow
 */

Avatar avatar(
  new Face(
    new CustomMouth(&faceEffectState),
    new CustomEye(false, &faceEffectState),
    new CustomEye(true, &faceEffectState),
    new CustomEyeblow(false, &faceEffectState),
    new CustomEyeblow(true, &faceEffectState)
  )
);


/*
 * ============================================================
 * WebSocket 与网络配置
 * ============================================================
 */

WebSocketsClient webSocket;
Preferences prefs;

char wsHost[128] = "";
char wsToken[192] = "";

static constexpr uint16_t WS_PORT = 443;
static constexpr char WS_PATH[] = "/";

String authenticatedWsPath;


/*
 * WebSocket Token 必须与 Render 上的 ROBOT_WS_TOKEN 相同。
 *
 * 服务端连接形式：
 *
 * wss://your-render-host/?token=ROBOT_WS_TOKEN
 */


/*
 * ============================================================
 * SD 卡配置
 * ============================================================
 */

static constexpr int SD_SPI_CS_PIN = 4;
static constexpr int SD_SPI_SCK_PIN = 36;
static constexpr int SD_SPI_MISO_PIN = 35;
static constexpr int SD_SPI_MOSI_PIN = 37;

bool sdCardReady = false;


/*
 * ============================================================
 * RGB 灯光
 * ============================================================
 */

struct LedTheme {
  uint8_t r;
  uint8_t g;
  uint8_t b;
  uint8_t maxBrightness;
  uint16_t periodMs;
};

LedTheme currentLedTheme = {
  20,
  20,
  30,
  80,
  3000
};

static constexpr uint8_t LED_COUNT = 12;
static constexpr unsigned long LED_UPDATE_INTERVAL_MS = 40;
static constexpr float SPEAKING_LIGHT_BOOST = 1.22f;

unsigned long lastLedUpdateMs = 0;


/*
 * 头顶触摸灯效。
 */

bool headTouchLightActive = false;
unsigned long headTouchLightStartedMs = 0;

static constexpr unsigned long HEAD_TOUCH_LIGHT_CYCLE_MS = 600;
static constexpr uint8_t HEAD_TOUCH_LIGHT_FLASH_COUNT = 2;


/*
 * ============================================================
 * 屏幕文字
 * ============================================================
 */

String activeSpeechText = "";

bool speechClearScheduled = false;
unsigned long speechClearAtMs = 0;

static constexpr unsigned long DEFAULT_DISPLAY_DURATION_MS = 5000;
static constexpr unsigned long MIN_DISPLAY_DURATION_MS = 1000;
static constexpr unsigned long MAX_DISPLAY_DURATION_MS = 10000;


/*
 * ============================================================
 * 屏幕触摸
 * ============================================================
 */

static constexpr unsigned long TOUCH_EVENT_DEBOUNCE_MS = 700;
unsigned long lastTouchEventMs = 0;


/*
 * ============================================================
 * 头顶触摸
 * ============================================================
 */

static constexpr unsigned long HEAD_TOUCH_DEBOUNCE_MS = 700;
static constexpr uint16_t HEAD_TOUCH_THRESHOLD = 1;

unsigned long lastHeadTouchEventMs = 0;


/*
 * ============================================================
 * IMU 摇晃检测
 * ============================================================
 */

bool imuReady = false;

float lastAccelX = 0.0f;
float lastAccelY = 0.0f;
float lastAccelZ = 0.0f;

unsigned long lastImuReadMs = 0;
unsigned long lastShakeEventMs = 0;

static constexpr unsigned long IMU_READ_INTERVAL_MS = 50;
static constexpr unsigned long SHAKE_DEBOUNCE_MS = 1800;

static constexpr float SHAKE_DELTA_THRESHOLD = 1.20f;


/*
 * 连续采样防误触发。
 */

uint8_t shakeCandidateCount = 0;

static constexpr uint8_t SHAKE_REQUIRED_SAMPLES = 2;


/*
 * 摇晃后的临时眩晕表情。
 */

bool shakeDizzyActive = false;

FaceEffect shakePreviousFaceEffect =
  FaceEffect::None;

unsigned long shakeDizzyUntilMs = 0;

static constexpr unsigned long SHAKE_DIZZY_DURATION_MS = 2200;


/*
 * ============================================================
 * 舵机动作
 * ============================================================
 */

enum GestureKind {
  GESTURE_NONE = 0,
  GESTURE_NOD,
  GESTURE_SHAKE_HEAD,
  GESTURE_LOOK_LEFT,
  GESTURE_LOOK_RIGHT,
  GESTURE_TILT_UP
};

GestureKind activeGesture = GESTURE_NONE;
uint8_t gestureStep = 0;
unsigned long nextGestureStepMs = 0;


/*
 * ============================================================
 * 网络音频
 * ============================================================
 *
 * 服务端发送：
 *
 * 1. audio_start JSON
 * 2. 等待 audio_ready
 * 3. 发送二进制 PCM
 * 4. audio_end JSON
 * 5. 固件开始播放
 * 6. 发送 audio_started
 *
 * 音频格式：
 *
 * PCM signed 16-bit little-endian
 * 16000 Hz
 * mono
 */

static constexpr char NETWORK_AUDIO_PART_PATH[] =
  "/network_audio.wav.part";

static constexpr char NETWORK_AUDIO_PATH[] =
  "/network_audio.wav";

/*
 * 限制解码后的 PCM 文件大小。
 */
static constexpr size_t NETWORK_AUDIO_MAX_BYTES =
  32 * 1024 * 1024;

/*
 * 限制单个 WebSocket 二进制帧大小。
 */
static constexpr size_t NETWORK_AUDIO_MAX_FRAME_BYTES =
  8192;

/*
 * 两个 PCM 数据帧之间允许的最大间隔。
 */
static constexpr unsigned long NETWORK_AUDIO_TIMEOUT_MS =
  30000;

/*
 * 定期 flush 的间隔。
 */
static constexpr uint32_t NETWORK_AUDIO_FLUSH_INTERVAL_BYTES =
  16384U;

bool networkAudioReceiving = false;
bool networkAudioReady = false;

String networkAudioStreamId = "";

File networkAudioFile;

uint32_t networkAudioPcmBytes = 0;
uint32_t lastNetworkAudioFlushBytes = 0;
unsigned long lastNetworkAudioPacketMs = 0;



/*
 * WAV 播放缓存。
 */

static constexpr size_t WAV_BUFFER_COUNT = 3;
static constexpr size_t WAV_BUFFER_SIZE = 1024;

uint8_t wavData[WAV_BUFFER_COUNT][WAV_BUFFER_SIZE];


/*
 * ============================================================
 * WAV 结构
 * ============================================================
 */

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
 * ============================================================
 * 函数声明
 * ============================================================
 */

void updateGesture();
void cancelGesture();

void startNod();
void startShakeHead();
void startLookLeft();
void startLookRight();
void startTiltUp();

void updateTouchInput();
void updateHeadTouchInput();
void updateShakeDetection();
void updateShakeDizzyEffect();

void sendTouchTapEvent(int32_t x, int32_t y);
void sendHeadTouchEvent();
void sendShakeEvent();

void updateSpeechText();
void updateLedBreath();
void startHeadTouchLightEffect();

void handleWebSocketText(
  uint8_t* payload,
  size_t length
);

void handleWebSocketBinary(
  uint8_t* payload,
  size_t length
);

void handleAudioStart(JsonDocument& doc);
void handleAudioEnd(JsonDocument& doc);
void handleAudioStop(JsonDocument& doc);
void handleAudioAbort(JsonDocument& doc);

void resetNetworkAudioState(bool removeFiles);
bool writeNetworkAudioWavHeader(
  File& file,
  uint32_t pcmBytes
) {
  if (
    !file ||
    pcmBytes == 0 ||
    pcmBytes > NETWORK_AUDIO_MAX_BYTES ||
    (pcmBytes & 1U) != 0
  ) {
    return false;
  }

  /*
   * RIFF chunk size = 36 + PCM 数据长度。
   *
   * 32 MB 限制下不会发生 uint32_t 溢出。
   */
  const uint32_t riffChunkSize =
    36U + pcmBytes;

  uint8_t header[44] = {};

  memcpy(header + 0, "RIFF", 4);

  writeLe32(
    header,
    4,
    riffChunkSize
  );

  memcpy(header + 8, "WAVE", 4);
  memcpy(header + 12, "fmt ", 4);

  /*
   * fmt chunk。
   */
  writeLe32(header, 16, 16);
  writeLe16(header, 20, 1);
  writeLe16(header, 22, 1);
  writeLe32(header, 24, 16000);
  writeLe32(header, 28, 32000);
  writeLe16(header, 32, 2);
  writeLe16(header, 34, 16);

  /*
   * data chunk。
   */
  memcpy(header + 36, "data", 4);

  writeLe32(
    header,
    40,
    pcmBytes
  );

  if (!file.seek(0)) {
    return false;
  }

  const size_t written =
    file.write(
      header,
      sizeof(header)
    );

  return written == sizeof(header);
}



/*
 * ============================================================
 * 通用辅助函数
 * ============================================================
 */

bool elapsedMs(
  unsigned long now,
  unsigned long previous,
  unsigned long interval
) {
  return static_cast<unsigned long>(
    now - previous
  ) >= interval;
}


unsigned long clampDisplayDuration(
  unsigned long durationMs
) {
  if (durationMs < MIN_DISPLAY_DURATION_MS) {
    return MIN_DISPLAY_DURATION_MS;
  }

  if (durationMs > MAX_DISPLAY_DURATION_MS) {
    return MAX_DISPLAY_DURATION_MS;
  }

  return durationMs;
}


/*
 * ============================================================
 * SD WAV 播放
 * ============================================================
 *
 * 支持：
 *
 * - PCM；
 * - 8-bit 或 16-bit；
 * - mono 或 stereo；
 * - 任意正常 WAV data chunk 位置。
 */

bool playSdWav(const char* filename) {
  if (!sdCardReady) {
    Serial.println(
      "[AUDIO] SD card is not ready"
    );

    return false;
  }

  if (
    filename == nullptr ||
    !SD.exists(filename)
  ) {
    Serial.printf(
      "[AUDIO] WAV file not found: %s\n",
      filename == nullptr ? "(null)" : filename
    );

    return false;
  }

  File file = SD.open(
    filename,
    FILE_READ
  );

  if (!file) {
    Serial.printf(
      "[AUDIO] Failed to open: %s\n",
      filename
    );

    return false;
  }

  WavHeader header = {};

  const size_t headerBytesRead =
    file.read(
      reinterpret_cast<uint8_t*>(&header),
      sizeof(header)
    );

  if (headerBytesRead != sizeof(header)) {
    Serial.println(
      "[AUDIO] WAV header read failed"
    );

    file.close();
    return false;
  }

  Serial.printf(
    "[AUDIO] WAV %s: format=%u, channels=%u, "
    "rate=%lu, bits=%u\n",
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
    header.channels == 0 ||
    header.channels > 2 ||
    header.sampleRate == 0 ||
    header.fmtChunkSize < 16 ||
    (
      header.bitsPerSample != 8 &&
      header.bitsPerSample != 16
    )
  ) {
    Serial.println(
      "[AUDIO] Unsupported WAV format"
    );

    file.close();
    return false;
  }

  const uint32_t afterFmtOffset =
    offsetof(WavHeader, audioFormat) +
    header.fmtChunkSize;

  if (!file.seek(afterFmtOffset)) {
    Serial.println(
      "[AUDIO] Failed to seek after fmt chunk"
    );

    file.close();
    return false;
  }

  WavSubChunk subChunk = {};
  bool dataChunkFound = false;

  while (
    file.available() >=
      static_cast<int>(sizeof(WavSubChunk)) &&
    file.read(
      reinterpret_cast<uint8_t*>(&subChunk),
      sizeof(subChunk)
    ) == sizeof(subChunk)
  ) {
    if (
      memcmp(
        subChunk.identifier,
        "data",
        4
      ) == 0
    ) {
      dataChunkFound = true;
      break;
    }

    uint32_t nextChunkOffset =
      static_cast<uint32_t>(file.position()) +
      subChunk.chunkSize;

    if (subChunk.chunkSize & 1U) {
      nextChunkOffset++;
    }

    if (!file.seek(nextChunkOffset)) {
      break;
    }
  }

  if (!dataChunkFound) {
    Serial.println(
      "[AUDIO] WAV data chunk not found"
    );

    file.close();
    return false;
  }

  uint32_t remaining = subChunk.chunkSize;
  size_t bufferIndex = 0;

  const bool stereo =
    header.channels == 2;

  const bool is16Bit =
    header.bitsPerSample == 16;

  Serial.printf(
    "[AUDIO] Playing %s, data=%lu bytes\n",
    filename,
    static_cast<unsigned long>(remaining)
  );

  while (remaining > 0) {
    const size_t requested =
      remaining < WAV_BUFFER_SIZE
        ? remaining
        : WAV_BUFFER_SIZE;

    const size_t bytesRead =
      file.read(
        wavData[bufferIndex],
        requested
      );

    if (bytesRead == 0) {
      Serial.println(
        "[AUDIO] Unexpected end of WAV data"
      );

      file.close();
      return false;
    }

    remaining -= bytesRead;

    if (is16Bit) {
      if ((bytesRead & 1U) != 0) {
        Serial.println(
          "[AUDIO] Invalid 16-bit WAV chunk"
        );

        file.close();
        return false;
      }

      M5.Speaker.playRaw(
        reinterpret_cast<const int16_t*>(
          wavData[bufferIndex]
        ),
        bytesRead / 2,
        header.sampleRate,
        stereo,
        1,
        0
      );
    } else {
      M5.Speaker.playRaw(
        reinterpret_cast<const uint8_t*>(
          wavData[bufferIndex]
        ),
        bytesRead,
        header.sampleRate,
        stereo,
        1,
        0
      );
    }

    bufferIndex++;

    if (bufferIndex >= WAV_BUFFER_COUNT) {
      bufferIndex = 0;
    }

    delay(1);
  }

  file.close();

  Serial.println(
    "[AUDIO] WAV data queued"
  );

  return true;
}


/*
 * ============================================================
 * 小端序写入
 * ============================================================
 */

void writeLe16(
  uint8_t* buffer,
  size_t offset,
  uint16_t value
) {
  buffer[offset] =
    static_cast<uint8_t>(value & 0xFF);

  buffer[offset + 1] =
    static_cast<uint8_t>(
      (value >> 8) & 0xFF
    );
}


void writeLe32(
  uint8_t* buffer,
  size_t offset,
  uint32_t value
) {
  buffer[offset] =
    static_cast<uint8_t>(value & 0xFF);

  buffer[offset + 1] =
    static_cast<uint8_t>(
      (value >> 8) & 0xFF
    );

  buffer[offset + 2] =
    static_cast<uint8_t>(
      (value >> 16) & 0xFF
    );

  buffer[offset + 3] =
    static_cast<uint8_t>(
      (value >> 24) & 0xFF
    );
}





/*
 * ============================================================
 * 音频状态发送
 * ============================================================
 */

void sendAudioStatus(
  const char* type,
  const char* streamId,
  const char* error
) {
  if (
    type == nullptr ||
    !webSocket.isConnected()
  ) {
    return;
  }

  StaticJsonDocument<512> doc;

  doc["type"] = type;

  if (streamId != nullptr) {
    doc["stream_id"] = streamId;
  }

  if (error != nullptr) {
    doc["error"] = error;
  }

  String output;
  serializeJson(doc, output);

  webSocket.sendTXT(output);
}


/*
 * ============================================================
 * 网络音频状态清理
 * ============================================================
 */

 void resetNetworkAudioState(
  bool removeFiles
) {
  if (networkAudioFile) {
    networkAudioFile.flush();
    networkAudioFile.close();
  }

  networkAudioReceiving = false;
  networkAudioReady = false;
  networkAudioStreamId = "";
  networkAudioPcmBytes = 0;
  lastNetworkAudioFlushBytes = 0;
  lastNetworkAudioPacketMs = 0;

  if (!removeFiles || !sdCardReady) {
    return;
  }

  if (SD.exists(NETWORK_AUDIO_PART_PATH)) {
    SD.remove(NETWORK_AUDIO_PART_PATH);
  }

  if (SD.exists(NETWORK_AUDIO_PATH)) {
    SD.remove(NETWORK_AUDIO_PATH);
  }
}



/*
 * ============================================================
 * 收到 audio_start
 * ============================================================
 */
void handleAudioStart(
  JsonDocument& doc
) {
  const char* streamId =
    doc["stream_id"] | "";

  const char* format =
    doc["format"] | "";

  const uint32_t sampleRate =
    doc["sample_rate"] | 0;

  const uint16_t channels =
    doc["channels"] | 0;

  const uint16_t bitsPerSample =
    doc["bits_per_sample"] | 0;

  if (!sdCardReady) {
    sendAudioStatus(
      "audio_error",
      streamId,
      "SD card is not ready"
    );

    return;
  }

  if (
    streamId[0] == '\0' ||
    strlen(streamId) > 96 ||
    strcmp(format, "pcm_s16le") != 0 ||
    sampleRate != 16000 ||
    channels != 1 ||
    bitsPerSample != 16
  ) {
    sendAudioStatus(
      "audio_error",
      streamId,
      "Unsupported audio format"
    );

    return;
  }

  /*
   * 终止旧播放和旧接收任务。
   */
  M5.Speaker.stop();
  resetNetworkAudioState(true);

  /*
   * 防止旧的 .part 文件被 FILE_WRITE 以追加方式继续写入。
   */
  if (SD.exists(NETWORK_AUDIO_PART_PATH)) {
    SD.remove(NETWORK_AUDIO_PART_PATH);
  }

  if (SD.exists(NETWORK_AUDIO_PATH)) {
    SD.remove(NETWORK_AUDIO_PATH);
  }

  networkAudioFile =
    SD.open(
      NETWORK_AUDIO_PART_PATH,
      FILE_WRITE
    );

  if (!networkAudioFile) {
    sendAudioStatus(
      "audio_error",
      streamId,
      "Cannot open audio file"
    );

    return;
  }

  uint8_t emptyHeader[44] = {};

  const size_t placeholderWritten =
    networkAudioFile.write(
      emptyHeader,
      sizeof(emptyHeader)
    );

  if (placeholderWritten != sizeof(emptyHeader)) {
    resetNetworkAudioState(true);

    sendAudioStatus(
      "audio_error",
      streamId,
      "Cannot write WAV placeholder"
    );

    return;
  }

  if (!networkAudioFile.flush()) {
    resetNetworkAudioState(true);

    sendAudioStatus(
      "audio_error",
      streamId,
      "Cannot flush WAV placeholder"
    );

    return;
  }

  networkAudioStreamId = streamId;
  networkAudioPcmBytes = 0;
  lastNetworkAudioFlushBytes = 0;
  networkAudioReceiving = true;
  networkAudioReady = false;
  lastNetworkAudioPacketMs = millis();

  Serial.printf(
    "[AUDIO] Receiving stream: %s\n",
    networkAudioStreamId.c_str()
  );

  /*
   * 只有发送 audio_ready 后，Render 端才允许发送 PCM。
   */
  sendAudioStatus(
    "audio_ready",
    networkAudioStreamId.c_str()
  );
}



/*
 * ============================================================
 * 收到网络音频二进制帧
 * ============================================================
 */
void handleWebSocketBinary(
  uint8_t* payload,
  size_t length
) {
  if (
    payload == nullptr ||
    length == 0
  ) {
    return;
  }

  /*
   * 限制单个 WebSocket PCM 帧大小。
   */
  if (
    length > NETWORK_AUDIO_MAX_FRAME_BYTES
  ) {
    const String streamId =
      networkAudioStreamId;

    resetNetworkAudioState(true);

    sendAudioStatus(
      "audio_error",
      streamId.c_str(),
      "Audio frame is too large"
    );

    Serial.printf(
      "[AUDIO] Rejected oversized frame: %u bytes\n",
      static_cast<unsigned>(length)
    );

    return;
  }

  if (
    !networkAudioReceiving ||
    !networkAudioFile ||
    networkAudioStreamId.length() == 0
  ) {
    Serial.println(
      "[AUDIO] Ignored binary frame"
    );

    return;
  }

  /*
   * 使用减法判断，避免 networkAudioPcmBytes + length
   * 在边界情况下发生整数溢出。
   */
  if (
    networkAudioPcmBytes >
      NETWORK_AUDIO_MAX_BYTES ||
    length >
      NETWORK_AUDIO_MAX_BYTES -
      networkAudioPcmBytes
  ) {
    const String streamId =
      networkAudioStreamId;

    resetNetworkAudioState(true);

    sendAudioStatus(
      "audio_error",
      streamId.c_str(),
      "Audio exceeds maximum size"
    );

    return;
  }

  /*
   * PCM 为 16-bit little-endian。
   * 单个 WebSocket 帧可以是奇数长度，因为一个 sample
   * 可能跨越两个 WebSocket 帧。
   * 因此这里只在 audio_end 时检查总长度是否为偶数。
   */
  const size_t written =
    networkAudioFile.write(
      payload,
      length
    );

  if (written != length) {
    const String streamId =
      networkAudioStreamId;

    resetNetworkAudioState(true);

    sendAudioStatus(
      "audio_error",
      streamId.c_str(),
      "SD write failed"
    );

    Serial.printf(
      "[AUDIO] SD short write: %u/%u bytes\n",
      static_cast<unsigned>(written),
      static_cast<unsigned>(length)
    );

    return;
  }

  networkAudioPcmBytes +=
    static_cast<uint32_t>(length);

  lastNetworkAudioPacketMs = millis();

  /*
   * 正确处理跨越 16 KB 边界的情况。
   */
  if (
    networkAudioPcmBytes -
    lastNetworkAudioFlushBytes >=
    NETWORK_AUDIO_FLUSH_INTERVAL_BYTES
  ) {
    if (!networkAudioFile.flush()) {
      const String streamId =
        networkAudioStreamId;

      resetNetworkAudioState(true);

      sendAudioStatus(
        "audio_error",
        streamId.c_str(),
        "SD flush failed"
      );

      return;
    }

    lastNetworkAudioFlushBytes =
      networkAudioPcmBytes;
  }
}



/*
 * ============================================================
 * 收到 audio_end
 * ============================================================
 */
void handleAudioEnd(
  JsonDocument& doc
) {
  const char* streamId =
    doc["stream_id"] | "";

  const uint32_t announcedPcmBytes =
    doc["pcm_bytes"] | 0;

  if (
    !networkAudioReceiving ||
    networkAudioStreamId != streamId
  ) {
    Serial.println(
      "[AUDIO] Ignored unexpected audio_end"
    );

    return;
  }

  if (
    announcedPcmBytes != 0 &&
    announcedPcmBytes != networkAudioPcmBytes
  ) {
    const String currentStreamId =
      networkAudioStreamId;

    resetNetworkAudioState(true);

    sendAudioStatus(
      "audio_error",
      currentStreamId.c_str(),
      "PCM byte count mismatch"
    );

    return;
  }

  /*
   * PCM signed 16-bit 必须包含完整的 sample。
   */
  if (
    networkAudioPcmBytes == 0 ||
    (networkAudioPcmBytes & 1U) != 0
  ) {
    const String currentStreamId =
      networkAudioStreamId;

    resetNetworkAudioState(true);

    sendAudioStatus(
      "audio_error",
      currentStreamId.c_str(),
      "Invalid or empty PCM audio"
    );

    return;
  }

  if (!networkAudioFile.flush()) {
    const String currentStreamId =
      networkAudioStreamId;

    resetNetworkAudioState(true);

    sendAudioStatus(
      "audio_error",
      currentStreamId.c_str(),
      "SD flush failed"
    );

    return;
  }

  networkAudioFile.close();

  /*
   * FILE_WRITE 在部分 Arduino SD 实现中会以追加方式打开。
   * 这里重新打开后通过 seek(0) 回写头部。
   */
  File file =
    SD.open(
      NETWORK_AUDIO_PART_PATH,
      FILE_WRITE
    );

  if (!file) {
    const String currentStreamId =
      networkAudioStreamId;

    resetNetworkAudioState(true);

    sendAudioStatus(
      "audio_error",
      currentStreamId.c_str(),
      "Cannot reopen WAV file"
    );

    return;
  }

  const bool headerOk =
    writeNetworkAudioWavHeader(
      file,
      networkAudioPcmBytes
    );

  const bool headerFlushed =
    file.flush();

  file.close();

  if (
    !headerOk ||
    !headerFlushed
  ) {
    const String currentStreamId =
      networkAudioStreamId;

    resetNetworkAudioState(true);

    sendAudioStatus(
      "audio_error",
      currentStreamId.c_str(),
      "Cannot write WAV header"
    );

    return;
  }

  /*
   * 只有完整的 .part 文件完成后才替换正式文件。
   */
  if (SD.exists(NETWORK_AUDIO_PATH)) {
    SD.remove(NETWORK_AUDIO_PATH);
  }

  if (
    !SD.rename(
      NETWORK_AUDIO_PART_PATH,
      NETWORK_AUDIO_PATH
    )
  ) {
    const String currentStreamId =
      networkAudioStreamId;

    resetNetworkAudioState(true);

    sendAudioStatus(
      "audio_error",
      currentStreamId.c_str(),
      "Cannot finalize WAV file"
    );

    return;
  }

  networkAudioReceiving = false;
  networkAudioReady = true;

  Serial.printf(
    "[AUDIO] Received %lu PCM bytes\n",
    static_cast<unsigned long>(
      networkAudioPcmBytes
    )
  );

  if (!startNetworkAudioPlayback()) {
    const String currentStreamId =
      networkAudioStreamId;

    resetNetworkAudioState(true);

    sendAudioStatus(
      "audio_error",
      currentStreamId.c_str(),
      "WAV playback failed"
    );

    return;
  }

  sendAudioStatus(
    "audio_started",
    networkAudioStreamId.c_str()
  );

  Serial.println(
    "[AUDIO] Network audio playback started"
  );
}


/*
 * ============================================================
 * 开始播放网络 WAV
 * ============================================================
 */

bool startNetworkAudioPlayback() {
  if (
    !networkAudioReady ||
    !sdCardReady ||
    !SD.exists(NETWORK_AUDIO_PATH)
  ) {
    return false;
  }

  const bool result =
    playSdWav(NETWORK_AUDIO_PATH);

  if (result) {
    Serial.println(
      "[AUDIO] Network WAV queued"
    );
  }

  return result;
}


/*
 * ============================================================
 * 收到 audio_stop
 * ============================================================
 */

void handleAudioStop(
  JsonDocument& doc
) {
  const char* streamId =
    doc["stream_id"] | "";

  if (
    streamId[0] != '\0' &&
    networkAudioStreamId.length() > 0 &&
    networkAudioStreamId != streamId
  ) {
    Serial.println(
      "[AUDIO] Ignored stop for another stream"
    );

    return;
  }

  M5.Speaker.stop();
  resetNetworkAudioState(true);

  Serial.println(
    "[AUDIO] Network audio stopped"
  );
}


/*
 * ============================================================
 * 收到 audio_abort
 * ============================================================
 */

void handleAudioAbort(
  JsonDocument& doc
) {
  handleAudioStop(doc);
}


/*
 * ============================================================
 * 网络音频接收超时
 * ============================================================
 */

void updateNetworkAudioTimeout() {
  if (!networkAudioReceiving) {
    return;
  }

  const unsigned long now = millis();

  if (
    elapsedMs(
      now,
      lastNetworkAudioPacketMs,
      NETWORK_AUDIO_TIMEOUT_MS
    )
  ) {
    const String streamId =
      networkAudioStreamId;

    Serial.println(
      "[AUDIO] Network audio receive timeout"
    );

    resetNetworkAudioState(true);

    sendAudioStatus(
      "audio_error",
      streamId.c_str(),
      "Audio receive timeout"
    );
  }
}


/*
 * ============================================================
 * 根据名称播放提示音
 * ============================================================
 */

void playSoundByName(
  const char* sound
) {
  if (
    sound == nullptr ||
    strcmp(sound, "none") == 0
  ) {
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

  Serial.printf(
    "[AUDIO] Unknown sound ignored: %s\n",
    sound
  );
}


/*
 * ============================================================
 * 表情
 * ============================================================
 */

void setExpressionByName(
  const char* expression
) {
  if (expression == nullptr) {
    return;
  }

  if (strcmp(expression, "happy") == 0) {
    avatar.setExpression(
      Expression::Happy
    );
  } else if (
    strcmp(expression, "sad") == 0
  ) {
    avatar.setExpression(
      Expression::Sad
    );
  } else if (
    strcmp(expression, "angry") == 0
  ) {
    avatar.setExpression(
      Expression::Angry
    );
  } else if (
    strcmp(expression, "doubt") == 0
  ) {
    avatar.setExpression(
      Expression::Doubt
    );
  } else if (
    strcmp(expression, "sleepy") == 0
  ) {
    avatar.setExpression(
      Expression::Sleepy
    );
  } else {
    avatar.setExpression(
      Expression::Neutral
    );
  }
}


void setFaceEffectByName(
  const char* effect
) {
  if (effect == nullptr) {
    return;
  }

  /*
   * 远程新指令覆盖临时摇晃表情。
   */
  shakeDizzyActive = false;
  shakeDizzyUntilMs = 0;

  if (strcmp(effect, "none") == 0) {
    faceEffectState.set(
      FaceEffect::None
    );
  } else if (
    strcmp(effect, "heart_eyes") == 0
  ) {
    faceEffectState.set(
      FaceEffect::HeartEyes
    );
  } else if (
    strcmp(effect, "sparkle_eyes") == 0
  ) {
    faceEffectState.set(
      FaceEffect::SparkleEyes
    );
  } else if (
    strcmp(effect, "dizzy_eyes") == 0
  ) {
    faceEffectState.set(
      FaceEffect::DizzyEyes
    );
  } else if (
    strcmp(effect, "tear_eyes") == 0
  ) {
    faceEffectState.set(
      FaceEffect::TearEyes
    );
  } else if (
    strcmp(effect, "surprised_face") == 0
  ) {
    faceEffectState.set(
      FaceEffect::SurprisedFace
    );
  } else if (
    strcmp(effect, "pout_face") == 0
  ) {
    faceEffectState.set(
      FaceEffect::PoutFace
    );
  } else if (
    strcmp(effect, "shy_face") == 0
  ) {
    faceEffectState.set(
      FaceEffect::ShyFace
    );
  } else if (
    strcmp(effect, "smug_face") == 0
  ) {
    faceEffectState.set(
      FaceEffect::SmugFace
    );
  } else if (
    strcmp(effect, "confused_face") == 0
  ) {
    faceEffectState.set(
      FaceEffect::ConfusedFace
    );
  } else if (
    strcmp(effect, "laugh_face") == 0
  ) {
    faceEffectState.set(
      FaceEffect::LaughFace
    );
  } else if (
    strcmp(effect, "kiss_face") == 0
  ) {
    faceEffectState.set(
      FaceEffect::KissFace
    );
  } else if (
    strcmp(effect, "nervous_face") == 0
  ) {
    faceEffectState.set(
      FaceEffect::NervousFace
    );
  } else if (
    strcmp(effect, "relieved_face") == 0
  ) {
    faceEffectState.set(
      FaceEffect::RelievedFace
    );
  } else if (
    strcmp(effect, "determined_face") == 0
  ) {
    faceEffectState.set(
      FaceEffect::DeterminedFace
    );
  } else {
    faceEffectState.set(
      FaceEffect::None
    );

    Serial.printf(
      "[FACE] Unknown effect: %s\n",
      effect
    );

    return;
  }

  Serial.printf(
    "[FACE] Effect: %s\n",
    effect
  );
}


void startShakeDizzyEffect() {
  const unsigned long now = millis();

  if (shakeDizzyActive) {
    shakeDizzyUntilMs =
      now + SHAKE_DIZZY_DURATION_MS;

    return;
  }

  shakePreviousFaceEffect =
    faceEffectState.get();

  shakeDizzyActive = true;

  shakeDizzyUntilMs =
    now + SHAKE_DIZZY_DURATION_MS;

  faceEffectState.set(
    FaceEffect::DizzyEyes
  );

  Serial.println(
    "[FACE] Shake dizzy effect started"
  );
}


void updateShakeDizzyEffect() {
  if (!shakeDizzyActive) {
    return;
  }

  const unsigned long now = millis();

  if (
    static_cast<long>(
      now - shakeDizzyUntilMs
    ) < 0
  ) {
    return;
  }

  faceEffectState.set(
    shakePreviousFaceEffect
  );

  shakeDizzyActive = false;
  shakeDizzyUntilMs = 0;

  Serial.println(
    "[FACE] Shake dizzy effect restored"
  );
}


/*
 * ============================================================
 * 屏幕文字
 * ============================================================
 */

void setSpeechTextForDuration(
  const char* text,
  unsigned long durationMs
) {
  if (text == nullptr) {
    return;
  }

  activeSpeechText = text;

  avatar.setSpeechText(
    activeSpeechText.c_str()
  );

  if (activeSpeechText.length() == 0) {
    speechClearScheduled = false;
    speechClearAtMs = 0;

    Serial.println(
      "[DISPLAY] Speech text cleared"
    );

    return;
  }

  durationMs =
    clampDisplayDuration(durationMs);

  speechClearAtMs =
    millis() + durationMs;

  speechClearScheduled = true;

  Serial.printf(
    "[DISPLAY] Text set: \"%s\", %lu ms\n",
    activeSpeechText.c_str(),
    durationMs
  );
}


void updateSpeechText() {
  if (!speechClearScheduled) {
    return;
  }

  const unsigned long now = millis();

  if (
    static_cast<long>(
      now - speechClearAtMs
    ) < 0
  ) {
    return;
  }

  avatar.setSpeechText("");

  activeSpeechText = "";
  speechClearScheduled = false;
  speechClearAtMs = 0;

  Serial.println(
    "[DISPLAY] Speech text auto-cleared"
  );
}


/*
 * ============================================================
 * 舵机动作
 * ============================================================
 */

void cancelGesture() {
  activeGesture = GESTURE_NONE;
  gestureStep = 0;
  nextGestureStepMs = 0;
}


void startGesture(
  GestureKind gesture,
  const char* name
) {
  if (
    gesture == GESTURE_NONE ||
    name == nullptr
  ) {
    return;
  }

  if (activeGesture != GESTURE_NONE) {
    Serial.printf(
      "[GESTURE] Ignored %s: another gesture active\n",
      name
    );

    return;
  }

  activeGesture = gesture;
  gestureStep = 0;
  nextGestureStepMs = 0;

  Serial.printf(
    "[GESTURE] %s started\n",
    name
  );
}


void startNod() {
  startGesture(
    GESTURE_NOD,
    "nod"
  );
}


void startShakeHead() {
  startGesture(
    GESTURE_SHAKE_HEAD,
    "shake_head"
  );
}


void startLookLeft() {
  startGesture(
    GESTURE_LOOK_LEFT,
    "look_left"
  );
}


void startLookRight() {
  startGesture(
    GESTURE_LOOK_RIGHT,
    "look_right"
  );
}


void startTiltUp() {
  startGesture(
    GESTURE_TILT_UP,
    "tilt_up"
  );
}


void updateGesture() {
  if (activeGesture == GESTURE_NONE) {
    return;
  }

  const unsigned long now = millis();

  if (
    nextGestureStepMs != 0 &&
    static_cast<long>(
      now - nextGestureStepMs
    ) < 0
  ) {
    return;
  }

  switch (activeGesture) {
    case GESTURE_NOD:
      switch (gestureStep) {
        case 0:
          M5StackChan.Motion.moveY(
            300,
            500
          );

          nextGestureStepMs =
            now + 350;

          break;

        case 1:
          M5StackChan.Motion.moveY(
            50,
            600
          );

          nextGestureStepMs =
            now + 350;

          break;

        case 2:
          M5StackChan.Motion.moveY(
            300,
            500
          );

          nextGestureStepMs =
            now + 350;

          break;

        case 3:
          M5StackChan.Motion.goHome(
            500
          );

          cancelGesture();
          return;

        default:
          cancelGesture();
          return;
      }

      gestureStep++;
      return;


    case GESTURE_SHAKE_HEAD:
      switch (gestureStep) {
        case 0:
          M5StackChan.Motion.moveX(
            -300,
            500
          );

          nextGestureStepMs =
            now + 550;

          break;

        case 1:
          M5StackChan.Motion.moveX(
            300,
            500
          );

          nextGestureStepMs =
            now + 550;

          break;

        case 2:
          M5StackChan.Motion.moveX(
            -300,
            500
          );

          nextGestureStepMs =
            now + 550;

          break;

        case 3:
          M5StackChan.Motion.goHome(
            600
          );

          cancelGesture();
          return;

        default:
          cancelGesture();
          return;
      }

      gestureStep++;
      return;


    case GESTURE_LOOK_LEFT:
      if (gestureStep == 0) {
        M5StackChan.Motion.moveX(
          -300,
          600
        );

        nextGestureStepMs =
          now + 900;

        gestureStep++;
      } else {
        M5StackChan.Motion.goHome(
          600
        );

        cancelGesture();
      }

      return;


    case GESTURE_LOOK_RIGHT:
      if (gestureStep == 0) {
        M5StackChan.Motion.moveX(
          300,
          600
        );

        nextGestureStepMs =
          now + 900;

        gestureStep++;
      } else {
        M5StackChan.Motion.goHome(
          600
        );

        cancelGesture();
      }

      return;


    case GESTURE_TILT_UP:
      if (gestureStep == 0) {
        M5StackChan.Motion.moveY(
          420,
          600
        );

        nextGestureStepMs =
          now + 900;

        gestureStep++;
      } else {
        M5StackChan.Motion.goHome(
          600
        );

        cancelGesture();
      }

      return;


    case GESTURE_NONE:
    default:
      cancelGesture();
      return;
  }
}


/*
 * ============================================================
 * 事件发送
 * ============================================================
 */

void sendTouchTapEvent(
  int32_t x,
  int32_t y
) {
  if (!webSocket.isConnected()) {
    Serial.println(
      "[TOUCH] WebSocket disconnected"
    );

    return;
  }

  StaticJsonDocument<192> doc;

  doc["type"] = "robot_event";
  doc["event"] = "touch_tap";
  doc["x"] = x;
  doc["y"] = y;
  doc["at_ms"] = millis();

  String output;
  serializeJson(doc, output);

  webSocket.sendTXT(output);

  Serial.printf(
    "[TOUCH] touch_tap sent: x=%ld, y=%ld\n",
    static_cast<long>(x),
    static_cast<long>(y)
  );
}


void sendHeadTouchEvent() {
  if (!webSocket.isConnected()) {
    Serial.println(
      "[HEAD] WebSocket disconnected"
    );

    return;
  }

  StaticJsonDocument<128> doc;

  doc["type"] = "robot_event";
  doc["event"] = "head_touch";
  doc["at_ms"] = millis();

  String output;
  serializeJson(doc, output);

  webSocket.sendTXT(output);

  Serial.println(
    "[HEAD] head_touch sent"
  );
}


void sendShakeEvent() {
  if (!webSocket.isConnected()) {
    Serial.println(
      "[IMU] WebSocket disconnected"
    );

    return;
  }

  StaticJsonDocument<128> doc;

  doc["type"] = "robot_event";
  doc["event"] = "shake";
  doc["at_ms"] = millis();

  String output;
  serializeJson(doc, output);

  webSocket.sendTXT(output);

  Serial.println(
    "[IMU] shake sent"
  );
}


/*
 * ============================================================
 * 触摸、头顶、IMU
 * ============================================================
 */

void updateTouchInput() {
  auto touch = M5.Touch.getDetail();

  if (!touch.wasPressed()) {
    return;
  }

  const unsigned long now = millis();

  if (
    lastTouchEventMs != 0 &&
    !elapsedMs(
      now,
      lastTouchEventMs,
      TOUCH_EVENT_DEBOUNCE_MS
    )
  ) {
    Serial.println(
      "[TOUCH] Ignored by debounce"
    );

    return;
  }

  lastTouchEventMs = now;

  Serial.printf(
    "[TOUCH] Pressed: x=%ld, y=%ld\n",
    static_cast<long>(touch.x),
    static_cast<long>(touch.y)
  );

  sendTouchTapEvent(
    touch.x,
    touch.y
  );
}


void updateHeadTouchInput() {
  const auto& intensities =
    M5StackChan.TouchSensor.getIntensities();

  const bool isTouched =
    intensities[0] >= HEAD_TOUCH_THRESHOLD ||
    intensities[1] >= HEAD_TOUCH_THRESHOLD ||
    intensities[2] >= HEAD_TOUCH_THRESHOLD;

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
    !elapsedMs(
      now,
      lastHeadTouchEventMs,
      HEAD_TOUCH_DEBOUNCE_MS
    )
  ) {
    Serial.println(
      "[HEAD] Ignored by debounce"
    );

    return;
  }

  lastHeadTouchEventMs = now;

  Serial.printf(
    "[HEAD] Touch: front=%u, middle=%u, back=%u\n",
    static_cast<unsigned>(intensities[0]),
    static_cast<unsigned>(intensities[1]),
    static_cast<unsigned>(intensities[2])
  );

  startHeadTouchLightEffect();
  sendHeadTouchEvent();
}


void updateShakeDetection() {
  const unsigned long now = millis();

  if (
    lastImuReadMs != 0 &&
    !elapsedMs(
      now,
      lastImuReadMs,
      IMU_READ_INTERVAL_MS
    )
  ) {
    return;
  }

  lastImuReadMs = now;

  float ax = 0.0f;
  float ay = 0.0f;
  float az = 0.0f;

  if (!M5.Imu.getAccel(&ax, &ay, &az)) {
    if (imuReady) {
      Serial.println(
        "[IMU] Failed to read acceleration"
      );
    }

    imuReady = false;
    shakeCandidateCount = 0;

    return;
  }

  if (!imuReady) {
    imuReady = true;

    lastAccelX = ax;
    lastAccelY = ay;
    lastAccelZ = az;

    Serial.println(
      "[IMU] Acceleration reading started"
    );

    return;
  }

  const float dx =
    ax - lastAccelX;

  const float dy =
    ay - lastAccelY;

  const float dz =
    az - lastAccelZ;

  const float deltaMagnitude =
    sqrtf(
      dx * dx +
      dy * dy +
      dz * dz
    );

  lastAccelX = ax;
  lastAccelY = ay;
  lastAccelZ = az;

  if (
    deltaMagnitude >=
    SHAKE_DELTA_THRESHOLD
  ) {
    if (
      shakeCandidateCount <
      SHAKE_REQUIRED_SAMPLES
    ) {
      shakeCandidateCount++;
    }
  } else {
    shakeCandidateCount = 0;
  }

  if (
    shakeCandidateCount <
    SHAKE_REQUIRED_SAMPLES
  ) {
    return;
  }

  shakeCandidateCount = 0;

  if (
    lastShakeEventMs != 0 &&
    !elapsedMs(
      now,
      lastShakeEventMs,
      SHAKE_DEBOUNCE_MS
    )
  ) {
    return;
  }

  lastShakeEventMs = now;

  Serial.printf(
    "[IMU] Shake detected, delta=%.3f, "
    "ax=%.3f, ay=%.3f, az=%.3f\n",
    deltaMagnitude,
    ax,
    ay,
    az
  );

  startShakeDizzyEffect();
  sendShakeEvent();
}


/*
 * ============================================================
 * RGB 灯光
 * ============================================================
 */

LedTheme getLedTheme() {
  const FaceEffect effect =
    faceEffectState.get();

  switch (effect) {
    case FaceEffect::HeartEyes:
    case FaceEffect::KissFace:
      return {
        255,
        35,
        130,
        180,
        2400
      };

    case FaceEffect::SparkleEyes:
    case FaceEffect::LaughFace:
      return {
        255,
        180,
        20,
        210,
        1600
      };

    case FaceEffect::DizzyEyes:
    case FaceEffect::ConfusedFace:
      return {
        100,
        30,
        255,
        150,
        2200
      };

    case FaceEffect::TearEyes:
      return {
        20,
        100,
        255,
        130,
        3500
      };

    case FaceEffect::SurprisedFace:
      return {
        255,
        255,
        255,
        230,
        1000
      };

    case FaceEffect::PoutFace:
      return {
        255,
        80,
        30,
        150,
        2600
      };

    case FaceEffect::ShyFace:
      return {
        255,
        70,
        140,
        120,
        3200
      };

    case FaceEffect::SmugFace:
      return {
        150,
        40,
        220,
        160,
        2200
      };

    case FaceEffect::NervousFace:
      return {
        255,
        100,
        20,
        120,
        1100
      };

    case FaceEffect::RelievedFace:
      return {
        30,
        220,
        100,
        140,
        3600
      };

    case FaceEffect::DeterminedFace:
      return {
        20,
        210,
        180,
        180,
        1800
      };

    case FaceEffect::None:
    default:
      break;
  }

  switch (avatar.getExpression()) {
    case Expression::Happy:
      return {
        255,
        180,
        0,
        170,
        2200
      };

    case Expression::Sad:
      return {
        0,
        80,
        255,
        120,
        3800
      };

    case Expression::Angry:
      return {
        255,
        0,
        0,
        190,
        900
      };

    case Expression::Doubt:
      return {
        150,
        40,
        220,
        130,
        2400
      };

    case Expression::Sleepy:
      return {
        30,
        20,
        120,
        80,
        5000
      };

    case Expression::Neutral:
    default:
      return {
        20,
        30,
        50,
        90,
        3200
      };
  }
}


void updateLedBreath() {
  const unsigned long now = millis();

  if (
    lastLedUpdateMs != 0 &&
    !elapsedMs(
      now,
      lastLedUpdateMs,
      LED_UPDATE_INTERVAL_MS
    )
  ) {
    return;
  }

  lastLedUpdateMs = now;

  const LedTheme theme =
    getLedTheme();

  const uint16_t periodMs =
    theme.periodMs == 0
      ? 1
      : theme.periodMs;

  const float phase =
    static_cast<float>(
      now % periodMs
    ) /
    static_cast<float>(periodMs);

  const float wave =
    (
      sinf(
        phase * 6.2831853f
      ) + 1.0f
    ) * 0.5f;

  const float maxBrightness =
    static_cast<float>(
      theme.maxBrightness
    ) / 255.0f;

  const float minBrightness =
    maxBrightness * 0.15f;

  float brightness =
    minBrightness +
    (
      maxBrightness -
      minBrightness
    ) * wave;

  if (activeSpeechText.length() > 0) {
    brightness *= SPEAKING_LIGHT_BOOST;
  }

  bool useHeadTouchPink = false;

  if (headTouchLightActive) {
    const unsigned long totalDuration =
      HEAD_TOUCH_LIGHT_CYCLE_MS *
      HEAD_TOUCH_LIGHT_FLASH_COUNT;

    const unsigned long elapsed =
      now - headTouchLightStartedMs;

    if (elapsed >= totalDuration) {
      headTouchLightActive = false;
    } else {
      const unsigned long cycleElapsed =
        elapsed % HEAD_TOUCH_LIGHT_CYCLE_MS;

      const float cyclePhase =
        static_cast<float>(cycleElapsed) /
        static_cast<float>(HEAD_TOUCH_LIGHT_CYCLE_MS);

      const float flashWave =
        sinf(
          cyclePhase * 3.14159265f
        );

      useHeadTouchPink = true;

      brightness =
        (220.0f / 255.0f) *
        (0.35f + flashWave * 0.65f);
    }
  }

  if (brightness < 0.0f) {
    brightness = 0.0f;
  }

  if (brightness > 1.0f) {
    brightness = 1.0f;
  }

  uint8_t r = 0;
  uint8_t g = 0;
  uint8_t b = 0;

  if (useHeadTouchPink) {
    r = static_cast<uint8_t>(
      255.0f * brightness
    );

    g = static_cast<uint8_t>(
      45.0f * brightness
    );

    b = static_cast<uint8_t>(
      150.0f * brightness
    );
  } else {
    r = static_cast<uint8_t>(
      static_cast<float>(theme.r) *
      brightness
    );

    g = static_cast<uint8_t>(
      static_cast<float>(theme.g) *
      brightness
    );

    b = static_cast<uint8_t>(
      static_cast<float>(theme.b) *
      brightness
    );
  }

  for (
    uint8_t i = 0;
    i < LED_COUNT;
    i++
  ) {
    M5StackChan.setRgbColor(
      i,
      r,
      g,
      b
    );
  }

  M5StackChan.refreshRgb();
}


void startHeadTouchLightEffect() {
  headTouchLightActive = true;
  headTouchLightStartedMs = millis();

  Serial.println(
    "[LED] Head-touch flash started"
  );
}


/*
 * ============================================================
 * WebSocket 文本消息
 * ============================================================
 */

void handleWebSocketText(
  uint8_t* payload,
  size_t length
) {
  if (
    payload == nullptr ||
    length == 0 ||
    length > 4096
  ) {
    return;
  }

  Serial.printf(
    "[WS] Received text message, %u bytes\n",
    static_cast<unsigned>(length)
  );

  DynamicJsonDocument doc(4096);

  DeserializationError error =
    deserializeJson(
      doc,
      payload,
      length
    );

  if (error) {
    Serial.printf(
      "[WS] JSON parse failed: %s\n",
      error.c_str()
    );

    return;
  }

  const char* messageType =
    doc["type"] | "";

  /*
   * 音频控制消息。
   */

  if (
    strcmp(messageType, "audio_start") == 0
  ) {
    handleAudioStart(doc);
    return;
  }

  if (
    strcmp(messageType, "audio_end") == 0
  ) {
    handleAudioEnd(doc);
    return;
  }

  if (
    strcmp(messageType, "audio_stop") == 0
  ) {
    handleAudioStop(doc);
    return;
  }

  if (
    strcmp(messageType, "audio_abort") == 0
  ) {
    handleAudioAbort(doc);
    return;
  }

  /*
   * 普通控制消息。
   */

  const char* expression =
    doc["expression"];

  const char* faceEffect =
    doc["face_effect"];

  const char* motion =
    doc["motion"];

  const char* sound =
    doc["sound"];

  const char* action =
    doc["action"];

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

    setSpeechTextForDuration(
      text,
      durationMs
    );
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
    } else if (
      strcmp(motion, "shake_head") == 0
    ) {
      startShakeHead();
    } else if (
      strcmp(motion, "look_left") == 0
    ) {
      startLookLeft();
    } else if (
      strcmp(motion, "look_right") == 0
    ) {
      startLookRight();
    } else if (
      strcmp(motion, "tilt_up") == 0
    ) {
      startTiltUp();
    } else if (
      strcmp(motion, "home") == 0
    ) {
      cancelGesture();

      M5StackChan.Motion.goHome(
        500
      );
    } else if (
      strcmp(motion, "none") == 0
    ) {
      /*
       * 不执行动作。
       */
    } else if (
      strcmp(motion, "shake") == 0
    ) {
      /*
       * shake 是用户事件，
       * 不是机器人主动动作。
       */
      Serial.println(
        "[GESTURE] shake is an event"
      );
    } else {
      Serial.printf(
        "[GESTURE] Unknown motion: %s\n",
        motion
      );
    }
  } else if (
    action != nullptr &&
    strcmp(action, "home") == 0
  ) {
    cancelGesture();

    M5StackChan.Motion.goHome(
      500
    );
  }

  if (sound != nullptr) {
    Serial.printf(
      "[ACTION] Sound: %s\n",
      sound
    );

    playSoundByName(sound);
  }
}


/*
 * ============================================================
 * WebSocket 事件回调
 * ============================================================
 */

void webSocketEvent(
  WStype_t type,
  uint8_t* payload,
  size_t length
) {
  switch (type) {
    case WStype_CONNECTED:
      Serial.println(
        "[WS] Connected to Render"
      );

      avatar.setExpression(
        Expression::Happy
      );

      cancelGesture();

      M5StackChan.Motion.goHome(
        500
      );

      break;


    case WStype_DISCONNECTED:
      Serial.println(
        "[WS] Disconnected from Render"
      );

      M5.Speaker.stop();
      resetNetworkAudioState(true);

      break;


    case WStype_ERROR:
      Serial.println(
        "[WS] WebSocket error"
      );

      M5.Speaker.stop();
      resetNetworkAudioState(true);

      break;


    case WStype_TEXT:
      handleWebSocketText(
        payload,
        length
      );

      break;


    case WStype_BIN:
      handleWebSocketBinary(
        payload,
        length
      );

      break;


    default:
      break;
  }
}


/*
 * ============================================================
 * 启动界面
 * ============================================================
 */

void showBootMessage(
  const char* line1,
  const char* line2 = nullptr
) {
  M5.Display.clear(TFT_BLACK);

  M5.Display.setTextColor(
    TFT_YELLOW,
    TFT_BLACK
  );

  M5.Display.setTextSize(2);
  M5.Display.setCursor(8, 35);
  M5.Display.println(line1);

  if (line2 != nullptr) {
    M5.Display.setTextColor(
      TFT_WHITE,
      TFT_BLACK
    );

    M5.Display.setTextSize(1);
    M5.Display.setCursor(8, 80);
    M5.Display.println(line2);
  }
}


/*
 * ============================================================
 * 读取配置
 * ============================================================
 */

void loadSavedConfiguration() {
  prefs.begin(
    "stackchan",
    false
  );

  String savedHost =
    prefs.getString(
      "server_host",
      ""
    );

  String savedToken =
    prefs.getString(
      "robot_token",
      ""
    );

  savedHost.toCharArray(
    wsHost,
    sizeof(wsHost)
  );

  savedToken.toCharArray(
    wsToken,
    sizeof(wsToken)
  );

  Serial.printf(
    "[CONFIG] Saved host: '%s'\n",
    wsHost
  );

  Serial.printf(
    "[CONFIG] Robot token: %s\n",
    strlen(wsToken) > 0
      ? "configured"
      : "missing"
  );
}


/*
 * ============================================================
 * 初始化
 * ============================================================
 */

void setup() {
  Serial.begin(115200);
  delay(1000);

  Serial.println();
  Serial.println(
    "=== StackChan secure robot firmware ==="
  );

  /*
   * 初始化 StackChan。
   */
  M5StackChan.begin();

  /*
   * 初始化 RGB。
   */
  lastLedUpdateMs =
    millis() - LED_UPDATE_INTERVAL_MS;

  updateLedBreath();

  /*
   * 初始化 SD。
   */
  SPI.begin(
    SD_SPI_SCK_PIN,
    SD_SPI_MISO_PIN,
    SD_SPI_MOSI_PIN,
    SD_SPI_CS_PIN
  );

  sdCardReady =
    SD.begin(
      SD_SPI_CS_PIN,
      SPI,
      25000000
    );

  if (sdCardReady) {
    Serial.println(
      "[SD] Card mounted"
    );

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
    Serial.println(
      "[SD] Card mount failed"
    );
  }

  /*
   * 音量。
   */
  M5.Speaker.setVolume(80);

  /*
   * 舵机。
   */
  M5StackChan.Motion.setAutoAngleSyncEnabled(
    false
  );

  M5StackChan.Motion.setAutoTorqueReleaseEnabled(
    true
  );

  M5StackChan.Motion.goHome(
    500
  );

  showBootMessage(
    "WiFi starting...",
    "Please wait"
  );

  /*
   * 读取已有配置。
   */
  loadSavedConfiguration();

  WiFi.mode(WIFI_STA);

  WiFiManager wm;

  WiFiManagerParameter serverParameter(
    "server",
    "Render Server Host",
    wsHost,
    sizeof(wsHost)
  );

  WiFiManagerParameter tokenParameter(
    "token",
    "Robot WebSocket Token",
    wsToken,
    sizeof(wsToken)
  );

  wm.addParameter(
    &serverParameter
  );

  wm.addParameter(
    &tokenParameter
  );

  wm.setConnectTimeout(15);

  Serial.println(
    "[WIFI] Trying saved WiFi credentials..."
  );

  if (!wm.autoConnect("StackChan-Setup")) {
    Serial.println(
      "[WIFI] Could not join saved WiFi"
    );

    showBootMessage(
      "WiFi setup",
      "AP: StackChan-Setup"
    );

    /*
     * 等待用户完成配网。
     */
    wm.startConfigPortal(
      "StackChan-Setup"
    );
  }

  Serial.printf(
    "[WIFI] Status: %d\n",
    WiFi.status()
  );

  Serial.printf(
    "[WIFI] IP: %s\n",
    WiFi.localIP().toString().c_str()
  );

  /*
   * 保存当前输入的配置。
   */
  const char* enteredHost =
    serverParameter.getValue();

  const char* enteredToken =
    tokenParameter.getValue();

  if (
    enteredHost != nullptr &&
    strlen(enteredHost) > 0
  ) {
    strncpy(
      wsHost,
      enteredHost,
      sizeof(wsHost) - 1
    );

    wsHost[
      sizeof(wsHost) - 1
    ] = '\0';

    prefs.putString(
      "server_host",
      wsHost
    );
  }

  if (
    enteredToken != nullptr &&
    strlen(enteredToken) > 0
  ) {
    strncpy(
      wsToken,
      enteredToken,
      sizeof(wsToken) - 1
    );

    wsToken[
      sizeof(wsToken) - 1
    ] = '\0';

    prefs.putString(
      "robot_token",
      wsToken
    );
  }

  Serial.printf(
    "[CONFIG] Active host: '%s'\n",
    wsHost
  );

  Serial.printf(
    "[CONFIG] Token: %s\n",
    strlen(wsToken) > 0
      ? "configured"
      : "missing"
  );

  /*
   * Avatar。
   */
  avatar.init();

  avatar.setSpeechFont(
    &fonts::efontCN_10
  );

  faceEffectState.set(
    FaceEffect::None
  );

  avatar.setExpression(
    Expression::Neutral
  );

  avatar.setSpeechText("");

  /*
   * 没有网络时不启动 WebSocket。
   */
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println(
      "[WIFI] Not connected"
    );

    return;
  }

  if (strlen(wsHost) == 0) {
    Serial.println(
      "[WS] Render host is empty"
    );

    return;
  }

  if (strlen(wsToken) == 0) {
    Serial.println(
      "[WS] Robot token is empty"
    );

    showBootMessage(
      "Missing token",
      "Set ROBOT_WS_TOKEN"
    );

    return;
  }

  /*
   * 构造带鉴权参数的路径。
   *
   * Token 使用 openssl rand -hex 32 生成时，
   * 只包含 URL 安全字符，不需要额外编码。
   */
  authenticatedWsPath =
    String(WS_PATH) +
    "?token=" +
    String(wsToken);

  Serial.printf(
    "[WS] Connecting to wss://%s:%u%s\n",
    wsHost,
    WS_PORT,
    authenticatedWsPath.c_str()
  );

  webSocket.onEvent(
    webSocketEvent
  );

  webSocket.setReconnectInterval(
    5000
  );

  webSocket.beginSSL(
    wsHost,
    WS_PORT,
    authenticatedWsPath.c_str()
  );
}


/*
 * ============================================================
 * 主循环
 * ============================================================
 */

void loop() {
  M5StackChan.update();

  webSocket.loop();

  updateNetworkAudioTimeout();

  updateGesture();
  updateSpeechText();

  updateTouchInput();
  updateHeadTouchInput();

  updateShakeDetection();
  updateShakeDizzyEffect();

  updateLedBreath();

  delay(10);
}
