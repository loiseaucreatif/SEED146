#ifndef SEED146_CELLULAR_PROTOTYPE_H
#define SEED146_CELLULAR_PROTOTYPE_H

#include <Arduino.h>
#include <LovyanGFX.h>

// CELLULAR keeps Conway's B3/S23 neighbourhood rules, then adds a biological
// layer: every living cell has health, age and a deterministic maximum life.
// Rendering remains separate from the model so generations can advance at a
// measured pace while the soft circles animate at a smoother display rate.
class CellularPrototype {
 public:
  static constexpr uint8_t kSectorCount = 8;

  explicit CellularPrototype(lgfx::LovyanGFX* targetDisplay);

  bool begin(uint32_t seed);
  bool regenerate(uint32_t seed);
  void showCurrentFrame();
  void update(uint32_t nowMs);
  void setMutation(float normalizedValue);

  bool isReady() const { return ready_; }
  uint32_t seed() const { return seed_; }
  uint32_t renderedFrameCount() const { return renderedFrameCount_; }
  float measuredFramesPerSecond() const { return measuredFramesPerSecond_; }
  uint32_t slowestFrameTimeUs() const { return slowestFrameTimeUs_; }
  uint32_t generation() const { return generation_; }
  uint16_t livingCellCount() const { return livingCellCount_; }
  uint32_t automaticReseedCount() const { return automaticReseedCount_; }
  uint8_t lastDetectedLoopPeriod() const { return lastDetectedLoopPeriod_; }
  float rotationPeriodSeconds() const { return rotationPeriodSeconds_; }
  float breathingPeriodSeconds() const {
    return kBreathingInhaleSeconds + kBreathingExhaleSeconds;
  }
  float breathingPhaseLevel() const { return breathingLevel(); }
  float sectorPopulationEnergy(uint8_t sectorIndex) const {
    return sectorIndex < kSectorCount
               ? sectorPopulationEnergy_[sectorIndex]
               : 0.0f;
  }
  const char* rotationDirectionName() const {
    return rotationDirection_ > 0.0f ? "CW" : "CCW";
  }
  const char* lastAutomaticReseedReason() const;
  size_t canvasSizeBytes() const {
    return (kCanvasWidth * kCanvasHeight * kCanvasBitsPerPixel + 7U) / 8U;
  }
  size_t modelSizeBytes() const;

 private:
  struct Cell {
    float displayedStrength;
    uint16_t age;
    uint16_t maximumAge;
    uint16_t vibrationPhase;
    uint8_t health;
    uint8_t alive;
  };

  enum class RestartReason : uint8_t {
    none,
    extinction,
    stagnation,
    loop,
  };

  enum class RestartPhase : uint8_t {
    none,
    fadingOut,
    fadingIn,
  };

  static constexpr uint16_t kCanvasWidth = 360;
  static constexpr uint16_t kCanvasHeight = 360;
  static constexpr uint8_t kCanvasBitsPerPixel = 4;

  // Twelve-pixel cells give thirty visible columns. Four additional columns on
  // every side make the simulated world 456 x 456 pixels, larger than the
  // display even though only its centered 360 x 360 window is rendered.
  static constexpr uint8_t kCellPitchPixels = 12;
  static constexpr uint8_t kVisibleGridSize = 30;
  static constexpr uint8_t kOffscreenMarginCells = 4;
  static constexpr uint8_t kWorldGridSize =
      kVisibleGridSize + kOffscreenMarginCells * 2U;
  static constexpr uint16_t kCellCount =
      static_cast<uint16_t>(kWorldGridSize) * kWorldGridSize;

  static constexpr uint32_t kTargetRenderPeriodMs = 33;
  static constexpr uint32_t kGenerationPeriodMs = 200;
  static constexpr uint8_t kMaximumDetectedLoopPeriod = 8;
  static constexpr uint8_t kTopologyHistoryLength =
      kMaximumDetectedLoopPeriod * 2U + 1U;
  static constexpr uint16_t kMinimumLivingPopulation = 48;
  static constexpr uint8_t kMinimumGenerationsBeforeRestart = 24;
  static constexpr uint8_t kStagnantGenerationLimit = 12;
  static constexpr uint32_t kRestartFadeOutMs = 450;
  static constexpr uint32_t kRestartFadeInMs = 650;
  static constexpr float kBreathingInhaleSeconds = 5.0f;
  static constexpr float kBreathingExhaleSeconds = 7.0f;
  static constexpr float kMinimumBreathingScale = 0.96f;
  static constexpr float kMaximumBreathingScale = 1.24f;
  static constexpr float kMinimumRotationSpeedMultiplier = 0.30f;
  static constexpr float kMaximumRotationSpeedMultiplier = 0.85f;
  static constexpr float kRotationBreathEmphasis = 1.35f;

  lgfx::LGFX_Sprite canvas_;
  Cell* cells_ = nullptr;
  uint8_t* nextAliveStates_ = nullptr;
  uint64_t topologyHistory_[kTopologyHistoryLength] = {};

  bool ready_ = false;
  uint32_t seed_ = 0;
  uint32_t randomState_ = 1;
  float mutation_ = 0.5f;
  float rotationAngleRadians_ = 0.0f;
  float rotationDirection_ = 1.0f;
  float rotationPeriodSeconds_ = 60.0f;
  float breathingTimeSeconds_ = 0.0f;
  uint32_t generation_ = 0;
  uint16_t livingCellCount_ = 0;
  uint8_t stagnantGenerationCount_ = 0;
  uint8_t topologyHistoryCount_ = 0;
  float sectorPopulationEnergy_[kSectorCount] = {};

  RestartReason restartReason_ = RestartReason::none;
  RestartPhase restartPhase_ = RestartPhase::none;
  uint32_t restartPhaseStartedMs_ = 0;
  uint32_t automaticReseedCount_ = 0;
  uint8_t lastDetectedLoopPeriod_ = 0;

  uint32_t lastRenderMs_ = 0;
  uint32_t lastGenerationMs_ = 0;
  uint32_t renderedFrameCount_ = 0;
  uint32_t slowestFrameTimeUs_ = 0;
  uint32_t framesInMeasurementWindow_ = 0;
  uint32_t measurementWindowStartedMs_ = 0;
  float measuredFramesPerSecond_ = 0.0f;

  uint32_t nextRandomUint32();
  float nextRandomUnitFloat();
  uint16_t cellIndex(uint8_t x, uint8_t y) const;
  uint8_t livingNeighbourCount(uint8_t x, uint8_t y) const;
  void initializeSeededWorld(uint32_t seed);
  void guaranteeVisibleStartingPopulation();
  void advanceGeneration(uint32_t nowMs);
  void applyMutations();
  uint64_t calculateTopologyHash() const;
  uint8_t detectRepeatedTopology(uint64_t topologyHash) const;
  float breathingLevel() const;
  void rememberTopology(uint64_t topologyHash);
  void scheduleAutomaticRestart(RestartReason reason,
                                uint8_t loopPeriod,
                                uint32_t nowMs);
  float updateRestartTransition(uint32_t nowMs);
  void renderDitheredCells(float elapsedSeconds,
                           float transitionBrightness,
                           uint32_t nowMs);
  void updateFrameRateMeasurement(uint32_t nowMs);
};

#endif  // SEED146_CELLULAR_PROTOTYPE_H
