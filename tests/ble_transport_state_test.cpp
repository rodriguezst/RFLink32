#include "../RFLink/BLETransportState.h"
#include <algorithm>
#include <cassert>
#include <cstdio>
#include <cstring>

using State = RFLink::BLE::TransportState<16>;
using Reason = State::Reason;
using Result = State::WriteResult;

static void ready(State &s, uint16_t peer = 7) {
  s.connect(peer);
  s.clean();
  s.authenticate(peer, true);
  s.subscribe(peer, false, true);
  s.publishReady();
  assert(s.reason() == Reason::Ready);
}

int main() {
  // The same security predicate is used for callbacks and live reconciliation.
  for (int bits = 0; bits < 8; ++bits) {
    for (uint8_t key : {uint8_t(0), uint8_t(7), uint8_t(15), uint8_t(16)}) {
      assert(RFLink::BLE::transportSecuritySatisfied(bits & 1, bits & 2, bits & 4, key) ==
             (bits == 7 && key == 16));
    }
  }

  // NimBLE 2.5.1 can restore encryption/CCCDs before delivering CONNECT.
  State delayed;
  assert(!delayed.authenticate(0, true));
  assert(!delayed.subscribe(0, false, true));
  assert(!delayed.subscribe(0, true, true));
  assert(delayed.generation == 0 && delayed.reason() == Reason::Disconnected);
  delayed.connect(0);
  auto current = delayed.session();
  // Only a fresh live snapshot may recover those facts; no callback replay.
  assert(delayed.reconcile(current, true, true, true));
  delayed.publishReady();
  assert(delayed.reason() != Reason::Ready); // Cleanup/consumer still required.
  delayed.clean();
  assert(delayed.reason() != Reason::Ready);
  delayed.publishReady();
  assert(delayed.reason() == Reason::Ready);
  assert(delayed.write(0, true, "10;PING;\n", 9) == Result::Accepted);

  // Deferred work from a previous incarnation of handle 0 must do nothing.
  const auto old = current;
  delayed.disconnect(0);
  assert(!delayed.reconcile(old, true, true, true));
  delayed.connect(0);
  current = delayed.session();
  assert(current.generation != old.generation);
  assert(!delayed.reconcile(old, true, true, true));
  assert(!delayed.secure && !delayed.txSubscribed && !delayed.statusSubscribed);
  assert(delayed.clean() == 9);
  assert(delayed.reconcile(current, true, true, false)); // Legacy client is valid.
  delayed.publishReady();
  assert(delayed.reason() == Reason::Ready);
  assert(!delayed.reconcile(old, false, false, false)); // Cannot revoke new READY either.
  assert(delayed.reason() == Reason::Ready);

  // Recover/revoke against actual CCCDs, never persisted bond subscriptions.
  assert(delayed.reconcile(current, true, false, true));
  assert(delayed.reason() == Reason::ResponseSubscription);
  assert(delayed.reconcile(current, true, true, true));
  assert(delayed.reason() == Reason::Consumer); // Reconciliation cannot publish READY.
  delayed.publishReady();
  assert(delayed.reconcile(current, false, true, true));
  assert(delayed.reason() == Reason::Security);
  delayed.publishReady();
  assert(delayed.reason() == Reason::Security);
  // Every ordering of authentication, TX CCCD, status CCCD and consumer cleanup.
  int order[] = {0, 1, 2, 3};
  do {
    State s;
    s.connect(7);
    for (int event : order) {
      switch (event) {
        case 0: s.authenticate(7, true); break;
        case 1: s.subscribe(7, false, true); break;
        case 2: s.subscribe(7, true, true); break;
        case 3: s.clean(); break;
      }
      assert(s.reason() != Reason::Ready); // Callbacks cannot publish READY.
    }
    s.publishReady();
    assert(s.reason() == Reason::Ready);
    assert(s.dirty && s.statusSubscribed);
  } while (std::next_permutation(order, order + 4));

  State s;
  ready(s); // Legacy client, no status subscription.
  assert(!s.statusSubscribed);
  assert(s.write(7, true, "10;PING;\n", 9) == Result::Accepted);
  const auto oldSession = s.generation;
  assert(!s.authenticate(99, false));
  assert(!s.disconnect(99));
  assert(!s.subscribe(99, false, false));
  assert(s.reason() == Reason::Ready && s.count == 9);
  assert(s.disconnect(7));
  assert(s.reason() == Reason::Disconnected);
  char byte;
  assert(!s.pop(byte));
  uint8_t encoded[17];
  s.encode(0x0807060504030201ULL, 1999, encoded);
  assert(encoded[9] == 0 && encoded[13] == 0);

  s.connect(7); // The stack can reuse the same handle, never the old session.
  assert(s.generation != oldSession && !s.secure && !s.txSubscribed && !s.statusSubscribed);
  s.authenticate(7, true);
  s.subscribe(7, false, true);
  s.publishReady();
  assert(s.reason() == Reason::Cleanup);
  assert(s.write(7, true, "10;PING;\n", 9) == Result::Cleanup);
  assert(s.clean() == 9);
  assert(s.count == 0);
  s.publishReady();
  assert(s.reason() == Reason::Ready);

  // Unsubscribe/re-authenticate cannot reuse READY until the consumer runs.
  s.subscribe(7, false, false);
  assert(s.write(7, true, "X", 1) == Result::ResponseSubscription);
  s.subscribe(7, false, true);
  assert(s.write(7, true, "X", 1) == Result::Consumer);
  s.publishReady();
  assert(s.write(7, false, "X", 1) == Result::Security);
  assert(s.reason() == Reason::Security && !s.secure);
  s.authenticate(7, true);
  assert(s.reason() == Reason::Consumer);
  s.publishReady();

  // Ring wrap and backpressure: a full queue must still be drainable.
  assert(s.write(7, true, "abcdefghijkl", 12) == Result::Accepted);
  for (char expected = 'a'; expected <= 'h'; ++expected) {
    assert(s.pop(byte) && byte == expected);
  }
  assert(s.write(7, true, "mnopqrstuvwx", 12) == Result::Accepted);
  assert(s.reason() == Reason::QueueFull);
  for (char expected = 'i'; expected <= 'x'; ++expected) {
    assert(s.pop(byte) && byte == expected);
  }
  assert(s.reason() == Reason::Ready && !s.pop(byte));

  // A lost fragment rejects the entire write and poisons the session until disconnect.
  assert(s.write(7, true, "123456789012", 12) == Result::Accepted);
  assert(s.write(7, true, "abcde", 5) == Result::Overflow);
  assert(s.count == 12 && s.reason() == Reason::Overflow);
  s.publishReady();
  assert(!s.publishedReady && !s.pop(byte));
  assert(s.write(7, true, "\n", 1) == Result::Overflow);
  s.disconnect(7);
  assert(s.clean() == 12);
  ready(s);

  // Exact, endian-independent wire format.
  s.encode(0x0807060504030201ULL, 1999, encoded);
  const uint8_t expected[] = {1,1,2,3,4,5,6,7,8,3,0,0,0,1,0,0xcf,7};
  assert(std::memcmp(encoded, expected, sizeof(expected)) == 0);
  s.generation = UINT32_MAX;
  s.connect(8);
  assert(s.generation == 1); // Zero is reserved for no active session.
  std::puts("BLE transport state tests passed (24 event orderings, early events, live recovery, stale generations, security, queue, format)");
}
