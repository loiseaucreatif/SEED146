#include "RadialPrototype.h"

#include <math.h>

namespace {

uint16_t swapBytes16(uint16_t value) {
  return static_cast<uint16_t>((value << 8U) | (value >> 8U));
}

// LovyanGFX drawing primitives accept 0xRRGGBB even when the destination is a
// 16-bit sprite. Supplying packed RGB565 here would be interpreted as a blue/
// green RGB888 value and would break RADIAL's monochrome screen identity.
uint32_t monochromeColor(uint8_t grayLevel) {
  return (static_cast<uint32_t>(grayLevel) << 16U) |
         (static_cast<uint32_t>(grayLevel) << 8U) |
         static_cast<uint32_t>(grayLevel);
}

}  // namespace

RadialPrototype::RadialPrototype(lgfx::LovyanGFX* targetDisplay)
    : targetDisplay_(targetDisplay), canvas_(targetDisplay) {}

bool RadialPrototype::begin(uint32_t seed) {
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

bool RadialPrototype::regenerate(uint32_t seed) {
  if (!ready_) {
    return false;
  }
  initializeSeededComposition(seed);
  return true;
}

void RadialPrototype::showCurrentFrame() {
  if (!ready_) {
    return;
  }
  canvas_.pushSprite(0, 0);
  lastFrameMs_ = millis();
  framesInMeasurementWindow_ = 0;
  measurementWindowStartedMs_ = lastFrameMs_;
  measuredFramesPerSecond_ = 0.0f;
}

void RadialPrototype::update(uint32_t nowMs) {
  if (!ready_) {
    return;
  }

  const uint32_t elapsedMs = nowMs - lastFrameMs_;
  if (elapsedMs < kTargetFramePeriodMs) {
    return;
  }
  lastFrameMs_ = nowMs;

  const float elapsedSeconds = min(elapsedMs, 100UL) / 1000.0f;
  const uint32_t frameStartedUs = micros();

  const float twistDifference = targetTwist_ - currentTwist_;
  const float maximumTwistChange =
      kTwistResponsePerSecond * elapsedSeconds;
  if (fabsf(twistDifference) <= maximumTwistChange) {
    currentTwist_ = targetTwist_;
  } else {
    currentTwist_ += twistDifference > 0.0f
                         ? maximumTwistChange
                         : -maximumTwistChange;
  }

  updateCenter(elapsedSeconds);

  // Each seed gives the lattice an independent cadence, while the moving
  // center supplies a second, slower spatial motion.
  rotationRadians_ += rotationDirection_ * rotationSpeedRadiansPerSecond_ *
                      elapsedSeconds;
  wavePhaseRadians_ += waveSpeedRadiansPerSecond_ * elapsedSeconds;
  if (fabsf(rotationRadians_) > TWO_PI) {
    rotationRadians_ += rotationRadians_ > 0.0f ? -TWO_PI : TWO_PI;
  }
  if (wavePhaseRadians_ > TWO_PI) {
    wavePhaseRadians_ -= TWO_PI;
  }

  fadePartOfCanvas();
  targetDisplay_->startWrite();
  drawPolarLattice();
  pushFadedRowsToDisplay();
  targetDisplay_->endWrite();
  updateSectorEnergy();

  const uint32_t frameTimeUs = micros() - frameStartedUs;
  slowestFrameTimeUs_ = max(slowestFrameTimeUs_, frameTimeUs);
  ++renderedFrameCount_;
  ++framesInMeasurementWindow_;
  updateFrameRateMeasurement(nowMs);
}

void RadialPrototype::setTwist(float normalizedValue) {
  targetTwist_ = constrain(normalizedValue, 0.0f, 1.0f);
}

float RadialPrototype::twist() const {
  return targetTwist_;
}

float RadialPrototype::sectorEnergy(uint8_t sectorIndex) const {
  return sectorIndex < kSectorCount ? sectorEnergy_[sectorIndex] : 0.0f;
}

bool RadialPrototype::isReady() const { return ready_; }
uint32_t RadialPrototype::seed() const { return seed_; }
uint8_t RadialPrototype::armCount() const { return armCount_; }
uint8_t RadialPrototype::symmetry() const { return symmetry_; }
const char* RadialPrototype::compositionFamilyName() const {
  switch (compositionFamily_) {
    case CompositionFamily::spiral:
      return "SPIRAL";
    case CompositionFamily::rosette:
      return "ROSETTE";
    case CompositionFamily::interference:
      return "INTERFERENCE";
  }
  return "UNKNOWN";
}
float RadialPrototype::primaryRotationRadians() const {
  return rotationRadians_ + phaseOffset_;
}
float RadialPrototype::primaryRotationDirection() const {
  return rotationDirection_;
}
float RadialPrototype::centerX() const { return centerX_; }
float RadialPrototype::centerY() const { return centerY_; }
uint32_t RadialPrototype::renderedFrameCount() const {
  return renderedFrameCount_;
}
float RadialPrototype::measuredFramesPerSecond() const {
  return measuredFramesPerSecond_;
}
uint32_t RadialPrototype::slowestFrameTimeUs() const {
  return slowestFrameTimeUs_;
}
size_t RadialPrototype::canvasSizeBytes() const {
  return static_cast<size_t>(kCanvasWidth) * kCanvasHeight * sizeof(uint16_t);
}

uint32_t RadialPrototype::nextRandomNumber() {
  uint32_t value = randomState_;
  value ^= value << 13U;
  value ^= value >> 17U;
  value ^= value << 5U;
  randomState_ = value;
  return value;
}

float RadialPrototype::nextRandomUnitFloat() {
  return static_cast<float>(nextRandomNumber() >> 8U) / 16777216.0f;
}

void RadialPrototype::initializeSeededComposition(uint32_t seed) {
  seed_ = seed;
  randomState_ = seed == 0 ? 0x6D2B79F5UL : seed;

  selectSeededCompositionFamily();
  initializeSeededCenterMotion(seed);
  rotationRadians_ = 0.0f;
  wavePhaseRadians_ = 0.0f;
  fadePhase_ = 0;

  canvas_.fillScreen(TFT_BLACK);
  drawPolarLattice();
  canvas_.pushSprite(0, 0);

  lastFrameMs_ = millis();
  renderedFrameCount_ = 0;
  slowestFrameTimeUs_ = 0;
  framesInMeasurementWindow_ = 0;
  measurementWindowStartedMs_ = lastFrameMs_;
  measuredFramesPerSecond_ = 0.0f;
  for (float& energy : sectorEnergy_) {
    energy = 0.0f;
  }
}

void RadialPrototype::selectSeededCompositionFamily() {
  // Seeds now choose a distinct composition family before varying its details.
  // This produces recognizably different visual systems rather than small
  // shifts of one fixed polar grid.
  compositionFamily_ = static_cast<CompositionFamily>(nextRandomNumber() % 3U);
  secondaryArmFamilyEnabled_ = false;

  switch (compositionFamily_) {
    case CompositionFamily::spiral:
      armCount_ = static_cast<uint8_t>(5U + nextRandomNumber() % 5U);
      ringCount_ = static_cast<uint8_t>(3U + nextRandomNumber() % 3U);
      symmetry_ = static_cast<uint8_t>(3U + nextRandomNumber() % 4U);
      armWaveAmplitudeRadians_ = 0.05f + nextRandomUnitFloat() * 0.09f;
      armWaveFrequency_ = 1.1f + nextRandomUnitFloat() * 1.2f;
      ringWaveAmplitudePixels_ = 3.0f + nextRandomUnitFloat() * 5.0f;
      armBrightness_ = 205.0f;
      ringBrightness_ = 52.0f;
      rotationSpeedRadiansPerSecond_ = 0.10f + nextRandomUnitFloat() * 0.12f;
      waveSpeedRadiansPerSecond_ = 0.36f + nextRandomUnitFloat() * 0.38f;
      break;

    case CompositionFamily::rosette:
      armCount_ = static_cast<uint8_t>(4U + nextRandomNumber() % 5U);
      ringCount_ = static_cast<uint8_t>(7U + nextRandomNumber() % 3U);
      symmetry_ = static_cast<uint8_t>(5U + nextRandomNumber() % 5U);
      armWaveAmplitudeRadians_ = 0.17f + nextRandomUnitFloat() * 0.16f;
      armWaveFrequency_ = 2.8f + nextRandomUnitFloat() * 1.9f;
      ringWaveAmplitudePixels_ = 10.0f + nextRandomUnitFloat() * 12.0f;
      armBrightness_ = 145.0f;
      ringBrightness_ = 112.0f;
      rotationSpeedRadiansPerSecond_ = 0.16f + nextRandomUnitFloat() * 0.14f;
      waveSpeedRadiansPerSecond_ = 0.80f + nextRandomUnitFloat() * 0.75f;
      break;

    case CompositionFamily::interference:
      armCount_ = static_cast<uint8_t>(4U + nextRandomNumber() % 4U);
      ringCount_ = static_cast<uint8_t>(4U + nextRandomNumber() % 3U);
      symmetry_ = static_cast<uint8_t>(3U + nextRandomNumber() % 6U);
      armWaveAmplitudeRadians_ = 0.13f + nextRandomUnitFloat() * 0.18f;
      armWaveFrequency_ = 1.8f + nextRandomUnitFloat() * 2.2f;
      ringWaveAmplitudePixels_ = 6.0f + nextRandomUnitFloat() * 10.0f;
      armBrightness_ = 165.0f;
      ringBrightness_ = 76.0f;
      rotationSpeedRadiansPerSecond_ = 0.12f + nextRandomUnitFloat() * 0.18f;
      waveSpeedRadiansPerSecond_ = 0.62f + nextRandomUnitFloat() * 0.86f;
      secondaryArmFamilyEnabled_ = true;
      break;
  }

  twistDirection_ = (nextRandomNumber() & 1U) == 0U ? -1.0f : 1.0f;
  rotationDirection_ = (nextRandomNumber() & 1U) == 0U ? -1.0f : 1.0f;
  phaseOffset_ = nextRandomUnitFloat() * TWO_PI;
}

void RadialPrototype::initializeSeededCenterMotion(uint32_t seed) {
  centerX_ = kCanvasCenter;
  centerY_ = kCanvasCenter;

  // Use a separate hash so changing family parameters never changes the center
  // trajectory for a previously known seed.
  const uint32_t motionHash = seed * 747796405UL + 2891336453UL;
  const float motionAngle =
      static_cast<float>(motionHash >> 8U) / 16777216.0f * TWO_PI;
  centerVelocityX_ = cosf(motionAngle) * kCenterSpeedPixelsPerSecond;
  centerVelocityY_ = sinf(motionAngle) * kCenterSpeedPixelsPerSecond;
  centerBoundaryGlideDirection_ =
      (motionHash & 1U) == 0U ? -1.0f : 1.0f;
}

void RadialPrototype::updateCenter(float elapsedSeconds) {
  float offsetX = centerX_ - kCanvasCenter;
  float offsetY = centerY_ - kCanvasCenter;
  const float distanceFromScreenCenter = sqrtf(offsetX * offsetX +
                                                offsetY * offsetY);

  if (distanceFromScreenCenter > kCenterSoftBoundaryStartRadius) {
    const float boundaryWidth = kCenterMaximumRoamRadius -
                                kCenterSoftBoundaryStartRadius;
    const float normalizedBoundaryDepth = constrain(
        (distanceFromScreenCenter - kCenterSoftBoundaryStartRadius) /
            boundaryWidth,
        0.0f,
        1.0f);
    const float smoothBoundaryDepth = normalizedBoundaryDepth *
                                      normalizedBoundaryDepth *
                                      (3.0f - 2.0f * normalizedBoundaryDepth);
    const float outwardNormalX = offsetX / distanceFromScreenCenter;
    const float outwardNormalY = offsetY / distanceFromScreenCenter;
    const float inwardVelocityChange =
        kCenterBoundarySteeringPixelsPerSecondSquared *
        smoothBoundaryDepth * elapsedSeconds;
    const float tangentialVelocityChange =
        kCenterBoundaryGlidePixelsPerSecondSquared * smoothBoundaryDepth *
        elapsedSeconds * centerBoundaryGlideDirection_;
    centerVelocityX_ -= outwardNormalX * inwardVelocityChange;
    centerVelocityY_ -= outwardNormalY * inwardVelocityChange;
    centerVelocityX_ -= outwardNormalY * tangentialVelocityChange;
    centerVelocityY_ += outwardNormalX * tangentialVelocityChange;

    const float steeredSpeed = sqrtf(centerVelocityX_ * centerVelocityX_ +
                                     centerVelocityY_ * centerVelocityY_);
    if (steeredSpeed > 0.001f) {
      centerVelocityX_ = centerVelocityX_ / steeredSpeed *
                         kCenterSpeedPixelsPerSecond;
      centerVelocityY_ = centerVelocityY_ / steeredSpeed *
                         kCenterSpeedPixelsPerSecond;
    }
  }

  centerX_ += centerVelocityX_ * elapsedSeconds;
  centerY_ += centerVelocityY_ * elapsedSeconds;

  offsetX = centerX_ - kCanvasCenter;
  offsetY = centerY_ - kCanvasCenter;
  const float updatedDistance = sqrtf(offsetX * offsetX + offsetY * offsetY);
  if (updatedDistance > kCenterMaximumRoamRadius) {
    const float outwardNormalX = offsetX / updatedDistance;
    const float outwardNormalY = offsetY / updatedDistance;
    centerX_ = kCanvasCenter + outwardNormalX * kCenterMaximumRoamRadius;
    centerY_ = kCanvasCenter + outwardNormalY * kCenterMaximumRoamRadius;
    const float outwardSpeed = centerVelocityX_ * outwardNormalX +
                                centerVelocityY_ * outwardNormalY;
    if (outwardSpeed > 0.0f) {
      centerVelocityX_ -= outwardNormalX * outwardSpeed;
      centerVelocityY_ -= outwardNormalY * outwardSpeed;
    }
  }
}

float RadialPrototype::distanceFromCenterToScreenEdge(
    float angleRadians) const {
  // Find where a ray from the moving center meets the fixed circular screen.
  // This lets every visual family still reach the circumference as its own
  // center travels, instead of clipping most of the composition off-screen.
  const float centerOffsetX = centerX_ - kCanvasCenter;
  const float centerOffsetY = centerY_ - kCanvasCenter;
  const float directionX = cosf(angleRadians);
  const float directionY = sinf(angleRadians);
  const float projection = centerOffsetX * directionX +
                           centerOffsetY * directionY;
  const float distanceSquared = centerOffsetX * centerOffsetX +
                                centerOffsetY * centerOffsetY;
  const float discriminant = projection * projection +
      kOuterRadius * kOuterRadius - distanceSquared;
  return -projection + sqrtf(max(discriminant, 0.0f));
}

void RadialPrototype::fadePartOfCanvas() {
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

void RadialPrototype::pushFadedRowsToDisplay() {
  const uint16_t* storedPixels =
      static_cast<const uint16_t*>(canvas_.getBuffer());
  if (storedPixels == nullptr) {
    return;
  }

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

void RadialPrototype::drawPolarLattice() {
  drawSpiralArms(0.0f, twistDirection_,
                  static_cast<uint8_t>(armBrightness_));
  if (secondaryArmFamilyEnabled_) {
    // INTERFERENCE overlays a second family moving against the first one.
    // The opposing curl is a structural change, not just another color layer.
    drawSpiralArms(TWO_PI / (armCount_ * 2.0f), -twistDirection_, 112);
  }
  drawOscillatingRings();
}

void RadialPrototype::drawSpiralArms(float familyAngleOffset,
                                     float familyTwistDirection,
                                     uint8_t grayLevel) {
  // Twist spans about 20 degrees to 250 degrees from center to circumference.
  // Keeping a small curl at zero prevents the calm endpoint from looking like
  // a static calibration chart.
  const float twistRadians = (0.35f + currentTwist_ * 4.0f) *
                             familyTwistDirection;
  const uint32_t armColor = monochromeColor(grayLevel);
  constexpr uint8_t kArmSegmentCount = 44;

  for (uint8_t armIndex = 0; armIndex < armCount_; ++armIndex) {
    const float armBaseAngle =
        armIndex * TWO_PI / armCount_ + rotationRadians_ + phaseOffset_ +
        familyAngleOffset;
    float previousX = centerX_;
    float previousY = centerY_;

    for (uint8_t segmentIndex = 1;
         segmentIndex <= kArmSegmentCount;
         ++segmentIndex) {
      const float normalizedRadius =
          segmentIndex / static_cast<float>(kArmSegmentCount);
      const float radialWave = sinf(
          normalizedRadius * armWaveFrequency_ * TWO_PI +
          wavePhaseRadians_ + armIndex * TWO_PI / symmetry_);
      const float angle = armBaseAngle +
                          twistRadians * normalizedRadius * normalizedRadius +
                          radialWave * armWaveAmplitudeRadians_;
      const float radius =
          distanceFromCenterToScreenEdge(angle) * normalizedRadius;
      const float x = centerX_ + cosf(angle) * radius;
      const float y = centerY_ + sinf(angle) * radius;

      // Sparse deterministic gaps give the precise plotter lines a restrained
      // digital rhythm without using frame-to-frame random noise.
      const bool gap =
          ((segmentIndex + armIndex * 7U + renderedFrameCount_ / 5U) % 29U) == 0U;
      if (!gap) {
        canvas_.drawLine(previousX, previousY, x, y, armColor);
        targetDisplay_->drawLine(previousX, previousY, x, y, armColor);
      }
      previousX = x;
      previousY = y;
    }
  }
}

void RadialPrototype::drawOscillatingRings() {
  const uint32_t ringColor = monochromeColor(
      static_cast<uint8_t>(ringBrightness_));
  const float twistRadians =
      (0.35f + currentTwist_ * 4.0f) * twistDirection_;
  constexpr uint8_t kRingSegmentCount = 60;

  for (uint8_t ringIndex = 0; ringIndex < ringCount_; ++ringIndex) {
    // Fractions, rather than fixed pixel radii, preserve the full circular
    // coverage when the origin moves near one side of the display.
    const float baseRadiusFraction =
        (ringIndex + 1.0f) / (ringCount_ + 0.55f);
    float firstX = 0.0f;
    float firstY = 0.0f;
    float previousX = 0.0f;
    float previousY = 0.0f;

    // Sixty six-degree segments are enough for smooth one-pixel rings
    // while keeping trigonometry and SPI drawing bounded each frame.
    for (uint8_t angularStep = 0;
         angularStep <= kRingSegmentCount;
         ++angularStep) {
      const float angle = angularStep * TWO_PI / kRingSegmentCount;
      const float ringWave = sinf(
          angle * symmetry_ - wavePhaseRadians_ * 0.72f +
          ringIndex * 0.63f + phaseOffset_);
      const float ringRotation =
          rotationRadians_ * -0.55f +
          twistRadians * baseRadiusFraction * 0.12f;
      const float rotatedAngle = angle + ringRotation;
      const float screenEdgeDistance =
          distanceFromCenterToScreenEdge(rotatedAngle);
      const float radius = constrain(
          screenEdgeDistance * baseRadiusFraction +
              ringWave * ringWaveAmplitudePixels_,
          2.0f,
          screenEdgeDistance - 1.0f);
      const float x = centerX_ + cosf(rotatedAngle) * radius;
      const float y = centerY_ + sinf(rotatedAngle) * radius;

      if (angularStep == 0) {
        firstX = x;
        firstY = y;
      } else {
        canvas_.drawLine(previousX, previousY, x, y, ringColor);
        targetDisplay_->drawLine(previousX, previousY, x, y, ringColor);
      }
      previousX = x;
      previousY = y;
    }
    // Explicit closure avoids a one-segment seam after float rounding.
    canvas_.drawLine(previousX, previousY, firstX, firstY, ringColor);
    targetDisplay_->drawLine(previousX, previousY, firstX, firstY, ringColor);
  }
}

void RadialPrototype::updateSectorEnergy() {
  // The rotating arm axes are the algorithm's natural LED signal. A broad
  // cosine lobe lets neighboring sectors share energy rather than blink.
  for (uint8_t sectorIndex = 0; sectorIndex < kSectorCount; ++sectorIndex) {
    const float sectorAngle = sectorIndex * TWO_PI / kSectorCount;
    float strongestArm = 0.0f;
    for (uint8_t armIndex = 0; armIndex < armCount_; ++armIndex) {
      const float armAngle = armIndex * TWO_PI / armCount_ +
                             rotationRadians_ + phaseOffset_;
      const float alignment =
          0.5f + 0.5f * cosf(sectorAngle - armAngle);
      strongestArm = max(strongestArm, alignment * alignment * alignment);
    }
    const float travelingPulse =
        0.72f + 0.28f * sinf(wavePhaseRadians_ + sectorIndex * 1.21f);
    const float targetEnergy = constrain(strongestArm * travelingPulse,
                                         0.0f,
                                         1.0f);
    sectorEnergy_[sectorIndex] +=
        (targetEnergy - sectorEnergy_[sectorIndex]) * 0.12f;
  }
}

void RadialPrototype::updateFrameRateMeasurement(uint32_t nowMs) {
  const uint32_t measurementDurationMs = nowMs - measurementWindowStartedMs_;
  if (measurementDurationMs < 1000UL) {
    return;
  }
  measuredFramesPerSecond_ =
      framesInMeasurementWindow_ * 1000.0f / measurementDurationMs;
  framesInMeasurementWindow_ = 0;
  measurementWindowStartedMs_ = nowMs;
}
