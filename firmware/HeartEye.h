#ifndef HEART_EYE_H_
#define HEART_EYE_H_

#include <BoundingRect.h>
#include <DrawContext.h>
#include <Drawable.h>
#include <M5GFX.h>

class HeartEye final : public m5avatar::Drawable {
 public:
  // true = 左眼，false = 右眼。
  // 这与 m5stack-avatar 内部 Eye 的约定保持一致。
  explicit HeartEye(bool isLeft);

  void draw(M5Canvas *canvas,
            m5avatar::BoundingRect rect,
            m5avatar::DrawContext *ctx) override;

 private:
  bool isLeft_;
};

#endif  // HEART_EYE_H_
