#include "CustomMouth.h"

CustomMouth::CustomMouth(FaceEffectState *effectState)
    : effectState_(effectState),
      normalMouth_(50, 90, 4, 60) {}

void CustomMouth::draw(M5Canvas *canvas,
                       m5avatar::BoundingRect rect,
                       m5avatar::DrawContext *ctx) {
  if (effectState_ == nullptr) {
    normalMouth_.draw(canvas, rect, ctx);
    return;
  }

  switch (effectState_->get()) {
    case FaceEffect::SurprisedFace:
      drawSurprisedMouth(canvas, rect, ctx);
      return;

    case FaceEffect::PoutFace:
      drawPoutMouth(canvas, rect, ctx);
      return;

    case FaceEffect::ShyFace:
      drawShyMouth(canvas, rect, ctx);
      return;

    case FaceEffect::SmugFace:
      drawSmugMouth(canvas, rect, ctx);
      return;

    case FaceEffect::ConfusedFace:
      drawConfusedMouth(canvas, rect, ctx);
      return;

    case FaceEffect::LaughFace:
      drawLaughMouth(canvas, rect, ctx);
      return;

    case FaceEffect::KissFace:
      drawKissMouth(canvas, rect, ctx);
      return;

    case FaceEffect::NervousFace:
      drawNervousMouth(canvas, rect, ctx);
      return;

    case FaceEffect::RelievedFace:
      drawRelievedMouth(canvas, rect, ctx);
      return;

    case FaceEffect::DeterminedFace:
      drawDeterminedMouth(canvas, rect, ctx);
      return;

    case FaceEffect::None:
    case FaceEffect::HeartEyes:
    case FaceEffect::SparkleEyes:
    case FaceEffect::DizzyEyes:
    case FaceEffect::TearEyes:
    default:
      normalMouth_.draw(canvas, rect, ctx);
      return;
  }
}

void CustomMouth::drawSurprisedMouth(
    M5Canvas *canvas,
    m5avatar::BoundingRect rect,
    m5avatar::DrawContext *ctx) {
  uint16_t primaryColor =
      ctx->getColorDepth() == 1
          ? 1
          : ctx->getColorPalette()->get(COLOR_PRIMARY);

  int x = rect.getLeft();
  int y = rect.getTop() + 2;

  canvas->fillCircle(x, y, 13, primaryColor);
}

void CustomMouth::drawPoutMouth(
    M5Canvas *canvas,
    m5avatar::BoundingRect rect,
    m5avatar::DrawContext *ctx) {
  uint16_t primaryColor =
      ctx->getColorDepth() == 1
          ? 1
          : ctx->getColorPalette()->get(COLOR_PRIMARY);

  int x = rect.getLeft();
  int y = rect.getTop();

  canvas->fillEllipse(x, y, 12, 6, primaryColor);
}

void CustomMouth::drawShyMouth(
    M5Canvas *canvas,
    m5avatar::BoundingRect rect,
    m5avatar::DrawContext *ctx) {
  uint16_t primaryColor =
      ctx->getColorDepth() == 1
          ? 1
          : ctx->getColorPalette()->get(COLOR_PRIMARY);

  int x = rect.getLeft();
  int y = rect.getTop();

  canvas->drawLine(x - 7, y, x, y + 3, primaryColor);
  canvas->drawLine(x, y + 3, x + 7, y, primaryColor);

  canvas->drawLine(x - 22, y + 2, x - 16, y + 4, primaryColor);
  canvas->drawLine(x + 16, y + 4, x + 22, y + 2, primaryColor);
}

void CustomMouth::drawSmugMouth(
    M5Canvas *canvas,
    m5avatar::BoundingRect rect,
    m5avatar::DrawContext *ctx) {
  uint16_t primaryColor =
      ctx->getColorDepth() == 1
          ? 1
          : ctx->getColorPalette()->get(COLOR_PRIMARY);

  int x = rect.getLeft();
  int y = rect.getTop();

  canvas->drawLine(x - 10, y + 2, x, y + 3, primaryColor);
  canvas->drawLine(x, y + 3, x + 11, y - 3, primaryColor);
}

void CustomMouth::drawConfusedMouth(
    M5Canvas *canvas,
    m5avatar::BoundingRect rect,
    m5avatar::DrawContext *ctx) {
  uint16_t primaryColor =
      ctx->getColorDepth() == 1
          ? 1
          : ctx->getColorPalette()->get(COLOR_PRIMARY);

  int x = rect.getLeft();
  int y = rect.getTop();

  canvas->drawLine(x - 9, y - 2, x, y + 3, primaryColor);
  canvas->drawLine(x, y + 3, x + 9, y + 1, primaryColor);
}

void CustomMouth::drawLaughMouth(
    M5Canvas *canvas,
    m5avatar::BoundingRect rect,
    m5avatar::DrawContext *ctx) {
  uint16_t primaryColor =
      ctx->getColorDepth() == 1
          ? 1
          : ctx->getColorPalette()->get(COLOR_PRIMARY);

  int x = rect.getLeft();
  int y = rect.getTop() + 3;

  // 实心张嘴大笑。
  canvas->fillEllipse(x, y, 24, 13, primaryColor);
}

void CustomMouth::drawKissMouth(
    M5Canvas *canvas,
    m5avatar::BoundingRect rect,
    m5avatar::DrawContext *ctx) {
  uint16_t primaryColor =
      ctx->getColorDepth() == 1
          ? 1
          : ctx->getColorPalette()->get(COLOR_PRIMARY);

  int x = rect.getLeft();
  int y = rect.getTop();

  // 小圆嘟嘴。
  canvas->fillEllipse(x, y, 8, 6, primaryColor);
}

void CustomMouth::drawNervousMouth(
    M5Canvas *canvas,
    m5avatar::BoundingRect rect,
    m5avatar::DrawContext *ctx) {
  uint16_t primaryColor =
      ctx->getColorDepth() == 1
          ? 1
          : ctx->getColorPalette()->get(COLOR_PRIMARY);

  int x = rect.getLeft();
  int y = rect.getTop();

  // 波浪形紧张嘴。
  canvas->drawLine(x - 14, y, x - 9, y - 3, primaryColor);
  canvas->drawLine(x - 9, y - 3, x - 4, y + 3, primaryColor);
  canvas->drawLine(x - 4, y + 3, x + 1, y - 3, primaryColor);
  canvas->drawLine(x + 1, y - 3, x + 6, y + 3, primaryColor);
  canvas->drawLine(x + 6, y + 3, x + 13, y, primaryColor);
}

void CustomMouth::drawRelievedMouth(
    M5Canvas *canvas,
    m5avatar::BoundingRect rect,
    m5avatar::DrawContext *ctx) {
  uint16_t primaryColor =
      ctx->getColorDepth() == 1
          ? 1
          : ctx->getColorPalette()->get(COLOR_PRIMARY);

  int x = rect.getLeft();
  int y = rect.getTop() + 2;

  // 放松、宽大的笑嘴。
  canvas->drawLine(x - 22, y, x - 14, y + 5, primaryColor);
  canvas->drawLine(x - 14, y + 5, x - 5, y + 7, primaryColor);
  canvas->drawLine(x - 5, y + 7, x + 5, y + 7, primaryColor);
  canvas->drawLine(x + 5, y + 7, x + 14, y + 5, primaryColor);
  canvas->drawLine(x + 14, y + 5, x + 22, y, primaryColor);
}

void CustomMouth::drawDeterminedMouth(
    M5Canvas *canvas,
    m5avatar::BoundingRect rect,
    m5avatar::DrawContext *ctx) {
  uint16_t primaryColor =
      ctx->getColorDepth() == 1
          ? 1
          : ctx->getColorPalette()->get(COLOR_PRIMARY);

  int x = rect.getLeft();
  int y = rect.getTop();

  // 坚定的平直嘴。
  canvas->fillRect(x - 18, y - 2, 36, 5, primaryColor);
}
