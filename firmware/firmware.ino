#include <Arduino.h>
#include <M5Unified.h>
#include <M5StackChan.h>

enum NodStep {
  NOD_WAIT = 0,
  NOD_DOWN_1,
  NOD_UP,
  NOD_DOWN_2,
  NOD_HOME,
  NOD_DONE
};

NodStep nodStep = NOD_WAIT;
unsigned long nextStepAt = 0;

void showMessage(const char* line1, const char* line2 = nullptr) {
  M5.Display.clear(TFT_BLACK);
  M5.Display.setTextSize(2);
  M5.Display.setTextColor(TFT_WHITE, TFT_BLACK);
  M5.Display.setCursor(20, 70);
  M5.Display.println(line1);

  if (line2) {
    M5.Display.setCursor(20, 105);
    M5.Display.println(line2);
  }
}

void setup() {
  Serial.begin(115200);
  delay(1000);

  // 由 StackChan-BSP 初始化硬件；不手动定义舵机引脚。
  M5StackChan.begin();

  M5StackChan.Motion.setAutoAngleSyncEnabled(false);
  M5StackChan.Motion.setAutoTorqueReleaseEnabled(true);

  showMessage("Servo nod test", "Returning home...");
  M5StackChan.Motion.goHome(500);

  // 留出时间让归位动作开始执行，再进入单次点头。
  nextStepAt = millis() + 1200;
  Serial.println("[SERVO TEST] Home sent; nod will start shortly.");
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
      showMessage("Servo nod test", "Nodding once...");
      M5StackChan.Motion.moveY(300, 500);  // 30.0°
      Serial.println("[SERVO TEST] Nod: Y=30.0");
      nodStep = NOD_DOWN_1;
      nextStepAt = now + 350;
      break;

    case NOD_DOWN_1:
      M5StackChan.Motion.moveY(50, 600);   // 5.0°
      Serial.println("[SERVO TEST] Nod: Y=5.0");
      nodStep = NOD_UP;
      nextStepAt = now + 350;
      break;

    case NOD_UP:
      M5StackChan.Motion.moveY(300, 500);  // 30.0°
      Serial.println("[SERVO TEST] Nod: Y=30.0");
      nodStep = NOD_DOWN_2;
      nextStepAt = now + 350;
      break;

    case NOD_DOWN_2:
      M5StackChan.Motion.goHome(500);
      showMessage("Servo nod test", "Done: home");
      Serial.println("[SERVO TEST] Nod complete; goHome sent.");
      nodStep = NOD_HOME;
      nextStepAt = now + 800;
      break;

    case NOD_HOME:
      nodStep = NOD_DONE;
      Serial.println("[SERVO TEST] Complete.");
      break;

    case NOD_DONE:
      // 之后不再发出任何运动指令。
      break;
  }

  delay(20);
}

