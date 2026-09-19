# SDD-044: a download link belongs under the Ingress prefix

**Status:** Accepted 2026-09-19 ("yes please go"). Not yet released.
**Date:** 2026-09-19
**Repo:** `hms-cpap`. One frontend URL, one SQL helper.
**Related:** SDD-021 (serving the UI under Home Assistant Ingress), issue #35

## Trigger

Issue #35, TLaren, on the add-on at 5.2.17: generating a PDF report works and the
file is fine, but **Download** answers `{"error":404}` for every add-on user.
Reported with the cause already worked out, and it holds up.

## 1. Cause, from the code

SDD-021 made the UI work under Ingress in two halves: the server rewrites
`<base href>` to the prefix Supervisor passes in `X-Ingress-Path`, and
`apiBaseInterceptor` resolves the app's root-absolute API calls against that
base.

The interceptor only sees **Angular HttpClient** calls. The Download button is a
plain `<a href>` built by `downloadReportUrl()`, which returned
`/api/reports/<id>/download`. A root-absolute URL ignores `<base href>`, so under
Ingress the browser asked Home Assistant's own root, which knows nothing about
the add-on. Hence a 404 that never reached us. Outside Ingress the same URL is
correct, which is why it works everywhere else and only add-on users see it.

Second, smaller: on one report row `created_at` came from
`datetime('now','localtime')` (the table default) while `completed_at` came from
`sql::now()`, which was `datetime('now')`, UTC. One instant, written twice, the
machine's offset apart. SQLite only: `CURRENT_TIMESTAMP` and `NOW()` are already
the server's local clock.

## 2. Decision

- `downloadReportUrl()` resolves against `document.baseURI`, so the link lands
  under whatever prefix the app was served at. Outside Ingress `document.baseURI`
  is `<origin>/` and the URL is unchanged.
- `sql::now()` returns the server's local clock on every engine, SQLite included.
  It has exactly one caller, the report status transition, so no other timestamp
  moves.

Not done here: a sweep for other links that skip the interceptor. The only other
`<a href>` the app builds is the log download, and it uses a blob URL.

## 3. Tests

- Backend, all three engines: the two timestamps on one report row come from the
  same clock. It fails on SQLite without the fix (measured: 4 hours apart).
- E2E: the built UI served through a stand-in Ingress proxy that adds the prefix
  and `X-Ingress-Path`. Download reaches
  `/api/hassio_ingress/<token>/api/reports/1/download` and returns the PDF, with
  nothing routed outside the prefix; and the same click still works when served
  at the root.
