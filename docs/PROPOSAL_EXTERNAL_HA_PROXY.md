# Proposal: Home Assistant-owned shared Bluetooth proxies

Status: design-only RFC. No feature implementation is included in this proposal.
Issue: https://github.com/hms-homelab/hms-cpap/issues/104
Reviewed base: dbf521edaf50f882d6daab4c312b32abf026d3fb (7.1.1), 2026-11-23.

## User outcome

Use the ESPHome Bluetooth proxies already connected to Home Assistant for an
AirSense 11, while those proxies continue serving other HA Bluetooth integrations.
Use an unmodified CpapDash release to process the resulting live and stored data.
The user must not dedicate another ESP32 or replace ESPHome with CPAP firmware.

Proposed Settings label: **Use Home Assistant's Bluetooth connection**.
Off by default. This is a proposed integration mode, not a feature that a toggle
can enable in the current release.

```text
AirSense 11 <-> existing ESPHome proxy <-> Home Assistant Bluetooth manager
                                              |
                                    HA collector integration
                                              |
                              existing Mule WebSocket receiver
                                              |
                              native CpapDash night/import pipeline
```

HA owns device discovery, pairing, connection allocation and proxy selection.
CpapDash does not connect to the proxy API, obtain its encryption key, or compete
with HA for ownership. This also accommodates a travel proxy reachable by HA over
an existing VPN. It does not implement VPN setup or guarantee radio/connection
capacity: coexistence and reconnect behavior still require hardware acceptance.

This follows HA's documented Bluetooth device/adapter selection API:
https://contributors.home-assistant.io/core/integrations/bluetooth/
ESPHome documents that active connections consume limited slots while passive
advertisements are separate, so sharing is an acceptance criterion, not a promise
that every existing radio workload will fit:
https://esphome.io/components/bluetooth_proxy/

## What the current release already does

- `clients/ble/HostTransport.cpp` selects BlueZ, CoreBluetooth or WinRT. It has no
  ESPHome backend. Adding a configuration flag alone cannot create one.
- `docs/MULE_LOCAL_STREAM.md` documents the external collector channel: CDS1 live
  blocks, state/events, and kind-6 downloads. Existing HA collection can reuse it.
- `MuleService::queueCommand` admits only Summary, Identification and Settings for
  `as11_download`, with a date-only `from`. Stored waveform pages have no supported
  receiver-to-native-stored-night handoff.
- The direct host client already calls `pullStoredNight`, then
  `As11NightWriter::writeStoredGaps`; these must be reused rather than introducing
  another decoder or claiming a chart-cache reader solves ingestion.
- `MuleService` already provides one-command-at-a-time scheduling and a completion
  hook invoked outside its mutex. That hook currently routes DS2 answers only.

These findings explain why shared HA proxies work in the custom installation but
are not yet a complete, supported stock-app integration.

## Proposed implementation boundary

### 1. Explicit HA collector mode, opt-in

Add a persisted off-by-default configuration setting (candidate name:
`airsense11_ble.home_assistant.enabled`; naming subject to maintainer review).
Settings explains that a companion HA integration is required and displays its
connection, paired-machine identity, last contact, and import progress.

Enabling this mode must not silently turn off a running native collector or alter
pairing. Reject a conflicting local/direct Bluetooth configuration for the same
machine with actionable guidance. Keep keys in HA. Disabling the mode stops new
HA-mode commands and permits bounded cancellation; it neither deletes imported
records nor takes ownership of the CPAP. Define restart requirements explicitly.

Existing Mule/DS2 users and legacy external frames must retain their behavior.
The new opt-in gates the new, explicitly identified HA collector profile and
stored-page orchestration; it is not a retroactive blanket block on the existing
Mule stream. Continue respecting the stream-port configuration and do not enable
unrelated bridge features or expose any new public network listener implicitly.

### 2. Identify the collector and its capabilities

Negotiate a versioned HA collector profile on hello/ack, retaining the existing
legacy protocol. A candidate profile is `home_assistant` with capability
`as11_stored_page_v1`. Exact field names and negotiation must be agreed upstream.
No negotiation success means no new command is sent.

The profile is a protocol discriminator, NOT authentication. Preserve the existing
LAN trust boundary; do not claim that an opt-in toggle authenticates a collector.
Do not add a generic remote shell, arbitrary HTTP forwarder, or file-path upload.

Do not show or permit Mule/miner firmware updates, factory reset, reboot, or Wi-Fi
configuration for the HA collector profile. Those actions target actual Mule
hardware, not HA or a shared ESPHome proxy. Pair/read/status operations must have
explicit supported semantics; unsupported operations return a useful error.

### 3. Native stored-page handoff

Keep existing Summary/Identification/Settings commands unchanged. Keep live CDS1
and historical respiratory events on their existing paths. Do not mix stored
6.25-Hz recordings into a fabricated 25-Hz live stream.

Propose one new versioned read command, e.g. `as11_stored_page`, with an allowlisted
spool and UTC `fromDateTime`. A response binds bytes and `next_from` to the request
and connection generation, with explicit complete/failed status and integrity
metadata. Reuse kind-6 framing only after making its validation suitable for this
contract; this name and shape are NOT currently supported by released software.

The CpapDash coordinator plans nights from native Summary mask-on spans, asks one
page at a time, persists verified pages through `keepStoredPage`, and completes a
night through `writeStoredGaps` and the established native import notification.
Only mark done after every required spool and finalization succeeds. An interrupted
night remains retryable; replay must not duplicate events, pages or sessions.

Use a bounded worker/continuation so no radio wait or network wait holds
`MuleService::mu_`. Dispatch new AS11 answers alongside the existing DS2 completion
handler without replacing or breaking it. A synchronous `StoredPagePull` adapter
may wait only outside the service lock, with a deadline and cancellation.

### 4. Both sides enforce the read limits

- A fresh, connected Standby state is required; disconnected/unknown is not
  Standby. Recheck on each page and stop when therapy starts.
- Bind a transfer to one machine, unit, requested night and connection generation.
- Validate spool, real sleep-day/date/cursor and advancing cursor. Bound bytes per
  page, total bytes, pages, retries and total wall time. Choose final numeric caps
  from measured legitimate page sizes; do not copy limits blindly.
- Check ordered fragments, exact terminal/result agreement and integrity. Reject
  missing, duplicate/conflicting or cross-request fragments; never finalize partial
  or truncated data. Legacy behavior need not be redefined by this PR.
- Derive storage paths internally. Test traversal, symlinks and conflicting page
  content; do not expand the public API into arbitrary file storage.
- Preserve original SD/live samples over recovered data, real timing/gaps, lower
  sample rates and unknown event durations. Label recovered provenance accurately.
- Report whether a job is deferred, receiving, importing, complete or failed;
  never equate receiver connectivity with successful therapy recording.

## Suggested code locations

| Concern | Existing location / proposed change |
|---|---|
| Config persistence and default | `include/utils/AppConfig.h`; config API serialization and validation |
| Settings and source status | `frontend/src/app/models/config.model.ts`, `pages/settings/settings.component.ts`, Devices views/translations |
| HA profile / negotiated commands | `include/mule/MuleService.h`, `src/mule/MuleService.cpp`, Mule controller routes |
| AS11 job coordinator | A small `MuleAs11` service, following `MuleDs2` ownership/scheduling patterns |
| Completion wiring | `src/main.cpp`; compose dispatch instead of overwriting the DS2 hook |
| Stored planner/decoder/writer | Existing `services/As11StoredNight.*`, `As11NightWriter.*`; factor only necessary reusable logic |
| Radio owner / pairing / retransmit | Companion HA integration, separate deliverable/repository decision |
| Protocol/setup contract | `docs/MULE_LOCAL_STREAM.md`, a dedicated HA setup guide |

Do not submit private deployment helpers, household entities, credentials, pairing
keys, recordings or production paths. The CpapDash PR cannot alone install the HA
integration. Publish/support that companion separately or agree ownership with
the maintainers before advertising an end-to-end supported mode.

## Acceptance and regression tests required before a code PR

1. Default-off load/save/API/frontend round trip; unchanged native/Mule/DS2 behavior.
2. Capability negotiation, disabled profile, old collector, unsupported version,
   conflicting direct-owner configuration, and no firmware controls on HA units.
3. Synthetic external collector sends live blocks, verified historical events and
   Summary through the real receiver; supported reparse produces expected records.
4. Multi-page stored night follows cursors through all five native spools and
   produces native writer files, import notification and accurate provenance.
5. Fragment loss/reorder/cross-request data, truncation, invalid cursor/hash/size,
   unexpected connection generation and conflicting page content are rejected.
6. Disconnect/restart/replay, cancellation, Standby becoming Therapy, timeout and
   a partially complete night leave no falsely complete import and retry cleanly.
7. Original SD/live coverage wins; genuine gaps remain gaps; unknown durations
   remain unknown; replay does not duplicate sessions or events.
8. DS2 completion dispatch still works, no callback runs while holding the Mule
   mutex, and stopping the worker does not strand application shutdown.
9. A real HA test instance uses existing proxies while another Bluetooth integration
   still receives advertisements/works; home-to-travel reconnect retains identity.
   This hardware test needs separate user coordination; synthetic tests cannot
   prove proxy coexistence or travel failover.

## Deliverable and migration gates

This proposal contains documentation only: no feature code or new functional
tests are included. It does not deploy software, update firmware, alter pairing
or restart HA. Issue #104 had no comments at the time of the source review;
maintainer agreement on this interface is pending.

First seek agreement on the HA ownership/profile and stored-page contract. Then
implement against public synthetic fixtures, run focused plus relevant regression
suites, and submit a draft code PR with precise test counts and remaining hardware
limits. Do not promise a stock cutover merely because this PR merges: the HA
companion, post-therapy jobs, reports, app identity and data migration still need
their own compatibility checks and separately approved activation.
