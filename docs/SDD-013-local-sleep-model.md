# SDD-013: A local sleep model that answers from the database

**Status:** Draft. Decisions at the end, needs Albin's go before implementing
**Date:** 2026-08-11
**Repo:** `hms-cpap`
**Version target:** unassigned
**Depends on:** the SHHS sleep-stage harness (`training/sleep_stage/`, `src/ml/`),
the database abstraction (`include/database/IDatabase.h`, `SqlDialect.h`)
**Related:** `hms-cpapdash-api` SDD-041 (AI Coach), which solves the same
grounding problem against a cloud model and a Postgres schema

## Why

hms-cpap can already talk about a night, and every word of it comes from
somewhere else. `AppConfig::Llm` points at an endpoint, `llm_prompt.txt` says
what to ask for, and `AgentService` runs a tool-use loop against whatever model
that endpoint serves. Unplug the network and the feature is gone.

That is acceptable on the maestro hub. It is not acceptable for a standalone
install: hms-cpap on SQLite, no hub, no Postgres, no assumption of an internet
connection. There the narration has to be produced locally or not at all.

**This ships generically, for every platform hms-cpap builds on.** A Raspberry
Pi Zero 2 W is used throughout this spec as the *floor*: 512MB of RAM and a
1GHz Cortex-A53 is the least capable machine anyone will plausibly run this on.
Anything designed to fit there fits an x86 desktop, an ARM Mac, or a Pi 5 with
room to spare, and the same GGUF artifact runs on all of them. The constraint
buys portability; it is not a deployment target.

The reference for how is
[slvDev/esp32-ai](https://github.com/slvDev/esp32-ai), which runs a 28.9M
parameter model on an ESP32-S3 at 9.88 tokens/sec by keeping most of the
parameters in flash and reading roughly 450 bytes of them per token. The
technique itself does not survive contact with our requirements, for reasons
set out below, but the shape of the idea does: a small model, trained for one
narrow job, with a runtime built around the memory the board actually has.

## What exists today

Verified in source, not assumed.

**An LLM client, not a model.** `include/agent/IAgentLLM.h` wraps
`hms::LLMClient`. `src/agent/AgentService.cpp` (249 lines) subscribes to
`cpap/{device_id}/agent/query` over MQTT, runs a tool loop bounded by
`max_iterations = 5` at `temperature = 0.3`, and publishes to
`.../agent/response`. `agent.enabled` defaults to false and the struct comment
in `AppConfig.h` reads "requires LLM + PostgreSQL". There is no REST endpoint
and no chat page in the frontend; the only frontend references to the agent are
in `models/config.model.ts` and the settings component.

**Seven read-only queries that are already the right seven.**
`include/agent/AgentTools.h` declares `get_recent_sessions`,
`get_session_details`, `get_daily_summary`, `get_trend_data`,
`compare_periods`, `get_vitals`, `get_statistics`. Each is a hardcoded
parameterized SQL query with `device_id` injected at construction and never
exposed to the model. This is the retrieval layer this spec wants. It is also
the one piece that does not currently work on the target board, see below.

**A model harness, for a different kind of model.** `src/ml/` is a from-scratch
classical stack: `DecisionTree` (415 lines), `RandomForest`, `CrossValidator`,
`StandardScaler`, `FeatureEngine`, `HmmSmoother`, `SleepStageTrainer`. Models
serialize through `toJson()`/`fromJson()`, land in a versioned `model_dir`
(`shhs-rf-v1`), get trained by `MLTrainingService` (762 lines) and served live
by `LiveSleepStageRunner`. Offline work lives in `training/sleep_stage/`: a
Python extractor, a `feature_spec.md` that declares canonical feature order the
Python side must match, and `parity_check.py` diffing Python output against a
C++ fixture at 1e-6 tolerance.

That last pattern is the important asset. Offline training in Python, versioned
artifact on disk, native inference in the binary, a parity check binding the
two halves. This spec adds a second model type to that pattern rather than
inventing one.

What does not exist anywhere in the repo: any neural network. No tensors, no
matmul kernels, no quantization, no tokenizer, no autoregressive decode.
`third_party/` contains one thing, `mdns`.

**A database abstraction the agent does not use.** `include/database/` has
`IDatabase`, `DatabaseFactory`, `SqlDialect`, and concrete
`SQLiteDatabase`/`PostgresDatabase`/`MySQLDatabase`. `IDatabase` is explicit
that "all types are standard C++, no pqxx headers required".

`AgentTools` ignores all of it. Its header includes `<pqxx/pqxx>` and every
tool method takes `pqxx::connection&`. On a SQLite-only appliance the seven
tools cannot run at all.

## The floor

Everything below is sized against the worst machine we intend to support, so
that no platform needs a different model, a different artifact, or a different
code path.

Pi Zero 2 W: quad Cortex-A53 at 1GHz, **512MB LPDDR2**, storage on SD card.
Headless, after the OS, call it 400-430MB of usable RAM, and hms-cpap itself
has to keep running in that.

| Model | Params | Q4 on disk | Fits? |
|---|---|---|---|
| Gemma 3 270M | 270M | ~159MB | Yes |
| SmolLM2 360M | 360M | ~210MB | Yes |
| Qwen3 0.6B | 600M | ~380MB | Tight |
| Llama 3.2 1B | 1B | ~700MB | No |
| Llama 3.2 1B (Q2_K) | 1B | ~550MB | No |

A 1B does not fit in RAM at any quantization that leaves it coherent. It can be
mmapped and demand-paged off the SD card, which is not the same as running: a
dense transformer touches nearly every weight per token, so each token would
stream hundreds of megabytes off the card.

**Decision already made:** 270M.

## Why not Per-Layer Embeddings

PLE is a Gemma **3n** feature (E2B/E4B). Gemma 3 270M is a plain dense model and
does not have it. That alone does not settle the question, because 270M has the
same lopsided parameter distribution PLE is designed to exploit. Google's split
is **170M embedding, 100M transformer blocks**: a 256,000-token vocabulary at
640 hidden dimensions is 163.8M parameters, roughly two thirds of the model,
sitting in a lookup table.

Embedding lookups are sparse. llama.cpp mmaps its GGUF, so the page cache would
hold only the rows actually touched, and the resident cost of that table would
be a few hundred bytes per token rather than 100MB. Most of the PLE benefit,
for free, with no custom runtime.

**The output head breaks it.** Gemma ties the input embedding matrix to the
output projection. Input lookup is sparse; computing logits is not. Every token
requires the full 256k x 640 matrix, so every token reads all ~100MB of that
quantized table. Off an SD card, on an A53, that is the failure mode, and mmap
makes no difference to it.

**Vocabulary pruning is the fix, and it is a better one.** This model answers
questions about CPAP therapy from a schema we control. A 256k multilingual
vocabulary is nearly all dead weight for that job.

| | 256k vocab | 8k vocab | 16k vocab |
|---|---|---|---|
| Embedding params | 163.8M | 5.1M | 10.2M |
| Total params | 270M | ~105M | ~110M |
| Q4 resident | ~159MB | **~62MB** | **~65MB** |
| Output head read per token | ~100MB | ~3MB | ~6MB |

The pruned model fits entirely in RAM, needs no streaming, needs no custom
runtime, and leaves 340MB+ for hms-cpap. PLE would then have nothing left to
page out, because the thing it was going to page out has been deleted.

This is the TinyStories move: constrain the domain, then constrain the
vocabulary to match it.

**Cost of pruning.** Re-initializing the embedding matrix from retained rows
means a full fine-tune of embeddings and head, not a LoRA adapter. At ~105M
parameters that is still cheap on a single Colab T4.

## The corpus

Generated, because none exists. The same reason TinyStories synthesized its
corpus instead of finding one.

Source of truth for the inputs is our own data: sample real metric
distributions out of the existing tables so the model never trains on
physiologically impossible nights. Prompt a cloud model (Google AI Studio) with
`llm_prompt.txt` as the behavioral target and those metrics as the input, and
keep the pairs.

Target scale: order 10^4-10^5 pairs, weighted to cover the cases that matter
and are rare in one person's data. Excellent nights, non-compliant nights,
partial nights, high leak, high AHI, missing SpO2, missing STR, a first night
with no history to compare against.

The shipped artifact is fully offline. The training of it is not, and that is
fine. One thing to read before committing: the Gemini API terms restrict using
outputs to develop competing models. A CPAP night summarizer is not one, but
the sentence should be read rather than assumed.

## Training

Colab or Vertex, not the Pi and not the hub. Prune the vocabulary, re-init the
embedding matrix, full fine-tune, export GGUF at Q4.

Artifact lands in `model_dir` under a version string, the same convention as
`shhs-rf-v1`. Proposed: `narrator-g270m-v1`.

## Retrieval is deterministic

**No tool calling.** A ~105M model will not reliably drive a tool loop: it will
invent tool names and emit malformed arguments, and `max_iterations = 5` turns
that into five wrong turns instead of one. The model is never asked to decide
what to fetch.

C++ runs the queries, formats the results into a fact block, and the model's
only job is turning that block into sentences. That is a constrained
transformation task, which is what a model this size can actually be good at
after fine-tuning.

**Porting the seven tools.** They move off `pqxx::connection&` and onto
`IDatabase`. This is mechanical rather than novel, because `SqlDialect` already
carries every helper those queries need: `param()` for `$1` versus `?`,
`round()` for the Postgres `::numeric` cast, `daysAgo()`, and `sleepDay()` for
the 12-hour offset that defines a sleep day on all three backends.

After the port the same seven queries serve both the cloud agent and the local
model, on SQLite, MySQL, or Postgres.

## The generation contract

A model this small is fluent and cannot do arithmetic. The contract has to make
that irrelevant.

1. Every number in the output comes from the fact block. The model writes the
   language around them.
2. A validator scans the generated text for numeric tokens and rejects any that
   does not appear in the fact block. This is the same guard SDD-041 specifies
   for the cloud coach, and it matters more here.
3. On rejection, retry once at a lower temperature, then fall back to a
   deterministic template. The user gets a correct plain summary rather than a
   fluent wrong one.
4. Dates go through the same check. A summary that names the wrong night is the
   failure users notice first.

## Runtime

Vendor llama.cpp into `third_party/`.

The argument for hand-writing a forward pass held while the architecture was
going to be ours. It is Gemma's, so reimplementing it buys nothing, and
llama.cpp's ARM NEON dotprod kernels are exactly what a bandwidth-starved A53
needs and are not worth matching by hand.

**One artifact, every platform.** GGUF is architecture-independent, so the same
model file ships to x86-64 Linux, ARM64 Linux, macOS, and Windows. llama.cpp
selects its SIMD backend at build time, so the only per-platform work is the
build, and the repo already carries that: `build_arm.sh`, `build-mac/`,
`desktop/`, `Dockerfile`, `docs/CROSS_COMPILATION.md`. Note the Pi at
192.168.2.73 was decommissioned, so a Zero 2 W would be a new box rather than a
restored one, but nothing here waits on that hardware existing.

Feature gating follows the existing convention: a `local_model.enabled` flag
defaulting to false, alongside `llm.enabled`, `agent.enabled`, and
`ml_training.enabled`. An install that never turns it on pays nothing but the
linked runtime.

New code, mirroring existing layout:

```
include/model/LocalModel.h        llama.cpp wrapper behind an interface
include/model/FactBlock.h         typed fact block + serializer
include/model/NarrationService.h  query -> facts -> generate -> validate
src/model/...
training/narrator/                corpus generation, prune, fine-tune, parity
```

`LocalModel` sits behind an interface for the same reason `IAgentLLM` does:
tests must not need a model file.

## Memory budget on the board

| | |
|---|---|
| Model weights (8k vocab, Q4) | ~62MB |
| llama.cpp runtime + KV cache | to be measured, tens of MB at a 2048 context |
| hms-cpap + SQLite | the rest |

Generation runs as a **nightly batch job**, not a chat. That is what makes the
floor viable: at a few tokens per second a 300-token summary takes minutes,
which is unusable interactively and entirely fine once per night. Throughput on
that hardware is a measurement, not an estimate, and phase 1 exists to produce
that number.

On any larger machine the same job finishes in seconds. That headroom is not
spent here. Whether a faster host earns an interactive mode is a later
question, and answering it would mean revisiting the no-tool-calling decision,
which is a property of the model rather than of the host.

## Parity check

Mirroring `training/sleep_stage/parity_check.py`.

A fixture of fact blocks, run through the Python reference implementation and
through the C++ path, comparing tokenizer output exactly and logits at a stated
tolerance. Greedy decode, fixed seed. This is what catches a tokenizer or
quantization mismatch, which otherwise shows up as prose that is subtly worse
than it was in Colab and is very hard to attribute later.

## Relationship to the cloud product

These are two tiers with deliberately different constraints, and neither should
be designed as a compromise between them.

`hms-cpapdash-api` runs on a VPS with an internet connection and a billing
relationship. It ships frontier models through external APIs and its limits are
cost and latency. SDD-041 specifies the coach there against exactly that
assumption.

hms-cpap is self-hosted on hardware we do not choose, with no assumption of a
network. Its limit is 512MB of someone else's RAM. It gets a model that fits.

**The harness is ours in both tiers.** Only the model backend differs. The
loop, the tool layer, the fact block, the validators, the fallback, and the
trace all live in our own C++, and a model is something that gets plugged into
the back of it: an external API call on the VPS, a local GGUF on the box. No
vendor agent framework and no sidecar runner on either side.

This is consistent with how the rest of this codebase is built. `src/ml/` is a
hand-written decision tree and random forest rather than a wrapped library, and
`third_party/` holds exactly one dependency. It also means the two tiers can
share tool definitions and validators instead of reimplementing grounding twice
in two languages.

Note for `hms-cpapdash-api` SDD-040: its phase 0 is an open research spike on
the agent harness, weighing hand-rolled C++ against a framework sidecar. The
direction stated here answers that question in one direction. Whether SDD-040
adopts it is a decision in that repo, not this one.

What the two tiers share is the grounding discipline, not the model:
deterministic retrieval, numbers that trace to the data, a validator that
rejects invented figures, and a fallback that prefers a plain correct answer to
a fluent wrong one. That discipline exists in the cloud because it is good
practice. It exists here because a 105M model gives no other option.

## Deliberately out of scope

- **RAG and embeddings.** The data is a known schema with known query shapes, so
  SQL retrieves better than vectors and costs nothing. `AgentMemory` currently
  uses `nomic-embed-text`, 137M parameters, more resident memory than the
  language model itself. If fuzzy recall over past conversations is wanted
  later, a ~22M MiniLM at roughly 45MB is the version that fits this board.
- **Chat.** Nightly batch generation only. No REST endpoint, no chat UI, no MQTT
  query loop for the local model.
- **Tool calling**, per above.
- **On-device training.** Colab or Vertex.
- **Per-Layer Embeddings**, per above. Revisit only if the vocabulary cannot be
  pruned as far as this spec assumes.
- **Replacing the cloud agent.** `AgentService` stays. This is the offline path,
  not a migration.

## Tests

- `SqlDialect` port: each of the seven queries returns equivalent rows on
  SQLite and Postgres against the same seeded fixture.
- `FactBlock` serialization is stable and ordered, since prompt drift silently
  degrades a fine-tuned model.
- Numeric validator: accepts a summary whose figures are all in the block,
  rejects an invented figure, rejects a transposed one, rejects a wrong date.
- Fallback path: a model that fails validation twice produces the deterministic
  template and logs why.
- `NarrationService` against a scripted `LocalModel`, no model file required.
- Parity fixture, per above.

## Verification

The gate is the floor. On a real Zero 2 W, not in an emulator:

1. Peak RSS of hms-cpap plus the model during generation, held under budget with
   the service still serving.
2. Tokens per second, measured.
3. Wall clock for one night's summary end to end.
4. Thermals and SD wear over a week of nightly runs.
5. Output quality on held-out real nights, read by a human, against the same
   nights summarized by the cloud model.

Then a portability smoke on x86-64 Linux and on an ARM Mac: same GGUF, same
fixture, byte-identical output under greedy decode. Divergence there is a
quantization or SIMD bug and must be understood before release, not written off
as platform noise.

## Decisions

**Made:**

1. 270M, not 1B. A 1B does not fit 512MB, and a from-scratch 1B is not
   trainable on hobby compute anyway.
2. Standalone appliance on SQLite.
3. Corpus generated in Google AI Studio, fine-tune on Colab or Vertex.
4. Nightly batch, not chat.
5. Deterministic retrieval, model writes prose only.
6. Ships generically on every platform hms-cpap builds for, one GGUF artifact.
   The Zero 2 W is the sizing floor, not a deployment target.
7. The harness stays ours, in C++, in the binary. Models plug in behind it. No
   vendor agent framework, no sidecar, on either tier.

**Open:**

1. **Vocabulary size, 8k or 16k.** Quality against footprint. 8k saves ~3MB
   resident and halves the per-token head read; 16k is safer for anything the
   pruning set underestimates. Recommend building the token frequency histogram
   over the generated corpus first and deciding from the coverage curve rather
   than in the abstract.
2. Version target for the release.
3. Whether the `AgentTools` port to `IDatabase` ships as its own change ahead of
   this work. It stands on its own: it makes the existing cloud agent work on
   SQLite and MySQL, which it does not today.
4. Whether the nightly job also writes its summary into the existing summary
   storage the dashboard reads, or into a separate table so cloud and local
   summaries stay distinguishable.
5. **How the model file is distributed.** It is roughly 62MB, which does not
   belong in git. Options: bundled into the release tarball and the Docker
   image, or downloaded on first enable from a pinned URL with a hash check.
   The second matches how `esp32-ai` ships its models and keeps the repo small,
   but it means a first run needs a network on a feature whose whole point is
   not needing one. Worth checking what `shhs-rf-v1` does today before
   deciding.
