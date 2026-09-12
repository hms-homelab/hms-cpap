# SDD-021: hms-cpap as a Home Assistant add-on

**Status:** Proposed
**Date:** 2026-08-26
**Repo:** `hms-cpap` (the Ingress fix and the CI change) plus a new add-on repository
**Version:** target TBD (Albin's call)
**Depends on:** nothing
**Related:** SDD-019 and SDD-020 add entities to it, neither blocks it

## Trigger

The Home Assistant answer to CPAP data today is the prestomation myAir integration:
eight sensors, pulled from ResMed's cloud, once a day, of numbers ResMed chose to
publish. hms-cpap reads the card directly and already speaks MQTT discovery fluently.
The gap between what we publish and what an HA user can install is packaging, not
capability.

## What already exists, and must not be rebuilt

Every one of these already publishes `homeassistant/<platform>/<device>/<id>/config`
and its state topic:

- `src/mqtt/DiscoveryPublisher.cpp`: sensors and binary sensors, including power failure
- `src/services/DataPublisherService.cpp`: realtime, historical, daily, period summaries,
  therapy insights, oximetry, plus a `homeassistant/status` subscription so entities
  come back after an HA restart
- `src/services/SupplyPublisher.cpp`: per-supply sensors and a supplies-due binary sensor
- `src/services/CleaningPublisher.cpp`
- `src/services/LiveSleepStageRunner.cpp`: live sleep stage

There is also `dashboards/cpap-monitor.yaml`, which is a Lovelace dashboard that has
been sitting in the repo waiting for an install path.

So the add-on adds no entities. It adds an install button.

## The blocker: Ingress breaks the web UI

Home Assistant serves an add-on's UI under `/api/hassio_ingress/<token>/`. Our Angular
app assumes it lives at the root:

- `frontend/src/index.html` line 6 is `<base href="/">`
- there are 40 root absolute `'/api/...'` call sites in `frontend/src`

Under Ingress the page would load its bundle (Ingress rewrites nothing, the browser
resolves `/main.js` against the host) and then every single API call would 404 against
Home Assistant's own `/api`. This is not something the add-on wrapper can paper over. It
is a change in hms-cpap.

### The fix, both halves

**Base href, server side.** Home Assistant sends `X-Ingress-Path` on every proxied
request. `include/web/IngressBase.h` rewrites `<base href="/">` to
`<base href="{ingress_path}/">`, and `src/main.cpp` calls it from the two places that
serve `index.html`: a registered handler for `/`, and the 404 SPA fallback via the
`setCustomErrorHandler` overload that takes the request.

**Post-handling advice does not work here, and this was measured rather than assumed.**
A `registerPostHandlingAdvice` hook looked like the tidy answer, since one hook after
everything would catch both. With a debug line inside it, a request to
`/api/capabilities` logged and requests to `/` and to `/sessions` did not: Drogon's
static file router and its custom error handler both sit outside the controller pipeline
that advice hangs off. Two explicit hooks it is.

Two smaller traps found the same way. `resp->getHeader("content-type")` returns EMPTY in
Drogon, because the content type lives in its own member and is only serialised on the
way out; `contentTypeString()` is the one that answers. And a base href must end in a
slash, or the browser treats the last segment as a file name and drops it, resolving
every relative URL one level too high.

**The header is untrusted input.** hms-cpap is routinely reachable on a LAN with nothing
in front of it, so anyone who can reach the port can send `X-Ingress-Path`, and
reflecting it into an HTML attribute unchecked is an injection. `isSafePath` accepts only
a non-empty absolute path of at most 512 unreserved-URL characters and `/`, refusing
quotes, angle brackets, whitespace, backslashes, control characters, non-ASCII, `..` and
`//`. A rejected value leaves the page exactly as shipped.

**API calls, client side.** One `HttpInterceptor` that prefixes any request starting
with `/api` with `document.baseURI`. One file, and the 40 call sites stay as they are.
That matters: the same build has to keep working unchanged for plain Docker, the
desktop app (SDD-005) and the native service, where `baseURI` is `/` and the interceptor
is a no-op.

Both halves are testable without Home Assistant: curl with an `X-Ingress-Path` header,
and a unit test on the interceptor.

## Distribution

A **separate public repository**. Home Assistant requires `repository.yaml` at the root
of the repo and each add-on in its own subdirectory, and users add it under Settings,
Add-ons, three dots, Repositories. HACS is not an option and never will be: HACS
distributes integrations, cards and themes, and explicitly not add-ons. The Baby Tracker
add-on already works this way and that layout is the precedent.

Note for anyone reading the HA UI: 2026.2 renamed "Add-ons" to "Apps" in the interface
only. `config.yaml`, `repository.yaml`, Ingress, the Supervisor API and everything else
developer facing is unchanged.

## The image

`config.yaml` references an image with an `{arch}` placeholder, one tag per
architecture. CI today builds `linux/amd64,linux/arm64` into a single multi arch
manifest under one tag (`.github/workflows/docker-build.yml`, `docker/metadata-action`
plus `platforms:`). That manifest cannot be named per arch, so the workflow gains a
second push of per-arch tags: `ghcr.io/aamat09/hms-cpap-amd64` and
`ghcr.io/aamat09/hms-cpap-aarch64`, same build, same cache.

`arch:` is limited to `aarch64` and `amd64`. That covers 64 bit Pi 3/4/5, x86 boxes,
VMs, Green and Yellow, which is effectively every real install. armv7 is not on the
table: we would be cross building Drogon, libpqxx, libhpdf and the rest on Debian trixie
for it.

The alternative, letting the Supervisor build from source, is rejected on sight. Our
Dockerfile compiles a C++ service and an Angular bundle. On a Pi that is not an install,
it is an afternoon.

The add-on's own Dockerfile is `FROM` our published image plus `bashio` and `run.sh`,
rather than the HA Python base image. Our runtime is Debian trixie with Drogon from apt;
rebuilding that on the HA Alpine base is not a packaging tweak.

## config.yaml

```yaml
name: CpapDash
slug: cpapdash
version: "<matches the hms-cpap release>"
arch: [aarch64, amd64]
image: ghcr.io/aamat09/hms-cpap-{arch}
init: false
ingress: true
ingress_port: 8893
panel_icon: mdi:sleep
panel_title: CpapDash
hassio_api: true
homeassistant_api: true
services: ["mqtt:want"]
map: ["addon_config:rw", "share:rw"]
watchdog: "http://[HOST]:[PORT:8893]/health"
ports:
  8893/tcp: null          # optional direct access, Ingress does not need it
options: ...
schema: ...
```

`/health` already exists (`CpapController`, `ADD_METHOD_TO(... "/health" ...)`) and
SDD-016 already made it answer "can this be used" rather than "is a process alive",
which is exactly what a watchdog should be pointed at.

`mqtt:want` rather than `need`: hms-cpap is useful with its own web UI even if the user
has no broker, and refusing to start without one would be wrong.

`share:rw` is for `local_dir`. A user whose card contents land on a network share
mounted by HA needs us to be able to see it. The ezShare and Fysetc sources are network
protocols and need no device mapping at all, which is the reason this packages cleanly
in the first place.

## run.sh

`#!/usr/bin/env bashio`, and its whole job is translating add-on options into the
configuration hms-cpap already understands:

- `bashio::config` for each option, written into `/config/config.json` (or exported as
  the matching env var, which `AppConfig` already reads for every field)
- `bashio::services mqtt 'host'`, `port`, `username`, `password` so the broker is
  discovered rather than typed
- default database is SQLite under `/data`, so a first install needs no Postgres, no
  MySQL and no decisions. The existing engines stay available for anyone who wants them.
- `exec` the binary, so signals reach it and the Supervisor's stop is a clean stop

The options list is the short one: source (`ezshare` / `local` / `fysetc`), the source
URL or directory, burst interval, database choice, and the SleepHQ and myAir blocks if
those are enabled. Everything else stays in the settings page, which SDD-012 already
made complete.

## Init and the first run

`init: false` because our image has its own entrypoint and does not carry s6-overlay.
The setup wizard (SDD-006) is reachable through Ingress the moment the add-on starts,
and the run.sh option translation means most users will find it already configured. The
one thing to verify on real hardware is that `/config` and `/data` permissions satisfy
the writability preflight, since a bind mounted `/config` that is not writable is
already a known way to make hms_cpap refuse to boot.

## What the HA user gets that they cannot get today

Per-night AHI, usage, leak, mask pressure, events, supplies, cleaning schedule, live
sleep stage and realtime session state, all from the card, on their own hardware, with
no cloud account. With SDD-020 on top, their myAir numbers land in the same place, next
to ours, from the same add-on.

## Open questions

1. Does the add-on version track the hms-cpap version exactly, or float? Tracking is
   simpler and means one tag drives both, but it makes every hms-cpap patch release an
   add-on release.
2. Ingress and the file upload page. `setClientMaxBodySize` is 512 MB and Ingress
   proxies through Home Assistant; a multi hundred megabyte card zip through that proxy
   needs testing, not assuming.
3. Whether to publish the add-on repo under `hms-homelab` or as a CpapDash-branded repo,
   given it is the front door for people who have never heard of us.
4. The Lovelace dashboard in `dashboards/`: ship it in the add-on repo as a copy-paste
   snippet, or leave it where it is?

## Out of scope

- HACS. It does not host add-ons.
- A custom integration (`custom_components/`). MQTT discovery already gives HA every
  entity, and a Python integration would be a second implementation of what
  `DataPublisherService` does today.
- The desktop app and the native installers. Unaffected by everything here, provided the
  Ingress fix stays a no-op when the header is absent.
