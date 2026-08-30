#include <Arduino.h>
#include <M5Unified.h>
#include <M5StackChan.h>

void setup() {
  Serial.begin(115200);
  delay(1000);

  // 由 StackChan-BSP 初始化 CoreS3 与 StackChan 运动硬件。
  // 不手动指定任何舵机 GPIO、UART 或 PWM 参数。
  M5StackChan.begin();

  M5.Display.setTextSize(2);
  M5.Display.clear(TFT_BLACK);
  M5.Display.setTextColor(TFT_WHITE, TFT_BLACK);
  M5.Display.setCursor(20, 80);
  M5.Display.println("Servo home test");
  M5.Display.println("Returning home...");

  // 与参考项目相同的安全初始化策略。
  M5StackChan.Motion.setAutoAngleSyncEnabled(false);
  M5StackChan.Motion.setAutoTorqueReleaseEnabled(true);

  // 仅执行官方 BSP 定义的归位动作。
  // 不做 nod / shake / 任意角度 move。
  M5StackChan.Motion.goHome(500);

  Serial.println("[SERVO TEST] goHome(500) sent");
}

void loop() {
  M5StackChan.update();
  delay(20);
}

