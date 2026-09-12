# SDD-027: the Sleep Agent comes home

**Status:** Proposed
**Date:** 2026-09-08
**Repos:** `cpapdash-ai` (the tools leave it as a library), a new shared
library, `hms-cpap` (the tools arrive, plus a chat surface)
**Version:** target TBD (Albin's call)
**Related:** SDD-041 (the coach chat this replaces), SDD-067 (the extraction
that produced cpapdash-ai), SDD-071 (the explain tools), SDD-086 (SleepHQ
tools), `cpapdash-ai/docs/data-access.md` (why the tools are bound to the
question, not to a service)

## Trigger

Albin, 2026-09-08: "we are porting ALL AI tools from the service-ai so we can
do a little bit of AI magic in hms-cpap."

## What each side has today

**hms-cpap** has an agent from SDD-041: seven read tools (recent sessions,
session details, daily summary, trend, compare periods, vitals, statistics),
a bounded tool loop, and a pgvector memory. It listens on one MQTT topic,
`cpap/<device>/agent/query`, and answers on another. It is compiled only
`WITH_POSTGRESQL`, so on the SQLite installs, which is every Pi, every Docker
default and every Windows and Mac install, there is no agent at all. It has no
page in the web UI.

**cpapdash-ai** has thirty-eight tools plus the explain family, in 2,800 lines
of `AgentTools.cc`: nights, trends, events, statistics, periods, oximetry,
stored and generated summaries, equipment and cleaning with their write
actions, ML predictions, data coverage, devices, logs, commands and actions,
the ops journal, SleepHQ status, exports and export, reference search,
product knowledge, capabilities, glossary, the SDD-071 explain tools and the
`analyze` family. Around it: a runtime that knows nothing about CPAP
(`AgentRuntime`, 381 lines), the service that grounds numbers, corrects
failed and empty rounds, moderates, refuses to disclose our implementation,
streams progress and builds the first-question chips (`AgentService`, 1,235
lines), `Disclosure`, `ModerationService`, `WebSearch`, `SiteMap`, `Brief`,
`LlmGateway`.

The property that makes the port cheap is the one `data-access.md` records:
**every tool asks a question, not a service.** `QueryClient::ask(identity,
kind, params)` publishes one of twenty-six query kinds and the monolith's
`QueryResponder` answers. The tools never touch a database, a URL or a
schema. Bind the same tools to a query plane that answers from hms-cpap's own
tables and they run unchanged.

## The rule

**One tool layer, two query planes.** The tools, the runtime and the fences
move out of cpapdash-ai into a library both products consume. cpapdash-ai
keeps its bus-backed query plane. hms-cpap gets an in-process query plane
over its own `QueryService` and services. Neither product carries a copy of a
tool.

### Decisions

**D1. A library, not a copy.** `cpapdash-agent`, a new repo in the shape of
`hms-shared` and pulled the same way with FetchContent. It holds
`AgentTools`, `AgentRuntime`, `AgentService` minus its HTTP and bus edges,
`Disclosure`, `ModerationService`, `Brief`, `SiteMap`, the explain and analyze
families, and an `IQueryPlane` interface with the `QueryAnswer` shape that the
tools already consume. cpapdash-ai shrinks to `main`, its HTTP and SSE, the
bus `QueryClient` implementing `IQueryPlane`, `Access`, `Identity`, `S3`,
`AgentStore`, `Chart`. This is the library-first rule from the service split
applied in reverse, and the release order is the same: library first,
cpapdash-ai adopts it with a release that contains nothing else, then
hms-cpap.

**D2. The local query plane answers the same twenty-six kinds from local
data.** `LocalQueryPlane` in hms-cpap, one method per kind, over
`QueryService` and the services that already exist:

| Kind | Local source |
|---|---|
| dashboard, daily_summary, trend, statistics, recent_nights, session_detail, session_events, events, events_summary, signal_window, oximetry | `QueryService`, existing reads |
| coverage | one new query: first and last night, counts, gaps |
| summaries | `cpap_summaries` |
| equipment, cleaning, and their actions | SDD-004 and SDD-007 services |
| sleephq_status, sleephq_exports, sleephq_export | `SleepHqExportService` |
| ml_insights | SDD-013 local model and `MLTrainingService` |
| device_health | the source's own reachability: last successful burst, consecutive failures, last file seen |
| generate_summary | the existing LLM summary path |

**D3. Kinds with no local equivalent are not in the local tool set.** The ops
journal (SDD-040 never came to hms-cpap), alert_detail (notifications are a
cloud feature), and devices, device_logs, device_commands, device_action
(there is no Mule and Miner client in hms-cpap; the source is a card or a
share). `definitions()` takes a capability set from the query plane and omits
what it cannot answer, so `list_capabilities` stays honest and the model is
never offered a tool that returns "not available". When a local Mule and
Miner client exists, the device tools switch on by adding the capability.

**D4. The agent runs on SQLite.** `AgentMemory` (pgvector) becomes optional
behind the interface it already has; without it the agent has the current
conversation and no cross-conversation recall, which is what cpapdash-ai
ships today. Conversations persist in a new `agent_conversations` table on
all three engines. The `WITH_POSTGRESQL` gate around the agent goes.

**D5. The surface is the web UI.** `POST /api/agent/ask` streaming SSE with
the same events cpapdash-ai emits (status, token, done, error), and the agent
page ported from the API repo's frontend with its chips. The MQTT topic stays
for Home Assistant. The LLM is whatever `llm` in config.json says: Ollama
needs a tool-calling model, OpenAI and Anthropic work as they do in
cpapdash-ai through `LlmGateway`.

**D6. The two external fences are on when they can be, off when they cannot,
and never silently degraded.** OpenAI moderation and `search_reference` need
an OpenAI key. With one in config they run exactly as in the cloud. Without
one, moderation falls back to the local rule set `ModerationService` already
has, and `search_reference` is out of the tool set per D3. Disclosure and
numeric grounding have no external dependency and are always on.

## What ships, in order

1. `cpapdash-agent` v1.0.0: the library, its tests moved with it (unit 619
   today), `IQueryPlane`, capability-scoped `definitions()`.
2. cpapdash-ai adopts it. A release whose only change is the adoption, so
   "the suites pass unchanged" is a checkable claim.
3. hms-cpap: `LocalQueryPlane` with a contract test per kind against the
   fixtures cpapdash-ai's responder tests already use, the SQLite-capable
   agent, the SSE endpoint, the page, the settings for provider and key.

## Tests

- Library: the existing cpapdash-ai unit suite, moved.
- hms-cpap: one contract test per query kind, asserting the `LocalQueryPlane`
  answer has the shape the tool reads, using the same fixtures as the cloud
  responder so the two planes cannot drift apart silently. The tool loop
  under a fake LLM, as `BurstOrchestrationTest` does for the collector. The
  capability scoping: with no SleepHQ configured, `list_capabilities` does not
  mention it and `definitions()` has no SleepHQ tool.

## Out of scope

- The ops journal and alerts on a local install.
- Cross-conversation memory without PostgreSQL.
- Any change to what the tools do. If a tool is wrong it is wrong in both
  products and gets fixed once, in the library.
