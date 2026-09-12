#include "14_BLE.h"

#if defined(RFLINK_BLE_ENABLED) && defined(ESP32)

#include "3_Serial.h"
#include "RFLink.h"

#include <NimBLEDevice.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>

namespace RFLink {
  namespace BLE {

    namespace {
      constexpr char serviceUuid[] = "6E400001-B5A3-F393-E0A9-E50E24DCCA9E";
      constexpr char rxCharacteristicUuid[] = "6E400002-B5A3-F393-E0A9-E50E24DCCA9E";
      constexpr char txCharacteristicUuid[] = "6E400003-B5A3-F393-E0A9-E50E24DCCA9E";
      constexpr size_t defaultNotificationSize = 20;
      constexpr TickType_t notificationChunkDelay = pdMS_TO_TICKS(10);
      constexpr size_t rxQueueSize = INPUT_COMMAND_SIZE + 64;

      NimBLEServer *server = nullptr;
      NimBLECharacteristic *txCharacteristic = nullptr;
      QueueHandle_t rxQueue = nullptr;

      bool running = false;
      volatile bool connected = false;
      volatile bool restartAdvertisingRequested = false;
      volatile bool resetConnectionStateRequested = false;
      unsigned long droppedRxBytes = 0;

      char commandBuffer[INPUT_COMMAND_SIZE];
      size_t commandLength = 0;
      bool discardUntilNewline = false;

      void resetCommandBuffer() {
        commandBuffer[0] = 0;
        commandLength = 0;
        discardUntilNewline = false;
      }

      class ServerCallbacks : public NimBLEServerCallbacks {
        void onConnect(NimBLEServer *connectedServer) override {
          (void) connectedServer;
          connected = true;
          restartAdvertisingRequested = false;
        }

        void onDisconnect(NimBLEServer *disconnectedServer) override {
          (void) disconnectedServer;
          connected = false;
          restartAdvertisingRequested = true;
          resetConnectionStateRequested = true;
        }
      };

      class RxCallbacks : public NimBLECharacteristicCallbacks {
        void onWrite(NimBLECharacteristic *characteristic) override {
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
        if (!running || !connected || txCharacteristic == nullptr)
          return;

        while (length > 0) {
          size_t chunkSize = length;
          if (chunkSize > defaultNotificationSize)
            chunkSize = defaultNotificationSize;

          txCharacteristic->notify(data, chunkSize);

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

    namespace params {
      bool enabled = false;
      String deviceName(F("RFLink32"));
    }

    const char jsonNameEnabled[] = "enabled";
    const char jsonNameDeviceName[] = "device_name";

    Config::ConfigItem configItems[] = {
            Config::ConfigItem(jsonNameEnabled, Config::SectionId::BLE_id, false, paramsUpdatedCallback),
            Config::ConfigItem(jsonNameDeviceName, Config::SectionId::BLE_id, "RFLink32", paramsUpdatedCallback),
            Config::ConfigItem()};

    void setup() {
      if (rxQueue == nullptr)
        rxQueue = xQueueCreate(rxQueueSize, sizeof(char));

      resetCommandBuffer();
      refreshParametersFromConfig(false);

      if (params::enabled)
        start();
    }

    void mainLoop() {
      if (!running)
        return;

      if (resetConnectionStateRequested) {
        resetConnectionStateRequested = false;
        resetCommandBuffer();
        if (rxQueue != nullptr)
          xQueueReset(rxQueue);
      }

      if (restartAdvertisingRequested && !connected) {
        restartAdvertisingRequested = false;
        NimBLEDevice::startAdvertising();
      }

      char byte;
      while (rxQueue != nullptr && xQueueReceive(rxQueue, &byte, 0) == pdTRUE)
        processReceivedByte(byte);
    }

    void paramsUpdatedCallback() {
      refreshParametersFromConfig();
    }

    void refreshParametersFromConfig(bool triggerChanges) {
      Config::ConfigItem *item;
      bool enabledChanged = false;
      bool deviceNameChanged = false;

      item = Config::findConfigItem(jsonNameEnabled, Config::SectionId::BLE_id);
      if (item != nullptr && item->getBoolValue() != params::enabled) {
        params::enabled = item->getBoolValue();
        enabledChanged = true;
      }

      item = Config::findConfigItem(jsonNameDeviceName, Config::SectionId::BLE_id);
      if (item != nullptr && params::deviceName != item->getCharValue()) {
        params::deviceName = item->getCharValue();
        deviceNameChanged = true;
      }

      if (!triggerChanges)
        return;

      if (enabledChanged) {
        if (params::enabled)
          start();
        else
          stop();
      } else if (deviceNameChanged && params::enabled) {
        restart();
      }
    }

    void start() {
      if (running)
        return;

      if (rxQueue == nullptr)
        rxQueue = xQueueCreate(rxQueueSize, sizeof(char));

      if (rxQueue == nullptr) {
        Serial.println(F("Failed to allocate BLE receive queue"));
        return;
      }

          NimBLEDevice::init(params::deviceName.c_str());
          server = NimBLEDevice::createServer();
      server->setCallbacks(&serverCallbacks);

          NimBLEService *service = server->createService(serviceUuid);

      txCharacteristic = service->createCharacteristic(
              txCharacteristicUuid,
              NIMBLE_PROPERTY::NOTIFY);

          NimBLECharacteristic *rxCharacteristic = service->createCharacteristic(
              rxCharacteristicUuid,
              NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR);
      rxCharacteristic->setCallbacks(&rxCallbacks);

      service->start();

      NimBLEAdvertising *advertising = NimBLEDevice::getAdvertising();
      advertising->addServiceUUID(serviceUuid);
      advertising->setScanResponse(true);
      advertising->setMinPreferred(0x06);
      advertising->setMaxPreferred(0x12);

      connected = false;
      restartAdvertisingRequested = false;
      resetConnectionStateRequested = false;
      running = true;
      NimBLEDevice::startAdvertising();
      Serial.println(F("BLE UART service started"));
    }

    void stop() {
      if (!running)
        return;

      NimBLEDevice::stopAdvertising();
      NimBLEDevice::deinit(true);

      server = nullptr;
      txCharacteristic = nullptr;
      connected = false;
      restartAdvertisingRequested = false;
      resetConnectionStateRequested = false;
      running = false;
      resetCommandBuffer();

      if (rxQueue != nullptr)
        xQueueReset(rxQueue);

      Serial.println(F("BLE UART service stopped"));
    }

    void restart() {
      stop();
      start();
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
      status[F("connected")] = connected;
      status[F("rx_dropped_bytes")] = droppedRxBytes;
    }

  }
}

#endif // RFLINK_BLE_ENABLED && ESP32