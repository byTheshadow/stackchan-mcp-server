#ifndef CUSTOM_MOUTH_H_
#define CUSTOM_MOUTH_H_

#include <DrawContext.h>
#include <Drawable.h>
#include <M5GFX.h>
#include <Mouth.h>

#include "CustomEye.h"

class CustomMouth final : public m5avatar::Drawable {
 public:
  explicit CustomMouth(FaceEffectState *effectState);

  void draw(M5Canvas *canvas,
            m5avatar::BoundingRect rect,
            m5avatar::DrawContext *ctx) override;

 private:
  void drawSurprisedMouth(M5Canvas *canvas,
                          m5avatar::BoundingRect rect,
                          m5avatar::DrawContext *ctx);

  void drawPoutMouth(M5Canvas *canvas,
                     m5avatar::BoundingRect rect,
                     m5avatar::DrawContext *ctx);

  void drawShyMouth(M5Canvas *canvas,
                    m5avatar::BoundingRect rect,
                    m5avatar::DrawContext *ctx);

  void drawSmugMouth(M5Canvas *canvas,
                     m5avatar::BoundingRect rect,
                     m5avatar::DrawContext *ctx);

  void drawConfusedMouth(M5Canvas *canvas,
                         m5avatar::BoundingRect rect,
                         m5avatar::DrawContext *ctx);

  void drawLaughMouth(M5Canvas *canvas,
                      m5avatar::BoundingRect rect,
                      m5avatar::DrawContext *ctx);

  void drawKissMouth(M5Canvas *canvas,
                     m5avatar::BoundingRect rect,
                     m5avatar::DrawContext *ctx);

  void drawNervousMouth(M5Canvas *canvas,
                        m5avatar::BoundingRect rect,
                        m5avatar::DrawContext *ctx);

  void drawRelievedMouth(M5Canvas *canvas,
                         m5avatar::BoundingRect rect,
                         m5avatar::DrawContext *ctx);

  void drawDeterminedMouth(M5Canvas *canvas,
                           m5avatar::BoundingRect rect,
                           m5avatar::DrawContext *ctx);

  FaceEffectState *effectState_;
  m5avatar::Mouth normalMouth_;
};

#endif  // CUSTOM_MOUTH_H_
