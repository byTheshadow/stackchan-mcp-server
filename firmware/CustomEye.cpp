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

  if (effectState_->get() == FaceEffect::HeartEyes) {
    drawHeartEyes(canvas, rect, ctx);
    return;
  }

  // 未知效果的安全回退：保持原生眼睛。
  normalEye_.draw(canvas, rect, ctx);
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
