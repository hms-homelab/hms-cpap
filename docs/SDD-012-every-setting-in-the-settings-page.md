# SDD-012: Every setting, for every mode, in the Settings page

**Status:** Proposed
**Date:** 2026-08-05
**Repo:** `hms-cpap`
**Version target:** unassigned
**Depends on:** SDD-005 (desktop app, supervised restart), SDD-006 (first-run wizard)
**Reported by:** Ken Narod (omken1@gmail.com), support thread 2026-08-03/05

## Trigger

Ken bought a Mule and Miner, completed the wizard, and asked how to make
hms-cpap write into the folder OSCAR reads:

> "hms-CPAP writes to `C:\Users\kendn\.hms-CPAP` but my OSCAR database is at
> `C:\Users\kendn\OneDrive\Documents\OSCAR20_Data`. Is this the problem with
> the auto data upload to OSCAR? How do I point the program at the OSCAR
> database?"

The setting he needs is `archive_dir`. The wizard collects it
(`setup.component.ts:924`, outside the source branches, so every mode gets it).
The Settings page does not expose it at all. Ken has already finished the
wizard, so the only field that matters to him is unreachable without editing
`~/.hms-cpap/config.json` by hand or setting `CPAP_ARCHIVE_DIR`.

Auditing the rest found the same shape 25 more times.

## Current state

### Defect 1: 26 of 63 config keys have no Settings field

`AppConfig` (`include/utils/AppConfig.h`) defines 63 user-settable keys.
`settings.component.ts` binds 37. Missing:

| Group | Missing keys |
|---|---|
| `fysetc.*` | enabled, listen_port, listen_bind, connection_timeout_s, archive_dir, log_dir |
| `agent.*` | enabled, embed_model, temperature, max_iterations |
| `cpapdash.*` | enabled, api_url, token, auto_sync |
| `sleep_stage.*` | enabled, live_inference, model_dir, model_version |
| top level | archive_dir, web_port, static_dir |
| `llm.*` | max_tokens, prompt_file |
| `mqtt.*` | client_id |
| `sleephq.*` | quiet_minutes |

`setup_complete` is excluded deliberately: it is an internal state flag, not a
user setting.

### Defect 2: selecting Fysetc strands the user

The Settings source dropdown offers three values:

```html
<option value="ezshare">ezShare WiFi SD
<option value="local">Local Directory
<option value="fysetc">Fysetc
```

Choosing `fysetc` writes `source: "fysetc"`, which `main.cpp:152` honours. But
none of the six `fysetc.*` settings has a field, so the listen port, bind
address and archive directory cannot be set. The user has switched their data
source to a mode they cannot configure, and cannot see why nothing arrives.

### Defect 3: the Settings page cannot restart, and does not say so

`PUT /api/config` writes the file and re-applies exactly two things:

```
:432   if (burst_service_) burst_service_->markConfigDirty();
:439   if (sync_) sync_->setSettings(...)      // cpapdash.*
```

Everything else reaches the file and the in-memory config, and never the
running service. The controller already documents the consequence at `:437`:

> "The wizard's restart hid this; the Settings page does not restart."

So a user changes their MQTT broker, sees "saved", and nothing happens. There
is no error and no hint that a restart is owed.

The machinery to fix this already exists and is already tested.
`SetupService::restartMode(bool supervised, exe_path)` returns:

| Mode | Behaviour |
|---|---|
| `SupervisedExit` | `HMS_CPAP_SUPERVISED=1`: exit 0, the SDD-005 shell respawns |
| `ReExec` | standalone: `execv` ourselves |
| `Unsupported` | no executable path; tell the user honestly |

The only endpoint that uses it is `setupApply`, which is gated:

```
:541   if (!setupStillOpen(config_)) {
:542       cb(jsonError("Setup is already complete", k403Forbidden));
```

Post-setup, the restart path returns 403. **No new supervisor is needed. The
existing one is unreachable from Settings.**

## Design

### 1. Complete the config surface

Add the 25 missing keys (26 minus `setup_complete`) to `config.model.ts` and
`settings.component.ts`, following the existing collapsible-section pattern:

- `archive_dir` joins the Data Source section, next to Local Directory, and is
  shown for **every** source, not gated on `source === 'local'` the way
  `local_dir` is.
- Four new sections: **Fysetc**, **Agent**, **Sleep Stage**, **CpapDash Cloud**.
- Loose keys join their existing sections.
- A new **Advanced** section holds `web_port` and `static_dir`.

Backend delta is four keys `updateConfig` does not currently parse:
`static_dir`, `llm.max_tokens`, `llm.prompt_file`, `mqtt.client_id`. Every other
missing key is already accepted (`agent`, `sleep_stage`, `fysetc`, `cpapdash`,
`web_port`, `archive_dir` are all parsed today), so this is overwhelmingly a
frontend change.

### 2. Classify every setting as hot-reload or restart-required

Verified against `BurstCollectorService::ConfigSnapshot`
(`BurstCollectorService.h:480`) and `reloadConfig()`, not assumed. The snapshot
is the whole hot-reload surface: a key absent from it cannot be carried by
`markConfigDirty()` no matter what the controller does.

**Reloads live** (in the snapshot and handled by `reloadConfig()`):

`source`, `ezshare_url`, `local_dir`, `device_id`, `device_name`,
`burst_interval`, `database.*`, `mqtt.enabled/broker/port/username/password`,
`llm.enabled/provider/endpoint/model/api_key`, `o2ring.*`, plus `cpapdash.*`
via `sync_->setSettings()`.

`archive_dir` joins this list through a different route. It is absent from the
snapshot, and reaches consumers only as the `CPAP_ARCHIVE_DIR` environment
variable that `main.cpp:480` writes once at startup. `updateConfig` now
re-exports it on change, so it applies on the next burst.

**Requires restart** (absent from the snapshot entirely):

`web_port`, `static_dir`, `ezshare_range`, `llm.max_tokens`, `llm.prompt_file`,
`sleephq.*`, `agent.*`, `sleep_stage.*`, and the `fysetc.*` detail settings
(`listen_port`, `listen_bind`, `connection_timeout_s`, `archive_dir`,
`log_dir`). `reloadConfig()` handles starting and stopping the Fysetc server
when `source` changes, but never re-reads its settings.

### 2b. Two defects this verification exposed

Both are pre-existing, both are out of scope for this SDD, and both are
recorded here so they are not lost:

- **`mqtt.client_id` is applied but not detected.** `reloadConfig()` copies it
  into the rebuilt client (`:2541`), but the `if` that decides whether to
  rebuild at all (`:2531`) does not compare it. Changing only the client ID
  therefore does nothing until some other MQTT field also changes.
- **A database change hot-reloads only partially.** `reloadConfig()` rebuilds
  `db_service_` for the burst collector, but `main.cpp` hands the original
  `IDatabase` to five consumers and only this one is swapped. The wizard's
  claim that a DB change "CANNOT be hot-reloaded" is therefore closer to the
  truth than the collector's behaviour suggests, and a partial swap is arguably
  worse than none. Until this is resolved, `database.*` is flagged
  **restart-required** in the UI even though the collector does reload it.

### 3. Surface the restart honestly

- Fields that require a restart are marked in the UI.
- Saving a restart-required field raises a banner: what changed, that it is
  saved, and that it is not yet in effect.
- The banner offers **Restart now**, calling a new endpoint.
- After restarting, poll `/health` until the process answers, reusing the
  wizard's existing poll (`setup.component.ts:991`, ~40 attempts / 20s).
- When `restart_mode` is `Unsupported`, say so plainly instead of spinning:
  the process cannot restart itself and the user must do it. `setupApply`
  already models this exact response.

### 4. New endpoint: `POST /api/config/restart`

Ungated equivalent of the restart half of `setupApply`. Returns the same shape:

```json
{ "restarting": true, "restart_mode": "supervised|reexec|none", "message": "..." }
```

It restarts only. It does not provision databases or reopen any of the
setup-only surface, so relaxing this gate does not reopen SDD-006's provisioning
endpoints to a completed install.

### 5. `web_port` needs special handling

Changing the port from a page served on that port kills the connection the user
is using. Required behaviour:

1. An explicit warning on the field, stronger than the generic restart flag.
2. On save + restart, the UI computes the new origin, and once `/health`
   answers on the new port, navigates there with Settings still open, so the
   user lands back where they were rather than on a dead tab.
3. If the new port never answers within the poll window, say so and show the
   old and new URLs, since at that point the user may have to reach the box
   another way.

## Testing

- `SetupService::restartMode` is already covered; the new endpoint is a thin
  passthrough, consistent with the SDD-006 note that controller logic cannot be
  unit tested (the test binary excludes `src/controllers`).
- Backend: extend the `updateConfig` round-trip coverage to the four new keys,
  asserting a value written through the endpoint survives `save()` + reload.
- Manual: for each restart-required group, change a value, confirm the banner,
  restart, confirm the new value is live in the running service.
- Fysetc specifically: select it as the source and confirm the section appears
  and is configurable, closing Defect 2.

## Decisions

All decided by Albin on 2026-08-05.

1. **Secrets in new fields.** `cpapdash.token` keeps the existing `********`
   sentinel, matching `mqtt.password`, `llm.api_key`, `database.password` and
   `sleephq.client_secret`. **This is already implemented on both sides**
   (`AppConfig.h:625` redacts on read, `CpapController.cpp:401` ignores an
   incoming `********`), so the field is pure UI work and the real token is
   never sent to the browser.

2. **Restart scope.** **Restart now** is offered in the banner after a save that
   touched a restart-required field. Never automatic.

3. **`static_dir` exposure.** Editable, in the Advanced section, **behind a
   confirm**. The field shows the currently resolved path, and the confirm
   states plainly that a wrong value makes the web UI unreachable and recovering
   means editing `config.json` by hand.

4. **Fysetc visibility.** Shown **only when `source === 'fysetc'`**. Defect 2 is
   still closed by this: the stranding was that the settings did not exist at
   all, so selecting Fysetc left nothing to configure. With the section present,
   choosing Fysetc reveals it and the mode becomes configurable.

5. **Agent section gating.** Shown **disabled, with the reason stated** when LLM
   or PostgreSQL is missing, so the feature stays discoverable and the user
   learns what to turn on.

6. **One release or two?** One. `archive_dir` ships with the rest. Ken has the
   `config.json` workaround in the meantime and is waiting on convenience, not
   function.

7. **Version number.** Unassigned. Albin's call at tag time.
