#ifndef SEED146_RADIAL_PROTOTYPE_H
#define SEED146_RADIAL_PROTOTYPE_H

#include <Arduino.h>
#include <LovyanGFX.h>

/**
 * First RADIAL artwork experiment.
 *
 * The renderer builds a polar lattice from spiral arms and oscillating rings.
 * It owns no input or LED hardware: the sketch translates physical actions
 * into normalized parameter values and reads back eight angular energy values.
 */
class RadialPrototype {
 public:
  static constexpr uint8_t kSectorCount = 8;

  explicit RadialPrototype(lgfx::LovyanGFX* targetDisplay);

  // Allocates one RGB565 screen canvas in PSRAM. Repeated seed changes reuse it.
  bool begin(uint32_t seed);
  bool regenerate(uint32_t seed);
  void showCurrentFrame();
  void update(uint32_t nowMs);

  // 0.0 produces almost radial arms; 1.0 curls them into strong spirals.
  void setTwist(float normalizedValue);
  float twist() const;

  // Sector 0 begins at screen-right; indices advance clockwise.
  float sectorEnergy(uint8_t sectorIndex) const;

  bool isReady() const;
  uint32_t seed() const;
  uint8_t armCount() const;
  uint8_t symmetry() const;
  uint32_t renderedFrameCount() const;
  float measuredFramesPerSecond() const;
  uint32_t slowestFrameTimeUs() const;
  size_t canvasSizeBytes() const;

 private:
  static constexpr uint16_t kCanvasWidth = 360;
  static constexpr uint16_t kCanvasHeight = 360;
  static constexpr float kCanvasCenter = 180.0f;
  static constexpr float kOuterRadius = 178.0f;
  static constexpr uint32_t kTargetFramePeriodMs = 33;
  static constexpr uint8_t kFadePhaseCount = 8;
  static constexpr uint16_t kFadeRowsPerFrame =
      kCanvasHeight / kFadePhaseCount;
  static constexpr uint8_t kFadeMultiplier = 238;
  static constexpr uint16_t kFadeRowPermutationMultiplier = 73;
  static constexpr float kTwistResponsePerSecond = 1.5f;

  lgfx::LovyanGFX* targetDisplay_;
  lgfx::LGFX_Sprite canvas_;

  bool ready_ = false;
  uint32_t seed_ = 0;
  uint32_t randomState_ = 1;
  uint8_t armCount_ = 8;
  uint8_t symmetry_ = 5;
  float armWaveAmplitudeRadians_ = 0.12f;
  float armWaveFrequency_ = 2.0f;
  float ringWaveAmplitudePixels_ = 6.0f;
  float rotationDirection_ = 1.0f;
  float phaseOffset_ = 0.0f;
  float currentTwist_ = 0.5f;
  float targetTwist_ = 0.5f;
  float rotationRadians_ = 0.0f;
  float wavePhaseRadians_ = 0.0f;
  float sectorEnergy_[kSectorCount] = {};
  uint8_t fadePhase_ = 0;
  uint16_t fadedRows_[kFadeRowsPerFrame];

  uint32_t lastFrameMs_ = 0;
  uint32_t renderedFrameCount_ = 0;
  uint32_t slowestFrameTimeUs_ = 0;
  uint32_t framesInMeasurementWindow_ = 0;
  uint32_t measurementWindowStartedMs_ = 0;
  float measuredFramesPerSecond_ = 0.0f;

  uint32_t nextRandomNumber();
  float nextRandomUnitFloat();
  void initializeSeededComposition(uint32_t seed);
  void fadePartOfCanvas();
  void pushFadedRowsToDisplay();
  void drawPolarLattice();
  void drawSpiralArms();
  void drawOscillatingRings();
  void updateSectorEnergy();
  void updateFrameRateMeasurement(uint32_t nowMs);
};

#endif  // SEED146_RADIAL_PROTOTYPE_H
