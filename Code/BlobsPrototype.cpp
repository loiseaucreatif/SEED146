#include "BlobsPrototype.h"

#include <math.h>
#include <string.h>

namespace {

// A fixed Bayer matrix makes the gray transition read as old-school ordered
// dithering. Because it is anchored to screen pixels, the grain stays calm
// while the underlying soft field moves through it.
constexpr uint8_t kBayer4x4[4][4] = {
    {0, 8, 2, 10},
    {12, 4, 14, 6},
    {3, 11, 1, 9},
    {15, 7, 13, 5},
};

// Stable broad anchors guarantee coverage before seed variation. Several lie
// outside the 360 x 360 viewport so BLOBS never becomes a centered collection
// of framed circles.
constexpr float kAnchorX[8] = {-52.0f, 104.0f, 292.0f, 408.0f,
                                356.0f, 184.0f, -34.0f, 174.0f};
constexpr float kAnchorY[8] = {64.0f, -42.0f, -28.0f, 154.0f,
                                354.0f, 414.0f, 316.0f, 184.0f};

}  // namespace

BlobsPrototype::BlobsPrototype(lgfx::LovyanGFX* targetDisplay)
    : targetDisplay_(targetDisplay), canvas_(targetDisplay) {}

bool BlobsPrototype::begin(uint32_t seed) {
  if (ready_) {
    return true;
  }

  // The field is displayed only as black or white, but LovyanGFX's validated
  // four-bit grayscale sprite keeps palette handling predictable and needs
  // only 64,800 bytes in PSRAM at full 360 x 360 resolution.
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

bool BlobsPrototype::regenerate(uint32_t seed) {
  if (!ready_) {
    return false;
  }
  initializeSeededComposition(seed);
  showCurrentFrame();
  return true;
}

void BlobsPrototype::showCurrentFrame() {
  if (!ready_) {
    return;
  }
  canvas_.pushSprite(0, 0);
  // Exclude time spent in the menu so returning cannot jump the material.
  lastFrameMs_ = millis();
  measurementWindowStartedMs_ = lastFrameMs_;
  framesInMeasurementWindow_ = 0;
  measuredFramesPerSecond_ = 0.0f;
}

void BlobsPrototype::setDrift(float normalizedValue) {
  const float dialPosition = constrain(normalizedValue, 0.0f, 1.0f);
  // Translate the whole exposed range toward more spatial tension. Its
  // minimum is the proven dense field state, avoiding the white composition
  // observed in the two earlier, lower-range physical tests.
  targetDrift_ = kMinimumInternalDrift +
      dialPosition * (1.0f - kMinimumInternalDrift);
}

float BlobsPrototype::edgeEnergy(uint8_t sectorIndex) const {
  return sectorIndex < kSectorCount ? edgeEnergy_[sectorIndex] : 0.0f;
}

float BlobsPrototype::nextRandomUnitFloat() {
  // A private xorshift sequence makes every seed exactly reproducible.
  randomState_ ^= randomState_ << 13U;
  randomState_ ^= randomState_ >> 17U;
  randomState_ ^= randomState_ << 5U;
  return (randomState_ & 0xFFFFFFU) / 16777216.0f;
}

void BlobsPrototype::initializeSeededComposition(uint32_t seed) {
  seed_ = seed;
  randomState_ = seed ? seed : 1U;
  currentDrift_ = targetDrift_;
  animationTimeSeconds_ = nextRandomUnitFloat() * 40.0f;

  for (uint8_t index = 0; index < kBlobCount; ++index) {
    Blob& blob = blobs_[index];
    // Larger seed displacement makes a short-click regeneration immediately
    // legible even when the material moves slowly between frames.
    blob.baseX = kAnchorX[index] + (nextRandomUnitFloat() - 0.5f) * 90.0f;
    blob.baseY = kAnchorY[index] + (nextRandomUnitFloat() - 0.5f) * 90.0f;
    blob.amplitudeX = 34.0f + nextRandomUnitFloat() * 72.0f;
    blob.amplitudeY = 34.0f + nextRandomUnitFloat() * 72.0f;
    blob.phaseX = nextRandomUnitFloat() * TWO_PI;
    blob.phaseY = nextRandomUnitFloat() * TWO_PI;

    // Independent 20–44 second base periods avoid a shared pulse or a
    // recognizable central rotation. Their tempo is fixed to the physically
    // approved former MATTER 100% baseline.
    const float periodX = 20.0f + nextRandomUnitFloat() * 24.0f;
    const float periodY = 20.0f + nextRandomUnitFloat() * 24.0f;
    blob.angularSpeedX = TWO_PI / periodX;
    blob.angularSpeedY = TWO_PI / periodY;
    // Step 7.2 pulls the camera back again: these influence radii expose
    // several separate masses and their necks at once while outside anchors
    // still keep the scalar model larger than the viewport.
    blob.radius = 50.0f + nextRandomUnitFloat() * 28.0f;

  }

  updateBlobCenters();
  updateEdgeEnergy();
  renderDitheredField();
  lastFrameMs_ = millis();
  measurementWindowStartedMs_ = lastFrameMs_;
  framesInMeasurementWindow_ = 0;
  measuredFramesPerSecond_ = 0.0f;
}

void BlobsPrototype::updateBlobCenters() {
  float weightedCenterX = 0.0f;
  float weightedCenterY = 0.0f;
  float totalWeight = 0.0f;

  for (uint8_t index = 0; index < kBlobCount; ++index) {
    const Blob& blob = blobs_[index];
    // At the internal 50% field value this is the approved baseline. The
    // user-facing dial is deliberately translated above that value so its
    // minimum does not expose an overly white composition.
    const float wanderScale = 0.55f + currentDrift_ * 0.90f;
    // A weak second harmonic bends each path away from a simple ellipse while
    // remaining continuous and deterministic.
    blobCenterX_[index] =
        blob.baseX + sinf(animationTimeSeconds_ * blob.angularSpeedX +
                          blob.phaseX) * blob.amplitudeX * wanderScale +
        sinf(animationTimeSeconds_ * blob.angularSpeedY * 0.43f +
             blob.phaseY * 1.7f) * blob.amplitudeX * 0.22f * wanderScale;
    blobCenterY_[index] =
        blob.baseY + sinf(animationTimeSeconds_ * blob.angularSpeedY +
                          blob.phaseY) * blob.amplitudeY * wanderScale +
        cosf(animationTimeSeconds_ * blob.angularSpeedX * 0.39f +
             blob.phaseX * 1.3f) * blob.amplitudeY * 0.22f * wanderScale;

    weightedCenterX += blobCenterX_[index];
    weightedCenterY += blobCenterY_[index];
    totalWeight += 1.0f;
  }

  if (totalWeight > 0.0f) {
    fieldCenterX_ = weightedCenterX / totalWeight;
    fieldCenterY_ = weightedCenterY / totalWeight;
  }
}

float BlobsPrototype::calculateSoftFieldLevel(float fieldX, float fieldY) const {
  // These are the same density, radius and soft-threshold coefficients used
  // by the screen renderer. LED energy therefore means visible material at
  // the edge, rather than proximity to an invisible simplified simulation.
  const float tensionOffset = (currentDrift_ - 0.5f) * 0.30f;
  const float densityScale = 1.15f - tensionOffset * 0.33f;
  const float radiusScale = 1.05f - tensionOffset * 0.40f;
  const float softFieldStart = 0.51f + tensionOffset;
  const float softFieldEnd = 2.05f + tensionOffset;
  float fieldStrength = 0.0f;

  for (uint8_t blobIndex = 0; blobIndex < kBlobCount; ++blobIndex) {
    const float offsetX = fieldX - blobCenterX_[blobIndex];
    const float offsetY = fieldY - blobCenterY_[blobIndex];
    const float radius = blobs_[blobIndex].radius * radiusScale;
    const float radiusSquared = radius * radius;
    const float distanceSquared = offsetX * offsetX + offsetY * offsetY;
    fieldStrength += densityScale * radiusSquared /
                     (distanceSquared + radiusSquared * 0.20f);
  }

  return constrain((fieldStrength - softFieldStart) /
                       (softFieldEnd - softFieldStart),
                   0.0f,
                   1.0f);
}

void BlobsPrototype::updateEdgeEnergy() {
  constexpr float kSectorAngle = TWO_PI / kSectorCount;
  constexpr float kProbeAngleOffset = kSectorAngle * 0.28f;
  constexpr float kVisibleMaterialThreshold = 0.56f;

  for (uint8_t sectorIndex = 0; sectorIndex < kSectorCount; ++sectorIndex) {
    const float sectorCenterAngle = sectorIndex * kSectorAngle;
    float strongestProbeLevel = 0.0f;

    // Sampling the center and two nearby angles makes a broad blob contact
    // light one coherent LED zone without triggering a neighboring sector
    // merely from one dithered edge pixel.
    for (int8_t probe = -1; probe <= 1; ++probe) {
      const float probeAngle = sectorCenterAngle + probe * kProbeAngleOffset;
      const float probeX = kScreenCenter + cosf(probeAngle) * kEdgeProbeRadius;
      const float probeY = kScreenCenter + sinf(probeAngle) * kEdgeProbeRadius;
      strongestProbeLevel = max(strongestProbeLevel,
                                calculateSoftFieldLevel(probeX, probeY));
    }

    const float targetEnergy = constrain(
        (strongestProbeLevel - kVisibleMaterialThreshold) /
            (1.0f - kVisibleMaterialThreshold),
        0.0f,
        1.0f);
    // Fast arrival makes contact legible; the slower fall makes retreat feel
    // viscous instead of producing harsh, frame-rate-limited LED flicker.
    const float response = targetEnergy > edgeEnergy_[sectorIndex]
                               ? 0.42f
                               : 0.10f;
    edgeEnergy_[sectorIndex] +=
        (targetEnergy - edgeEnergy_[sectorIndex]) * response;
  }
}

void BlobsPrototype::renderDitheredField() {
  // Two four-bit pixels share each byte: the even X pixel is the high nibble
  // and the odd X pixel is the low nibble. Writing the complete packed buffer
  // directly avoids tens of thousands of palette conversions and virtual
  // drawPixel calls per frame while preserving exactly the same black/white
  // Bayer pattern.
  uint8_t* storedPixels = static_cast<uint8_t*>(canvas_.getBuffer());
  if (storedPixels == nullptr) {
    return;
  }
  constexpr uint16_t kPackedRowBytes = kCanvasWidth / 2U;
  for (uint16_t gridX = 0; gridX < kFieldGridSize; ++gridX) {
    currentFieldRow_[gridX] =
        calculateSoftFieldLevel(gridX * kFieldSampleSize, 0.0f);
  }

  for (uint16_t gridY = 0; gridY < kFieldGridSize - 1U; ++gridY) {
    const float nextFieldY = (gridY + 1U) * kFieldSampleSize;
    for (uint16_t gridX = 0; gridX < kFieldGridSize; ++gridX) {
      nextFieldRow_[gridX] =
          calculateSoftFieldLevel(gridX * kFieldSampleSize, nextFieldY);
    }

    for (uint16_t gridX = 0; gridX < kFieldGridSize - 1U; ++gridX) {
      const float topLeft = currentFieldRow_[gridX];
      const float topRight = currentFieldRow_[gridX + 1U];
      const float bottomLeft = nextFieldRow_[gridX];
      const float bottomRight = nextFieldRow_[gridX + 1U];
      const uint16_t cellStartX = gridX * kFieldSampleSize;
      const uint16_t cellStartY = gridY * kFieldSampleSize;

      for (uint8_t localY = 0; localY < kFieldSampleSize; ++localY) {
        const float verticalFraction =
            (localY + 0.5f) / kFieldSampleSize;
        const float leftLevel = topLeft +
            (bottomLeft - topLeft) * verticalFraction;
        const float rightLevel = topRight +
            (bottomRight - topRight) * verticalFraction;
        const uint16_t screenY = cellStartY + localY;
        uint8_t* row = storedPixels +
            static_cast<size_t>(screenY) * kPackedRowBytes + gridX * 2U;

        for (uint8_t pairIndex = 0; pairIndex < 2U; ++pairIndex) {
          uint8_t packedPair = 0;
          for (uint8_t pixelInPair = 0; pixelInPair < 2U; ++pixelInPair) {
            const uint8_t localX = pairIndex * 2U + pixelInPair;
            const float horizontalFraction =
                (localX + 0.5f) / kFieldSampleSize;
            const float softLevel = leftLevel +
                (rightLevel - leftLevel) * horizontalFraction;
            const uint8_t whiteThresholdCount = static_cast<uint8_t>(
                constrain(static_cast<int>(softLevel * 16.0f + 0.5f), 0, 16));
            const uint16_t screenX = cellStartX + localX;
            if (whiteThresholdCount >
                kBayer4x4[screenY & 3U][screenX & 3U]) {
              packedPair |= pixelInPair == 0U ? 0xF0U : 0x0FU;
            }
          }
          row[pairIndex] = packedPair;
        }
      }
    }
    memcpy(currentFieldRow_, nextFieldRow_, sizeof(currentFieldRow_));
  }
}

void BlobsPrototype::update(uint32_t nowMs) {
  if (!ready_) {
    return;
  }
  const uint32_t elapsedMs = nowMs - lastFrameMs_;
  if (elapsedMs < kTargetFramePeriodMs) {
    return;
  }
  lastFrameMs_ = nowMs;
  const uint32_t frameStartedUs = micros();
  const float elapsedSeconds = min(elapsedMs, 120UL) / 1000.0f;

  currentDrift_ += constrain(
      targetDrift_ - currentDrift_,
      -elapsedSeconds * kDriftResponsePerSecond,
      elapsedSeconds * kDriftResponsePerSecond);
  // Keep the physically approved former MATTER 100% tempo fixed. DRIFT must
  // change spatial tension and wander, never make the instrument hurry.
  constexpr float kApprovedMotionTimeScale = 1.55f;
  animationTimeSeconds_ += elapsedSeconds * kApprovedMotionTimeScale;
  updateBlobCenters();
  updateEdgeEnergy();
  renderDitheredField();
  canvas_.pushSprite(0, 0);

  slowestFrameTimeUs_ = max(slowestFrameTimeUs_, micros() - frameStartedUs);
  ++renderedFrameCount_;
  updateFrameRateMeasurement(nowMs);
}

void BlobsPrototype::updateFrameRateMeasurement(uint32_t nowMs) {
  ++framesInMeasurementWindow_;
  const uint32_t measurementDurationMs = nowMs - measurementWindowStartedMs_;
  if (measurementDurationMs >= 1000U) {
    measuredFramesPerSecond_ =
        framesInMeasurementWindow_ * 1000.0f / measurementDurationMs;
    framesInMeasurementWindow_ = 0;
    measurementWindowStartedMs_ = nowMs;
  }
}
