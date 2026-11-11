#include "FlowPrototype.h"

#include <math.h>

namespace {

// Converts an 8-bit gray level into the 0xRRGGBB color accepted by LovyanGFX.
// All three channels receive the same value, so no color can enter the artwork.
uint32_t monochromeColor(uint8_t grayLevel) {
  return (static_cast<uint32_t>(grayLevel) << 16U) |
         (static_cast<uint32_t>(grayLevel) << 8U) |
         static_cast<uint32_t>(grayLevel);
}

uint16_t swapBytes16(uint16_t value) {
  return static_cast<uint16_t>((value << 8U) | (value >> 8U));
}

}  // namespace

FlowPrototype::FlowPrototype(lgfx::LovyanGFX* targetDisplay)
    : targetDisplay_(targetDisplay), canvas_(targetDisplay) {}

bool FlowPrototype::begin(uint32_t seed) {
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

bool FlowPrototype::regenerate(uint32_t seed) {
  if (!ready_) {
    return false;
  }

  // No createSprite() call occurs here: repeated clicks therefore cannot leak
  // or fragment the large 259,200-byte PSRAM allocation.
  initializeSeededComposition(seed);
  return true;
}

void FlowPrototype::showCurrentFrame() {
  if (!ready_) {
    return;
  }
  canvas_.pushSprite(0, 0);
  // Ignore time spent in the menu so particles do not jump when artwork resumes.
  lastFrameMs_ = millis();
  // Restart only the one-second FPS sampling window. Including paused menu time
  // would incorrectly report a performance drop after returning to artwork.
  framesInMeasurementWindow_ = 0;
  measurementWindowStartedMs_ = lastFrameMs_;
  measuredFramesPerSecond_ = 0.0f;
}

void FlowPrototype::update(uint32_t nowMs) {
  if (!ready_) {
    return;
  }

  const uint32_t elapsedMs = nowMs - lastFrameMs_;
  if (elapsedMs < kTargetFramePeriodMs) {
    return;
  }
  lastFrameMs_ = nowMs;

  // Very long pauses (for example a serial debugger stop) are clamped so one
  // delayed frame cannot throw every particle across the complete display.
  const uint32_t simulationElapsedMs = min(elapsedMs, 100UL);
  const float elapsedSeconds = simulationElapsedMs / 1000.0f;
  const uint32_t frameStartedUs = micros();

  // Smoothly approach the value selected with the encoder. The limited change
  // per frame keeps trails continuous when the user turns several detents fast.
  const float turbulenceDifference =
      targetTurbulence_ - currentTurbulence_;
  const float maximumTurbulenceChange =
      kTurbulenceResponsePerSecond * elapsedSeconds;
  if (fabsf(turbulenceDifference) <= maximumTurbulenceChange) {
    currentTurbulence_ = targetTurbulence_;
  } else {
    currentTurbulence_ += turbulenceDifference > 0.0f
                              ? maximumTurbulenceChange
                              : -maximumTurbulenceChange;
  }

  updateVortexCenter(elapsedSeconds);

  globalRenderRotationRadians_ +=
      kGlobalRotationRadiansPerSecond * elapsedSeconds;
  if (globalRenderRotationRadians_ < -TWO_PI) {
    globalRenderRotationRadians_ += TWO_PI;
  }

  fadePartOfCanvas();

  // Short line segments are sent directly so motion appears every frame. The
  // faded background rows are synchronized separately across the full screen.
  targetDisplay_->startWrite();
  updateParticles(simulationElapsedMs, elapsedSeconds);
  pushFadedRowsToDisplay();
  targetDisplay_->endWrite();

  const uint32_t frameTimeUs = micros() - frameStartedUs;
  if (frameTimeUs > slowestFrameTimeUs_) {
    slowestFrameTimeUs_ = frameTimeUs;
  }

  ++renderedFrameCount_;
  ++framesInMeasurementWindow_;
  updateFrameRateMeasurement(nowMs);
}

bool FlowPrototype::isReady() const {
  return ready_;
}

uint32_t FlowPrototype::seed() const {
  return seed_;
}

uint32_t FlowPrototype::renderedFrameCount() const {
  return renderedFrameCount_;
}

float FlowPrototype::measuredFramesPerSecond() const {
  return measuredFramesPerSecond_;
}

uint32_t FlowPrototype::slowestFrameTimeUs() const {
  return slowestFrameTimeUs_;
}

size_t FlowPrototype::canvasSizeBytes() const {
  return static_cast<size_t>(kCanvasWidth) * kCanvasHeight * sizeof(uint16_t);
}

void FlowPrototype::setTurbulence(float normalizedValue) {
  targetTurbulence_ = constrain(normalizedValue, 0.0f, 1.0f);
}

float FlowPrototype::turbulence() const {
  // Return the target so serial feedback immediately matches the knob detent,
  // even while the visible transition is still being smoothed.
  return targetTurbulence_;
}

float FlowPrototype::sectorEnergy(uint8_t sectorIndex) const {
  if (sectorIndex >= kSectorCount) {
    return 0.0f;
  }
  return sectorEnergy_[sectorIndex];
}

float FlowPrototype::vortexCenterX() const {
  return vortexCenterX_;
}

float FlowPrototype::vortexCenterY() const {
  return vortexCenterY_;
}

uint32_t FlowPrototype::nextRandomNumber() {
  // Xorshift32 is small, fast, and fully deterministic. It is intentionally
  // private to FLOW so other code cannot accidentally disturb its sequence.
  uint32_t value = randomState_;
  value ^= value << 13U;
  value ^= value >> 17U;
  value ^= value << 5U;
  randomState_ = value;
  return value;
}

float FlowPrototype::nextRandomUnitFloat() {
  // The upper 24 random bits map exactly into the useful precision of a float.
  return static_cast<float>(nextRandomNumber() >> 8U) / 16777216.0f;
}

void FlowPrototype::initializeSeededComposition(uint32_t seed) {
  seed_ = seed;
  // Xorshift generators cannot use zero as their state. This fixed fallback
  // keeps seed 0 deterministic instead of silently using time-based randomness.
  randomState_ = seed == 0 ? 0x6D2B79F5UL : seed;

  canvas_.fillScreen(TFT_BLACK);
  fieldPhaseA_ = nextRandomUnitFloat() * 1000.0f;
  fieldPhaseB_ = nextRandomUnitFloat() * 1000.0f;
  vortexCenterX_ = kCanvasCenter;
  vortexCenterY_ = kCanvasCenter;

  // Derive motion direction directly from the seed without consuming FLOW's
  // particle random sequence. A given seed therefore always gets the same path.
  const uint32_t motionHash = seed * 747796405UL + 2891336453UL;
  const float motionAngle =
      static_cast<float>(motionHash >> 8U) / 16777216.0f * TWO_PI;
  vortexCenterVelocityX_ =
      cosf(motionAngle) * kVortexCenterSpeedPixelsPerSecond;
  vortexCenterVelocityY_ =
      sinf(motionAngle) * kVortexCenterSpeedPixelsPerSecond;
  // A second seed bit selects which way the center initially glides along the
  // circular limit. This tangential bias prevents a mechanical radial reversal.
  vortexBoundaryGlideDirection_ =
      (motionHash & 1U) == 0U ? -1.0f : 1.0f;
  globalRenderRotationRadians_ = 0.0f;
  fadePhase_ = 0;

  for (Particle& particle : particles_) {
    respawnParticle(particle);
  }

  // A one-time complete transfer makes the seed change visually unambiguous.
  // Normal animation returns immediately to the validated partial-row updates.
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

void FlowPrototype::updateVortexCenter(float elapsedSeconds) {
  float offsetX = vortexCenterX_ - kCanvasCenter;
  float offsetY = vortexCenterY_ - kCanvasCenter;
  const float distanceFromScreenCenter =
      sqrtf(offsetX * offsetX + offsetY * offsetY);

  if (distanceFromScreenCenter > kVortexSoftBoundaryStartRadius) {
    const float boundaryWidth = kVortexMaximumRoamRadius -
                                kVortexSoftBoundaryStartRadius;
    const float normalizedBoundaryDepth = constrain(
        (distanceFromScreenCenter - kVortexSoftBoundaryStartRadius) /
            boundaryWidth,
        0.0f,
        1.0f);
    // Smoothstep starts and ends with zero slope. The steering force therefore
    // appears gradually rather than creating a visible corner in the path.
    const float smoothBoundaryDepth =
        normalizedBoundaryDepth * normalizedBoundaryDepth *
        (3.0f - 2.0f * normalizedBoundaryDepth);
    const float outwardNormalX = offsetX / distanceFromScreenCenter;
    const float outwardNormalY = offsetY / distanceFromScreenCenter;
    const float inwardVelocityChange =
        kVortexBoundarySteeringPixelsPerSecondSquared *
        smoothBoundaryDepth * elapsedSeconds;
    const float tangentialVelocityChange =
        kVortexBoundaryGlidePixelsPerSecondSquared *
        smoothBoundaryDepth * elapsedSeconds *
        vortexBoundaryGlideDirection_;
    vortexCenterVelocityX_ -= outwardNormalX * inwardVelocityChange;
    vortexCenterVelocityY_ -= outwardNormalY * inwardVelocityChange;
    // (-normalY, normalX) is tangent to the circular boundary. Adding it
    // progressively creates the requested grazing arc instead of a radial stop.
    vortexCenterVelocityX_ -= outwardNormalY * tangentialVelocityChange;
    vortexCenterVelocityY_ += outwardNormalX * tangentialVelocityChange;

    // Steering changes direction, not speed. Renormalizing avoids a mechanical
    // slowdown at the edge and produces a continuous grazing curve.
    const float steeredSpeed = sqrtf(
        vortexCenterVelocityX_ * vortexCenterVelocityX_ +
        vortexCenterVelocityY_ * vortexCenterVelocityY_);
    if (steeredSpeed > 0.001f) {
      vortexCenterVelocityX_ =
          vortexCenterVelocityX_ / steeredSpeed *
          kVortexCenterSpeedPixelsPerSecond;
      vortexCenterVelocityY_ =
          vortexCenterVelocityY_ / steeredSpeed *
          kVortexCenterSpeedPixelsPerSecond;
    }
  }

  vortexCenterX_ += vortexCenterVelocityX_ * elapsedSeconds;
  vortexCenterY_ += vortexCenterVelocityY_ * elapsedSeconds;

  // This safety limit should rarely activate because soft steering begins
  // 25 pixels earlier. If a very long delayed frame overshoots, place the center
  // on the limit and remove only its outward motion, letting it slide tangentially.
  offsetX = vortexCenterX_ - kCanvasCenter;
  offsetY = vortexCenterY_ - kCanvasCenter;
  const float updatedDistance = sqrtf(offsetX * offsetX + offsetY * offsetY);
  if (updatedDistance > kVortexMaximumRoamRadius) {
    const float outwardNormalX = offsetX / updatedDistance;
    const float outwardNormalY = offsetY / updatedDistance;
    vortexCenterX_ = kCanvasCenter +
                     outwardNormalX * kVortexMaximumRoamRadius;
    vortexCenterY_ = kCanvasCenter +
                     outwardNormalY * kVortexMaximumRoamRadius;
    const float outwardSpeed =
        vortexCenterVelocityX_ * outwardNormalX +
        vortexCenterVelocityY_ * outwardNormalY;
    if (outwardSpeed > 0.0f) {
      vortexCenterVelocityX_ -= outwardNormalX * outwardSpeed;
      vortexCenterVelocityY_ -= outwardNormalY * outwardSpeed;
    }
  }
}

void FlowPrototype::respawnParticle(Particle& particle) {
  // Rejection sampling chooses a uniform random point inside a circle. Points
  // generated in the surrounding square are retried when outside the radius.
  float centeredX;
  float centeredY;
  const float radiusSquared =
      static_cast<float>(kParticleSpawnRadius * kParticleSpawnRadius);
  do {
    centeredX = (nextRandomUnitFloat() * 2.0f - 1.0f) *
                kParticleSpawnRadius;
    centeredY = (nextRandomUnitFloat() * 2.0f - 1.0f) *
                kParticleSpawnRadius;
  } while (centeredX * centeredX + centeredY * centeredY > radiusSquared);

  particle.x = kCanvasCenter + centeredX;
  particle.y = kCanvasCenter + centeredY;
  updateRenderedPosition(particle);
  particle.remainingLifeMs =
      3000UL + static_cast<uint32_t>(nextRandomUnitFloat() * 6000.0f);

  // Most particles form the body of the flow. Smaller deterministic groups
  // become very slow drifters or fast tracer lines, adding visual hierarchy.
  const float speedGroup = nextRandomUnitFloat();
  if (speedGroup < 0.12f) {
    particle.speedMultiplier = 0.25f + nextRandomUnitFloat() * 0.25f;
  } else if (speedGroup > 0.90f) {
    particle.speedMultiplier = 1.70f + nextRandomUnitFloat() * 0.80f;
  } else {
    particle.speedMultiplier = 0.65f + nextRandomUnitFloat() * 0.75f;
  }

  const float speedBrightness = min(particle.speedMultiplier / 2.5f, 1.0f);
  particle.grayLevel = static_cast<uint8_t>(
      90.0f + speedBrightness * 120.0f + nextRandomUnitFloat() * 40.0f);

  particle.timeUntilGlitchMs =
      12000UL + static_cast<uint32_t>(nextRandomUnitFloat() * 24000.0f);
  particle.glitchRemainingMs = 0;
  particle.glitchDirectionX = 0.0f;
  particle.glitchDirectionY = 0.0f;
  particle.glitchSpeedMultiplier = 1.0f;
}

void FlowPrototype::updateRenderedPosition(Particle& particle) {
  const float centerOffsetX = particle.x - kCanvasCenter;
  const float centerOffsetY = particle.y - kCanvasCenter;
  const float rotationCos = cosf(globalRenderRotationRadians_);
  const float rotationSin = sinf(globalRenderRotationRadians_);
  particle.renderedX = kCanvasCenter +
                       rotationCos * centerOffsetX -
                       rotationSin * centerOffsetY;
  particle.renderedY = kCanvasCenter +
                       rotationSin * centerOffsetX +
                       rotationCos * centerOffsetY;
}

void FlowPrototype::fadePartOfCanvas() {
  // A complete 360 x 360 fade every frame would repeatedly scan 129,600 PSRAM
  // pixels. Each frame instead fades 45 rows dispersed over the whole surface.
  // This removes the visible top-to-bottom dark band while every pixel still
  // receives exactly one fade pass every eight frames.
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
      // LovyanGFX stores a 16-bit sprite in byte-swapped RGB565 order. Convert
      // to normal RGB565, attenuate every channel, then restore storage order.
      const uint16_t rgb565 = swapBytes16(row[x]);
      const uint16_t red =
          ((rgb565 >> 11U) & 0x1FU) * kFadeMultiplier >> 8U;
      const uint16_t green =
          ((rgb565 >> 5U) & 0x3FU) * kFadeMultiplier >> 8U;
      const uint16_t blue =
          (rgb565 & 0x1FU) * kFadeMultiplier >> 8U;
      const uint16_t fadedRgb565 =
          static_cast<uint16_t>((red << 11U) | (green << 5U) | blue);
      row[x] = swapBytes16(fadedRgb565);
    }
  }

  fadePhase_ = (fadePhase_ + 1U) % kFadePhaseCount;
}

void FlowPrototype::pushFadedRowsToDisplay() {
  const uint16_t* storedPixels =
      static_cast<const uint16_t*>(canvas_.getBuffer());
  if (storedPixels == nullptr) {
    return;
  }

  // swap565_t tells LovyanGFX that the sprite buffer is already stored in the
  // byte order expected for transmission. The rows are individually placed,
  // but startWrite() keeps them inside one display transaction.
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

void FlowPrototype::updateParticles(uint32_t elapsedMs, float elapsedSeconds) {
  const float boundaryRadiusSquared =
      static_cast<float>(kParticleBoundaryRadius * kParticleBoundaryRadius);
  const float renderRotationCos = cosf(globalRenderRotationRadians_);
  const float renderRotationSin = sinf(globalRenderRotationRadians_);
  // This small fixed array gathers the activity created during this frame.
  // It lives on the stack and performs no allocation inside the render loop.
  float frameSectorEnergy[kSectorCount] = {};

  for (uint16_t particleIndex = 0;
       particleIndex < kParticleCount;
       ++particleIndex) {
    Particle& particle = particles_[particleIndex];
    const float oldX = particle.x;
    const float oldY = particle.y;
    const float oldRenderedX = particle.renderedX;
    const float oldRenderedY = particle.renderedY;
    // Screen offsets enforce the physical circular boundary. Field offsets use
    // the independently moving vortex center and determine local flow direction.
    const float screenOffsetX = oldX - kCanvasCenter;
    const float screenOffsetY = oldY - kCanvasCenter;
    const float screenRadiusSquared = screenOffsetX * screenOffsetX +
                                      screenOffsetY * screenOffsetY;

    if (particle.remainingLifeMs <= elapsedMs ||
        screenRadiusSquared > boundaryRadiusSquared) {
      respawnParticle(particle);
      continue;
    }
    particle.remainingLifeMs -= elapsedMs;

    if (particle.glitchRemainingMs > 0) {
      particle.glitchRemainingMs =
          particle.glitchRemainingMs > elapsedMs
              ? particle.glitchRemainingMs - elapsedMs
              : 0;
    } else if (particle.timeUntilGlitchMs <= elapsedMs) {
      // A glitch is a short, seed-driven event: one particle accelerates and
      // receives a temporary directional bias. It is repeatable, not frame
      // noise, so the composition remains coherent rather than flickering.
      particle.glitchRemainingMs = static_cast<uint16_t>(
          90.0f + nextRandomUnitFloat() * 230.0f);
      particle.glitchDirectionX = nextRandomUnitFloat() * 2.0f - 1.0f;
      particle.glitchDirectionY = nextRandomUnitFloat() * 2.0f - 1.0f;
      particle.glitchSpeedMultiplier =
          1.6f + nextRandomUnitFloat() * 1.8f;
      particle.timeUntilGlitchMs =
          12000UL + static_cast<uint32_t>(nextRandomUnitFloat() * 24000.0f);
    } else {
      particle.timeUntilGlitchMs -= elapsedMs;
    }

    // In screen coordinates Y points downward. (-y, x) is therefore the
    // clockwise tangent requested for the local filament motion.
    const float fieldOffsetX = oldX - vortexCenterX_;
    const float fieldOffsetY = oldY - vortexCenterY_;
    const float fieldRadiusSquared = fieldOffsetX * fieldOffsetX +
                                     fieldOffsetY * fieldOffsetY;
    const float radius = sqrtf(fieldRadiusSquared + 36.0f);
    const float radialX = fieldOffsetX / radius;
    const float radialY = fieldOffsetY / radius;
    const float tangentX = -fieldOffsetY / radius;
    const float tangentY = fieldOffsetX / radius;
    const float polarAngle = atan2f(fieldOffsetY, fieldOffsetX);

    // Polar harmonics replace the former independent X/Y waves that produced
    // an unintended rounded square. Radial and three-lobed variations retain
    // irregularity while respecting the circular display geometry.
    // A factor of 1.0 corresponds to the approved 50% visual baseline. The
    // small minimum values keep the calm setting alive rather than mechanical.
    const float turbulenceScale = currentTurbulence_ * 2.0f;
    const float radialFlowAmplitude = 0.04f + 0.18f * turbulenceScale;
    const float lobeAmplitude = 0.03f + 0.10f * turbulenceScale;
    const float secondaryVortexStrength = 0.08f + 0.08f * turbulenceScale;
    const float radialFlow =
        sinf(radius * 0.050f + polarAngle * 2.0f +
             fieldPhaseA_ * 0.01f) * radialFlowAmplitude;
    const float threeLobeVariation =
        sinf(polarAngle * 3.0f + fieldPhaseB_ * 0.01f) * lobeAmplitude;

    // A weak off-center clockwise vortex breaks perfect radial symmetry. The
    // final render transform rotates this complete asymmetry counter-clockwise.
    const float secondaryOffsetX = fieldOffsetX - 46.0f;
    const float secondaryOffsetY = fieldOffsetY + 31.0f;
    const float secondaryRadius = sqrtf(
        secondaryOffsetX * secondaryOffsetX +
        secondaryOffsetY * secondaryOffsetY + 64.0f);
    const float secondaryTangentX = -secondaryOffsetY / secondaryRadius;
    const float secondaryTangentY = secondaryOffsetX / secondaryRadius;

    float directionX = tangentX * (0.82f + threeLobeVariation) +
                       radialX * radialFlow +
                       secondaryTangentX * secondaryVortexStrength;
    float directionY = tangentY * (0.82f + threeLobeVariation) +
                       radialY * radialFlow +
                       secondaryTangentY * secondaryVortexStrength;

    if (particle.glitchRemainingMs > 0) {
      const float glitchDisplacementStrength =
          0.55f + 0.30f * turbulenceScale;
      directionX +=
          particle.glitchDirectionX * glitchDisplacementStrength;
      directionY +=
          particle.glitchDirectionY * glitchDisplacementStrength;
    }

    const float directionLength =
        sqrtf(directionX * directionX + directionY * directionY + 0.0001f);
    directionX /= directionLength;
    directionY /= directionLength;

    const float activeGlitchSpeed = particle.glitchRemainingMs > 0
                                        ? particle.glitchSpeedMultiplier
                                        : 1.0f;
    const float traveledDistance = kParticleSpeedPixelsPerSecond *
                                   particle.speedMultiplier *
                                   activeGlitchSpeed * elapsedSeconds;
    particle.x += directionX * traveledDistance;
    particle.y += directionY * traveledDistance;

    // Simulation coordinates follow the clockwise field. Rendering applies a
    // separate slow negative rotation, so the composition as a whole drifts
    // counter-clockwise while individual filaments continue clockwise.
    const float newCenterOffsetX = particle.x - kCanvasCenter;
    const float newCenterOffsetY = particle.y - kCanvasCenter;
    const float newRenderedX = kCanvasCenter +
        renderRotationCos * newCenterOffsetX -
        renderRotationSin * newCenterOffsetY;
    const float newRenderedY = kCanvasCenter +
        renderRotationSin * newCenterOffsetX +
        renderRotationCos * newCenterOffsetY;

    const int32_t oldPixelX = static_cast<int32_t>(oldRenderedX + 0.5f);
    const int32_t oldPixelY = static_cast<int32_t>(oldRenderedY + 0.5f);
    const int32_t newPixelX = static_cast<int32_t>(newRenderedX + 0.5f);
    const int32_t newPixelY = static_cast<int32_t>(newRenderedY + 0.5f);
    const uint32_t particleColor = monochromeColor(particle.grayLevel);

    // During some glitch frames, omit the line entirely. These deterministic
    // gaps create a restrained digital discontinuity instead of random noise.
    const bool drawGlitchGap =
        particle.glitchRemainingMs > 0 &&
        (renderedFrameCount_ + particleIndex) % 3U == 0;
    if (!drawGlitchGap) {
      canvas_.drawLine(
          oldPixelX, oldPixelY, newPixelX, newPixelY, particleColor);
      targetDisplay_->drawLine(
          oldPixelX, oldPixelY, newPixelX, newPixelY, particleColor);
    }

    particle.renderedX = newRenderedX;
    particle.renderedY = newRenderedY;

    // Screen Y grows downward, so atan2() angles advance clockwise on the
    // physical display. This directly maps the artwork into eight LED sectors.
    float renderedAngle = atan2f(newRenderedY - kCanvasCenter,
                                 newRenderedX - kCanvasCenter);
    if (renderedAngle < 0.0f) {
      renderedAngle += TWO_PI;
    }
    const uint8_t sectorIndex = static_cast<uint8_t>(
        renderedAngle * kSectorCount / TWO_PI) % kSectorCount;
    const float particleActivity = min(traveledDistance / 2.0f, 1.0f);
    frameSectorEnergy[sectorIndex] += particleActivity;
  }

  // A quick attack makes passing filaments readable; slower decay leaves a
  // short luminous wake. Dividing by twelve normalizes typical sector density.
  for (uint8_t sectorIndex = 0;
       sectorIndex < kSectorCount;
       ++sectorIndex) {
    const float targetEnergy =
        min(frameSectorEnergy[sectorIndex] / 12.0f, 1.0f);
    const float smoothing = targetEnergy > sectorEnergy_[sectorIndex]
                                ? 0.38f
                                : 0.10f;
    sectorEnergy_[sectorIndex] +=
        (targetEnergy - sectorEnergy_[sectorIndex]) * smoothing;
  }
}

void FlowPrototype::updateFrameRateMeasurement(uint32_t nowMs) {
  const uint32_t measurementDurationMs = nowMs - measurementWindowStartedMs_;
  if (measurementDurationMs < 1000UL) {
    return;
  }

  measuredFramesPerSecond_ =
      framesInMeasurementWindow_ * 1000.0f / measurementDurationMs;
  framesInMeasurementWindow_ = 0;
  measurementWindowStartedMs_ = nowMs;
}
