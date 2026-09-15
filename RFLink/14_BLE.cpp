#include "14_BLE.h"

#if defined(RFLINK_BLE_ENABLED) && defined(ESP32)

#include "3_Serial.h"
#include "RFLink.h"

#include <NimBLEDevice.h>
#include <atomic>
#include <esp_system.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>

namespace RFLink {
  namespace BLE {

    namespace {
      constexpr char serviceUuid[] = "6E400001-B5A3-F393-E0A9-E50E24DCCA9E";
      constexpr char rxCharacteristicUuid[] = "6E400002-B5A3-F393-E0A9-E50E24DCCA9E";
      constexpr char txCharacteristicUuid[] = "6E400003-B5A3-F393-E0A9-E50E24DCCA9E";
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
      static_assert(MYNEWT_VAL(BLE_STORE_MAX_CCCDS) >= 2 * maxBonds, "Reserve subscriptions for all bonds");

      NimBLECharacteristic *txCharacteristic = nullptr;
      QueueHandle_t rxQueue = nullptr;

      bool running = false;
      std::atomic<bool> connected{false};
      std::atomic<bool> authenticated{false};
      std::atomic<bool> restartAdvertisingRequested{false};
      std::atomic<bool> resetConnectionStateRequested{false};
      std::atomic<uint16_t> connectionHandle{BLE_HS_CONN_HANDLE_NONE};
      std::atomic<uint32_t> connectionGeneration{0};
      unsigned long droppedRxBytes = 0;
      unsigned long droppedTxBytes = 0;
      unsigned long failedNotifications = 0;

      char commandBuffer[INPUT_COMMAND_SIZE];
      size_t commandLength = 0;
      bool discardUntilNewline = false;

      void resetCommandBuffer() {
        commandBuffer[0] = 0;
        commandLength = 0;
        discardUntilNewline = false;
      }

      void paramsUpdatedCallback() {
        Serial.println(F("BLE settings saved; reboot required to apply"));
      }

      bool meetsSecurity(const NimBLEConnInfo &connInfo) {
        return connInfo.isEncrypted() && connInfo.isAuthenticated() &&
               connInfo.isBonded() && connInfo.getSecKeySize() == 16;
      }

      class ServerCallbacks : public NimBLEServerCallbacks {
        void onConnect(NimBLEServer *connectedServer, NimBLEConnInfo &connInfo) override {
          authenticated = false;
          connectionHandle = connInfo.getConnHandle();
          connectionGeneration++;
          // NimBLE can deliver CONNECT after ENC_CHANGE; its descriptor is live.
          authenticated = meetsSecurity(connInfo);
          connected = true;
          restartAdvertisingRequested = false;
          if (!authenticated && !NimBLEDevice::startSecurity(connInfo.getConnHandle()))
            connectedServer->disconnect(connInfo.getConnHandle());
        }

        void onDisconnect(NimBLEServer *disconnectedServer, NimBLEConnInfo &connInfo, int reason) override {
          (void) disconnectedServer;
          (void) reason;
          // NimBLE removes the connection before DISCONNECT. Ignore stale events
          // for a reused live handle, or for a different current connection.
          if (ble_gap_conn_find(connInfo.getConnHandle(), nullptr) == 0 ||
              (connectionHandle != BLE_HS_CONN_HANDLE_NONE && connectionHandle != connInfo.getConnHandle()))
            return;

          authenticated = false;
          connected = false;
          connectionHandle = BLE_HS_CONN_HANDLE_NONE;
          connectionGeneration++;
          resetConnectionStateRequested = true;
          restartAdvertisingRequested = true;
          Serial.printf("BLE UART disconnected; bonds %d/%d\r\n", NimBLEDevice::getNumBonds(), maxBonds);
        }

        uint32_t onPassKeyDisplay() override {
          authenticated = false;
          const uint32_t passkey = esp_random() % 1000000;
          // Use USB/hardware Serial directly: sendRawPrint also broadcasts to
          // BLE, TCP and potentially OLED, and must never carry this PIN.
          Serial.printf("BLE pairing PIN: %06lu\r\n", static_cast<unsigned long>(passkey));
          return passkey;
        }

        void onAuthenticationComplete(NimBLEConnInfo &connInfo) override {
          authenticated = meetsSecurity(connInfo);
          if (!authenticated) {
            Serial.println(F("BLE authentication failed; UART access denied"));
            NimBLEDevice::getServer()->disconnect(connInfo.getConnHandle());
          } else {
            Serial.println(F("BLE UART authenticated"));
          }
        }

        void onConfirmPassKey(NimBLEConnInfo &connInfo, uint32_t pin) override {
          (void) pin;
          // Display-only passkey entry, never unattended confirmation.
          NimBLEDevice::injectConfirmPasskey(connInfo, false);
        }
      };

      class RxCallbacks : public NimBLECharacteristicCallbacks {
        void onWrite(NimBLECharacteristic *characteristic, NimBLEConnInfo &connInfo) override {
          if (!authenticated || connInfo.getConnHandle() != connectionHandle ||
              !connInfo.isEncrypted() || !connInfo.isAuthenticated() || !connInfo.isBonded())
            return;

          std::string value = characteristic->getValue();

          for (char byte : value) {
            if (rxQueue == nullptr || xQueueSend(rxQueue, &byte, 0) != pdTRUE)
              droppedRxBytes++;
          }
        }
      };

      ServerCallbacks serverCallbacks;
      RxCallbacks rxCallbacks;

      void notifyBytes(const uint8_t *data, size_t length) {
        if (!running || !connected || !authenticated || txCharacteristic == nullptr)
          return;

        const uint32_t generation = connectionGeneration;
        const uint16_t handle = connectionHandle;
        if (handle == BLE_HS_CONN_HANDLE_NONE)
          return;

        while (length > 0) {
          if (!connected || !authenticated || generation != connectionGeneration) {
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

          if (!connected || !authenticated || generation != connectionGeneration) {
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
            resetCommandBuffer();
            return;
          }

          if (commandLength > 0)
            executeCommand();
          return;
        }

        if (discardUntilNewline)
          return;

        if (commandLength >= INPUT_COMMAND_SIZE - 1) {
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

      if (resetConnectionStateRequested.exchange(false)) {
        resetCommandBuffer();
        if (rxQueue != nullptr)
          xQueueReset(rxQueue);
      }

      if (!connected && !resetConnectionStateRequested && restartAdvertisingRequested.exchange(false)) {
        NimBLEDevice::startAdvertising();
      }

      char byte;
      while (authenticated && !resetConnectionStateRequested && rxQueue != nullptr &&
             xQueueReceive(rxQueue, &byte, 0) == pdTRUE)
        processReceivedByte(byte);
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

      if (rxQueue == nullptr)
        rxQueue = xQueueCreate(rxQueueSize, sizeof(char));

      if (rxQueue == nullptr) {
        Serial.println(F("Failed to allocate BLE receive queue"));
        return;
      }

      resetCommandBuffer();
      const char *name = deviceName != nullptr ? deviceName->getCharValue() : "RFLink32";
      if (!NimBLEDevice::init(name)) {
        Serial.println(F("Failed to initialize BLE"));
        return;
      }
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

      NimBLECharacteristic *rxCharacteristic = service->createCharacteristic(
              rxCharacteristicUuid,
              NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR |
              NIMBLE_PROPERTY::WRITE_ENC | NIMBLE_PROPERTY::WRITE_AUTHEN);
      rxCharacteristic->setCallbacks(&rxCallbacks);

      // NimBLE 2.x starts the GATT server, including all services, when advertising starts.
      NimBLEAdvertising *advertising = NimBLEDevice::getAdvertising();
      advertising->addServiceUUID(serviceUuid);
      advertising->enableScanResponse(true);
      advertising->setName(name);
      advertising->setPreferredParams(0x06, 0x12);

      running = NimBLEDevice::startAdvertising();
      if (!running) {
        Serial.println(F("Failed to start BLE advertising"));
        return;
      }
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
      status[F("connected")] = connected.load();
      status[F("rx_dropped_bytes")] = droppedRxBytes;
      status[F("tx_dropped_bytes")] = droppedTxBytes;
      status[F("tx_failed_notifications")] = failedNotifications;
    }

  }
}

#endif // RFLINK_BLE_ENABLED && ESP32
