#ifndef RFLINK_BLE_TRANSPORT_STATE_H
#define RFLINK_BLE_TRANSPORT_STATE_H

#include <stddef.h>
#include <stdint.h>

namespace RFLink { namespace BLE {

inline bool transportSecuritySatisfied(bool encrypted, bool authenticated, bool bonded, uint8_t keySize) {
  return encrypted && authenticated && bonded && keySize == 16;
}

// All access is serialized by the caller. No BLE/RTOS calls or allocation here.
template<size_t Capacity>
struct TransportState {
  enum class Reason : uint8_t {
    Ready = 0, Disconnected = 1, Security = 2, Consumer = 3,
    Cleanup = 4, ResponseSubscription = 5, Overflow = 6, QueueFull = 7
  };
  enum class WriteResult : uint8_t {
    Accepted, WrongConnection, Security, Consumer, Cleanup,
    ResponseSubscription, Overflow
  };
  struct Session {
    uint16_t handle;
    uint32_t generation;
  };
  static constexpr uint16_t noHandle = 0xffff;
  uint16_t handle = noHandle;
  uint32_t generation = 0;
  bool secure = false, txSubscribed = false, statusSubscribed = false;
  bool consumerInitialized = false, cleanup = true, overflow = false;
  bool publishedReady = false, dirty = true;
  size_t head = 0, count = 0;
  char bytes[Capacity];

  bool matches(uint16_t peer) const { return handle != noHandle && handle == peer; }
  Session session() const { return {handle, generation}; }
  bool matches(Session expected) const {
    return matches(expected.handle) && generation == expected.generation;
  }
  // A snapshot belongs to the session that requested it, even if handles are reused.
  // Reconciliation can revoke READY, but only the consumer may publish it.
  bool reconcile(Session expected, bool security, bool tx, bool status) {
    if (!matches(expected)) return false;
    if (secure != security || txSubscribed != tx || statusSubscribed != status) dirty = true;
    secure = security;
    txSubscribed = tx;
    statusSubscribed = status;
    if (!secure || !txSubscribed) publishedReady = false;
    return true;
  }
  Reason reason() const {
    if (handle == noHandle) return Reason::Disconnected;
    if (!secure) return Reason::Security;
    if (!consumerInitialized) return Reason::Consumer;
    if (overflow) return Reason::Overflow;
    if (cleanup) return Reason::Cleanup;
    if (!txSubscribed) return Reason::ResponseSubscription;
    if (!publishedReady) return Reason::Consumer;
    if (count == Capacity) return Reason::QueueFull;
    return Reason::Ready;
  }
  void connect(uint16_t peer) {
    handle = peer;
    if (++generation == 0) ++generation;
    secure = txSubscribed = statusSubscribed = publishedReady = false;
    cleanup = dirty = true;
    overflow = false;
  }
  bool disconnect(uint16_t peer) {
    if (!matches(peer)) return false;
    handle = noHandle;
    secure = txSubscribed = statusSubscribed = publishedReady = false;
    cleanup = dirty = true;
    return true;
  }
  bool authenticate(uint16_t peer, bool security) {
    if (!matches(peer)) return false;
    secure = security;
    if (!secure) publishedReady = false;
    dirty = true;
    return true;
  }
  bool subscribe(uint16_t peer, bool status, bool enabled) {
    if (!matches(peer)) return false;
    if (status) statusSubscribed = enabled;
    else {
      txSubscribed = enabled;
      if (!enabled) publishedReady = false;
    }
    dirty = true;
    return true;
  }
  // Called only by the command consumer after resetting its parser.
  size_t clean() {
    const size_t discarded = count;
    head = count = 0;
    cleanup = overflow = false;
    consumerInitialized = true;
    dirty = true;
    return discarded;
  }
  void publishReady() {
    const bool ready = handle != noHandle && secure && consumerInitialized &&
                       !cleanup && !overflow && txSubscribed;
    if (ready != publishedReady) dirty = true;
    publishedReady = ready;
  }
  WriteResult write(uint16_t peer, bool security, const char* data, size_t length) {
    if (!matches(peer)) return WriteResult::WrongConnection;
    if (!security) {
      authenticate(peer, false);
      return WriteResult::Security;
    }
    switch (reason()) {
      case Reason::Ready:
      case Reason::QueueFull: break;
      case Reason::Security: return WriteResult::Security;
      case Reason::Consumer: return WriteResult::Consumer;
      case Reason::Cleanup: return WriteResult::Cleanup;
      case Reason::ResponseSubscription: return WriteResult::ResponseSubscription;
      case Reason::Overflow: return WriteResult::Overflow;
      default: return WriteResult::WrongConnection;
    }
    // Never execute a command with a silently missing middle fragment.
    if (length > Capacity - count) {
      overflow = cleanup = dirty = true;
      publishedReady = false;
      return WriteResult::Overflow;
    }
    for (size_t i = 0; i < length; ++i) bytes[(head + count + i) % Capacity] = data[i];
    count += length;
    if (count == Capacity) dirty = true;
    return WriteResult::Accepted;
  }
  bool pop(char& byte) {
    const auto current = reason();
    if ((current != Reason::Ready && current != Reason::QueueFull) || count == 0) return false;
    if (count == Capacity) dirty = true;
    byte = bytes[head];
    head = (head + 1) % Capacity;
    --count;
    return true;
  }
  // Version 1, 17 bytes, little endian; maxCommand excludes CR/LF.
  void encode(uint64_t boot, uint16_t maxCommand, uint8_t (&out)[17]) const {
    out[0] = 1;
    for (unsigned i = 0; i < 8; ++i) out[1 + i] = boot >> (8 * i);
    const uint32_t session = handle == noHandle ? 0 : generation;
    for (unsigned i = 0; i < 4; ++i) out[9 + i] = session >> (8 * i);
    out[13] = reason() == Reason::Ready;
    out[14] = static_cast<uint8_t>(reason());
    out[15] = maxCommand;
    out[16] = maxCommand >> 8;
  }
};

}} // namespace RFLink::BLE
#endif
