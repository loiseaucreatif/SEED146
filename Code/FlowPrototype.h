#ifndef SEED146_FLOW_PROTOTYPE_H
#define SEED146_FLOW_PROTOTYPE_H

#include <Arduino.h>
#include <LovyanGFX.h>

/**
 * First visual FLOW experiment.
 *
 * This class deliberately owns only the artwork. It does not read the encoder,
 * button, touch controller, or LEDs. Keeping those responsibilities separate
 * lets us judge rendering quality and performance before designing the final
 * application architecture.
 */
class FlowPrototype {
 public:
  static constexpr uint8_t kSectorCount = 8;

  explicit FlowPrototype(lgfx::LovyanGFX* targetDisplay);

  // Allocates one 360 x 360 RGB565 canvas in PSRAM and initializes a
  // deterministic particle composition. Returns false if allocation fails.
  bool begin(uint32_t seed);

  // Clears the existing canvas and rebuilds the composition from a new seed.
  // The already allocated PSRAM canvas and selected turbulence are preserved.
  bool regenerate(uint32_t seed);

  // Restores the preserved artwork after another screen temporarily used the
  // panel. No simulation step or PSRAM allocation occurs.
  void showCurrentFrame();

  // Advances and renders at the internally limited frame rate.
  void update(uint32_t nowMs);

  // Sets the amount of irregular cross-current. The public value is normalized:
  // 0.0 means calm, 0.5 reproduces the approved FLOW, and 1.0 is most turbulent.
  void setTurbulence(float normalizedValue);
  float turbulence() const;

  // Returns smoothed activity for one angular screen sector, normalized 0..1.
  // Sector 0 starts at screen-right and the indices advance clockwise.
  float sectorEnergy(uint8_t sectorIndex) const;
  float vortexCenterX() const;
  float vortexCenterY() const;

  bool isReady() const;
  uint32_t seed() const;
  uint32_t renderedFrameCount() const;
  float measuredFramesPerSecond() const;
  uint32_t slowestFrameTimeUs() const;
  size_t canvasSizeBytes() const;

 private:
  struct Particle {
    float x;
    float y;
    float speedMultiplier;
    float glitchDirectionX;
    float glitchDirectionY;
    float glitchSpeedMultiplier;
    float renderedX;
    float renderedY;
    uint32_t remainingLifeMs;
    uint32_t timeUntilGlitchMs;
    uint16_t glitchRemainingMs;
    uint8_t grayLevel;
  };

  static constexpr uint16_t kCanvasWidth = 360;
  static constexpr uint16_t kCanvasHeight = 360;
  static constexpr uint16_t kCanvasCenter = 180;
  static constexpr uint16_t kParticleCount = 140;
  static constexpr uint16_t kParticleSpawnRadius = 176;
  static constexpr uint16_t kParticleBoundaryRadius = 179;
  static constexpr uint32_t kTargetFramePeriodMs = 33;  // About 30 FPS maximum.
  static constexpr uint8_t kFadePhaseCount = 8;
  static constexpr uint16_t kFadeRowsPerFrame =
      kCanvasHeight / kFadePhaseCount;
  static constexpr uint8_t kFadeMultiplier = 244;  // 244 / 256 per fade pass.
  static constexpr float kParticleSpeedPixelsPerSecond = 31.0f;
  static constexpr float kVortexCenterSpeedPixelsPerSecond = 7.0f;
  // The center moves freely inside 80 px. Between 80 and 105 px, a progressive
  // inward steering force bends its path so it grazes the circular limit.
  static constexpr float kVortexSoftBoundaryStartRadius = 80.0f;
  static constexpr float kVortexMaximumRoamRadius = 105.0f;
  static constexpr float kVortexBoundarySteeringPixelsPerSecondSquared = 3.2f;
  static constexpr float kVortexBoundaryGlidePixelsPerSecondSquared = 4.0f;
  // The displayed value approaches the encoder target gradually. A full-range
  // transition takes less than one second, avoiding an abrupt visual jump.
  static constexpr float kTurbulenceResponsePerSecond = 1.5f;
  // Negative screen-space rotation is counter-clockwise because display Y
  // coordinates grow downward. One revolution takes approximately 90 seconds:
  // visible enough to read, but still slower than the local filament motion.
  static constexpr float kGlobalRotationRadiansPerSecond = -0.069813170f;
  // 73 and 360 have no common divisor. Multiplying a row index by 73 therefore
  // visits every row exactly once in a visually dispersed order.
  static constexpr uint16_t kFadeRowPermutationMultiplier = 73;

  lgfx::LovyanGFX* targetDisplay_;
  lgfx::LGFX_Sprite canvas_;
  Particle particles_[kParticleCount];

  bool ready_ = false;
  uint32_t seed_ = 0;
  uint32_t randomState_ = 1;
  float fieldPhaseA_ = 0.0f;
  float fieldPhaseB_ = 0.0f;
  float currentTurbulence_ = 0.5f;
  float targetTurbulence_ = 0.5f;
  float vortexCenterX_ = kCanvasCenter;
  float vortexCenterY_ = kCanvasCenter;
  float vortexCenterVelocityX_ = 0.0f;
  float vortexCenterVelocityY_ = 0.0f;
  float vortexBoundaryGlideDirection_ = 1.0f;
  float sectorEnergy_[kSectorCount] = {};
  float globalRenderRotationRadians_ = 0.0f;
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
  void updateVortexCenter(float elapsedSeconds);
  void respawnParticle(Particle& particle);
  void updateRenderedPosition(Particle& particle);
  void fadePartOfCanvas();
  void pushFadedRowsToDisplay();
  void updateParticles(uint32_t elapsedMs, float elapsedSeconds);
  void updateFrameRateMeasurement(uint32_t nowMs);
};

#endif  // SEED146_FLOW_PROTOTYPE_H
