/*
 * SEED146 - CrowPanel 1.46inch gen-art experiment
 * By Loiseau creatif - version 1.0 - 2026
 *
 * A conceptual art piece displaying generative art to help you focus, escape,
 * or simply decorate your home.
 *
 * Link to the project:
 * https://github.com/loiseaucreatif/SEED146
 *
 * Link to the product:
 * https://www.elecrow.com/crowpanel-1-46inch-hmi-esp32-rotary-display-360-360-ips-round-touch-knob-screen.html?idd=5
 *
 * I wrote this code with the help of Codex.
 * This experiment can be a source of inspiration and information for Makers.
 *
 * Enjoy!
 */

#include <Arduino.h>
#include <Adafruit_NeoPixel.h>
#include <LovyanGFX.h>
#include <Wire.h>
#include <cst816t.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <driver/gpio.h>
#include <esp_err.h>
#include <esp_sleep.h>
#include <esp_system.h>
#include <lvgl.h>
#include <Preferences.h>

#include "ElecrowDHR55146DPanel.h"
#include "AttractorPrototype.h"
#include "BlobsPrototype.h"
#include "CellularPrototype.h"
#include "FlowPrototype.h"
#include "MenuIconAssets.h"
#include "LogoAssets.h"
#include "SplashAssets.h"
#include "OrbitPrototype.h"
#include "RadialPrototype.h"
#include "ParticlesPrototype.h"

#if !defined(CONFIG_IDF_TARGET_ESP32S3)
#error "seed146 requires an ESP32-S3 target. Select: ESP32S3 Dev Module."
#endif

namespace {

// Set this to true when a future hardware test needs the Serial Monitor again.
// False keeps the approved v1.0 release quiet and skips periodic test work.
constexpr bool kSerialDiagnosticsEnabled = false;
constexpr uint32_t kSerialBaud = 115200;
constexpr uint32_t kSerialWaitMs = 3000;
constexpr uint32_t kHeartbeatPeriodMs = 2000;
constexpr size_t kPsramProbeSize = 1024U * 1024U;

// Keep diagnostic calls in place so they can be restored with one switch.
// When disabled, the compiler removes the Serial calls and their formatting.
template <typename... Args>
void diagnosticPrintf(const char* format, Args... args) {
  if (kSerialDiagnosticsEnabled) {
    Serial.printf(format, args...);
  }
}

void diagnosticPrintln() {
  if (kSerialDiagnosticsEnabled) {
    Serial.println();
  }
}

template <typename T>
void diagnosticPrintln(const T& message) {
  if (kSerialDiagnosticsEnabled) {
    Serial.println(message);
  }
}

constexpr uint8_t kLedPowerEnablePin = 17;
constexpr uint8_t kLedDataPin = 48;
constexpr uint8_t kLedCount = 8;
// This compile-time default lives in program memory (ROM/flash). A later
// SETTINGS control may replace the active value and persist that choice in NVS.
constexpr uint8_t kDefaultLedMaximumPowerPercent = 100;

constexpr uint8_t kEncoderAPin = 45;
constexpr uint8_t kEncoderBPin = 42;
constexpr uint8_t kEncoderButtonPin = 41;
// Measured on the DHR55146D: one physical detent produces two valid AB
// transitions. Using four would incorrectly report only every second detent.
constexpr int8_t kTransitionsPerDetent = 2;
// A frame of BLOBS can take more than 100 ms, so button edges must be captured
// outside the render loop. This short ISR-side debounce filters switch bounce
// without rejecting a deliberately brief physical click.
constexpr uint32_t kButtonInterruptDebounceUs = 6000;
constexpr uint32_t kLongPressMs = 800;
constexpr uint32_t kClickFeedbackMs = 250;
constexpr uint32_t kLongPressFeedbackMs = 400;

constexpr uint8_t kDisplaySclkPin = 10;
constexpr uint8_t kDisplayMosiPin = 11;
constexpr uint8_t kDisplayDcPin = 3;
constexpr uint8_t kDisplayCsPin = 9;
constexpr uint8_t kDisplayResetPin = 14;
constexpr uint8_t kDisplayBacklightPin = 46;
constexpr uint16_t kScreenWidth = 360;
constexpr uint16_t kScreenHeight = 360;
// 80 MHz from Elecrow's full demo produced corrupted vertical bands on the
// tested unit. The independently working MakerGuides baseline uses 60 MHz.
constexpr uint32_t kDisplayWriteFrequencyHz = 60000000;
constexpr uint32_t kDisplayReadFrequencyHz = 20000000;
constexpr uint32_t kBacklightPwmFrequencyHz = 5000;
constexpr uint8_t kBacklightPwmResolution = 8;
constexpr uint8_t kInitialBacklightPercent = 50;

// The touch controller communicates over I2C: two shared wires carry its
// register data. GPIO 5 is a separate interrupt line which tells the ESP32
// that a new touch sample is ready, avoiding constant I2C polling.
constexpr uint8_t kTouchSdaPin = 6;
constexpr uint8_t kTouchSclPin = 7;
constexpr uint8_t kTouchResetPin = 13;
constexpr uint8_t kTouchInterruptPin = 5;
constexpr uint8_t kTouchI2cAddress = 0x15;
constexpr uint32_t kTouchI2cFrequencyHz = 400000;
// Touch mode can produce an event every 10 ms. Limit serial messages so they
// remain readable while forwarding every valid sample to LVGL.
constexpr uint32_t kTouchSerialPeriodMs = 50;
// The controller normally sends continuous samples every 10 ms. If a release
// interrupt is missed, this timeout prevents LVGL from keeping a stuck finger.
constexpr uint32_t kTouchReleaseTimeoutMs = 80;
// The finger can touch the display while pressing the encoder shaft. Wait
// briefly after touch release so a button edge from that same gesture wins.
constexpr uint32_t kSettingsTouchButtonPriorityMs = 150;
// The CST816D's hardware double-click event did not fire on the physical
// panel. Two separate press edges in its validated touch mode are reliable.
constexpr uint32_t kArtworkDoubleTouchGapMs = 500;
constexpr uint32_t kStandbyTouchPollingDelayMs = 20;
constexpr uint64_t kStandbySafetyWakeIntervalUs = 10ULL * 1000ULL * 1000ULL;

// LVGL renders a small horizontal strip rather than allocating a complete
// 360 x 360 frame buffer. Forty RGB565 lines use 28,800 bytes and are then
// transferred to the display whenever LVGL finishes drawing a region.
constexpr uint16_t kLvglBufferLineCount = 40;
constexpr uint8_t kRgb565BytesPerPixel = 2;
constexpr uint32_t kLvglLoopDelayMs = 5;

// Two visible seconds, measured after the complete image reaches the panel.
// The ring makes one synchronized pulse, with its green peak halfway through.
constexpr uint32_t kSplashScreenDurationMs = 2000;
constexpr uint32_t kSplashLedRiseDurationMs = kSplashScreenDurationMs / 2;
constexpr uint32_t kSplashLedFramePeriodMs = 5;
constexpr uint32_t kSplashFullHueTurn = 65536;
constexpr uint16_t kSplashGreenHue = 21845;
// This identifier is intentionally independent from the Arduino core version.
// It identifies the seed146 application shown at the bottom of SETTINGS.
constexpr char kApplicationVersion[] = "v1.0";
constexpr uint32_t kInitialFlowSeed = 146001;
constexpr uint8_t kInitialFlowTurbulencePercent = 50;
constexpr uint8_t kFlowTurbulenceStepPercent = 5;
constexpr uint32_t kInitialRadialSeed = 146002;
constexpr uint8_t kInitialRadialTwistPercent = 50;
constexpr uint8_t kRadialTwistStepPercent = 5;
constexpr uint32_t kInitialAttractorSeed = 146003;
constexpr uint8_t kInitialAttractorChaosPercent = 50;
constexpr uint8_t kAttractorChaosStepPercent = 5;
constexpr uint32_t kInitialOrbitSeed = 146005;
constexpr uint8_t kInitialOrbitEccentricityPercent = 50;
constexpr uint8_t kOrbitEccentricityStepPercent = 5;
constexpr uint32_t kInitialBlobsSeed = 146006;
constexpr uint8_t kInitialBlobsDriftPercent = 50;
constexpr uint8_t kBlobsDriftStepPercent = 5;
constexpr uint32_t kInitialCellularSeed = 146007;
constexpr uint8_t kInitialCellularMutationPercent = 50;
constexpr uint8_t kCellularMutationStepPercent = 5;
constexpr uint32_t kParameterThresholdPulseOnMs = 110;
constexpr uint32_t kParameterThresholdPulseOffMs = 75;
constexpr uint32_t kParameterLedFeedbackDurationMs = 1200;
constexpr uint32_t kArtworkLedFramePeriodMs = 33;
// Each ambient LED frame moves 6% toward its new RGB target. At about 30 FPS,
// this creates roughly half a second of visible inertia instead of flicker.
constexpr float kArtworkLedColorSmoothing = 0.06f;
// RADIAL's artwork rotation is intentionally meditative. The LED head travels
// three times faster while retaining the exact seed-selected direction.
constexpr float kRadialLedRotationSpeedMultiplier = 3.0f;

constexpr uint8_t kControlMenuItemCount = 8;
// Physical menu review showed that the halo first assigned to SETTINGS is the
// one visually centered at the top. Shift FLOW one counter-clockwise LED so it
// now uses that validated twelve-o'clock halo (inner LEDs 6 and 7).
constexpr uint8_t kMenuTopClockwiseLedIndex = 7;
constexpr uint8_t kMenuInnerLedLevel = 175;
constexpr uint8_t kMenuOuterLedLevel = 38;
const char* const kControlMenuItemNames[kControlMenuItemCount] = {
    "VORTEX",
    "RADIAL",
    "ATTRACTOR",
    "PARTICLES",
    "ORBIT",
    "BLOBS",
    "CELLULAR",
    "SETTINGS",
};

// The source artwork is retained as transparent PNG in raw/assets/source. These
// A8 descriptors carry only opacity in flash; LVGL applies the white tint when
// it draws the selected icon, avoiding an RGB image buffer in RAM.
lv_image_dsc_t makeMenuIconDescriptor(const uint8_t* alphaMap) {
  lv_image_dsc_t descriptor{};
  descriptor.header.magic = LV_IMAGE_HEADER_MAGIC;
  descriptor.header.cf = LV_COLOR_FORMAT_A8;
  descriptor.header.flags = 0;
  descriptor.header.w = kMenuIconWidth;
  descriptor.header.h = kMenuIconHeight;
  descriptor.header.stride = kMenuIconWidth;
  descriptor.data_size = kMenuIconDataSize;
  descriptor.data = alphaMap;
  return descriptor;
}

const lv_image_dsc_t kMenuIconFlowDescriptor =
    makeMenuIconDescriptor(kMenuIcon_flow_map);
const lv_image_dsc_t kMenuIconRadialDescriptor =
    makeMenuIconDescriptor(kMenuIcon_radial_map);
const lv_image_dsc_t kMenuIconAttractorDescriptor =
    makeMenuIconDescriptor(kMenuIcon_attractor_map);
const lv_image_dsc_t kMenuIconParticlesDescriptor =
    makeMenuIconDescriptor(kMenuIcon_particles_map);
const lv_image_dsc_t kMenuIconOrbitDescriptor =
    makeMenuIconDescriptor(kMenuIcon_orbit_map);
const lv_image_dsc_t kMenuIconBlobsDescriptor =
    makeMenuIconDescriptor(kMenuIcon_blobs_map);
const lv_image_dsc_t kMenuIconCellularDescriptor =
    makeMenuIconDescriptor(kMenuIcon_cellular_map);
const lv_image_dsc_t kMenuIconSettingsDescriptor =
    makeMenuIconDescriptor(kMenuIcon_settings_map);

const lv_image_dsc_t* const kControlMenuItemIcons[kControlMenuItemCount] = {
    &kMenuIconFlowDescriptor,
    &kMenuIconRadialDescriptor,
    &kMenuIconAttractorDescriptor,
    &kMenuIconParticlesDescriptor,
    &kMenuIconOrbitDescriptor,
    &kMenuIconBlobsDescriptor,
    &kMenuIconCellularDescriptor,
    &kMenuIconSettingsDescriptor,
};

// ESP32 NVS is flash-backed, non-volatile storage. It replaces the EEPROM/ROM
// terminology often used for microcontroller preferences, without adding a
// dependency or writing during the render loop.
constexpr char kSettingsPreferencesNamespace[] = "seed146";
constexpr char kAutomatEnabledPreferenceKey[] = "automat";
constexpr char kAutomatDurationPreferenceKey[] = "autoSecs";
constexpr char kLedLuminosityPreferenceKey[] = "ledLum";
constexpr char kSettingsPreferenceSchemaKey[] = "settingsV";
constexpr uint8_t kSettingsPreferenceSchemaVersion = 3;
constexpr bool kDefaultAutomatEnabled = true;
constexpr uint8_t kDefaultAutomatDurationSeconds = 15;
constexpr uint8_t kAutomatMinimumDurationSeconds = 15;
constexpr uint8_t kAutomatMaximumDurationSeconds = 120;
constexpr uint8_t kAutomatDurationStepSeconds = 15;
constexpr uint8_t kLedLuminosityStepPercent = 10;
// Even when the user turns ambient artwork LEDs OFF, menus remain usable with
// a restrained quarter-strength halo rather than disappearing completely.
constexpr uint8_t kMenuLedLuminosityWhenOffPercent = 25;
constexpr uint8_t kArtworkAlgorithmCount = 7;
constexpr uint8_t kAutomaticParameterStepPercent = 5;

// Quadrature transition lookup indexed by previous AB state then current AB.
// Invalid transitions return zero and are counted for the hardware report.
// The table is read inside a GPIO interrupt. DRAM storage keeps it available
// even during the short periods in which flash cache access is unavailable.
DRAM_ATTR const int8_t kQuadratureTable[16] = {
    0, -1, 1, 0,
    1, 0, 0, -1,
    -1, 0, 0, 1,
    0, 1, -1, 0,
};

class Seed146Display : public lgfx::LGFX_Device {
 public:
  Seed146Display() {
    {
      auto config = bus_.config();
      config.spi_host = SPI2_HOST;
      config.spi_mode = 0;
      config.freq_write = kDisplayWriteFrequencyHz;
      config.freq_read = kDisplayReadFrequencyHz;
      config.spi_3wire = true;
      config.use_lock = true;
      config.dma_channel = SPI_DMA_CH_AUTO;
      config.pin_sclk = kDisplaySclkPin;
      config.pin_mosi = kDisplayMosiPin;
      config.pin_miso = -1;
      config.pin_dc = kDisplayDcPin;
      bus_.config(config);
      panel_.setBus(&bus_);
    }

    {
      auto config = panel_.config();
      config.pin_cs = kDisplayCsPin;
      // Reset is performed once, manually, before display.init(). Letting
      // LovyanGFX reset the panel again produced an unreliable startup.
      config.pin_rst = -1;
      config.pin_busy = -1;
      config.memory_width = kScreenWidth;
      config.memory_height = kScreenHeight;
      config.panel_width = kScreenWidth;
      config.panel_height = kScreenHeight;
      config.offset_x = 0;
      config.offset_y = 0;
      config.offset_rotation = 0;
      config.dummy_read_pixel = 8;
      config.dummy_read_bits = 1;
      config.readable = false;
      config.invert = false;
      config.rgb_order = true;
      config.dlen_16bit = false;
      config.bus_shared = false;
      panel_.config(config);
    }

    setPanel(&panel_);
  }

 private:
  lgfx::Panel_DHR55146D panel_;
  lgfx::Bus_SPI bus_;
};

Adafruit_NeoPixel ledRing(kLedCount, kLedDataPin, NEO_GRB + NEO_KHZ800);
Seed146Display display;
cst816t touchController(Wire, kTouchResetPin, kTouchInterruptPin);
FlowPrototype flowPrototype(&display);
RadialPrototype radialPrototype(&display);
AttractorPrototype attractorPrototype(&display);
ParticlesPrototype particlesPrototype(&display);
OrbitPrototype orbitPrototype(&display);
BlobsPrototype blobsPrototype(&display);
CellularPrototype cellularPrototype(&display);

alignas(4) uint8_t lvglDrawBuffer[kScreenWidth * kLvglBufferLineCount *
                                  kRgb565BytesPerPixel];
lv_display_t* lvglDisplay = nullptr;
lv_indev_t* lvglTouchInput = nullptr;

enum class ApplicationMode : uint8_t {
  artwork,
  controlMenu,
  settings,
};
enum class ArtworkAlgorithm : uint8_t {
  flow,
  radial,
  attractor,
  particles,
  orbit,
  blobs,
  cellular,
};
ApplicationMode applicationMode = ApplicationMode::artwork;
ArtworkAlgorithm activeArtworkAlgorithm = ArtworkAlgorithm::flow;
uint8_t selectedControlMenuIndex = 0;
lv_obj_t* controlMenuCurrentLabel = nullptr;
lv_obj_t* controlMenuCurrentIcon = nullptr;
lv_obj_t* settingsAutomatLabel = nullptr;
lv_obj_t* settingsAutomatValueLabel = nullptr;
lv_obj_t* settingsLuminosityLabel = nullptr;
lv_obj_t* settingsLuminosityValueLabel = nullptr;
bool automatEnabled = kDefaultAutomatEnabled;
uint8_t automatDurationSeconds = kDefaultAutomatDurationSeconds;
uint32_t automatLastChangeMs = 0;
// Keep SETTINGS input ownership explicit. This prevents a just-recreated LVGL
// screen from ever falling back to the parent menu's encoder handler.
bool settingsScreenOwnsEncoder = false;

// Keep the active limit separate from its ROM default so a future user setting
// can change it without rewriting firmware. It applies to every LED path:
// artworks, menus, parameter feedback, and button acknowledgements.
uint8_t ledMaximumPowerPercent = kDefaultLedMaximumPowerPercent;

enum class SettingsSelection : uint8_t {
  automat,
  luminosity,
};
SettingsSelection selectedSettingsControl = SettingsSelection::automat;
enum class SettingsTouchState : uint8_t {
  ready,
  tracking,
  waitingForButton,
  blockedUntilRelease,
};
SettingsTouchState settingsTouchState = SettingsTouchState::ready;
uint32_t settingsTouchReleasedMs = 0;
enum class ArtworkStandbyMode : uint8_t {
  running,
  sleeping,
  waitingForSecondWakeTouch,
  pollingFallback,
};
volatile ArtworkStandbyMode artworkStandbyMode = ArtworkStandbyMode::running;
uint8_t backlightBeforeStandbyPercent = kInitialBacklightPercent;
bool firstArtworkTouchReleased = false;
uint32_t firstArtworkTouchReleasedMs = 0;
bool firstWakeTouchReleased = false;
uint32_t firstWakeTouchReleasedMs = 0;
bool fallbackFirstTouchStarted = false;
bool ignoreArtworkTouchRelease = false;

uint32_t lastHeartbeatMs = 0;
int32_t encoderDetentCount = 0;
uint8_t flowTurbulencePercent = kInitialFlowTurbulencePercent;
uint32_t activeFlowSeed = kInitialFlowSeed;
uint8_t radialTwistPercent = kInitialRadialTwistPercent;
uint32_t activeRadialSeed = kInitialRadialSeed;
uint8_t attractorChaosPercent = kInitialAttractorChaosPercent;
uint32_t activeAttractorSeed = kInitialAttractorSeed;
uint32_t activeParticlesSeed = 146004;
uint8_t particlesEnergyPercent = 50;
constexpr uint8_t kParticlesEnergyStepPercent = 5;
uint32_t activeOrbitSeed = kInitialOrbitSeed;
uint8_t orbitEccentricityPercent = kInitialOrbitEccentricityPercent;
uint32_t activeBlobsSeed = kInitialBlobsSeed;
uint8_t blobsDriftPercent = kInitialBlobsDriftPercent;
uint32_t activeCellularSeed = kInitialCellularSeed;
uint8_t cellularMutationPercent = kInitialCellularMutationPercent;
uint32_t reportedCellularAutomaticReseedCount = 0;
uint32_t invalidEncoderTransitions = 0;
portMUX_TYPE encoderStateMutex = portMUX_INITIALIZER_UNLOCKED;
volatile int8_t encoderTransitionAccumulator = 0;
volatile uint8_t previousEncoderState = 0;
volatile int16_t pendingEncoderDetents = 0;
volatile uint32_t pendingInvalidEncoderTransitions = 0;

bool debouncedButtonPressed = false;
bool longPressReported = false;
uint32_t buttonPressedMs = 0;
portMUX_TYPE buttonStateMutex = portMUX_INITIALIZER_UNLOCKED;
volatile bool pendingButtonPress = false;
volatile bool pendingButtonRelease = false;
volatile uint32_t pendingButtonPressMs = 0;
volatile uint32_t pendingButtonReleaseMs = 0;
volatile uint32_t lastButtonInterruptUs = 0;

bool ledFeedbackActive = false;
uint32_t ledFeedbackUntilMs = 0;

// Each meaningful dial threshold uses this short, non-blocking double pulse.
// A state machine rather than delay() keeps rendering and input responsive.
enum class ParameterThresholdPulseStage : uint8_t {
  idle,
  firstOn,
  firstOff,
  secondOn,
  secondOff,
};
ParameterThresholdPulseStage parameterThresholdPulseStage =
    ParameterThresholdPulseStage::idle;
uint32_t parameterThresholdPulseStageUntilMs = 0;
uint32_t parameterThresholdPulseColor = 0;
bool parameterLedFeedbackActive = false;
uint32_t parameterLedFeedbackUntilMs = 0;
uint32_t lastArtworkLedFrameMs = 0;
float displayedArtworkLedRed[kLedCount] = {};
float displayedArtworkLedGreen[kLedCount] = {};
float displayedArtworkLedBlue[kLedCount] = {};
// At a future low global power limit, these retained errors let RADIAL
// alternate adjacent hardware levels over time, preserving its soft tail.
float radialLedDitherErrorRed[kLedCount] = {};
float radialLedDitherErrorGreen[kLedCount] = {};
float radialLedDitherErrorBlue[kLedCount] = {};
float radialLedTargetRed[kLedCount] = {};
float radialLedTargetGreen[kLedCount] = {};
float radialLedTargetBlue[kLedCount] = {};
SemaphoreHandle_t ledRingMutex = nullptr;
TaskHandle_t radialLedRefreshTaskHandle = nullptr;

bool displayInitialized = false;
uint8_t backlightPercent = kInitialBacklightPercent;

bool touchControllerDetected = false;
uint32_t touchEventCount = 0;
uint32_t lastTouchSerialMs = 0;
uint16_t lastTouchX = 0;
uint16_t lastTouchY = 0;
uint32_t lastTouchSampleMs = 0;
bool latestTouchPressed = false;

bool lvglInitialized = false;

// Soak-test metrics retain the worst value observed since startup. A stable
// application can allocate memory during initialization, but should not keep
// losing free memory once the interface is running.
uint32_t minimumFreeHeapBytes = UINT32_MAX;
uint32_t minimumFreePsramBytes = UINT32_MAX;
uint32_t maximumLoopWorkTimeUs = 0;
uint32_t displayFlushCount = 0;

// These state-transition functions are implemented after LVGL setup, but input
// handling calls them earlier in this file. Explicit declarations make that
// relationship clear and avoid relying on Arduino's automatic prototype pass.
void enterControlMenu();
void leaveControlMenu();
void enterSettings();
void leaveSettings();
void confirmControlMenuSelection(uint32_t nowMs);
void enterArtworkStandby();
void leaveArtworkStandby();
bool initializeRadialPrototype();
bool initializeAttractorPrototype();
bool initializeParticlesPrototype();
bool initializeOrbitPrototype();
bool initializeBlobsPrototype();
bool initializeCellularPrototype();
void transmitLedRing();
void radialLedRefreshTask(void* unusedParameter);
void updateSettingsScreen();
void adjustAutomatValue(int8_t direction);
void adjustLuminosityValue(int8_t direction);
void selectNextSettingsControl();
void discardPendingEncoderDetents();
void updateAutomaticMode(uint32_t nowMs);

void printSize(const char* label, uint32_t bytes) {
  diagnosticPrintf("%-18s %10lu bytes (%.2f MiB)\n",
                label,
                static_cast<unsigned long>(bytes),
                static_cast<double>(bytes) / (1024.0 * 1024.0));
}

void printCoreVersion() {
#if defined(ESP_ARDUINO_VERSION_STR)
  diagnosticPrintf("Arduino-ESP32    %s\n", ESP_ARDUINO_VERSION_STR);
#elif defined(ESP_ARDUINO_VERSION_MAJOR)
  diagnosticPrintf("Arduino-ESP32    %d.%d.%d\n",
                ESP_ARDUINO_VERSION_MAJOR,
                ESP_ARDUINO_VERSION_MINOR,
                ESP_ARDUINO_VERSION_PATCH);
#else
  diagnosticPrintln("Arduino-ESP32    version unknown");
#endif
}

bool probePsram() {
  if (!psramFound()) {
    diagnosticPrintln("PSRAM probe       FAIL: PSRAM was not detected");
    return false;
  }

  void* probe = ps_malloc(kPsramProbeSize);
  if (probe == nullptr) {
    diagnosticPrintln("PSRAM probe       FAIL: 1 MiB allocation failed");
    return false;
  }

  memset(probe, 0xA5, kPsramProbeSize);
  free(probe);
  diagnosticPrintln("PSRAM probe       PASS: 1 MiB allocated, written, and freed");
  return true;
}

void printBaselineReport() {
  diagnosticPrintln();
  diagnosticPrintln("========================================");
  diagnosticPrintf("seed146 %s — hardware diagnostics\n", kApplicationVersion);
  diagnosticPrintln("========================================");
  diagnosticPrintf("Chip model        %s\n", ESP.getChipModel());
  diagnosticPrintf("Chip revision     %d\n", ESP.getChipRevision());
  diagnosticPrintf("CPU frequency     %lu MHz\n",
                static_cast<unsigned long>(ESP.getCpuFreqMHz()));
  diagnosticPrintf("CPU cores         %d\n", ESP.getChipCores());
  printCoreVersion();

#if defined(ARDUINO_USB_CDC_ON_BOOT) && ARDUINO_USB_CDC_ON_BOOT
  diagnosticPrintln("USB CDC on boot   Enabled");
#else
  diagnosticPrintln("USB CDC on boot   Disabled");
#endif

  diagnosticPrintln();
  printSize("Flash", ESP.getFlashChipSize());
  printSize("Heap total", ESP.getHeapSize());
  printSize("Heap free", ESP.getFreeHeap());
  printSize("PSRAM total", ESP.getPsramSize());
  printSize("PSRAM free", ESP.getFreePsram());
  diagnosticPrintln();

  const bool psramOk = probePsram();
  diagnosticPrintln();
  diagnosticPrintf("BASELINE RESULT   %s\n", psramOk ? "PASS" : "FAIL");
  diagnosticPrintln("Expected hardware: ESP32-S3, 16 MiB flash, 8 MiB PSRAM");
  diagnosticPrintln("========================================");
}

uint8_t clampAutomatDurationSeconds(uint8_t durationSeconds) {
  const uint8_t clampedDuration = constrain(durationSeconds,
                                            kAutomatMinimumDurationSeconds,
                                            kAutomatMaximumDurationSeconds);
  // A malformed NVS value must never create an undocumented duration. Round
  // it down to the closest validated 15-second step before the UI uses it.
  return kAutomatMinimumDurationSeconds +
         ((clampedDuration - kAutomatMinimumDurationSeconds) /
          kAutomatDurationStepSeconds) *
             kAutomatDurationStepSeconds;
}

uint8_t clampLedLuminosityPercent(uint8_t luminosityPercent) {
  const uint8_t clampedLuminosity = constrain(luminosityPercent, 0, 100);
  // OFF is followed by the documented ten-percent steps only.
  return static_cast<uint8_t>(
      (clampedLuminosity / kLedLuminosityStepPercent) *
      kLedLuminosityStepPercent);
}

void saveSettings() {
  Preferences preferences;
  if (!preferences.begin(kSettingsPreferencesNamespace, false)) {
    diagnosticPrintln("SETTINGS          SAVE FAIL | NVS unavailable");
    return;
  }

  const bool enabledWasSaved = preferences.putBool(
      kAutomatEnabledPreferenceKey, automatEnabled) == sizeof(bool);
  const bool durationWasSaved = preferences.putUChar(
      kAutomatDurationPreferenceKey, automatDurationSeconds) == sizeof(uint8_t);
  const bool luminosityWasSaved = preferences.putUChar(
      kLedLuminosityPreferenceKey, ledMaximumPowerPercent) == sizeof(uint8_t);
  const bool schemaWasSaved = preferences.putUChar(
      kSettingsPreferenceSchemaKey,
      kSettingsPreferenceSchemaVersion) == sizeof(uint8_t);
  preferences.end();

  diagnosticPrintf("SETTINGS          SAVE | automat %s | duration %u s | luminosity %u%% | %s\n",
                automatEnabled ? "ON" : "OFF",
                static_cast<unsigned int>(automatDurationSeconds),
                static_cast<unsigned int>(ledMaximumPowerPercent),
                enabledWasSaved && durationWasSaved && luminosityWasSaved &&
                        schemaWasSaved
                    ? "PASS"
                    : "FAIL");
}

void loadAutomatSettings() {
  Preferences preferences;
  if (!preferences.begin(kSettingsPreferencesNamespace, false)) {
    diagnosticPrintln("SETTINGS          LOAD FAIL | NVS unavailable");
    return;
  }

  const uint8_t storedSettingsSchema = preferences.getUChar(
      kSettingsPreferenceSchemaKey, 0);
  const bool hasCurrentSettingsSchema =
      storedSettingsSchema == kSettingsPreferenceSchemaVersion;
  const bool hasAutomatEncoderSchema = storedSettingsSchema == 2;
  if (hasCurrentSettingsSchema || hasAutomatEncoderSchema) {
    automatEnabled = preferences.getBool(kAutomatEnabledPreferenceKey,
                                         kDefaultAutomatEnabled);
    automatDurationSeconds = clampAutomatDurationSeconds(
        preferences.getUChar(kAutomatDurationPreferenceKey,
                             kDefaultAutomatDurationSeconds));
    ledMaximumPowerPercent = hasCurrentSettingsSchema
                                  ? clampLedLuminosityPercent(preferences.getUChar(
                                        kLedLuminosityPreferenceKey,
                                        kDefaultLedMaximumPowerPercent))
                                  : kDefaultLedMaximumPowerPercent;
  } else {
    // Version 2 replaces the separate touch Switch and swipe duration with one
    // encoder value. Reset the experimental version-1 settings once so this
    // simpler control always starts at the requested 15-second default.
    automatEnabled = kDefaultAutomatEnabled;
    automatDurationSeconds = kDefaultAutomatDurationSeconds;
    ledMaximumPowerPercent = kDefaultLedMaximumPowerPercent;
  }
  preferences.end();

  if (!hasCurrentSettingsSchema) {
    // Version 2 already used the encoder AUTOMAT setting, so preserve that
    // choice and add only LUMINOSITY at its requested 100% default.
    saveSettings();
  }
  diagnosticPrintf("SETTINGS          LOAD | automat %s | duration %u s | luminosity %u%% | %s\n",
                automatEnabled ? "ON" : "OFF",
                static_cast<unsigned int>(automatDurationSeconds),
                static_cast<unsigned int>(ledMaximumPowerPercent),
                hasCurrentSettingsSchema ? "restored" : "migrated/default");
}

uint8_t effectiveLedMaximumPowerPercent() {
  if (ledMaximumPowerPercent != 0) {
    return ledMaximumPowerPercent;
  }
  // OFF silences the ambient artwork light. Menus and the temporary encoder
  // parameter indication remain visible at the same restrained level.
  if (applicationMode != ApplicationMode::artwork ||
      parameterLedFeedbackActive) {
    return kMenuLedLuminosityWhenOffPercent;
  }
  return 0;
}

uint8_t ledMaximumBrightnessByte() {
  // NeoPixel uses 0..255 while the product setting uses the clearer 0..100%.
  // Rounding ensures the ROM default of 100% maps exactly to full output 255.
  return static_cast<uint8_t>(
      (static_cast<uint16_t>(effectiveLedMaximumPowerPercent()) * 255U + 50U) /
      100U);
}

void applyLedLuminosityForCurrentMode() {
  ledRing.setBrightness(ledMaximumBrightnessByte());
}

void setLedMaximumPowerPercent(uint8_t maximumPowerPercent) {
  ledMaximumPowerPercent = clampLedLuminosityPercent(maximumPowerPercent);
  applyLedLuminosityForCurrentMode();
}

void initializeLedRing() {
  // GPIO 17 controls the transistor that supplies power to the LED ring.
  pinMode(kLedPowerEnablePin, OUTPUT);
  digitalWrite(kLedPowerEnablePin, HIGH);

  ledRing.begin();
  applyLedLuminosityForCurrentMode();
  diagnosticPrintf("RGB LED           MAX POWER | %u%% | effective %u%% | brightness %u/255\n",
                static_cast<unsigned int>(ledMaximumPowerPercent),
                static_cast<unsigned int>(effectiveLedMaximumPowerPercent()),
                static_cast<unsigned int>(ledMaximumBrightnessByte()));
  ledRingMutex = xSemaphoreCreateMutex();
  if (ledRingMutex == nullptr) {
    diagnosticPrintln("RGB LED           FAIL | output mutex allocation");
  }
  // Explicitly send an all-off frame before the splash begins its LED pulse.
  ledRing.clear();
  transmitLedRing();

  // The screen renderer can occupy the main loop for about 48 ms. A dedicated
  // low-priority task refreshes only RADIAL's fractional tail levels at 125 Hz,
  // above the visible flicker range. The mutex prevents concurrent transfers
  // when menu or button feedback temporarily owns the same physical ring.
  const BaseType_t taskCreated = xTaskCreatePinnedToCore(
      radialLedRefreshTask,
      "radial-led-refresh",
      2048,
      nullptr,
      1,
      &radialLedRefreshTaskHandle,
      0);
  if (taskCreated != pdPASS) {
    radialLedRefreshTaskHandle = nullptr;
    diagnosticPrintln("RGB LED           FAIL | RADIAL refresh task allocation");
  }
}

void setBacklightPercent(uint8_t percent) {
  backlightPercent = constrain(percent, 0, 100);
  if (!displayInitialized) {
    return;
  }

  const uint32_t maxDuty = (1UL << kBacklightPwmResolution) - 1UL;
  const uint32_t duty = (backlightPercent * maxDuty) / 100UL;
  ledcWrite(kDisplayBacklightPin, duty);
  diagnosticPrintf("BACKLIGHT         %u%% | duty %lu/%lu\n",
                static_cast<unsigned int>(backlightPercent),
                static_cast<unsigned long>(duty),
                static_cast<unsigned long>(maxDuty));
}

void discardInputsCapturedDuringStandby() {
  // GPIO interrupts still capture rotary and button edges while the model is
  // paused. Discard them so they cannot change a parameter or seed on resume.
  discardPendingEncoderDetents();
  portENTER_CRITICAL(&buttonStateMutex);
  pendingButtonPress = false;
  pendingButtonRelease = false;
  portEXIT_CRITICAL(&buttonStateMutex);
  debouncedButtonPressed = false;
  longPressReported = false;
}

void enterArtworkStandby() {
  if (applicationMode != ApplicationMode::artwork ||
      artworkStandbyMode != ArtworkStandbyMode::running) {
    return;
  }

  // Light sleep retains the model and PSRAM canvas. GPIO 5 is the CST816D's
  // active-low interrupt, so one touch wakes the CPU; software then waits for
  // a distinct second touch before restoring the artwork. A timer prevents
  // the chip from staying asleep indefinitely if that interrupt is missed.
  const esp_err_t touchPinResult = gpio_wakeup_enable(
      static_cast<gpio_num_t>(kTouchInterruptPin), GPIO_INTR_LOW_LEVEL);
  const esp_err_t gpioWakeResult = esp_sleep_enable_gpio_wakeup();
  const esp_err_t timerWakeResult =
      esp_sleep_enable_timer_wakeup(kStandbySafetyWakeIntervalUs);
  const bool lightSleepReady = touchPinResult == ESP_OK &&
                               gpioWakeResult == ESP_OK &&
                               timerWakeResult == ESP_OK;

  backlightBeforeStandbyPercent = backlightPercent;
  firstArtworkTouchReleased = false;
  firstWakeTouchReleased = false;
  fallbackFirstTouchStarted = false;
  ignoreArtworkTouchRelease = true;
  artworkStandbyMode = lightSleepReady
                           ? ArtworkStandbyMode::sleeping
                           : ArtworkStandbyMode::pollingFallback;
  ledFeedbackActive = false;
  parameterLedFeedbackActive = false;
  parameterThresholdPulseStage = ParameterThresholdPulseStage::idle;
  // The RADIAL refresh task checks standby mode before its next LED frame.
  ledRing.clear();
  transmitLedRing();
  digitalWrite(kLedPowerEnablePin, LOW);
  setBacklightPercent(0);
  discardInputsCapturedDuringStandby();
  diagnosticPrintf("STANDBY           ENTER | %s | GPIO %s | timer %s\n",
                lightSleepReady ? "light sleep" : "visual pause fallback",
                esp_err_to_name(gpioWakeResult),
                esp_err_to_name(timerWakeResult));
}

void leaveArtworkStandby() {
  if (artworkStandbyMode == ArtworkStandbyMode::running) {
    return;
  }

  // showCurrentFrame() restores the preserved canvas and resets each model's
  // frame clock, so time in standby does not create a motion jump on resume.
  digitalWrite(kLedPowerEnablePin, HIGH);
  delay(1);
  ledRing.clear();
  transmitLedRing();
  applyLedLuminosityForCurrentMode();
  switch (activeArtworkAlgorithm) {
    case ArtworkAlgorithm::flow:
      flowPrototype.showCurrentFrame();
      break;
    case ArtworkAlgorithm::radial:
      radialPrototype.showCurrentFrame();
      break;
    case ArtworkAlgorithm::attractor:
      attractorPrototype.showCurrentFrame();
      break;
    case ArtworkAlgorithm::particles:
      particlesPrototype.showCurrentFrame();
      break;
    case ArtworkAlgorithm::orbit:
      orbitPrototype.showCurrentFrame();
      break;
    case ArtworkAlgorithm::blobs:
      blobsPrototype.showCurrentFrame();
      break;
    case ArtworkAlgorithm::cellular:
      cellularPrototype.showCurrentFrame();
      break;
  }
  const uint32_t resumedMs = millis();
  automatLastChangeMs = resumedMs;
  lastArtworkLedFrameMs = resumedMs - kArtworkLedFramePeriodMs;
  discardInputsCapturedDuringStandby();
  firstArtworkTouchReleased = false;
  ignoreArtworkTouchRelease = true;
  gpio_wakeup_disable(static_cast<gpio_num_t>(kTouchInterruptPin));
  esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_GPIO);
  esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_TIMER);
  artworkStandbyMode = ArtworkStandbyMode::running;
  setBacklightPercent(backlightBeforeStandbyPercent);
  diagnosticPrintln("STANDBY           RESUME | artwork and LEDs restored");
}

void initializeDisplay() {
  // Retain Elecrow's known-working manual reset before LovyanGFX init.
  pinMode(kDisplayResetPin, OUTPUT);
  digitalWrite(kDisplayResetPin, HIGH);
  delay(10);
  digitalWrite(kDisplayResetPin, LOW);
  delay(10);
  digitalWrite(kDisplayResetPin, HIGH);
  delay(10);

  display.init();
  display.initDMA();
  display.setRotation(0);
  display.setColorDepth(16);

  if (!ledcAttach(kDisplayBacklightPin,
                  kBacklightPwmFrequencyHz,
                  kBacklightPwmResolution)) {
    diagnosticPrintln("DISPLAY           FAIL: backlight PWM attach");
  }

  displayInitialized = true;
  setBacklightPercent(kInitialBacklightPercent);
  display.fillScreen(TFT_BLACK);

  diagnosticPrintln();
  diagnosticPrintln("DISPLAY           READY | LovyanGFX DHR55146D | 360 x 360");
  diagnosticPrintln("Backlight          FIXED | 50% during artwork");
}

void showIntroductionSplashScreen() {
  // Explicit RGB565 type avoids depending on the display's byte-swap state.
  // The complete composited frame lives in flash, not in a runtime framebuffer.
  display.pushImage(0, 0, kScreenWidth, kScreenHeight,
                    reinterpret_cast<const lgfx::rgb565_t*>(kSplashPixels));
  display.waitDMA();

  // The requested startup peak is 100%, independently of persisted LUMINOSITY.
  // Keep that preference intact and restore its effective limit after blackout.
  ledRing.clear();
  ledRing.setBrightness(255);
  transmitLedRing();
  const uint32_t splashBecameVisibleMs = millis();
  diagnosticPrintln();
  diagnosticPrintf("SPLASH            SHOW | cyan grid + violet/magenta glow + logo | %lu ms\n",
                static_cast<unsigned long>(kSplashScreenDurationMs));
  diagnosticPrintln("SPLASH LED        RISE | full hue spectrum | green peak 100%");
  bool greenPeakShown = false;
  while (true) {
    const uint32_t elapsedMs = millis() - splashBecameVisibleMs;
    if (elapsedMs >= kSplashScreenDurationMs) {
      break;
    }

    // Send an exact full-power green frame at the rise/fall boundary, even if
    // task scheduling skips the exact midpoint timestamp.
    if (elapsedMs >= kSplashLedRiseDurationMs && !greenPeakShown) {
      ledRing.fill(ledRing.Color(0, 255, 0));
      transmitLedRing();
      greenPeakShown = true;
      diagnosticPrintln("SPLASH LED        PEAK | green 100% | falling to black");
      delay(kSplashLedFramePeriodMs);
      continue;
    }

    const bool rising = elapsedMs < kSplashLedRiseDurationMs;
    const float phase = rising
        ? static_cast<float>(elapsedMs) / kSplashLedRiseDurationMs
        : static_cast<float>(elapsedMs - kSplashLedRiseDurationMs) /
              (kSplashScreenDurationMs - kSplashLedRiseDurationMs);
    // Half-cosine ramps have no abrupt brightness jump at either endpoint.
    const float intensity = rising
        ? 0.5f * (1.0f - cosf(PI * phase))
        : 0.5f * (1.0f + cosf(PI * phase));
    const uint8_t value = static_cast<uint8_t>(intensity * 255.0f + 0.5f);
    // Starting at green and making one full HSV turn visits cyan, blue,
    // magenta, red and yellow before returning to green at maximum intensity.
    const uint16_t hue = rising
        ? static_cast<uint16_t>(kSplashGreenHue +
              static_cast<uint32_t>(phase * kSplashFullHueTurn))
        : kSplashGreenHue;
    ledRing.fill(ledRing.ColorHSV(hue, 255, value));
    transmitLedRing();
    delay(kSplashLedFramePeriodMs);
  }

  // Explicit final off frame and immediate display clear tie both endings to
  // the same timestamp, before touch/input and the first artwork initialize.
  ledRing.clear();
  transmitLedRing();
  display.fillScreen(TFT_BLACK);
  display.waitDMA();
  applyLedLuminosityForCurrentMode();
  diagnosticPrintln("SPLASH            HIDE | LEDs black | saved luminosity restored");
}

void clearDisplayBeforeArtworkLoad() {
  // The menu is rendered by LVGL while artworks use their own canvases. Clear
  // every panel pixel before handing control back so a prior LVGL frame cannot
  // remain faintly visible through the next artwork's sparse first frame.
  display.waitDMA();
  display.fillScreen(TFT_BLACK);
  display.waitDMA();
}

uint8_t activeParameterPercent() {
  switch (activeArtworkAlgorithm) {
    case ArtworkAlgorithm::flow:
      return flowTurbulencePercent;
    case ArtworkAlgorithm::radial:
      return radialTwistPercent;
    case ArtworkAlgorithm::attractor:
      return attractorChaosPercent;
    case ArtworkAlgorithm::particles:
      return particlesEnergyPercent;
    case ArtworkAlgorithm::orbit:
      return orbitEccentricityPercent;
    case ArtworkAlgorithm::blobs:
      return blobsDriftPercent;
    case ArtworkAlgorithm::cellular:
      return cellularMutationPercent;
  }
  return 0;
}

const char* activeParameterName() {
  switch (activeArtworkAlgorithm) {
    case ArtworkAlgorithm::flow:
      return "turbulence";
    case ArtworkAlgorithm::radial:
      return "twist";
    case ArtworkAlgorithm::attractor:
      return "chaos";
    case ArtworkAlgorithm::particles:
      return "energy";
    case ArtworkAlgorithm::orbit:
      return "eccentricity";
    case ArtworkAlgorithm::blobs:
      return "drift";
    case ArtworkAlgorithm::cellular:
      return "mutation";
  }
  return "parameter";
}

uint32_t parameterLedColor(uint8_t parameterPercent) {
  // From 0 to 50%, red and blue rise from 0 to 255 while green remains full:
  // pure green therefore becomes pure white at the approved midpoint.
  if (parameterPercent <= 50) {
    const uint8_t whiteContribution = static_cast<uint8_t>(
        (static_cast<uint16_t>(parameterPercent) * 255U) / 50U);
    return ledRing.Color(whiteContribution, 255, whiteContribution);
  }

  // From 50 to 100%, red remains full while green and blue fall to zero:
  // pure white therefore becomes pure red at maximum turbulence.
  const uint8_t whiteContribution = static_cast<uint8_t>(
      (static_cast<uint16_t>(100U - parameterPercent) * 255U) / 50U);
  return ledRing.Color(255, whiteContribution, whiteContribution);
}

void showParameterLedLevel() {
  const uint32_t color = parameterLedColor(activeParameterPercent());
  ledRing.fill(color);
  transmitLedRing();

  // Start the later ambient transition from the color physically visible now.
  const float red = static_cast<float>((color >> 16U) & 0xFFU);
  const float green = static_cast<float>((color >> 8U) & 0xFFU);
  const float blue = static_cast<float>(color & 0xFFU);
  for (uint8_t ledIndex = 0; ledIndex < kLedCount; ++ledIndex) {
    displayedArtworkLedRed[ledIndex] = red;
    displayedArtworkLedGreen[ledIndex] = green;
    displayedArtworkLedBlue[ledIndex] = blue;
  }
}

void showControlMenuLedSelection() {
  // Distribute any number of menu entries over the eight physical LED positions.
  // With the current eight entries, each menu maps exactly to one LED.
  const uint8_t angularLedOffset = static_cast<uint8_t>(
      (static_cast<uint16_t>(selectedControlMenuIndex) * kLedCount +
       kControlMenuItemCount / 2U) /
      kControlMenuItemCount) % kLedCount;
  // A menu axis lies between two physical LEDs. The clockwise-side inner LED
  // advances with the selected item; the counter-clockwise inner LED is just
  // before it in the validated clockwise chain direction.
  const uint8_t clockwiseInnerLed =
      (kMenuTopClockwiseLedIndex + angularLedOffset) % kLedCount;
  const uint8_t counterClockwiseInnerLed =
      (clockwiseInnerLed + kLedCount - 1U) % kLedCount;
  const uint8_t clockwiseOuterLed =
      (clockwiseInnerLed + 1U) % kLedCount;
  const uint8_t counterClockwiseOuterLed =
      (counterClockwiseInnerLed + kLedCount - 1U) % kLedCount;

  // Two equal inner LEDs center the light on the virtual axis. Two dim outer
  // LEDs extend the lateral fade symmetrically, for four illuminated pixels.
  ledRing.clear();
  ledRing.setPixelColor(
      clockwiseInnerLed,
      ledRing.Color(kMenuInnerLedLevel,
                    kMenuInnerLedLevel,
                    kMenuInnerLedLevel));
  ledRing.setPixelColor(
      counterClockwiseInnerLed,
      ledRing.Color(kMenuInnerLedLevel,
                    kMenuInnerLedLevel,
                    kMenuInnerLedLevel));
  ledRing.setPixelColor(
      clockwiseOuterLed,
      ledRing.Color(kMenuOuterLedLevel,
                    kMenuOuterLedLevel,
                    kMenuOuterLedLevel));
  ledRing.setPixelColor(
      counterClockwiseOuterLed,
      ledRing.Color(kMenuOuterLedLevel,
                    kMenuOuterLedLevel,
                    kMenuOuterLedLevel));
  transmitLedRing();
}

uint8_t scaleLedChannel(uint8_t channel, float brightness) {
  return static_cast<uint8_t>(constrain(
      static_cast<int>(channel * brightness + 0.5f), 0, 255));
}

uint8_t ditherRadialLedChannel(float logicalChannel,
                               float& retainedQuantizationError) {
  // Adafruit_NeoPixel applies the global brightness cap before transmission.
  // At a future low power setting, error diffusion chooses between the reduced
  // hardware levels and carries the fractional remainder to the next frame.
  constexpr uint16_t kNeoPixelScaleDenominator = 256U;
  const uint16_t libraryBrightnessScale =
      static_cast<uint16_t>(ledMaximumBrightnessByte()) + 1U;
  const uint8_t maximumPhysicalChannelLevel = ledMaximumBrightnessByte();

  const float desiredPhysicalLevel =
      constrain(logicalChannel, 0.0f, 255.0f) *
      maximumPhysicalChannelLevel / 255.0f;
  const float levelWithRetainedError =
      desiredPhysicalLevel + retainedQuantizationError;
  const int physicalLevel = constrain(
      static_cast<int>(floorf(levelWithRetainedError + 0.5f)),
      0,
      maximumPhysicalChannelLevel);
  retainedQuantizationError = levelWithRetainedError - physicalLevel;

  if (physicalLevel == 0) {
    return 0;
  }

  // Return the smallest input byte that the NeoPixel library's brightness
  // scaling maps back to the selected physical level.
  const uint16_t unscaledChannel =
      (physicalLevel * kNeoPixelScaleDenominator +
       libraryBrightnessScale - 1U) /
      libraryBrightnessScale;
  return static_cast<uint8_t>(unscaledChannel > 255U
                                  ? 255U
                                  : unscaledChannel);
}

void transmitLedRing() {
  if (ledRingMutex == nullptr) {
    ledRing.show();
    return;
  }

  if (xSemaphoreTake(ledRingMutex, portMAX_DELAY) == pdTRUE) {
    ledRing.show();
    xSemaphoreGive(ledRingMutex);
  }
}

bool radialAmbientLedAnimationIsActive() {
  return applicationMode == ApplicationMode::artwork &&
         artworkStandbyMode == ArtworkStandbyMode::running &&
         activeArtworkAlgorithm == ArtworkAlgorithm::radial &&
         radialPrototype.isReady() &&
         !ledFeedbackActive &&
         !parameterLedFeedbackActive &&
         parameterThresholdPulseStage == ParameterThresholdPulseStage::idle;
}

void radialLedRefreshTask(void* unusedParameter) {
  (void)unusedParameter;
  constexpr uint32_t kRefreshPeriodMs = 8;  // 125 Hz.
  TickType_t previousWakeTick = xTaskGetTickCount();

  while (true) {
    if (radialAmbientLedAnimationIsActive() && ledRingMutex != nullptr &&
        xSemaphoreTake(ledRingMutex, pdMS_TO_TICKS(2)) == pdTRUE) {
      for (uint8_t ledIndex = 0; ledIndex < kLedCount; ++ledIndex) {
        ledRing.setPixelColor(
            ledIndex,
            ledRing.Color(
                ditherRadialLedChannel(
                    radialLedTargetRed[ledIndex],
                    radialLedDitherErrorRed[ledIndex]),
                ditherRadialLedChannel(
                    radialLedTargetGreen[ledIndex],
                    radialLedDitherErrorGreen[ledIndex]),
                ditherRadialLedChannel(
                    radialLedTargetBlue[ledIndex],
                    radialLedDitherErrorBlue[ledIndex])));
      }
      // Direct call is safe here because this task already owns the mutex.
      ledRing.show();
      xSemaphoreGive(ledRingMutex);
    }

    vTaskDelayUntil(&previousWakeTick, pdMS_TO_TICKS(kRefreshPeriodMs));
  }
}

void updateFlowLedAnimation(uint32_t nowMs) {
  if (!flowPrototype.isReady() ||
      nowMs - lastArtworkLedFrameMs < kArtworkLedFramePeriodMs) {
    return;
  }
  lastArtworkLedFrameMs = nowMs;

  // The same green/white/red semantic palette remains recognizable, while a
  // small seed-driven wave creates nearby hues: teal/lime when calm and
  // magenta/orange when turbulent. Brightness always comes from FLOW energy.
  const float turbulence = flowTurbulencePercent / 100.0f;
  const uint32_t seedPhase = flowPrototype.seed() & 0xFFU;

  // With 140 particles, all eight raw sector energies tend to be fairly close.
  // Measure their local range so LEDs describe the composition's hierarchy
  // instead of all reproducing nearly the same absolute brightness.
  float sectorEnergy[FlowPrototype::kSectorCount];
  float minimumSectorEnergy = 1.0f;
  float maximumSectorEnergy = 0.0f;
  for (uint8_t sectorIndex = 0;
       sectorIndex < FlowPrototype::kSectorCount;
       ++sectorIndex) {
    sectorEnergy[sectorIndex] = flowPrototype.sectorEnergy(sectorIndex);
    minimumSectorEnergy = min(minimumSectorEnergy, sectorEnergy[sectorIndex]);
    maximumSectorEnergy = max(maximumSectorEnergy, sectorEnergy[sectorIndex]);
  }
  // A minimum range prevents tiny numerical differences from being amplified
  // into nervous full-scale flashes when the composition is momentarily even.
  const float effectiveEnergyRange =
      max(maximumSectorEnergy - minimumSectorEnergy, 0.06f);

  for (uint8_t sectorIndex = 0;
       sectorIndex < FlowPrototype::kSectorCount;
       ++sectorIndex) {
    uint32_t packedBaseColor = parameterLedColor(flowTurbulencePercent);
    int red = static_cast<int>((packedBaseColor >> 16U) & 0xFFU);
    int green = static_cast<int>((packedBaseColor >> 8U) & 0xFFU);
    int blue = static_cast<int>(packedBaseColor & 0xFFU);

    const float paletteWave = sinf(
        nowMs * (0.0012f + turbulence * 0.0022f) +
        sectorIndex * 1.37f + seedPhase * 0.019f);
    const int accent = static_cast<int>(fabsf(paletteWave) * 38.0f);

    if (turbulence < 0.5f) {
      // Positive accents lean toward teal; negative accents lean toward lime.
      if (paletteWave >= 0.0f) {
        blue += accent;
      } else {
        red += accent;
      }
    } else if (turbulence > 0.5f) {
      // Positive accents lean toward magenta; negative accents toward orange.
      if (paletteWave >= 0.0f) {
        blue += accent;
      } else {
        green += accent;
      }
    } else {
      // Around the midpoint, gently alternate cool and warm whites by reducing
      // one channel rather than trying to add beyond pure white.
      if (paletteWave >= 0.0f) {
        red -= accent;
      } else {
        blue -= accent;
      }
    }

    red = constrain(red, 0, 255);
    green = constrain(green, 0, 255);
    blue = constrain(blue, 0, 255);

    const float relativeEnergy = constrain(
        (sectorEnergy[sectorIndex] - minimumSectorEnergy) /
            effectiveEnergyRange,
        0.0f,
        1.0f);
    // Squaring pushes medium values downward while retaining the strongest
    // sector. This creates real dark/light separation around the physical ring.
    const float contrastedEnergy = relativeEnergy * relativeEnergy;
    // The 1.5% floor is just enough to retain the ring's outline. Turbulence
    // raises peak contrast; the global maximum-power setting is applied at
    // NeoPixel output, consistently for every artwork and feedback mode.
    const float brightness =
        0.015f + contrastedEnergy * (0.62f + turbulence * 0.30f);
    const float targetRed = scaleLedChannel(red, brightness);
    const float targetGreen = scaleLedChannel(green, brightness);
    const float targetBlue = scaleLedChannel(blue, brightness);

    // Smooth the final RGB output—not only the underlying FLOW energy—because
    // adaptive contrast and palette motion can both change a target quickly.
    displayedArtworkLedRed[sectorIndex] +=
        (targetRed - displayedArtworkLedRed[sectorIndex]) *
        kArtworkLedColorSmoothing;
    displayedArtworkLedGreen[sectorIndex] +=
        (targetGreen - displayedArtworkLedGreen[sectorIndex]) *
        kArtworkLedColorSmoothing;
    displayedArtworkLedBlue[sectorIndex] +=
        (targetBlue - displayedArtworkLedBlue[sectorIndex]) *
        kArtworkLedColorSmoothing;

    ledRing.setPixelColor(
        sectorIndex,
        ledRing.Color(
            static_cast<uint8_t>(displayedArtworkLedRed[sectorIndex] + 0.5f),
            static_cast<uint8_t>(displayedArtworkLedGreen[sectorIndex] + 0.5f),
            static_cast<uint8_t>(displayedArtworkLedBlue[sectorIndex] + 0.5f)));
  }
  transmitLedRing();
}

void updateRadialLedAnimation(uint32_t nowMs) {
  if (!radialPrototype.isReady() ||
      nowMs - lastArtworkLedFrameMs < kArtworkLedFramePeriodMs) {
    return;
  }
  lastArtworkLedFrameMs = nowMs;

  // RADIAL keeps the same green / white / red parameter language as FLOW.
  // After the temporary full-ring adjustment feedback ends, the base color is
  // distributed as one slowly rotating, diffuse energy lobe. Its angle is the
  // same seed-selected primary rotation used by RADIAL's arm structure.
  const uint32_t packedBaseColor = parameterLedColor(radialTwistPercent);
  const int baseRed = static_cast<int>((packedBaseColor >> 16U) & 0xFFU);
  const int baseGreen = static_cast<int>((packedBaseColor >> 8U) & 0xFFU);
  const int baseBlue = static_cast<int>(packedBaseColor & 0xFFU);
  const float twist = radialTwistPercent / 100.0f;
  const float primaryRotation = radialPrototype.primaryRotationRadians();
  // Multiplying the seed-selected phase by an integer keeps wrapping seamless:
  // when the renderer removes one 2*PI turn, the LED phase removes three.
  const float acceleratedRotation =
      primaryRotation * kRadialLedRotationSpeedMultiplier;
  const float rotationDirection =
      radialPrototype.primaryRotationDirection();
  const float seedTintPhase = (radialPrototype.seed() & 0xFFU) * 0.019f;

  for (uint8_t sectorIndex = 0;
       sectorIndex < RadialPrototype::kSectorCount;
       ++sectorIndex) {
    const float sectorAngle = sectorIndex * TWO_PI /
                              RadialPrototype::kSectorCount;
    const float energy = radialPrototype.sectorEnergy(sectorIndex);
    // Measure distance behind the moving head in its actual direction.
    float trailingDistance = rotationDirection > 0.0f
                                 ? acceleratedRotation - sectorAngle
                                 : sectorAngle - acceleratedRotation;
    trailingDistance = fmodf(trailingDistance, TWO_PI);
    if (trailingDistance < 0.0f) {
      trailingDistance += TWO_PI;
    }
    const float normalizedTrailingDistance = trailingDistance / TWO_PI;
    // A circular profile needs two soft edges. Behind the head, brightness
    // decays through the long tail. Ahead of it, the next LED rises gradually
    // before becoming the head. Both curves meet at full brightness across the
    // 1.0 -> 0.0 wrap, removing the previous abrupt off-to-on jump.
    // Stretch the tail farther around the ring. Its final quarter is already
    // at very low intensity, so the perceptual dark gap remains even though
    // fewer degrees are mathematically fixed at zero.
    constexpr float kTailFadeEnd = 0.74f;
    constexpr float kLeadingFadeStart = 0.86f;
    float tailBrightness = 0.0f;
    if (normalizedTrailingDistance < kTailFadeEnd) {
      const float fadeProgress =
          normalizedTrailingDistance / kTailFadeEnd;
      // A half-cosine spreads the loss of light evenly over more angular
      // distance and approaches black with zero slope. This gives the last two
      // visible LEDs several subtle low levels instead of one steep drop.
      tailBrightness = 0.5f * (1.0f + cosf(PI * fadeProgress));
    } else if (normalizedTrailingDistance > kLeadingFadeStart) {
      const float riseProgress =
          (normalizedTrailingDistance - kLeadingFadeStart) /
          (1.0f - kLeadingFadeStart);
      // The same smoothstep shape progressively lights the LED ahead.
      tailBrightness = riseProgress * riseProgress *
                       (3.0f - 2.0f * riseProgress);
    }
    // Sector energy only nuances the illuminated tail. It cannot lift the dark
    // gap, which guarantees that not all eight LEDs remain visibly on.
    const float brightness =
        0.001f + tailBrightness * (0.56f + energy * 0.10f);

    int red = baseRed;
    int green = baseGreen;
    int blue = baseBlue;
    // A maximum 12-point channel shift stays diffuse and preserves the
    // parameter color. Calm green becomes gently teal/lime; high red becomes
    // gently magenta/orange; midpoint white remains close to white.
    const float tintWave = sinf(
        sectorAngle * 1.31f + acceleratedRotation * 0.7f + seedTintPhase);
    const int tint = static_cast<int>(fabsf(tintWave) * 12.0f);
    if (twist < 0.5f) {
      if (tintWave >= 0.0f) {
        blue += tint;
      } else {
        red += tint;
      }
    } else if (twist > 0.5f) {
      if (tintWave >= 0.0f) {
        blue += tint;
      } else {
        green += tint;
      }
    } else if (tintWave >= 0.0f) {
      red -= tint;
    } else {
      blue -= tint;
    }

    red = constrain(red, 0, 255);
    green = constrain(green, 0, 255);
    blue = constrain(blue, 0, 255);
    const float targetRed = scaleLedChannel(red, brightness);
    const float targetGreen = scaleLedChannel(green, brightness);
    const float targetBlue = scaleLedChannel(blue, brightness);

    displayedArtworkLedRed[sectorIndex] +=
        (targetRed - displayedArtworkLedRed[sectorIndex]) *
        kArtworkLedColorSmoothing;
    displayedArtworkLedGreen[sectorIndex] +=
        (targetGreen - displayedArtworkLedGreen[sectorIndex]) *
        kArtworkLedColorSmoothing;
    displayedArtworkLedBlue[sectorIndex] +=
        (targetBlue - displayedArtworkLedBlue[sectorIndex]) *
        kArtworkLedColorSmoothing;

    // The main loop updates the slow artistic envelope. The independent LED
    // refresh task reads these targets and resolves fractional hardware levels
    // at 125 Hz, so dithering is no longer tied to RADIAL's ~20 FPS renderer.
    radialLedTargetRed[sectorIndex] = displayedArtworkLedRed[sectorIndex];
    radialLedTargetGreen[sectorIndex] = displayedArtworkLedGreen[sectorIndex];
    radialLedTargetBlue[sectorIndex] = displayedArtworkLedBlue[sectorIndex];
  }
}

void updateAttractorLedAnimation(uint32_t nowMs) {
  if (!attractorPrototype.isReady() ||
      nowMs - lastArtworkLedFrameMs < kArtworkLedFramePeriodMs) {
    return;
  }
  lastArtworkLedFrameMs = nowMs;

  // ATTRACTOR LEDs are normally black. A rare bright screen tracer reaching
  // the circular edge excites only its matching angular LED sector. CHAOS
  // keeps the established green/white/red parameter-color semantics.
  const uint32_t packedBaseColor = parameterLedColor(attractorChaosPercent);
  const int baseRed = static_cast<int>((packedBaseColor >> 16U) & 0xFFU);
  const int baseGreen = static_cast<int>((packedBaseColor >> 8U) & 0xFFU);
  const int baseBlue = static_cast<int>(packedBaseColor & 0xFFU);
  const float seedPhase = (attractorPrototype.seed() & 0xFFU) * 0.019f;

  for (uint8_t sectorIndex = 0;
       sectorIndex < AttractorPrototype::kSectorCount;
       ++sectorIndex) {
    const float edgeActivity =
        attractorPrototype.edgeSparkle(sectorIndex);
    // Two unrelated fast waves create an irregular deterministic scintillation
    // while the renderer's edge envelope supplies the progressive fade-out.
    const float sparkleWaveA = sinf(
        nowMs * (0.031f + sectorIndex * 0.0007f) +
        sectorIndex * 1.91f + seedPhase);
    const float sparkleWaveB = sinf(
        nowMs * (0.017f + sectorIndex * 0.0004f) -
        sectorIndex * 0.73f + seedPhase * 1.37f);
    const float scintillation =
        0.20f + 0.80f * fabsf(sparkleWaveA * sparkleWaveB);
    const float brightness = edgeActivity * scintillation * 0.88f;
    const float targetRed =
        scaleLedChannel(baseRed, brightness);
    const float targetGreen =
        scaleLedChannel(baseGreen, brightness);
    const float targetBlue =
        scaleLedChannel(baseBlue, brightness);

    // Fast attack makes the physical edge contact immediate. A slower release
    // preserves a visible fade to true black after the scintillation ends.
    const float redSmoothing =
        targetRed > displayedArtworkLedRed[sectorIndex] ? 0.65f : 0.08f;
    const float greenSmoothing =
        targetGreen > displayedArtworkLedGreen[sectorIndex] ? 0.65f : 0.08f;
    const float blueSmoothing =
        targetBlue > displayedArtworkLedBlue[sectorIndex] ? 0.65f : 0.08f;
    displayedArtworkLedRed[sectorIndex] +=
        (targetRed - displayedArtworkLedRed[sectorIndex]) *
        redSmoothing;
    displayedArtworkLedGreen[sectorIndex] +=
        (targetGreen - displayedArtworkLedGreen[sectorIndex]) *
        greenSmoothing;
    displayedArtworkLedBlue[sectorIndex] +=
        (targetBlue - displayedArtworkLedBlue[sectorIndex]) *
        blueSmoothing;

    ledRing.setPixelColor(
        sectorIndex,
        ledRing.Color(
            static_cast<uint8_t>(displayedArtworkLedRed[sectorIndex] + 0.5f),
            static_cast<uint8_t>(displayedArtworkLedGreen[sectorIndex] + 0.5f),
            static_cast<uint8_t>(displayedArtworkLedBlue[sectorIndex] + 0.5f)));
  }
  transmitLedRing();
}

void startParameterThresholdPulse(uint8_t parameterPercent, uint32_t nowMs) {
  // The pulse color repeats the stable feedback color: green at 0%, white at
  // 50%, red at 100%. This makes the three mechanical landmarks distinguishable.
  parameterThresholdPulseColor = parameterLedColor(parameterPercent);
  parameterThresholdPulseStage = ParameterThresholdPulseStage::firstOn;
  parameterThresholdPulseStageUntilMs = nowMs + kParameterThresholdPulseOnMs;
  ledRing.fill(parameterThresholdPulseColor);
  transmitLedRing();
}

void showButtonFeedback(uint8_t red,
                        uint8_t green,
                        uint8_t blue,
                        uint32_t durationMs,
                        uint32_t nowMs) {
  ledFeedbackActive = true;
  parameterThresholdPulseStage = ParameterThresholdPulseStage::idle;
  ledFeedbackUntilMs = nowMs + durationMs;
  ledRing.fill(ledRing.Color(red, green, blue));
  transmitLedRing();

  // When the acknowledgement ends, ambient animation fades naturally from
  // this physically displayed color instead of jumping to an old cached state.
  for (uint8_t ledIndex = 0; ledIndex < kLedCount; ++ledIndex) {
    displayedArtworkLedRed[ledIndex] = red;
    displayedArtworkLedGreen[ledIndex] = green;
    displayedArtworkLedBlue[ledIndex] = blue;
  }
}

void updateParticlesLedAnimation(uint32_t nowMs) {
  if (!particlesPrototype.isReady() ||
      nowMs - lastArtworkLedFrameMs < kArtworkLedFramePeriodMs) return;
  lastArtworkLedFrameMs = nowMs;
  const uint32_t color = parameterLedColor(particlesEnergyPercent);
  // Screen-edge impulses decay in simulation time. No autonomous chase or
  // sparkle is added: the LED event remains tied to the particle impact.
  for (uint8_t sector = 0; sector < ParticlesPrototype::kSectorCount; ++sector) {
    const float brightness = particlesPrototype.edgeImpulse(sector) * 0.85f;
    ledRing.setPixelColor(sector, ledRing.Color(
        static_cast<uint8_t>(((color >> 16U) & 255U) * brightness),
        static_cast<uint8_t>(((color >> 8U) & 255U) * brightness),
        static_cast<uint8_t>((color & 255U) * brightness)));
  }
  transmitLedRing();
}

void updateOrbitLedAnimation(uint32_t nowMs) {
  if (!orbitPrototype.isReady() ||
      nowMs - lastArtworkLedFrameMs < kArtworkLedFramePeriodMs) {
    return;
  }
  lastArtworkLedFrameMs = nowMs;
  const uint32_t color = parameterLedColor(orbitEccentricityPercent);
  for (uint8_t sectorIndex = 0;
       sectorIndex < OrbitPrototype::kSectorCount;
       ++sectorIndex) {
    // Three LEDs centered on the contacted quarter share one spatial envelope.
    // The renderer derives it from distance to the struck border.
    const float brightness = orbitPrototype.sectorEnergy(sectorIndex) * 0.95f;
    ledRing.setPixelColor(
        sectorIndex,
        ledRing.Color(
            static_cast<uint8_t>(((color >> 16U) & 255U) * brightness),
            static_cast<uint8_t>(((color >> 8U) & 255U) * brightness),
            static_cast<uint8_t>((color & 255U) * brightness)));
  }
  transmitLedRing();
}

void updateBlobsLedAnimation(uint32_t nowMs) {
  if (!blobsPrototype.isReady() ||
      nowMs - lastArtworkLedFrameMs < kArtworkLedFramePeriodMs) {
    return;
  }
  lastArtworkLedFrameMs = nowMs;
  const uint32_t packedBaseColor = parameterLedColor(blobsDriftPercent);
  const uint8_t baseRed = static_cast<uint8_t>((packedBaseColor >> 16U) & 255U);
  const uint8_t baseGreen = static_cast<uint8_t>((packedBaseColor >> 8U) & 255U);
  const uint8_t baseBlue = static_cast<uint8_t>(packedBaseColor & 255U);

  for (uint8_t sectorIndex = 0; sectorIndex < BlobsPrototype::kSectorCount;
       ++sectorIndex) {
    // The renderer provides this energy by probing the true circular edge of
    // the scalar field. It is not a decorative chase: black sectors mean no
    // visible material currently reaches that part of the screen boundary.
    const float brightness = blobsPrototype.edgeEnergy(sectorIndex) * 0.90f;
    const float targetRed = scaleLedChannel(baseRed, brightness);
    const float targetGreen = scaleLedChannel(baseGreen, brightness);
    const float targetBlue = scaleLedChannel(baseBlue, brightness);
    // Rendering advances at about 8 FPS, but the LED loop runs at 30 FPS.
    // This separate smoothing makes the viscous response continuous between
    // those field samples without inventing motion that is absent on screen.
    const float response = targetRed > displayedArtworkLedRed[sectorIndex]
                               ? 0.24f
                               : 0.07f;
    displayedArtworkLedRed[sectorIndex] +=
        (targetRed - displayedArtworkLedRed[sectorIndex]) * response;
    displayedArtworkLedGreen[sectorIndex] +=
        (targetGreen - displayedArtworkLedGreen[sectorIndex]) * response;
    displayedArtworkLedBlue[sectorIndex] +=
        (targetBlue - displayedArtworkLedBlue[sectorIndex]) * response;
    ledRing.setPixelColor(
        sectorIndex,
        ledRing.Color(
            static_cast<uint8_t>(displayedArtworkLedRed[sectorIndex] + 0.5f),
            static_cast<uint8_t>(displayedArtworkLedGreen[sectorIndex] + 0.5f),
            static_cast<uint8_t>(displayedArtworkLedBlue[sectorIndex] + 0.5f)));
  }
  transmitLedRing();
}

void updateCellularLedAnimation(uint32_t nowMs) {
  if (!cellularPrototype.isReady() ||
      nowMs - lastArtworkLedFrameMs < kArtworkLedFramePeriodMs) {
    return;
  }
  lastArtworkLedFrameMs = nowMs;

  float minimumPopulationEnergy = 1000000.0f;
  float maximumPopulationEnergy = 0.0f;
  for (uint8_t sectorIndex = 0;
       sectorIndex < CellularPrototype::kSectorCount;
       ++sectorIndex) {
    const float energy =
        cellularPrototype.sectorPopulationEnergy(sectorIndex);
    minimumPopulationEnergy = min(minimumPopulationEnergy, energy);
    maximumPopulationEnergy = max(maximumPopulationEnergy, energy);
  }
  const float effectivePopulationRange =
      max(maximumPopulationEnergy - minimumPopulationEnergy, 2.5f);
  // Breathing remains a nuance rather than an autonomous global pulse. It
  // changes peak brightness by only twelve percent while sector density
  // remains the dominant visible animation signal.
  const float breathingBrightness =
      0.88f + cellularPrototype.breathingPhaseLevel() * 0.12f;

  // MUTATION owns the ambient palette, as it does during direct parameter
  // feedback: low values are green, the midpoint is a cool blue-white, and
  // high values are red. Softer endpoint colors keep the persistent animation
  // pastel even though temporary parameter feedback remains more explicit.
  constexpr float kLowMutationRed = 105.0f;
  constexpr float kLowMutationGreen = 255.0f;
  constexpr float kLowMutationBlue = 170.0f;
  constexpr float kMidMutationRed = 205.0f;
  constexpr float kMidMutationGreen = 225.0f;
  constexpr float kMidMutationBlue = 255.0f;
  constexpr float kHighMutationRed = 255.0f;
  constexpr float kHighMutationGreen = 105.0f;
  constexpr float kHighMutationBlue = 125.0f;
  const float mutation = cellularMutationPercent / 100.0f;
  const bool belowMidpoint = mutation <= 0.5f;
  const float paletteProgress = belowMidpoint
                                    ? mutation * 2.0f
                                    : (mutation - 0.5f) * 2.0f;
  const float paletteRed = belowMidpoint
      ? kLowMutationRed +
            (kMidMutationRed - kLowMutationRed) * paletteProgress
      : kMidMutationRed +
            (kHighMutationRed - kMidMutationRed) * paletteProgress;
  const float paletteGreen = belowMidpoint
      ? kLowMutationGreen +
            (kMidMutationGreen - kLowMutationGreen) * paletteProgress
      : kMidMutationGreen +
            (kHighMutationGreen - kMidMutationGreen) * paletteProgress;
  const float paletteBlue = belowMidpoint
      ? kLowMutationBlue +
            (kMidMutationBlue - kLowMutationBlue) * paletteProgress
      : kMidMutationBlue +
            (kHighMutationBlue - kMidMutationBlue) * paletteProgress;

  for (uint8_t sectorIndex = 0;
       sectorIndex < CellularPrototype::kSectorCount;
       ++sectorIndex) {
    const float relativePopulation = constrain(
        (cellularPrototype.sectorPopulationEnergy(sectorIndex) -
         minimumPopulationEnergy) /
            effectivePopulationRange,
        0.0f,
        1.0f);
    const float contrastedPopulation =
        relativePopulation * relativePopulation;
    const float brightness =
        (0.025f + contrastedPopulation * 0.72f) * breathingBrightness;
    const float targetRed = paletteRed * brightness;
    const float targetGreen = paletteGreen * brightness;
    const float targetBlue = paletteBlue * brightness;

    // New activity appears gently; dying colonies leave a longer afterglow.
    // This keeps the biological color changes legible at the 5 Hz generation
    // rate without creating discrete LED steps.
    const float response =
        targetRed + targetGreen + targetBlue >
                displayedArtworkLedRed[sectorIndex] +
                    displayedArtworkLedGreen[sectorIndex] +
                    displayedArtworkLedBlue[sectorIndex]
            ? 0.14f
            : 0.055f;
    displayedArtworkLedRed[sectorIndex] +=
        (targetRed - displayedArtworkLedRed[sectorIndex]) * response;
    displayedArtworkLedGreen[sectorIndex] +=
        (targetGreen - displayedArtworkLedGreen[sectorIndex]) * response;
    displayedArtworkLedBlue[sectorIndex] +=
        (targetBlue - displayedArtworkLedBlue[sectorIndex]) * response;
    ledRing.setPixelColor(
        sectorIndex,
        ledRing.Color(
            static_cast<uint8_t>(displayedArtworkLedRed[sectorIndex] + 0.5f),
            static_cast<uint8_t>(displayedArtworkLedGreen[sectorIndex] + 0.5f),
            static_cast<uint8_t>(displayedArtworkLedBlue[sectorIndex] + 0.5f)));
  }
  transmitLedRing();
}

void updateLedFeedback(uint32_t nowMs) {
  // Button feedback has priority over a parameter-threshold pulse.
  if (ledFeedbackActive) {
    if (static_cast<int32_t>(nowMs - ledFeedbackUntilMs) >= 0) {
      ledFeedbackActive = false;
      if (applicationMode == ApplicationMode::controlMenu ||
          applicationMode == ApplicationMode::settings) {
        showControlMenuLedSelection();
      } else if (parameterLedFeedbackActive) {
        showParameterLedLevel();
      }
    }
    return;
  }

  if (applicationMode == ApplicationMode::controlMenu ||
      applicationMode == ApplicationMode::settings) {
    return;
  }

  if (parameterThresholdPulseStage == ParameterThresholdPulseStage::idle ||
      static_cast<int32_t>(nowMs - parameterThresholdPulseStageUntilMs) < 0) {
    if (parameterLedFeedbackActive &&
        static_cast<int32_t>(nowMs - parameterLedFeedbackUntilMs) >= 0) {
      parameterLedFeedbackActive = false;
      parameterThresholdPulseStage = ParameterThresholdPulseStage::idle;
      if (ledMaximumPowerPercent == 0) {
        // The encoder indication has ended: remove its last color immediately
        // instead of leaving it visible until the next ambient LED frame.
        applyLedLuminosityForCurrentMode();
        ledRing.clear();
        transmitLedRing();
      }
    }
    if (!parameterLedFeedbackActive) {
      if (activeArtworkAlgorithm == ArtworkAlgorithm::flow) {
        updateFlowLedAnimation(nowMs);
      } else if (activeArtworkAlgorithm == ArtworkAlgorithm::radial) {
        updateRadialLedAnimation(nowMs);
      } else if (activeArtworkAlgorithm == ArtworkAlgorithm::attractor) {
        updateAttractorLedAnimation(nowMs);
      } else if (activeArtworkAlgorithm == ArtworkAlgorithm::particles) {
        updateParticlesLedAnimation(nowMs);
      } else if (activeArtworkAlgorithm == ArtworkAlgorithm::orbit) {
        updateOrbitLedAnimation(nowMs);
      } else if (activeArtworkAlgorithm == ArtworkAlgorithm::blobs) {
        updateBlobsLedAnimation(nowMs);
      } else {
        updateCellularLedAnimation(nowMs);
      }
    }
    return;
  }

  switch (parameterThresholdPulseStage) {
    case ParameterThresholdPulseStage::firstOn:
      parameterThresholdPulseStage = ParameterThresholdPulseStage::firstOff;
      parameterThresholdPulseStageUntilMs =
          nowMs + kParameterThresholdPulseOffMs;
      ledRing.clear();
      transmitLedRing();
      break;
    case ParameterThresholdPulseStage::firstOff:
      parameterThresholdPulseStage = ParameterThresholdPulseStage::secondOn;
      parameterThresholdPulseStageUntilMs =
          nowMs + kParameterThresholdPulseOnMs;
      ledRing.fill(parameterThresholdPulseColor);
      transmitLedRing();
      break;
    case ParameterThresholdPulseStage::secondOn:
      parameterThresholdPulseStage = ParameterThresholdPulseStage::secondOff;
      parameterThresholdPulseStageUntilMs =
          nowMs + kParameterThresholdPulseOffMs;
      ledRing.clear();
      transmitLedRing();
      break;
    case ParameterThresholdPulseStage::secondOff:
      parameterThresholdPulseStage = ParameterThresholdPulseStage::idle;
      showParameterLedLevel();
      break;
    case ParameterThresholdPulseStage::idle:
      break;
  }
}

uint8_t readEncoderState() {
  return (static_cast<uint8_t>(digitalRead(kEncoderAPin)) << 1U) |
         static_cast<uint8_t>(digitalRead(kEncoderBPin));
}

void ARDUINO_ISR_ATTR captureEncoderTransition() {
  // ORBIT can occupy the main loop for a long frame. Capture only the two GPIO
  // levels and quadrature state here; parameter changes and Serial output stay
  // in the normal loop where they are safe.
  const uint8_t currentState =
      (static_cast<uint8_t>(gpio_get_level(
           static_cast<gpio_num_t>(kEncoderAPin))) << 1U) |
      static_cast<uint8_t>(gpio_get_level(
          static_cast<gpio_num_t>(kEncoderBPin)));

  portENTER_CRITICAL_ISR(&encoderStateMutex);
  if (currentState != previousEncoderState) {
    const uint8_t tableIndex = (previousEncoderState << 2U) | currentState;
    const int8_t transition = kQuadratureTable[tableIndex];
    previousEncoderState = currentState;

    if (transition == 0) {
      ++pendingInvalidEncoderTransitions;
      encoderTransitionAccumulator = 0;
    } else {
      encoderTransitionAccumulator += transition;
      if (encoderTransitionAccumulator >= kTransitionsPerDetent) {
        encoderTransitionAccumulator = 0;
        ++pendingEncoderDetents;
      } else if (encoderTransitionAccumulator <= -kTransitionsPerDetent) {
        encoderTransitionAccumulator = 0;
        --pendingEncoderDetents;
      }
    }
  }
  portEXIT_CRITICAL_ISR(&encoderStateMutex);
}

void ARDUINO_ISR_ATTR captureButtonTransition() {
  // Store only physical edges and their timestamps here. Seed changes, LED
  // feedback and menu navigation stay in updateButton(), where Serial and
  // rendering APIs are safe. This lets a short click survive a long BLOBS
  // rasterization frame instead of being lost between two digitalRead calls.
  const uint32_t edgeAtUs = micros();
  const bool pressed = gpio_get_level(
      static_cast<gpio_num_t>(kEncoderButtonPin)) == 0;

  portENTER_CRITICAL_ISR(&buttonStateMutex);
  if (edgeAtUs - lastButtonInterruptUs >= kButtonInterruptDebounceUs) {
    lastButtonInterruptUs = edgeAtUs;
    if (pressed) {
      pendingButtonPressMs = edgeAtUs / 1000U;
      pendingButtonPress = true;
    } else {
      pendingButtonReleaseMs = edgeAtUs / 1000U;
      pendingButtonRelease = true;
    }
  }
  portEXIT_CRITICAL_ISR(&buttonStateMutex);
}

void updateControlMenuText() {
  if (!lvglInitialized || controlMenuCurrentLabel == nullptr ||
      controlMenuCurrentIcon == nullptr) {
    return;
  }

  lv_label_set_text(controlMenuCurrentLabel,
                    kControlMenuItemNames[selectedControlMenuIndex]);
  lv_image_set_src(controlMenuCurrentIcon,
                   kControlMenuItemIcons[selectedControlMenuIndex]);
}

void moveControlMenuSelection(int8_t direction) {
  const int nextIndex = static_cast<int>(selectedControlMenuIndex) + direction;
  selectedControlMenuIndex = static_cast<uint8_t>(
      (nextIndex + kControlMenuItemCount) % kControlMenuItemCount);
  updateControlMenuText();
  showControlMenuLedSelection();
  diagnosticPrintf("MENU              SELECT | %u/%u | %s\n",
                static_cast<unsigned int>(selectedControlMenuIndex + 1U),
                static_cast<unsigned int>(kControlMenuItemCount),
                kControlMenuItemNames[selectedControlMenuIndex]);
}

void reportEncoderDetent(int8_t direction) {
  encoderDetentCount += direction;
  ledFeedbackActive = false;

  if (applicationMode == ApplicationMode::controlMenu) {
    moveControlMenuSelection(direction);
    return;
  }

  if (settingsScreenOwnsEncoder ||
      applicationMode == ApplicationMode::settings) {
    if (selectedSettingsControl == SettingsSelection::automat) {
      adjustAutomatValue(direction);
    } else {
      adjustLuminosityValue(direction);
    }
    return;
  }

  // Conversion stays outside the renderers so they remain independent of
  // encoder pins, detents, and the product's direction convention.
  const uint8_t previousParameterPercent = activeParameterPercent();
  if (activeArtworkAlgorithm == ArtworkAlgorithm::flow) {
    const int nextPercent = static_cast<int>(flowTurbulencePercent) +
                            direction * kFlowTurbulenceStepPercent;
    flowTurbulencePercent =
        static_cast<uint8_t>(constrain(nextPercent, 0, 100));
    flowPrototype.setTurbulence(flowTurbulencePercent / 100.0f);
  } else if (activeArtworkAlgorithm == ArtworkAlgorithm::radial) {
    const int nextPercent = static_cast<int>(radialTwistPercent) +
                            direction * kRadialTwistStepPercent;
    radialTwistPercent =
        static_cast<uint8_t>(constrain(nextPercent, 0, 100));
    radialPrototype.setTwist(radialTwistPercent / 100.0f);
  } else if (activeArtworkAlgorithm == ArtworkAlgorithm::attractor) {
    const int nextPercent = static_cast<int>(attractorChaosPercent) +
                            direction * kAttractorChaosStepPercent;
    attractorChaosPercent =
        static_cast<uint8_t>(constrain(nextPercent, 0, 100));
    attractorPrototype.setChaos(attractorChaosPercent / 100.0f);
  } else if (activeArtworkAlgorithm == ArtworkAlgorithm::particles) {
    const int nextPercent = particlesEnergyPercent + direction * kParticlesEnergyStepPercent;
    particlesEnergyPercent = static_cast<uint8_t>(constrain(nextPercent, 0, 100));
    particlesPrototype.setEnergy(particlesEnergyPercent / 100.0f);
  } else if (activeArtworkAlgorithm == ArtworkAlgorithm::orbit) {
    const int nextPercent = orbitEccentricityPercent +
                            direction * kOrbitEccentricityStepPercent;
    orbitEccentricityPercent =
        static_cast<uint8_t>(constrain(nextPercent, 0, 100));
    orbitPrototype.setEccentricity(orbitEccentricityPercent / 100.0f);
  } else if (activeArtworkAlgorithm == ArtworkAlgorithm::blobs) {
    const int nextPercent = static_cast<int>(blobsDriftPercent) +
                            direction * kBlobsDriftStepPercent;
    blobsDriftPercent =
        static_cast<uint8_t>(constrain(nextPercent, 0, 100));
    blobsPrototype.setDrift(blobsDriftPercent / 100.0f);
  } else {
    const int nextPercent = static_cast<int>(cellularMutationPercent) +
                            direction * kCellularMutationStepPercent;
    cellularMutationPercent =
        static_cast<uint8_t>(constrain(nextPercent, 0, 100));
    cellularPrototype.setMutation(cellularMutationPercent / 100.0f);
  }
  parameterLedFeedbackActive = true;
  parameterLedFeedbackUntilMs = millis() + kParameterLedFeedbackDurationMs;
  applyLedLuminosityForCurrentMode();

  const uint8_t newParameterPercent = activeParameterPercent();
  const bool reachedParameterLandmark =
      (newParameterPercent == 0 || newParameterPercent == 50 ||
       newParameterPercent == 100) &&
      newParameterPercent != previousParameterPercent;
  if (reachedParameterLandmark) {
    startParameterThresholdPulse(newParameterPercent, millis());
  } else if (newParameterPercent != previousParameterPercent) {
    // A genuine parameter change supersedes an earlier landmark pulse. In
    // contrast, an additional detent held against 0% or 100% changes nothing,
    // so it must not erase the pulse that just acknowledged that endpoint.
    parameterThresholdPulseStage = ParameterThresholdPulseStage::idle;
    showParameterLedLevel();
  }

  diagnosticPrintf("ENCODER           %s | count %+ld | %s %u%%\n",
                direction > 0 ? "CW" : "CCW",
                static_cast<long>(encoderDetentCount),
                activeParameterName(),
                static_cast<unsigned int>(newParameterPercent));
}

void updateEncoder() {
  int16_t capturedDetents = 0;
  uint32_t capturedInvalidTransitions = 0;
  portENTER_CRITICAL(&encoderStateMutex);
  capturedDetents = pendingEncoderDetents;
  pendingEncoderDetents = 0;
  capturedInvalidTransitions = pendingInvalidEncoderTransitions;
  pendingInvalidEncoderTransitions = 0;
  portEXIT_CRITICAL(&encoderStateMutex);

  invalidEncoderTransitions += capturedInvalidTransitions;
  while (capturedDetents > 0) {
    // Polarity was physically validated against clockwise knob movement.
    reportEncoderDetent(1);
    --capturedDetents;
  }
  while (capturedDetents < 0) {
    reportEncoderDetent(-1);
    ++capturedDetents;
  }
}

void discardPendingEncoderDetents() {
  // An encoder turn that was already captured while a menu transition was
  // being drawn belongs to neither the parent menu nor SETTINGS. Dropping it
  // avoids an accidental first AUTOMAT change and gives every entry a clean
  // input state.
  portENTER_CRITICAL(&encoderStateMutex);
  pendingEncoderDetents = 0;
  encoderTransitionAccumulator = 0;
  portEXIT_CRITICAL(&encoderStateMutex);
}

uint32_t nextDeterministicSeed(uint32_t currentSeed) {
  // This standard linear-congruential step creates a repeatable seed sequence.
  // It depends only on the current seed—not millis() or touch timing—so the
  // first, second, and later clicks reproduce the same series after every boot.
  uint32_t nextSeed = currentSeed * 1664525UL + 1013904223UL;
  if (nextSeed == 0) {
    nextSeed = 1;
  }
  return nextSeed;
}

void generateNextFlowVariation() {
  activeFlowSeed = nextDeterministicSeed(activeFlowSeed);
  if (flowPrototype.regenerate(activeFlowSeed)) {
    diagnosticPrintf("FLOW              NEW SEED | %lu | turbulence %u%%\n",
                  static_cast<unsigned long>(activeFlowSeed),
                  static_cast<unsigned int>(flowTurbulencePercent));
  } else {
    diagnosticPrintln("FLOW              NEW SEED FAIL | renderer not ready");
  }
}

void generateNextRadialVariation() {
  activeRadialSeed = nextDeterministicSeed(activeRadialSeed);
  if (radialPrototype.regenerate(activeRadialSeed)) {
    diagnosticPrintf("RADIAL            NEW SEED | %lu | %s | twist %u%% | arms %u | symmetry %u\n",
                  static_cast<unsigned long>(activeRadialSeed),
                  radialPrototype.compositionFamilyName(),
                  static_cast<unsigned int>(radialTwistPercent),
                  static_cast<unsigned int>(radialPrototype.armCount()),
                  static_cast<unsigned int>(radialPrototype.symmetry()));
  } else {
    diagnosticPrintln("RADIAL            NEW SEED FAIL | renderer not ready");
  }
}

void generateNextAttractorVariation() {
  activeAttractorSeed = nextDeterministicSeed(activeAttractorSeed);
  if (attractorPrototype.regenerate(activeAttractorSeed)) {
    diagnosticPrintf("ATTRACTOR         NEW SEED | %lu | %s | chaos %u%%\n",
                  static_cast<unsigned long>(activeAttractorSeed),
                  attractorPrototype.familyName(),
                  static_cast<unsigned int>(attractorChaosPercent));
  } else {
    diagnosticPrintln("ATTRACTOR         NEW SEED FAIL | renderer not ready");
  }
}

void generateNextParticlesVariation() {
  activeParticlesSeed = nextDeterministicSeed(activeParticlesSeed);
  if (particlesPrototype.regenerate(activeParticlesSeed)) {
    diagnosticPrintf("PARTICLES          NEW SEED | %lu | %s | energy %u%%\n",
                  static_cast<unsigned long>(activeParticlesSeed),
                  particlesPrototype.familyName(), particlesEnergyPercent);
  } else {
    diagnosticPrintln("PARTICLES          NEW SEED FAIL | renderer not ready");
  }
}

void generateNextOrbitVariation() {
  activeOrbitSeed = nextDeterministicSeed(activeOrbitSeed);
  if (orbitPrototype.regenerate(activeOrbitSeed)) {
    diagnosticPrintf(
        "ORBIT             NEW SEED | %lu | attractors %u | symmetry %u | eccentricity %u%%\n",
        static_cast<unsigned long>(activeOrbitSeed),
        static_cast<unsigned int>(orbitPrototype.attractorCount()),
        static_cast<unsigned int>(orbitPrototype.symmetryOrder()),
        static_cast<unsigned int>(orbitEccentricityPercent));
  } else {
    diagnosticPrintln("ORBIT             NEW SEED FAIL | renderer not ready");
  }
}

void generateNextBlobsVariation() {
  activeBlobsSeed = nextDeterministicSeed(activeBlobsSeed);
  if (blobsPrototype.regenerate(activeBlobsSeed)) {
    diagnosticPrintf("BLOBS             NEW SEED | %lu | drift %u%%\n",
                  static_cast<unsigned long>(activeBlobsSeed),
                  static_cast<unsigned int>(blobsDriftPercent));
  } else {
    diagnosticPrintln("BLOBS             NEW SEED FAIL | renderer not ready");
  }
}

void generateNextCellularVariation() {
  activeCellularSeed = nextDeterministicSeed(cellularPrototype.seed());
  if (cellularPrototype.regenerate(activeCellularSeed)) {
    reportedCellularAutomaticReseedCount =
        cellularPrototype.automaticReseedCount();
    diagnosticPrintf(
        "CELLULAR          NEW SEED | %lu | mutation %u%% | population %u\n",
        static_cast<unsigned long>(activeCellularSeed),
        static_cast<unsigned int>(cellularMutationPercent),
        static_cast<unsigned int>(cellularPrototype.livingCellCount()));
  } else {
    diagnosticPrintln("CELLULAR          NEW SEED FAIL | renderer not ready");
  }
}

void updateButton(uint32_t nowMs) {
  bool capturedPress = false;
  bool capturedRelease = false;
  uint32_t capturedPressMs = 0;
  uint32_t capturedReleaseMs = 0;

  // Copy and clear the ISR-owned state atomically. Both a press and a release
  // can arrive during one slow render frame, so process them in timestamp
  // order below rather than relying on the GPIO's final level.
  portENTER_CRITICAL(&buttonStateMutex);
  capturedPress = pendingButtonPress;
  capturedRelease = pendingButtonRelease;
  capturedPressMs = pendingButtonPressMs;
  capturedReleaseMs = pendingButtonReleaseMs;
  pendingButtonPress = false;
  pendingButtonRelease = false;
  portEXIT_CRITICAL(&buttonStateMutex);

  if (capturedPress || capturedRelease) {
    // An encoder-button gesture cancels a pending first screen touch.
    firstArtworkTouchReleased = false;
  }

  if (capturedPress && !debouncedButtonPressed) {
      debouncedButtonPressed = true;
      buttonPressedMs = capturedPressMs;
      longPressReported = false;
      diagnosticPrintln("BUTTON            DOWN");
  }

  if (capturedRelease && debouncedButtonPressed) {
      const uint32_t pressDurationMs = capturedReleaseMs - buttonPressedMs;
      debouncedButtonPressed = false;
      if (!longPressReported) {
        diagnosticPrintf("BUTTON            CLICK | %lu ms\n",
                      static_cast<unsigned long>(pressDurationMs));
        if (applicationMode == ApplicationMode::artwork) {
          if (activeArtworkAlgorithm == ArtworkAlgorithm::flow) {
            generateNextFlowVariation();
          } else if (activeArtworkAlgorithm == ArtworkAlgorithm::radial) {
            generateNextRadialVariation();
          } else if (activeArtworkAlgorithm == ArtworkAlgorithm::attractor) {
            generateNextAttractorVariation();
          } else if (activeArtworkAlgorithm == ArtworkAlgorithm::particles) {
            generateNextParticlesVariation();
          } else if (activeArtworkAlgorithm == ArtworkAlgorithm::orbit) {
            generateNextOrbitVariation();
          } else if (activeArtworkAlgorithm == ArtworkAlgorithm::blobs) {
            generateNextBlobsVariation();
          } else {
            generateNextCellularVariation();
          }
          // Use loop time for the acknowledgement duration: the edge may have
          // waited behind a long render frame before reaching this safe path.
          showButtonFeedback(0, 255, 40, kClickFeedbackMs, nowMs);
        } else if (applicationMode == ApplicationMode::controlMenu) {
          confirmControlMenuSelection(nowMs);
        } else {
          // SETTINGS has one encoder-controlled value, so a short click can
          // safely return to the parent model menu without changing it.
          leaveSettings();
          showButtonFeedback(0, 255, 40, kClickFeedbackMs, nowMs);
        }
      } else {
        diagnosticPrintf("BUTTON            UP | %lu ms\n",
                      static_cast<unsigned long>(pressDurationMs));
      }
  }

  if (debouncedButtonPressed && !longPressReported &&
      nowMs - buttonPressedMs >= kLongPressMs) {
    longPressReported = true;
    diagnosticPrintf("BUTTON            LONG PRESS | threshold %lu ms\n",
                  static_cast<unsigned long>(kLongPressMs));
    if (applicationMode == ApplicationMode::artwork) {
      enterControlMenu();
    } else if (applicationMode == ApplicationMode::settings) {
      // SETTINGS short-click returns to its parent menu; long press preserves
      // the familiar global escape route and restores the paused artwork.
      leaveControlMenu();
    } else {
      leaveControlMenu();
    }
    showButtonFeedback(210, 0, 255, kLongPressFeedbackMs, nowMs);
  }
}

uint8_t randomAutomaticParameterPercent() {
  // The ESP32 hardware random generator produces an independent value for
  // every automatic change. Quantizing it preserves the established 5-percent
  // parameter language used by the physical encoder.
  constexpr uint8_t kParameterValueCount =
      100 / kAutomaticParameterStepPercent + 1;
  return static_cast<uint8_t>((esp_random() % kParameterValueCount) *
                              kAutomaticParameterStepPercent);
}

bool activateAutomaticArtwork(ArtworkAlgorithm nextArtwork,
                              uint8_t parameterPercent) {
  // Initialization stays lazy: a canvas is allocated only when an artwork is
  // first chosen. If allocation fails, leave the current artwork untouched.
  if (nextArtwork == ArtworkAlgorithm::radial &&
      !initializeRadialPrototype()) {
    return false;
  }
  if (nextArtwork == ArtworkAlgorithm::attractor &&
      !initializeAttractorPrototype()) {
    return false;
  }
  if (nextArtwork == ArtworkAlgorithm::particles &&
      !initializeParticlesPrototype()) {
    return false;
  }
  if (nextArtwork == ArtworkAlgorithm::orbit && !initializeOrbitPrototype()) {
    return false;
  }
  if (nextArtwork == ArtworkAlgorithm::blobs && !initializeBlobsPrototype()) {
    return false;
  }
  if (nextArtwork == ArtworkAlgorithm::cellular &&
      !initializeCellularPrototype()) {
    return false;
  }

  clearDisplayBeforeArtworkLoad();
  activeArtworkAlgorithm = nextArtwork;
  if (nextArtwork == ArtworkAlgorithm::flow) {
    flowTurbulencePercent = parameterPercent;
    flowPrototype.setTurbulence(flowTurbulencePercent / 100.0f);
  } else if (nextArtwork == ArtworkAlgorithm::radial) {
    radialTwistPercent = parameterPercent;
    radialPrototype.setTwist(radialTwistPercent / 100.0f);
  } else if (nextArtwork == ArtworkAlgorithm::attractor) {
    attractorChaosPercent = parameterPercent;
    attractorPrototype.setChaos(attractorChaosPercent / 100.0f);
  } else if (nextArtwork == ArtworkAlgorithm::particles) {
    particlesEnergyPercent = parameterPercent;
    particlesPrototype.setEnergy(particlesEnergyPercent / 100.0f);
  } else if (nextArtwork == ArtworkAlgorithm::orbit) {
    orbitEccentricityPercent = parameterPercent;
    orbitPrototype.setEccentricity(orbitEccentricityPercent / 100.0f);
  } else if (nextArtwork == ArtworkAlgorithm::blobs) {
    blobsDriftPercent = parameterPercent;
    blobsPrototype.setDrift(blobsDriftPercent / 100.0f);
  } else {
    cellularMutationPercent = parameterPercent;
    cellularPrototype.setMutation(cellularMutationPercent / 100.0f);
  }

  // A timed change is not a manual adjustment, so it must not pin the LEDs to
  // the direct parameter-feedback color instead of restoring artwork-derived
  // light.
  parameterLedFeedbackActive = false;
  parameterThresholdPulseStage = ParameterThresholdPulseStage::idle;
  applyLedLuminosityForCurrentMode();
  if (ledMaximumPowerPercent == 0) {
    ledRing.clear();
    transmitLedRing();
  }
  return true;
}

void updateAutomaticMode(uint32_t nowMs) {
  if (!automatEnabled) {
    // Keep a disabled period from accumulating while the user explores menus.
    automatLastChangeMs = nowMs;
    return;
  }

  const uint32_t periodMs =
      static_cast<uint32_t>(automatDurationSeconds) * 1000UL;
  if (nowMs - automatLastChangeMs < periodMs) {
    return;
  }
  automatLastChangeMs = nowMs;

  const uint8_t previousArtworkIndex =
      static_cast<uint8_t>(activeArtworkAlgorithm);
  uint8_t nextArtworkIndex = esp_random() % (kArtworkAlgorithmCount - 1U);
  if (nextArtworkIndex >= previousArtworkIndex) {
    ++nextArtworkIndex;
  }
  const ArtworkAlgorithm nextArtwork =
      static_cast<ArtworkAlgorithm>(nextArtworkIndex);
  const uint8_t nextParameterPercent = randomAutomaticParameterPercent();

  if (!activateAutomaticArtwork(nextArtwork, nextParameterPercent)) {
    diagnosticPrintf("AUTOMAT           CHANGE FAIL | %s kept\n",
                  kControlMenuItemNames[previousArtworkIndex]);
    return;
  }

  diagnosticPrintf("AUTOMAT           CHANGE | %s -> %s | %s %u%%\n",
                kControlMenuItemNames[previousArtworkIndex],
                kControlMenuItemNames[nextArtworkIndex],
                activeParameterName(),
                static_cast<unsigned int>(nextParameterPercent));
}

void initializeInput() {
  pinMode(kEncoderAPin, INPUT_PULLUP);
  pinMode(kEncoderBPin, INPUT_PULLUP);
  pinMode(kEncoderButtonPin, INPUT_PULLUP);

  previousEncoderState = readEncoderState();
  attachInterrupt(digitalPinToInterrupt(kEncoderAPin),
                  captureEncoderTransition,
                  CHANGE);
  attachInterrupt(digitalPinToInterrupt(kEncoderBPin),
                  captureEncoderTransition,
                  CHANGE);
  debouncedButtonPressed = digitalRead(kEncoderButtonPin) == LOW;
  buttonPressedMs = millis();
  lastButtonInterruptUs = micros();
  attachInterrupt(digitalPinToInterrupt(kEncoderButtonPin),
                  captureButtonTransition,
                  CHANGE);

  showParameterLedLevel();

  diagnosticPrintln();
  diagnosticPrintln("INPUT test        READY");
  diagnosticPrintln("Rotate: adjust active artwork parameter + LED level feedback");
  diagnosticPrintln("At 0/50/100%: LED ring gives one short double green/white/red pulse");
  diagnosticPrintln("Short click: full ring flashes GREEN");
  diagnosticPrintln("Long press (800 ms): open/close control menu + MAGENTA");
}

void initializeTouch() {
  // setPins() must run before the library calls Wire.begin(). Otherwise the
  // ESP32 would use its generic I2C defaults instead of the CrowPanel wiring.
  Wire.setPins(kTouchSdaPin, kTouchSclPin);
  touchController.begin(mode_touch);
  Wire.setClock(kTouchI2cFrequencyHz);

  // The library exposes identification data, but not its initialization
  // result. A harmless address probe therefore gives us an explicit PASS/FAIL
  // diagnostic when wiring, reset, or I2C communication is unavailable.
  Wire.beginTransmission(kTouchI2cAddress);
  touchControllerDetected = Wire.endTransmission() == 0;

  if (kSerialDiagnosticsEnabled) {
    diagnosticPrintln();
    if (touchControllerDetected) {
      diagnosticPrintf("TOUCH controller  PASS | %s | I2C 0x%02X\n",
                       touchController.version().c_str(),
                       static_cast<unsigned int>(kTouchI2cAddress));
      diagnosticPrintln("TOUCH             READY | raw coordinates will feed LVGL");
    } else {
      diagnosticPrintf("TOUCH controller  FAIL | no response at I2C 0x%02X\n",
                       static_cast<unsigned int>(kTouchI2cAddress));
    }
  }
}

const char* touchGestureName(uint8_t gestureId) {
  // Gesture names come from the CST816T library. During this coordinate test,
  // most continuous samples normally report NONE; that is not an error.
  switch (gestureId) {
    case GESTURE_NONE:
      return "NONE";
    case GESTURE_SWIPE_UP:
      return "SWIPE UP";
    case GESTURE_SWIPE_DOWN:
      return "SWIPE DOWN";
    case GESTURE_SWIPE_LEFT:
      return "SWIPE LEFT";
    case GESTURE_SWIPE_RIGHT:
      return "SWIPE RIGHT";
    case GESTURE_SINGLE_CLICK:
      return "SINGLE CLICK";
    case GESTURE_DOUBLE_CLICK:
      return "DOUBLE CLICK";
    case GESTURE_LONG_PRESS:
      return "LONG PRESS";
    default:
      return "UNKNOWN";
  }
}

void handleArtworkDoubleTouch(bool wasPressed, uint32_t nowMs) {
  if (applicationMode != ApplicationMode::artwork) {
    firstArtworkTouchReleased = false;
    return;
  }

  // Pressing the encoder shaft can also put a finger on the glass. That
  // physical button always wins over the touch gesture.
  if (debouncedButtonPressed || digitalRead(kEncoderButtonPin) == LOW) {
    firstArtworkTouchReleased = false;
    return;
  }

  const bool justPressed = !wasPressed && latestTouchPressed;
  const bool justReleased = wasPressed && !latestTouchPressed;

  if (artworkStandbyMode == ArtworkStandbyMode::waitingForSecondWakeTouch ||
      artworkStandbyMode == ArtworkStandbyMode::pollingFallback) {
    if (artworkStandbyMode == ArtworkStandbyMode::pollingFallback &&
        justPressed && !fallbackFirstTouchStarted) {
      fallbackFirstTouchStarted = true;
    } else if (justReleased &&
               (artworkStandbyMode ==
                    ArtworkStandbyMode::waitingForSecondWakeTouch ||
                fallbackFirstTouchStarted) &&
               !firstWakeTouchReleased) {
      firstWakeTouchReleased = true;
      firstWakeTouchReleasedMs = nowMs;
    } else if (justPressed && firstWakeTouchReleased &&
               nowMs - firstWakeTouchReleasedMs <=
                   kArtworkDoubleTouchGapMs) {
      leaveArtworkStandby();
    } else if (firstWakeTouchReleased &&
               nowMs - firstWakeTouchReleasedMs >
                   kArtworkDoubleTouchGapMs) {
      firstWakeTouchReleased = false;
      if (artworkStandbyMode == ArtworkStandbyMode::waitingForSecondWakeTouch) {
        artworkStandbyMode = ArtworkStandbyMode::sleeping;
      } else {
        fallbackFirstTouchStarted = justPressed;
      }
    }
    return;
  }

  if (artworkStandbyMode == ArtworkStandbyMode::sleeping) {
    return;
  }

  if (ignoreArtworkTouchRelease) {
    if (justReleased) {
      ignoreArtworkTouchRelease = false;
    }
    return;
  }

  if (justPressed && firstArtworkTouchReleased &&
      nowMs - firstArtworkTouchReleasedMs <= kArtworkDoubleTouchGapMs) {
    enterArtworkStandby();
  } else if (justReleased) {
    firstArtworkTouchReleased = true;
    firstArtworkTouchReleasedMs = nowMs;
  } else if (firstArtworkTouchReleased &&
             nowMs - firstArtworkTouchReleasedMs >
                 kArtworkDoubleTouchGapMs) {
    firstArtworkTouchReleased = false;
  }
}

void updateTouch(uint32_t nowMs) {
  if (!touchControllerDetected) {
    return;
  }

  const bool wasPressed = latestTouchPressed;
  if (touchController.available()) {
    lastTouchX = touchController.x;
    lastTouchY = touchController.y;

    // Reject impossible coordinates before sending them to LVGL. Step 0.5
    // confirmed that the raw axes already match the display orientation.
    const bool coordinateIsOnScreen =
        lastTouchX < kScreenWidth && lastTouchY < kScreenHeight;
    latestTouchPressed =
        touchController.finger_num > 0 && coordinateIsOnScreen;

    if (latestTouchPressed) {
      if (kSerialDiagnosticsEnabled) {
        ++touchEventCount;
      }
      lastTouchSampleMs = nowMs;
    }

    if (kSerialDiagnosticsEnabled &&
        nowMs - lastTouchSerialMs >= kTouchSerialPeriodMs) {
      lastTouchSerialMs = nowMs;
      diagnosticPrintf("TOUCH              x %3u | y %3u | fingers %u | gesture %s | %s\n",
                    static_cast<unsigned int>(lastTouchX),
                    static_cast<unsigned int>(lastTouchY),
                    static_cast<unsigned int>(touchController.finger_num),
                    touchGestureName(touchController.gesture_id),
                    coordinateIsOnScreen ? "on screen" : "OUT OF RANGE");
    }
  }

  if (latestTouchPressed &&
      nowMs - lastTouchSampleMs > kTouchReleaseTimeoutMs) {
    latestTouchPressed = false;
  }

  handleArtworkDoubleTouch(wasPressed, nowMs);

  if (applicationMode != ApplicationMode::settings) {
    settingsTouchState = SettingsTouchState::ready;
    return;
  }

  // A physical press takes precedence even if the finger registered on the
  // glass first. Cancel that whole touch, then wait for the finger to leave.
  if (debouncedButtonPressed || digitalRead(kEncoderButtonPin) == LOW) {
    settingsTouchState = latestTouchPressed
                             ? SettingsTouchState::blockedUntilRelease
                             : SettingsTouchState::ready;
    return;
  }

  if (settingsTouchState == SettingsTouchState::blockedUntilRelease) {
    if (!latestTouchPressed) {
      settingsTouchState = SettingsTouchState::ready;
    }
  } else if (settingsTouchState == SettingsTouchState::ready) {
    if (latestTouchPressed) {
      settingsTouchState = SettingsTouchState::tracking;
    }
  } else if (settingsTouchState == SettingsTouchState::tracking) {
    if (!latestTouchPressed) {
      settingsTouchReleasedMs = nowMs;
      settingsTouchState = SettingsTouchState::waitingForButton;
    }
  } else if (latestTouchPressed) {
    // A brief gap in controller samples belongs to the same finger contact.
    settingsTouchState = SettingsTouchState::tracking;
  } else if (nowMs - settingsTouchReleasedMs >=
             kSettingsTouchButtonPriorityMs) {
    settingsTouchState = SettingsTouchState::ready;
    selectNextSettingsControl();
  }
}

uint32_t readLvglTickMilliseconds() {
  // LVGL uses this monotonically increasing millisecond clock for animation,
  // input timing, and deciding when its internal timers must run.
  return millis();
}

void flushLvglPixelsToDisplay(lv_display_t* lvglDisplayBeingFlushed,
                              const lv_area_t* renderedArea,
                              uint8_t* renderedPixels) {
  const int32_t renderedWidth =
      renderedArea->x2 - renderedArea->x1 + 1;
  const int32_t renderedHeight =
      renderedArea->y2 - renderedArea->y1 + 1;

  // DMA transfers pixels without making the CPU send every SPI byte itself.
  // waitDMA() is important with one draw buffer: LVGL must not reuse that
  // memory before the display transfer has actually completed.
  display.pushImageDMA(
      renderedArea->x1,
      renderedArea->y1,
      renderedWidth,
      renderedHeight,
      reinterpret_cast<lgfx::rgb565_t*>(renderedPixels));
  display.waitDMA();
  if (kSerialDiagnosticsEnabled) {
    ++displayFlushCount;
  }

  lv_display_flush_ready(lvglDisplayBeingFlushed);
}

void readTouchForLvgl(lv_indev_t* inputDevice, lv_indev_data_t* inputData) {
  (void)inputDevice;

  inputData->point.x = lastTouchX;
  inputData->point.y = lastTouchY;
  inputData->state = latestTouchPressed
                         ? LV_INDEV_STATE_PRESSED
                         : LV_INDEV_STATE_RELEASED;
}

// Both menus share the same final logo. The flash-backed A8 data preserves
// the real Madimi One outlines without loading a font or PNG decoder at runtime.
void createMenuBrandLogo(lv_obj_t* screen) {
  static const lv_image_dsc_t logoDescriptor = []() {
    lv_image_dsc_t descriptor{};
    descriptor.header.magic = LV_IMAGE_HEADER_MAGIC;
    descriptor.header.cf = LV_COLOR_FORMAT_A8;
    descriptor.header.w = kBrandLogoWidth;
    descriptor.header.h = kBrandLogoHeight;
    descriptor.header.stride = kBrandLogoWidth;
    descriptor.data_size = sizeof(kBrandLogoAlpha);
    descriptor.data = kBrandLogoAlpha;
    return descriptor;
  }();
  lv_obj_t* logoImage = lv_image_create(screen);
  lv_image_set_src(logoImage, &logoDescriptor);
  lv_obj_align(logoImage, LV_ALIGN_TOP_MID, 0, 36);
  lv_obj_set_style_image_recolor(logoImage, lv_color_white(), 0);
  lv_obj_set_style_image_recolor_opa(logoImage, LV_OPA_COVER, 0);
  lv_obj_remove_flag(logoImage, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_remove_flag(logoImage, LV_OBJ_FLAG_CLICKABLE);
}

void createControlMenuScreen() {
  lv_obj_t* screen = lv_screen_active();
  lv_obj_clean(screen);
  settingsScreenOwnsEncoder = false;
  settingsAutomatLabel = nullptr;
  settingsAutomatValueLabel = nullptr;
  settingsLuminosityLabel = nullptr;
  settingsLuminosityValueLabel = nullptr;
  lv_obj_set_style_bg_color(screen, lv_color_black(), 0);
  lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
  lv_obj_remove_flag(screen, LV_OBJ_FLAG_SCROLLABLE);

  lv_obj_t* circularFrame = lv_obj_create(screen);
  lv_obj_set_size(circularFrame, 342, 342);
  lv_obj_center(circularFrame);
  lv_obj_set_style_radius(circularFrame, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_bg_opa(circularFrame, LV_OPA_TRANSP, 0);
  // The circular viewport is structural only: menu artwork must meet pure
  // black at the edge rather than appearing inside a white outlined disc.
  lv_obj_set_style_border_width(circularFrame, 0, 0);
  lv_obj_remove_flag(circularFrame, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_remove_flag(circularFrame, LV_OBJ_FLAG_CLICKABLE);

  createMenuBrandLogo(screen);

  controlMenuCurrentLabel = lv_label_create(screen);
  lv_obj_set_width(controlMenuCurrentLabel, 280);
  lv_obj_set_style_text_align(controlMenuCurrentLabel, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_set_style_text_color(controlMenuCurrentLabel, lv_color_white(), 0);
  lv_obj_set_style_text_font(
      controlMenuCurrentLabel, &lv_font_montserrat_28, 0);
  lv_obj_align(controlMenuCurrentLabel, LV_ALIGN_TOP_MID, 0, 112);

  // Every menu image is A8: its stored byte controls opacity while this white
  // tint gives the menu the same sparse monochrome language as the art.
  controlMenuCurrentIcon = lv_image_create(screen);
  lv_image_set_src(controlMenuCurrentIcon,
                   kControlMenuItemIcons[selectedControlMenuIndex]);
  lv_obj_set_size(controlMenuCurrentIcon, kMenuIconWidth, kMenuIconHeight);
  lv_obj_align(controlMenuCurrentIcon, LV_ALIGN_TOP_MID, 0, 156);
  lv_obj_set_style_image_recolor(controlMenuCurrentIcon, lv_color_white(), 0);
  lv_obj_set_style_image_recolor_opa(
      controlMenuCurrentIcon, LV_OPA_COVER, 0);
  lv_obj_remove_flag(controlMenuCurrentIcon, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_remove_flag(controlMenuCurrentIcon, LV_OBJ_FLAG_CLICKABLE);
}

void updateSettingsScreen() {
  if (!lvglInitialized || settingsAutomatLabel == nullptr ||
      settingsAutomatValueLabel == nullptr || settingsLuminosityLabel == nullptr ||
      settingsLuminosityValueLabel == nullptr) {
    return;
  }

  lv_obj_set_style_text_color(settingsAutomatLabel,
                              selectedSettingsControl == SettingsSelection::automat
                                  ? lv_color_white()
                                  : lv_color_hex(0x78828E),
                              0);
  lv_obj_set_style_text_color(settingsAutomatValueLabel,
                              selectedSettingsControl == SettingsSelection::automat
                                  ? (automatEnabled ? lv_color_white()
                                                    : lv_color_hex(0x78828E))
                                  : lv_color_hex(0x58606A),
                              0);

  char automatValueText[12];
  if (automatEnabled) {
    snprintf(automatValueText, sizeof(automatValueText), "%u s",
             static_cast<unsigned int>(automatDurationSeconds));
  } else {
    snprintf(automatValueText, sizeof(automatValueText), "OFF");
  }
  lv_label_set_text(settingsAutomatValueLabel, automatValueText);

  lv_obj_set_style_text_color(
      settingsLuminosityLabel,
      selectedSettingsControl == SettingsSelection::luminosity
          ? lv_color_white()
          : lv_color_hex(0x78828E),
      0);
  lv_obj_set_style_text_color(
      settingsLuminosityValueLabel,
      selectedSettingsControl == SettingsSelection::luminosity
          ? (ledMaximumPowerPercent == 0 ? lv_color_hex(0x78828E)
                                         : lv_color_white())
          : lv_color_hex(0x58606A),
      0);
  char luminosityValueText[8];
  if (ledMaximumPowerPercent == 0) {
    snprintf(luminosityValueText, sizeof(luminosityValueText), "OFF");
  } else {
    snprintf(luminosityValueText, sizeof(luminosityValueText), "%u%%",
             static_cast<unsigned int>(ledMaximumPowerPercent));
  }
  lv_label_set_text(settingsLuminosityValueLabel, luminosityValueText);
  lv_obj_invalidate(lv_screen_active());
}

void adjustAutomatValue(int8_t direction) {
  if (direction == 0) {
    return;
  }

  const bool previousEnabled = automatEnabled;
  const uint8_t previousDurationSeconds = automatDurationSeconds;
  if (direction > 0) {
    if (!automatEnabled) {
      automatEnabled = true;
      automatDurationSeconds = kAutomatMinimumDurationSeconds;
    } else {
      automatDurationSeconds = static_cast<uint8_t>(min(
          static_cast<int>(automatDurationSeconds) +
              kAutomatDurationStepSeconds,
          static_cast<int>(kAutomatMaximumDurationSeconds)));
    }
  } else if (automatEnabled &&
             automatDurationSeconds <= kAutomatMinimumDurationSeconds) {
    // OFF sits immediately before 15 seconds in the encoder sequence.
    automatEnabled = false;
    automatDurationSeconds = kAutomatMinimumDurationSeconds;
  } else if (automatEnabled) {
    automatDurationSeconds = static_cast<uint8_t>(
        automatDurationSeconds - kAutomatDurationStepSeconds);
  }

  if (automatEnabled == previousEnabled &&
      automatDurationSeconds == previousDurationSeconds) {
    return;
  }
  saveSettings();
  updateSettingsScreen();
  if (automatEnabled) {
    diagnosticPrintf("SETTINGS          AUTOMAT | %u s\n",
                  static_cast<unsigned int>(automatDurationSeconds));
  } else {
    diagnosticPrintln("SETTINGS          AUTOMAT | OFF");
  }
}

void adjustLuminosityValue(int8_t direction) {
  if (direction == 0) {
    return;
  }

  const uint8_t previousLuminosityPercent = ledMaximumPowerPercent;
  const int nextLuminosityPercent =
      static_cast<int>(ledMaximumPowerPercent) +
      direction * kLedLuminosityStepPercent;
  setLedMaximumPowerPercent(static_cast<uint8_t>(constrain(
      nextLuminosityPercent, 0, 100)));
  if (ledMaximumPowerPercent == previousLuminosityPercent) {
    return;
  }

  // setBrightness() rescales the ring buffer but does not transmit it. Send
  // the current menu halo immediately so the selected maximum is observable
  // while the user turns the encoder.
  transmitLedRing();
  saveSettings();
  updateSettingsScreen();
  if (ledMaximumPowerPercent == 0) {
    diagnosticPrintln("SETTINGS          LUMINOSITY | OFF");
  } else {
    diagnosticPrintf("SETTINGS          LUMINOSITY | %u%%\n",
                  static_cast<unsigned int>(ledMaximumPowerPercent));
  }
}

void selectNextSettingsControl() {
  selectedSettingsControl =
      selectedSettingsControl == SettingsSelection::automat
          ? SettingsSelection::luminosity
          : SettingsSelection::automat;
  updateSettingsScreen();
  diagnosticPrintf("SETTINGS          SELECT | %s\n",
                selectedSettingsControl == SettingsSelection::automat
                    ? "AUTOMAT"
                    : "LUMINOSITY");
}

void disableAutomatForManualArtworkSelection() {
  if (!automatEnabled) {
    return;
  }
  automatEnabled = false;
  saveSettings();
  diagnosticPrintln("AUTOMAT           OFF | manual artwork selection");
}

void createSettingsScreen() {
  lv_obj_t* screen = lv_screen_active();
  lv_obj_clean(screen);
  controlMenuCurrentLabel = nullptr;
  controlMenuCurrentIcon = nullptr;
  lv_obj_set_style_bg_color(screen, lv_color_black(), 0);
  lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
  lv_obj_remove_flag(screen, LV_OBJ_FLAG_SCROLLABLE);

  lv_obj_t* circularFrame = lv_obj_create(screen);
  lv_obj_set_size(circularFrame, 342, 342);
  lv_obj_center(circularFrame);
  lv_obj_set_style_radius(circularFrame, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_bg_opa(circularFrame, LV_OPA_TRANSP, 0);
  // SETTINGS shares the menu's unframed, full-black screen treatment.
  lv_obj_set_style_border_width(circularFrame, 0, 0);
  lv_obj_remove_flag(circularFrame, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_remove_flag(circularFrame, LV_OBJ_FLAG_CLICKABLE);

  createMenuBrandLogo(screen);

  lv_obj_t* settingsTitleLabel = lv_label_create(screen);
  lv_label_set_text(settingsTitleLabel, "SETTINGS");
  lv_obj_set_style_text_color(settingsTitleLabel, lv_color_hex(0xAAB4BE), 0);
  lv_obj_set_style_text_font(settingsTitleLabel, &lv_font_montserrat_14, 0);
  lv_obj_align(settingsTitleLabel, LV_ALIGN_TOP_MID, 0, 92);

  settingsAutomatLabel = lv_label_create(screen);
  lv_label_set_text(settingsAutomatLabel, "AUTOMAT");
  lv_obj_set_style_text_font(settingsAutomatLabel, &lv_font_montserrat_20, 0);
  lv_obj_align(settingsAutomatLabel, LV_ALIGN_TOP_MID, 0, 126);

  settingsAutomatValueLabel = lv_label_create(screen);
  lv_obj_set_style_text_font(settingsAutomatValueLabel,
                              &lv_font_montserrat_28,
                              0);
  lv_obj_align(settingsAutomatValueLabel, LV_ALIGN_TOP_MID, 0, 160);

  settingsLuminosityLabel = lv_label_create(screen);
  lv_label_set_text(settingsLuminosityLabel, "LUMINOSITY");
  lv_obj_set_style_text_font(settingsLuminosityLabel, &lv_font_montserrat_20,
                             0);
  lv_obj_align(settingsLuminosityLabel, LV_ALIGN_TOP_MID, 0, 208);

  settingsLuminosityValueLabel = lv_label_create(screen);
  lv_obj_set_style_text_font(settingsLuminosityValueLabel,
                              &lv_font_montserrat_28,
                              0);
  lv_obj_align(settingsLuminosityValueLabel, LV_ALIGN_TOP_MID, 0, 242);

  lv_obj_t* versionLabel = lv_label_create(screen);
  lv_label_set_text(versionLabel, kApplicationVersion);
  lv_obj_set_style_text_color(versionLabel, lv_color_hex(0x78828E), 0);
  lv_obj_set_style_text_font(versionLabel, &lv_font_montserrat_14, 0);
  lv_obj_align(versionLabel, LV_ALIGN_BOTTOM_MID, 0, -44);

  updateSettingsScreen();
}

void initializeLvgl() {
  lv_init();
  lv_tick_set_cb(readLvglTickMilliseconds);

  lvglDisplay = lv_display_create(kScreenWidth, kScreenHeight);
  if (lvglDisplay == nullptr) {
    diagnosticPrintln("LVGL              FAIL | display allocation");
    return;
  }

  lv_display_set_color_format(lvglDisplay, LV_COLOR_FORMAT_RGB565);
  lv_display_set_flush_cb(lvglDisplay, flushLvglPixelsToDisplay);
  lv_display_set_buffers(lvglDisplay,
                         lvglDrawBuffer,
                         nullptr,
                         sizeof(lvglDrawBuffer),
                         LV_DISPLAY_RENDER_MODE_PARTIAL);

  lvglTouchInput = lv_indev_create();
  if (lvglTouchInput == nullptr) {
    diagnosticPrintln("LVGL              FAIL | touch input allocation");
    return;
  }
  lv_indev_set_type(lvglTouchInput, LV_INDEV_TYPE_POINTER);
  lv_indev_set_read_cb(lvglTouchInput, readTouchForLvgl);

  createControlMenuScreen();
  lvglInitialized = true;
  updateControlMenuText();

  diagnosticPrintln();
  diagnosticPrintf("LVGL              READY | %d.%d.%d | RGB565 | buffer %u bytes\n",
                LVGL_VERSION_MAJOR,
                LVGL_VERSION_MINOR,
                LVGL_VERSION_PATCH,
                static_cast<unsigned int>(sizeof(lvglDrawBuffer)));
  diagnosticPrintln("LVGL menu         READY | encoder navigation | long press exits");
}

void enterControlMenu() {
  if (!lvglInitialized) {
    initializeLvgl();
  }
  if (!lvglInitialized) {
    diagnosticPrintln("MENU              FAIL | LVGL unavailable");
    return;
  }

  applicationMode = ApplicationMode::controlMenu;
  applyLedLuminosityForCurrentMode();
  // A long press from SETTINGS returns directly to the artwork, leaving the
  // SETTINGS LVGL widgets as the last active screen. Recreate the model menu
  // before navigating it again; otherwise the encoder moves a hidden menu
  // while the stale SETTINGS values appear frozen on screen.
  if (controlMenuCurrentLabel == nullptr) {
    createControlMenuScreen();
  }
  switch (activeArtworkAlgorithm) {
    case ArtworkAlgorithm::flow:
      selectedControlMenuIndex = 0;
      break;
    case ArtworkAlgorithm::radial:
      selectedControlMenuIndex = 1;
      break;
    case ArtworkAlgorithm::attractor:
      selectedControlMenuIndex = 2;
      break;
    case ArtworkAlgorithm::particles:
      selectedControlMenuIndex = 3;
      break;
    case ArtworkAlgorithm::orbit:
      selectedControlMenuIndex = 4;
      break;
    case ArtworkAlgorithm::blobs:
      selectedControlMenuIndex = 5;
      break;
    case ArtworkAlgorithm::cellular:
      selectedControlMenuIndex = 6;
      break;
  }
  updateControlMenuText();
  lv_obj_invalidate(lv_screen_active());
  showControlMenuLedSelection();
  diagnosticPrintf("MODE              CONTROL MENU | %s paused\n",
                kControlMenuItemNames[selectedControlMenuIndex]);
}

void enterSettings() {
  applicationMode = ApplicationMode::settings;
  settingsScreenOwnsEncoder = true;
  selectedSettingsControl = SettingsSelection::automat;
  settingsTouchState = latestTouchPressed
                           ? SettingsTouchState::blockedUntilRelease
                           : SettingsTouchState::ready;
  discardPendingEncoderDetents();
  applyLedLuminosityForCurrentMode();
  createSettingsScreen();
  showControlMenuLedSelection();
  diagnosticPrintf("MODE              SETTINGS | automat %s | duration %u s\n",
                automatEnabled ? "ON" : "OFF",
                static_cast<unsigned int>(automatDurationSeconds));
}

void leaveSettings() {
  applicationMode = ApplicationMode::controlMenu;
  settingsScreenOwnsEncoder = false;
  applyLedLuminosityForCurrentMode();
  createControlMenuScreen();
  updateControlMenuText();
  showControlMenuLedSelection();
  diagnosticPrintln("MODE              CONTROL MENU | SETTINGS closed");
}

void leaveControlMenu() {
  applicationMode = ApplicationMode::artwork;
  settingsScreenOwnsEncoder = false;
  parameterLedFeedbackActive = false;
  parameterThresholdPulseStage = ParameterThresholdPulseStage::idle;
  applyLedLuminosityForCurrentMode();
  // Never let a menu halo linger while an artwork starts, especially when the
  // saved LUMINOSITY value is OFF and the artwork ring must be fully black.
  ledRing.clear();
  transmitLedRing();
  // Time spent browsing models or SETTINGS does not consume an automatic
  // period. The full chosen duration starts again when artwork is visible.
  automatLastChangeMs = millis();
  clearDisplayBeforeArtworkLoad();
  switch (activeArtworkAlgorithm) {
    case ArtworkAlgorithm::flow:
      flowPrototype.showCurrentFrame();
      break;
    case ArtworkAlgorithm::radial:
      radialPrototype.showCurrentFrame();
      break;
    case ArtworkAlgorithm::attractor:
      attractorPrototype.showCurrentFrame();
      break;
    case ArtworkAlgorithm::particles:
      particlesPrototype.showCurrentFrame();
      break;
    case ArtworkAlgorithm::orbit:
      orbitPrototype.showCurrentFrame();
      break;
    case ArtworkAlgorithm::blobs:
      blobsPrototype.showCurrentFrame();
      break;
    case ArtworkAlgorithm::cellular:
      cellularPrototype.showCurrentFrame();
      break;
  }
  uint8_t activeMenuIndex = 0;
  switch (activeArtworkAlgorithm) {
    case ArtworkAlgorithm::flow:
      activeMenuIndex = 0;
      break;
    case ArtworkAlgorithm::radial:
      activeMenuIndex = 1;
      break;
    case ArtworkAlgorithm::attractor:
      activeMenuIndex = 2;
      break;
    case ArtworkAlgorithm::particles:
      activeMenuIndex = 3;
      break;
    case ArtworkAlgorithm::orbit:
      activeMenuIndex = 4;
      break;
    case ArtworkAlgorithm::blobs:
      activeMenuIndex = 5;
      break;
    case ArtworkAlgorithm::cellular:
      activeMenuIndex = 6;
      break;
  }
  diagnosticPrintf("MODE              ARTWORK | %s restored\n",
                kControlMenuItemNames[activeMenuIndex]);
}

void confirmControlMenuSelection(uint32_t nowMs) {
  if (selectedControlMenuIndex < kArtworkAlgorithmCount) {
    disableAutomatForManualArtworkSelection();
  }

  if (selectedControlMenuIndex == 0) {
    diagnosticPrintln("MENU              CONFIRM | VORTEX");
    activeArtworkAlgorithm = ArtworkAlgorithm::flow;
    leaveControlMenu();
    showButtonFeedback(0, 255, 40, kClickFeedbackMs, nowMs);
    return;
  }

  if (selectedControlMenuIndex == 1) {
    diagnosticPrintln("MENU              CONFIRM | RADIAL");
    if (!initializeRadialPrototype()) {
      showButtonFeedback(255, 90, 0, kClickFeedbackMs, nowMs);
      return;
    }
    activeArtworkAlgorithm = ArtworkAlgorithm::radial;
    leaveControlMenu();
    showButtonFeedback(0, 255, 40, kClickFeedbackMs, nowMs);
    return;
  }

  if (selectedControlMenuIndex == 2) {
    diagnosticPrintln("MENU              CONFIRM | ATTRACTOR");
    if (!initializeAttractorPrototype()) {
      showButtonFeedback(255, 90, 0, kClickFeedbackMs, nowMs);
      return;
    }
    activeArtworkAlgorithm = ArtworkAlgorithm::attractor;
    leaveControlMenu();
    showButtonFeedback(0, 255, 40, kClickFeedbackMs, nowMs);
    return;
  }

  if (selectedControlMenuIndex == 3) {
    diagnosticPrintln("MENU              CONFIRM | PARTICLES");
    if (!initializeParticlesPrototype()) {
      showButtonFeedback(255, 90, 0, kClickFeedbackMs, nowMs);
      return;
    }
    activeArtworkAlgorithm = ArtworkAlgorithm::particles;
    leaveControlMenu();
    showButtonFeedback(0, 255, 40, kClickFeedbackMs, nowMs);
    return;
  }

  if (selectedControlMenuIndex == 4) {
    diagnosticPrintln("MENU              CONFIRM | ORBIT");
    if (!initializeOrbitPrototype()) {
      showButtonFeedback(255, 90, 0, kClickFeedbackMs, nowMs);
      return;
    }
    activeArtworkAlgorithm = ArtworkAlgorithm::orbit;
    leaveControlMenu();
    showButtonFeedback(0, 255, 40, kClickFeedbackMs, nowMs);
    return;
  }

  if (selectedControlMenuIndex == 5) {
    diagnosticPrintln("MENU              CONFIRM | BLOBS");
    if (!initializeBlobsPrototype()) {
      showButtonFeedback(255, 90, 0, kClickFeedbackMs, nowMs);
      return;
    }
    activeArtworkAlgorithm = ArtworkAlgorithm::blobs;
    leaveControlMenu();
    showButtonFeedback(0, 255, 40, kClickFeedbackMs, nowMs);
    return;
  }

  if (selectedControlMenuIndex == 6) {
    diagnosticPrintln("MENU              CONFIRM | CELLULAR");
    if (!initializeCellularPrototype()) {
      showButtonFeedback(255, 90, 0, kClickFeedbackMs, nowMs);
      return;
    }
    activeArtworkAlgorithm = ArtworkAlgorithm::cellular;
    leaveControlMenu();
    showButtonFeedback(0, 255, 40, kClickFeedbackMs, nowMs);
    return;
  }

  if (selectedControlMenuIndex == 7) {
    diagnosticPrintln("MENU              CONFIRM | SETTINGS");
    enterSettings();
    showButtonFeedback(0, 255, 40, kClickFeedbackMs, nowMs);
    return;
  }

  diagnosticPrintf("MENU              UNAVAILABLE | %s\n",
                kControlMenuItemNames[selectedControlMenuIndex]);
  // Amber distinguishes an unavailable placeholder from successful green and
  // long-press magenta acknowledgements.
  showButtonFeedback(255, 90, 0, kClickFeedbackMs, nowMs);
}

void updateSoakMetrics(uint32_t loopWorkTimeUs) {
  const uint32_t currentFreeHeapBytes = ESP.getFreeHeap();
  const uint32_t currentFreePsramBytes = ESP.getFreePsram();

  if (currentFreeHeapBytes < minimumFreeHeapBytes) {
    minimumFreeHeapBytes = currentFreeHeapBytes;
  }
  if (currentFreePsramBytes < minimumFreePsramBytes) {
    minimumFreePsramBytes = currentFreePsramBytes;
  }
  if (loopWorkTimeUs > maximumLoopWorkTimeUs) {
    maximumLoopWorkTimeUs = loopWorkTimeUs;
  }
}

void initializeFlowPrototype() {
  const uint32_t psramBeforeCanvasBytes = ESP.getFreePsram();
  if (!flowPrototype.begin(activeFlowSeed)) {
    diagnosticPrintln();
    diagnosticPrintln("FLOW              FAIL | 360 x 360 PSRAM canvas allocation");
    return;
  }

  const uint32_t psramAfterCanvasBytes = ESP.getFreePsram();
  diagnosticPrintln();
  diagnosticPrintf("FLOW              READY | seed %lu | particles 140\n",
                static_cast<unsigned long>(flowPrototype.seed()));
  diagnosticPrintf("FLOW canvas       %u bytes | PSRAM used %lu bytes\n",
                static_cast<unsigned int>(flowPrototype.canvasSizeBytes()),
                static_cast<unsigned long>(psramBeforeCanvasBytes -
                                           psramAfterCanvasBytes));
  flowPrototype.setTurbulence(flowTurbulencePercent / 100.0f);
  diagnosticPrintf("FLOW turbulence   %u%% | encoder step %u%%\n",
                static_cast<unsigned int>(flowTurbulencePercent),
                static_cast<unsigned int>(kFlowTurbulenceStepPercent));
  diagnosticPrintln("FLOW / VORTEX     FINAL | step 1.6 creative baseline");
}

bool initializeRadialPrototype() {
  if (radialPrototype.isReady()) {
    return true;
  }

  const uint32_t psramBeforeCanvasBytes = ESP.getFreePsram();
  if (!radialPrototype.begin(activeRadialSeed)) {
    diagnosticPrintln();
    diagnosticPrintln("RADIAL            FAIL | 360 x 360 PSRAM canvas allocation");
    return false;
  }

  radialPrototype.setTwist(radialTwistPercent / 100.0f);
  const uint32_t psramAfterCanvasBytes = ESP.getFreePsram();
  diagnosticPrintln();
  diagnosticPrintf("RADIAL            READY | seed %lu | %s | arms %u | symmetry %u\n",
                static_cast<unsigned long>(radialPrototype.seed()),
                radialPrototype.compositionFamilyName(),
                static_cast<unsigned int>(radialPrototype.armCount()),
                static_cast<unsigned int>(radialPrototype.symmetry()));
  diagnosticPrintf("RADIAL canvas     %u bytes | PSRAM used %lu bytes\n",
                static_cast<unsigned int>(radialPrototype.canvasSizeBytes()),
                static_cast<unsigned long>(psramBeforeCanvasBytes -
                                           psramAfterCanvasBytes));
  diagnosticPrintf("RADIAL twist      %u%% | encoder step %u%%\n",
                static_cast<unsigned int>(radialTwistPercent),
                static_cast<unsigned int>(kRadialTwistStepPercent));
  diagnosticPrintln("RADIAL            PROTOTYPE | step 3.0 physical review required");
  return true;
}

bool initializeAttractorPrototype() {
  if (attractorPrototype.isReady()) {
    return true;
  }

  const uint32_t psramBeforeCanvasBytes = ESP.getFreePsram();
  if (!attractorPrototype.begin(activeAttractorSeed)) {
    diagnosticPrintln();
    diagnosticPrintln("ATTRACTOR         FAIL | 360 x 360 PSRAM canvas allocation");
    return false;
  }

  attractorPrototype.setChaos(attractorChaosPercent / 100.0f);
  const uint32_t psramAfterCanvasBytes = ESP.getFreePsram();
  diagnosticPrintln();
  diagnosticPrintf("ATTRACTOR         READY | seed %lu | %s\n",
                static_cast<unsigned long>(attractorPrototype.seed()),
                attractorPrototype.familyName());
  diagnosticPrintf("ATTRACTOR canvas  %u bytes | PSRAM used %lu bytes\n",
                static_cast<unsigned int>(attractorPrototype.canvasSizeBytes()),
                static_cast<unsigned long>(psramBeforeCanvasBytes -
                                           psramAfterCanvasBytes));
  diagnosticPrintf("ATTRACTOR chaos   %u%% | encoder step %u%%\n",
                static_cast<unsigned int>(attractorChaosPercent),
                static_cast<unsigned int>(kAttractorChaosStepPercent));
  diagnosticPrintln("ATTRACTOR         PROTOTYPE | step 4.0 physical review required");
  return true;
}

bool initializeParticlesPrototype() {
  if (particlesPrototype.isReady()) return true;
  particlesPrototype.setEnergy(particlesEnergyPercent / 100.0f);
  if (!particlesPrototype.begin(activeParticlesSeed)) {
    diagnosticPrintln("PARTICLES          FAIL | PSRAM canvas allocation");
    return false;
  }
  diagnosticPrintf("PARTICLES          READY | seed %lu | %s | energy %u%% | canvas %u bytes\n",
                static_cast<unsigned long>(activeParticlesSeed),
                particlesPrototype.familyName(), particlesEnergyPercent,
                static_cast<unsigned int>(particlesPrototype.canvasSizeBytes()));
  diagnosticPrintln("PARTICLES          BASELINE | step 5.1 physically approved");
  return true;
}

bool initializeOrbitPrototype() {
  if (orbitPrototype.isReady()) {
    return true;
  }

  const uint32_t psramBeforeCanvasBytes = ESP.getFreePsram();
  orbitPrototype.setEccentricity(orbitEccentricityPercent / 100.0f);
  if (!orbitPrototype.begin(activeOrbitSeed)) {
    diagnosticPrintln("ORBIT             FAIL | PSRAM canvas allocation");
    return false;
  }
  const uint32_t psramAfterCanvasBytes = ESP.getFreePsram();
  diagnosticPrintf(
      "ORBIT             READY | seed %lu | attractors %u | symmetry %u | eccentricity %u%%\n",
      static_cast<unsigned long>(activeOrbitSeed),
      static_cast<unsigned int>(orbitPrototype.attractorCount()),
      static_cast<unsigned int>(orbitPrototype.symmetryOrder()),
      static_cast<unsigned int>(orbitEccentricityPercent));
  diagnosticPrintf("ORBIT canvas      %u bytes | PSRAM used %lu bytes\n",
                static_cast<unsigned int>(orbitPrototype.canvasSizeBytes()),
                static_cast<unsigned long>(psramBeforeCanvasBytes -
                                           psramAfterCanvasBytes));
  diagnosticPrintln("ORBIT             BASELINE | final physically approved algorithm");
  return true;
}

bool initializeBlobsPrototype() {
  if (blobsPrototype.isReady()) {
    return true;
  }

  const uint32_t psramBeforeCanvasBytes = ESP.getFreePsram();
  blobsPrototype.setDrift(blobsDriftPercent / 100.0f);
  if (!blobsPrototype.begin(activeBlobsSeed)) {
    diagnosticPrintln("BLOBS             FAIL | PSRAM canvas allocation");
    return false;
  }
  const uint32_t psramAfterCanvasBytes = ESP.getFreePsram();
  diagnosticPrintf("BLOBS             READY | seed %lu | drift %u%%\n",
                static_cast<unsigned long>(activeBlobsSeed),
                static_cast<unsigned int>(blobsDriftPercent));
  diagnosticPrintf("BLOBS canvas      %u bytes | PSRAM used %lu bytes\n",
                static_cast<unsigned int>(blobsPrototype.canvasSizeBytes()),
                static_cast<unsigned long>(psramBeforeCanvasBytes -
                                           psramAfterCanvasBytes));
  diagnosticPrintf("BLOBS drift       %u%% | encoder step %u%%\n",
                static_cast<unsigned int>(blobsDriftPercent),
                static_cast<unsigned int>(kBlobsDriftStepPercent));
  diagnosticPrintln("BLOBS             BASELINE | final physically approved algorithm");
  return true;
}

bool initializeCellularPrototype() {
  if (cellularPrototype.isReady()) {
    return true;
  }

  const uint32_t psramBeforeAllocationBytes = ESP.getFreePsram();
  cellularPrototype.setMutation(cellularMutationPercent / 100.0f);
  if (!cellularPrototype.begin(activeCellularSeed)) {
    diagnosticPrintln("CELLULAR          FAIL | PSRAM model/canvas allocation");
    return false;
  }
  const uint32_t psramAfterAllocationBytes = ESP.getFreePsram();
  reportedCellularAutomaticReseedCount =
      cellularPrototype.automaticReseedCount();
  diagnosticPrintf(
      "CELLULAR          READY | seed %lu | mutation %u%% | population %u\n",
      static_cast<unsigned long>(activeCellularSeed),
      static_cast<unsigned int>(cellularMutationPercent),
      static_cast<unsigned int>(cellularPrototype.livingCellCount()));
  diagnosticPrintf(
      "CELLULAR memory   canvas %u bytes | model %u bytes | PSRAM used %lu bytes\n",
      static_cast<unsigned int>(cellularPrototype.canvasSizeBytes()),
      static_cast<unsigned int>(cellularPrototype.modelSizeBytes()),
      static_cast<unsigned long>(psramBeforeAllocationBytes -
                                 psramAfterAllocationBytes));
  diagnosticPrintf("CELLULAR mutation %u%% | encoder step %u%%\n",
                static_cast<unsigned int>(cellularMutationPercent),
                static_cast<unsigned int>(kCellularMutationStepPercent));
  diagnosticPrintf("CELLULAR rotation %s | %.1f s per turn\n",
                cellularPrototype.rotationDirectionName(),
                static_cast<double>(cellularPrototype.rotationPeriodSeconds()));
  diagnosticPrintf("CELLULAR breath   %.1f s | inhale 5.0 s | exhale 7.0 s | zoom 96-124%% | rotation 30-85%%\n",
                static_cast<double>(cellularPrototype.breathingPeriodSeconds()));
  diagnosticPrintln("CELLULAR          BASELINE | screen and LEDs physically approved");
  return true;
}

void reportArtworkHeartbeat(uint32_t nowMs) {
  const char* algorithmName = "FLOW";
  const char* parameterName = "turbulence";
  float activeFramesPerSecond = flowPrototype.measuredFramesPerSecond();
  uint32_t activeSlowestFrameUs = flowPrototype.slowestFrameTimeUs();
  uint32_t activeFrameCount = flowPrototype.renderedFrameCount();
  uint32_t activeSeed = flowPrototype.seed();
  float activeCenterX = flowPrototype.vortexCenterX();
  float activeCenterY = flowPrototype.vortexCenterY();

  if (activeArtworkAlgorithm == ArtworkAlgorithm::radial) {
    algorithmName = "RADIAL";
    parameterName = "twist";
    activeFramesPerSecond = radialPrototype.measuredFramesPerSecond();
    activeSlowestFrameUs = radialPrototype.slowestFrameTimeUs();
    activeFrameCount = radialPrototype.renderedFrameCount();
    activeSeed = radialPrototype.seed();
    activeCenterX = radialPrototype.centerX();
    activeCenterY = radialPrototype.centerY();
  } else if (activeArtworkAlgorithm == ArtworkAlgorithm::attractor) {
    algorithmName = "ATTRACTOR";
    parameterName = "chaos";
    activeFramesPerSecond = attractorPrototype.measuredFramesPerSecond();
    activeSlowestFrameUs = attractorPrototype.slowestFrameTimeUs();
    activeFrameCount = attractorPrototype.renderedFrameCount();
    activeSeed = attractorPrototype.seed();
    activeCenterX = kScreenWidth * 0.5f;
    activeCenterY = kScreenHeight * 0.5f;
  }

  if (activeArtworkAlgorithm == ArtworkAlgorithm::particles) {
    algorithmName = "PARTICLES";
    parameterName = "energy";
    activeFramesPerSecond = particlesPrototype.measuredFramesPerSecond();
    activeSlowestFrameUs = particlesPrototype.slowestFrameTimeUs();
    activeFrameCount = particlesPrototype.renderedFrameCount();
    activeSeed = particlesPrototype.seed();
    activeCenterX = kScreenWidth * 0.5f;
    activeCenterY = kScreenHeight * 0.5f;
  } else if (activeArtworkAlgorithm == ArtworkAlgorithm::orbit) {
    algorithmName = "ORBIT";
    parameterName = "eccentricity";
    activeFramesPerSecond = orbitPrototype.measuredFramesPerSecond();
    activeSlowestFrameUs = orbitPrototype.slowestFrameTimeUs();
    activeFrameCount = orbitPrototype.renderedFrameCount();
    activeSeed = orbitPrototype.seed();
    activeCenterX = orbitPrototype.kaleidoscopeCenterX();
    activeCenterY = orbitPrototype.kaleidoscopeCenterY();
  } else if (activeArtworkAlgorithm == ArtworkAlgorithm::blobs) {
    algorithmName = "BLOBS";
    parameterName = "drift";
    activeFramesPerSecond = blobsPrototype.measuredFramesPerSecond();
    activeSlowestFrameUs = blobsPrototype.slowestFrameTimeUs();
    activeFrameCount = blobsPrototype.renderedFrameCount();
    activeSeed = blobsPrototype.seed();
    activeCenterX = blobsPrototype.fieldCenterX();
    activeCenterY = blobsPrototype.fieldCenterY();
  } else if (activeArtworkAlgorithm == ArtworkAlgorithm::cellular) {
    algorithmName = "CELLULAR";
    parameterName = "mutation";
    activeFramesPerSecond = cellularPrototype.measuredFramesPerSecond();
    activeSlowestFrameUs = cellularPrototype.slowestFrameTimeUs();
    activeFrameCount = cellularPrototype.renderedFrameCount();
    activeSeed = cellularPrototype.seed();
    activeCenterX = kScreenWidth * 0.5f;
    activeCenterY = kScreenHeight * 0.5f;
  }

  diagnosticPrintf(
      "heartbeat %8lu ms | heap %lu/min %lu | PSRAM %lu/min %lu | "
      "%s %.1f FPS | frame max %lu us | frames %lu | "
      "mode %s | seed %lu | %s %u%% | "
      "center %.1f,%.1f | "
      "encoder %+ld | invalid %lu | touch %lu | LVGL flush %lu\n",
      static_cast<unsigned long>(nowMs),
      static_cast<unsigned long>(ESP.getFreeHeap()),
      static_cast<unsigned long>(minimumFreeHeapBytes),
      static_cast<unsigned long>(ESP.getFreePsram()),
      static_cast<unsigned long>(minimumFreePsramBytes),
      algorithmName,
      static_cast<double>(activeFramesPerSecond),
      static_cast<unsigned long>(activeSlowestFrameUs),
      static_cast<unsigned long>(activeFrameCount),
      applicationMode == ApplicationMode::artwork ? "ART" : "MENU",
      static_cast<unsigned long>(activeSeed),
      parameterName,
      static_cast<unsigned int>(activeParameterPercent()),
      static_cast<double>(activeCenterX),
      static_cast<double>(activeCenterY),
      static_cast<long>(encoderDetentCount),
      static_cast<unsigned long>(invalidEncoderTransitions),
      static_cast<unsigned long>(touchEventCount),
      static_cast<unsigned long>(displayFlushCount));

  if (activeArtworkAlgorithm == ArtworkAlgorithm::cellular) {
    diagnosticPrintf(
        "CELLULAR STATE    generation %lu | population %u | auto seeds %lu | last %s | period %u | rotation %s %.1fs | breath %.1fs\n",
        static_cast<unsigned long>(cellularPrototype.generation()),
        static_cast<unsigned int>(cellularPrototype.livingCellCount()),
        static_cast<unsigned long>(
            cellularPrototype.automaticReseedCount()),
        cellularPrototype.lastAutomaticReseedReason(),
        static_cast<unsigned int>(cellularPrototype.lastDetectedLoopPeriod()),
        cellularPrototype.rotationDirectionName(),
        static_cast<double>(cellularPrototype.rotationPeriodSeconds()),
        static_cast<double>(cellularPrototype.breathingPeriodSeconds()));
  }
}

}  // namespace

void setup() {
  if (kSerialDiagnosticsEnabled) {
    Serial.begin(kSerialBaud);
    const uint32_t waitStartedMs = millis();
    while (!Serial && (millis() - waitStartedMs < kSerialWaitMs)) {
      delay(10);
    }
  }

  loadAutomatSettings();
  if (kSerialDiagnosticsEnabled) {
    printBaselineReport();
  }
  initializeLedRing();
  initializeDisplay();
  showIntroductionSplashScreen();
  initializeInput();
  initializeTouch();
  initializeFlowPrototype();
  automatLastChangeMs = millis();
  if (kSerialDiagnosticsEnabled) {
    lastHeartbeatMs = millis();
  }
}

void loop() {
  const uint32_t loopStartedUs =
      kSerialDiagnosticsEnabled ? micros() : 0;
  const uint32_t nowMs = millis();

  if (artworkStandbyMode != ArtworkStandbyMode::running) {
    // No renderer, LVGL timer, AUTOMAT transition, or LED animation runs while
    // asleep. The touch controller alone remains in its validated touch mode.
    updateTouch(nowMs);
    discardInputsCapturedDuringStandby();
    if (artworkStandbyMode == ArtworkStandbyMode::sleeping &&
        !latestTouchPressed && digitalRead(kTouchInterruptPin) == HIGH) {
      const esp_err_t sleepResult = esp_light_sleep_start();
      if (sleepResult != ESP_OK) {
        artworkStandbyMode = ArtworkStandbyMode::pollingFallback;
        fallbackFirstTouchStarted = false;
        firstWakeTouchReleased = false;
        diagnosticPrintf("STANDBY           SLEEP FAIL | %s | visual pause\n",
                      esp_err_to_name(sleepResult));
      } else if (esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_GPIO) {
        // The GPIO wake itself counts as touch one, even if its short I2C
        // sample is gone before the main loop runs again.
        latestTouchPressed = true;
        lastTouchSampleMs = millis();
        firstWakeTouchReleased = false;
        artworkStandbyMode = ArtworkStandbyMode::waitingForSecondWakeTouch;
        diagnosticPrintln("STANDBY           WAKE TOUCH 1/2");
      } else if (esp_sleep_get_wakeup_cause() != ESP_SLEEP_WAKEUP_TIMER) {
        artworkStandbyMode = ArtworkStandbyMode::pollingFallback;
        fallbackFirstTouchStarted = false;
        firstWakeTouchReleased = false;
        diagnosticPrintln("STANDBY           WAKE UNKNOWN | visual pause");
      }
    }
    delay(kStandbyTouchPollingDelayMs);
    return;
  }

  updateEncoder();
  updateButton(nowMs);
  updateTouch(nowMs);
  if (artworkStandbyMode != ArtworkStandbyMode::running) {
    delay(kStandbyTouchPollingDelayMs);
    return;
  }
  if (applicationMode == ApplicationMode::artwork) {
    updateAutomaticMode(nowMs);
  }
  updateLedFeedback(nowMs);
  if (applicationMode == ApplicationMode::artwork) {
    if (activeArtworkAlgorithm == ArtworkAlgorithm::flow) {
      flowPrototype.update(nowMs);
    } else if (activeArtworkAlgorithm == ArtworkAlgorithm::radial) {
      radialPrototype.update(nowMs);
    } else if (activeArtworkAlgorithm == ArtworkAlgorithm::attractor) {
      attractorPrototype.update(nowMs);
    } else if (activeArtworkAlgorithm == ArtworkAlgorithm::particles) {
      particlesPrototype.update(nowMs);
    } else if (activeArtworkAlgorithm == ArtworkAlgorithm::orbit) {
      orbitPrototype.update(nowMs);
    } else if (activeArtworkAlgorithm == ArtworkAlgorithm::blobs) {
      blobsPrototype.update(nowMs);
    } else {
      cellularPrototype.update(nowMs);
      if (cellularPrototype.automaticReseedCount() !=
          reportedCellularAutomaticReseedCount) {
        reportedCellularAutomaticReseedCount =
            cellularPrototype.automaticReseedCount();
        activeCellularSeed = cellularPrototype.seed();
        diagnosticPrintf(
            "CELLULAR          AUTO SEED | %lu | %s | period %u | mutation %u%%\n",
            static_cast<unsigned long>(activeCellularSeed),
            cellularPrototype.lastAutomaticReseedReason(),
            static_cast<unsigned int>(
                cellularPrototype.lastDetectedLoopPeriod()),
            static_cast<unsigned int>(cellularMutationPercent));
      }
    }
  } else if (lvglInitialized) {
    // LVGL owns the display only while the menu is visible. The active artwork
    // remains paused with its PSRAM canvas intact and resumes after one restore.
    lv_timer_handler();
  }

  // The deliberate 5 ms delay below is excluded: this metric measures actual
  // input, LVGL rendering, and display-transfer work performed by one loop.
  if (kSerialDiagnosticsEnabled) {
    updateSoakMetrics(micros() - loopStartedUs);
    if (nowMs - lastHeartbeatMs >= kHeartbeatPeriodMs) {
      lastHeartbeatMs = nowMs;
      reportArtworkHeartbeat(nowMs);
    }
  }

  delay(kLvglLoopDelayMs);
}
