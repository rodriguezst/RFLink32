#ifndef RFLINK_BLE_LIVE_CONNECTION_H
#define RFLINK_BLE_LIVE_CONNECTION_H

#include "BLETransportState.h"
#include <NimBLEDevice.h>
// NimBLE-Arduino 2.5.x has no public connection-scoped CCCD getter. Isolate
// its internal ATT read adapter here; review this API when upgrading NimBLE.
// read_local and the NVS bond store cannot provide the live peer's CCCD state.
#include <nimble/nimble/host/src/ble_att_priv.h>
#include <nimble/porting/nimble/include/os/os_mbuf.h>

namespace RFLink { namespace BLE {

struct LiveConnectionState {
  bool exists = false;
  bool secure = false;
  bool txSubscribed = false;
  bool statusSubscribed = false;
};

inline bool readLiveNotifySubscription(uint16_t connection, uint16_t cccd) {
  if (cccd == 0) return false;
  os_mbuf *buffer = ble_hs_mbuf_att_pkt();
  if (buffer == nullptr) return false;
  uint8_t attError = 0;
  uint8_t value[2] = {};
  const int result = ble_att_svr_read_handle(connection, cccd, 0, buffer, &attError);
  const bool enabled = result == 0 && OS_MBUF_PKTLEN(buffer) == sizeof(value) &&
                       os_mbuf_copydata(buffer, 0, sizeof(value), value) == 0 && (value[0] & 1);
  os_mbuf_free_chain(buffer);
  return enabled;
}

// Call only from the NimBLE host task, outside the application state lock.
// These are local reads of the active connection, never ATT traffic or NVS reads.
inline LiveConnectionState readLiveConnection(uint16_t handle, uint16_t txCccd, uint16_t statusCccd) {
  LiveConnectionState state;
  ble_gap_conn_desc connection{};
  if (ble_gap_conn_find(handle, &connection) != 0) return state;
  state.exists = true;
  state.secure = transportSecuritySatisfied(connection.sec_state.encrypted,
                                            connection.sec_state.authenticated,
                                            connection.sec_state.bonded,
                                            connection.sec_state.key_size);
  state.txSubscribed = readLiveNotifySubscription(handle, txCccd);
  state.statusSubscribed = readLiveNotifySubscription(handle, statusCccd);
  return state;
}

}} // namespace RFLink::BLE
#endif
