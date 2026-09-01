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
    /*
     * 右眉抬高，左眉保持较低。
     * isLeft_ == false 表示右眉。
     */
    int eyebrowY = y - 2;

    if (!isLeft_) {
      eyebrowY -= 9;
    }

    canvas->fillRect(x - 16, eyebrowY - 2, 32, 4, primaryColor);
    return;
  }

  if (effect == FaceEffect::ConfusedFace) {
    /*
     * 右眉明显抬高，左眉略微下压。
     */
    int eyebrowY = y - 2;

    if (!isLeft_) {
      eyebrowY -= 8;
    } else {
      eyebrowY += 3;
    }

    canvas->fillRect(x - 16, eyebrowY - 2, 32, 4, primaryColor);
    return;
  }

  /*
   * none 和其他效果继续走原生 Eyeblow。
   * 由于原始构造器高度为 0，这里通常不会实际绘制眉毛，
   * 与当前默认 Face 完全一致。
   */
  normalEyeblow_.draw(canvas, rect, ctx);
}
