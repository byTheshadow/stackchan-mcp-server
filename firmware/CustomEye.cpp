#include "CustomEye.h"

FaceEffectState::FaceEffectState()
    : effect_(static_cast<uint8_t>(FaceEffect::None)) {}

void FaceEffectState::set(FaceEffect effect) {
  effect_ = static_cast<uint8_t>(effect);
}

FaceEffect FaceEffectState::get() const {
  return static_cast<FaceEffect>(effect_);
}

CustomEye::CustomEye(bool isLeft, FaceEffectState *effectState)
    : isLeft_(isLeft),
      effectState_(effectState),
      normalEye_(8, isLeft) {}

void CustomEye::draw(M5Canvas *canvas,
                     m5avatar::BoundingRect rect,
                     m5avatar::DrawContext *ctx) {
  /*
   * 默认、兼容性最重要的路径：
   * 完整使用 m5stack-avatar 原生 Eye 的逻辑。
   *
   * 因此 neutral / happy / sad / angry / doubt / sleepy
   * 都会恢复为之前已经实机验证过的原生样子。
   */
  if (
      effectState_ == nullptr ||
      effectState_->get() == FaceEffect::None
  ) {
    normalEye_.draw(canvas, rect, ctx);
    return;
  }

  switch (effectState_->get()) {
    case FaceEffect::HeartEyes:
      drawHeartEyes(canvas, rect, ctx);
      return;

    case FaceEffect::SparkleEyes:
      drawSparkleEyes(canvas, rect, ctx);
      return;

    case FaceEffect::DizzyEyes:
      drawDizzyEyes(canvas, rect, ctx);
      return;

    case FaceEffect::TearEyes:
      drawTearEyes(canvas, rect, ctx);
      return;

    case FaceEffect::SurprisedFace:
      drawSurprisedFace(canvas, rect, ctx);
      return;

    case FaceEffect::PoutFace:
      drawPoutFace(canvas, rect, ctx);
      return;

    case FaceEffect::ShyFace:
      drawShyFace(canvas, rect, ctx);
      return;

    case FaceEffect::SmugFace:
      drawSmugFace(canvas, rect, ctx);
      return;

    case FaceEffect::ConfusedFace:
      drawConfusedFace(canvas, rect, ctx);
      return;

    case FaceEffect::None:
    default:
      normalEye_.draw(canvas, rect, ctx);
      return;
  }



}

void CustomEye::drawHeartEyes(M5Canvas *canvas,
                              m5avatar::BoundingRect rect,
                              m5avatar::DrawContext *ctx) {
  /*
   * Face::draw() 已经在传入的 rect 中叠加了呼吸位移。
   */
  int centerX = rect.getCenterX();
  int centerY = rect.getCenterY();

  m5avatar::Gaze gaze =
      isLeft_ ? ctx->getLeftGaze() : ctx->getRightGaze();

  /*
   * 与 m5stack-avatar 新眼睛实现中的视线偏移尺度一致。
   */
  int x = centerX + static_cast<int>(gaze.getHorizontal() * 8.0f);
  int y = centerY + static_cast<int>(gaze.getVertical() * 5.0f);

  float openRatio =
      isLeft_ ? ctx->getLeftEyeOpenRatio() : ctx->getRightEyeOpenRatio();

  uint16_t primaryColor =
      ctx->getColorDepth() == 1
          ? 1
          : ctx->getColorPalette()->get(COLOR_PRIMARY);

  /*
   * 自动眨眼期间，把爱心临时绘制为闭眼横线。
   */
  if (openRatio <= 0.10f) {
    constexpr int kClosedEyeWidth = 28;
    constexpr int kClosedEyeHeight = 4;

    canvas->fillRect(
        x - kClosedEyeWidth / 2,
        y - kClosedEyeHeight / 2,
        kClosedEyeWidth,
        kClosedEyeHeight,
        primaryColor
    );
    return;
  }

  constexpr int kRadius = 13;

  /*
   * 眨眼过程中爱心下半部分随眼睛开合比例收缩。
   */
  int bottomHeight = static_cast<int>(kRadius * openRatio);
  if (bottomHeight < 4) {
    bottomHeight = 4;
  }

  constexpr int kLobeRadius = 7;
  constexpr int kLobeOffsetX = 6;
  constexpr int kLobeOffsetY = 4;

  // 心形顶部的两个圆瓣。
  canvas->fillCircle(
      x - kLobeOffsetX,
      y - kLobeOffsetY,
      kLobeRadius,
      primaryColor
  );

  canvas->fillCircle(
      x + kLobeOffsetX,
      y - kLobeOffsetY,
      kLobeRadius,
      primaryColor
  );

  // 心形下半部。
  canvas->fillTriangle(
      x - kRadius,
      y - 1,
      x + kRadius,
      y - 1,
      x,
      y + bottomHeight,
      primaryColor
  );
}


void CustomEye::drawSparkleEyes(M5Canvas *canvas,
                                m5avatar::BoundingRect rect,
                                m5avatar::DrawContext *ctx) {
  int centerX = rect.getCenterX();
  int centerY = rect.getCenterY();

  m5avatar::Gaze gaze =
      isLeft_ ? ctx->getLeftGaze() : ctx->getRightGaze();

  int x = centerX + static_cast<int>(gaze.getHorizontal() * 8.0f);
  int y = centerY + static_cast<int>(gaze.getVertical() * 5.0f);

  float openRatio =
      isLeft_ ? ctx->getLeftEyeOpenRatio() : ctx->getRightEyeOpenRatio();

  uint16_t primaryColor =
      ctx->getColorDepth() == 1
          ? 1
          : ctx->getColorPalette()->get(COLOR_PRIMARY);

  // 闭眼时统一画横线，保留自动眨眼。
  if (openRatio <= 0.10f) {
    canvas->fillRect(x - 14, y - 2, 28, 4, primaryColor);
    return;
  }

  /*
   * 两个填充三角形组成四角闪光：
   *
   *        ▲
   *      ◀ ◆ ▶
   *        ▼
   */
  constexpr int kHalfWidth = 13;
  constexpr int kHalfHeight = 16;
  constexpr int kMiddleHeight = 7;

  canvas->fillTriangle(
      x,
      y - kHalfHeight,
      x - kHalfWidth,
      y + kMiddleHeight,
      x + kHalfWidth,
      y + kMiddleHeight,
      primaryColor
  );

  canvas->fillTriangle(
      x,
      y + kHalfHeight,
      x - kHalfWidth,
      y - kMiddleHeight,
      x + kHalfWidth,
      y - kMiddleHeight,
      primaryColor
  );
}
void CustomEye::drawDizzyEyes(M5Canvas *canvas,
                              m5avatar::BoundingRect rect,
                              m5avatar::DrawContext *ctx) {
  int centerX = rect.getCenterX();
  int centerY = rect.getCenterY();

  m5avatar::Gaze gaze =
      isLeft_ ? ctx->getLeftGaze() : ctx->getRightGaze();

  int x = centerX + static_cast<int>(gaze.getHorizontal() * 5.0f);
  int y = centerY + static_cast<int>(gaze.getVertical() * 3.0f);

  float openRatio =
      isLeft_ ? ctx->getLeftEyeOpenRatio() : ctx->getRightEyeOpenRatio();

  uint16_t primaryColor =
      ctx->getColorDepth() == 1
          ? 1
          : ctx->getColorPalette()->get(COLOR_PRIMARY);

  uint16_t backgroundColor =
      ctx->getColorDepth() == 1
          ? 0
          : ctx->getColorPalette()->get(COLOR_BACKGROUND);

  if (openRatio <= 0.10f) {
    canvas->fillRect(x - 14, y - 2, 28, 4, primaryColor);
    return;
  }

  /*
   * 三层同心圆：
   *
   *   █████████
   *   ██     ██
   *   ██ ███ ██
   *   ██ ███ ██
   *   ██     ██
   *   █████████
   *
   * 视觉上接近卡通式的“头晕 / 转圈眼”。
   */
  constexpr int kOuterRadius = 14;
  constexpr int kMiddleRadius = 9;
  constexpr int kInnerRadius = 4;

  canvas->fillCircle(x, y, kOuterRadius, primaryColor);
  canvas->fillCircle(x, y, kMiddleRadius, backgroundColor);
  canvas->fillCircle(x, y, kInnerRadius, primaryColor);
}
void CustomEye::drawTearEyes(M5Canvas *canvas,
                             m5avatar::BoundingRect rect,
                             m5avatar::DrawContext *ctx) {
  /*
   * 先画原生眼睛。
   *
   * 因此 tear_eyes 不是完全覆盖，而是“原生 Expression + 泪滴”。
   * 尤其适合 expression: "sad"。
   */
  normalEye_.draw(canvas, rect, ctx);

  float openRatio =
      isLeft_ ? ctx->getLeftEyeOpenRatio() : ctx->getRightEyeOpenRatio();

  // 眼睛闭合时不单独画泪滴，避免泪滴漂在闭眼横线下显得突兀。
  if (openRatio <= 0.10f) {
    return;
  }

  uint16_t primaryColor =
      ctx->getColorDepth() == 1
          ? 1
          : ctx->getColorPalette()->get(COLOR_PRIMARY);

  int centerX = rect.getCenterX();
  int centerY = rect.getCenterY();

  m5avatar::Gaze gaze =
      isLeft_ ? ctx->getLeftGaze() : ctx->getRightGaze();

  int gazeX = static_cast<int>(gaze.getHorizontal() * 8.0f);

  /*
   * 左右眼的泪滴都画在脸中央一侧：
   *
   * 左眼：右下角
   * 右眼：左下角
   *
   * 这样两个泪滴不会画到脸部最外缘。
   */
  int tearX = isLeft_
      ? centerX + gazeX + 10
      : centerX + gazeX - 10;

  int tearY = centerY + 18;

  // 泪滴上尖下圆：一个小三角形 + 一个小圆。
  canvas->fillTriangle(
      tearX,
      tearY - 6,
      tearX - 5,
      tearY + 2,
      tearX + 5,
      tearY + 2,
      primaryColor
  );

  canvas->fillCircle(
      tearX,
      tearY + 3,
      5,
      primaryColor
  );
}
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

  // 大圆眼。
  canvas->fillCircle(x, y, radius, primaryColor);

  // 左上背景色圆形高光。
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
  /*
   * pout_face 的重点是 CustomMouth 的嘟嘴。
   * 眼睛保持库原生逻辑，因此仍支持六种基础 Expression。
   */
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

  // 比原生眼小的圆点眼。
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

  /*
   * 左眼睁开、右眼半眯。
   *
   * Face 的顺序和库中 Eye 的约定：
   * isLeft_ == true  是左眼；
   * isLeft_ == false 是右眼。
   */
  if (isLeft_) {
    if (openRatio <= 0.10f) {
      canvas->fillRect(x - 14, y - 2, 28, 4, primaryColor);
      return;
    }

    canvas->fillCircle(x, y, 12, primaryColor);
  } else {
    // 右侧眯眼的位置略低，形成得意感。
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

  // 左大右小，形成明显的困惑感。
  int radius = isLeft_ ? 14 : 8;
  radius = static_cast<int>(radius * openRatio);

  if (radius < 3) {
    radius = 3;
  }

  canvas->fillCircle(x, y, radius, primaryColor);
}
