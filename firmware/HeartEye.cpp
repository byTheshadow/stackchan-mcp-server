#include "HeartEye.h"

HeartEye::HeartEye(bool isLeft) : isLeft_(isLeft) {}

void HeartEye::draw(M5Canvas *canvas,
                    m5avatar::BoundingRect rect,
                    m5avatar::DrawContext *ctx) {
  // Face::draw() 已经将呼吸位移加入 rect。
  int centerX = rect.getCenterX();
  int centerY = rect.getCenterY();

  // 与库内新版 BaseEye 的视线偏移尺度一致。
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


  // 眨眼：爱心缩成短横线。
  if (openRatio <= 0.10f) {
    constexpr int kClosedEyeWidth = 28;
    constexpr int kClosedEyeHeight = 4;

    canvas->fillRect(
        x - kClosedEyeWidth / 2,
        y - kClosedEyeHeight / 2,
        kClosedEyeWidth,
        kClosedEyeHeight,
        primaryColor);
    return;
  }

  // 基础尺寸。Face 默认眼部位置之间相距约 140 px，
  // 因此此尺寸不会让左右爱心相互接触。
  constexpr int kRadius = 13;

  // 让眨眼过程中的爱心高度平滑缩小。
  int verticalScale = static_cast<int>(kRadius * openRatio);
  if (verticalScale < 4) {
    verticalScale = 4;
  }

  // 爱心上方两个圆瓣。
  int lobeRadius = kRadius / 2 + 1;
  int lobeOffsetX = kRadius / 2;
  int lobeOffsetY = kRadius / 3;

  canvas->fillCircle(
      x - lobeOffsetX,
      y - lobeOffsetY,
      lobeRadius,
      primaryColor);

  canvas->fillCircle(
      x + lobeOffsetX,
      y - lobeOffsetY,
      lobeRadius,
      primaryColor);

  // 爱心下半部：两个圆瓣与一个向下的实心三角形拼合。
  canvas->fillTriangle(
      x - kRadius,
      y - 1,
      x + kRadius,
      y - 1,
      x,
      y + verticalScale,
      primaryColor);
}
