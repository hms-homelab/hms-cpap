# SDD-023: an index that is not an AHI, in Home Assistant

**Status:** Proposed
**Date:** 2026-09-06
**Repo:** `hms-cpap` — the MQTT layer only
**Version:** target TBD (Albin's call)
**Depends on:** SDD-081's `SessionMetrics::index_kind`, already shipped in parser
`v2026.8.1` and pinned since hms-cpap `v5.1.4`
**Related:** SDD-022 (`format`), SDD-005 (MQTT discovery)

## Trigger

The S.Box owner who opened issue #28, asked what he actually wants:

> I'd love to be able to get useable data into home assistant and the
> possibility of overlaying data from other devices like smart rings

He can now select his machine (SDD-022) and his card parses. What reaches Home
Assistant is wrong.

## What happens today

`index_kind` is honoured in **exactly one place in the entire codebase**:

```
src/services/BurstCollectorService.cpp:2539   the LLM summary prompt
```

Nowhere in the MQTT path. `publishHistoricalState` publishes unconditionally:

```cpp
// src/services/DataPublisherService.cpp:436
mqtt_client_->publish("cpap/" + device_id_ + "/historical/ahi",
                     std::to_string(m.ahi), 0, true);
```

and declares the sensor unconditionally:

```cpp
// src/services/DataPublisherService.cpp:210
{"ahi", "events/h", "", "mdi:alert-circle-outline"},
```

So an S.Box user gets a Home Assistant entity **named AHI**, carrying an
apnea-only number, with nothing anywhere saying it is not one.

### Why this is worse than the LLM case

The LLM case was a sentence in one summary. This is an entity. It lands in the
recorder database, it gets put on dashboards, it gets used in automations, and
the label travels with it forever. A number that cannot be compared against
ResMed thresholds will be compared against them, because it is called AHI and
sits next to a history graph.

It also under-reports by construction: the S.Box marks apneas and does not mark
hypopneas in any readable form, so the number is systematically LOWER than the
AHI the same night would score on a ResMed. A user watching it go down has no
way to know they are watching half a measurement.

## Decision

Two changes, and the second is the one that is easy to leave out.

### 1. A sensor of its own

```
cpap/<device>/historical/apnea_index        the apnea-only index
homeassistant/sensor/<device>/hist_apnea_index/config
```

Its own name, its own icon, its own unique_id. Not a renamed `ahi`: a user who
switches machines must see a new entity appear, not a familiar one change
meaning underneath them.

### 2. `ahi` is WITHHELD when the index is not an AHI

Adding the new sensor without withholding the old one achieves nothing. The
dashboard card the user already has still says AHI and still shows the same
number, and nothing about their experience changes.

**Withholding is not simply skipping the publish.** Both the discovery config
and the state are published RETAINED:

```cpp
mqtt_client_->publish(discovery_topic, config_json, 1, true);   // retained
mqtt_client_->publish(state_topic, value, 0, true);             // retained
```

A retained message survives until it is replaced or cleared. So an install that
has ever published `ahi` keeps that entity in Home Assistant, showing its last
value, forever — including an existing ResMed user who moves to an S.Box, which
is exactly the person most likely to misread it. Skipping the publish leaves the
stale number on their dashboard looking current.

Clearing a retained topic means publishing an EMPTY payload to it, retained:

```
publish("homeassistant/sensor/<device>/hist_ahi/config", "", 1, true)
publish("cpap/<device>/historical/ahi",                  "", 0, true)
```

The discovery clear is what removes the entity; the state clear is belt and
braces for anything still subscribed.

### The rule

| `index_kind` | `ahi` | `apnea_index` |
|---|---|---|
| `AHI` | published | cleared |
| `Ungraded` | **cleared** | published |

Symmetrical on purpose. A user going the other way — S.Box to ResMed — must not
be left with a stale `apnea_index` entity either.

## Where the code goes

`publishHistoricalState(const SessionMetrics& m)` already receives the whole
`SessionMetrics`, so `m.index_kind` is in scope at the exact line that needs it.
No signature changes, no plumbing.

- `src/services/DataPublisherService.cpp:210` — add `apnea_index` to
  `historical_sensors`. The count in the log line ("31 historical sensors")
  becomes 32 and must be updated with it; it is asserted in tests.
- `src/services/DataPublisherService.cpp:436` — branch on `m.index_kind`,
  publish one and clear the other.
- A small helper for the clear, because doing it in two places by hand is how
  one of them gets forgotten.

## Naming

`apnea_index`, not `ai`. `AI` already means something else in this product
(the LLM summaries), and an entity called `sensor.cpap_ai` in a Home Assistant
autocomplete list is a trap. Not `sefam_index` either: the property is
"apneas only, no hypopneas", which is a property of the DATA and not of the
vendor. If another machine ever reports the same shape, this sensor already
fits it — the same reason SDD-022 put the axis on `format` rather than on a
vendor name.

## Tests

- `index_kind == AHI` publishes `ahi` and clears `apnea_index`.
- `index_kind == Ungraded` publishes `apnea_index` and clears `ahi`.
- The clear is an EMPTY RETAINED payload on both the discovery and state topics,
  not an absent publish. This is the assertion that actually protects the user;
  a test that only checks "ahi was not published" passes against the broken
  behaviour.
- Discovery declares 32 sensors.

Existing MQTT suites (`MqttClientTest`, `STRMqttIntegrationTest`) are excluded
from the coverage run because they need a live broker, so these go against the
mock publisher in `test_DataPublisherService.cpp`, which is where the other
discovery assertions already live.

## Out of scope

- **The cloud, which is already done.** Checked, not assumed: `63e7071` landed
  the S.Box work in `hms-cpapdash-api`. `index_kind` is written by
  `ParsingService`, read back with `COALESCE(index_kind, 'ahi')` in
  `QueryController`, served by `SessionController` and `AdminController`, and
  consumed by the cloud dashboard. The alert rule is gated:

  ```cpp
  if (prefs.notify_severe_ahi && n.ahi_is_gradable && n.ahi > 10.0)
  ```

  What remains open there is that rule's own noted "Slice C": a Sefam user's
  settings screen still offers a severe-AHI toggle that can never fire. A dead
  switch, not a wrong number. Cloud repo, cloud deploy, not this SDD.

- **This repo's Angular dashboard**, which still renders the index in the AHI
  position. Same defect as the MQTT one and the same fix shape, but a different
  surface with different tests; worth its own slice rather than smuggling it in
  here.
- **Grading the index.** There is nothing to grade it against. When there is,
  `index_kind` gains a value and this sensor gains a `device_class` — the
  reason the index is computed and stored rather than withheld.
