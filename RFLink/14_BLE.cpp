#include "14_BLE.h"

#if defined(RFLINK_BLE_ENABLED) && defined(ESP32)

#include "3_Serial.h"
#include "RFLink.h"
#include "BLETransportState.h"
#include "BLEDiagnostics.h"

#include <NimBLEDevice.h>
#include <atomic>
#include <esp_system.h>
#include <freertos/FreeRTOS.h>
#ifdef USING_NIMBLE_ARDUINO_HEADERS
#include "nimble/porting/nimble/include/nimble/nimble_port.h"
#else
#include "nimble/nimble_port.h"
#endif

namespace RFLink {
  namespace BLE {

    namespace {
      constexpr char serviceUuid[] = "6E400001-B5A3-F393-E0A9-E50E24DCCA9E";
      constexpr char rxCharacteristicUuid[] = "6E400002-B5A3-F393-E0A9-E50E24DCCA9E";
      constexpr char txCharacteristicUuid[] = "6E400003-B5A3-F393-E0A9-E50E24DCCA9E";
      constexpr char statusServiceUuid[] = "A8F10001-8D5B-4A6D-9F32-70E4B2C6D901";
      constexpr char statusCharacteristicUuid[] = "A8F10002-8D5B-4A6D-9F32-70E4B2C6D901";
      constexpr size_t defaultNotificationSize = 20;
      constexpr size_t notificationOverhead = 3;
      constexpr size_t maxNotificationSize = 512;
      constexpr TickType_t notificationChunkDelay = pdMS_TO_TICKS(10);
      constexpr size_t rxQueueSize = INPUT_COMMAND_SIZE + 64;
      constexpr int maxBonds = CONFIG_BT_NIMBLE_MAX_BONDS;

      static_assert(maxBonds > 0, "BLE UART requires persistent bonds");
      static_assert(MYNEWT_VAL(BLE_STORE_MAX_BONDS) == maxBonds,
                    "NimBLE bond capacity must match CONFIG_BT_NIMBLE_MAX_BONDS");
      static_assert(MYNEWT_VAL(BLE_STORE_CONFIG_PERSIST), "BLE bonds must persist in NVS");
      static_assert(CONFIG_BT_NIMBLE_MAX_CONNECTIONS == 1, "BLE UART supports one active client");
      static_assert(MYNEWT_VAL(BLE_STORE_MAX_CCCDS) >= 3 * maxBonds, "Reserve UART, status and Service Changed subscriptions for all bonds");

      static_assert(INPUT_COMMAND_SIZE > 1 && INPUT_COMMAND_SIZE <= 65536, "Status command limit is uint16");
      NimBLECharacteristic *txCharacteristic = nullptr;
      NimBLECharacteristic *statusCharacteristic = nullptr;
      using State = TransportState<rxQueueSize>;
      State transport;
      // Only bounded memory operations under this lock; never Serial, BLE or the CLI.
      portMUX_TYPE stateMux = portMUX_INITIALIZER_UNLOCKED;
      struct StateLock {
        StateLock() { portENTER_CRITICAL(&stateMux); }
        ~StateLock() { portEXIT_CRITICAL(&stateMux); }
      };
      uint64_t bootId = 0;
      ble_npl_event statusEvent;
      std::atomic<bool> statusEventPending{false};
      uint32_t nextStatusAttempt = 0; // Protected by stateMux.
      bool running = false;
      std::atomic<bool> restartAdvertisingRequested{false};
      std::atomic<uint32_t> pendingPasskey{UINT32_MAX};
      std::atomic<unsigned long> droppedRxBytes{0};
      std::atomic<unsigned long> droppedTxBytes{0};
      std::atomic<unsigned long> failedNotifications{0};
      std::atomic<unsigned long> failedStatusNotifications{0};
      uint32_t parserGeneration = 0; // Command consumer only.

#ifdef RFLINK_BLE_DEBUG
      Diagnostics diagnostics;
      uint8_t diagnosticSecurity = 0, diagnosticKeySize = 0; // stateMux

      // One fixed report at boot, after registration. Look up the live stack
      // database rather than assuming that a C++ characteristic was registered.
      void reportGattRegistration(NimBLEService *service, NimBLECharacteristic *characteristic) {
        uint16_t serviceHandle = 0, declarationHandle = 0, valueHandle = 0;
        const auto serviceId = service->getUUID();
        const auto characteristicId = characteristic->getUUID();
        const int serviceResult = ble_gatts_find_svc(serviceId.getBase(), &serviceHandle);
        const int characteristicResult = ble_gatts_find_chr(serviceId.getBase(), characteristicId.getBase(),
                                                            &declarationHandle, &valueHandle);
        Serial.printf("[BLEDBG] t=%lu gatt svc=%s svc_rc=%d svc_h=%u chr=%s chr_rc=%d decl_h=%u value_h=%u props=0x%04x\r\n",
                      static_cast<unsigned long>(millis()), serviceId.toString().c_str(), serviceResult, serviceHandle,
                      characteristicId.toString().c_str(), characteristicResult, declarationHandle, valueHandle,
                      characteristic->getProperties());
      }

      uint8_t securityFlags(NimBLEConnInfo &info) {
        return info.isEncrypted() | (info.isAuthenticated() << 1) | (info.isBonded() << 2);
      }
      // Caller holds stateMux; capture only bounded records, never format/print.
      void trace(const char *name, const char *result, NimBLEConnInfo *info = nullptr,
                 size_t bytes = 0, int detail = 0) {
        const uint16_t handle = info ? info->getConnHandle() : transport.handle;
        const uint8_t security = info ? securityFlags(*info) : diagnosticSecurity;
        const uint8_t key = info ? info->getSecKeySize() : diagnosticKeySize;
        if (info && transport.matches(handle)) {
          diagnosticSecurity = security;
          diagnosticKeySize = key;
        }
        diagnostics.record({millis(), transport.generation, handle, static_cast<uint16_t>(bytes),
                            security, key, name, result, detail});
      }
      const char *writeResultName(State::WriteResult result, NimBLEConnInfo &info) {
        switch (result) {
          case State::WriteResult::Accepted: return "accepted";
          case State::WriteResult::WrongConnection: return "wrong_connection";
          case State::WriteResult::Security:
            if (!info.isEncrypted()) return "not_encrypted";
            if (!info.isAuthenticated()) return "not_authenticated";
            if (!info.isBonded()) return "not_bonded";
            if (info.getSecKeySize() != 16) return "key_size";
            return "auth_callback_pending";
          case State::WriteResult::Consumer: return "consumer_pending";
          case State::WriteResult::Cleanup: return "cleanup_pending";
          case State::WriteResult::ResponseSubscription: return "tx_unsubscribed";
          case State::WriteResult::Overflow: return "queue_overflow";
        }
        return "unknown";
      }
      Diagnostics::Snapshot diagnosticSnapshot() {
        StateLock lock;
        return {transport.generation, static_cast<uint32_t>(droppedRxBytes.load()),
                static_cast<uint32_t>(failedStatusNotifications.load()), transport.handle,
                static_cast<uint16_t>(transport.count), static_cast<uint8_t>(transport.reason()),
                diagnosticSecurity, diagnosticKeySize};
      }
#define BLE_TRACE(...) trace(__VA_ARGS__)
#else
#define BLE_TRACE(...) do {} while (0)
#endif

      char commandBuffer[INPUT_COMMAND_SIZE];
      size_t commandLength = 0;
      bool discardUntilNewline = false;

      void resetCommandBuffer() {
#ifdef RFLINK_BLE_DEBUG
        diagnostics.parserBytes = 0;
        diagnostics.discarding = false;
#endif
        commandBuffer[0] = 0;
        commandLength = 0;
        discardUntilNewline = false;
      }

      void paramsUpdatedCallback() {
        Serial.println(F("BLE settings saved; reboot required to apply"));
      }

      bool meetsSecurity(NimBLEConnInfo &info) {
        return info.isEncrypted() && info.isAuthenticated() && info.isBonded() && info.getSecKeySize() == 16;
      }

      // Runs on the NimBLE host, serialized with connection/security callbacks.
      // Explicit payloads avoid sharing the READ value with the command task.
      void sendStatusEvent(ble_npl_event *) {
        uint8_t value[17];
        uint16_t handle;
        bool send, overflow;
        {
          StateLock lock;
          handle = transport.handle;
          overflow = transport.overflow && handle != State::noHandle;
          send = transport.dirty && transport.secure && transport.statusSubscribed && handle != State::noHandle;
          transport.encode(bootId, INPUT_COMMAND_SIZE - 1, value);
          if (send) {
            BLE_TRACE("status_notify", "attempt", nullptr, sizeof(value), value[14]);
            transport.dirty = false;
            nextStatusAttempt = 0;
          }
        }
        if (send && !statusCharacteristic->notify(value, sizeof(value), handle)) {
          failedStatusNotifications++;
          StateLock lock;
          BLE_TRACE("status_notify", "failed");
          transport.dirty = true;
          nextStatusAttempt = millis() + 250;
        }
        // Lost command fragments cannot be recovered safely in this session.
        if (overflow) NimBLEDevice::getServer()->disconnect(handle);
        statusEventPending = false;
      }

      class ServerCallbacks : public NimBLEServerCallbacks {
        void onConnect(NimBLEServer *server, NimBLEConnInfo &info) override {
          {
            StateLock lock;
            transport.connect(info.getConnHandle());
            BLE_TRACE("connect", "current", &info);
            nextStatusAttempt = 0;
          }
          restartAdvertisingRequested = false;
          if (!NimBLEDevice::startSecurity(info.getConnHandle())) {
            {
              StateLock lock;
              BLE_TRACE("security_start", "failed", &info);
            }
            server->disconnect(info.getConnHandle());
          }
        }

        void onDisconnect(NimBLEServer *, NimBLEConnInfo &info, int reason) override {
          (void) reason;
          {
            StateLock lock;
            BLE_TRACE("disconnect", transport.matches(info.getConnHandle()) ? "current" : "ignored", &info, 0, reason);
            if (!transport.disconnect(info.getConnHandle())) return;
#ifdef RFLINK_BLE_DEBUG
            diagnosticSecurity = diagnosticKeySize = 0;
#endif
          }
          pendingPasskey = UINT32_MAX;
          restartAdvertisingRequested = true;
        }

        uint32_t onPassKeyDisplay() override {
          const uint32_t passkey = esp_random() % 1000000;
          // No peer identity in this callback: never mutate connection state here.
          // USB-only printing is deferred to the consumer; never broadcast the PIN.
          pendingPasskey = passkey;
          return passkey;
        }

        void onAuthenticationComplete(NimBLEConnInfo &info) override {
          const bool secure = meetsSecurity(info);
          {
            StateLock lock;
            BLE_TRACE("authentication", transport.matches(info.getConnHandle()) ? (secure ? "accepted" : "rejected") : "ignored", &info);
            if (!transport.authenticate(info.getConnHandle(), secure)) return;
          }
          if (!secure) NimBLEDevice::getServer()->disconnect(info.getConnHandle());
        }

        void onConfirmPassKey(NimBLEConnInfo &info, uint32_t pin) override {
          (void) pin;
          // Display-only passkey entry, never unattended confirmation.
          NimBLEDevice::injectConfirmPasskey(info, false);
        }
      };

      class RxCallbacks : public NimBLECharacteristicCallbacks {
        void onWrite(NimBLECharacteristic *characteristic, NimBLEConnInfo &info) override {
#ifdef RFLINK_BLE_DEBUG
          diagnostics.writes++;
          diagnostics.lastWrite = millis();
#endif
          const auto &value = characteristic->getValue();
          StateLock lock;
          BLE_TRACE("write_enter", "entered", &info, value.size());
          const auto result = transport.write(info.getConnHandle(), meetsSecurity(info),
                                               reinterpret_cast<const char *>(value.data()), value.size());
          if (result != State::WriteResult::Accepted) droppedRxBytes += value.size();
#ifdef RFLINK_BLE_DEBUG
          if (result == State::WriteResult::Accepted) {
            diagnostics.accepted++;
            diagnostics.enqueued += value.size();
          } else diagnostics.rejected++;
#endif
          BLE_TRACE("write_result", writeResultName(result, info), &info, value.size(), static_cast<int>(result));
        }
      };

      class SubscriptionCallbacks : public NimBLECharacteristicCallbacks {
        void onStatus(NimBLECharacteristic *characteristic, int code) override {
          if (characteristic != statusCharacteristic || code == 0) return;
          failedStatusNotifications++;
          StateLock lock;
          BLE_TRACE("status_completion", "failed_no_peer_id", nullptr, 0, code);
          // This callback has no peer identity. Request a fresh snapshot only;
          // never change readiness or security based on an old completion.
          transport.dirty = true;
          nextStatusAttempt = millis() + 250;
        }
        void onSubscribe(NimBLECharacteristic *characteristic, NimBLEConnInfo &info, uint16_t value) override {
          StateLock lock;
          BLE_TRACE(characteristic == statusCharacteristic ? "status_subscribe" : "tx_subscribe",
                    transport.matches(info.getConnHandle()) ? "current" : "ignored", &info, 0, value);
          transport.subscribe(info.getConnHandle(), characteristic == statusCharacteristic, (value & 1) != 0);
        }
        void onRead(NimBLECharacteristic *characteristic, NimBLEConnInfo &info) override {
          if (characteristic != statusCharacteristic) return;
          uint8_t value[17];
          bool allowed;
          {
            StateLock lock;
            allowed = transport.matches(info.getConnHandle()) && meetsSecurity(info);
            BLE_TRACE("status_read", allowed ? "accepted" : "rejected", &info);
            transport.encode(bootId, INPUT_COMMAND_SIZE - 1, value);
          }
          // ATT enforces READ_ENC/READ_AUTHEN; also withhold the value if
          // bonding/key size do not meet the command transport policy.
          characteristic->setValue(value, allowed ? sizeof(value) : 0);
        }
      };

      ServerCallbacks serverCallbacks;
      RxCallbacks rxCallbacks;
      SubscriptionCallbacks subscriptionCallbacks;

      bool canSend(uint32_t generation, uint16_t handle) {
        StateLock lock;
        return transport.matches(handle) && transport.generation == generation &&
               (transport.reason() == State::Reason::Ready || transport.reason() == State::Reason::QueueFull);
      }

      void notifyBytes(const uint8_t *data, size_t length) {
        if (!running || txCharacteristic == nullptr)
          return;

        uint32_t generation;
        uint16_t handle;
        {
          StateLock lock;
          generation = transport.generation;
          handle = transport.handle;
        }

        while (length > 0) {
          if (!canSend(generation, handle)) {
            droppedTxBytes += length;
            return;
          }

          const uint16_t mtu = NimBLEDevice::getServer()->getPeerMTU(handle);
          size_t payloadSize = defaultNotificationSize;
          if (mtu > defaultNotificationSize + notificationOverhead)
            payloadSize = mtu - notificationOverhead;
          if (payloadSize > maxNotificationSize)
            payloadSize = maxNotificationSize;

          size_t chunkSize = length;
          if (chunkSize > payloadSize)
            chunkSize = payloadSize;

          if (!canSend(generation, handle)) {
            droppedTxBytes += length;
            return;
          }

          if (!txCharacteristic->notify(data, chunkSize, handle)) {
            failedNotifications++;
            droppedTxBytes += length;
            return;
          }

          data += chunkSize;
          length -= chunkSize;

          if (length > 0)
            vTaskDelay(notificationChunkDelay);
        }
      }

      void executeCommand() {
        {
          StateLock lock;
          if (parserGeneration != transport.generation ||
              (transport.reason() != State::Reason::Ready && transport.reason() != State::Reason::QueueFull)) {
#ifdef RFLINK_BLE_DEBUG
            diagnostics.parserDropped += commandLength;
#endif
            resetCommandBuffer();
            return;
          }
        }
#ifdef RFLINK_BLE_DEBUG
        diagnostics.commands++;
#endif
        commandBuffer[commandLength] = 0;
        RFLink::sendRawPrint(F("\33[2K\r"));
        RFLink::sendRawPrint(F("Message arrived [BLE]:"));
        RFLink::sendRawPrint(commandBuffer);
        RFLink::sendRawPrint(F("\r\n"));
        RFLink::executeCliCommand(commandBuffer);
        resetCommandBuffer();
      }

      void processReceivedByte(char byte) {
        if (byte == '\r' || byte == '\n') {
          if (discardUntilNewline) {
#ifdef RFLINK_BLE_DEBUG
            diagnostics.parserDropped++;
#endif
            resetCommandBuffer();
            return;
          }

          if (commandLength > 0)
            executeCommand();
          return;
        }

        if (discardUntilNewline) {
#ifdef RFLINK_BLE_DEBUG
          diagnostics.parserDropped++;
#endif
          return;
        }

        if (commandLength >= INPUT_COMMAND_SIZE - 1) {
#ifdef RFLINK_BLE_DEBUG
          diagnostics.parserDropped += commandLength + 1;
#endif
          discardUntilNewline = true;
          commandLength = 0;
          broadcastMessage(F("Error: BLE command is too long and was ignored\r\n"));
          return;
        }

        commandBuffer[commandLength++] = byte;
      }
    }

    const char jsonNameEnabled[] = "enabled";
    const char jsonNameDeviceName[] = "device_name";

    Config::ConfigItem configItems[] = {
            Config::ConfigItem(jsonNameEnabled, Config::SectionId::BLE_id, false, paramsUpdatedCallback),
            Config::ConfigItem(jsonNameDeviceName, Config::SectionId::BLE_id, "RFLink32", paramsUpdatedCallback),
            Config::ConfigItem()};

    void mainLoop() {
      if (!running)
        return;

#ifdef RFLINK_BLE_DEBUG
      diagnostics.lastConsumer = millis();
#endif
      const uint32_t passkey = pendingPasskey.exchange(UINT32_MAX);
      if (passkey != UINT32_MAX)
        Serial.printf("BLE pairing PIN: %06lu\r\n", static_cast<unsigned long>(passkey));

      bool advertise, notifyStatus;
      {
        StateLock lock;
        if (transport.cleanup && (!transport.overflow || transport.handle == State::noHandle)) {
          BLE_TRACE("queue_reset", "consumer", nullptr, transport.count);
#ifdef RFLINK_BLE_DEBUG
          diagnostics.resets++;
          diagnostics.parserDropped += commandLength;
          diagnostics.resetBytes += transport.count;
#endif
          resetCommandBuffer();
          droppedRxBytes += transport.clean();
          parserGeneration = transport.generation;
        }
#ifdef RFLINK_BLE_DEBUG
        const auto previousReason = transport.reason();
#endif
        transport.publishReady(); // Only the initialized consumer may publish READY.
#ifdef RFLINK_BLE_DEBUG
        if (previousReason != transport.reason())
          BLE_TRACE("availability", "consumer", nullptr, 0, static_cast<int>(transport.reason()));
#endif
        advertise = transport.handle == State::noHandle;
        notifyStatus = transport.overflow || (transport.dirty && transport.secure && transport.statusSubscribed &&
                       (nextStatusAttempt == 0 || static_cast<int32_t>(millis() - nextStatusAttempt) >= 0));
      }

      if (advertise && restartAdvertisingRequested.exchange(false))
        NimBLEDevice::startAdvertising();
      if (notifyStatus && !statusEventPending.exchange(true))
        ble_npl_eventq_put(nimble_port_get_dflt_eventq(), &statusEvent);

      char byte;
      // Bound each pass so continuous input cannot starve other firmware work.
      for (size_t budget = 0; budget < rxQueueSize; ++budget) {
        {
          StateLock lock;
          if (parserGeneration != transport.generation || !transport.pop(byte)) break;
#ifdef RFLINK_BLE_DEBUG
          diagnostics.consumed++;
#endif
        }
        processReceivedByte(byte);
#ifdef RFLINK_BLE_DEBUG
        diagnostics.parserBytes = commandLength;
        diagnostics.discarding = discardUntilNewline;
#endif
      }
    }

    void setup() {
      if (running)
        return;

      // Configuration is read only at boot; edits take effect after reboot.
      Config::ConfigItem *enabled = Config::findConfigItem(jsonNameEnabled, Config::SectionId::BLE_id);
      if (enabled == nullptr || !enabled->getBoolValue()) {
        Serial.println(F("BLE UART service disabled"));
        return;
      }
      Config::ConfigItem *deviceName = Config::findConfigItem(jsonNameDeviceName, Config::SectionId::BLE_id);

      resetCommandBuffer();
      const char *name = deviceName != nullptr ? deviceName->getCharValue() : "RFLink32";
      if (!NimBLEDevice::init(name)) {
        Serial.println(F("Failed to initialize BLE"));
        return;
      }
      // The radio is initialized, so esp_random has hardware entropy.
      bootId = (static_cast<uint64_t>(esp_random()) << 32) | esp_random();
      ble_npl_event_init(&statusEvent, sendStatusEvent, nullptr);
      NimBLEDevice::setSecurityAuth(true, true, true); // Bonding, MITM, Secure Connections.
      NimBLEDevice::setSecurityIOCap(BLE_HS_IO_DISPLAY_ONLY);
      // Keep NimBLE's default key distribution, NVS storage and oldest-bond eviction.
      NimBLEServer *server = NimBLEDevice::createServer();
      server->setCallbacks(&serverCallbacks, false);
      // Reset the previous client's command state in mainLoop before advertising.
      server->advertiseOnDisconnect(false);

      NimBLEService *service = server->createService(serviceUuid);

      txCharacteristic = service->createCharacteristic(
              txCharacteristicUuid,
              NIMBLE_PROPERTY::NOTIFY | NIMBLE_PROPERTY::READ_ENC | NIMBLE_PROPERTY::READ_AUTHEN,
              maxNotificationSize);
      txCharacteristic->setCallbacks(&subscriptionCallbacks);

      NimBLECharacteristic *rxCharacteristic = service->createCharacteristic(
              rxCharacteristicUuid,
              NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR |
              NIMBLE_PROPERTY::WRITE_ENC | NIMBLE_PROPERTY::WRITE_AUTHEN);
      rxCharacteristic->setCallbacks(&rxCallbacks);

      NimBLEService *statusService = server->createService(statusServiceUuid);
      statusCharacteristic = statusService->createCharacteristic(
              statusCharacteristicUuid, NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY |
              NIMBLE_PROPERTY::READ_ENC | NIMBLE_PROPERTY::READ_AUTHEN, 17);
      statusCharacteristic->setCallbacks(&subscriptionCallbacks);

      // NimBLE 2.x starts the GATT server, including all services, when advertising starts.
      NimBLEAdvertising *advertising = NimBLEDevice::getAdvertising();
      advertising->addServiceUUID(serviceUuid);
      advertising->enableScanResponse(true);
      advertising->setName(name);
      advertising->setPreferredParams(0x06, 0x12);

#ifdef RFLINK_BLE_DEBUG
      if (!diagnostics.start(diagnosticSnapshot))
        Serial.println(F("BLE USB diagnostics task allocation failed"));
#endif
      running = NimBLEDevice::startAdvertising();
      if (!running) {
        Serial.println(F("Failed to start BLE advertising"));
        return;
      }
#ifdef RFLINK_BLE_DEBUG
      reportGattRegistration(service, rxCharacteristic);
      reportGattRegistration(service, txCharacteristic);
      reportGattRegistration(statusService, statusCharacteristic);
#endif
      Serial.printf("BLE UART service started; bonds %d/%d\r\n", NimBLEDevice::getNumBonds(), maxBonds);
    }

    void broadcastMessage(const char *message) {
      if (message == nullptr)
        return;

      notifyBytes(reinterpret_cast<const uint8_t *>(message), strlen(message));
    }

    void broadcastMessage(const __FlashStringHelper *message) {
      if (message == nullptr)
        return;

      String buffer(message);
      broadcastMessage(buffer.c_str());
    }

    void broadcastMessage(char value) {
      notifyBytes(reinterpret_cast<const uint8_t *>(&value), 1);
    }

    void getStatusJsonString(JsonObject &output) {
      JsonObject status = output.createNestedObject(F("ble"));
      status[F("status")] = running ? F("running") : F("disabled");
      bool connected, ready;
      uint8_t reason;
      uint32_t session;
      {
        StateLock lock;
        connected = transport.handle != State::noHandle;
        ready = transport.reason() == State::Reason::Ready;
        reason = static_cast<uint8_t>(transport.reason());
        session = connected ? transport.generation : 0;
      }
      status[F("connected")] = connected;
      status[F("ready")] = ready;
      status[F("reason")] = reason;
      status[F("session_id")] = session;
      status[F("rx_dropped_bytes")] = droppedRxBytes.load();
      status[F("tx_dropped_bytes")] = droppedTxBytes.load();
      status[F("tx_failed_notifications")] = failedNotifications.load();
      status[F("status_failed_notifications")] = failedStatusNotifications.load();
    }

  }
}

#endif // RFLINK_BLE_ENABLED && ESP32
