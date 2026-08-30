#include <Arduino.h>
#include <M5Unified.h>
#include <M5StackChan.h>
#include <Avatar.h>

using namespace m5avatar;

Avatar avatar;

enum NodStep {
  NOD_WAIT = 0,
  NOD_TO_30,
  NOD_TO_5,
  NOD_TO_30_AGAIN,
  NOD_HOME,
  NOD_DONE
};

NodStep nodStep = NOD_WAIT;
unsigned long nextStepAt = 0;

void showStatus(const char* status) {
  M5.Display.fillRect(0, 205, 320, 35, TFT_BLACK);
  M5.Display.setTextSize(1);
  M5.Display.setTextColor(TFT_WHITE, TFT_BLACK);
  M5.Display.setCursor(8, 214);
  M5.Display.print(status);
}

void setup() {
  Serial.begin(115200);
  delay(1000);

  // 仅由 StackChan-BSP 初始化底层硬件。
  // 不再额外调用 M5.begin()，避免重复初始化。
  M5StackChan.begin();

  M5StackChan.Motion.setAutoAngleSyncEnabled(false);
  M5StackChan.Motion.setAutoTorqueReleaseEnabled(true);

  // 在 BSP 初始化完成后启动 Avatar 表情。
  avatar.init();
  avatar.setExpression(Expression::Neutral);

  showStatus("Avatar + Servo test: homing...");

  // 已在本机验证安全的归位动作。
  M5StackChan.Motion.goHome(500);

  // 给归位留出时间，然后仅点头一次。
  nextStepAt = millis() + 1500;

  Serial.println("[TEST] Avatar initialized; goHome sent.");
}

void loop() {
  M5StackChan.update();

  const unsigned long now = millis();
  if (now < nextStepAt) {
    delay(20);
    return;
  }

  switch (nodStep) {
    case NOD_WAIT:
      avatar.setExpression(Expression::Happy);
      showStatus("Avatar + Servo test: nodding...");
      M5StackChan.Motion.moveY(300, 500);  // 30.0°
      Serial.println("[TEST] Nod: Y=30.0");
      nodStep = NOD_TO_30;
      nextStepAt = now + 350;
      break;

    case NOD_TO_30:
      M5StackChan.Motion.moveY(50, 600);   // 5.0°
      Serial.println("[TEST] Nod: Y=5.0");
      nodStep = NOD_TO_5;
      nextStepAt = now + 350;
      break;

    case NOD_TO_5:
      M5StackChan.Motion.moveY(300, 500);  // 30.0°
      Serial.println("[TEST] Nod: Y=30.0");
      nodStep = NOD_TO_30_AGAIN;
      nextStepAt = now + 350;
      break;

    case NOD_TO_30_AGAIN:
      M5StackChan.Motion.goHome(500);
      avatar.setExpression(Expression::Neutral);
      showStatus("Avatar + Servo test: done / home");
      Serial.println("[TEST] Nod complete; goHome sent.");
      nodStep = NOD_HOME;
      nextStepAt = now + 800;
      break;

    case NOD_HOME:
      nodStep = NOD_DONE;
      Serial.println("[TEST] Complete.");
      break;

    case NOD_DONE:
      // 后续不再发送运动命令。
      break;
  }

  delay(20);
}
