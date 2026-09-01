#ifndef CUSTOM_EYEBLOW_H_
#define CUSTOM_EYEBLOW_H_

#include <DrawContext.h>
#include <Drawable.h>
#include <Eyeblow.h>
#include <M5GFX.h>

#include "CustomEye.h"

class CustomEyeblow final : public m5avatar::Drawable {
 public:
  CustomEyeblow(bool isLeft, FaceEffectState *effectState);

  void draw(M5Canvas *canvas,
            m5avatar::BoundingRect rect,
            m5avatar::DrawContext *ctx) override;

 private:
  void drawNervousEyeblow(M5Canvas *canvas,
                          m5avatar::BoundingRect rect,
                          m5avatar::DrawContext *ctx);

  void drawDeterminedEyeblow(M5Canvas *canvas,
                             m5avatar::BoundingRect rect,
                             m5avatar::DrawContext *ctx);

  bool isLeft_;
  FaceEffectState *effectState_;
  m5avatar::Eyeblow normalEyeblow_;
};

#endif  // CUSTOM_EYEBLOW_H_
