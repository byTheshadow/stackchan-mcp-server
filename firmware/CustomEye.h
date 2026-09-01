#ifndef CUSTOM_EYE_H_
#define CUSTOM_EYE_H_

#include <BoundingRect.h>
#include <DrawContext.h>
#include <Drawable.h>
#include <Eye.h>
#include <M5GFX.h>

enum class FaceEffect : uint8_t {
  None = 0,

  // 已验证的眼睛效果
  HeartEyes,
  SparkleEyes,
  DizzyEyes,
  TearEyes,

  // 新增完整脸部效果
  SurprisedFace,
  PoutFace,
  ShyFace,
  SmugFace,
  ConfusedFace
};

/*
 * 两只眼睛、嘴巴和眉毛共用的 face_effect 状态。
 *
 * WebSocket 指令调用 set()；
 * Avatar 绘制任务读取 get()。
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
 * FaceEffect::None：
 *   调用 m5stack-avatar 原生 Eye，因此六种内置 Expression 完整保留。
 *
 * 其他 FaceEffect：
 *   使用自定义绘制。
 */
class CustomEye final : public m5avatar::Drawable {
 public:
  CustomEye(bool isLeft, FaceEffectState *effectState);

  void draw(M5Canvas *canvas,
            m5avatar::BoundingRect rect,
            m5avatar::DrawContext *ctx) override;

 private:
  // 已有自定义眼睛效果
  void drawHeartEyes(M5Canvas *canvas,
                     m5avatar::BoundingRect rect,
                     m5avatar::DrawContext *ctx);

  void drawSparkleEyes(M5Canvas *canvas,
                       m5avatar::BoundingRect rect,
                       m5avatar::DrawContext *ctx);

  void drawDizzyEyes(M5Canvas *canvas,
                     m5avatar::BoundingRect rect,
                     m5avatar::DrawContext *ctx);

  void drawTearEyes(M5Canvas *canvas,
                    m5avatar::BoundingRect rect,
                    m5avatar::DrawContext *ctx);

  // 新增完整脸部效果的眼睛部分
  void drawSurprisedFace(M5Canvas *canvas,
                         m5avatar::BoundingRect rect,
                         m5avatar::DrawContext *ctx);

  void drawPoutFace(M5Canvas *canvas,
                    m5avatar::BoundingRect rect,
                    m5avatar::DrawContext *ctx);

  void drawShyFace(M5Canvas *canvas,
                   m5avatar::BoundingRect rect,
                   m5avatar::DrawContext *ctx);

  void drawSmugFace(M5Canvas *canvas,
                    m5avatar::BoundingRect rect,
                    m5avatar::DrawContext *ctx);

  void drawConfusedFace(M5Canvas *canvas,
                        m5avatar::BoundingRect rect,
                        m5avatar::DrawContext *ctx);

  bool isLeft_;
  FaceEffectState *effectState_;

  // FaceEffect::None 时，完全委托给原生眼睛。
  m5avatar::Eye normalEye_;
};

#endif  // CUSTOM_EYE_H_
