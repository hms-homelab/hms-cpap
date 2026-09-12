# SDD-017: Device controls

**Status:** Proposed
**Date:** 2026-08-23
**Repo:** `hms-cpap`
**Version:** 5.1.0 (restart / O2 / reset, local transport). Mode switch and the
cloud transport are later releases — see "The split".
**Depends on:** SDD-012 (settings + restart); for the later half,
hms-cpapdash-api SDD-065 (personal access token) and the hms-cpap sessions
push/pull spec
**Related:** hms-cpapdash-api `DeviceController` (SDD-010, SDD-019, SDD-028);
push-c3 SDD-006 (always-on local control plane — **not implemented**)

## Trigger

Ken Narod asked for two things that turn out to be one thing:

1. take the pain out of switching a device between cloud and local, and
2. let him stay in local mode permanently and push to the API from hms-cpap.

Both are the operating-mode switch. Today an owner changes it from the Angular
dashboard at cpapdash.com, by holding a button, or over USB. hms-cpap is the one
place that can reach a device *both* ways — it sits on the user's LAN AND already
authenticates to hms-cpapdash-api — so it is the cheapest place to put the
controls, and it ships without App Store review.

This spec covers hms-cpap only. Whether cpapdash-app also gets a Device section is
a separate decision, deliberately not settled here.

## What already exists, and must not be rebuilt

Both transports are already present, addressed and configured. That is the whole
reason this is cheap: the new work is a layer that picks one, plus a page.

**Local — `EzShareClient`** (`src/clients/EzShareClient.cpp`): a libcurl client
already pointed at the device, with `getBaseURL()` / `setBaseURL()`. Talking to
`/api/status` is the same client and the same host, a different path.

**Cloud — `CpapDashSyncService`** (SDD-004): owns an injected `Transport`
(`url, body -> out`, libcurl by default) and the `cpapdash` config block
(`enabled`, `api_url`, `token`, `auto_sync`). Its token is a `UserAuthFilter`
bearer — the *same* filter guarding every device-command endpoint. Verified:
`/v1/equipment/sync` and `/api/devices/{serial}/commands` both sit behind
`cpapdash::UserAuthFilter`. The credential hms-cpap already holds is the one these
controls need.

**Discovery — already solved in firmware.** `wifi_manager.c` registers
`_cpapdash._tcp` with the **instance named by serial** and TXT carrying
`serial`, `fw`, and `mode` ("proxy"/"cloud"). So hms-cpap can browse, disambiguate
multiple units, and know which transport applies *before* trying one.

**The cloud command surface, already user-authenticated.** No new backend work for
the commands themselves:

| Endpoint | Method |
|---|---|
| `/api/devices/{serial}/commands` | POST (send), GET (history) |
| `/api/devices/{serial}/status` | GET |
| `/api/devices/{serial}/logs/refresh`, `/logs` | POST, GET |

## Which transport, and why it is not a preference

`control_server_start()` in the mule's `main.c` runs **only** when
`op_mode == OP_MODE_EZSHARE_PROXY`. push-c3 SDD-006 would make the control plane
always-on, but it is still Draft. So today:

| Device mode | Local HTTP | Cloud queue |
|---|---|---|
| ezShare proxy ("local") | **yes** | no — device does not poll |
| cloud push | **no server at all** | **yes** — planner emits on long-poll |

The transport is *determined by the mode*, not chosen. hms-cpap must not offer a
transport toggle; it reads TXT `mode` from the browse and knows.

**One trap to design for.** mDNS registers in **both** modes and advertises the
control port either way, but nothing listens in cloud mode. TXT `mode` is the
signal not to try — never probe the port to find out.

**A second trap.** The mode switch is the one command that changes which transport
works. Switch a unit to cloud over local HTTP and the local server disappears
underneath you. **That is success, not an error**, and it must not be reported as
a failure. Confirmation comes from re-browsing mDNS until TXT `mode` flips (the
device reboots and re-registers), never from polling the endpoint you just killed.

The O2 toggle is the opposite and it is worth knowing why: it reboots the
**miner**, not the mule, so the mule's HTTP server stays up and `/api/status`
keeps answering. O2 state can be confirmed by polling; mode cannot.

## The command set

Four commands, all owner-tier in the API — authorized by device ownership alone,
no Authorized Remote Session. OTA and wifi/identity provisioning are **out of
scope** (Albin, 2026-08-23).

**Three of the four ship in 5.1.0.** The mode switch is held back until hms-cpap
can follow it — see "The mode switch changes where hms-cpap's OWN data comes
from".

| Command | Local (`/api/config`, flat) | Cloud (nested envelope) |
|---|---|---|
| Restart | `POST /api/reboot` | `{"cmd":"reboot"}` |
| Mode switch | `{"mode":"local"\|"cloud"}` | `{"cmd":"config","config":{"mode":…}}` |
| O2 ring | `{"o2_enabled":bool}` | `{"cmd":"config","config":{"o2_enabled":…}}` |
| Factory reset | `POST /api/reset` | `{"cmd":"reset"}` |

**The two shapes differ and the controls layer translates.** Local `/api/config`
takes flat keys (`mode`, `wifi_ssid`, `o2_enabled`); the cloud wants them nested
under `config`. Do not leak either shape into the UI.

**Current O2 state is readable**: `/api/status` reports `o2_enabled` from
`cloud_uploader_get_miner_o2_enabled()`, so the toggle is stateful, not
fire-and-forget.

Two API behaviours to carry over rather than rediscover:

- **Priority.** `reboot`, mode switch and O2 default to `high` so they preempt the
  sync walk (SDD-028 `nextPreemptiveCommand` only picks `high` or a reset).
  Without it the command queues on a device wedged mid-walk, never emits, and
  expires — the dead end on FE-C3-00000025 (ticket 74, command 297). Local mode is
  what an owner reaches for *because* cloud sync is broken, so it needs this most.
  `reset` is a barrier and flushes the queue behind it.
- **The confirm dialog is a contract, not polish.** SDD-019 (rev 2026-08-20)
  removed the 2-minute online-dwell gate, because a unit flapping every ~50s never
  accumulates 120s of uptime and the owner could never queue anything (ticket 74,
  Rohan on FE-C3-00000018). The API comment states the client's confirm dialog
  carries the warning instead. Angular has one; hms-cpap must too.

## The mode switch changes where hms-cpap's OWN data comes from

This is the constraint the rest of the plan bends around, and it is easy to miss
because the command looks like it only touches the device.

hms-cpap does not merely *control* the device, it *collects from* it.
`CPAP_SOURCE` selects how: `ezshare` (default), `local`, `lowenstein`, `fysetc`.
When the device is in ezShare-proxy mode, `BurstCollectorService` polls it and the
nights land in hms-cpap's own database.

Put that device into cloud-push mode and it stops serving files entirely. The
nights go to hms-cpapdash-api instead. hms-cpap's collector gets nothing, its
dashboard stops updating, and the numbers now live in the other system.

**There is no cloud source in hms-cpap.** `CPAP_SOURCE` has no such value, and
`CpapDashSyncService` is equipment-only and push-direction — there is no path that
reads sessions back from the API. So today a mode switch does not just change the
device, it silently blinds the product the user is looking at.

**So hms-cpap must follow the device**, and it can only follow in a direction it
has a source for:

| Switch | hms-cpap must become | Exists? |
|---|---|---|
| cloud → local (ezShare proxy) | `CPAP_SOURCE=ezshare` | **yes** |
| local → cloud push | `CPAP_SOURCE=cloud` | **no — does not exist** |

The mechanism for following already exists: `config_->source` is settable and
`POST /api/config/restart` (SDD-012) restarts the process so restart-required
settings take effect.

**Following is prompted, not automatic** (Albin, 2026-08-23): after a mode change
hms-cpap asks "this device now reports to X — switch hms-cpap to match?" and the
user confirms. A service restart is too much consequence to attach silently to one
button on a device page.

## The split

**5.1.0 ships the mode-NEUTRAL controls on the local transport: restart, O2
toggle, factory reset.** These three cannot blind hms-cpap, so they are safe to
ship against a source model that has no cloud value yet. This also de-risks the
page, the mDNS discovery and the command log before the hard part.

**The mode switch ships later, in both directions at once, after the cloud source
exists** (Albin, 2026-08-23). Shipping it earlier would mean shipping only
local→cloud — because a cloud-mode device has no local HTTP to reach — which is
precisely the direction that leaves hms-cpap blind. The one order in which the
switch is never destructive is: PAT, then a cloud source, then both directions.

Dependency chain:

1. **hms-cpapdash-api SDD-065** — personal access token (a credential that does
   not expire daily).
2. **SDD-018, sessions to and from the cloud** — paired push and pull. Pull is
   what creates `CPAP_SOURCE=cloud` and unblocks the mode switch; push is Ken's
   actual ask (below).
3. **This spec's mode switch** — both directions, with prompted following.

Cloud-mode units are **listed, with controls disabled and the reason shown**. Not
hidden: a user with one unit in cloud mode opening an empty Device section reads
that as a bug rather than a mode.

## What Ken actually asked for is not the mode switch

Worth stating plainly, because it reorders the value. Ken's ask was to "stay
connected in local mode all the time and push to the API through hms-cpap" — that
is not switching to cloud mode at all. It is staying in local mode and having
hms-cpap upload sessions, so he gets the cloud features without the device leaving
the mode he wants it in.

That is a session **push** path from hms-cpap to the API, and it does not exist:
`CpapDashSyncService` syncs equipment only. It is **SDD-018**, paired with the
pull path (Albin, 2026-08-23), and it needs SDD-065's token like everything else
here.

If that ships, Ken may never need the mode switch.

## The blocking problem for the cloud half: the credential

**Pre-existing bug in SDD-004, not introduced here.** SDD-004 chose "a pasted
long-lived TOKEN … no cloud password at rest and the user can revoke it". What
exists is a 24-hour user JWT:

- `JwtService::createToken` uses the configured `expiry_hours`;
- prod `/etc/hms-cpapdash-api/config.yaml` sets `expiry_hours: 24`;
- there is no personal-access-token or refresh mechanism in `JwtService.h`
  (`createDeviceToken` is for devices, not users);
- `CpapDashSyncService.cpp:324` treats 401 as "the pasted token was revoked or is
  wrong … the user keeps working offline until they repaste it", with no refresh.

So a user pastes a token, equipment sync works, and about a day later it silently
stops until they notice and paste another. **Equipment sync is broken this way
today**, independent of this spec.

**Decision (Albin, 2026-08-23): add a real personal access token to the API** —
long-lived, revocable, hashed at rest, accepted by `UserAuthFilter`. It honours
SDD-004's original intent, fixes equipment sync as a side effect, and does not
weaken web or phone sessions the way raising `expiry_hours` would. Its own spec,
in hms-cpapdash-api, and it gates the cloud half.

## The page

A **new Device section** in the Angular dashboard — not inside Settings (SDD-012).
Settings persist values; these fire actions with a lifecycle, and mixing the two
invites a user to think a queued reboot already happened.

**Units are a list; expand one to control it.** mDNS returns serial, fw and mode
for every unit on the LAN at no extra cost, so the list shows fleet state at a
glance and the controls stay unambiguously scoped to one expanded unit.

**Addressing: mDNS browse, with the existing manually-configured host as
fallback.** mDNS is routinely blocked on guest and VLAN networks and does not
cross subnets, and some users reach the card through the hms-mm bridge. The bare
`cpapdash.local` hostname collides across units, so it cannot be the only path
once a second M&M exists.

**One command log, both transports.** Every action lands in the same list with a
state. Local entries resolve immediately; cloud entries will move
queued→sent→acked when that half lands. This costs a little bookkeeping local HTTP
does not need, but the user learns one model and the cloud half drops in without
redesigning the page. **Queued is not applied** — a cloud UI that reports success
on a 202 is lying to someone whose device is offline.

**Factory reset gets a plain confirm dialog carrying the warning**, matching what
Angular does today and what SDD-019 expects of a client.

## Security posture

hms-cpap's web UI binds `0.0.0.0:8893` with **no authentication of any kind**.

For the local half this changes almost nothing: the device's own `control_server`
is *also* unauthenticated and already exposes reset, reboot and the O2 toggle to
anyone on the same LAN — its root page has the buttons. hms-cpap is not creating
reachability that is not already there.

**The cloud half is a genuine escalation** and is explicitly deferred, not ignored:
an unauthenticated local UI would become a proxy to a device that is otherwise
reachable only with an authenticated cloud account. Decided now, revisited when
the cloud half is built (Albin, 2026-08-23).

Authenticating the whole hms-cpap web UI is the actually-correct fix — therapy
data sits on `0.0.0.0` unauthenticated today — and deserves its own SDD.

## Decisions, all Albin's (2026-08-23)

| | |
|---|---|
| Credential | Personal access token in the API (SDD-065) |
| Split | 5.1.0 = restart / O2 / reset on local. Mode switch waits for the cloud source, then ships both directions |
| Commands | Restart, mode switch, O2 toggle, factory reset. **No OTA / support tier** |
| Mode following | Prompted, not automatic — hms-cpap asks before changing its own source and restarting |
| Session push/pull | Its own SDD, push and pull paired |
| UI | New Device section |
| Cloud-mode units | Listed, disabled, reason shown |
| Addressing | mDNS browse + manual host fallback |
| Reset guard | Plain confirm dialog with the warning |
| Multi-unit | List, expand one to control |
| LAN auth | Ship as-is for local; revisit for cloud |
| Feedback | One command log across both transports |

## Out of scope

- cpapdash-app's Device section (separate decision).
- OTA and wifi/identity provisioning (support tier, needs the remote grant).
- push-c3 SDD-006. If it ships, the transport table collapses to "local always
  available", which simplifies this design but does not block it.
- Authenticating the hms-cpap web UI (own SDD).
- Cloud AI features in hms-cpap (brief item 4, separate spec).
