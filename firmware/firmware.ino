#include <Arduino.h>
#include <M5Unified.h>
#include <M5StackChan.h>
#include <math.h>
#include <stddef.h>
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
 * 自定义脸部效果状态。
 */
FaceEffectState faceEffectState;


/*
 * RGB 灯光主题。
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
  20,
  20,
  30,
  80,
  3000
};

unsigned long lastLedUpdateMs = 0;

static constexpr unsigned long LED_UPDATE_INTERVAL_MS = 40;
static constexpr uint8_t LED_COUNT = 12;


/*
 * 头顶触摸灯效。
 */
bool headTouchLightActive = false;
unsigned long headTouchLightStartedMs = 0;

static constexpr unsigned long HEAD_TOUCH_LIGHT_CYCLE_MS = 600;
static constexpr uint8_t HEAD_TOUCH_LIGHT_FLASH_COUNT = 2;


/*
 * 屏幕存在文字时，视为正在说话。
 */
static constexpr float SPEAKING_LIGHT_BOOST = 1.22f;


/*
 * Avatar。
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
 * WebSocket 与配置。
 */
WebSocketsClient webSocket;
Preferences prefs;

char ws_host[128] = "";

const uint16_t ws_port = 443;
const char* ws_path = "/";


/*
 * CoreS3 microSD SPI 引脚。
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


/*
 * 舵机动作类型。
 */
enum GestureKind {
  GESTURE_NONE = 0,
  GESTURE_NOD,
  GESTURE_SHAKE_HEAD,
  GESTURE_LOOK_LEFT,
  GESTURE_LOOK_RIGHT,
  GESTURE_TILT_UP,
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


/*
 * IMU 用户摇晃检测。
 *
 * 第一版只检测加速度变化，
 * 不保存、不上传原始传感器数据。
 */
bool imuReady = false;

float lastAccelX = 0.0f;
float lastAccelY = 0.0f;
float lastAccelZ = 0.0f;

unsigned long lastImuReadMs = 0;
unsigned long lastShakeEventMs = 0;

static constexpr unsigned long IMU_READ_INTERVAL_MS = 50;
static constexpr unsigned long SHAKE_DEBOUNCE_MS = 1800;

/*
 * M5Unified 加速度通常以 G 为单位。
 * 该阈值用于第一版测试。
 */
static constexpr float SHAKE_DELTA_THRESHOLD = 1.20f;

/*
 * 用户摇晃时的临时晕眩表情。
 *
 * 只覆盖 face_effect，不改变基础 expression。
 * 到时间后恢复摇晃前的 face_effect。
 */
bool shakeDizzyActive = false;

FaceEffect shakePreviousFaceEffect =
  FaceEffect::None;

unsigned long shakeDizzyUntilMs = 0;

static constexpr unsigned long
  SHAKE_DIZZY_DURATION_MS = 2200;



/*
 * WAV 文件头。
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
 * 流式音频接收（Render 端 MP3 -> PCM 管道转码后写入 SD 卡）。
 *
 * 协议：
 * - audio_start（JSON 文本帧）：开始一路新的 PCM 流；
 * - 二进制帧：裸 PCM s16le 数据，原样追加写入；
 * - audio_end（JSON 文本帧）：流结束，回写 WAV 头并播放；
 * - audio_stop（JSON 文本帧）：中止当前流，清理临时文件。
 *
 * 落盘策略（节省 SD 空间、RAM 恒定）：
 * - 开始时创建 /audio.wav.part，先写入 44 字节占位 WAV 头；
 * - 后续二进制帧的 PCM 数据直接追加写入同一文件；
 * - audio_end 时以 "r+" 模式重新打开该文件（不截断），
 *   回写正确的 WAV 头，再 rename 为 /audio.wav 并复用 playSdWav()。
 *
 * 重要细节：ESP32 SD 库的 FILE_WRITE 宏通常对应
 * fopen(path, "w+")，会截断已有内容。因此回写头部时
 * 绝不能再次用 FILE_WRITE 打开该文件，必须使用 "r+"，
 * 否则已经写入的 PCM 数据会被清空。
 */
enum AudioStreamState {
  AUDIO_STREAM_IDLE = 0,
  AUDIO_STREAM_RECEIVING,
};

AudioStreamState audioStreamState = AUDIO_STREAM_IDLE;

String audioStreamId = "";

uint32_t audioStreamSampleRate = 16000;
uint16_t audioStreamChannels = 1;
uint16_t audioStreamBitsPerSample = 16;

File audioStreamFile;
uint32_t audioStreamBytesWritten = 0;

static const char* AUDIO_STREAM_PART_FILENAME = "/audio.wav.part";
static const char* AUDIO_STREAM_FINAL_FILENAME = "/audio.wav";

static constexpr size_t AUDIO_STREAM_WAV_HEADER_SIZE = 44;


/*
 * 函数声明。
 */
void updateGesture();
void cancelGesture();

void startNod();
void startGesture(GestureKind gesture, const char* name);
void startShakeHead();
void startLookLeft();
void startLookRight();
void startTiltUp();
void startTiltDown();

void sendTouchTapEvent(int32_t x, int32_t y);
void sendHeadTouchEvent();
void sendShakeEvent();

void updateTouchInput();
void updateHeadTouchInput();
void updateShakeDetection();

void updateSpeechText();
void updateLedBreath();
void startHeadTouchLightEffect();

void buildAudioStreamWavHeader(
  uint8_t* buffer,
  uint32_t sampleRate,
  uint16_t channels,
  uint16_t bitsPerSample,
  uint32_t dataSize
);

bool beginAudioStream(
  const String& streamId,
  uint32_t sampleRate,
  uint16_t channels,
  uint16_t bitsPerSample
);

void writeAudioStreamChunk(const uint8_t* data, size_t length);
void finishAudioStream(const String& streamId);
void abortAudioStream();

void sendAudioReadyEvent(const String& streamId);
void sendAudioChunkAckEvent(const String& streamId, uint32_t bytesWritten);
void sendAudioAbortEvent(const String& streamId, const char* reason);


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
    header.sampleRate == 0 ||
    header.fmtChunkSize < 16
  ) {
    Serial.println(
      "[AUDIO] Unsupported WAV: require PCM, 8/16-bit, mono/stereo"
    );
    file.close();
    return false;
  }

  /*
   * 跳过 fmt chunk 的剩余内容，并查找 data chunk。
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

    uint32_t nextChunkOffset =
      static_cast<uint32_t>(file.position()) + subChunk.chunkSize;

    /*
     * WAV chunk 按偶数字节对齐。
     */
    if (subChunk.chunkSize & 1U) {
      nextChunkOffset++;
    }

    if (!file.seek(nextChunkOffset)) {
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


/*
 * 构造一个标准 44 字节 canonical WAV 头，写入 buffer。
 *
 * buffer 必须至少有 AUDIO_STREAM_WAV_HEADER_SIZE (44) 字节。
 * 该布局与 playSdWav() 里 WavHeader + "data" WavSubChunk
 * 期望读取到的字节完全一致（小端序，ESP32 原生字节序）。
 */
void buildAudioStreamWavHeader(
  uint8_t* buffer,
  uint32_t sampleRate,
  uint16_t channels,
  uint16_t bitsPerSample,
  uint32_t dataSize
) {
  const uint16_t bytesPerSample =
    static_cast<uint16_t>(bitsPerSample / 8);

  const uint32_t byteRate =
    sampleRate * channels * bytesPerSample;

  const uint16_t blockAlign =
    static_cast<uint16_t>(channels * bytesPerSample);

  const uint32_t riffChunkSize = 36 + dataSize;
  const uint32_t fmtChunkSize = 16;
  const uint16_t audioFormat = 1; // PCM

  size_t offset = 0;

  memcpy(buffer + offset, "RIFF", 4);
  offset += 4;

  memcpy(buffer + offset, &riffChunkSize, sizeof(riffChunkSize));
  offset += sizeof(riffChunkSize);

  memcpy(buffer + offset, "WAVE", 4);
  offset += 4;

  memcpy(buffer + offset, "fmt ", 4);
  offset += 4;

  memcpy(buffer + offset, &fmtChunkSize, sizeof(fmtChunkSize));
  offset += sizeof(fmtChunkSize);

  memcpy(buffer + offset, &audioFormat, sizeof(audioFormat));
  offset += sizeof(audioFormat);

  memcpy(buffer + offset, &channels, sizeof(channels));
  offset += sizeof(channels);

  memcpy(buffer + offset, &sampleRate, sizeof(sampleRate));
  offset += sizeof(sampleRate);

  memcpy(buffer + offset, &byteRate, sizeof(byteRate));
  offset += sizeof(byteRate);

  memcpy(buffer + offset, &blockAlign, sizeof(blockAlign));
  offset += sizeof(blockAlign);

  memcpy(buffer + offset, &bitsPerSample, sizeof(bitsPerSample));
  offset += sizeof(bitsPerSample);

  memcpy(buffer + offset, "data", 4);
  offset += 4;

  memcpy(buffer + offset, &dataSize, sizeof(dataSize));
  offset += sizeof(dataSize);

  // offset 此时应等于 AUDIO_STREAM_WAV_HEADER_SIZE (44)。
}


/*
 * 开始一路新的流式 PCM 接收。
 *
 * 如果当前已有正在接收的流，视为被新流打断：
 * 先中止旧流并清理其临时文件，再开始新流。
 */
bool beginAudioStream(
  const String& streamId,
  uint32_t sampleRate,
  uint16_t channels,
  uint16_t bitsPerSample
) {
  if (!sdCardReady) {
    Serial.println("[AUDIO-STREAM] SD card not ready, cannot start stream");
    return false;
  }

  if (sampleRate == 0 || channels == 0 || channels > 2 ||
      (bitsPerSample != 8 && bitsPerSample != 16)) {
    Serial.println("[AUDIO-STREAM] Invalid stream parameters");
    return false;
  }

  if (audioStreamState == AUDIO_STREAM_RECEIVING) {
    Serial.println(
      "[AUDIO-STREAM] New audio_start interrupts the previous stream"
    );
    abortAudioStream();
  }

  if (SD.exists(AUDIO_STREAM_PART_FILENAME)) {
    SD.remove(AUDIO_STREAM_PART_FILENAME);
  }

  /*
   * FILE_WRITE 在 ESP32 SD 库上等价于以截断方式打开，
   * 这里正是我们想要的：确保从空文件开始。
   */
  audioStreamFile = SD.open(AUDIO_STREAM_PART_FILENAME, FILE_WRITE);

  if (!audioStreamFile) {
    Serial.println("[AUDIO-STREAM] Failed to create part file");
    return false;
  }

  uint8_t placeholder[AUDIO_STREAM_WAV_HEADER_SIZE] = {0};

  if (
    audioStreamFile.write(placeholder, AUDIO_STREAM_WAV_HEADER_SIZE) !=
    AUDIO_STREAM_WAV_HEADER_SIZE
  ) {
    Serial.println("[AUDIO-STREAM] Failed to write placeholder header");
    audioStreamFile.close();
    SD.remove(AUDIO_STREAM_PART_FILENAME);
    return false;
  }

  audioStreamId = streamId;
  audioStreamSampleRate = sampleRate;
  audioStreamChannels = channels;
  audioStreamBitsPerSample = bitsPerSample;
  audioStreamBytesWritten = 0;
  audioStreamState = AUDIO_STREAM_RECEIVING;

  Serial.printf(
    "[AUDIO-STREAM] Started stream_id=%s rate=%lu channels=%u bits=%u\n",
    streamId.c_str(),
    static_cast<unsigned long>(sampleRate),
    static_cast<unsigned>(channels),
    static_cast<unsigned>(bitsPerSample)
  );

  return true;
}


/*
 * 接收一帧二进制 PCM 数据并追加写入临时文件。
 */
/*
 * 接收一帧二进制 PCM 数据并追加写入临时文件。
 */
void writeAudioStreamChunk(
  const uint8_t* data,
  size_t length
) {
  Serial.printf(
    "[AUDIO-STREAM] Binary PCM received: %u bytes\n",
    static_cast<unsigned>(length)
  );

  /*
   * WStype_FRAGMENT_FIN 有时会传入 length=0。
   * 它只是分片结束通知，不是音频数据。
   */
  if (
    data == nullptr ||
    length == 0
  ) {
    Serial.println(
      "[AUDIO-STREAM] Empty binary frame ignored"
    );
    return;
  }

  if (
    audioStreamState != AUDIO_STREAM_RECEIVING
  ) {
    Serial.println(
      "[AUDIO-STREAM] Binary frame ignored: "
      "no active stream"
    );
    return;
  }

  if (!audioStreamFile) {
    Serial.println(
      "[AUDIO-STREAM] Audio stream file is not open"
    );

    sendAudioAbortEvent(
      audioStreamId,
      "audio_file_not_open"
    );

    abortAudioStream();
    return;
  }

  const size_t written =
    audioStreamFile.write(data, length);

  Serial.printf(
    "[AUDIO-STREAM] SD write result: %u/%u bytes\n",
    static_cast<unsigned>(written),
    static_cast<unsigned>(length)
  );

  if (written != length) {
    Serial.println(
      "[AUDIO-STREAM] SD write failed"
    );

    sendAudioAbortEvent(
      audioStreamId,
      "sd_write_failed"
    );

    abortAudioStream();
    return;
  }

  audioStreamBytesWritten +=
    static_cast<uint32_t>(written);

  /*
   * bytes_written 返回本次数据块写入的字节数。
   * Render 当前只使用 ACK 作为流控信号。
   */
  sendAudioChunkAckEvent(
    audioStreamId,
    static_cast<uint32_t>(written)
  );

  Serial.printf(
    "[AUDIO-STREAM] Chunk ACK sent: "
    "stream_id=%s, chunk=%u, total=%lu\n",
    audioStreamId.c_str(),
    static_cast<unsigned>(written),
    static_cast<unsigned long>(
      audioStreamBytesWritten
    )
  );
}


/*
 * 结束当前流：回写正确的 WAV 头，
 * 重命名为最终文件，并复用 playSdWav() 播放。
 */
void finishAudioStream(
  const String& streamId
) {
  if (
    audioStreamState != AUDIO_STREAM_RECEIVING
  ) {
    Serial.println(
      "[AUDIO-STREAM] audio_end ignored: "
      "no active stream"
    );
    return;
  }

  if (
    streamId.length() > 0 &&
    streamId != audioStreamId
  ) {
    Serial.printf(
      "[AUDIO-STREAM] audio_end stream_id "
      "mismatch: expected=%s got=%s\n",
      audioStreamId.c_str(),
      streamId.c_str()
    );
    return;
  }

  audioStreamFile.flush();
  audioStreamFile.close();

  const uint32_t dataSize =
    audioStreamBytesWritten;

  File finalizeFile =
    SD.open(
      AUDIO_STREAM_PART_FILENAME,
      "r+"
    );

  if (!finalizeFile) {
    Serial.println(
      "[AUDIO-STREAM] Failed to reopen part "
      "file for header finalize"
    );

    audioStreamState =
      AUDIO_STREAM_IDLE;

    audioStreamId = "";

    return;
  }

  uint8_t headerBuffer[
    AUDIO_STREAM_WAV_HEADER_SIZE
  ];

  buildAudioStreamWavHeader(
    headerBuffer,
    audioStreamSampleRate,
    audioStreamChannels,
    audioStreamBitsPerSample,
    dataSize
  );

  finalizeFile.seek(0);

  const size_t headerWritten =
    finalizeFile.write(
      headerBuffer,
      AUDIO_STREAM_WAV_HEADER_SIZE
    );

  finalizeFile.flush();
  finalizeFile.close();

  audioStreamState =
    AUDIO_STREAM_IDLE;

  audioStreamId = "";

  if (
    headerWritten !=
    AUDIO_STREAM_WAV_HEADER_SIZE
  ) {
    Serial.println(
      "[AUDIO-STREAM] Failed to rewrite WAV header"
    );

    SD.remove(
      AUDIO_STREAM_PART_FILENAME
    );

    return;
  }

  if (
    SD.exists(AUDIO_STREAM_FINAL_FILENAME)
  ) {
    SD.remove(
      AUDIO_STREAM_FINAL_FILENAME
    );
  }

  if (
    !SD.rename(
      AUDIO_STREAM_PART_FILENAME,
      AUDIO_STREAM_FINAL_FILENAME
    )
  ) {
    Serial.println(
      "[AUDIO-STREAM] Rename to final WAV failed"
    );
    return;
  }

  Serial.printf(
    "[AUDIO-STREAM] Finalized %lu bytes PCM, "
    "playing %s\n",
    static_cast<unsigned long>(dataSize),
    AUDIO_STREAM_FINAL_FILENAME
  );

  playSdWav(
    AUDIO_STREAM_FINAL_FILENAME
  );
}





/*
 * 结束当前流：回写正确的 WAV 头，
 * 重命名为最终文件，并复用 playSdWav() 播放。
 */
void finishAudioStream(const String& streamId) {
  if (audioStreamState != AUDIO_STREAM_RECEIVING) {
    Serial.println("[AUDIO-STREAM] audio_end ignored: no active stream");
    return;
  }

  if (streamId.length() > 0 && streamId != audioStreamId) {
    Serial.printf(
      "[AUDIO-STREAM] audio_end stream_id mismatch: expected=%s got=%s\n",
      audioStreamId.c_str(),
      streamId.c_str()
    );
    return;
  }

  audioStreamFile.flush();
  audioStreamFile.close();

  const uint32_t dataSize = audioStreamBytesWritten;

  /*
   * 关键：这里必须用 "r+" 重新打开，不能用 FILE_WRITE。
   * "r+" 不会截断已有内容，且初始文件位置在 0，
   * 可以直接覆盖写入前 44 字节的占位头。
   */
  File finalizeFile = SD.open(AUDIO_STREAM_PART_FILENAME, "r+");

  if (!finalizeFile) {
    Serial.println(
      "[AUDIO-STREAM] Failed to reopen part file for header finalize"
    );
    audioStreamState = AUDIO_STREAM_IDLE;
    audioStreamId = "";
    return;
  }

  uint8_t headerBuffer[AUDIO_STREAM_WAV_HEADER_SIZE];

  buildAudioStreamWavHeader(
    headerBuffer,
    audioStreamSampleRate,
    audioStreamChannels,
    audioStreamBitsPerSample,
    dataSize
  );

  finalizeFile.seek(0);

  const size_t headerWritten = finalizeFile.write(
    headerBuffer,
    AUDIO_STREAM_WAV_HEADER_SIZE
  );

  finalizeFile.flush();
  finalizeFile.close();

  audioStreamState = AUDIO_STREAM_IDLE;
  audioStreamId = "";

  if (headerWritten != AUDIO_STREAM_WAV_HEADER_SIZE) {
    Serial.println("[AUDIO-STREAM] Failed to rewrite WAV header");
    SD.remove(AUDIO_STREAM_PART_FILENAME);
    return;
  }

  if (SD.exists(AUDIO_STREAM_FINAL_FILENAME)) {
    SD.remove(AUDIO_STREAM_FINAL_FILENAME);
  }

  if (!SD.rename(AUDIO_STREAM_PART_FILENAME, AUDIO_STREAM_FINAL_FILENAME)) {
    Serial.println("[AUDIO-STREAM] Rename to final WAV failed");
    return;
  }

  Serial.printf(
    "[AUDIO-STREAM] Finalized %lu bytes PCM, playing %s\n",
    static_cast<unsigned long>(dataSize),
    AUDIO_STREAM_FINAL_FILENAME
  );

  playSdWav(AUDIO_STREAM_FINAL_FILENAME);
}


/*
 * 中止当前流（audio_stop，或被新流打断，或写入失败）。
 *
 * 停止扬声器、关闭并删除临时文件、重置音频流状态。
 */
void abortAudioStream() {
  if (audioStreamState == AUDIO_STREAM_RECEIVING) {
    audioStreamFile.close();
  }

  if (SD.exists(AUDIO_STREAM_PART_FILENAME)) {
    SD.remove(AUDIO_STREAM_PART_FILENAME);
  }

  audioStreamState = AUDIO_STREAM_IDLE;
  audioStreamId = "";
  audioStreamBytesWritten = 0;

  M5.Speaker.stop();

  Serial.println("[AUDIO-STREAM] Stream aborted, temp file removed");
}


/*
 * 通过 WebSocket 通知 Render：可以开始发送二进制 PCM 帧了。
 */
void sendAudioReadyEvent(const String& streamId) {
  if (!webSocket.isConnected()) {
    return;
  }

  StaticJsonDocument<128> doc;

  doc["type"] = "audio_ready";
  doc["stream_id"] = streamId;

  String json;
  serializeJson(doc, json);

  webSocket.sendTXT(json);
}


/*
 * 通过 WebSocket 确认已写入的累计字节数（用于 Render 端流控）。
 */
 
 void sendAudioChunkAckEvent(
  const String& streamId,
  uint32_t bytesWritten
) {
  if (!webSocket.isConnected()) {
    Serial.println(
      "[AUDIO-STREAM] Cannot send ACK: "
      "WebSocket disconnected"
    );
    return;
  }

  StaticJsonDocument<192> doc;

  doc["type"] =
    "audio_chunk_ack";

  doc["stream_id"] =
    streamId;

  doc["bytes_written"] =
    bytesWritten;

  String json;
  serializeJson(doc, json);

  Serial.printf(
    "[AUDIO-STREAM] Sending ACK: %s\n",
    json.c_str()
  );

  webSocket.sendTXT(json);

  Serial.println(
    "[AUDIO-STREAM] ACK sendTXT completed"
  );
}





/*
 * 通过 WebSocket 通知 Render：当前流已被设备端中止。
 */
void sendAudioAbortEvent(const String& streamId, const char* reason) {
  if (!webSocket.isConnected()) {
    return;
  }

  StaticJsonDocument<160> doc;

  doc["type"] = "audio_abort";
  doc["stream_id"] = streamId;
  doc["reason"] = reason;

  String json;
  serializeJson(doc, json);

  webSocket.sendTXT(json);
}


/*
 * 根据名称播放声音。
 */
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


/*
 * 显示启动信息。
 *
 * 只能在 avatar.init() 之前调用。
 */
void showBootMessage(
  const char* line1,
  const char* line2 = nullptr
) {
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


/*
 * 设置原生表情。
 */
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


/*
 * 设置自定义脸部效果。
 */
 void setFaceEffectByName(const char* effect) {
  if (effect == nullptr) {
    return;
  }

  /*
   * 如果角色在晕眩期间主动设置了新的 face_effect，
   * 以角色的新指令为准。
   *
   * 这样不会在远程新表情设置后，
   * 又被旧的 shake 状态恢复覆盖。
   */
  shakeDizzyActive = false;
  shakeDizzyUntilMs = 0;

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

  Serial.printf(
    "[FACE] Effect: %s\n",
    effect
  );
}

void startShakeDizzyEffect() {
  const unsigned long now = millis();

  /*
   * 连续摇晃时不保存当前的 dizzy_eyes，
   * 只延长晕眩时间。
   */
  if (shakeDizzyActive) {
    shakeDizzyUntilMs =
      now + SHAKE_DIZZY_DURATION_MS;

    Serial.println(
      "[FACE] Shake detected again; "
      "dizzy effect extended"
    );

    return;
  }

  shakePreviousFaceEffect =
    faceEffectState.get();

  shakeDizzyActive = true;

  shakeDizzyUntilMs =
    now + SHAKE_DIZZY_DURATION_MS;

  faceEffectState.set(FaceEffect::DizzyEyes);

  Serial.println(
    "[FACE] Shake feedback: dizzy_eyes started"
  );
}


void updateShakeDizzyEffect() {
  if (!shakeDizzyActive) {
    return;
  }

  const unsigned long now = millis();

  if (
    static_cast<long>(now - shakeDizzyUntilMs) < 0
  ) {
    return;
  }

  faceEffectState.set(shakePreviousFaceEffect);

  shakeDizzyActive = false;
  shakeDizzyUntilMs = 0;

  Serial.println(
    "[FACE] Shake feedback: "
    "previous face effect restored"
  );
}

/*
 * 限制屏幕文字显示时间。
 */
unsigned long clampDisplayDuration(unsigned long durationMs) {
  if (durationMs < MIN_DISPLAY_DURATION_MS) {
    return MIN_DISPLAY_DURATION_MS;
  }

  if (durationMs > MAX_DISPLAY_DURATION_MS) {
    return MAX_DISPLAY_DURATION_MS;
  }

  return durationMs;
}


/*
 * 设置屏幕文字及显示时长。
 */
void setSpeechTextForDuration(
  const char* text,
  unsigned long durationMs
) {
  if (text == nullptr) {
    return;
  }

  activeSpeechText = text;
  avatar.setSpeechText(activeSpeechText.c_str());

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


/*
 * 自动清除屏幕文字。
 */
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


/*
 * 取消当前舵机动作。
 */
void cancelGesture() {
  activeGesture = GESTURE_NONE;
  gestureStep = 0;
  nextGestureStepMs = 0;
}


/*
 * 启动通用舵机动作。
 */
void startGesture(GestureKind gesture, const char* name) {
  if (activeGesture != GESTURE_NONE) {
    Serial.printf(
      "[GESTURE] Ignored %s: another gesture is active\n",
      name
    );
    return;
  }

  activeGesture = gesture;
  gestureStep = 0;
  nextGestureStepMs = 0;

  Serial.printf("[GESTURE] %s started\n", name);
}


/*
 * 启动点头。
 */
void startNod() {
  startGesture(GESTURE_NOD, "nod");
}


/*
 * 启动机器人主动左右摇头。
 */
void startShakeHead() {
  startGesture(GESTURE_SHAKE_HEAD, "shake_head");
}


/*
 * 启动向左看。
 */
void startLookLeft() {
  startGesture(GESTURE_LOOK_LEFT, "look_left");
}


/*
 * 启动向右看。
 */
void startLookRight() {
  startGesture(GESTURE_LOOK_RIGHT, "look_right");
}


/*
 * 启动抬头。
 */
void startTiltUp() {
  startGesture(GESTURE_TILT_UP, "tilt_up");
}



/*
 * 更新舵机动作。
 *
 * 使用保守测试范围：
 *
 * X = ±300
 * Y = 0、50、300、420
 */
void updateGesture() {
  if (activeGesture == GESTURE_NONE) {
    return;
  }

  const unsigned long now = millis();

  if (
    nextGestureStepMs != 0 &&
    static_cast<long>(now - nextGestureStepMs) < 0
  ) {
    return;
  }

  switch (activeGesture) {
    case GESTURE_NOD:
      switch (gestureStep) {
        case 0:
          M5StackChan.Motion.moveY(300, 500);
          Serial.println("[GESTURE] Nod step 1: Y=300");
          nextGestureStepMs = now + 350;
          break;

        case 1:
          M5StackChan.Motion.moveY(50, 600);
          Serial.println("[GESTURE] Nod step 2: Y=50");
          nextGestureStepMs = now + 350;
          break;

        case 2:
          M5StackChan.Motion.moveY(300, 500);
          Serial.println("[GESTURE] Nod step 3: Y=300");
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
      return;


    case GESTURE_SHAKE_HEAD:
      switch (gestureStep) {
        case 0:
          M5StackChan.Motion.moveX(-300, 500);
          Serial.println("[GESTURE] Shake-head step 1: X=-300");
          nextGestureStepMs = now + 550;
          break;

        case 1:
          M5StackChan.Motion.moveX(300, 500);
          Serial.println("[GESTURE] Shake-head step 2: X=300");
          nextGestureStepMs = now + 550;
          break;

        case 2:
          M5StackChan.Motion.moveX(-300, 500);
          Serial.println("[GESTURE] Shake-head step 3: X=-300");
          nextGestureStepMs = now + 550;
          break;

        case 3:
          M5StackChan.Motion.goHome(600);
          Serial.println("[GESTURE] Shake-head complete");
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
        M5StackChan.Motion.moveX(-300, 600);
        Serial.println("[GESTURE] Look-left: X=-300");
        nextGestureStepMs = now + 900;
        gestureStep++;
      } else {
        M5StackChan.Motion.goHome(600);
        Serial.println("[GESTURE] Look-left complete");
        cancelGesture();
      }

      return;


    case GESTURE_LOOK_RIGHT:
      if (gestureStep == 0) {
        M5StackChan.Motion.moveX(300, 600);
        Serial.println("[GESTURE] Look-right: X=300");
        nextGestureStepMs = now + 900;
        gestureStep++;
      } else {
        M5StackChan.Motion.goHome(600);
        Serial.println("[GESTURE] Look-right complete");
        cancelGesture();
      }

      return;


    case GESTURE_TILT_UP:
      if (gestureStep == 0) {
        M5StackChan.Motion.moveY(420, 600);
        Serial.println("[GESTURE] Tilt-up: Y=420");
        nextGestureStepMs = now + 900;
        gestureStep++;
      } else {
        M5StackChan.Motion.goHome(600);
        Serial.println("[GESTURE] Tilt-up complete");
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
 * 通过 WebSocket 上报屏幕触摸事件。
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


/*
 * 通过 WebSocket 上报头顶触摸事件。
 */
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


/*
 * 通过 WebSocket 上报用户摇晃事件。
 *
 * shake 表示用户摇晃机器人，
 * 不是机器人主动执行摇头动作。
 */
void sendShakeEvent() {
  if (!webSocket.isConnected()) {
    Serial.println(
      "[IMU] Shake detected, but WebSocket is disconnected"
    );
    return;
  }

  StaticJsonDocument<128> eventDoc;

  eventDoc["type"] = "robot_event";
  eventDoc["event"] = "shake";
  eventDoc["at_ms"] = millis();

  String eventJson;
  serializeJson(eventDoc, eventJson);

  webSocket.sendTXT(eventJson);

  Serial.println("[IMU] shake sent");
}


/*
 * 更新 IMU 摇晃检测。
 */
void updateShakeDetection() {
  const unsigned long now = millis();

  if (
    lastImuReadMs != 0 &&
    now - lastImuReadMs < IMU_READ_INTERVAL_MS
  ) {
    return;
  }

  lastImuReadMs = now;

  float ax = 0.0f;
  float ay = 0.0f;
  float az = 0.0f;

  if (!M5.Imu.getAccel(&ax, &ay, &az)) {
    if (imuReady) {
      Serial.println("[IMU] Failed to read acceleration");
    }

    imuReady = false;
    return;
  }

  if (!imuReady) {
    imuReady = true;

    lastAccelX = ax;
    lastAccelY = ay;
    lastAccelZ = az;

    Serial.println("[IMU] Acceleration reading started");

    Serial.printf(
      "[IMU] ax=%.3f ay=%.3f az=%.3f\n",
      ax,
      ay,
      az
    );

    return;
  }

  const float dx = ax - lastAccelX;
  const float dy = ay - lastAccelY;
  const float dz = az - lastAccelZ;

  const float deltaMagnitude =
    sqrtf(dx * dx + dy * dy + dz * dz);

  lastAccelX = ax;
  lastAccelY = ay;
  lastAccelZ = az;

  if (
    deltaMagnitude >= SHAKE_DELTA_THRESHOLD &&
    (
      lastShakeEventMs == 0 ||
      now - lastShakeEventMs >= SHAKE_DEBOUNCE_MS
    )
  ) {
    lastShakeEventMs = now;

   Serial.printf(
  "[IMU] SHAKE detected, delta=%.3f "
  "ax=%.3f ay=%.3f az=%.3f\n",
  deltaMagnitude,
  ax,
  ay,
  az
);

startShakeDizzyEffect();
sendShakeEvent();


    /*
     * 用户摇晃只上报事件，
     * 不自动触发舵机动作。
     */

  }
}


/*
 * 更新头顶触摸输入。
 */
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
   * 仅在“未触摸 -> 触摸”的边缘上报一次。
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

  startHeadTouchLightEffect();
  sendHeadTouchEvent();
}


/*
 * 更新屏幕触摸输入。
 *
 * M5StackChan.update() 已经在 loop() 开头执行，
 * 因此这里不再调用 M5.update()。
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


/*
 * WebSocket 事件处理。
 */
void webSocketEvent(
  WStype_t type,
  uint8_t* payload,
  size_t length
) {

  Serial.printf(
  "[WS] Event type=%d, length=%u\n",
  static_cast<int>(type),
  static_cast<unsigned>(length)
);

  switch (type) {
    case WStype_CONNECTED:
      Serial.println("[WS] Connected to Render");

      avatar.setExpression(Expression::Happy);
      cancelGesture();
      M5StackChan.Motion.goHome(500);
      break;


    case WStype_DISCONNECTED:
      Serial.println("[WS] Disconnected from Render");

      /*
       * 连接断开时，正在接收的音频流已不可能收到
       * audio_end，主动中止并清理临时文件，
       * 避免下次连接后遗留半成品文件。
       */
      if (audioStreamState == AUDIO_STREAM_RECEIVING) {
        abortAudioStream();
      }

      break;


    case WStype_ERROR:
      Serial.println("[WS] WebSocket error");
      break;

  case WStype_BIN: {
  Serial.printf(
    "[WS] Binary message received: %u bytes\n",
    static_cast<unsigned>(length)
  );

  /*
   * 非分片的完整二进制消息。
   */
  writeAudioStreamChunk(
    payload,
    length
  );

  break;
}


case WStype_FRAGMENT_BIN_START: {
  Serial.printf(
    "[WS] Binary message start: %u bytes\n",
    static_cast<unsigned>(length)
  );

  /*
   * 当前 Node.js ws 发送的 4096 字节数据
   * 会从这里进入。
   */
  if (length > 0) {
    writeAudioStreamChunk(
      payload,
      length
    );
  }

  break;
}


case WStype_FRAGMENT: {
  Serial.printf(
    "[WS] Binary continuation: %u bytes\n",
    static_cast<unsigned>(length)
  );

  /*
   * 只有确实带有数据的 continuation
   * 才写入音频文件。
   */
  if (length > 0) {
    writeAudioStreamChunk(
      payload,
      length
    );
  }

  break;
}


case WStype_FRAGMENT_FIN: {
  Serial.printf(
    "[WS] Binary message finish: %u bytes\n",
    static_cast<unsigned>(length)
  );

  /*
   * 当前实际情况 length=0。
   * 它只是消息结束通知，不能再次写入，
   * 也不能再次发送 ACK。
   */
  if (length > 0) {
    writeAudioStreamChunk(
      payload,
      length
    );
  }

  break;
}





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

      /*
       * 音频流控制消息优先处理，处理完直接返回，
       * 不与下面的 expression/motion/sound 等
       * 动作字段混在一起解析。
       */
      const char* msgType = doc["type"];

      if (msgType != nullptr && strcmp(msgType, "audio_start") == 0) {
        const char* streamIdRaw = doc["stream_id"];

        String streamId =
          streamIdRaw != nullptr ? String(streamIdRaw) : String("");

        const uint32_t sampleRate = doc["sample_rate"] | 16000;
        const uint16_t channels =
          static_cast<uint16_t>(doc["channels"] | 1);
        const uint16_t bitsPerSample =
          static_cast<uint16_t>(doc["bits_per_sample"] | 16);

        if (beginAudioStream(streamId, sampleRate, channels, bitsPerSample)) {
          sendAudioReadyEvent(streamId);
        } else {
          sendAudioAbortEvent(streamId, "start_failed");
        }

        return;
      }

      if (msgType != nullptr && strcmp(msgType, "audio_end") == 0) {
        const char* streamIdRaw = doc["stream_id"];

        String streamId =
          streamIdRaw != nullptr ? String(streamIdRaw) : String("");

        finishAudioStream(streamId);
        return;
      }

      if (msgType != nullptr && strcmp(msgType, "audio_stop") == 0) {
        Serial.println("[AUDIO-STREAM] audio_stop received");
        abortAudioStream();
        return;
      }

      const char* expression = doc["expression"];
      const char* faceEffect = doc["face_effect"];
      const char* motion = doc["motion"];
      const char* sound = doc["sound"];
      const char* action = doc["action"];

      /*
       * 只有 Render 明确发送 text_to_display 时才更新文字。
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

        } else if (strcmp(motion, "shake_head") == 0) {
          startShakeHead();

        } else if (strcmp(motion, "look_left") == 0) {
          startLookLeft();

        } else if (strcmp(motion, "look_right") == 0) {
          startLookRight();

        } else if (strcmp(motion, "tilt_up") == 0) {
          startTiltUp();



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
           * shake 是用户摇晃事件，
           * 不是机器人主动舵机动作。
           */
          Serial.println(
            "[GESTURE] 'shake' is a user event, "
            "not a robot motion"
          );

        } else {
          Serial.printf(
            "[GESTURE] Unknown motion ignored: %s\n",
            motion
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


/*
 * 根据脸部效果和原生表情获取灯光主题。
 */
LedTheme getLedTheme() {
  FaceEffect effect = faceEffectState.get();

  /*
   * 自定义 face_effect 优先级高于 expression。
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
   * 没有 face_effect 时使用 expression 主题。
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


/*
 * 更新 RGB 呼吸灯。
 */
void updateLedBreath() {
  const unsigned long now = millis();

  if (now - lastLedUpdateMs < LED_UPDATE_INTERVAL_MS) {
    return;
  }

  lastLedUpdateMs = now;

  const LedTheme theme = getLedTheme();

  const uint16_t periodMs =
    theme.periodMs == 0
      ? 1
      : theme.periodMs;

  const float phase =
    static_cast<float>(now % periodMs) /
    static_cast<float>(periodMs);

  const float wave =
    (sinf(phase * 6.2831853f) + 1.0f) * 0.5f;

  const float maxBrightness =
    static_cast<float>(theme.maxBrightness) / 255.0f;

  const float minBrightness =
    maxBrightness * 0.15f;

  float brightness =
    minBrightness +
    (maxBrightness - minBrightness) * wave;

  /*
   * 屏幕有文字时增强亮度。
   */
  if (activeSpeechText.length() > 0) {
    brightness *= SPEAKING_LIGHT_BOOST;
  }

  /*
   * 头顶触摸临时粉色闪烁。
   */
  bool useHeadTouchPink = false;

  if (headTouchLightActive) {
    const unsigned long totalDuration =
      HEAD_TOUCH_LIGHT_CYCLE_MS *
      HEAD_TOUCH_LIGHT_FLASH_COUNT;

    const unsigned long elapsed =
      now - headTouchLightStartedMs;

    if (elapsed >= totalDuration) {
      headTouchLightActive = false;

      Serial.println(
        "[LED] Head-touch pink flash finished"
      );
    } else {
      const unsigned long cycleElapsed =
        elapsed % HEAD_TOUCH_LIGHT_CYCLE_MS;

      const float cyclePhase =
        static_cast<float>(cycleElapsed) /
        static_cast<float>(HEAD_TOUCH_LIGHT_CYCLE_MS);

      const float flashWave =
        sinf(cyclePhase * 3.14159265f);

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

  uint8_t r;
  uint8_t g;
  uint8_t b;

  if (useHeadTouchPink) {
    r = static_cast<uint8_t>(255.0f * brightness);
    g = static_cast<uint8_t>(45.0f * brightness);
    b = static_cast<uint8_t>(150.0f * brightness);
  } else {
    r = static_cast<uint8_t>(
      static_cast<float>(theme.r) * brightness
    );

    g = static_cast<uint8_t>(
      static_cast<float>(theme.g) * brightness
    );

    b = static_cast<uint8_t>(
      static_cast<float>(theme.b) * brightness
    );
  }

  for (uint8_t i = 0; i < LED_COUNT; ++i) {
    M5StackChan.setRgbColor(i, r, g, b);
  }

  M5StackChan.refreshRgb();
}


/*
 * 开始头顶触摸粉色灯效。
 */
void startHeadTouchLightEffect() {
  headTouchLightActive = true;
  headTouchLightStartedMs = millis();

  Serial.println(
    "[LED] Head-touch soft flash started"
  );
}


/*
 * 初始化。
 */
void setup() {
  Serial.begin(115200);
  delay(1000);

  Serial.println();

  Serial.println(
    "=== StackChan WiFi + Render + Servo + "
    "Display + Touch + IMU firmware ==="
  );

  /*
   * StackChan-BSP 负责设备、舵机供电和舵机总线初始化。
   */
  M5StackChan.begin();

  /*
   * 初始化 RGB 灯。
   */
  lastLedUpdateMs =
    millis() - LED_UPDATE_INTERVAL_MS;

  updateLedBreath();


  /*
   * 初始化 CoreS3 microSD。
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

    /*
     * 清理上次异常断电/断线可能残留的临时流文件，
     * 避免占用 SD 空间或被误当成完整音频。
     */
    if (SD.exists(AUDIO_STREAM_PART_FILENAME)) {
      SD.remove(AUDIO_STREAM_PART_FILENAME);
      Serial.println(
        "[SD] Removed leftover /audio.wav.part from previous session"
      );
    }
  } else {
    Serial.println("[SD] Card mount failed");
  }


  /*
   * M5Unified 音量范围为 0 至 255。
   */
  M5.Speaker.setVolume(80);


  /*
   * 舵机设置。
   */
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


  /*
   * 读取已保存的 Render 主机地址。
   */
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


  /*
   * Wi-Fi 配置。
   */
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
     * 不设置门户超时，
     * 等待用户完成配网。
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


  /*
   * 保存本次输入的 Render 地址。
   */
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
   * 让 Avatar 的 Balloon 使用中文字体。
   */
  avatar.setSpeechFont(&fonts::efontCN_10);


  /*
   * 默认状态：
   * - neutral 表情；
   * - 原生眼睛；
   * - 无文字。
   */
  faceEffectState.set(FaceEffect::None);
  avatar.setExpression(Expression::Neutral);
  avatar.setSpeechText("");


  /*
   * 如果没有 Wi-Fi，不启动 WebSocket。
   */
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


  /*
   * 启动安全 WebSocket。
   */
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


/*
 * 主循环。
 */
void loop() {
  M5StackChan.update();
  webSocket.loop();

  updateGesture();
  updateSpeechText();
  updateTouchInput();
  updateHeadTouchInput();

  updateShakeDetection();
  updateShakeDizzyEffect();

  updateLedBreath();

  delay(10);
}