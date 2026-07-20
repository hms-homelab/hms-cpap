# Privacy

**Short version: this software has no operator. It collects nothing, sends
nothing home, and has no analytics. Your data sits on your hardware. The only
ways data leaves your machine are integrations you switch on yourself, and each
one is listed below.**

This document describes what the software does, not a promise made by a service
provider, because there is no service provider. You run it, so you are the one
handling the data.

## There is no telemetry

HMS-CPAP contains no analytics, no crash reporting, no usage tracking, no update
pings, no license checks, and no phone-home of any kind. The authors of this
project cannot see your data, your usage, or even that you are running it.

There is no hosted HMS-CPAP service. Nobody receives your data by default.

## What is stored, and where

All of it stays on infrastructure you control:

| What | Where |
|---|---|
| Therapy sessions, metrics, summaries | Your database (SQLite, PostgreSQL, or MySQL) |
| Raw device files copied from the SD card | Your archive directory on your disk |
| Pulse oximetry readings | Your database |
| Equipment profiles and supply dates | Your database |
| Generated PDF reports | Your reports directory on your disk |
| Configuration, including any API tokens | `~/.hms-cpap/config.json` on your disk |

Nothing is encrypted at rest by the application. Anyone with access to that
machine, that disk, or a backup of either can read all of it. Treat the archive
directory and the database as sensitive health data, because that is what they
are. Full disk encryption is a reasonable idea.

**Tokens are stored in plaintext** in the config file. The API redacts them when
it returns settings, but the file itself is readable by anyone who can read it.
Restrict its permissions.

## Every way data can leave your machine

All of these are **off by default**. Each requires you to enable it and provide
credentials. This is the complete list.

### 1. Your MQTT broker
If MQTT is configured, therapy metrics, session state, and supply status are
published to the broker you name -- typically for Home Assistant. Whatever your
broker and its subscribers do with that data is between you and them. If your
broker is exposed to the internet or shared, so are these metrics.

### 2. SleepHQ export (`sleephq.com`)
If SleepHQ export is enabled, this uploads **complete therapy data files**,
including `STR.edf` and your machine identification files, to SleepHQ's servers.
This is a real transfer of your health data to a third party. Their handling of
it is governed by their own privacy policy and terms, not this project's. Review
them before enabling this.

### 3. CpapDash cloud sync (`api.cpapdash.com`)
If cloud sync is enabled with a token, **equipment profiles and supply
information** are mirrored to the CpapDash API. This syncs your equipment
catalog -- machine and accessory names, models, and replacement dates -- and not
your therapy or session data. It is opt-in, off by default, and local remains the
source of truth. CpapDash is a separate service with its own privacy policy and
terms.

### 4. LLM session summaries
If LLM summaries are enabled, **session data is sent to whatever endpoint you
configure**. The default provider is Ollama, which normally means a model running
on your own hardware, and in that case nothing leaves your network. **If you
point the endpoint at a hosted provider instead, your therapy data is sent to
that company** and becomes subject to their retention, logging, and training
policies. This is entirely your choice and your responsibility. Check whether
your provider trains on submitted data.

### 5. Your CPAP device and SD bridge
The service talks to your machine's WiFi SD card or bridge over your local
network. This traffic is unencrypted HTTP on your LAN. It does not leave your
network, but anyone on that network can observe it.

## No authentication by default

HMS-CPAP is designed for a trusted home network and **ships with no
authentication**. Anyone who can reach the web UI or the API can read all of your
therapy data and change your configuration.

**Do not expose this service directly to the internet.** If you need remote
access, put it behind a VPN, or behind a reverse proxy that enforces
authentication. Port-forwarding it publishes your health records.

## Deleting your data

There is no account and nothing to request. Stop the service, then delete the
database, the archive directory, the reports directory, and `~/.hms-cpap/`. Data
already uploaded to SleepHQ or CpapDash must be deleted through those services
under their own policies.

## If you run this for other people

The moment you run this for someone other than yourself, you are processing
another person's health data and you take on the corresponding legal obligations.
Depending on where you are and what you are doing, that may include HIPAA, the
GDPR or UK GDPR, or local health privacy law. **This project provides no
compliance features** -- no access controls, no audit logging, no encryption at
rest, no breach notification, no data processing agreement. Meeting those
obligations is entirely on you. See [DISCLAIMER.md](DISCLAIMER.md).

## Changes

This document describes the software's behavior at the version you are running.
Read `CHANGELOG.md` when upgrading, since a new release can add a new
integration. No update ever enables an outbound integration on your behalf.
