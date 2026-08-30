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

  // 1. 由 M5StackChan 完成所有底层硬件（IO、供电、舵机）的初始化
  M5StackChan.begin();

  // 2. 舵机安全配置
  M5StackChan.Motion.setAutoAngleSyncEnabled(false);
  M5StackChan.Motion.setAutoTorqueReleaseEnabled(true);

  // 3. 舵机回中
  M5StackChan.Motion.goHome(500);

  // 4. 初始化 Avatar 表情
  avatar.init();
  avatar.setExpression(Expression::Neutral);

  nextStepAt = millis() + 1500;
  Serial.println("[TEST] Setup finished. Preparing gesture...");
}

void loop() {
  // 注意：只调用 M5StackChan.update()，它会自行调度内部逻辑
  M5StackChan.update();

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
