#ifndef SEED146_PARTICLES_PROTOTYPE_H
#define SEED146_PARTICLES_PROTOTYPE_H
#include <Arduino.h>
#include <LovyanGFX.h>

// Inertial particles: each point owns its velocity, instead of following a
// rotating field. Input and LED hardware remain in the main sketch.
class ParticlesPrototype {
 public:
  static constexpr uint8_t kSectorCount = 8;
  explicit ParticlesPrototype(lgfx::LovyanGFX* targetDisplay);
  bool begin(uint32_t seed);
  bool regenerate(uint32_t seed);
  void showCurrentFrame();
  void update(uint32_t nowMs);
  void setEnergy(float value);
  bool isReady() const { return ready_; }
  uint32_t seed() const { return seed_; }
  const char* familyName() const;
  float edgeImpulse(uint8_t sector) const;
  uint32_t renderedFrameCount() const { return renderedFrameCount_; }
  float measuredFramesPerSecond() const { return measuredFramesPerSecond_; }
  uint32_t slowestFrameTimeUs() const { return slowestFrameTimeUs_; }
  size_t canvasSizeBytes() const { return kCanvasWidth * kCanvasHeight * 2U; }
 private:
  enum class Family : uint8_t { burst, cross, bounce };
  struct Particle {
    float x, y;
    float velocityX, velocityY; // Pixels per second at reference energy.
    float remainingLifeSeconds;
    uint8_t grayLevel;
    bool scattered;
    Family behavior;
  };
  static constexpr uint16_t kCanvasWidth = 360;
  static constexpr uint16_t kCanvasHeight = 360;
  static constexpr float kCenter = 180.0f;
  static constexpr float kBoundaryRadius = 176.0f;
  static constexpr uint16_t kParticleCount = 160;
  static constexpr uint32_t kTargetFramePeriodMs = 33;
  // Strong attenuation keeps a short wake; dispersed rows avoid a dark sweep.
  static constexpr uint8_t kFadePhaseCount = 8;
  static constexpr uint16_t kFadeRowsPerFrame = 45;
  static constexpr uint8_t kFadeMultiplier = 100;
  static constexpr uint16_t kFadeRowPermutationMultiplier = 73;
  static constexpr float kMaximumSpeed = 240.0f;
  static constexpr float kLocalImpulseRadius = 105.0f;
  lgfx::LovyanGFX* targetDisplay_;
  lgfx::LGFX_Sprite canvas_;
  Particle particles_[kParticleCount] = {};
  bool ready_ = false;
  uint32_t seed_ = 0, randomState_ = 1;
  // Separate fixed pools keep a burst from replacing the jets or rebounders.
  static constexpr uint16_t kBounceCount = 56;
  static constexpr uint16_t kBurstCount = 56;
  static constexpr uint16_t kCrossCount = kParticleCount - kBounceCount - kBurstCount;
  static constexpr float kGlobalRotationRadiansPerSecond = TWO_PI / 45.0f;
  float globalRotation_ = 0.0f;
  float previousGlobalRotation_ = 0.0f;
  float burstRateScale_ = 1.0f;
  float jetRateScale_ = 1.0f;
  float targetEnergy_ = 0.5f, currentEnergy_ = 0.5f;
  float axisAngle_ = 0.0f;
  float eventCountdownSeconds_ = 0.0f;
  float emissionAccumulator_ = 0.0f;
  uint16_t nextBounceIndex_ = 0;
  uint16_t nextBurstIndex_ = 0;
  uint16_t nextCrossIndex_ = 0;
  float edgeImpulse_[kSectorCount] = {};
  float edgeCooldown_[kSectorCount] = {};
  uint8_t fadePhase_ = 0;
  uint16_t fadedRows_[kFadeRowsPerFrame] = {};
  uint32_t lastFrameMs_ = 0, renderedFrameCount_ = 0;
  uint32_t slowestFrameTimeUs_ = 0, framesInMeasurementWindow_ = 0;
  uint32_t measurementWindowStartedMs_ = 0;
  float measuredFramesPerSecond_ = 0.0f;
  float randomUnit();
  void initializeSeededComposition(uint32_t seed);
  void emitParticle(float x, float y, float angle, Family behavior);
  void triggerEvent();
  void simulateAndDraw(float elapsedSeconds);
  void registerEdgeContact(const Particle& particle);
  void fadePartOfCanvas();
  void pushFadedRowsToDisplay();
};
#endif
