#include "ParticlesPrototype.h"
#include <math.h>

namespace {
uint32_t monochromeColor(uint8_t gray) {
  return (uint32_t(gray) << 16U) | (uint32_t(gray) << 8U) | gray;
}
uint16_t swapBytes16(uint16_t value) {
  return static_cast<uint16_t>((value << 8U) | (value >> 8U));
}
}

ParticlesPrototype::ParticlesPrototype(lgfx::LovyanGFX* targetDisplay)
    : targetDisplay_(targetDisplay), canvas_(targetDisplay) {}

bool ParticlesPrototype::begin(uint32_t seed) {
  if (ready_) return true;
  // One canvas in external PSRAM, allocated only on first menu selection.
  canvas_.setColorDepth(16);
  canvas_.setPsram(true);
  if (!canvas_.createSprite(kCanvasWidth, kCanvasHeight)) return false;
  ready_ = true;
  initializeSeededComposition(seed);
  return true;
}
bool ParticlesPrototype::regenerate(uint32_t seed) {
  if (!ready_) return false;
  initializeSeededComposition(seed);
  showCurrentFrame();
  return true;
}
void ParticlesPrototype::showCurrentFrame() {
  if (!ready_) return;
  canvas_.pushSprite(0, 0);
  // Resume from the preserved state, excluding menu time from motion and FPS.
  lastFrameMs_ = millis();
  measurementWindowStartedMs_ = lastFrameMs_;
  framesInMeasurementWindow_ = 0;
  measuredFramesPerSecond_ = 0;
}
void ParticlesPrototype::setEnergy(float value) {
  targetEnergy_ = constrain(value, 0.0f, 1.0f);
}
const char* ParticlesPrototype::familyName() const {
  return "MIX";
}
float ParticlesPrototype::edgeImpulse(uint8_t sector) const {
  return sector < kSectorCount ? edgeImpulse_[sector] : 0.0f;
}
float ParticlesPrototype::randomUnit() {
  // Local xorshift sequence: the same seed reproduces the event sequence.
  randomState_ ^= randomState_ << 13U;
  randomState_ ^= randomState_ >> 17U;
  randomState_ ^= randomState_ << 5U;
  return (randomState_ & 0xFFFFFFU) / 16777216.0f;
}
void ParticlesPrototype::initializeSeededComposition(uint32_t seed) {
  seed_ = seed;
  randomState_ = seed ? seed : 1;
  // Seeds vary the balance and model-space jet axis; all three behaviors
  // remain present. The complete model has a separate continuous rotation.
  axisAngle_ = randomUnit() * TWO_PI;
  burstRateScale_ = 0.8f + randomUnit() * 0.4f;
  jetRateScale_ = 0.8f + randomUnit() * 0.4f;
  globalRotation_ = 0;
  previousGlobalRotation_ = 0;
  currentEnergy_ = targetEnergy_;
  nextBounceIndex_ = nextBurstIndex_ = nextCrossIndex_ = 0;
  emissionAccumulator_ = 0;
  eventCountdownSeconds_ = 0;
  fadePhase_ = 0;
  for (auto& particle : particles_) particle = {};
  for (uint8_t sector = 0; sector < kSectorCount; ++sector) {
    edgeImpulse_[sector] = 0;
    edgeCooldown_[sector] = 0;
  }
  canvas_.fillScreen(TFT_BLACK);
  for (uint16_t index = 0; index < kBounceCount; ++index) {
    const float angle = randomUnit() * TWO_PI;
    const float radius = sqrtf(randomUnit()) * (kBoundaryRadius - 10);
    emitParticle(kCenter + cosf(angle) * radius,
                 kCenter + sinf(angle) * radius, randomUnit() * TWO_PI,
                 Family::bounce);
  }
  triggerEvent();
  lastFrameMs_ = millis();
  measurementWindowStartedMs_ = lastFrameMs_;
  framesInMeasurementWindow_ = 0;
  measuredFramesPerSecond_ = 0;
}
void ParticlesPrototype::emitParticle(float x, float y, float angle, Family behavior) {
  uint16_t slot;
  switch (behavior) {
    case Family::bounce:
      slot = nextBounceIndex_;
      nextBounceIndex_ = (nextBounceIndex_ + 1U) % kBounceCount;
      break;
    case Family::burst:
      slot = kBounceCount + nextBurstIndex_;
      nextBurstIndex_ = (nextBurstIndex_ + 1U) % kBurstCount;
      break;
    case Family::cross:
      slot = kBounceCount + kBurstCount + nextCrossIndex_;
      nextCrossIndex_ = (nextCrossIndex_ + 1U) % kCrossCount;
      break;
  }
  Particle& particle = particles_[slot];
  const float speed = 65.0f + randomUnit() * 100.0f;
  particle = {x, y, cosf(angle) * speed, sinf(angle) * speed,
              1.2f + randomUnit() * 2.0f,
              static_cast<uint8_t>(100 + randomUnit() * 155), false, behavior};
}
void ParticlesPrototype::triggerEvent() {
  // Salvos jump between off-center sources in the rotating model space.
  const float angle = randomUnit() * TWO_PI;
  const float radius = 35.0f + randomUnit() * 65.0f;
  const float sourceX = kCenter + cosf(angle) * radius;
  const float sourceY = kCenter + sinf(angle) * radius;
  eventCountdownSeconds_ = (0.45f + randomUnit() * 0.7f) /
                           ((0.65f + currentEnergy_ * 1.7f) * burstRateScale_);
  {
    const uint16_t count = 16 + static_cast<uint16_t>(currentEnergy_ * 16);
    for (uint16_t index = 0; index < count; ++index) {
      emitParticle(sourceX, sourceY, randomUnit() * TWO_PI, Family::burst);
    }
  }
  {
    // A localized outward kick changes autonomous velocities. The soft radius
    // avoids a singularity at the source; a speed cap keeps updates bounded.
    for (auto& particle : particles_) {
      if (particle.behavior != Family::bounce) continue;
      const float offsetX = particle.x - sourceX;
      const float offsetY = particle.y - sourceY;
      const float distance = sqrtf(offsetX * offsetX + offsetY * offsetY + 1);
      if (distance < kLocalImpulseRadius) {
        const float impulse = (40 + currentEnergy_ * 160) *
                              (1 - distance / kLocalImpulseRadius);
        particle.velocityX += offsetX / distance * impulse;
        particle.velocityY += offsetY / distance * impulse;
      }
    }
  }
}
void ParticlesPrototype::registerEdgeContact(const Particle& particle) {
  // Only bright particles excite the LEDs. A per-sector cooldown prevents
  // dense salvos from holding a sector permanently lit.
  if (particle.grayLevel < 220) return;
  // Match the displayed contact after global rotation, not its model sector.
  float angle = atan2f(particle.y - kCenter, particle.x - kCenter) + globalRotation_;
  if (angle >= TWO_PI) angle -= TWO_PI;
  if (angle < 0) angle += TWO_PI;
  const uint8_t sector = static_cast<uint8_t>(angle * kSectorCount / TWO_PI) % kSectorCount;
  if (edgeCooldown_[sector] > 0) return;
  edgeImpulse_[sector] = 1;
  edgeCooldown_[sector] = 0.55f;
}
void ParticlesPrototype::simulateAndDraw(float elapsedSeconds) {
  const float speedScale = 0.55f + currentEnergy_ * 1.65f;
  eventCountdownSeconds_ -= elapsedSeconds;
  if (eventCountdownSeconds_ <= 0) triggerEvent();
  {
    emissionAccumulator_ += elapsedSeconds * (18 + currentEnergy_ * 28) * jetRateScale_;
    while (emissionAccumulator_ >= 1) {
      emissionAccumulator_ -= 1;
      // Two opposing jets have seed-fixed axes and finite width. Each point
      // receives a single scattering kick on entering their meeting region.
      const float direction = randomUnit() < 0.5f ? 1.0f : -1.0f;
      const float sideways = (randomUnit() - 0.5f) * 35;
      const float cosine = cosf(axisAngle_), sine = sinf(axisAngle_);
      emitParticle(kCenter - cosine * 164 * direction - sine * sideways,
                   kCenter - sine * 164 * direction + cosine * sideways,
                   axisAngle_ + (direction < 0 ? PI : 0) + (randomUnit() - 0.5f) * 0.10f, Family::cross);
    }
  }
  // Only four trig calls per frame. Old endpoints use the previous angle,
  // so the short stroke includes both local motion and whole-model rotation.
  const float oldCosine = cosf(previousGlobalRotation_);
  const float oldSine = sinf(previousGlobalRotation_);
  const float cosine = cosf(globalRotation_);
  const float sine = sinf(globalRotation_);
  for (auto& particle : particles_) {
    if (particle.behavior != Family::bounce) {
      particle.remainingLifeSeconds -= elapsedSeconds;
      if (particle.remainingLifeSeconds <= 0) continue;
    }
    const float oldX = particle.x, oldY = particle.y;
    if (particle.behavior == Family::cross && !particle.scattered &&
        hypotf(oldX - kCenter, oldY - kCenter) < 42) {
      particle.scattered = true;
      const float kick = (randomUnit() - 0.5f) * (100 + currentEnergy_ * 240);
      particle.velocityX -= sinf(axisAngle_) * kick;
      particle.velocityY += cosf(axisAngle_) * kick;
    }
    const float speed = hypotf(particle.velocityX, particle.velocityY);
    if (speed > kMaximumSpeed) {
      particle.velocityX *= kMaximumSpeed / speed;
      particle.velocityY *= kMaximumSpeed / speed;
    }
    particle.x += particle.velocityX * speedScale * elapsedSeconds;
    particle.y += particle.velocityY * speedScale * elapsedSeconds;
    const float offsetX = particle.x - kCenter, offsetY = particle.y - kCenter;
    const float radius = hypotf(offsetX, offsetY);
    if (radius >= kBoundaryRadius) {
      const float normalX = offsetX / radius, normalY = offsetY / radius;
      particle.x = kCenter + normalX * (kBoundaryRadius - 0.5f);
      particle.y = kCenter + normalY * (kBoundaryRadius - 0.5f);
      registerEdgeContact(particle);
      if (particle.behavior == Family::bounce) {
        // Reflect only outward velocity on the circular wall. This is a local
        // impact, not a tangential force or rotation of the composition.
        const float outwardSpeed = particle.velocityX * normalX + particle.velocityY * normalY;
        if (outwardSpeed > 0) {
          particle.velocityX -= 2 * outwardSpeed * normalX;
          particle.velocityY -= 2 * outwardSpeed * normalY;
        }
      } else {
        particle.remainingLifeSeconds = 0;
      }
    }
    const float oldScreenX = kCenter + oldCosine * (oldX - kCenter) - oldSine * (oldY - kCenter);
    const float oldScreenY = kCenter + oldSine * (oldX - kCenter) + oldCosine * (oldY - kCenter);
    const float screenX = kCenter + cosine * (particle.x - kCenter) - sine * (particle.y - kCenter);
    const float screenY = kCenter + sine * (particle.x - kCenter) + cosine * (particle.y - kCenter);
    const uint32_t color = monochromeColor(particle.grayLevel);
    canvas_.drawLine(oldScreenX, oldScreenY, screenX, screenY, color);
    targetDisplay_->drawLine(oldScreenX, oldScreenY, screenX, screenY, color);
    // Sparse brighter heads make individual particles readable among wakes.
    if (particle.grayLevel >= 220) {
      canvas_.fillRect(screenX, screenY, 2, 2, color);
      targetDisplay_->fillRect(screenX, screenY, 2, 2, color);
    }
  }
}
void ParticlesPrototype::update(uint32_t nowMs) {
  if (!ready_) return;
  const uint32_t elapsedMs = nowMs - lastFrameMs_;
  if (elapsedMs < kTargetFramePeriodMs) return;
  lastFrameMs_ = nowMs;
  const uint32_t startedUs = micros();
  // Clamp debugger stalls and smooth ENERGY without reallocating particles.
  const float elapsedSeconds = min(elapsedMs, 50UL) / 1000.0f;
  currentEnergy_ += constrain(targetEnergy_ - currentEnergy_,
                              -elapsedSeconds * 1.5f, elapsedSeconds * 1.5f);
  for (uint8_t sector = 0; sector < kSectorCount; ++sector) {
    edgeImpulse_[sector] = max(0.0f, edgeImpulse_[sector] - elapsedSeconds * 3.5f);
    edgeCooldown_[sector] = max(0.0f, edgeCooldown_[sector] - elapsedSeconds);
  }
  previousGlobalRotation_ = globalRotation_;
  globalRotation_ += kGlobalRotationRadiansPerSecond * elapsedSeconds;
  if (globalRotation_ >= TWO_PI) globalRotation_ -= TWO_PI;
  fadePartOfCanvas();
  targetDisplay_->startWrite();
  simulateAndDraw(elapsedSeconds);
  pushFadedRowsToDisplay();
  targetDisplay_->endWrite();
  slowestFrameTimeUs_ = max(slowestFrameTimeUs_, micros() - startedUs);
  ++renderedFrameCount_;
  ++framesInMeasurementWindow_;
  const uint32_t windowMs = nowMs - measurementWindowStartedMs_;
  if (windowMs >= 1000) {
    measuredFramesPerSecond_ = framesInMeasurementWindow_ * 1000.0f / windowMs;
    framesInMeasurementWindow_ = 0;
    measurementWindowStartedMs_ = nowMs;
  }
}

void ParticlesPrototype::fadePartOfCanvas() {
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

void ParticlesPrototype::pushFadedRowsToDisplay() {
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
