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
  HeartEyes
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

  bool isLeft_;
  FaceEffectState *effectState_;

  /*
   * 原生 Eye 保留在这里。
   * FaceEffect::None 时直接调用它的 draw()。
   */
  m5avatar::Eye normalEye_;
};

#endif  // CUSTOM_EYE_H_
