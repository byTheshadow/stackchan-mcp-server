#include "CustomEyeblow.h"

CustomEyeblow::CustomEyeblow(bool isLeft,
                             FaceEffectState *effectState)
    : isLeft_(isLeft),
      effectState_(effectState),
      normalEyeblow_(32, 0, isLeft) {}

void CustomEyeblow::draw(M5Canvas *canvas,
                         m5avatar::BoundingRect rect,
                         m5avatar::DrawContext *ctx) {
  FaceEffect effect =
      effectState_ == nullptr
          ? FaceEffect::None
          : effectState_->get();

  uint16_t primaryColor =
      ctx->getColorDepth() == 1
          ? 1
          : ctx->getColorPalette()->get(COLOR_PRIMARY);

  int x = rect.getLeft();
  int y = rect.getTop();

  if (effect == FaceEffect::SmugFace) {
    // 右眉抬高，左眉保持较低。
    int eyebrowY = y - 2;

    if (!isLeft_) {
      eyebrowY -= 9;
    }

    canvas->fillRect(x - 16, eyebrowY - 2, 32, 4, primaryColor);
    return;
  }

  if (effect == FaceEffect::ConfusedFace) {
    // 右眉明显抬高，左眉略微下压。
    int eyebrowY = y - 2;

    if (!isLeft_) {
      eyebrowY -= 8;
    } else {
      eyebrowY += 3;
    }

    canvas->fillRect(x - 16, eyebrowY - 2, 32, 4, primaryColor);
    return;
  }

  if (effect == FaceEffect::NervousFace) {
    drawNervousEyeblow(canvas, rect, ctx);
    return;
  }

  if (effect == FaceEffect::DeterminedFace) {
    drawDeterminedEyeblow(canvas, rect, ctx);
    return;
  }

  /*
   * none 和其他效果继续走原生 Eyeblow。
   * 当前 normalEyeblow_ 的高度是 0，因此一般不会实际绘制眉毛，
   * 与原先默认 Face 的行为一致。
   */
  normalEyeblow_.draw(canvas, rect, ctx);
}

void CustomEyeblow::drawNervousEyeblow(
    M5Canvas *canvas,
    m5avatar::BoundingRect rect,
    m5avatar::DrawContext *ctx) {
  uint16_t primaryColor =
      ctx->getColorDepth() == 1
          ? 1
          : ctx->getColorPalette()->get(COLOR_PRIMARY);

  int x = rect.getLeft();
  int y = rect.getTop();

  // 紧张：右眉稍高、左眉稍低。
  int eyebrowY = y - 2;

  if (!isLeft_) {
    eyebrowY -= 5;
  } else {
    eyebrowY += 2;
  }

  canvas->fillRect(x - 14, eyebrowY - 2, 28, 4, primaryColor);
}

void CustomEyeblow::drawDeterminedEyeblow(
    M5Canvas *canvas,
    m5avatar::BoundingRect rect,
    m5avatar::DrawContext *ctx) {
  uint16_t primaryColor =
      ctx->getColorDepth() == 1
          ? 1
          : ctx->getColorPalette()->get(COLOR_PRIMARY);

  int x = rect.getLeft();
  int y = rect.getTop();

  /*
   * 眉毛向脸部中央压低：
   * 左眉右端更低，右眉左端更低。
   */
  int innerOffset = isLeft_ ? -3 : 3;

  canvas->drawLine(
      x - 15,
      y - 2 - innerOffset,
      x + 15,
      y - 5 + innerOffset,
      primaryColor);

  canvas->drawLine(
      x - 15,
      y - 1 - innerOffset,
      x + 15,
      y - 4 + innerOffset,
      primaryColor);
}
