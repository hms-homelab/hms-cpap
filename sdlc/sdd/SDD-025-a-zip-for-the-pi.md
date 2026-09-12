# SDD-025: a zip for the Pi

**Status:** Accepted 2026-09-08 (Albin: "a zip that's it")
**Date:** 2026-09-08
**Repo:** `hms-cpap`. One workflow job, one install script, one zip.
**Version:** 5.2.3
**Related:** SDD-016 (the supervisor, which this deliberately does NOT use on
Linux), SDD-021 (the Home Assistant add-on, which covers the 64-bit Docker case)

## Trigger

picpapdash2 is a Pi Zero 2 W on 32-bit Raspberry Pi OS. Installing hms-cpap on
it today means building from source on the Pi: apt the build dependencies,
rsync the tree, cmake, and about 35 minutes of `make -j2` with a swapfile
added so it does not fall over. The release page offers a macOS zip, a Windows
zip, a dmg, an exe, and a Docker image for amd64 and arm64. Nothing runs on a
32-bit Pi, which is the machine hms-cpap ran on for its first six months.

## What ships

One more release asset beside the macOS and Windows zips:

```
hms-cpap-linux-armhf.zip
  hms_cpap                  the service, built for armhf on Debian trixie
  static/browser/           the Angular UI, same build as every other asset
  hms-cpap.service          systemd unit template
  install.sh                the whole install, idempotent
  README.txt                three lines: unzip, run install.sh, open the URL
```

Nothing else. No supervisor, no tray, no installer framework. A Pi is headless
and the web UI is the front end; the supervisor's job on a desktop (own the
child, show why it failed, autostart at login) is systemd's job here, and
systemd already does it.

## Decisions

### 1. Built on trixie, dynamic, from the Dockerfile that already exists

Drogon is packaged in Debian trixie and not in bookworm. A bookworm build
would compile Drogon under emulation on every tag, and the result would still
be a binary that has to match the Pi's own Drogon ABI. So: **trixie only**,
which is what Raspberry Pi OS has shipped since mid 2026 and what both coyote
Pis run. A Pi on bookworm is out of scope for this SDD.

That also settles static versus dynamic. Static third-party libraries only buy
running on a release the binary was not built on, and trixie-only gives that
up already. **Fully dynamic**, the same link the Docker image uses, and
install.sh installs the same runtime packages the image's final stage does.
There is no new CMake option and no second dependency list to drift.

The build itself is the Dockerfile's `builder` stage with
`--platform linux/arm/v7` and `--output type=local`. One recipe, already
proven on amd64 and arm64 by every release; the job adds a platform and
extracts `build/hms_cpap` instead of copying it into a runtime image. Under
QEMU on a hosted runner this is on the order of 45 minutes. It runs on the tag
push only, in parallel with the other release jobs, and the release step waits
for it like it waits for the macOS zip.

### 2. install.sh is the upgrade path too

```
sudo ./install.sh
```

1. `apt-get install` the runtime list: ca-certificates, curl, libcurl4t64,
   libpq5, libpqxx-7.10, libssl3, libjsoncpp26, libpaho-mqtt1.3,
   libpaho-mqttpp3-1, libspdlog1.15, libfmt10, libsqlite3-0, libmariadb3,
   libdrogon1t64, libtrantor1, libhpdf-2.3.0. The list lives in the script and
   is checked against the Dockerfile by a test (see Tests).
2. Refuse anything that is not armhf trixie, with the reason, before touching
   the system.
3. Copy `hms_cpap` to `/usr/local/bin/hms_cpap`, keeping the previous one as
   `hms_cpap.previous`.
4. Copy `static/browser` to `<home>/static/browser` of the invoking user
   (`$SUDO_USER`).
5. Write `/etc/systemd/system/hms-cpap.service` from the template with
   `User=`, `HOME=` and `WorkingDirectory=` filled in for that user, and
   `HMS_CPAP_SUPERVISED=1` so the service does not try to open a browser.
6. `systemctl daemon-reload`, `enable --now` (or `restart` if it was already
   enabled), wait up to 30 seconds for `/health` on the configured port, print
   the URL, and print `setup_complete` from the health JSON so the user knows
   whether the wizard is next.

Re-running it on a newer zip is the upgrade: steps 3 to 6 replace the binary
and UI and restart. Config, database and archive under `~/.hms-cpap` are never
touched, which is the same rule the Windows uninstaller follows (SDD-016).

Runs as root through sudo because `/usr/local/bin` and `/etc/systemd/system`
need it. The service itself runs as the user, not root. No `--user` unit: a
headless Pi has no login session, so a user unit needs linger enabled, which
needs root anyway, and a system unit with `User=` is the plainer thing.

### 3. What the script refuses to do

- No config.json is written. First run is the web wizard, same as every other
  platform (SDD-006).
- No swap, no overclock, no card tuning. Those are the Pi owner's business.
- No uninstall. `systemctl disable --now hms-cpap` and removing the three
  paths is documented in README.txt; a script that deletes things a user may
  have moved is how a database gets lost.

## Tests

- **A script test that the runtime list matches the Dockerfile**, so the two
  cannot drift: parse the `apt-get install` list from the Dockerfile's runtime
  stage and from install.sh and assert they are equal. Runs in the normal suite
  (a plain shell or Python check under `tests/scripts/`), no Docker needed.
- **The workflow job verifies the artefact** before uploading: `file` says
  `ELF 32-bit LSB ... ARM, EABI5`, and `qemu-arm-static ./hms_cpap --preflight`
  exits and prints a report rather than crashing, using the runtime stage of
  the same Dockerfile as the environment.
- **On picpapdash2**, by hand, the first time: wipe as in the 5.2.2 test,
  unzip, `sudo ./install.sh`, and the dashboard fills from the card, newest
  night first. After that the release page is the install path for that Pi.

## Out of scope

- bookworm, arm64 native (the Docker image covers 64-bit), a .deb. A .deb is
  the right long-term shape and would make install.sh three lines; it is not
  worth the packaging work until a second person installs on a Pi.
- The supervisor on Linux. Nothing in SDD-016 changes.
