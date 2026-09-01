#ifndef CUSTOM_EYE_H_
#define CUSTOM_EYE_H_

#include <BoundingRect.h>
#include <DrawContext.h>
#include <Drawable.h>
#include <Eye.h>
#include <M5GFX.h>

/*
 * face_effect 是独立于 Expression 的视觉效果。
 *
 * Expression:
 *   neutral / happy / sad / angry / doubt / sleepy
 *
 * FaceEffect:
 *   none / heart_eyes
 */
enum class FaceEffect : uint8_t {
  None = 0,

  // 已有眼睛效果
  HeartEyes,
  SparkleEyes,
  DizzyEyes,
  TearEyes,

  // 新增完整脸部效果
  SurprisedFace,
  PoutFace,
  ShyFace,
  SmugFace,
  ConfusedFace
};



/*
 * 两只 CustomEye 共用同一个状态对象。
 *
 * 主循环中的 WebSocket 指令调用 set()；
 * Avatar 的绘制任务调用 get()。
 *
 * 当前只读写一个 uint8_t 枚举值，适合这个轻量用途。
 */
class FaceEffectState {
 public:
  FaceEffectState();

  void set(FaceEffect effect);
  FaceEffect get() const;

 private:
  volatile uint8_t effect_;
};

/*
 * 可切换的代理眼睛。
 *
 * FaceEffect::None:
 *   直接委托给库原生 m5avatar::Eye，
 *   所以全部六种内置 Expression 的眼睛逻辑完全保留。
 *
 * FaceEffect::HeartEyes:
 *   绘制自定义爱心眼。
 */
class CustomEye final : public m5avatar::Drawable {
 public:
  CustomEye(bool isLeft, FaceEffectState *effectState);

  void draw(M5Canvas *canvas,
            m5avatar::BoundingRect rect,
            m5avatar::DrawContext *ctx) override;

 private:
 void drawHeartEyes(M5Canvas *canvas,
                   m5avatar::BoundingRect rect,
                   m5avatar::DrawContext *ctx);

void drawSparkleEyes(M5Canvas *canvas,
                     m5avatar::BoundingRect rect,
                     m5avatar::DrawContext *ctx);

void drawDizzyEyes(M5Canvas *canvas,
                   m5avatar::BoundingRect rect,
                   m5avatar::DrawContext *ctx);

void drawTearEyes(M5Canvas *canvas,
                  m5avatar::BoundingRect rect,
                  m5avatar::DrawContext *ctx);

                  void drawSurprisedFace(M5Canvas *canvas,
                       m5avatar::BoundingRect rect,
                       m5avatar::DrawContext *ctx);

void drawPoutFace(M5Canvas *canvas,
                  m5avatar::BoundingRect rect,
                  m5avatar::DrawContext *ctx);

void drawShyFace(M5Canvas *canvas,
                 m5avatar::BoundingRect rect,
                 m5avatar::DrawContext *ctx);

void drawSmugFace(M5Canvas *canvas,
                  m5avatar::BoundingRect rect,
                  m5avatar::DrawContext *ctx);

void drawConfusedFace(M5Canvas *canvas,
                      m5avatar::BoundingRect rect,
                      m5avatar::DrawContext *ctx);



  bool isLeft_;
  FaceEffectState *effectState_;

  /*
   * 原生 Eye 保留在这里。
   * FaceEffect::None 时直接调用它的 draw()。
   */
  m5avatar::Eye normalEye_;
};

#endif  // CUSTOM_EYE_H_
void CustomEye::drawSurprisedFace(
    M5Canvas *canvas,
    m5avatar::BoundingRect rect,
    m5avatar::DrawContext *ctx) {
  int centerX = rect.getCenterX();
  int centerY = rect.getCenterY();

  m5avatar::Gaze gaze =
      isLeft_ ? ctx->getLeftGaze() : ctx->getRightGaze();

  int x = centerX + static_cast<int>(gaze.getHorizontal() * 6.0f);
  int y = centerY + static_cast<int>(gaze.getVertical() * 4.0f);

  float openRatio =
      isLeft_ ? ctx->getLeftEyeOpenRatio()
              : ctx->getRightEyeOpenRatio();

  uint16_t primaryColor =
      ctx->getColorDepth() == 1
          ? 1
          : ctx->getColorPalette()->get(COLOR_PRIMARY);

  uint16_t backgroundColor =
      ctx->getColorDepth() == 1
          ? 0
          : ctx->getColorPalette()->get(COLOR_BACKGROUND);

  if (openRatio <= 0.10f) {
    canvas->fillRect(x - 15, y - 2, 30, 4, primaryColor);
    return;
  }

  int radius = static_cast<int>(16.0f * openRatio);
  if (radius < 5) {
    radius = 5;
  }

  canvas->fillCircle(x, y, radius, primaryColor);

  // 背景色小圆形成高光效果。
  canvas->fillCircle(
      x - radius / 3,
      y - radius / 3,
      radius / 4,
      backgroundColor);
}
void CustomEye::drawPoutFace(
    M5Canvas *canvas,
    m5avatar::BoundingRect rect,
    m5avatar::DrawContext *ctx) {
  normalEye_.draw(canvas, rect, ctx);
}
void CustomEye::drawShyFace(
    M5Canvas *canvas,
    m5avatar::BoundingRect rect,
    m5avatar::DrawContext *ctx) {
  int centerX = rect.getCenterX();
  int centerY = rect.getCenterY();

  m5avatar::Gaze gaze =
      isLeft_ ? ctx->getLeftGaze() : ctx->getRightGaze();

  int x = centerX + static_cast<int>(gaze.getHorizontal() * 4.0f);
  int y = centerY + static_cast<int>(gaze.getVertical() * 3.0f);

  float openRatio =
      isLeft_ ? ctx->getLeftEyeOpenRatio()
              : ctx->getRightEyeOpenRatio();

  uint16_t primaryColor =
      ctx->getColorDepth() == 1
          ? 1
          : ctx->getColorPalette()->get(COLOR_PRIMARY);

  if (openRatio <= 0.10f) {
    canvas->fillRect(x - 11, y - 2, 22, 4, primaryColor);
    return;
  }

  int radius = static_cast<int>(5.0f * openRatio);
  if (radius < 2) {
    radius = 2;
  }

  canvas->fillCircle(x, y, radius, primaryColor);
}
void CustomEye::drawSmugFace(
    M5Canvas *canvas,
    m5avatar::BoundingRect rect,
    m5avatar::DrawContext *ctx) {
  int centerX = rect.getCenterX();
  int centerY = rect.getCenterY();

  m5avatar::Gaze gaze =
      isLeft_ ? ctx->getLeftGaze() : ctx->getRightGaze();

  int x = centerX + static_cast<int>(gaze.getHorizontal() * 6.0f);
  int y = centerY + static_cast<int>(gaze.getVertical() * 4.0f);

  float openRatio =
      isLeft_ ? ctx->getLeftEyeOpenRatio()
              : ctx->getRightEyeOpenRatio();

  uint16_t primaryColor =
      ctx->getColorDepth() == 1
          ? 1
          : ctx->getColorPalette()->get(COLOR_PRIMARY);

  // 左眼睁开，右眼半眯。
  if (isLeft_) {
    if (openRatio <= 0.10f) {
      canvas->fillRect(x - 14, y - 2, 28, 4, primaryColor);
      return;
    }

    canvas->fillCircle(x, y, 12, primaryColor);
  } else {
    canvas->fillRect(x - 14, y + 3, 28, 4, primaryColor);
  }
}
void CustomEye::drawConfusedFace(
    M5Canvas *canvas,
    m5avatar::BoundingRect rect,
    m5avatar::DrawContext *ctx) {
  int centerX = rect.getCenterX();
  int centerY = rect.getCenterY();

  m5avatar::Gaze gaze =
      isLeft_ ? ctx->getLeftGaze() : ctx->getRightGaze();

  int x = centerX + static_cast<int>(gaze.getHorizontal() * 5.0f);
  int y = centerY + static_cast<int>(gaze.getVertical() * 3.0f);

  float openRatio =
      isLeft_ ? ctx->getLeftEyeOpenRatio()
              : ctx->getRightEyeOpenRatio();

  uint16_t primaryColor =
      ctx->getColorDepth() == 1
          ? 1
          : ctx->getColorPalette()->get(COLOR_PRIMARY);

  if (openRatio <= 0.10f) {
    canvas->fillRect(x - 13, y - 2, 26, 4, primaryColor);
    return;
  }

  int radius = isLeft_ ? 14 : 8;
  radius = static_cast<int>(radius * openRatio);

  if (radius < 3) {
    radius = 3;
  }

  canvas->fillCircle(x, y, radius, primaryColor);
}
