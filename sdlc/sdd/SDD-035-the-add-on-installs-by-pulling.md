# SDD-035: the add-on installs by pulling

**Status:** Accepted 2026-09-15 (§4): D1 built in `hms-cpap`'s existing
`docker-build.yml`, D2 the add-on pulls the exact version, D3 no Docker Hub
mirror. He asked for it: "lets create a image in ghrc and waays to count
installs".
**Date:** 2026-09-15
**Repos:** `hms-cpap` (the workflow that publishes images),
`hms-cpap-ha-addon` (the add-on's `config.yaml` and `Dockerfile`).

## Trigger

Albin asks how many add-on installs there are, and asked for a GHCR image.
Both were worth checking rather than assuming, and one of the two is already
done.

## 1. What is true today (verified 2026-09-15)

- **The heavy image already exists and is already pulled.** The add-on's
  `Dockerfile` is `FROM ghcr.io/hms-homelab/hms-cpap:5.2.11`, and
  `docker-build.yml` pushes that image as an OCI index over `linux/amd64` and
  `linux/arm64` on every `v*` tag. A user installing the add-on pulls the
  service image; nothing compiles on their box.
- **What the Supervisor still does per install**: builds a thin layer over it,
  which is one `apt-get install jq` and a `COPY run.sh`. It needs the network
  and a working apt mirror at install time, and it is the only reason an
  install can fail after the pull succeeds.
- **The add-on declares `amd64` and `aarch64`** and no `image:` key, which is
  what makes the Supervisor build that layer locally.
- **GHCR cannot count installs.** `GET /orgs/hms-homelab/packages/container/
  hms-cpap` and its `/versions` return `created_at`, `html_url`, `id`,
  `metadata`, `name`, `updated_at`, `url` and nothing about downloads or
  pulls. Docker Hub publishes a pull counter; GitHub does not.
- **Home Assistant does count them, publicly.**
  `https://analytics.home-assistant.io/addons.json` carries every add-on by
  slug, custom repositories included. On 2026-09-15 `e45c2a8b_cpapdash` read
  `total 7`, versions 5.2.4 (2), 5.2.5 (3), 5.2.6 (1), 5.2.7 (1),
  `protected 7`, `auto_update 3`. The `e45c2a8b` half is a hash of the
  repository URL, so every install of this add-on reports under one key. It
  counts only instances that opted into HA analytics, so it is a floor.

## 2. Design

### 2.1 Publish the add-on's own image (D1)

`docker-build.yml`, on the same `v*` tag that already publishes the service
image, builds the add-on layer too and pushes it per architecture:

```
ghcr.io/hms-homelab/amd64-cpapdash:<version>
ghcr.io/hms-homelab/aarch64-cpapdash:<version>
```

The Dockerfile is the add-on repo's, unchanged, with its `FROM` pinned to the
version being released. Both packages are public, like the service image.

### 2.2 The add-on points at it

`config.yaml` gains `image: ghcr.io/hms-homelab/{arch}-cpapdash`, which the
Supervisor expands per architecture and pulls at the tag matching `version:`.
Install and update become a pull: no apt, no build, nothing to fail
afterwards.

**The order is the whole risk.** A `version:` that names a tag with no image
is an add-on nobody can install or update. So the version bump commit lands
only after both images are pushed and verified present, and the workflow that
bumps the add-on repo does that check first.

**The way back** is one line: remove `image:` and the Supervisor builds
locally again from the Dockerfile that stays in the repo.

### 2.3 Counting installs

Nothing here counts anything, and no telemetry is added. The number comes from
Home Assistant's own analytics (§1), which needs nothing from us:

```bash
curl -s https://analytics.home-assistant.io/addons.json |
  python3 -c "import json,sys; print(json.load(sys.stdin)['e45c2a8b_cpapdash'])"
```

Worth a line in the release checklist so the number is read at each release,
and it is already in the assistant's memory so it stops being a question.

## 3. What this does not do

Add an architecture. `armv7`/`armhf` stays out: the service image is not
published for it (the workflow builds it as a preflight and does not push),
and the add-on does not declare it. Nothing about the service, the data or the
UI changes.

## 4. Decisions (Albin's)

- **D1 where the add-on image is built. Answered 2026-09-15: "yes to the
  build on the currect docker yaml".** `hms-cpap`'s existing
  `docker-build.yml`, beside the service image, so one tag produces
  everything and the bump can verify both.
- **D2 the tag the add-on pulls. Answered: the exact version**, never
  `latest`. `version: 5.2.14` pulls `{arch}-cpapdash:5.2.14`, so what the user
  sees is what they get and a rollback is pointing at the older tag.
- **D3 Docker Hub as well. Answered: no.** Its counter counts every CI job,
  mirror and retry, which is a different question from how many people run
  this. Home Assistant's analytics answers that one and costs nothing.

## 5. Tests

- The workflow, on a tag: both `{arch}-cpapdash` packages exist at that
  version and are public; `docker manifest inspect` says the platform is what
  the name claims.
- An install proof on a real Supervisor (Albin's HA at 192.168.2.7): the
  add-on updates by pulling, the log shows no build step, and the page loads
  through Ingress afterwards.
- The bump guard: with an image deliberately missing, the workflow refuses to
  bump `version:` and says why.

## 6. Release

The add-on repo's bump is its own release, on the tag after this lands.
