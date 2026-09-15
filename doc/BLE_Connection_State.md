# Delayed CONNECT and readiness recovery

The reported order — authentication and subscriptions at `t=713168` with diagnostic generation 5, followed by CONNECT at `t=713259` and generation 6 — is possible with the installed **NimBLE-Arduino 2.5.1**. It is independent of the Service Changed/cache fix.

## Source finding

In the bundled `nimble/nimble/host/src/ble_gap.c`:

- `ble_gap_rx_conn_complete` allocates and inserts the live connection, then starts the remote version/features exchange. It does not immediately call the application's CONNECT callback.
- `ble_gap_rx_rd_rem_ver_info_complete` / `ble_gap_rx_rd_rem_sup_feat_complete` eventually call `ble_gap_event_connect_call`, which delivers CONNECT to NimBLEServer and then RFLink.
- Meanwhile `ble_gap_enc_event` can deliver ENC_CHANGE and call `ble_gatts_bonding_restored`, producing authentication and restored subscription callbacks first.

RFLink's `onConnect` already initialized its state before starting security. Moving that initialization earlier *within the same handler* cannot fix events that precede entry into the handler. The previous firmware discarded those events, then reset the cached flags on CONNECT and never reconciled them. An already secured link could therefore remain at reason 2 indefinitely.

The diagnostic `g` is the firmware's current generation at capture time, not an incarnation identifier supplied by NimBLE with that event. Security fields come from the callback's connection descriptor, or the most recent such sample; they are distinct from the internal cached readiness flags. Protected READ success does not, by itself, prove that those internal flags were updated. USB summaries and event lines can be printed in a different order; compare capture timestamps.

## Current behavior

1. CONNECT initializes the session immediately and uses the security state of its current descriptor. If the link already meets encryption, MITM/authentication, bonding and 16-byte-key requirements, it does not redundantly initiate security.
2. The consumer schedules a host-task check immediately after initialization and approximately every 250 ms while connected. The check reads the live security descriptor and current TX/status CCCDs locally. Persisted bond subscriptions and early callback payloads are not replayed as readiness evidence.
3. Each queued check captures both handle and generation. Both are checked before querying and before applying the result. A reused handle alone cannot authorize applying an older snapshot. Query and application run on the NimBLE host, serialized with connection lifecycle callbacks.
4. Reconciliation can revoke READY or update its prerequisites; only the consumer can publish READY after cleanup. The status CCCD is still optional for command use. A failed CCCD query is treated as an unconfirmed subscription and cannot grant READY.
5. DISCONNECT also checks that the old handle is no longer live before invalidating state. A disconnect before the delayed CONNECT callback still requests advertising again; it must not leave the firmware stranded merely because no application session had been created yet.

`BLELiveConnection.h` isolates the internal NimBLE connection-scoped ATT-read adapter. A local read with no connection context, or reading the NVS CCCD record, is not an equivalent substitute. The adapter reads only the two discovered CCCD handles; it emits no radio requests and allocates/frees a bounded mbuf for each local read.

## Validation

The host regression suite covers early callbacks before CONNECT, recovery from a current snapshot, cleanup and consumer publication, rejection of stale snapshots after disconnect/reuse of handle 0, rejection of stale revocations, live TX unsubscription, security loss, optional status subscription, and the security predicate's encryption/authentication/bonding/key-size combinations. Existing command queue, wire-format and 24 event-order tests remain in place.

Run `g++ -std=c++11 -Wall -Wextra -Werror -fsanitize=address,undefined tests/ble_transport_state_test.cpp -o /tmp/ble_transport_test && /tmp/ble_transport_test` and compile the normal/debug PlatformIO environments. Firmware builds validate the adapter against the installed NimBLE headers/library; host tests do not simulate the controller's actual remote-feature exchange or iOS scheduling.

On hardware, reconnect an already bonded client repeatedly, including the observed early-authentication order, and confirm reason 2 progresses through consumer/subscription prerequisites to READY without an app-side bypass. Also test TX unsubscribe/resubscribe, disconnect before delayed CONNECT, and rapid handle reuse. Allow firmware-loop scheduling time in addition to the 250 ms refresh interval.
