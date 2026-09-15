#ifndef RFLINK_BLE_DIAGNOSTICS_H
#define RFLINK_BLE_DIAGNOSTICS_H

// Optional USB-only capture. No logging/allocation/waiting in BLE callbacks.
#ifdef RFLINK_BLE_DEBUG
#include <Arduino.h>
#include <atomic>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

namespace RFLink { namespace BLE {
class Diagnostics {
public:
  struct Snapshot {
    uint32_t generation, dropped, notificationFailures;
    uint16_t handle, queued;
    uint8_t reason, security, keySize;
  };
  struct Event {
    uint32_t time, generation;
    uint16_t handle, bytes;
    uint8_t security, keySize;
    const char *name, *result;
    int detail;
  };
  std::atomic<uint32_t> writes{0}, accepted{0}, rejected{0}, enqueued{0}, consumed{0};
  std::atomic<uint32_t> parserBytes{0}, parserDropped{0};
  std::atomic<bool> discarding{false};
  std::atomic<uint32_t> resets{0}, resetBytes{0}, commands{0}, lastConsumer{0}, lastWrite{0};

  bool start(Snapshot (*snapshot)()) {
    getSnapshot = snapshot;
    return xTaskCreate(taskEntry, "ble-usb-debug", 3072, this, 1, nullptr) == pdPASS;
  }

  void record(const Event &event) {
    portENTER_CRITICAL(&mux);
    if (count == capacity) {
      ++lostEvents;
    } else {
      events[(head + count) % capacity] = event;
      ++count;
    }
    portEXIT_CRITICAL(&mux);
  }

private:
  static constexpr size_t capacity = 48;
  Event events[capacity];
  size_t head = 0, count = 0;
  uint32_t lostEvents = 0;
  portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;
  Snapshot (*getSnapshot)() = nullptr;

  static void taskEntry(void *context) { static_cast<Diagnostics *>(context)->run(); }
  void run() {
    // At most 128 USB bytes per 100 ms, including periodic summaries.
    char line[512];
    size_t length = 0, offset = 0;
    uint32_t nextSummary = 0;
    for (;;) {
      const uint32_t now = millis();
      if (offset == length) {
        offset = 0;
        if (static_cast<int32_t>(now - nextSummary) >= 0) {
          const Snapshot s = getSnapshot();
          uint32_t lost;
          portENTER_CRITICAL(&mux);
          lost = lostEvents;
          portEXIT_CRITICAL(&mux);
          length = snprintf(line, sizeof(line),
              "[BLEDBG] t=%lu h=%u g=%lu summary reason=%u enc=%u auth=%u bond=%u key=%u "
              "write=%lu ok=%lu reject=%lu enq=%lu drop=%lu used=%lu q=%u reset=%lu reset_bytes=%lu "
              "cmd=%lu partial=%lu parser_drop=%lu discard_line=%u last_rx=%lu last_cons=%lu age=%lu nf=%lu lost=%lu\r\n",
              (unsigned long)now, s.handle, (unsigned long)s.generation, s.reason,
              !!(s.security & 1), !!(s.security & 2), !!(s.security & 4), s.keySize,
              (unsigned long)writes.load(), (unsigned long)accepted.load(), (unsigned long)rejected.load(),
              (unsigned long)enqueued.load(), (unsigned long)s.dropped, (unsigned long)consumed.load(), s.queued,
              (unsigned long)resets.load(), (unsigned long)resetBytes.load(), (unsigned long)commands.load(),
              (unsigned long)parserBytes.load(), (unsigned long)parserDropped.load(), discarding.load(),
              (unsigned long)lastWrite.load(), (unsigned long)lastConsumer.load(),
              (unsigned long)(now - lastConsumer.load()), (unsigned long)s.notificationFailures, (unsigned long)lost);
          nextSummary = now + 2000;
        } else {
          Event event{};
          bool available;
          portENTER_CRITICAL(&mux);
          available = count != 0;
          if (available) {
            event = events[head];
            head = (head + 1) % capacity;
            --count;
          }
          portEXIT_CRITICAL(&mux);
          length = available ? snprintf(line, sizeof(line),
              "[BLEDBG] t=%lu h=%u g=%lu %s n=%u result=%s detail=%d enc=%u auth=%u bond=%u key=%u\r\n",
              (unsigned long)event.time, event.handle, (unsigned long)event.generation,
              event.name, event.bytes, event.result, event.detail,
              !!(event.security & 1), !!(event.security & 2), !!(event.security & 4), event.keySize) : 0;
        }
        if (length >= sizeof(line)) length = sizeof(line) - 1;
      }
      if (offset < length) {
        const int available = Serial.availableForWrite();
        size_t chunk = length - offset;
        if (chunk > 128) chunk = 128;
        if (available > 0) {
          if (chunk > static_cast<size_t>(available)) chunk = available;
          offset += Serial.write(reinterpret_cast<const uint8_t *>(line) + offset, chunk);
        }
      }
      vTaskDelay(pdMS_TO_TICKS(100));
    }
  }
};
}} // namespace RFLink::BLE
#endif // RFLINK_BLE_DEBUG
#endif
