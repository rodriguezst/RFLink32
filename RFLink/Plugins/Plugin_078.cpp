// CLEMSA Mastercode MV12 36-bit transmitter (ESP32 / Arduino 2.x).
// Command format and configurable defaults: Plugin_078.h.
// Distributed under the RFLink Gateway license; retain License.txt.

#define CLEMSAMV12_PLUGIN_ID 078
#define PLUGIN_DESC_078 "CLEMSA Mastercode MV12 remote"

#ifdef PLUGIN_TX_078
#include "../4_Display.h"
#include "../1_Radio.h"
#include "../7_Utils.h"

#define PLUGIN_078_ID "CLEMSA-MV12"

#include "Plugin_078.h"
#include "../1_Radio.h"
#include "../RFLink.h"

#include <driver/rmt.h>
#include <esp_heap_caps.h>
#include <esp32-hal-matrix.h>
#include <Arduino.h>

namespace RFLink { namespace ClemsaMV12 {

// RFLink32 has no other RMT consumer. Reserve this channel for MV12.
#ifndef RFLINK_MV12_RMT_CHANNEL
#define RFLINK_MV12_RMT_CHANNEL 0
#endif

static constexpr rmt_channel_t channel =
    static_cast<rmt_channel_t>(RFLINK_MV12_RMT_CHANNEL);

static bool cleanupFailed = false;

static bool reportError(const char *message) {
    RFLink::sendRawPrint("CLEMSA-MV12;ERROR=");
    RFLink::sendRawPrint(message);
    RFLink::sendRawPrint(";", true);

    return false;
}

static bool transmit(const Request &request) {
    if (cleanupFailed) return reportError("RMT_CLEANUP_FAILED_REBOOT_REQUIRED");
    const int pin = Radio::pins::TX_DATA;
    if (!GPIO_IS_VALID_OUTPUT_GPIO(pin)) return reportError("INVALID_TX_PIN");
    if (!Radio::hardwareProperlyInitialized) return reportError("RADIO_NOT_READY");

    const size_t count = pairCount(request);
    // rmt_write_items does not copy the buffer. Keep internal RAM alive until
    // completion (or until the driver has been stopped on every error path).
    auto *items = static_cast<rmt_item32_t *>(heap_caps_malloc(
        count * sizeof(rmt_item32_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    if (!items) return reportError("OUT_OF_MEMORY");

    size_t index = 0;
    const bool encoded = encode(request, [&](uint16_t high, uint16_t low) {
        if (index >= count || high > 32767 || low > 32767) return false;
        items[index].val = 0;
        items[index].level0 = 1;
        items[index].duration0 = high;
        items[index].level1 = 0;
        items[index].duration1 = low;
        ++index;
        return true;
    });
    if (!encoded || index != count) {
        heap_caps_free(items);
        return reportError("ENCODING_FAILED");
    }

    Radio::set_Radio_mode(Radio::Radio_OFF);
    // The shared SX127x DIO2 must be LOW before starting direct OOK TX.
    digitalWrite(pin, LOW);
    if (Radio::hardwareProperlyInitialized)
        Radio::set_Radio_mode(Radio::Radio_TX);

    esp_err_t result = Radio::hardwareProperlyInitialized ? ESP_OK : ESP_FAIL;
    bool installed = false;
    rmt_config_t config = RMT_DEFAULT_CONFIG_TX(static_cast<gpio_num_t>(pin), channel);
    config.clk_div = 80; // APB 80 MHz -> one microsecond per tick.
    config.mem_block_num = 1;
    config.tx_config.carrier_en = false; // Chopping is explicitly encoded.
    config.tx_config.loop_en = false;
    config.tx_config.idle_output_en = true;
    config.tx_config.idle_level = RMT_IDLE_LEVEL_LOW;
    // Configure after Radio_TX, since pinMode(OUTPUT) changes the GPIO mux.
    if (result == ESP_OK) result = rmt_config(&config);
    if (result == ESP_OK) {
        result = rmt_driver_install(channel, 0, ESP_INTR_FLAG_IRAM);
        installed = result == ESP_OK;
    }
    if (result == ESP_OK) result = rmt_set_source_clk(channel, RMT_BASECLK_APB);
    uint32_t clockHz = 0;
    if (result == ESP_OK) result = rmt_get_counter_clock(channel, &clockHz);
    if (result == ESP_OK && clockHz != 1000000) result = ESP_FAIL;
    if (result == ESP_OK) result = rmt_write_items(channel, items, count, false);
    if (result == ESP_OK) {
        const uint32_t timeoutMs = (durationUs(request) + 999) / 1000 + 200;
        result = rmt_wait_tx_done(channel, pdMS_TO_TICKS(timeoutMs) + 1);
    }
    if (installed) {
        const esp_err_t stop = rmt_tx_stop(channel);
        const esp_err_t uninstall = rmt_driver_uninstall(channel);
        cleanupFailed = uninstall != ESP_OK;
        if (result == ESP_OK) result = stop != ESP_OK ? stop : uninstall;
    }
    // Remove RMT's connection before freeing the buffer and returning the
    // shared DIO2 to input. Standby prevents driving against the radio's RX.
    Radio::set_Radio_mode(Radio::Radio_OFF);
    pinMatrixOutDetach(pin, false, false);
    digitalWrite(pin, LOW);
    pinMode(pin, INPUT);
    // If uninstall unexpectedly fails, the ISR might still reference items.
    // Retain that buffer and refuse further sends until reboot, avoiding UAF.
    if (!cleanupFailed) heap_caps_free(items);
    Radio::set_Radio_mode(Radio::Radio_RX);
    if (result != ESP_OK || !Radio::hardwareProperlyInitialized)
        return reportError("TX_OR_RX_RESTORE_FAILED");

    const uint64_t data = payload(request);
    char message[160];
    snprintf(message, sizeof(message),
             "CLEMSA-MV12;BITS=%u;PAYLOAD=%01X%08lX;"
             "CHIP=%u;SLOT=%u;GUARD=%u;FRAMES=%u;DURATION_US=%lu;",
             unsigned(BIT_COUNT), unsigned((data >> 32) & 0xFU),
             static_cast<unsigned long>(uint32_t(data)),
             unsigned(request.chipUs), unsigned(request.slotUs), unsigned(request.guardUs),
             unsigned(request.frames), static_cast<unsigned long>(durationUs(request)));
    RFLink::sendRawPrint(message, true);
    return true;
}
}} // namespace RFLink::ClemsaMV12

boolean PluginTX_078(byte, const char *command) {
    using namespace RFLink::ClemsaMV12;

    if (!matches(command)) return false;
    Request request;
    if (const char *error = parse(command, request)) return reportError(error);

    // DURATION repeats complete transmission blocks until at least the
    // requested wall-clock duration has elapsed. Never truncate a block.
    if (request.durationMs != 0) {
        const uint32_t startedAt = millis();

        do {
            if (!transmit(request)) return false;
        } while (static_cast<uint32_t>(millis() - startedAt) < request.durationMs);

        return true;
    }

    // REPEAT is the number of complete transmission blocks.
    // Each block contains request.frames MV12 frames.
    for (uint8_t i = 0; i < request.repeat; ++i) {
        if (!transmit(request)) return false;
    }

    return true;
}

#endif // PLUGIN_TX_078
