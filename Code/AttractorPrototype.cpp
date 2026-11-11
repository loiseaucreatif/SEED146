#include "AttractorPrototype.h"

#include <math.h>

namespace {

// Curated Peter de Jong coefficient sets avoid the many random combinations
// that collapse to a point or a short loop. A small seed-dependent variation
// is added later, so each deterministic seed still has its own composition.
struct ReferenceCoefficients {
  float a;
  float b;
  float c;
  float d;
};

constexpr ReferenceCoefficients kReferenceFamilies[] = {
    {1.400f, -2.300f, 2.400f, -2.100f},
    {-2.000f, -2.000f, -1.200f, 2.000f},
    {1.641f, 1.902f, 0.316f, 1.525f},
    {-0.827f, -1.637f, 1.659f, -0.943f},
    {-2.240f, 0.430f, -0.650f, -2.430f},
    {2.010f, -2.530f, 1.610f, -0.330f},
};

constexpr const char* kFamilyNames[] = {
    "DE JONG I",
    "DE JONG II",
    "DE JONG III",
    "DE JONG IV",
    "DE JONG V",
    "DE JONG VI",
};

constexpr uint8_t kFamilyCount =
    sizeof(kReferenceFamilies) / sizeof(kReferenceFamilies[0]);

uint16_t swapBytes16(uint16_t value) {
  return static_cast<uint16_t>((value << 8U) | (value >> 8U));
}

uint32_t monochromeColor(uint8_t grayLevel) {
  return (static_cast<uint32_t>(grayLevel) << 16U) |
         (static_cast<uint32_t>(grayLevel) << 8U) |
         static_cast<uint32_t>(grayLevel);
}

uint32_t mixDeterministicBits(uint32_t value) {
  // Integer avalanche hash: nearby point indices produce unrelated values,
  // but the result remains identical after every reboot for the same seed.
  value ^= value >> 16U;
  value *= 0x7FEB352DUL;
  value ^= value >> 15U;
  value *= 0x846CA68BUL;
  value ^= value >> 16U;
  return value;
}

float hashToSignedUnitFloat(uint32_t value) {
  const uint32_t hashed = mixDeterministicBits(value);
  const float unitValue =
      static_cast<float>(hashed >> 8U) / 16777216.0f;
  return unitValue * 2.0f - 1.0f;
}

}  // namespace

AttractorPrototype::AttractorPrototype(lgfx::LovyanGFX* targetDisplay)
    : targetDisplay_(targetDisplay), canvas_(targetDisplay) {}

bool AttractorPrototype::begin(uint32_t seed) {
  canvas_.setColorDepth(16);
  canvas_.setPsram(true);
  if (canvas_.createSprite(kCanvasWidth, kCanvasHeight) == nullptr) {
    ready_ = false;
    return false;
  }

  ready_ = true;
  initializeSeededComposition(seed);
  return true;
}

bool AttractorPrototype::regenerate(uint32_t seed) {
  if (!ready_) {
    return false;
  }
  initializeSeededComposition(seed);
  return true;
}

void AttractorPrototype::showCurrentFrame() {
  if (!ready_) {
    return;
  }
  canvas_.pushSprite(0, 0);
  lastFrameMs_ = millis();
  framesInMeasurementWindow_ = 0;
  measurementWindowStartedMs_ = lastFrameMs_;
  measuredFramesPerSecond_ = 0.0f;
}

void AttractorPrototype::update(uint32_t nowMs) {
  if (!ready_) {
    return;
  }

  const uint32_t elapsedMs = nowMs - lastFrameMs_;
  if (elapsedMs < kTargetFramePeriodMs) {
    return;
  }
  lastFrameMs_ = nowMs;

  const float elapsedSeconds = min(elapsedMs, 120UL) / 1000.0f;
  const uint32_t frameStartedUs = micros();

  // Limit coefficient motion so old and new point clouds cross-fade rather
  // than the attractor snapping to a different mathematical system.
  const float chaosDifference = targetChaos_ - currentChaos_;
  const float maximumChaosChange = kChaosResponsePerSecond * elapsedSeconds;
  if (fabsf(chaosDifference) <= maximumChaosChange) {
    currentChaos_ = targetChaos_;
  } else {
    currentChaos_ += chaosDifference > 0.0f
                         ? maximumChaosChange
                         : -maximumChaosChange;
  }

  morphPhaseRadians_ += kMorphRadiansPerSecond * elapsedSeconds;
  if (morphPhaseRadians_ > TWO_PI) {
    morphPhaseRadians_ -= TWO_PI;
  }
  rotationElapsedSeconds_ += elapsedSeconds;
  globalRotationRadians_ +=
      kGlobalRotationRadiansPerSecond * elapsedSeconds;
  if (globalRotationRadians_ > TWO_PI) {
    globalRotationRadians_ -= TWO_PI;
  }

  fadePartOfCanvas();
  targetDisplay_->startWrite();
  drawDepthPoints(elapsedSeconds, true, true);
  pushFadedRowsToDisplay();
  targetDisplay_->endWrite();

  const uint32_t frameTimeUs = micros() - frameStartedUs;
  slowestFrameTimeUs_ = max(slowestFrameTimeUs_, frameTimeUs);
  ++renderedFrameCount_;
  ++framesInMeasurementWindow_;
  updateFrameRateMeasurement(nowMs);
}

void AttractorPrototype::setChaos(float normalizedValue) {
  targetChaos_ = constrain(normalizedValue, 0.0f, 1.0f);
}

float AttractorPrototype::chaos() const { return targetChaos_; }

float AttractorPrototype::sectorEnergy(uint8_t sectorIndex) const {
  return sectorIndex < kSectorCount ? sectorEnergy_[sectorIndex] : 0.0f;
}

float AttractorPrototype::edgeSparkle(uint8_t sectorIndex) const {
  return sectorIndex < kSectorCount ? edgeSparkle_[sectorIndex] : 0.0f;
}

bool AttractorPrototype::isReady() const { return ready_; }
uint32_t AttractorPrototype::seed() const { return seed_; }
const char* AttractorPrototype::familyName() const {
  return familyIndex_ < kFamilyCount ? kFamilyNames[familyIndex_] : "DE JONG";
}
uint32_t AttractorPrototype::renderedFrameCount() const {
  return renderedFrameCount_;
}
float AttractorPrototype::measuredFramesPerSecond() const {
  return measuredFramesPerSecond_;
}
uint32_t AttractorPrototype::slowestFrameTimeUs() const {
  return slowestFrameTimeUs_;
}
size_t AttractorPrototype::canvasSizeBytes() const {
  return static_cast<size_t>(kCanvasWidth) * kCanvasHeight * sizeof(uint16_t);
}

uint32_t AttractorPrototype::nextRandomNumber() {
  uint32_t value = randomState_;
  value ^= value << 13U;
  value ^= value >> 17U;
  value ^= value << 5U;
  randomState_ = value;
  return value;
}

float AttractorPrototype::nextRandomUnitFloat() {
  return static_cast<float>(nextRandomNumber() >> 8U) / 16777216.0f;
}

void AttractorPrototype::initializeSeededComposition(uint32_t seed) {
  seed_ = seed;
  randomState_ = seed == 0 ? 0x6D2B79F5UL : seed;
  familyIndex_ = static_cast<uint8_t>(nextRandomNumber() % kFamilyCount);
  const ReferenceCoefficients& reference = kReferenceFamilies[familyIndex_];
  baseCoefficients_ = {reference.a, reference.b, reference.c, reference.d};

  // Keep seed variation restrained around a proven family. The independent
  // CHAOS direction then defines a repeatable continuous path through nearby
  // coefficient space instead of sampling unrelated values on every frame.
  baseCoefficients_.a += (nextRandomUnitFloat() - 0.5f) * 0.18f;
  baseCoefficients_.b += (nextRandomUnitFloat() - 0.5f) * 0.18f;
  baseCoefficients_.c += (nextRandomUnitFloat() - 0.5f) * 0.18f;
  baseCoefficients_.d += (nextRandomUnitFloat() - 0.5f) * 0.18f;
  coefficientDirection_.a = (nextRandomUnitFloat() - 0.5f) * 0.90f;
  coefficientDirection_.b = (nextRandomUnitFloat() - 0.5f) * 0.90f;
  coefficientDirection_.c = (nextRandomUnitFloat() - 0.5f) * 0.90f;
  coefficientDirection_.d = (nextRandomUnitFloat() - 0.5f) * 0.90f;

  // A non-uniform, seed-selected camera separates ATTRACTOR from the circular
  // grammar of FLOW and RADIAL. Horizontal/vertical scale, shear, orientation,
  // and off-center framing deliberately expose only a fragment of the system.
  // Model coordinates are normalized to fill -1..1 on both axes below. These
  // screen scales then guarantee broad coverage without relying on one
  // coefficient family's naturally larger numerical range.
  scaleXPixelsPerUnit_ = 168.0f + nextRandomUnitFloat() * 20.0f;
  scaleYPixelsPerUnit_ = 168.0f + nextRandomUnitFloat() * 20.0f;
  shearXFromY_ = (nextRandomUnitFloat() - 0.5f) * 0.70f;
  viewportCenterX_ = 172.0f + nextRandomUnitFloat() * 16.0f;
  viewportCenterY_ = 172.0f + nextRandomUnitFloat() * 16.0f;
  fixedOrientationRadians_ = nextRandomUnitFloat() * TWO_PI;

  // A seed chooses one axis or any combination of X/Y/Z. Axis angles use
  // bounded nonlinear rocking curves rather than a constant-speed turn.
  const uint8_t rotationAxisMask =
      static_cast<uint8_t>(1U + nextRandomNumber() % 7U);
  rotationAmplitudeX_ = (rotationAxisMask & 0x01U)
                            ? 0.28f + nextRandomUnitFloat() * 0.52f
                            : 0.0f;
  rotationAmplitudeY_ = (rotationAxisMask & 0x02U)
                            ? 0.28f + nextRandomUnitFloat() * 0.52f
                            : 0.0f;
  rotationAmplitudeZ_ = (rotationAxisMask & 0x04U)
                            ? 0.18f + nextRandomUnitFloat() * 0.42f
                            : 0.0f;
  rotationSpeedX_ = 0.16f + nextRandomUnitFloat() * 0.21f;
  rotationSpeedY_ = 0.13f + nextRandomUnitFloat() * 0.23f;
  rotationSpeedZ_ = 0.11f + nextRandomUnitFloat() * 0.19f;
  rotationPhaseX_ = nextRandomUnitFloat() * TWO_PI;
  rotationPhaseY_ = nextRandomUnitFloat() * TWO_PI;
  rotationPhaseZ_ = nextRandomUnitFloat() * TWO_PI;
  modelDepthPhase_ = nextRandomUnitFloat() * TWO_PI;
  rotationElapsedSeconds_ = 0.0f;
  globalRotationRadians_ = 0.0f;

  // Four phase-shifted coefficient movements animate the mathematical form
  // itself. Their amplitudes remain small enough to preserve family identity.
  coefficientBreathing_.a = 0.07f + nextRandomUnitFloat() * 0.09f;
  coefficientBreathing_.b = 0.07f + nextRandomUnitFloat() * 0.09f;
  coefficientBreathing_.c = 0.07f + nextRandomUnitFloat() * 0.09f;
  coefficientBreathing_.d = 0.07f + nextRandomUnitFloat() * 0.09f;
  morphPhaseRadians_ = 0.0f;
  fadePhase_ = 0;
  currentChaos_ = targetChaos_;
  for (float& energy : sectorEnergy_) {
    energy = 0.0f;
  }
  for (float& sparkle : edgeSparkle_) {
    sparkle = 0.0f;
  }
  for (bool& wasNearEdge : pointWasNearScreenEdge_) {
    wasNearEdge = false;
  }

  canvas_.fillScreen(TFT_BLACK);
  resetOrbit();
  const Coefficients coefficients = currentCoefficients();
  for (uint16_t iteration = 0; iteration < kWarmupIterations; ++iteration) {
    advanceOrbit(coefficients);
  }
  initializeDepthPoints(coefficients);
  drawDepthPoints(0.0f, false, false);
  canvas_.pushSprite(0, 0);

  lastFrameMs_ = millis();
  renderedFrameCount_ = 0;
  slowestFrameTimeUs_ = 0;
  framesInMeasurementWindow_ = 0;
  measurementWindowStartedMs_ = lastFrameMs_;
  measuredFramesPerSecond_ = 0.0f;
}

AttractorPrototype::Coefficients AttractorPrototype::currentCoefficients()
    const {
  // The midpoint maps to zero offset. Both endpoints remain within a small
  // neighborhood of the seed-selected reference attractor.
  const float signedChaosOffset = (currentChaos_ - 0.5f) * 2.0f;
  return {
      baseCoefficients_.a + coefficientDirection_.a * signedChaosOffset +
          coefficientBreathing_.a * sinf(morphPhaseRadians_),
      baseCoefficients_.b + coefficientDirection_.b * signedChaosOffset +
          coefficientBreathing_.b * sinf(morphPhaseRadians_ + HALF_PI),
      baseCoefficients_.c + coefficientDirection_.c * signedChaosOffset +
          coefficientBreathing_.c * sinf(morphPhaseRadians_ + PI),
      baseCoefficients_.d + coefficientDirection_.d * signedChaosOffset +
          coefficientBreathing_.d * sinf(morphPhaseRadians_ + PI + HALF_PI),
  };
}

void AttractorPrototype::resetOrbit() {
  // Avoid the exact origin: for some coefficient families it can fall onto a
  // short symmetric cycle before the strange orbit has formed.
  orbitX_ = 0.04f + nextRandomUnitFloat() * 0.12f;
  orbitY_ = 0.04f + nextRandomUnitFloat() * 0.12f;
}

void AttractorPrototype::advanceOrbit(const Coefficients& coefficients) {
  const float nextX = sinf(coefficients.a * orbitY_) -
                      cosf(coefficients.b * orbitX_);
  const float nextY = sinf(coefficients.c * orbitX_) -
                      cosf(coefficients.d * orbitY_);
  orbitX_ = nextX;
  orbitY_ = nextY;
}

void AttractorPrototype::initializeDepthPoints(
    const Coefficients& coefficients) {
  // The multiplier is coprime with 900, so it visits every depth slot once in
  // a scrambled order. Nearby attractor samples do not form synchronized Z
  // sheets, and point recycling is evenly distributed from the first frame.
  constexpr uint16_t kDepthPermutationMultiplier = 367;
  for (uint16_t pointIndex = 0;
       pointIndex < kDepthPointCount;
       ++pointIndex) {
    advanceOrbit(coefficients);
    DepthPoint& point = depthPoints_[pointIndex];
    point.modelX = orbitX_;
    point.modelY = orbitY_;
    point.modelZ = 0.0f;
    const uint16_t depthSlot =
        (pointIndex * kDepthPermutationMultiplier) % kDepthPointCount;
    point.depth =
        (depthSlot + nextRandomUnitFloat()) / kDepthPointCount;
  }
  normalizeDepthPointModelCoordinates();
}

void AttractorPrototype::normalizeDepthPointModelCoordinates() {
  float minimumX = depthPoints_[0].modelX;
  float maximumX = depthPoints_[0].modelX;
  float minimumY = depthPoints_[0].modelY;
  float maximumY = depthPoints_[0].modelY;
  for (const DepthPoint& point : depthPoints_) {
    minimumX = min(minimumX, point.modelX);
    maximumX = max(maximumX, point.modelX);
    minimumY = min(minimumY, point.modelY);
    maximumY = max(maximumY, point.modelY);
  }

  sourceCenterX_ = (minimumX + maximumX) * 0.5f;
  sourceCenterY_ = (minimumY + maximumY) * 0.5f;
  const float halfWidth = max((maximumX - minimumX) * 0.5f, 0.10f);
  const float halfHeight = max((maximumY - minimumY) * 0.5f, 0.10f);
  sourceNormalizationX_ = 1.0f / halfWidth;
  sourceNormalizationY_ = 1.0f / halfHeight;

  for (DepthPoint& point : depthPoints_) {
    point.modelX = (point.modelX - sourceCenterX_) * sourceNormalizationX_;
    point.modelY = (point.modelY - sourceCenterY_) * sourceNormalizationY_;
  }

  // Find a real, densely populated part of the attractor rather than assuming
  // its bounding-box midpoint contains artwork. Centering this nucleus removes
  // seeds whose ring-like geometry would otherwise leave a black central hole.
  constexpr float kDenseNeighborRadiusSquared = 0.15f * 0.15f;
  uint16_t densestPointIndex = 0;
  uint16_t densestNeighborCount = 0;
  for (uint16_t candidateIndex = 0;
       candidateIndex < kDepthPointCount;
       candidateIndex += 3U) {
    const uint32_t pathHash = mixDeterministicBits(
        seed_ ^ (static_cast<uint32_t>(candidateIndex) * 0x9E3779B9UL));
    // Anchor to a structurally stable particle, not one assigned a later
    // curved or disordered trajectory.
    if (pathHash % 100U >= 55U) {
      continue;
    }
    uint16_t neighborCount = 0;
    const DepthPoint& candidate = depthPoints_[candidateIndex];
    for (const DepthPoint& neighbor : depthPoints_) {
      const float differenceX = neighbor.modelX - candidate.modelX;
      const float differenceY = neighbor.modelY - candidate.modelY;
      if (differenceX * differenceX + differenceY * differenceY <=
          kDenseNeighborRadiusSquared) {
        ++neighborCount;
      }
    }
    if (neighborCount > densestNeighborCount) {
      densestNeighborCount = neighborCount;
      densestPointIndex = candidateIndex;
    }
  }

  modelAnchorX_ = depthPoints_[densestPointIndex].modelX;
  modelAnchorY_ = depthPoints_[densestPointIndex].modelY;
  float maximumAbsoluteX = 0.10f;
  float maximumAbsoluteY = 0.10f;
  for (DepthPoint& point : depthPoints_) {
    point.modelX -= modelAnchorX_;
    point.modelY -= modelAnchorY_;
    maximumAbsoluteX = max(maximumAbsoluteX, fabsf(point.modelX));
    maximumAbsoluteY = max(maximumAbsoluteY, fabsf(point.modelY));
  }
  modelPostScaleX_ = 1.0f / maximumAbsoluteX;
  modelPostScaleY_ = 1.0f / maximumAbsoluteY;
  modelZAtOrigin_ =
      sinf(modelDepthPhase_) * 0.52f +
      sinf(-modelDepthPhase_) * 0.23f;

  for (DepthPoint& point : depthPoints_) {
    point.modelX *= modelPostScaleX_;
    point.modelY *= modelPostScaleY_;
    // Two non-matching waves turn the flat attractor into a folded volume.
    // Subtracting the origin value keeps the selected dense nucleus fixed at
    // the true 3D rotation center rather than only at screen-space X/Y zero.
    point.modelZ =
        sinf(point.modelX * 2.3f + point.modelY * 1.7f + modelDepthPhase_) *
            0.52f +
        sinf(point.modelX * 4.1f - point.modelY * 2.8f - modelDepthPhase_) *
            0.23f - modelZAtOrigin_;
  }
}

void AttractorPrototype::assignRecycledPointModelPosition(
    DepthPoint& point,
    const Coefficients& coefficients) {
  for (uint8_t orbitStep = 0; orbitStep < 3; ++orbitStep) {
    advanceOrbit(coefficients);
  }
  const float normalizedX =
      (orbitX_ - sourceCenterX_) * sourceNormalizationX_;
  const float normalizedY =
      (orbitY_ - sourceCenterY_) * sourceNormalizationY_;
  point.modelX = (normalizedX - modelAnchorX_) * modelPostScaleX_;
  point.modelY = (normalizedY - modelAnchorY_) * modelPostScaleY_;
  point.modelZ =
      sinf(point.modelX * 2.3f + point.modelY * 1.7f + modelDepthPhase_) *
          0.52f +
      sinf(point.modelX * 4.1f - point.modelY * 2.8f - modelDepthPhase_) *
          0.23f - modelZAtOrigin_;
}

void AttractorPrototype::drawDepthPoints(float elapsedSeconds,
                                         bool collectSectorEnergy,
                                         bool drawDirectlyToDisplay) {
  const Coefficients coefficients = currentCoefficients();
  // Each axis combines two incommensurate sine waves. Direction and angular
  // speed therefore accelerate, slow, and reverse without a mechanical loop.
  const float rotationTimeX =
      rotationElapsedSeconds_ * rotationSpeedX_ + rotationPhaseX_;
  const float rotationTimeY =
      rotationElapsedSeconds_ * rotationSpeedY_ + rotationPhaseY_;
  const float rotationTimeZ =
      rotationElapsedSeconds_ * rotationSpeedZ_ + rotationPhaseZ_;
  const float rotationAngleX = rotationAmplitudeX_ *
      (sinf(rotationTimeX) + 0.32f * sinf(rotationTimeX * 1.73f + 0.4f));
  const float rotationAngleY = rotationAmplitudeY_ *
      (sinf(rotationTimeY) + 0.29f * sinf(rotationTimeY * 2.11f + 1.1f));
  const float rotationAngleZ = fixedOrientationRadians_ +
      globalRotationRadians_ +
      rotationAmplitudeZ_ *
          (sinf(rotationTimeZ) + 0.35f * sinf(rotationTimeZ * 1.61f + 2.0f));
  const float cosineX = cosf(rotationAngleX);
  const float sineX = sinf(rotationAngleX);
  const float cosineY = cosf(rotationAngleY);
  const float sineY = sinf(rotationAngleY);
  const float cosineZ = cosf(rotationAngleZ);
  const float sineZ = sinf(rotationAngleZ);
  constexpr float kFarPerspectiveScale = 0.32f;
  constexpr float kPerspectiveScaleRange = 2.20f;
  constexpr float kFarFadeEnd = 0.07f;
  constexpr float kNearFadeStart = 0.94f;
  uint16_t sectorHits[kSectorCount] = {};
  uint16_t visiblePointCount = 0;

  // Edge energy belongs to the artwork renderer rather than the LED code, so
  // it remains tied to real geometry even if the LED presentation changes.
  const float sparkleDecay = max(
      0.0f, 1.0f - kEdgeSparkleDecayPerSecond * elapsedSeconds);
  for (float& sparkle : edgeSparkle_) {
    sparkle *= sparkleDecay;
    if (sparkle < 0.003f) {
      sparkle = 0.0f;
    }
  }

  for (uint16_t pointIndex = 0;
       pointIndex < kDepthPointCount;
       ++pointIndex) {
    DepthPoint& point = depthPoints_[pointIndex];
    point.depth += kForwardDepthPerSecond * elapsedSeconds;
    if (point.depth >= 1.0f) {
      point.depth -= 1.0f;
      // Only the point that just passed the viewer receives a new position on
      // the current attractor. CHAOS and ambient coefficient changes therefore
      // enter progressively, without rebuilding or fading the complete cloud.
      assignRecycledPointModelPosition(point, coefficients);
    }

    float movingModelX = point.modelX;
    float movingModelY = point.modelY;
    float movingModelZ = point.modelZ;

    // Most particles preserve the mathematical volume. About 30% follow a
    // smooth curved route and 15% use seed-driven interpolated disorder.
    // Nothing calls random() here: every apparently random path is repeatable.
    const uint32_t pathHash = mixDeterministicBits(
        seed_ ^ (static_cast<uint32_t>(pointIndex) * 0x9E3779B9UL));
    const uint8_t trajectoryClass = pathHash % 100U;
    if (trajectoryClass >= 55U && trajectoryClass < 85U) {
      const float pathPhase =
          ((pathHash >> 8U) & 0xFFU) / 255.0f * TWO_PI;
      const float pathFrequency =
          1.2f + ((pathHash >> 16U) & 0x07U) * 0.24f;
      const float pathAmplitude =
          0.055f + ((pathHash >> 24U) & 0xFFU) / 255.0f * 0.105f;
      movingModelX += sinf(
          point.depth * TWO_PI * pathFrequency + pathPhase) * pathAmplitude;
      movingModelY += cosf(
          point.depth * TWO_PI * (pathFrequency * 0.73f) - pathPhase) *
          pathAmplitude;
      movingModelZ += sinf(
          point.depth * TWO_PI * (pathFrequency * 0.47f) + pathPhase) *
          pathAmplitude * 1.4f;
    } else if (trajectoryClass >= 85U) {
      constexpr uint8_t kDisorderSegmentCount = 7;
      const float disorderPosition =
          point.depth * kDisorderSegmentCount;
      const uint8_t disorderSegment =
          min(static_cast<uint8_t>(disorderPosition),
              static_cast<uint8_t>(kDisorderSegmentCount - 1U));
      const float segmentFraction = disorderPosition - disorderSegment;
      const float smoothFraction = segmentFraction * segmentFraction *
                                   (3.0f - 2.0f * segmentFraction);
      const uint32_t pointPathKey =
          seed_ ^ (static_cast<uint32_t>(pointIndex) * 0x85EBCA6BUL);
      const uint32_t segmentKey = pointPathKey ^
          (static_cast<uint32_t>(disorderSegment) * 0xC2B2AE35UL);
      const uint32_t nextSegmentKey = pointPathKey ^
          (static_cast<uint32_t>(disorderSegment + 1U) * 0xC2B2AE35UL);
      const float noiseX =
          hashToSignedUnitFloat(segmentKey) * (1.0f - smoothFraction) +
          hashToSignedUnitFloat(nextSegmentKey) * smoothFraction;
      const float noiseY =
          hashToSignedUnitFloat(segmentKey ^ 0xA511E9B3UL) *
              (1.0f - smoothFraction) +
          hashToSignedUnitFloat(nextSegmentKey ^ 0xA511E9B3UL) *
              smoothFraction;
      const float noiseZ =
          hashToSignedUnitFloat(segmentKey ^ 0x63D83595UL) *
              (1.0f - smoothFraction) +
          hashToSignedUnitFloat(nextSegmentKey ^ 0x63D83595UL) *
              smoothFraction;
      movingModelX += noiseX * 0.18f;
      movingModelY += noiseY * 0.18f;
      movingModelZ += noiseZ * 0.22f;
    }

    // Rotate the complete folded volume around X, then Y, then Z. This is a
    // true 3D model transform; forward travel remains the separate depth axis.
    const float afterXRotationY =
        movingModelY * cosineX - movingModelZ * sineX;
    const float afterXRotationZ =
        movingModelY * sineX + movingModelZ * cosineX;
    const float afterYRotationX =
        movingModelX * cosineY + afterXRotationZ * sineY;
    const float afterYRotationZ =
        -movingModelX * sineY + afterXRotationZ * cosineY;
    const float rotatedX =
        afterYRotationX * cosineZ - afterXRotationY * sineZ;
    const float rotatedY =
        afterYRotationX * sineZ + afterXRotationY * cosineZ;
    const float shearedX = rotatedX + rotatedY * shearXFromY_;

    // Squared depth creates perspective acceleration: a point moves slowly in
    // the distance, then expands rapidly as it passes the observer.
    const float perspectiveScale =
        kFarPerspectiveScale +
        point.depth * point.depth * kPerspectiveScaleRange;
    const float modelDepthScale = constrain(
        1.0f + afterYRotationZ * 0.12f, 0.82f, 1.18f);

    // Only individual points fade at the extreme far/near planes. Starting
    // depths are distributed continuously, so no global brightness event or
    // periodic seam can occur.
    float depthWeight = 1.0f;
    if (point.depth < kFarFadeEnd) {
      const float fadeProgress = point.depth / kFarFadeEnd;
      depthWeight = fadeProgress * fadeProgress *
                    (3.0f - 2.0f * fadeProgress);
    } else if (point.depth > kNearFadeStart) {
      const float fadeProgress =
          (1.0f - point.depth) / (1.0f - kNearFadeStart);
      depthWeight = fadeProgress * fadeProgress *
                    (3.0f - 2.0f * fadeProgress);
    }

    uint8_t grayLevel = 102;
    if ((pointIndex + seed_) % 43U == 0U) {
      grayLevel = 220;
    } else if ((pointIndex + seed_) % 7U == 0U) {
      grayLevel = 150;
    }
    // Distance also controls brightness, reinforcing depth without color.
    const float distanceBrightness = 0.35f + point.depth * 0.65f;
    const uint8_t depthGrayLevel = static_cast<uint8_t>(
        grayLevel * depthWeight * distanceBrightness + 0.5f);
    if (depthGrayLevel < 8U) {
      continue;
    }

    const float pixelOffsetX =
        shearedX * scaleXPixelsPerUnit_ * perspectiveScale * modelDepthScale;
    const float pixelOffsetY =
        rotatedY * scaleYPixelsPerUnit_ * perspectiveScale * modelDepthScale;
    const int16_t pixelX = static_cast<int16_t>(
        viewportCenterX_ + pixelOffsetX + 0.5f);
    const int16_t pixelY = static_cast<int16_t>(
        viewportCenterY_ + pixelOffsetY + 0.5f);

    const float screenOffsetX = pixelX - kCanvasCenter;
    const float screenOffsetY = pixelY - kCanvasCenter;
    const float screenRadiusSquared =
        screenOffsetX * screenOffsetX + screenOffsetY * screenOffsetY;
    const bool pointIsNearScreenEdge =
        screenRadiusSquared >=
            kEdgeDetectionRadius * kEdgeDetectionRadius &&
        screenRadiusSquared <=
            kEdgeDetectionOuterRadius * kEdgeDetectionOuterRadius;
    // Use the same rare bright tracers already present in the artwork. This
    // produces readable individual glints instead of saturating all LEDs from
    // the hundreds of ordinary particles crossing the edge every second.
    const bool pointIsSparkleTracer =
        ((pointIndex + seed_) % 43U) == 0U;
    if (pointIsNearScreenEdge &&
        !pointWasNearScreenEdge_[pointIndex] &&
        pointIsSparkleTracer) {
      float edgeAngle = atan2f(screenOffsetY, screenOffsetX);
      if (edgeAngle < 0.0f) {
        edgeAngle += TWO_PI;
      }
      const uint8_t edgeSector = static_cast<uint8_t>(
          edgeAngle * kSectorCount / TWO_PI) % kSectorCount;
      edgeSparkle_[edgeSector] = 1.0f;
    }
    pointWasNearScreenEdge_[pointIndex] = pointIsNearScreenEdge;

    if (pixelX < 1 || pixelX >= kCanvasWidth - 1 ||
        pixelY < 1 || pixelY >= kCanvasHeight - 1) {
      continue;
    }

    // Perspective must change apparent size as well as position. Discrete
    // pixel footprints remain crisp on the 360 px panel while making nearby
    // particles unmistakably pass the observer instead of only spreading out.
    uint8_t particleSizePixels = 1;
    if (point.depth >= 0.95f) {
      particleSizePixels = 5;
    } else if (point.depth >= 0.86f) {
      particleSizePixels = 4;
    } else if (point.depth >= 0.72f) {
      particleSizePixels = 3;
    } else if (point.depth >= 0.52f) {
      particleSizePixels = 2;
    }
    const int16_t particleLeft =
        pixelX - static_cast<int16_t>(particleSizePixels / 2U);
    const int16_t particleTop =
        pixelY - static_cast<int16_t>(particleSizePixels / 2U);
    const uint32_t pointColor = monochromeColor(depthGrayLevel);
    canvas_.fillRect(particleLeft,
                     particleTop,
                     particleSizePixels,
                     particleSizePixels,
                     pointColor);
    if (drawDirectlyToDisplay) {
      targetDisplay_->fillRect(particleLeft,
                               particleTop,
                               particleSizePixels,
                               particleSizePixels,
                               pointColor);
    }
    ++visiblePointCount;

    if (collectSectorEnergy) {
      float pointAngle = atan2f(screenOffsetY, screenOffsetX);
      if (pointAngle < 0.0f) {
        pointAngle += TWO_PI;
      }
      const uint8_t sectorIndex = static_cast<uint8_t>(
          pointAngle * kSectorCount / TWO_PI) % kSectorCount;
      ++sectorHits[sectorIndex];
    }
  }

  if (!collectSectorEnergy || visiblePointCount == 0) {
    return;
  }

  uint16_t maximumSectorHits = 1;
  for (uint8_t sectorIndex = 0; sectorIndex < kSectorCount; ++sectorIndex) {
    maximumSectorHits = max(maximumSectorHits, sectorHits[sectorIndex]);
  }
  for (uint8_t sectorIndex = 0; sectorIndex < kSectorCount; ++sectorIndex) {
    const float targetEnergy =
        sectorHits[sectorIndex] / static_cast<float>(maximumSectorHits);
    sectorEnergy_[sectorIndex] +=
        (targetEnergy - sectorEnergy_[sectorIndex]) * 0.10f;
  }
}

void AttractorPrototype::fadePartOfCanvas() {
  uint16_t* storedPixels = static_cast<uint16_t*>(canvas_.getBuffer());
  if (storedPixels == nullptr) {
    return;
  }

  const uint16_t firstPermutationIndex = fadePhase_ * kFadeRowsPerFrame;
  for (uint16_t rowSlot = 0; rowSlot < kFadeRowsPerFrame; ++rowSlot) {
    const uint16_t permutationIndex = firstPermutationIndex + rowSlot;
    const uint16_t y =
        (permutationIndex * kFadeRowPermutationMultiplier) % kCanvasHeight;
    fadedRows_[rowSlot] = y;
    uint16_t* row = storedPixels + static_cast<size_t>(y) * kCanvasWidth;
    for (uint16_t x = 0; x < kCanvasWidth; ++x) {
      const uint16_t rgb565 = swapBytes16(row[x]);
      const uint16_t red =
          ((rgb565 >> 11U) & 0x1FU) * kFadeMultiplier >> 8U;
      const uint16_t green =
          ((rgb565 >> 5U) & 0x3FU) * kFadeMultiplier >> 8U;
      const uint16_t blue =
          (rgb565 & 0x1FU) * kFadeMultiplier >> 8U;
      row[x] = swapBytes16(static_cast<uint16_t>(
          (red << 11U) | (green << 5U) | blue));
    }
  }
  fadePhase_ = (fadePhase_ + 1U) % kFadePhaseCount;
}

void AttractorPrototype::pushFadedRowsToDisplay() {
  const uint16_t* storedPixels =
      static_cast<const uint16_t*>(canvas_.getBuffer());
  if (storedPixels == nullptr) {
    return;
  }

  // Only rows changed by this frame's dispersed fade need retransmission.
  // Each row already contains any newly drawn points, so sending it after the
  // direct point writes cannot erase fresh artwork.
  for (uint16_t rowSlot = 0; rowSlot < kFadeRowsPerFrame; ++rowSlot) {
    const uint16_t y = fadedRows_[rowSlot];
    const uint16_t* firstRowPixel =
        storedPixels + static_cast<size_t>(y) * kCanvasWidth;
    targetDisplay_->pushImage(
        0,
        y,
        kCanvasWidth,
        1,
        reinterpret_cast<const lgfx::swap565_t*>(firstRowPixel));
  }
}

void AttractorPrototype::updateFrameRateMeasurement(uint32_t nowMs) {
  const uint32_t measurementDurationMs = nowMs - measurementWindowStartedMs_;
  if (measurementDurationMs < 1000UL) {
    return;
  }
  measuredFramesPerSecond_ =
      framesInMeasurementWindow_ * 1000.0f / measurementDurationMs;
  framesInMeasurementWindow_ = 0;
  measurementWindowStartedMs_ = nowMs;
}
