#include "OrbitPrototype.h"

#include <math.h>

namespace {

uint32_t grayscalePaletteIndex(uint8_t grayLevel) {
  // A 4-bit LovyanGFX sprite expects a palette index rather than RGB888.
  // Its automatically created grayscale palette maps 0..15 to 0..255.
  return grayLevel >> 4U;
}


}  // namespace

OrbitPrototype::OrbitPrototype(lgfx::LovyanGFX* targetDisplay)
    : targetDisplay_(targetDisplay), canvas_(targetDisplay) {}

bool OrbitPrototype::begin(uint32_t seed) {
  if (ready_) {
    return true;
  }

  // ORBIT is monochrome, so a 16-entry grayscale palette preserves full
  // 360x360 geometry while reducing the PSRAM canvas from 259,200 to 64,800
  // bytes. Keep it in PSRAM for this isolated format/performance test.
  canvas_.setColorDepth(kCanvasBitsPerPixel);
  canvas_.setPsram(true);
  if (!canvas_.createSprite(kCanvasWidth, kCanvasHeight)) {
    return false;
  }
  canvas_.setPaletteGrayscale();

  ready_ = true;
  initializeSeededComposition(seed);
  return true;
}

bool OrbitPrototype::regenerate(uint32_t seed) {
  if (!ready_) {
    return false;
  }
  initializeSeededComposition(seed);
  showCurrentFrame();
  return true;
}

void OrbitPrototype::showCurrentFrame() {
  if (!ready_) {
    return;
  }
  canvas_.pushSprite(0, 0);
  // Menu time must not advance the model or create a long connecting stroke.
  lastFrameMs_ = millis();
  measurementWindowStartedMs_ = lastFrameMs_;
  framesInMeasurementWindow_ = 0;
  measuredFramesPerSecond_ = 0.0f;
}

void OrbitPrototype::setEccentricity(float normalizedValue) {
  targetEccentricity_ = constrain(normalizedValue, 0.0f, 1.0f);
}

float OrbitPrototype::sectorEnergy(uint8_t sectorIndex) const {
  return sectorIndex < kSectorCount ? sectorEnergy_[sectorIndex] : 0.0f;
}

float OrbitPrototype::nextRandomUnitFloat() {
  randomState_ ^= randomState_ << 13U;
  randomState_ ^= randomState_ >> 17U;
  randomState_ ^= randomState_ << 5U;
  return (randomState_ & 0xFFFFFFU) / 16777216.0f;
}

void OrbitPrototype::initializeSeededComposition(uint32_t seed) {
  seed_ = seed;
  randomState_ = seed ? seed : 1U;

  attractorCount_ = static_cast<uint8_t>(2U + randomState_ % 3U);
  symmetryOrder_ = static_cast<uint8_t>(3U + ((randomState_ >> 5U) % 3U));
  const float initialSpinDirection =
      nextRandomUnitFloat() < 0.5f ? -1.0f : 1.0f;
  globalRotationAngle_ = nextRandomUnitFloat() * TWO_PI;
  globalAngularVelocity_ =
      initialSpinDirection * kSlowestSpinRadiansPerSecond;
  targetGlobalAngularVelocity_ = globalAngularVelocity_;
  zoomPhase_ = nextRandomUnitFloat() * TWO_PI;
  // Keep the four former pivot/lens draws in the seeded sequence so existing
  // seeds still select exactly the same orbital bodies and attractors.
  nextRandomUnitFloat();
  nextRandomUnitFloat();
  for (uint8_t axisIndex = 0; axisIndex < symmetryOrder_; ++axisIndex) {
    const float facetAngle = TWO_PI * axisIndex / symmetryOrder_;
    facetCosine_[axisIndex] = cosf(facetAngle);
    facetSine_[axisIndex] = sinf(facetAngle);
  }

  nextRandomUnitFloat();
  nextRandomUnitFloat();

  for (uint8_t index = 0; index < attractorCount_; ++index) {
    Attractor& attractor = attractors_[index];
    attractor.orbitRadius = 14.0f + nextRandomUnitFloat() * 34.0f;
    attractor.orbitFlattening = 0.48f + nextRandomUnitFloat() * 0.38f;
    attractor.phase = nextRandomUnitFloat() * TWO_PI;
    attractor.angularSpeed =
        (0.035f + nextRandomUnitFloat() * 0.055f) *
        (nextRandomUnitFloat() < 0.5f ? -1.0f : 1.0f);
  }

  for (uint8_t index = 0; index < kBodyCount; ++index) {
    Body& body = bodies_[index];
    body.attractorIndex = static_cast<uint8_t>(index % attractorCount_);
    body.radius = 18.0f + nextRandomUnitFloat() * 62.0f;
    body.phase = nextRandomUnitFloat() * TWO_PI;
    body.angularSpeed = 0.22f + nextRandomUnitFloat() * 0.72f;
    body.ellipseAngle = nextRandomUnitFloat() * TWO_PI;
    body.precessionSpeed = (nextRandomUnitFloat() - 0.5f) * 0.075f;
    body.eccentricityScale = 0.60f + nextRandomUnitFloat() * 0.55f;
    body.transferPhase = nextRandomUnitFloat() * TWO_PI;
    body.transferSpeed = 0.055f + nextRandomUnitFloat() * 0.055f;
    body.direction = nextRandomUnitFloat() < 0.24f ? -1 : 1;
    body.grayLevel = static_cast<uint8_t>(82 + nextRandomUnitFloat() * 138);
    body.brightHead = (index % 11U) == (seed_ % 11U);
  }
  for (uint8_t sectorIndex = 0; sectorIndex < kSectorCount; ++sectorIndex) {
    sectorEnergy_[sectorIndex] = 0.0f;
  }
  reboundLedEnvelopeActive_ = false;

  animationTimeSeconds_ = 0.0f;
  currentEccentricity_ = targetEccentricity_;
  trailNextSlot_ = 0;
  trailCount_ = 0;
  lastTrailSampleTime_ = -kTrailSamplePeriodSeconds;
  coreX_ = kScreenCenter;
  coreY_ = kScreenCenter;
  const float direction = nextRandomUnitFloat() * TWO_PI;
  coreVelocityX_ = cosf(direction) * kCoreSpeedPixelsPerSecond;
  coreVelocityY_ = sinf(direction) * kCoreSpeedPixelsPerSecond;
  renderedFrameCount_ = 0;
  slowestFrameTimeUs_ = 0;
  framesInMeasurementWindow_ = 0;
  measuredFramesPerSecond_ = 0.0f;
  canvas_.fillScreen(TFT_BLACK);
  lastFrameMs_ = millis();
  measurementWindowStartedMs_ = lastFrameMs_;
}

void OrbitPrototype::updateCore(float elapsedSeconds) {
  // Solve the contact time so the remaining part of this frame follows the
  // new direction. The 0.1 s update cap permits at most one contact per frame.
  const float x = coreX_ - kScreenCenter;
  const float y = coreY_ - kScreenCenter;
  const float speedSquared = coreVelocityX_ * coreVelocityX_ +
                             coreVelocityY_ * coreVelocityY_;
  const float projection = x * coreVelocityX_ + y * coreVelocityY_;
  const float distanceSquared = x * x + y * y;
  const float discriminant = projection * projection +
      speedSquared * max(0.0f, kCoreBoundaryRadius * kCoreBoundaryRadius -
                                  distanceSquared);
  const float contactSeconds = max(0.0f,
      (-projection + sqrtf(discriminant)) / speedSquared);
  if (contactSeconds > elapsedSeconds) {
    coreX_ += coreVelocityX_ * elapsedSeconds;
    coreY_ += coreVelocityY_ * elapsedSeconds;
    return;
  }
  coreX_ += coreVelocityX_ * contactSeconds;
  coreY_ += coreVelocityY_ * contactSeconds;
  const float normalAngle = atan2f(coreY_ - kScreenCenter,
                                   coreX_ - kScreenCenter);

  // Remember the contacted quarter and its outward normal. Three consecutive
  // LEDs will span 90 degrees around that quarter's central LED, while their
  // brightness follows the model's distance from this exact border.
  float positiveContactAngle = normalAngle;
  if (positiveContactAngle < 0.0f) {
    positiveContactAngle += TWO_PI;
  }
  reboundQuarterIndex_ = static_cast<uint8_t>(
      positiveContactAngle * 4.0f / TWO_PI) % 4U;
  reboundBorderNormalX_ = cosf(normalAngle);
  reboundBorderNormalY_ = sinf(normalAngle);
  reboundLedEnvelopeActive_ = true;

  // +/-60 degrees about the inward normal prevents grazing/stuck contacts.
  // The local seeded generator makes the sequence reproducible after reboot.
  const float direction = normalAngle + PI +
      (nextRandomUnitFloat() - 0.5f) * (2.0f * PI / 3.0f);
  coreVelocityX_ = cosf(direction) * kCoreSpeedPixelsPerSecond;
  coreVelocityY_ = sinf(direction) * kCoreSpeedPixelsPerSecond;

  // A glancing departure carries more apparent spin than a frontal one. The
  // sign of the tangent component selects rotation direction, so the same
  // seeded random rebound sequence also reproduces the self-rotation sequence.
  const float tangentX = -reboundBorderNormalY_;
  const float tangentY = reboundBorderNormalX_;
  const float tangentialFraction = constrain(
      (coreVelocityX_ * tangentX + coreVelocityY_ * tangentY) /
          kCoreSpeedPixelsPerSecond,
      -1.0f,
      1.0f);
  const float tangentialStrength = min(
      1.0f,
      fabsf(tangentialFraction) / 0.8660254f);
  const float targetSpinMagnitude =
      kSlowestSpinRadiansPerSecond +
      (kFastestSpinRadiansPerSecond - kSlowestSpinRadiansPerSecond) *
          tangentialStrength;
  const float spinDirection = fabsf(tangentialFraction) > 0.08f
                                  ? (tangentialFraction > 0.0f ? 1.0f : -1.0f)
                                  : (targetGlobalAngularVelocity_ >= 0.0f
                                         ? 1.0f
                                         : -1.0f);
  targetGlobalAngularVelocity_ = spinDirection * targetSpinMagnitude;

  const float remainder = elapsedSeconds - contactSeconds;
  coreX_ += coreVelocityX_ * remainder;
  coreY_ += coreVelocityY_ * remainder;
}

void OrbitPrototype::updateGlobalRotation(float elapsedSeconds) {
  const float maximumVelocityChange =
      kSpinResponseRadiansPerSecondSquared * elapsedSeconds;
  globalAngularVelocity_ += constrain(
      targetGlobalAngularVelocity_ - globalAngularVelocity_,
      -maximumVelocityChange,
      maximumVelocityChange);
  globalRotationAngle_ += globalAngularVelocity_ * elapsedSeconds;
  if (globalRotationAngle_ > TWO_PI) {
    globalRotationAngle_ -= TWO_PI;
  } else if (globalRotationAngle_ < -TWO_PI) {
    globalRotationAngle_ += TWO_PI;
  }
}

void OrbitPrototype::updateReboundLedEnvelope() {
  for (uint8_t sectorIndex = 0; sectorIndex < kSectorCount; ++sectorIndex) {
    sectorEnergy_[sectorIndex] = 0.0f;
  }
  if (!reboundLedEnvelopeActive_) {
    return;
  }

  const float relativeX = coreX_ - kScreenCenter;
  const float relativeY = coreY_ - kScreenCenter;
  // At contact, the projection onto the stored outward normal equals the
  // boundary radius. Moving inward reduces it, so this difference is the
  // distance travelled away from the particular border that was struck.
  const float distanceFromContactedBorder = max(
      0.0f,
      kCoreBoundaryRadius -
          (relativeX * reboundBorderNormalX_ +
           relativeY * reboundBorderNormalY_));
  const float linearBrightness = constrain(
      1.0f - distanceFromContactedBorder / kReboundLedFadeDistancePixels,
      0.0f,
      1.0f);
  if (linearBrightness <= 0.0f) {
    reboundLedEnvelopeActive_ = false;
    return;
  }

  // Smoothstep keeps full light near impact and avoids an abrupt final cutoff.
  const float brightness = linearBrightness * linearBrightness *
                           (3.0f - 2.0f * linearBrightness);
  const uint8_t centerQuarterLed = reboundQuarterIndex_ * 2U + 1U;
  const uint8_t previousQuarterLed =
      (centerQuarterLed + kSectorCount - 1U) % kSectorCount;
  const uint8_t nextQuarterLed = (centerQuarterLed + 1U) % kSectorCount;
  sectorEnergy_[previousQuarterLed] = brightness;
  sectorEnergy_[centerQuarterLed] = brightness;
  sectorEnergy_[nextQuarterLed] = brightness;
}

void OrbitPrototype::drawTrail(uint8_t bodyIndex, float modelX, float modelY) {
  const Body& body = bodies_[bodyIndex];
  // The sparse white-headed trajectories keep every history sample. Secondary
  // gray trajectories connect every other sample across the same 3.2-second
  // history, nearly halving their PSRAM line writes without shortening them.
  const uint8_t historyStride = body.brightHead ? 1U : 2U;
  for (uint8_t copy = 0; copy < symmetryOrder_ * 2U; ++copy) {
    int32_t previousPixelX = 0;
    int32_t previousPixelY = 0;
    bool hasPrevious = false;
    // Oldest first: newer strokes paint over older, dimmer strokes.
    uint8_t sample = 0;
    while (true) {
      const bool drawingCurrentPoint = sample == trailCount_;
      float x = modelX, y = modelY;
      float brightness = 1.0f;
      if (!drawingCurrentPoint) {
        const uint8_t slot = (trailNextSlot_ + kTrailSampleCount -
                             trailCount_ + sample) % kTrailSampleCount;
        x = trail_[bodyIndex][slot].x;
        y = trail_[bodyIndex][slot].y;
        brightness = frameTrailBrightness_[slot];
      }
      float screenX, screenY;
      transformModelPoint(x, y, copy, screenX, screenY);
      const int32_t pixelX = lroundf(screenX);
      const int32_t pixelY = lroundf(screenY);
      if (hasPrevious) {
        const uint8_t gray = static_cast<uint8_t>(
            body.grayLevel * brightness);
        // Reject only segments whose two endpoints lie beyond the same canvas
        // side. Any segment that can cross the viewport still reaches LovyanGFX
        // and uses its normal clipping, preserving every visible pixel.
        const bool entirelyOutsideCanvas =
            (previousPixelX < 0 && pixelX < 0) ||
            (previousPixelX >= kCanvasWidth && pixelX >= kCanvasWidth) ||
            (previousPixelY < 0 && pixelY < 0) ||
            (previousPixelY >= kCanvasHeight && pixelY >= kCanvasHeight);
        if (!entirelyOutsideCanvas) {
          canvas_.drawLine(previousPixelX, previousPixelY,
                           pixelX, pixelY, grayscalePaletteIndex(gray));
        }
      }
      previousPixelX = pixelX;
      previousPixelY = pixelY;
      hasPrevious = true;

      if (drawingCurrentPoint) {
        break;
      }
      const uint8_t nextHistorySample = sample + historyStride;
      sample = nextHistorySample < trailCount_
                   ? nextHistorySample
                   : trailCount_;
    }
    if (body.brightHead) {
      canvas_.fillRect(previousPixelX - 1, previousPixelY - 1,
                       2, 2, TFT_WHITE);
    }
  }
}

void OrbitPrototype::calculateAttractorPosition(uint8_t index,
                                                float& x,
                                                float& y) const {
  const Attractor& attractor = attractors_[index];
  const float angle = attractor.phase +
                      animationTimeSeconds_ * attractor.angularSpeed;
  x = cosf(angle) * attractor.orbitRadius;
  y = sinf(angle) * attractor.orbitRadius * attractor.orbitFlattening;
}

void OrbitPrototype::transformModelPoint(float modelX,
                                         float modelY,
                                         uint8_t copyIndex,
                                         float& screenX,
                                         float& screenY) const {
  screenX = coreX_ + frameTransformXX_[copyIndex] * modelX +
            frameTransformXY_[copyIndex] * modelY;
  screenY = coreY_ + frameTransformYX_[copyIndex] * modelX +
            frameTransformYY_[copyIndex] * modelY;
}

void OrbitPrototype::updateFrameProjectionCache() {
  const float scale = kImmersiveModelScale * frameZoom_;
  for (uint8_t copyIndex = 0;
       copyIndex < symmetryOrder_ * 2U;
       ++copyIndex) {
    const uint8_t axisIndex = copyIndex / 2U;
    const float axisCosine = facetCosine_[axisIndex];
    const float axisSine = facetSine_[axisIndex];
    const float mirrorSign = (copyIndex & 1U) != 0U ? -1.0f : 1.0f;

    frameTransformXX_[copyIndex] =
        scale * (frameGlobalCosine_ * axisCosine -
                 frameGlobalSine_ * axisSine);
    frameTransformXY_[copyIndex] =
        scale * mirrorSign * (-frameGlobalCosine_ * axisSine -
                              frameGlobalSine_ * axisCosine);
    frameTransformYX_[copyIndex] =
        scale * (frameGlobalSine_ * axisCosine +
                 frameGlobalCosine_ * axisSine);
    frameTransformYY_[copyIndex] =
        scale * mirrorSign * (-frameGlobalSine_ * axisSine +
                              frameGlobalCosine_ * axisCosine);
  }
}

void OrbitPrototype::drawOrbitalSystem() {
  // Whole-model trigonometry is shared by every reflected point and is
  // therefore evaluated once per frame rather than hundreds of times.
  // Keep the approved breathing motion inside the physically smoother compact
  // range. The former 72-132% expansion made long PSRAM line runs dominate
  // frame time; 72-88% retains a visible inhale/exhale without the costly swell.
  frameZoom_ = 0.80f + 0.08f * sinf(
      animationTimeSeconds_ * TWO_PI / kZoomPeriodSeconds + zoomPhase_);
  frameGlobalCosine_ = cosf(globalRotationAngle_);
  frameGlobalSine_ = sinf(globalRotationAngle_);
  updateFrameProjectionCache();

  // History timestamps are common to all 42 bodies and all reflected copies.
  // Compute the exact existing quadratic fade once for each occupied slot.
  for (uint8_t sample = 0; sample < trailCount_; ++sample) {
    const uint8_t slot = (trailNextSlot_ + kTrailSampleCount -
                          trailCount_ + sample) % kTrailSampleCount;
    const float age = animationTimeSeconds_ - trailTimes_[slot];
    const float remaining = max(
        0.0f,
        1.0f - age /
            (kTrailSampleCount * kTrailSamplePeriodSeconds));
    frameTrailBrightness_[slot] = remaining * remaining;
  }

  float attractorX[kMaximumAttractorCount] = {};
  float attractorY[kMaximumAttractorCount] = {};
  for (uint8_t index = 0; index < attractorCount_; ++index) {
    calculateAttractorPosition(index, attractorX[index], attractorY[index]);
  }

  const bool storeSample = animationTimeSeconds_ - lastTrailSampleTime_ >=
                           kTrailSamplePeriodSeconds;
  const uint8_t copyCount = symmetryOrder_ * 2U;
  for (uint8_t bodyIndex = 0; bodyIndex < kBodyCount; ++bodyIndex) {
    const Body& body = bodies_[bodyIndex];
    const uint8_t nextAttractorIndex =
        static_cast<uint8_t>((body.attractorIndex + 1U) % attractorCount_);

    // At high ECCENTRICITY, a narrow smooth pulse transfers selected bodies
    // toward the neighboring attractor. It stays deterministic and periodic.
    const float transferWave = max(
        0.0f,
        sinf(body.transferPhase + animationTimeSeconds_ * body.transferSpeed));
    const float transferStrength = constrain(
        (currentEccentricity_ - 0.55f) / 0.45f, 0.0f, 1.0f);
    const float transferMix = transferStrength *
                              transferWave * transferWave *
                              transferWave * transferWave;
    const float centerX = attractorX[body.attractorIndex] +
        (attractorX[nextAttractorIndex] - attractorX[body.attractorIndex]) *
            transferMix;
    const float centerY = attractorY[body.attractorIndex] +
        (attractorY[nextAttractorIndex] - attractorY[body.attractorIndex]) *
            transferMix;

    const float eccentricity = constrain(
        0.10f + currentEccentricity_ * 0.72f * body.eccentricityScale,
        0.08f,
        0.86f);
    const float orbitalAngle = body.phase +
        animationTimeSeconds_ * body.angularSpeed * body.direction;
    const float orbitalCosine = cosf(orbitalAngle);
    const float orbitalSine = sinf(orbitalAngle);
    const float semiMajorAxis = body.radius * (1.0f - transferMix * 0.20f);
    const float semiMinorAxis = semiMajorAxis *
                                sqrtf(1.0f - eccentricity * eccentricity);
    const float ellipseX = semiMajorAxis * (orbitalCosine -
                                            eccentricity * 0.34f);
    const float ellipseY = semiMinorAxis * orbitalSine;
    const float ellipseAngle = body.ellipseAngle +
        animationTimeSeconds_ * body.precessionSpeed *
            (0.35f + currentEccentricity_);
    const float ellipseCosine = cosf(ellipseAngle);
    const float ellipseSine = sinf(ellipseAngle);
    const float modelX = centerX +
                         ellipseCosine * ellipseX - ellipseSine * ellipseY;
    const float modelY = centerY +
                         ellipseSine * ellipseX + ellipseCosine * ellipseY;

    drawTrail(bodyIndex, modelX, modelY);
    if (storeSample) {
      trail_[bodyIndex][trailNextSlot_] = {modelX, modelY};
    }
  }

  if (storeSample) {
    trailTimes_[trailNextSlot_] = animationTimeSeconds_;
    trailNextSlot_ = (trailNextSlot_ + 1U) % kTrailSampleCount;
    if (trailCount_ < kTrailSampleCount) ++trailCount_;
    lastTrailSampleTime_ = animationTimeSeconds_;
  }

  // Report the transformed symmetry convergence point for serial diagnosis.
  transformModelPoint(0.0f, 0.0f, 0, reportedCenterX_, reportedCenterY_);
}

void OrbitPrototype::update(uint32_t nowMs) {
  if (!ready_) {
    return;
  }

  const uint32_t elapsedMs = nowMs - lastFrameMs_;
  if (elapsedMs < kTargetFramePeriodMs) {
    return;
  }
  lastFrameMs_ = nowMs;
  const uint32_t frameStartedUs = micros();
  const float elapsedSeconds = min(elapsedMs, 100UL) / 1000.0f;
  currentEccentricity_ += constrain(
      targetEccentricity_ - currentEccentricity_,
      -elapsedSeconds * kEccentricityResponsePerSecond,
      elapsedSeconds * kEccentricityResponsePerSecond);
  animationTimeSeconds_ += elapsedSeconds;
  updateCore(elapsedSeconds);
  updateGlobalRotation(elapsedSeconds);
  updateReboundLedEnvelope();

  // Reproject the complete history into one reusable frame. Only the finished
  // frame goes to the display; no stale pixels remain at the previous position.
  canvas_.fillScreen(TFT_BLACK);
  drawOrbitalSystem();
  canvas_.pushSprite(0, 0);

  slowestFrameTimeUs_ = max(slowestFrameTimeUs_, micros() - frameStartedUs);
  ++renderedFrameCount_;
  updateFrameRateMeasurement(nowMs);
}

void OrbitPrototype::updateFrameRateMeasurement(uint32_t nowMs) {
  ++framesInMeasurementWindow_;
  const uint32_t windowMs = nowMs - measurementWindowStartedMs_;
  if (windowMs >= 1000U) {
    measuredFramesPerSecond_ = framesInMeasurementWindow_ * 1000.0f / windowMs;
    framesInMeasurementWindow_ = 0;
    measurementWindowStartedMs_ = nowMs;
  }
}
