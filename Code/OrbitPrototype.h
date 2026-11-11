#ifndef SEED146_ORBIT_PROTOTYPE_H
#define SEED146_ORBIT_PROTOTYPE_H

#include <Arduino.h>
#include <LovyanGFX.h>

// ORBIT draws a small deterministic orbital system, then reflects that system
// through a moving kaleidoscope. Input and LED hardware stay in the main sketch.
class OrbitPrototype {
 public:
  static constexpr uint8_t kSectorCount = 8;

  explicit OrbitPrototype(lgfx::LovyanGFX* targetDisplay);

  bool begin(uint32_t seed);
  bool regenerate(uint32_t seed);
  void showCurrentFrame();
  void update(uint32_t nowMs);

  // 0.0 keeps soft ellipses; 1.0 adds elongated paths and orbital transfers.
  void setEccentricity(float normalizedValue);

  bool isReady() const { return ready_; }
  uint32_t seed() const { return seed_; }
  uint8_t attractorCount() const { return attractorCount_; }
  uint8_t symmetryOrder() const { return symmetryOrder_; }
  float sectorEnergy(uint8_t sectorIndex) const;
  float kaleidoscopeCenterX() const { return reportedCenterX_; }
  float kaleidoscopeCenterY() const { return reportedCenterY_; }
  uint32_t renderedFrameCount() const { return renderedFrameCount_; }
  float measuredFramesPerSecond() const { return measuredFramesPerSecond_; }
  uint32_t slowestFrameTimeUs() const { return slowestFrameTimeUs_; }
  size_t canvasSizeBytes() const {
    return (kCanvasWidth * kCanvasHeight * kCanvasBitsPerPixel + 7U) / 8U;
  }

 private:
  struct Attractor {
    float orbitRadius;
    float orbitFlattening;
    float phase;
    float angularSpeed;
  };

  struct Body {
    uint8_t attractorIndex;
    float radius;
    float phase;
    float angularSpeed;
    float ellipseAngle;
    float precessionSpeed;
    float eccentricityScale;
    float transferPhase;
    float transferSpeed;
    int8_t direction;
    uint8_t grayLevel;
    bool brightHead;
  };

  static constexpr uint16_t kCanvasWidth = 360;
  static constexpr uint16_t kCanvasHeight = 360;
  static constexpr uint8_t kCanvasBitsPerPixel = 4;
  static constexpr float kScreenCenter = 180.0f;
  static constexpr float kVisibleRadius = 177.0f;
  static constexpr uint32_t kTargetFramePeriodMs = 33;
  static constexpr uint8_t kBodyCount = 42;
  static constexpr uint8_t kMaximumAttractorCount = 4;
  static constexpr uint8_t kMaximumSymmetryOrder = 5;
  static constexpr uint8_t kMaximumCopyCount = kMaximumSymmetryOrder * 2;
  // A bounded history in model coordinates survives movement off screen.
  static constexpr uint8_t kTrailSampleCount = 40;
  static constexpr float kTrailSamplePeriodSeconds = 0.08f;
  static constexpr float kCoreSpeedPixelsPerSecond = 80.0f;
  static constexpr float kCoreBoundaryRadius = 160.0f;
  // Light follows distance from the contacted border. At the shallowest
  // allowed rebound angle, 80 px is reached just before the next collision.
  static constexpr float kReboundLedFadeDistancePixels = 80.0f;
  static constexpr float kEccentricityResponsePerSecond = 0.45f;
  // ORBIT must feel larger than the physical viewport at every zoom phase.
  // Scaling model space here preserves all orbital timing and seed structure.
  static constexpr float kImmersiveModelScale = 2.2f;
  static constexpr float kZoomPeriodSeconds = 18.0f;
  // The whole kaleidoscope spins around its moving core. Each rebound maps its
  // outgoing tangential velocity to a 12-24 second turn and may reverse it.
  static constexpr float kSlowestSpinRadiansPerSecond = 0.2617994f;
  static constexpr float kFastestSpinRadiansPerSecond = 0.5235988f;
  static constexpr float kSpinResponseRadiansPerSecondSquared = 1.2f;

  lgfx::LovyanGFX* targetDisplay_;
  lgfx::LGFX_Sprite canvas_;
  Attractor attractors_[kMaximumAttractorCount] = {};
  Body bodies_[kBodyCount] = {};
  struct ModelPoint { float x; float y; };
  ModelPoint trail_[kBodyCount][kTrailSampleCount] = {};
  float trailTimes_[kTrailSampleCount] = {};
  uint8_t trailNextSlot_ = 0;
  uint8_t trailCount_ = 0;
  float lastTrailSampleTime_ = -kTrailSamplePeriodSeconds;
  float coreX_ = kScreenCenter;
  float coreY_ = kScreenCenter;
  float coreVelocityX_ = 0.0f;
  float coreVelocityY_ = 0.0f;
  // One rebound illuminates three LEDs centered on the contacted quarter.
  // Their envelope is derived from distance to that border, not elapsed time.
  float sectorEnergy_[kSectorCount] = {};
  float reboundBorderNormalX_ = 0.0f;
  float reboundBorderNormalY_ = 0.0f;
  uint8_t reboundQuarterIndex_ = 0;
  bool reboundLedEnvelopeActive_ = false;

  bool ready_ = false;
  uint32_t seed_ = 0;
  uint32_t randomState_ = 1;
  uint8_t attractorCount_ = 3;
  uint8_t symmetryOrder_ = 3;
  float targetEccentricity_ = 0.5f;
  float currentEccentricity_ = 0.5f;
  float animationTimeSeconds_ = 0.0f;
  float globalRotationAngle_ = 0.0f;
  float globalAngularVelocity_ = kSlowestSpinRadiansPerSecond;
  float targetGlobalAngularVelocity_ = kSlowestSpinRadiansPerSecond;
  float zoomPhase_ = 0.0f;
  float facetCosine_[kMaximumSymmetryOrder] = {};
  float facetSine_[kMaximumSymmetryOrder] = {};
  // Each reflected copy uses one cached 2x2 projection matrix per frame.
  // This replaces repeated kaleidoscope/zoom/rotation multiplication for
  // every point in every trail without changing the resulting coordinates.
  float frameTransformXX_[kMaximumCopyCount] = {};
  float frameTransformXY_[kMaximumCopyCount] = {};
  float frameTransformYX_[kMaximumCopyCount] = {};
  float frameTransformYY_[kMaximumCopyCount] = {};
  // Trail age is shared by every body and reflected copy. Cache its quadratic
  // fade once per history slot instead of recalculating it thousands of times.
  float frameTrailBrightness_[kTrailSampleCount] = {};
  float frameZoom_ = 1.0f;
  float frameGlobalCosine_ = 1.0f;
  float frameGlobalSine_ = 0.0f;
  float reportedCenterX_ = kScreenCenter;
  float reportedCenterY_ = kScreenCenter;

  uint32_t lastFrameMs_ = 0;
  uint32_t renderedFrameCount_ = 0;
  uint32_t slowestFrameTimeUs_ = 0;
  uint32_t framesInMeasurementWindow_ = 0;
  uint32_t measurementWindowStartedMs_ = 0;
  float measuredFramesPerSecond_ = 0.0f;

  float nextRandomUnitFloat();
  void initializeSeededComposition(uint32_t seed);
  void updateCore(float elapsedSeconds);
  void updateGlobalRotation(float elapsedSeconds);
  void updateReboundLedEnvelope();
  void drawTrail(uint8_t bodyIndex, float modelX, float modelY);
  void drawOrbitalSystem();
  void calculateAttractorPosition(uint8_t index, float& x, float& y) const;
  void updateFrameProjectionCache();
  void transformModelPoint(float modelX,
                           float modelY,
                           uint8_t copyIndex,
                           float& screenX,
                           float& screenY) const;
  void updateFrameRateMeasurement(uint32_t nowMs);
};

#endif  // SEED146_ORBIT_PROTOTYPE_H
