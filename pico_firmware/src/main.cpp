// ============================================================================
// AetherAim Pico HID Proxy Firmware
//
// Hardware: Raspberry Pi Pico (RP2040) or Pico 2 (RP2350)
//
// Architecture:
//   Physical Mouse ──USB──► [Pico USB Host] ──► OneEuroFilter ──► [Pico USB Device] ──USB──► PC
//
// The PC sees a standard USB HID mouse. The filter runs transparently
// on the Pico. No software runs on the PC — undetectable by anti-cheat.
//
// Pin connections:
//   GPIO 0  — USB Host D+  (to mouse, via 1.5kΩ pull-up to 3.3V)
//   GPIO 1  — USB Host D-  (to mouse)
//   3.3V    — USB Host VBUS (direct connection or via P-channel MOSFET)
//   GND     — Common ground
//
//   The Pico's built-in micro-USB port connects to the PC (USB Device).
//   The mouse connects to GPIO 0/1 via a USB breakout or second micro-USB.
//
// Build:
//   mkdir build && cd build
//   cmake .. -DPICO_SDK_PATH=/path/to/pico-sdk
//   make -j4
//
//   The Pico SDK and Pico-PIO-USB library must be installed.
//   See CMakeLists.txt for dependency configuration.
// ============================================================================

#include <cstdio>
#include <cstring>
#include <cmath>
#include <algorithm>

// Pico SDK
#include "pico/stdlib.h"
#include "pico/multicore.h"
#include "pico/time.h"
#include "hardware/clocks.h"
#include "hardware/irq.h"

// TinyUSB (Device side — Pico → PC)
#include "tusb.h"
#include "device/usbd.h"

// Pico-PIO-USB (Host side — Mouse → Pico)
#include "pio_usb.h"

// Our filter
#include "one_euro_filter.hpp"

// Serial console (runtime parameter tuning)
#include "serial_console.hpp"

// ============================================================================
// Configuration
// ============================================================================

// ── Filter parameters (tune these for the user's tremor profile) ───────
static float FILTER_MIN_CUTOFF  = 1.2f;   // Hz — lower = more smoothing
static float FILTER_BETA        = 10.0f;  // Speed adaptation
static float FILTER_D_CUTOFF    = 1.0f;   // Hz — derivative smoothness
static float FILTER_SPEED_COEFF = 1.0f;   // Speed sensitivity

// ── Deadzone ──────────────────────────────────────────────────────────
static int   DEADZONE_PX = 0;     // Pixels per axis
static int   MAX_DELTA_PX = 100;  // Clamp per event

// ── USB Host pin configuration ────────────────────────────────────────
// These match the Pico-PIO-USB default example wiring.
static constexpr uint PIN_USB_HOST_DP = 0;   // D+ (green wire)
static constexpr uint PIN_USB_HOST_DM = 1;   // D- (white wire)
// VBUS is powered directly from Pico 3.3V (pin 36)
// No VBUS detection pin needed

// ============================================================================
// Globals
// ============================================================================

static aether::OneEuroFilter2D g_filter(
    FILTER_MIN_CUTOFF, FILTER_BETA, FILTER_D_CUTOFF, FILTER_SPEED_COEFF);

// Sub-pixel accumulators
static float g_fractX = 0.0f;
static float g_fractY = 0.0f;

// Timing
static uint64_t g_lastTimeUs = 0;
static bool     g_firstEvent = true;

// Mouse connection state
static bool g_mouseConnected = false;

// ============================================================================
// HID Report Descriptor (standard mouse, sent to PC)
// ============================================================================

static const uint8_t DESC_HID_MOUSE[] = {
    // This tells the PC we're a standard 3-button mouse with scroll wheel
    0x05, 0x01,        // USAGE_PAGE (Generic Desktop)
    0x09, 0x02,        // USAGE (Mouse)
    0xA1, 0x01,        // COLLECTION (Application)
    0x09, 0x01,        //   USAGE (Pointer)
    0xA1, 0x00,        //   COLLECTION (Physical)
    0x05, 0x09,        //     USAGE_PAGE (Button)
    0x19, 0x01,        //     USAGE_MINIMUM (Button 1)
    0x29, 0x05,        //     USAGE_MAXIMUM (Button 5)
    0x15, 0x00,        //     LOGICAL_MINIMUM (0)
    0x25, 0x01,        //     LOGICAL_MAXIMUM (1)
    0x95, 0x05,        //     REPORT_COUNT (5)
    0x75, 0x01,        //     REPORT_SIZE (1)
    0x81, 0x02,        //     INPUT (Data,Var,Abs)
    0x95, 0x01,        //     REPORT_COUNT (1)
    0x75, 0x03,        //     REPORT_SIZE (3)
    0x81, 0x01,        //     INPUT (Const)  — padding
    0x05, 0x01,        //     USAGE_PAGE (Generic Desktop)
    0x09, 0x30,        //     USAGE (X)
    0x09, 0x31,        //     USAGE (Y)
    0x09, 0x38,        //     USAGE (Wheel)
    0x15, 0x81,        //     LOGICAL_MINIMUM (-127)
    0x25, 0x7F,        //     LOGICAL_MAXIMUM (127)
    0x75, 0x08,        //     REPORT_SIZE (8)
    0x95, 0x03,        //     REPORT_COUNT (3)
    0x81, 0x06,        //     INPUT (Data,Var,Rel)
    0xC0,              //   END_COLLECTION
    0xC0               // END_COLLECTION
};

// HID report structure (matches the descriptor above)
struct MouseReport {
    uint8_t buttons;   // Bitmask: bit0=left, bit1=right, bit2=middle
    int8_t  dx;        // X delta (-127 to 127)
    int8_t  dy;        // Y delta (-127 to 127)
    int8_t  wheel;    // Scroll wheel
};

static_assert(sizeof(MouseReport) == 4, "MouseReport must be 4 bytes");

// ============================================================================
// Ring buffer for filtered reports (ISR → main thread)
// ============================================================================

static constexpr int REPORT_QUEUE_SIZE = 16;

static MouseReport g_reportQueue[REPORT_QUEUE_SIZE];
static volatile int g_reportWriteIdx = 0;
static volatile int g_reportReadIdx  = 0;

static inline bool queueEmpty() {
    return g_reportReadIdx == g_reportWriteIdx;
}

static inline bool queueFull() {
    return ((g_reportWriteIdx + 1) % REPORT_QUEUE_SIZE) == g_reportReadIdx;
}

static inline bool queuePush(const MouseReport& r) {
    if (queueFull()) return false;
    g_reportQueue[g_reportWriteIdx] = r;
    g_reportWriteIdx = (g_reportWriteIdx + 1) % REPORT_QUEUE_SIZE;
    return true;
}

static inline bool queuePop(MouseReport& r) {
    if (queueEmpty()) return false;
    r = g_reportQueue[g_reportReadIdx];
    g_reportReadIdx = (g_reportReadIdx + 1) % REPORT_QUEUE_SIZE;
    return true;
}

// ============================================================================
// USB Device (Pico → PC) — TinyUSB HID callbacks
// ============================================================================

// Called when PC requests the HID report descriptor
uint8_t const* tud_hid_descriptor_report_cb(uint8_t instance) {
    (void)instance;
    return DESC_HID_MOUSE;
}

uint16_t tud_hid_descriptor_report_cb_len(uint8_t instance) {
    (void)instance;
    return sizeof(DESC_HID_MOUSE);
}

// ============================================================================
// USB Host (Mouse → Pico) — Pico-PIO-USB HID callback
// ============================================================================

// Called by Pico-PIO-USB when the mouse connects or disconnects
void tuh_hid_mount_cb(uint8_t devAddr, uint8_t instance,
                       uint8_t const* descReport, uint16_t descLen) {
    (void)descReport; (void)descLen;
    printf("Mouse connected: devAddr=%d, instance=%d\n", devAddr, instance);

    // Request reports from this device
    tuh_hid_receive_report(devAddr, instance);

    g_mouseConnected = true;
    g_filter.reset();
    g_firstEvent = true;
}

void tuh_hid_umount_cb(uint8_t devAddr, uint8_t instance) {
    (void)devAddr; (void)instance;
    printf("Mouse disconnected\n");
    g_mouseConnected = false;
}

// ── THIS IS THE CORE: raw mouse data arrives here ────────────────────
// Called by Pico-PIO-USB when a HID report is received from the mouse.
// We filter the delta and queue it for transmission to the PC.
void tuh_hid_report_received_cb(uint8_t devAddr, uint8_t instance,
                                 uint8_t const* report, uint16_t len) {
    (void)devAddr; (void)instance;

    // Standard mouse report: 4+ bytes
    // Byte 0: buttons
    // Byte 1: X delta (signed)
    // Byte 2: Y delta (signed)
    // Byte 3: wheel
    if (len < 3) return;

    const MouseReport* rawReport = reinterpret_cast<const MouseReport*>(report);

    int16_t rawDx = static_cast<int16_t>(rawReport->dx);
    int16_t rawDy = static_cast<int16_t>(rawReport->dy);

    // ── Timing ─────────────────────────────────────────────────────────
    uint64_t nowUs = time_us_64();
    float dt;
    if (g_firstEvent) {
        dt = 0.001f;  // Assume 1ms for first event
        g_firstEvent = false;
    } else {
        uint64_t deltaUs = nowUs - g_lastTimeUs;
        // Clamp: 100μs min, 500ms max (sleep resume)
        if (deltaUs < 100) deltaUs = 100;
        if (deltaUs > 500000) deltaUs = 500000;
        dt = static_cast<float>(deltaUs) * 1e-6f;
    }
    g_lastTimeUs = nowUs;

    // ── Deadzone ──────────────────────────────────────────────────────
    if (DEADZONE_PX > 0 &&
        std::abs(rawDx) < DEADZONE_PX && std::abs(rawDy) < DEADZONE_PX) {
        // Suppress — request next report, don't queue anything
        tuh_hid_receive_report(devAddr, instance);
        return;
    }

    // ── MaxDelta clamp ────────────────────────────────────────────────
    float fdx = static_cast<float>(rawDx);
    float fdy = static_cast<float>(rawDy);
    fdx = std::clamp(fdx, -static_cast<float>(MAX_DELTA_PX),
                     static_cast<float>(MAX_DELTA_PX));
    fdy = std::clamp(fdy, -static_cast<float>(MAX_DELTA_PX),
                     static_cast<float>(MAX_DELTA_PX));

    // ── OneEuro Filter ────────────────────────────────────────────────
    auto filtered = g_filter.filter(fdx, fdy, dt);

    // ── Sub-pixel accumulation ────────────────────────────────────────
    float totalX = filtered.x + g_fractX;
    float totalY = filtered.y + g_fractY;

    int8_t intX = static_cast<int8_t>(
        std::clamp(static_cast<int>(totalX), -127, 127));
    int8_t intY = static_cast<int8_t>(
        std::clamp(static_cast<int>(totalY), -127, 127));

    g_fractX = totalX - static_cast<float>(intX);
    g_fractY = totalY - static_cast<float>(intY);

    // ── Queue for transmission to PC ──────────────────────────────────
    MouseReport filtReport;
    filtReport.buttons = rawReport->buttons;
    filtReport.dx      = intX;
    filtReport.dy      = intY;
    filtReport.wheel   = (len >= 4) ? rawReport->wheel : 0;

    queuePush(filtReport);

    // Request the next report from the mouse
    tuh_hid_receive_report(devAddr, instance);
}

// ============================================================================
// Core 1: USB Host task (Pico-PIO-USB)
// ============================================================================

static void core1_host_task() {
    printf("Core 1: USB Host task started\n");

    // Initialize USB Host on PIO
    pio_usb_configuration_t pioCfg = PIO_USB_DEFAULT_CONFIG;
    pioCfg.pin_dp = PIN_USB_HOST_DP;
    // pin_dm is determined automatically (pin_dp + 1)

    // Initialize the host stack with one port
    tuh_configure(1, TUH_CFGID_RPI_PIO_USB_CONFIGURATION, &pioCfg);
    tuh_init(BOARD_TUH_RHPORT);

    // Main host loop
    while (true) {
        tuh_task();  // Process USB host events
        // tuh_task calls our callbacks (mount, umount, report_received)
        // which populate g_reportQueue

        // Brief sleep to avoid busy-waiting
        // (tuh_task returns quickly when idle)
        sleep_us(100);  // 0.1ms → up to 10kHz USB event processing
    }
}

// ============================================================================
// Core 0: USB Device task (TinyUSB) + main control
// ============================================================================

int main() {
    // ── Init ───────────────────────────────────────────────────────────
    stdio_init_all();

    printf("\n\n");
    printf("==========================================\n");
    printf("  AetherAim Pico HID Proxy v1.0\n");
    printf("  Filter: OneEuro (fc=%.1f, beta=%.1f)\n",
           FILTER_MIN_CUTOFF, FILTER_BETA);
    printf("==========================================\n\n");

    // Init TinyUSB device stack
    tud_init(BOARD_TUD_RHPORT);

    // Launch USB Host on Core 1
    multicore_launch_core1(core1_host_task);

    // ── LED heartbeat ──────────────────────────────────────────────────
    // Blink the built-in LED to show the device is alive
    constexpr uint LED_PIN = 25;  // Pico built-in LED
    gpio_init(LED_PIN);
    gpio_set_dir(LED_PIN, GPIO_OUT);

    uint64_t lastLedToggle = 0;
    bool ledState = false;

    // ── Main loop (Core 0) ─────────────────────────────────────────────
    printf("Core 0: USB Device + queue drain started\n");

    while (true) {
        // Process USB device events (TinyUSB)
        tud_task();

        // Drain filtered report queue → send to PC
        if (tud_hid_ready()) {
            MouseReport report;
            if (queuePop(report)) {
                tud_hid_report(0, &report, sizeof(report));
            }
        }

        // LED heartbeat
        uint64_t now = time_us_64();
        if (now - lastLedToggle > 250000) {  // 250ms
            lastLedToggle = now;
            ledState = !ledState;
            gpio_put(LED_PIN, ledState);

            // Blink faster when mouse is connected
            if (g_mouseConnected) {
                // Quick double-blink pattern
                gpio_put(LED_PIN, 1);
                sleep_us(50000);
                gpio_put(LED_PIN, 0);
                sleep_us(50000);
                gpio_put(LED_PIN, 1);
                sleep_us(50000);
                gpio_put(LED_PIN, 0);
            }
        }

        // Serial console — runtime parameter tuning
        aether::console::pollSerial(
            g_filter,
            FILTER_MIN_CUTOFF, FILTER_BETA, FILTER_D_CUTOFF, FILTER_SPEED_COEFF,
            DEADZONE_PX, MAX_DELTA_PX);

        // TinyUSB device task needs regular polling
        if (!queueEmpty()) {
            // Don't sleep — drain quickly
        } else {
            sleep_us(100);
        }
    }

    return 0;
}
