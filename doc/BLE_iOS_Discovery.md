# CoreBluetooth: status service found, characteristic list empty

Use distinct UUIDs for the status service and its characteristic:

```swift
static let statusService = CBUUID(string: "A8F10001-8D5B-4A6D-9F32-70E4B2C6D901")
static let statusValue = CBUUID(string: "A8F10002-8D5B-4A6D-9F32-70E4B2C6D901")
```

The RFLink firmware creates this characteristic before advertising. In the installed NimBLE implementation, the characteristic declaration is registered with plain ATT READ permission; encryption/authentication permissions apply to its **value**. Waiting for READY, pairing, or subscribing to UART is not a prerequisite for discovering the declaration. The expected CoreBluetooth properties for status are READ + NOTIFY (`0x12`). Its value still requires the full transport security policy.

A successful **filtered** discovery returning an empty list means no matching characteristic was returned to that request. It does not establish why: a filter mismatch, an invalidated service reference, a stale system cache, or an actual server/ATT discrepancy need different evidence. Do not silently switch to legacy PING when the status service exists.

## Patch for the supplied BLETransport

The supplied UUID constants, service identity check, and READ/NOTIFY checks are correct. The recovery gap is in the missing-status `guard`: it calls `finish` immediately when `error == nil`. `handleDiscoveryError` only runs when there is an error, so its existing unfiltered retry never runs for this case.

Add this stored property to BLETransport:

```swift
private var retriedMissingStatusDiscovery = false
```

Reset it at the beginning of `run(...)`, alongside creation of the operation deadline:

```swift
retriedMissingStatusDiscovery = false
```

Do **not** reset it in `retryBeforeWrite`: the extra discovery budget belongs to the entire operation, including reconnects.

Replace only the missing-status guard inside `didDiscoverCharacteristicsFor` with:

```swift
guard let status = service.characteristics?.first(where: { $0.uuid == Self.statusValue }) else {
    if !gate.writeStarted, !retriedMissingStatusDiscovery,
       gate.canProceed(now: now) {
        retriedMissingStatusDiscovery = true
        gate.updateFirmwareReady(false)
        statusTracker.invalidateReadiness()
        lastStatusIssue = "Status missing from discovery; enumerating service"
        log("Status discovery succeeded without a match; retrying once with UUID filter=nil on the same current service")
        peripheral.discoverCharacteristics(nil, for: service)
        return
    }
    finish(gate.writeStarted ? .uncertain : .unavailable,
           "Status characteristic is still unavailable; discovery recovery exhausted or disallowed. No additional command bytes were sent.")
    return
}
```

The surrounding callback already checks the active operation, deadline, connection state and current service identity. Preserve those checks and the subsequent READ/NOTIFY property validation. This change adds at most one unfiltered request for a missing result, never extends the deadline, never enables writes, and never creates a synthetic ATT error. Existing bounded retries for actual errors remain separate.

For a diagnostic run, another option is to make the **initial** status request unfiltered:

```swift
peripheral.discoverCharacteristics(nil, for: readinessService)
```

Continue logging the entire `service.characteristics` inventory before selecting the status UUID. `nil` asks CoreBluetooth to discover all characteristics in that service; it is **not a system-cache flush** and is not guaranteed to repair the problem. If it also returns empty, do not loop indefinitely or treat a delay as proof of pairing completion.

## Evidence to collect if it remains empty

- Log the outgoing service UUID, requested UUIDs (or `nil`), peripheral identifier and an application connection-attempt identifier. Log each callback's raw error domain/code when non-nil, and explicitly log `error=nil` otherwise.
- Capture the debug firmware's startup GATT inventory and the connection/authentication/disconnection events from the same run. `svc_rc=0`, `chr_rc=0` and nonzero declaration/value handles for status establish that the stack registered it. A nonzero lookup result is a firmware registration problem to investigate.
- If `didModifyServices` invalidates UART/status, invalidate READY and all references to those services/characteristics. The supplied handler cancels the attempt; the next attempt must use freshly discovered references.
- With the app disconnected, inspect the service using an independent BLE browser, preferably another central device to avoid sharing the same iOS cache. If that central can discover status while the app cannot, focus on the app/iOS discovery path. If both fail while firmware lookup succeeds, inspect ATT traffic and service handle ranges.
- An ATT capture distinguishes a response actually missing declarations from a CoreBluetooth result served from cache. Reconnecting or calling `discoverServices` again does not guarantee cache invalidation. Do not change stable protocol UUIDs or remove security just to force a cache miss.

This patch addresses premature termination and improves diagnosis. It does not identify or fix the underlying cause of an empty result without those additional observations. Swift/CoreBluetooth behavior requires validation on iOS; this firmware workspace does not contain the app project.
