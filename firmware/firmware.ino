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

void setup() {
  Serial.begin(115200);
  delay(1000);

  // 1. 初始化 M5 硬件（统一屏幕与底层总线）
  auto cfg = M5.config();
  M5.begin(cfg);

  // 2. 单独初始化舵机运动总线与供电，避免 M5StackChan.begin() 抢占屏幕
  M5StackChan.Motion.begin();
  M5StackChan.Motion.setAutoAngleSyncEnabled(false);
  M5StackChan.Motion.setAutoTorqueReleaseEnabled(true);

  // 3. 启动 M5-Avatar 表情系统
  avatar.init();
  avatar.setExpression(Expression::Neutral);

  // 4. 舵机回中
  M5StackChan.Motion.goHome(500);

  nextStepAt = millis() + 1500;
  Serial.println("[TEST] Setup complete. Awaiting nod gesture.");
}

void loop() {
  M5.update();
  M5StackChan.Motion.update();

  const unsigned long now = millis();
  if (now < nextStepAt) {
    delay(20);
    return;
  }

  switch (nodStep) {
    case NOD_WAIT:
      avatar.setExpression(Expression::Happy);
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
      Serial.println("[TEST] Nod complete; goHome sent.");
      nodStep = NOD_HOME;
      nextStepAt = now + 800;
      break;

    case NOD_HOME:
      nodStep = NOD_DONE;
      Serial.println("[TEST] Complete.");
      break;

    case NOD_DONE:
      break;
  }

  delay(20);
}
