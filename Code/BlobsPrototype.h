#ifndef SEED146_BLOBS_PROTOTYPE_H
#define SEED146_BLOBS_PROTOTYPE_H

#include <Arduino.h>
#include <LovyanGFX.h>

// BLOBS renders a continuous scalar field rather than independent outlined
// objects. Several mass centers live beyond the display, so the circular
// viewport always feels cut from a larger field of material.
class BlobsPrototype {
 public:
  // Sector 0 begins at screen-right; indices advance clockwise, matching the
  // existing screen-to-ring convention used by the other edge-reactive art.
  static constexpr uint8_t kSectorCount = 8;

  explicit BlobsPrototype(lgfx::LovyanGFX* targetDisplay);

  bool begin(uint32_t seed);
  bool regenerate(uint32_t seed);
  void showCurrentFrame();
  void update(uint32_t nowMs);
  // DRIFT mixes the amplitude of each mass's wandering path with its field
  // cohesion. It deliberately leaves the approved animation tempo unchanged.
  void setDrift(float normalizedValue);

  bool isReady() const { return ready_; }
  uint32_t seed() const { return seed_; }
  uint32_t renderedFrameCount() const { return renderedFrameCount_; }
  float measuredFramesPerSecond() const { return measuredFramesPerSecond_; }
  uint32_t slowestFrameTimeUs() const { return slowestFrameTimeUs_; }
  float fieldCenterX() const { return fieldCenterX_; }
  float fieldCenterY() const { return fieldCenterY_; }
  // Brightness of material actually reaching each circular screen-edge sector.
  float edgeEnergy(uint8_t sectorIndex) const;
  size_t canvasSizeBytes() const {
    return (kCanvasWidth * kCanvasHeight * kCanvasBitsPerPixel + 7U) / 8U;
  }

 private:
  struct Blob {
    float baseX;
    float baseY;
    float amplitudeX;
    float amplitudeY;
    float phaseX;
    float phaseY;
    float angularSpeedX;
    float angularSpeedY;
    float radius;
  };

  static constexpr uint16_t kCanvasWidth = 360;
  static constexpr uint16_t kCanvasHeight = 360;
  static constexpr float kScreenCenter = 180.0f;
  static constexpr float kEdgeProbeRadius = 172.0f;
  static constexpr uint8_t kCanvasBitsPerPixel = 4;
  static constexpr uint8_t kBlobCount = 8;
  // The expensive metaball field is sampled on a four-pixel grid, then
  // bilinearly interpolated back to every display pixel before dithering.
  // This retains soft edges while reducing field divisions by about four.
  static constexpr uint8_t kFieldSampleSize = 4;
  static constexpr uint16_t kFieldGridSize =
      kCanvasWidth / kFieldSampleSize + 1U;
  static constexpr uint32_t kTargetFramePeriodMs = 33;
  // The first translated minimum was still too white on the physical display.
  // Keep the user-facing dial at 0–100%, but start its internal window at the
  // proven dense 50% field state. The upper endpoint remains unchanged.
  static constexpr float kMinimumInternalDrift = 0.50f;
  static constexpr float kDriftResponsePerSecond = 0.22f;

  lgfx::LovyanGFX* targetDisplay_;
  lgfx::LGFX_Sprite canvas_;
  Blob blobs_[kBlobCount] = {};
  float blobCenterX_[kBlobCount] = {};
  float blobCenterY_[kBlobCount] = {};
  float currentFieldRow_[kFieldGridSize] = {};
  float nextFieldRow_[kFieldGridSize] = {};
  float edgeEnergy_[kSectorCount] = {};

  bool ready_ = false;
  uint32_t seed_ = 0;
  uint32_t randomState_ = 1;
  float targetDrift_ = 0.5f;
  float currentDrift_ = 0.5f;
  float animationTimeSeconds_ = 0.0f;
  float fieldCenterX_ = 180.0f;
  float fieldCenterY_ = 180.0f;

  uint32_t lastFrameMs_ = 0;
  uint32_t renderedFrameCount_ = 0;
  uint32_t slowestFrameTimeUs_ = 0;
  uint32_t framesInMeasurementWindow_ = 0;
  uint32_t measurementWindowStartedMs_ = 0;
  float measuredFramesPerSecond_ = 0.0f;

  float nextRandomUnitFloat();
  void initializeSeededComposition(uint32_t seed);
  void updateBlobCenters();
  float calculateSoftFieldLevel(float fieldX, float fieldY) const;
  void updateEdgeEnergy();
  void renderDitheredField();
  void updateFrameRateMeasurement(uint32_t nowMs);
};

#endif  // SEED146_BLOBS_PROTOTYPE_H
