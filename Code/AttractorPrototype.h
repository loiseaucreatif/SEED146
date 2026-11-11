#ifndef SEED146_ATTRACTOR_PROTOTYPE_H
#define SEED146_ATTRACTOR_PROTOTYPE_H

#include <Arduino.h>
#include <LovyanGFX.h>

/**
 * First ATTRACTOR artwork experiment.
 *
 * Peter de Jong's equations repeatedly transform one (x, y) point into the
 * next. Their sine and cosine outputs stay bounded, which makes this strange
 * attractor well suited to a deterministic, full-screen embedded renderer.
 * This class owns no input or LED hardware.
 */
class AttractorPrototype {
 public:
  static constexpr uint8_t kSectorCount = 8;

  explicit AttractorPrototype(lgfx::LovyanGFX* targetDisplay);

  // Allocates one RGB565 canvas in PSRAM. Later seed changes reuse it.
  bool begin(uint32_t seed);
  bool regenerate(uint32_t seed);
  void showCurrentFrame();
  void update(uint32_t nowMs);

  // CHAOS continuously moves between related coefficient sets. The normalized
  // midpoint is the seed's reference composition.
  void setChaos(float normalizedValue);
  float chaos() const;

  // Sector 0 begins at screen-right; indices advance clockwise.
  float sectorEnergy(uint8_t sectorIndex) const;
  // A rare bright tracer entering the outer screen ring creates an impulse in
  // its matching angular sector. Values decay continuously from 1.0 to black.
  float edgeSparkle(uint8_t sectorIndex) const;

  bool isReady() const;
  uint32_t seed() const;
  const char* familyName() const;
  uint32_t renderedFrameCount() const;
  float measuredFramesPerSecond() const;
  uint32_t slowestFrameTimeUs() const;
  size_t canvasSizeBytes() const;

 private:
  struct Coefficients {
    float a;
    float b;
    float c;
    float d;
  };

  // A fixed set of points forms a genuine 3D depth field. Model X/Y comes
  // from the attractor; depth advances toward the viewer independently.
  struct DepthPoint {
    float modelX;
    float modelY;
    float modelZ;
    float depth;
  };

  static constexpr uint16_t kCanvasWidth = 360;
  static constexpr uint16_t kCanvasHeight = 360;
  static constexpr float kCanvasCenter = 180.0f;
  static constexpr uint32_t kTargetFramePeriodMs = 33;
  static constexpr uint16_t kDepthPointCount = 900;
  static constexpr uint16_t kWarmupIterations = 96;
  static constexpr uint8_t kFadePhaseCount = 8;
  static constexpr uint16_t kFadeRowsPerFrame =
      kCanvasHeight / kFadePhaseCount;
  static constexpr uint8_t kFadeMultiplier = 234;
  static constexpr uint16_t kFadeRowPermutationMultiplier = 73;
  static constexpr float kChaosResponsePerSecond = 1.2f;
  // One coefficient-morph cycle lasts 18 seconds. Unlike FLOW and RADIAL,
  // ATTRACTOR does not rotate as one circular composition: its equation itself
  // changes, so folds appear, separate, and reconnect in place.
  static constexpr float kMorphRadiansPerSecond = 0.349065850f;
  // Every point travels from the far plane to the viewer in about 5.5 seconds.
  // Evenly distributed starting depths make recycling continuous and local.
  static constexpr float kForwardDepthPerSecond = 0.18f;
  // A separate global turn makes one revolution in three minutes. It is slow
  // enough to remain a background orientation change rather than a vortex.
  static constexpr float kGlobalRotationRadiansPerSecond = 0.034906585f;
  static constexpr float kEdgeSparkleDecayPerSecond = 1.8f;
  static constexpr float kEdgeDetectionRadius = 169.0f;
  static constexpr float kEdgeDetectionOuterRadius = 183.0f;

  lgfx::LovyanGFX* targetDisplay_;
  lgfx::LGFX_Sprite canvas_;

  bool ready_ = false;
  uint32_t seed_ = 0;
  uint32_t randomState_ = 1;
  uint8_t familyIndex_ = 0;
  Coefficients baseCoefficients_ = {};
  Coefficients coefficientDirection_ = {};
  Coefficients coefficientBreathing_ = {};
  float currentChaos_ = 0.5f;
  float targetChaos_ = 0.5f;
  float orbitX_ = 0.1f;
  float orbitY_ = 0.1f;
  float scaleXPixelsPerUnit_ = 128.0f;
  float scaleYPixelsPerUnit_ = 96.0f;
  float shearXFromY_ = 0.0f;
  float viewportCenterX_ = kCanvasCenter;
  float viewportCenterY_ = kCanvasCenter;
  float fixedOrientationRadians_ = 0.0f;
  float morphPhaseRadians_ = 0.0f;
  float sourceCenterX_ = 0.0f;
  float sourceCenterY_ = 0.0f;
  float sourceNormalizationX_ = 1.0f;
  float sourceNormalizationY_ = 1.0f;
  float modelAnchorX_ = 0.0f;
  float modelAnchorY_ = 0.0f;
  float modelPostScaleX_ = 1.0f;
  float modelPostScaleY_ = 1.0f;
  float modelZAtOrigin_ = 0.0f;
  float modelDepthPhase_ = 0.0f;
  float rotationElapsedSeconds_ = 0.0f;
  float globalRotationRadians_ = 0.0f;
  float rotationAmplitudeX_ = 0.0f;
  float rotationAmplitudeY_ = 0.0f;
  float rotationAmplitudeZ_ = 0.0f;
  float rotationSpeedX_ = 0.0f;
  float rotationSpeedY_ = 0.0f;
  float rotationSpeedZ_ = 0.0f;
  float rotationPhaseX_ = 0.0f;
  float rotationPhaseY_ = 0.0f;
  float rotationPhaseZ_ = 0.0f;
  DepthPoint depthPoints_[kDepthPointCount] = {};
  bool pointWasNearScreenEdge_[kDepthPointCount] = {};
  float sectorEnergy_[kSectorCount] = {};
  float edgeSparkle_[kSectorCount] = {};
  uint8_t fadePhase_ = 0;
  uint16_t fadedRows_[kFadeRowsPerFrame] = {};

  uint32_t lastFrameMs_ = 0;
  uint32_t renderedFrameCount_ = 0;
  uint32_t slowestFrameTimeUs_ = 0;
  uint32_t framesInMeasurementWindow_ = 0;
  uint32_t measurementWindowStartedMs_ = 0;
  float measuredFramesPerSecond_ = 0.0f;

  uint32_t nextRandomNumber();
  float nextRandomUnitFloat();
  void initializeSeededComposition(uint32_t seed);
  Coefficients currentCoefficients() const;
  void resetOrbit();
  void advanceOrbit(const Coefficients& coefficients);
  void initializeDepthPoints(const Coefficients& coefficients);
  void normalizeDepthPointModelCoordinates();
  void assignRecycledPointModelPosition(DepthPoint& point,
                                        const Coefficients& coefficients);
  void drawDepthPoints(float elapsedSeconds,
                       bool collectSectorEnergy,
                       bool drawDirectlyToDisplay);
  void fadePartOfCanvas();
  void pushFadedRowsToDisplay();
  void updateFrameRateMeasurement(uint32_t nowMs);
};

#endif  // SEED146_ATTRACTOR_PROTOTYPE_H
