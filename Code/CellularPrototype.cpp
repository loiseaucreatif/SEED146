#include "CellularPrototype.h"

#include <math.h>
#include <string.h>

namespace {

// The same stationary Bayer structure that gives BLOBS its approved
// black-and-white softness is used here. CELLULAR keeps separate circular
// bodies, so it shares the material language without becoming another
// continuous metaball field.
constexpr uint8_t kBayer4x4[4][4] = {
    {0, 8, 2, 10},
    {12, 4, 14, 6},
    {3, 11, 1, 9},
    {15, 7, 13, 5},
};

uint8_t clampByte(int value) {
  return static_cast<uint8_t>(constrain(value, 0, 255));
}

}  // namespace

CellularPrototype::CellularPrototype(lgfx::LovyanGFX* targetDisplay)
    : canvas_(targetDisplay) {}

bool CellularPrototype::begin(uint32_t seed) {
  if (ready_) {
    return true;
  }

  // Allocate the model once in PSRAM. No cell, history or render allocation
  // occurs inside the animation loop.
  cells_ = static_cast<Cell*>(ps_malloc(sizeof(Cell) * kCellCount));
  nextAliveStates_ = static_cast<uint8_t*>(ps_malloc(kCellCount));
  if (cells_ == nullptr || nextAliveStates_ == nullptr) {
    if (cells_ != nullptr) {
      free(cells_);
      cells_ = nullptr;
    }
    if (nextAliveStates_ != nullptr) {
      free(nextAliveStates_);
      nextAliveStates_ = nullptr;
    }
    return false;
  }

  canvas_.setColorDepth(kCanvasBitsPerPixel);
  canvas_.setPsram(true);
  if (!canvas_.createSprite(kCanvasWidth, kCanvasHeight)) {
    free(cells_);
    free(nextAliveStates_);
    cells_ = nullptr;
    nextAliveStates_ = nullptr;
    return false;
  }
  canvas_.setPaletteGrayscale();

  ready_ = true;
  initializeSeededWorld(seed);
  return true;
}

bool CellularPrototype::regenerate(uint32_t seed) {
  if (!ready_) {
    return false;
  }
  restartPhase_ = RestartPhase::none;
  restartReason_ = RestartReason::none;
  initializeSeededWorld(seed);
  showCurrentFrame();
  return true;
}

void CellularPrototype::showCurrentFrame() {
  if (!ready_) {
    return;
  }
  canvas_.pushSprite(0, 0);
  const uint32_t nowMs = millis();
  // Menu time belongs neither to the visual vibration nor to cellular time.
  lastRenderMs_ = nowMs;
  lastGenerationMs_ = nowMs;
  measurementWindowStartedMs_ = nowMs;
  framesInMeasurementWindow_ = 0;
  measuredFramesPerSecond_ = 0.0f;
}

void CellularPrototype::setMutation(float normalizedValue) {
  mutation_ = constrain(normalizedValue, 0.0f, 1.0f);
}

size_t CellularPrototype::modelSizeBytes() const {
  return sizeof(Cell) * kCellCount + kCellCount;
}

const char* CellularPrototype::lastAutomaticReseedReason() const {
  switch (restartReason_) {
    case RestartReason::extinction:
      return "LOW POPULATION";
    case RestartReason::stagnation:
      return "STAGNATION";
    case RestartReason::loop:
      return "LOOP";
    case RestartReason::none:
      break;
  }
  return "NONE";
}

uint32_t CellularPrototype::nextRandomUint32() {
  randomState_ ^= randomState_ << 13U;
  randomState_ ^= randomState_ >> 17U;
  randomState_ ^= randomState_ << 5U;
  return randomState_;
}

float CellularPrototype::nextRandomUnitFloat() {
  return (nextRandomUint32() & 0xFFFFFFU) / 16777216.0f;
}

uint16_t CellularPrototype::cellIndex(uint8_t x, uint8_t y) const {
  return static_cast<uint16_t>(y) * kWorldGridSize + x;
}

uint8_t CellularPrototype::livingNeighbourCount(uint8_t x, uint8_t y) const {
  uint8_t count = 0;
  for (int8_t offsetY = -1; offsetY <= 1; ++offsetY) {
    for (int8_t offsetX = -1; offsetX <= 1; ++offsetX) {
      if (offsetX == 0 && offsetY == 0) {
        continue;
      }
      // The oversized world wraps at its own hidden boundary, not at the
      // display edge. Visible border cells therefore have genuine neighbours
      // beyond the circular viewport.
      const uint8_t neighbourX = static_cast<uint8_t>(
          (x + offsetX + kWorldGridSize) % kWorldGridSize);
      const uint8_t neighbourY = static_cast<uint8_t>(
          (y + offsetY + kWorldGridSize) % kWorldGridSize);
      count += cells_[cellIndex(neighbourX, neighbourY)].alive != 0U;
    }
  }
  return count;
}

void CellularPrototype::initializeSeededWorld(uint32_t seed) {
  seed_ = seed ? seed : 1U;
  randomState_ = seed_;
  generation_ = 0;
  stagnantGenerationCount_ = 0;
  topologyHistoryCount_ = 0;
  lastDetectedLoopPeriod_ = 0;
  memset(topologyHistory_, 0, sizeof(topologyHistory_));

  // A narrow seed-dependent density range keeps every composition fine and
  // populated while still creating visibly different starting topologies.
  const float initialDensity = 0.34f + nextRandomUnitFloat() * 0.05f;
  // Derive display rotation without consuming the model's random sequence.
  // Adding this graphical transform therefore preserves every approved seeded
  // cellular topology from step 8.1.
  const uint32_t rotationBits = seed_ * 747796405UL + 2891336453UL;
  rotationAngleRadians_ =
      (rotationBits & 0xFFFFU) * (TWO_PI / 65536.0f);
  rotationDirection_ = (rotationBits & 0x10000U) == 0U ? -1.0f : 1.0f;
  rotationPeriodSeconds_ =
      48.0f + ((rotationBits >> 17U) & 0x7FFFU) * (24.0f / 32767.0f);
  breathingTimeSeconds_ = ((rotationBits >> 5U) & 0x0FFFU) *
      ((kBreathingInhaleSeconds + kBreathingExhaleSeconds) / 4095.0f);
  for (uint16_t index = 0; index < kCellCount; ++index) {
    Cell& cell = cells_[index];
    cell.alive = nextRandomUnitFloat() < initialDensity ? 1U : 0U;
    cell.age = 0;
    cell.maximumAge = static_cast<uint16_t>(
        70U + nextRandomUint32() % 151U);  // 14–44 seconds at 5 Hz.
    cell.vibrationPhase = static_cast<uint16_t>(nextRandomUint32() & 0xFFFFU);
    cell.health = cell.alive
                      ? static_cast<uint8_t>(170U + nextRandomUint32() % 61U)
                      : 0U;
    cell.displayedStrength = 0.0f;
  }

  guaranteeVisibleStartingPopulation();
  livingCellCount_ = 0;
  for (uint16_t index = 0; index < kCellCount; ++index) {
    livingCellCount_ += cells_[index].alive != 0U;
  }
  rememberTopology(calculateTopologyHash());

  const uint32_t nowMs = millis();
  lastGenerationMs_ = nowMs;
  lastRenderMs_ = nowMs;
  measurementWindowStartedMs_ = nowMs;
  framesInMeasurementWindow_ = 0;
  measuredFramesPerSecond_ = 0.0f;
  renderDitheredCells(1.0f / 30.0f, 1.0f, nowMs);
}

void CellularPrototype::guaranteeVisibleStartingPopulation() {
  constexpr uint16_t kMinimumVisibleCells = 300;
  constexpr uint16_t kMinimumCellsPerQuadrant = 65;
  uint16_t visibleCount = 0;
  uint16_t quadrantCount[4] = {};

  for (uint8_t visibleY = 0; visibleY < kVisibleGridSize; ++visibleY) {
    for (uint8_t visibleX = 0; visibleX < kVisibleGridSize; ++visibleX) {
      const uint8_t worldX = visibleX + kOffscreenMarginCells;
      const uint8_t worldY = visibleY + kOffscreenMarginCells;
      if (cells_[cellIndex(worldX, worldY)].alive == 0U) {
        continue;
      }
      ++visibleCount;
      const uint8_t quadrant = (visibleX >= kVisibleGridSize / 2U ? 1U : 0U) +
          (visibleY >= kVisibleGridSize / 2U ? 2U : 0U);
      ++quadrantCount[quadrant];
    }
  }

  // Deterministically fill under-populated areas. This is not an after-the-
  // fact random retry: the same seed always adds the same cells and produces
  // exactly the same starting composition.
  uint16_t safetyAttempts = 0;
  while ((visibleCount < kMinimumVisibleCells ||
          quadrantCount[0] < kMinimumCellsPerQuadrant ||
          quadrantCount[1] < kMinimumCellsPerQuadrant ||
          quadrantCount[2] < kMinimumCellsPerQuadrant ||
          quadrantCount[3] < kMinimumCellsPerQuadrant) &&
         safetyAttempts < 5000U) {
    ++safetyAttempts;
    const uint8_t visibleX = nextRandomUint32() % kVisibleGridSize;
    const uint8_t visibleY = nextRandomUint32() % kVisibleGridSize;
    const uint8_t quadrant = (visibleX >= kVisibleGridSize / 2U ? 1U : 0U) +
        (visibleY >= kVisibleGridSize / 2U ? 2U : 0U);
    if (visibleCount >= kMinimumVisibleCells &&
        quadrantCount[quadrant] >= kMinimumCellsPerQuadrant) {
      continue;
    }

    Cell& cell = cells_[cellIndex(visibleX + kOffscreenMarginCells,
                                  visibleY + kOffscreenMarginCells)];
    if (cell.alive == 0U) {
      cell.alive = 1U;
      cell.health = static_cast<uint8_t>(170U + nextRandomUint32() % 61U);
      ++visibleCount;
      ++quadrantCount[quadrant];
    }
  }
}

void CellularPrototype::advanceGeneration(uint32_t nowMs) {
  for (uint8_t y = 0; y < kWorldGridSize; ++y) {
    for (uint8_t x = 0; x < kWorldGridSize; ++x) {
      const uint16_t index = cellIndex(x, y);
      const Cell& cell = cells_[index];
      const uint8_t neighbours = livingNeighbourCount(x, y);
      const bool survives = cell.alive != 0U &&
          (neighbours == 2U || neighbours == 3U) &&
          cell.age + 1U < cell.maximumAge;
      const bool born = cell.alive == 0U && neighbours == 3U;
      // Bit zero is the next alive state; the upper bits retain the neighbour
      // count from the untouched current generation for the health update.
      nextAliveStates_[index] = static_cast<uint8_t>(
          (neighbours << 1U) | (survives || born ? 1U : 0U));
    }
  }

  applyMutations();

  livingCellCount_ = 0;
  uint16_t births = 0;
  uint16_t deaths = 0;
  for (uint16_t index = 0; index < kCellCount; ++index) {
    Cell& cell = cells_[index];
    const bool wasAlive = cell.alive != 0U;
    const bool willLive = (nextAliveStates_[index] & 1U) != 0U;
    const uint8_t neighbours = nextAliveStates_[index] >> 1U;
    cell.alive = willLive ? 1U : 0U;
    births += !wasAlive && willLive;
    deaths += wasAlive && !willLive;

    if (willLive && !wasAlive) {
      cell.age = 0;
      cell.maximumAge = static_cast<uint16_t>(
          70U + nextRandomUint32() % 151U);
      cell.health = static_cast<uint8_t>(175U + nextRandomUint32() % 56U);
      cell.vibrationPhase = static_cast<uint16_t>(
          nextRandomUint32() & 0xFFFFU);
    } else if (willLive) {
      ++cell.age;
      // Three neighbours are the healthiest condition. Two keeps the cell
      // alive but slowly raises its stress, which becomes visible vibration.
      const int healthChange = neighbours == 3U ? 12 : -5;
      cell.health = clampByte(static_cast<int>(cell.health) + healthChange);
    } else if (wasAlive) {
      cell.health = 0U;
    }

    livingCellCount_ += willLive;
  }

  ++generation_;
  const uint16_t changedCells = births + deaths;
  if (changedCells <= 3U) {
    ++stagnantGenerationCount_;
  } else {
    stagnantGenerationCount_ = 0;
  }

  const uint64_t topologyHash = calculateTopologyHash();
  const uint8_t repeatedPeriod = detectRepeatedTopology(topologyHash);
  rememberTopology(topologyHash);

  if (generation_ < kMinimumGenerationsBeforeRestart ||
      restartPhase_ != RestartPhase::none) {
    return;
  }
  if (livingCellCount_ < kMinimumLivingPopulation) {
    scheduleAutomaticRestart(RestartReason::extinction, 0, nowMs);
  } else if (repeatedPeriod != 0U) {
    scheduleAutomaticRestart(RestartReason::loop, repeatedPeriod, nowMs);
  } else if (stagnantGenerationCount_ >= kStagnantGenerationLimit) {
    scheduleAutomaticRestart(RestartReason::stagnation, 0, nowMs);
  }
}

void CellularPrototype::applyMutations() {
  // MUTATION is intentionally nonlinear near zero. The pure aged-Conway model
  // remains readable at low values; the upper range can inject up to four
  // deterministic births/deaths per generation without becoming white noise.
  const float expectedMutationCount = mutation_ * mutation_ * 4.0f;
  uint8_t mutationCount = static_cast<uint8_t>(expectedMutationCount);
  if (nextRandomUnitFloat() < expectedMutationCount - mutationCount) {
    ++mutationCount;
  }

  for (uint8_t mutationIndex = 0;
       mutationIndex < mutationCount;
       ++mutationIndex) {
    const uint16_t index = nextRandomUint32() % kCellCount;
    nextAliveStates_[index] ^= 1U;
  }
}

uint64_t CellularPrototype::calculateTopologyHash() const {
  // Loop detection deliberately hashes only alive/dead topology. Age, health
  // and render interpolation may still be changing, but a repeated cellular
  // pattern would otherwise remain structurally trapped.
  uint64_t hash = 1469598103934665603ULL;
  for (uint16_t index = 0; index < kCellCount; ++index) {
    hash ^= cells_[index].alive != 0U ? 0xA7U : 0x31U;
    hash *= 1099511628211ULL;
  }
  return hash;
}

uint8_t CellularPrototype::detectRepeatedTopology(uint64_t topologyHash) const {
  // Require two completed repetitions. A transient that happens to revisit
  // one previous state is therefore not mistaken for an established loop.
  for (uint8_t period = 1; period <= kMaximumDetectedLoopPeriod; ++period) {
    if (topologyHistoryCount_ < period * 2U) {
      continue;
    }
    if (topologyHistory_[topologyHistoryCount_ - period] == topologyHash &&
        topologyHistory_[topologyHistoryCount_ - period * 2U] == topologyHash) {
      return period;
    }
  }
  return 0;
}

void CellularPrototype::rememberTopology(uint64_t topologyHash) {
  if (topologyHistoryCount_ < kTopologyHistoryLength) {
    topologyHistory_[topologyHistoryCount_++] = topologyHash;
    return;
  }
  memmove(topologyHistory_,
          topologyHistory_ + 1,
          sizeof(topologyHistory_[0]) * (kTopologyHistoryLength - 1U));
  topologyHistory_[kTopologyHistoryLength - 1U] = topologyHash;
}

float CellularPrototype::breathingLevel() const {
  // Two half-cosines provide zero velocity at empty lungs, full lungs and the
  // join between them. The longer exhale gives the motion a calm meditative
  // asymmetry without introducing a visible timing corner.
  if (breathingTimeSeconds_ < kBreathingInhaleSeconds) {
    const float inhaleProgress =
        breathingTimeSeconds_ / kBreathingInhaleSeconds;
    return 0.5f - 0.5f * cosf(TWO_PI * 0.5f * inhaleProgress);
  }
  const float exhaleProgress =
      (breathingTimeSeconds_ - kBreathingInhaleSeconds) /
      kBreathingExhaleSeconds;
  return 0.5f + 0.5f * cosf(TWO_PI * 0.5f * exhaleProgress);
}

void CellularPrototype::scheduleAutomaticRestart(RestartReason reason,
                                                 uint8_t loopPeriod,
                                                 uint32_t nowMs) {
  restartReason_ = reason;
  lastDetectedLoopPeriod_ = loopPeriod;
  restartPhase_ = RestartPhase::fadingOut;
  restartPhaseStartedMs_ = nowMs;
}

float CellularPrototype::updateRestartTransition(uint32_t nowMs) {
  if (restartPhase_ == RestartPhase::none) {
    return 1.0f;
  }

  const uint32_t phaseElapsedMs = nowMs - restartPhaseStartedMs_;
  if (restartPhase_ == RestartPhase::fadingOut) {
    if (phaseElapsedMs < kRestartFadeOutMs) {
      return 1.0f - phaseElapsedMs / static_cast<float>(kRestartFadeOutMs);
    }

    uint32_t nextSeed = seed_ * 1664525UL + 1013904223UL;
    if (nextSeed == 0U) {
      nextSeed = 1U;
    }
    ++automaticReseedCount_;
    const uint8_t detectedLoopPeriod = lastDetectedLoopPeriod_;
    initializeSeededWorld(nextSeed);
    lastDetectedLoopPeriod_ = detectedLoopPeriod;
    restartPhase_ = RestartPhase::fadingIn;
    restartPhaseStartedMs_ = nowMs;
    return 0.0f;
  }

  if (phaseElapsedMs >= kRestartFadeInMs) {
    restartPhase_ = RestartPhase::none;
    return 1.0f;
  }
  return phaseElapsedMs / static_cast<float>(kRestartFadeInMs);
}

void CellularPrototype::renderDitheredCells(float elapsedSeconds,
                                            float transitionBrightness,
                                            uint32_t nowMs) {
  uint8_t* storedPixels = static_cast<uint8_t*>(canvas_.getBuffer());
  if (storedPixels == nullptr) {
    return;
  }
  memset(storedPixels, 0, canvasSizeBytes());

  const float generationFraction = constrain(
      (nowMs - lastGenerationMs_) / static_cast<float>(kGenerationPeriodMs),
      0.0f,
      1.0f);
  const float animationSeconds = nowMs * 0.001f;
  const float rotationCosine = cosf(rotationAngleRadians_);
  const float rotationSine = sinf(rotationAngleRadians_);
  const float breath = breathingLevel();
  const float breathingScale = kMinimumBreathingScale + breath *
      (kMaximumBreathingScale - kMinimumBreathingScale);

  // CELLULAR's ambient LEDs describe the complete visible population rather
  // than only border contacts. These fixed-size accumulators add no render-loop
  // allocation. Sector selection happens after the approved rotation and
  // breathing transforms, so the physical ring stays spatially attached to
  // the monochrome colonies.
  float sectorPopulationEnergy[kSectorCount] = {};

  for (uint8_t worldY = 0; worldY < kWorldGridSize; ++worldY) {
    for (uint8_t worldX = 0; worldX < kWorldGridSize; ++worldX) {
      Cell& cell = cells_[cellIndex(worldX, worldY)];
      float targetStrength = 0.0f;
      float stress = 1.0f;
      float maturity = 0.0f;

      if (cell.alive != 0U) {
        maturity = constrain((cell.age + generationFraction) / 7.0f,
                             0.0f,
                             1.0f);
        const float health = cell.health / 255.0f;
        const float lifetime = constrain(
            (cell.age + generationFraction) /
                static_cast<float>(cell.maximumAge),
            0.0f,
            1.0f);
        const float oldAge = constrain((lifetime - 0.68f) / 0.32f,
                                       0.0f,
                                       1.0f);
        stress = constrain((1.0f - health) * 0.72f + oldAge * 0.82f,
                           0.0f,
                           1.0f);
        targetStrength = (0.24f + maturity * 0.76f) *
            (0.58f + health * 0.42f) * (1.0f - oldAge * 0.62f);
      }

      const float responsePerSecond = targetStrength > cell.displayedStrength
                                          ? 4.8f
                                          : 2.7f;
      const float maximumChange = elapsedSeconds * responsePerSecond;
      cell.displayedStrength += constrain(
          targetStrength - cell.displayedStrength,
          -maximumChange,
          maximumChange);
      if (cell.displayedStrength < 0.012f) {
        cell.displayedStrength = 0.0f;
        continue;
      }

      const float phase = cell.vibrationPhase *
          (TWO_PI / 65536.0f);
      const float vibrationAmplitude = 0.10f + stress * 1.35f;
      const float vibrationSpeed = 2.0f + stress * 5.5f;
      const float offsetX = sinf(animationSeconds * vibrationSpeed + phase) *
          vibrationAmplitude;
      const float offsetY = cosf(animationSeconds * vibrationSpeed * 1.17f +
                                  phase * 1.71f) * vibrationAmplitude;

      const float unrotatedCenterX =
          (static_cast<int16_t>(worldX) - kOffscreenMarginCells + 0.5f) *
              kCellPitchPixels + offsetX;
      const float unrotatedCenterY =
          (static_cast<int16_t>(worldY) - kOffscreenMarginCells + 0.5f) *
              kCellPitchPixels + offsetY;
      const float modelX = unrotatedCenterX - kCanvasWidth * 0.5f;
      const float modelY = unrotatedCenterY - kCanvasHeight * 0.5f;
      const float centerX = kCanvasWidth * 0.5f + breathingScale *
          (modelX * rotationCosine - modelY * rotationSine);
      const float centerY = kCanvasHeight * 0.5f + breathingScale *
          (modelX * rotationSine + modelY * rotationCosine);
      const float bodyRadius = breathingScale *
          (2.0f + maturity * 3.2f + cell.displayedStrength * 0.8f);
      const float blurRadius = bodyRadius + 2.8f * breathingScale;

      const float screenOffsetX = centerX - kCanvasWidth * 0.5f;
      const float screenOffsetY = centerY - kCanvasHeight * 0.5f;
      constexpr float kVisibleRadius = kCanvasWidth * 0.5f;
      if (screenOffsetX * screenOffsetX + screenOffsetY * screenOffsetY <=
          kVisibleRadius * kVisibleRadius) {
        float screenAngle = atan2f(screenOffsetY, screenOffsetX);
        if (screenAngle < 0.0f) {
          screenAngle += TWO_PI;
        }
        const uint8_t sectorIndex = static_cast<uint8_t>(
            screenAngle * kSectorCount / TWO_PI) % kSectorCount;
        const float populationContribution =
            cell.displayedStrength * transitionBrightness;
        sectorPopulationEnergy[sectorIndex] += populationContribution;
      }

      const int16_t minimumX = max(0, static_cast<int>(floorf(centerX - blurRadius)));
      const int16_t maximumX = min(
          static_cast<int>(kCanvasWidth - 1U),
          static_cast<int>(ceilf(centerX + blurRadius)));
      const int16_t minimumY = max(0, static_cast<int>(floorf(centerY - blurRadius)));
      const int16_t maximumY = min(
          static_cast<int>(kCanvasHeight - 1U),
          static_cast<int>(ceilf(centerY + blurRadius)));
      if (minimumX > maximumX || minimumY > maximumY) {
        continue;
      }

      const float inverseSoftWidth = 1.0f / max(blurRadius - bodyRadius, 0.1f);
      for (int16_t pixelY = minimumY; pixelY <= maximumY; ++pixelY) {
        for (int16_t pixelX = minimumX; pixelX <= maximumX; ++pixelX) {
          const float distanceX = pixelX + 0.5f - centerX;
          const float distanceY = pixelY + 0.5f - centerY;
          const float distanceSquared = distanceX * distanceX +
                                        distanceY * distanceY;
          if (distanceSquared >= blurRadius * blurRadius) {
            continue;
          }
          float softLevel = 1.0f;
          if (distanceSquared > bodyRadius * bodyRadius) {
            const float distance = sqrtf(distanceSquared);
            const float edge = (blurRadius - distance) * inverseSoftWidth;
            softLevel = edge * edge * (3.0f - 2.0f * edge);
          }
          softLevel *= cell.displayedStrength * transitionBrightness;
          const uint8_t whiteThresholdCount = static_cast<uint8_t>(
              constrain(static_cast<int>(softLevel * 16.0f + 0.5f), 0, 16));
          if (whiteThresholdCount <=
              kBayer4x4[pixelY & 3U][pixelX & 3U]) {
            continue;
          }
          // Keep the packed sprite pointer local to this frame. Looking it up
          // again for every white pixel materially slows the dominant raster
          // loop on the ESP32-S3.
          constexpr uint16_t kPackedRowBytes = kCanvasWidth / 2U;
          uint8_t& packedPair = storedPixels[
              static_cast<size_t>(pixelY) * kPackedRowBytes + pixelX / 2U];
          if ((pixelX & 1U) == 0U) {
            packedPair |= 0xF0U;
          } else {
            packedPair |= 0x0FU;
          }
        }
      }
    }
  }

  for (uint8_t sectorIndex = 0; sectorIndex < kSectorCount; ++sectorIndex) {
    sectorPopulationEnergy_[sectorIndex] =
        sectorPopulationEnergy[sectorIndex];
  }
}

void CellularPrototype::update(uint32_t nowMs) {
  if (!ready_) {
    return;
  }

  // Generation timing is independent of display timing. If one frame is
  // unexpectedly slow, advance at most two generations so input cannot be
  // trapped behind a long catch-up loop.
  uint8_t generationsAdvanced = 0;
  while (restartPhase_ == RestartPhase::none &&
         nowMs - lastGenerationMs_ >= kGenerationPeriodMs &&
         generationsAdvanced < 2U) {
    lastGenerationMs_ += kGenerationPeriodMs;
    advanceGeneration(nowMs);
    ++generationsAdvanced;
  }
  if (nowMs - lastGenerationMs_ >= kGenerationPeriodMs * 3U) {
    lastGenerationMs_ = nowMs;
  }

  const uint32_t elapsedRenderMs = nowMs - lastRenderMs_;
  if (elapsedRenderMs < kTargetRenderPeriodMs) {
    return;
  }
  lastRenderMs_ = nowMs;
  const uint32_t frameStartedUs = micros();
  const float elapsedSeconds = min(elapsedRenderMs, uint32_t{120U}) / 1000.0f;
  breathingTimeSeconds_ += elapsedSeconds;
  const float breathingPeriod =
      kBreathingInhaleSeconds + kBreathingExhaleSeconds;
  if (breathingTimeSeconds_ >= breathingPeriod) {
    breathingTimeSeconds_ -= breathingPeriod;
  }
  // Rotation follows the same soft envelope as zoom: slow at the end of the
  // exhale, progressively faster through the inhale, then gently decelerating.
  const float emphasizedBreath =
      powf(breathingLevel(), kRotationBreathEmphasis);
  const float rotationBreathMultiplier = kMinimumRotationSpeedMultiplier +
      emphasizedBreath * (kMaximumRotationSpeedMultiplier -
                           kMinimumRotationSpeedMultiplier);
  rotationAngleRadians_ += rotationDirection_ * TWO_PI *
      elapsedSeconds * rotationBreathMultiplier / rotationPeriodSeconds_;
  if (rotationAngleRadians_ >= TWO_PI) {
    rotationAngleRadians_ -= TWO_PI;
  } else if (rotationAngleRadians_ < 0.0f) {
    rotationAngleRadians_ += TWO_PI;
  }
  const float transitionBrightness = updateRestartTransition(nowMs);

  renderDitheredCells(elapsedSeconds, transitionBrightness, nowMs);
  canvas_.pushSprite(0, 0);

  slowestFrameTimeUs_ = max(slowestFrameTimeUs_, micros() - frameStartedUs);
  ++renderedFrameCount_;
  updateFrameRateMeasurement(nowMs);
}

void CellularPrototype::updateFrameRateMeasurement(uint32_t nowMs) {
  ++framesInMeasurementWindow_;
  const uint32_t measurementDurationMs = nowMs - measurementWindowStartedMs_;
  if (measurementDurationMs >= 1000U) {
    measuredFramesPerSecond_ =
        framesInMeasurementWindow_ * 1000.0f / measurementDurationMs;
    framesInMeasurementWindow_ = 0;
    measurementWindowStartedMs_ = nowMs;
  }
}
