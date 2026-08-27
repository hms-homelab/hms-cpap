# SDD-020: What myAir already knows

**Status:** Proposed
**Date:** 2026-08-26
**Repo:** `hms-cpap`
**Version:** target TBD (Albin's call)
**Depends on:** SDD-019 (there is no index to compare until that ships)
**Reference:** `github.com/prestomation/resmed_myair_sensors`

## Trigger

ResMed already scores every night, on their own servers, from data their own machine
uploaded over its own modem. The myAir app shows the user a number. We show the user a
different number, computed from the SD card, and neither of us shows the other.

The prestomation Home Assistant integration proves the myAir API is reachable and has
been reachable for years. It pulls a 30 day window per account and exposes eight
sensors. We can pull the same window and do something it cannot: hold ResMed's numbers
next to ours, per night, per component.

## What the API actually gives

`query GetPatientSleepRecords`, `sleepRecords(startMonth, endMonth).items[]`:

`startDate`, `totalUsage` (minutes), `sleepScore`, `usageScore`, `ahiScore`,
`maskScore`, `leakScore`, `ahi`, `maskPairCount`, `leakPercentile`,
`sleepRecordPatientId`.

`query getPatientWrapper`:

`masks[].maskCode`, and `fgDevices[]` with `serialNumber`, `localizedName`,
`deviceSeries`, `deviceFamily`, `lastSleepDataReportTime`, `fgDeviceManufacturerName`,
`fgDevicePatientId`.

Two things are worth noticing.

**The sub-scores come down separately.** We do not have to guess how ResMed weights
usage against mask seal against events. `usageScore`, `maskScore`, `ahiScore` and
`leakScore` arrive as four numbers alongside the total, so the comparison can be per
component and the weighting can be measured off real nights instead of assumed.

**The device identity comes down too.** Today hms-cpap knows the machine only as
whatever the card and the config say. `serialNumber`, `localizedName` and `maskCode`
are facts from ResMed's own record of this patient, and the equipment profile (SDD-004)
currently asks the user to type them.

The window is 30 days. There is no history endpoint. Whatever we want to keep, we keep
ourselves.

## Authentication

Okta, authorization code with PKCE. Four steps, all of them plain HTTPS:

1. `POST https://{okta}/api/v1/authn` with username and password. Returns either
   `SUCCESS` with a `sessionToken`, or `MFA_REQUIRED` with a `stateToken` and an email
   factor id.
2. If MFA: `POST /api/v1/authn/factors/{factor}/verify?rememberDevice=true` with an
   empty `passCode` to make Okta send the email, then the same call with the code the
   user read out of their inbox. Returns the `sessionToken`. The `DT` cookie that comes
   back is the remembered device, and it is the difference between asking for a code
   once and asking for a code forever.
3. `GET /oauth2/{server}/v1/authorize` with the `sessionToken`, `prompt=none`,
   `response_mode=fragment`, `code_challenge` = S256 of a random verifier, redirects
   disabled. The authorization code arrives in the **fragment** of the `Location`
   header, which is the one non-obvious part of the whole flow.
4. `POST /oauth2/{server}/v1/token`, form encoded, with the `code_verifier`. Returns
   `access_token` and `id_token`.

GraphQL then wants `x-api-key` (a per-region constant), `Authorization: Bearer`, and a
set of `rmd*` headers of which only one is dynamic: `rmdcountry`, which is the
`myAirCountryId` claim read out of the `id_token` **without verifying the signature**.
We are not the audience of that token; we are reading a claim out of a token we were
handed.

Two regions exist, NA and EU, differing in Okta host, auth server id, client id, api
key, GraphQL host and `rmdproduct` string. Everything else is identical.

### Nothing new has to be linked

libcurl, jsoncpp and OpenSSL are already dependencies. PKCE is a SHA-256 and two
base64url encodes. The `id_token` claim is a base64 decode of the middle segment. There
is no JWT library to add and no OAuth library to add.

## Where it lives

`MyAirClient` beside `SleepHqClient` in `src/services/`, and a `MyAirService` that owns
the poll loop, exactly as `SleepHqExportService` owns its own.

Configuration follows the `AppConfig::SleepHQ` precedent: a `myair` block in
`config.json` with env overrides (`MYAIR_ENABLED`, `MYAIR_REGION`, `MYAIR_USERNAME`,
`MYAIR_PASSWORD`), plus a persisted `device_token` so the MFA cookie survives a restart.

**This stores the user's real ResMed password, not a scoped client secret.** That is a
genuine difference from the SleepHQ integration and it should be stated in the settings
UI rather than buried. It is the user's own account, on the user's own machine, in a
file they own, and it is off by default. Anything stronger (an OS keychain, an
encrypted blob) is a bigger change than this SDD and applies equally to
`sleephq.client_secret`, so it belongs in its own spec.

## Storage

New table `cpap_myair_records`, three engines, created in `DatabaseService` alongside
the rest:

```
record_date         DATE PRIMARY KEY (per account)
total_usage_min     FLOAT
sleep_score         INT
usage_score         INT
ahi_score           INT
mask_score          INT
leak_score          INT
ahi                 FLOAT
mask_pair_count     INT
leak_percentile     FLOAT
fetched_at          TIMESTAMP
```

Upsert on `record_date`, because the same 30 day window comes back on every poll and a
recent night can change after ResMed reprocesses it.

**myAir numbers never touch `cpap_daily_summary`.** Not as a backfill, not for a night
the card missed, not "just the AHI". That table is what our parser read off the card,
its provenance is the whole reason the numbers can be trusted, and one row of somebody
else's arithmetic in it would poison every comparison this SDD exists to make. The
tables join on date for display and that is all.

Device metadata (`serialNumber`, `localizedName`, `maskCode`) is a different case: it is
identity, not measurement, and it can prefill the SDD-004 equipment profile. Prefill,
with the source shown, never a silent overwrite of something the user typed.

## The comparison

Per night, per component, because the fields line up almost one to one:

| Ours | Theirs |
|---|---|
| `patient_hours` | `totalUsage / 60` |
| `ahi` | `ahi` |
| `leak_95` | `leakPercentile` |
| `mask_events` | `maskPairCount` |
| CpapDash index (SDD-019) | `sleepScore` |

`GET /api/myair/compare?from=&to=` returns both sides plus the delta, and a page renders
it. The two composite numbers are built from different weights and will not agree; that
is expected and the page should say so rather than imply one is wrong.

The value is not the delta, it is the delta **changing**. A stable offset between two
formulas is noise. A night where our AHI and theirs diverge sharply is a signal, and it
usually means one of a small number of real things:

- the card is missing a session the machine's modem already uploaded, which is the
  single most useful thing this feature can tell a user;
- the night boundary differs. We have been here before: a night is not a session, a
  roughly one hour file gap splits it, and SDD-057 exists because a night belongs to a
  timezone. myAir's `startDate` is ResMed's opinion of which day a night belongs to and
  it will not always match ours.
- the machine reprocessed and ResMed's number moved after the fact.

## Home Assistant

`MyAirService` publishes discovery for its own sensors, reusing the
`DataPublisherService` pattern, so a user running the SDD-021 add-on gets the myAir
entities and the local ones from one add-on, and does not need the prestomation
integration installed alongside. Same eight-ish sensors, plus the deltas, which is the
part they cannot have.

## Risk, stated plainly

This is an undocumented API with hardcoded client ids and api keys. ResMed can change it
any morning with no notice, and prestomation's own README says exactly that. Therefore:
off by default, failures degrade to "myAir unavailable" and never block a sweep or a
parse, and nothing downstream is allowed to depend on it. The parse of the card is the
product; this is a second opinion on it.

## Open questions

1. **MFA UX in a headless service.** The email code needs a human at the moment of
   login. First setup can prompt in the settings page, and `rememberDevice` should hold
   after that. What happens when the cookie eventually expires, on a box nobody is
   looking at, is not designed yet. A notification and a disabled-until-reauthorised
   state is the obvious answer.
2. **Poll cadence.** myAir updates once a day. prestomation polls every 30 minutes.
   Hourly, plus a manual refresh button, is probably right, but it should be a setting.
3. **Multi-device accounts.** `fgDevices` is a list. hms-cpap is single-machine today.
   Take the first, or make the user pick during setup?
4. ~~**AU accounts.**~~ **Settled.** ResMed serves Australia from the North America
   endpoints. prestomation's own region picker is labelled "North America and
   Australia" against `REGION_NA`, and offers no third option, which is why their README
   can claim three regions from two configs. Our picker now says the same thing, because
   a user in Sydney has no way to guess that "North America" is the right answer.

   Their second label is worth copying too: "Europe (Email MFA)". Europe is the region
   with an email verification step, and saying so in the picker stops the code prompt
   being a surprise.

## Out of scope

- Writing anything back to myAir. Read only, permanently.
- Pulling myAir into hms-cpapdash-api. Same client would work there, and the cloud has a
  better place to keep credentials, but that is a separate spec against a different
  threat model.
- Replacing the card as a data source. See the storage section.
