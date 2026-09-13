#ifndef _14_BLE_H_
#define _14_BLE_H_

#if defined(RFLINK_BLE_ENABLED) && defined(ESP32)

#include <ArduinoJson.h>
#include "11_Config.h"

namespace RFLink {
  namespace BLE {

    extern Config::ConfigItem configItems[];

    void setup();
    void mainLoop();

    void broadcastMessage(const char *message);
    void broadcastMessage(const __FlashStringHelper *message);
    void broadcastMessage(char value);

    void getStatusJsonString(JsonObject &output);

  }
}

#endif // RFLINK_BLE_ENABLED && ESP32
#endif // _14_BLE_H_
