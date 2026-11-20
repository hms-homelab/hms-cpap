#!/usr/bin/env python3
"""SDD-013: the narrator corpus, night facts -> summary pairs.

Stages, each resumable (a half-done run continues where it stopped):

  1. nights   generate_nights.py writes the synthetic nights (seeded, so a
              resumed run regenerates the same ones if the file is missing).
  2. facts    hms_cpap_narrator_facts render: each night through the
              runtime's NightFacts::from() and model::factBlock(), so the
              corpus input is byte for byte what the model sees at runtime.
  3. teacher  --teacher-provider anthropic (default: Claude Haiku 5.5, the
              instruction below as a cached system prompt, the fact block as
              the user turn) or gemini (Gemini Flash-Lite, the instruction and
              the fact block in one turn). Batched concurrency with a
              requests-per-minute limit; every answer is appended to
              teacher.jsonl as it arrives, and a resumed run asks only for the
              nights not yet answered. --teacher-mode batch sends the same
              requests as batch jobs instead (half price, for the full
              corpus), tracked in batches.jsonl (see stage_teacher_batch).
  4. check    hms_cpap_narrator_facts check: model::violations(), SDD-072's
              number check plus the ISO date check, the same check the nightly
              job applies to the model. A failing answer is dropped; the drop
              rate and reasons go to report.json.
  5. split    train / val / test JSONL by a hash of the night id.

The API key is read from the environment (ANTHROPIC_API_KEY or
GOOGLE_AI_API_KEY) and never written anywhere.

Usage:
    python build_corpus.py --out out/pilot --count 500
    python build_corpus.py --out out/full --count 295000 --teacher-mode batch --wait
    python build_corpus.py --out out/pilot --count 500 --teacher-provider gemini
"""

import argparse
import concurrent.futures as cf
import hashlib
import json
import math
import os
import random
import re
import subprocess
import sys
import threading
import time
import urllib.error
import urllib.request
from collections import Counter
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parent.parent
DEFAULT_TOOL = REPO / "build" / "hms_cpap_narrator_facts"
DEFAULT_MODEL = {"anthropic": "claude-haiku-5-5", "gemini": "gemini-3.5-flash-lite"}
API ="https://generativelanguage.googleapis.com/v1beta/models/{model}:generateContent"

# The teacher's whole instruction, the same for every night (so it caches), with
# the night's fact block after it. It replaces llm_prompt.txt + a corpus suffix:
# that prompt gives the AHI band limits and the 4-hour minimum as digits, the
# fact block does not, and in the pilot 43% of answers repeated them ("5 to 15
# range", "4-hour minimum") or computed a percentage, and failed the number
# check. The student sees only the fact block, so it must never learn to write
# a number that is not in it.
TEACHER_INSTRUCTION = """You write the short summary of one night of PAP therapy that a person reads the next morning. The night's data comes after these instructions. Your summary becomes training text for a small model that will write these summaries on its own, so every summary must be right and in the same voice.

Form: 3 or 4 plain, warm sentences of English prose in one paragraph. No lists, no headings, no markdown. Speak to the reader as "you". Never use an em dash.

What to say, and only what matters:
1. The night's date, written exactly as the data writes it.
2. How long the therapy ran against the target, in words: "past your target", "just short of your target", "well short of your target". You may give the hours as the data writes them. Do not mention any minimum.
3. The AHI as the data writes it, with its band named in words only: "the normal range", "the mild range", "the moderate range" or "the severe range". Pick the band with the usual adult bands (normal below 5 events per hour, mild from 5 to below 15, moderate from 15 to below 30, severe from 30 up). Those limits are only for choosing the word: never write a band limit or any threshold, in digits or in words.
4. At most one or two other points that stand out: leak only when it is notable (a high 95th percentile or maximum), SpO2 and ODI when the night has oximetry, an event type only when its count is not zero and it says something (for example most events were hypopneas, or central apneas were a large share).

Numbers: write only figures that appear in the data, exactly as the data writes them, with the same digits and decimals. Never compute anything: no percentages of your own, no sums, differences, ratios or rounding. Skip zero counts and figures that add nothing; a summary is not a list of the data, so never walk through every event type or every leak figure.

When the data says no AHI is available: say so in a few words, give the total events if useful, and do not grade the night, the index or the events in any way (no normal, mild, moderate, severe, good, poor, high or low for them). Never report an event type as zero. Never estimate a figure the data does not give.

Describe the night only. No medical advice, no diagnosis, and do not tell the reader to change settings or equipment or to see anyone.

Three examples of good summaries (shortened data):

Data:
Night: 2025-03-02
Usage: 7.42 hours (92.75% of 8h target)
AHI: 2.10 events/hour
Apneas and hypopneas (the events counted in the AHI): 16 (obstructive apnea=9, central apnea=0, clear airway apnea=0, unclassified apnea=0, hypopnea=7)
Other recorded events (not counted in the AHI): 0 (RERA=0, other event types=0)
Leak: avg=3.10 L/min, max=24.80 L/min, 95th=9.60 L/min, median=1.20 L/min
Summary: On 2025-03-02 you used your therapy for 7.42 hours, just short of your target. Your AHI was 2.10 events/hour, in the normal range, from a handful of obstructive apneas and hypopneas. Leak stayed low all night, so your mask seal held well.

Data:
Night: 2025-11-14
Usage: 5.10 hours (63.75% of 8h target)
AHI: 18.40 events/hour
Apneas and hypopneas (the events counted in the AHI): 94 (obstructive apnea=31, central apnea=4, clear airway apnea=0, unclassified apnea=0, hypopnea=59)
Leak: avg=28.50 L/min, max=96.20 L/min, 95th=61.30 L/min, median=22.00 L/min
O2 Ring Oximetry (overnight): Avg SpO2: 93.40%, Min SpO2: 84.00%, ODI (3%): 15.70 events/hour
Summary: On 2025-11-14 you used your therapy for 5.10 hours, well short of your target. Your AHI was 18.40 events/hour, in the moderate range, and most of the 94 apneas and hypopneas were hypopneas. Leak ran high, with a 95th percentile of 61.30 L/min. Your oximeter recorded an average SpO2 of 93.40% and a lowest reading of 84.00%.

Data:
Night: 2024-06-09
Usage: 8.35 hours (104.38% of 8h target)
AHI: not available for this machine -- it does not detect hypopneas, so no apnea-hypopnea index can be formed.
Total events: 41 (all apneas; this machine does not classify them)
Therapy pressure (delivered by the machine): avg=9.20 cmH2O
Summary: On 2024-06-09 you used your therapy for 8.35 hours, past your target. Your machine does not detect hypopneas, so no AHI is available for this night; it recorded 41 apneas. The pressure it delivered averaged 9.20 cmH2O.

Reply with the summary only."""

TEACHER_USER = "Night data:\n{facts}"

# Words that grade an index (counted, not dropped, on Ungraded nights).
GRADING_WORDS = re.compile(r"\b(mild|moderate|severe|normal|excellent|good|great|"
                           r"poor|high|low|well controlled)\b", re.I)


def log(msg):
    print(f"[build_corpus] {msg}", file=sys.stderr, flush=True)


def read_jsonl(path):
    if not path.exists():
        return []
    out = []
    with open(path) as fh:
        for line in fh:
            line = line.strip()
            if line:
                try:
                    out.append(json.loads(line))
                except json.JSONDecodeError:
                    pass   # a line cut short by an interrupted run
    return out


def run_tool(tool, mode, records):
    payload = "".join(json.dumps(r) + "\n" for r in records)
    p = subprocess.run([str(tool), mode], input=payload, capture_output=True, text=True,
                       check=True)
    return [json.loads(l) for l in p.stdout.splitlines() if l.strip()]


# -- 1, 2: nights and facts ------------------------------------------------------

def stage_nights(args, out):
    path = out / "nights.jsonl"
    if path.exists() and sum(1 for _ in open(path)) == args.count:
        log(f"nights: {args.count} already generated")
        return
    subprocess.run([sys.executable, str(HERE / "generate_nights.py"),
                    "--distributions", str(args.distributions), "--count", str(args.count),
                    "--seed", str(args.seed), "--output", str(path)], check=True)


def stage_facts(args, out):
    path = out / "facts.jsonl"
    nights = read_jsonl(out / "nights.jsonl")
    if path.exists() and sum(1 for _ in open(path)) == len(nights):
        log(f"facts: {len(nights)} already rendered")
        return
    stripped = [{k: v for k, v in n.items() if k != "tags"} for n in nights]
    rendered = run_tool(args.tool, "render", stripped)
    errors = [r for r in rendered if "error" in r]
    if errors:
        raise SystemExit(f"render failed for {len(errors)} nights, first: {errors[0]}")
    with open(path, "w") as fh:
        for r in rendered:
            fh.write(json.dumps(r) + "\n")
    log(f"facts: {len(rendered)} rendered")


# -- 3: teacher --------------------------------------------------------------------

class RateLimit:
    """At most [rpm] request starts per minute, spread evenly."""

    def __init__(self, rpm):
        self.interval = 60.0 / rpm
        self.next = time.monotonic()
        self.lock = threading.Lock()

    def wait(self):
        with self.lock:
            now = time.monotonic()
            start = max(now, self.next)
            self.next = start + self.interval
        if start > now:
            time.sleep(start - now)


class KeyRejected(Exception):
    """The API refused the key or the account (no credit): no further request
    can succeed."""


def refused(code, detail):
    return code in (401, 402, 403) or "API_KEY_INVALID" in detail


def user_text(facts):
    return TEACHER_USER.replace("{facts}", facts)


class Teacher:
    """Gemini: one user turn, the instruction then the night."""

    def __init__(self, args, key):
        self.args = args
        self.key = key
        self.url = API.format(model=args.model)
        self.limit = RateLimit(args.rpm)
        self.thinking = args.thinking_level or None

    def prompt(self, facts):
        return TEACHER_INSTRUCTION + "\n\n" + user_text(facts) + "\n"

    def body(self, facts):
        cfg = {"temperature": self.args.temperature, "maxOutputTokens": self.args.max_tokens}
        if self.thinking:
            cfg["thinkingConfig"] = {"thinkingLevel": self.thinking}
        return {"contents": [{"role": "user", "parts": [{"text": self.prompt(facts)}]}],
                "generationConfig": cfg}

    def call(self, rec):
        """One night's answer: {"id", "text", "finish", "usage"} or {"id", "error"}."""
        last = ""
        for attempt in range(self.args.attempts):
            self.limit.wait()
            req = urllib.request.Request(
                self.url, data=json.dumps(self.body(rec["facts"])).encode(),
                headers={"Content-Type": "application/json", "x-goog-api-key": self.key})
            t0 = time.monotonic()
            try:
                with urllib.request.urlopen(req, timeout=self.args.timeout) as resp:
                    data = json.load(resp)
            except urllib.error.HTTPError as e:
                detail = e.read().decode(errors="replace")[:300]
                last = f"http {e.code}: {detail}"
                if e.code == 400 and self.thinking and "thinking" in detail.lower():
                    log(f"the model refuses thinkingLevel={self.thinking}; sending none")
                    self.thinking = None
                    continue
                if e.code in (429, 500, 502, 503, 504):
                    time.sleep(min(60.0, 2.0 ** attempt + random.random()))
                    continue
                if refused(e.code, detail):
                    raise KeyRejected(last)
                return {"id": rec["id"], "error": last}
            except (urllib.error.URLError, TimeoutError, OSError) as e:
                last = f"network: {e}"
                time.sleep(min(60.0, 2.0 ** attempt + random.random()))
                continue
            out = answer_of(rec["id"], data, self.args.model)
            out["seconds"] = round(time.monotonic() - t0, 3)
            return out
        return {"id": rec["id"], "error": f"gave up after {self.args.attempts} tries: {last}"}


def answer_of(nid, data, model):
    """One GenerateContentResponse as a teacher.jsonl record (live and batch)."""
    cands = data.get("candidates") or []
    usage = data.get("usageMetadata", {})
    if not cands:
        return {"id": nid, "error": "no candidate: " +
                json.dumps(data.get("promptFeedback", {}))[:200], "usage": usage}
    parts = cands[0].get("content", {}).get("parts", [])
    text = "".join(p.get("text", "") for p in parts if not p.get("thought")).strip()
    return {"id": nid, "text": text, "finish": cands[0].get("finishReason"),
            "usage": usage, "model": data.get("modelVersion", model)}


# -- 3, Anthropic: Claude through the official SDK ---------------------------------
#
# The instruction is the system prompt, one text block marked for caching, and
# the night's fact block is the user turn, so every request after the first
# reads the instruction from the cache. Thinking is off for this short
# summarisation turn; Haiku 5.5 accepts thinking "disabled" only at effort high
# or below, so effort is sent as low. No temperature (non-default values are a
# 400 on this model) and no assistant prefill (a 400).

ANTHROPIC_MIN_CACHE_TOKENS = 512   # Haiku 5.5's shortest cacheable prefix
ANTHROPIC_BATCH_MAX = 100_000      # requests per Message Batch
ANTHROPIC_OK = ("end_turn", "stop_sequence")


def anthropic_params(model, facts, max_tokens, ttl=None):
    """One night's Messages request (live and batch)."""
    cache = {"type": "ephemeral"}
    if ttl:
        cache["ttl"] = ttl
    return {
        "model": model,
        "max_tokens": max_tokens,
        "system": [{"type": "text", "text": TEACHER_INSTRUCTION, "cache_control": cache}],
        "messages": [{"role": "user", "content": user_text(facts)}],
        "thinking": {"type": "disabled"},
        "output_config": {"effort": "low"},
    }


def usage_dict(usage):
    if usage is None:
        return {}
    if hasattr(usage, "model_dump"):
        return usage.model_dump(exclude_none=True)
    return dict(usage)


def anthropic_answer_of(nid, msg, model):
    """One Message as a teacher.jsonl record (live and batch). A refusal or a
    cut-off answer keeps its stop reason as finish, and the check stage drops
    it under that reason."""
    stop = msg.stop_reason
    rec = {"id": nid, "finish": stop, "usage": usage_dict(msg.usage),
           "model": getattr(msg, "model", model) or model}
    if stop not in ANTHROPIC_OK:
        rec["text"] = ""
        return rec
    rec["text"] = "".join(b.text for b in msg.content if b.type == "text").strip()
    return rec


def anthropic_refused(e):
    """True when no further request can succeed: a bad key, no permission, or
    no credit left on the account."""
    import anthropic
    if isinstance(e, (anthropic.AuthenticationError, anthropic.PermissionDeniedError)):
        return True
    status = getattr(e, "status_code", None)
    return status == 402 or (status == 400 and "credit balance" in str(e).lower())


def anthropic_client(args):
    key = os.environ.get("ANTHROPIC_API_KEY", "")
    if not key:
        raise SystemExit("ANTHROPIC_API_KEY is not set")
    import anthropic
    return anthropic.Anthropic(api_key=key, max_retries=args.attempts, timeout=args.timeout)


def instruction_tokens(client, model):
    """The cached prefix's size: the system block alone, as count_tokens sees it
    with a one-word user turn, minus that turn."""
    sys_block = [{"type": "text", "text": TEACHER_INSTRUCTION}]
    msgs = [{"role": "user", "content": "x"}]
    with_sys = client.messages.count_tokens(model=model, system=sys_block, messages=msgs)
    bare = client.messages.count_tokens(model=model, messages=msgs)
    return with_sys.input_tokens - bare.input_tokens


class AnthropicTeacher:
    def __init__(self, args, client):
        self.args = args
        self.client = client
        self.limit = RateLimit(args.rpm)

    def params(self, facts, ttl=None):
        return anthropic_params(self.args.model, facts, self.args.max_tokens, ttl)

    def call(self, rec):
        """One night's answer: {"id", "text", "finish", "usage"} or {"id", "error"}.
        The SDK retries 408/409/429/5xx and connection errors (max_retries)."""
        import anthropic
        self.limit.wait()
        t0 = time.monotonic()
        try:
            msg = self.client.messages.create(**self.params(rec["facts"]))
        except anthropic.APIStatusError as e:
            last = f"http {e.status_code}: {str(e)[:300]}"
            if anthropic_refused(e):
                raise KeyRejected(last)
            return {"id": rec["id"], "error": last}
        except anthropic.APIConnectionError as e:
            return {"id": rec["id"], "error": f"network: {e}"}
        out = anthropic_answer_of(rec["id"], msg, self.args.model)
        out["seconds"] = round(time.monotonic() - t0, 3)
        return out


def stage_teacher(args, out):
    path = out / "teacher.jsonl"
    facts = read_jsonl(out / "facts.jsonl")
    answered = {r["id"] for r in read_jsonl(path) if "text" in r}
    todo = [f for f in facts if f["id"] not in answered]
    log(f"teacher: {len(answered)} answered, {len(todo)} to ask ({args.model}, "
        f"concurrency {args.concurrency}, {args.rpm} rpm)")
    if not todo:
        return
    run = {}
    if args.teacher_provider == "anthropic":
        client = anthropic_client(args)
        n = instruction_tokens(client, args.model)
        run["instruction_tokens"] = n
        log(f"teacher: the cached instruction is {n} tokens")
        if n < ANTHROPIC_MIN_CACHE_TOKENS:
            log(f"teacher: under {ANTHROPIC_MIN_CACHE_TOKENS} tokens, the instruction will not cache")
        teacher = AnthropicTeacher(args, client)
    else:
        key = os.environ.get("GOOGLE_AI_API_KEY", "")
        if not key:
            raise SystemExit("GOOGLE_AI_API_KEY is not set")
        teacher = Teacher(args, key)
    done = 0
    t0 = time.monotonic()
    with open(path, "a") as fh, cf.ThreadPoolExecutor(args.concurrency) as pool:
        # Written as each answer arrives: an interrupted run loses none it has.
        futures = [pool.submit(teacher.call, rec) for rec in todo]
        for fut in cf.as_completed(futures):
            try:
                res = fut.result()
            except KeyRejected as e:
                for f in futures:
                    f.cancel()
                raise SystemExit(f"teacher: the API refused the key or account ({str(e)[:160]})")
            fh.write(json.dumps(res) + "\n")
            fh.flush()
            done += 1
            if done % 100 == 0 or done == len(todo):
                rate = done / max(time.monotonic() - t0, 1e-9)
                log(f"teacher: {done}/{len(todo)} ({rate:.2f}/s)")
    with open(out / "teacher_runs.jsonl", "a") as fh:
        fh.write(json.dumps({"asked": len(todo), "seconds": round(time.monotonic() - t0, 2),
                             "concurrency": args.concurrency, "rpm": args.rpm,
                             "model": args.model, **run}) + "\n")


# -- 3b: teacher through the Batch API ---------------------------------------------
#
# The same requests as the live path, sent as Batch API jobs (half the price,
# up to 24 h turnaround). batches.jsonl is an append-only ledger: each job is
# recorded the moment it is created, then each state it is seen in, then
# "collected" once its answers are in teacher.jsonl. A run picks up where the
# ledger says: it collects finished jobs, resubmits the nights of failed or
# expired ones, and submits the nights no job holds. With --wait it checks the
# jobs every --poll-seconds until none is left running; without, it checks once
# and returns (run it again later to collect).

BASE = "https://generativelanguage.googleapis.com"
TERMINAL = ("SUCCEEDED", "FAILED", "CANCELLED", "EXPIRED")


def api(key, method, url, body=None, headers=None, raw=False, timeout=300):
    data = body if isinstance(body, (bytes, type(None))) else json.dumps(body).encode()
    req = urllib.request.Request(url, data=data, method=method,
                                 headers={"x-goog-api-key": key, **(headers or {})})
    if data is not None and "Content-Type" not in req.headers:
        req.add_header("Content-Type", "application/json")
    try:
        with urllib.request.urlopen(req, timeout=timeout) as resp:
            payload = resp.read()
            return (payload, resp.headers) if raw else json.loads(payload or b"{}")
    except urllib.error.HTTPError as e:
        detail = e.read().decode(errors="replace")[:300]
        if refused(e.code, detail):
            raise KeyRejected(f"http {e.code}: {detail}")
        raise SystemExit(f"{method} {url.split('?')[0]}: http {e.code}: {detail}")


def upload_jsonl(key, path, display_name):
    size = path.stat().st_size
    _, headers = api(key, "POST", f"{BASE}/upload/v1beta/files",
                     {"file": {"display_name": display_name}}, raw=True, headers={
                         "X-Goog-Upload-Protocol": "resumable",
                         "X-Goog-Upload-Command": "start",
                         "X-Goog-Upload-Header-Content-Length": str(size),
                         "X-Goog-Upload-Header-Content-Type": "application/jsonl"})
    upload_url = headers.get("x-goog-upload-url")
    if not upload_url:
        raise SystemExit("file upload: no upload URL in the reply")
    out = api(key, "POST", upload_url, path.read_bytes(), headers={
        "Content-Type": "application/jsonl", "X-Goog-Upload-Offset": "0",
        "X-Goog-Upload-Command": "upload, finalize"})
    return out["file"]["name"]


def job_state(job):
    state = (job.get("metadata") or {}).get("state") or job.get("state") or ""
    return next((t for t in TERMINAL if state.endswith(t)), state or "UNKNOWN")


def responses_file(job):
    for path in (("response", "responsesFile"), ("metadata", "output", "responsesFile"),
                 ("dest", "fileName"), ("output", "responsesFile")):
        node = job
        for k in path:
            node = node.get(k) if isinstance(node, dict) else None
        if node:
            return node
    return None


class Ledger:
    def __init__(self, path):
        self.path = path
        self.jobs = {}   # job name -> latest record
        for r in read_jsonl(path):
            self.jobs.setdefault(r["job"], {}).update(r)

    def note(self, job, **fields):
        rec = {"job": job, "at": time.strftime("%Y-%m-%dT%H:%M:%S"), **fields}
        with open(self.path, "a") as fh:
            fh.write(json.dumps(rec) + "\n")
        self.jobs.setdefault(job, {}).update(rec)

    def held_ids(self, out):
        """Nights in a job that may still answer (running, or done and not yet collected)."""
        held = set()
        for j in self.jobs.values():
            if j.get("collected") or j.get("state") in ("FAILED", "CANCELLED", "EXPIRED"):
                continue
            held.update(json.loads((out / j["ids_file"]).read_text()))
        return held


def batch_collect(key, ledger, out, model):
    """Checks every job not yet collected; collects the finished ones."""
    for name, j in list(ledger.jobs.items()):
        if j.get("collected") or j.get("state") in ("FAILED", "CANCELLED", "EXPIRED"):
            continue
        job = api(key, "GET", f"{BASE}/v1beta/{name}")
        state = job_state(job)
        if state != j.get("state"):
            ledger.note(name, state=state)
            log(f"batch {name}: {state}")
        if state == "SUCCEEDED":
            rfile = responses_file(job)
            if not rfile:
                raise SystemExit(f"batch {name} succeeded with no responses file: "
                                 f"{json.dumps(job)[:300]}")
            payload, _ = api(key, "GET", f"{BASE}/download/v1beta/{rfile}:download?alt=media",
                             raw=True, timeout=1800)
            n = 0
            with open(out / "teacher.jsonl", "a") as fh:
                for line in payload.decode().splitlines():
                    if not line.strip():
                        continue
                    r = json.loads(line)
                    nid = r.get("key")
                    if "response" in r:
                        rec = answer_of(nid, r["response"], model)
                    else:
                        rec = {"id": nid, "error": json.dumps(r.get("error") or r)[:300]}
                    rec["batch"] = name
                    fh.write(json.dumps(rec) + "\n")
                    n += 1
            ledger.note(name, collected=True, answers=n,
                        created=(job.get("metadata") or {}).get("createTime"),
                        ended=(job.get("metadata") or {}).get("endTime")
                        or (job.get("metadata") or {}).get("updateTime"))
            log(f"batch {name}: collected {n} answers")


def batch_submit(key, ledger, out, args, teacher):
    facts = read_jsonl(out / "facts.jsonl")
    records = read_jsonl(out / "teacher.jsonl")
    answered = {r["id"] for r in records if "text" in r}
    tries = Counter(r["id"] for r in records if "error" in r)
    held = ledger.held_ids(out)
    todo = [f for f in facts if f["id"] not in answered and f["id"] not in held
            and tries[f["id"]] < args.attempts]
    if not todo:
        return 0
    for start in range(0, len(todo), args.batch_size):
        chunk = todo[start:start + args.batch_size]
        tag = f"{int(time.time())}-{start // args.batch_size}"
        req_path = out / f"batch-{tag}.jsonl"
        ids_path = out / f"batch-{tag}.ids.json"
        with open(req_path, "w") as fh:
            for f in chunk:
                fh.write(json.dumps({"key": f["id"], "request": teacher.body(f["facts"])}) + "\n")
        ids_path.write_text(json.dumps([f["id"] for f in chunk]))
        file_name = upload_jsonl(key, req_path, f"narrator-{tag}")
        job = api(key, "POST", f"{BASE}/v1beta/models/{args.model}:batchGenerateContent",
                  {"batch": {"display_name": f"narrator-{tag}",
                             "input_config": {"file_name": file_name}}})
        # Recorded at once: creating a job is not idempotent.
        ledger.note(job["name"], state=job_state(job), file=file_name, model=args.model,
                    ids_file=ids_path.name, count=len(chunk))
        log(f"batch {job['name']}: submitted {len(chunk)} nights ({file_name})")
    return len(todo)


def stage_teacher_batch(args, out):
    if args.teacher_provider == "anthropic":
        return stage_teacher_batch_anthropic(args, out)
    key = os.environ.get("GOOGLE_AI_API_KEY", "")
    if not key:
        raise SystemExit("GOOGLE_AI_API_KEY is not set")
    teacher = Teacher(args, key)
    ledger = Ledger(out / "batches.jsonl")
    running = lambda: sum(1 for j in ledger.jobs.values()
                          if not j.get("collected") and j.get("state") not in TERMINAL)
    try:
        batch_collect(key, ledger, out, args.model)
        submitted = batch_submit(key, ledger, out, args, teacher)
        active = running()
        while args.wait and active:
            log(f"batch: {active} job(s) running; next check in {args.poll_seconds:.0f} s")
            time.sleep(args.poll_seconds)
            batch_collect(key, ledger, out, args.model)
            if not running():
                # Nights of a job that failed or expired go again.
                submitted += batch_submit(key, ledger, out, args, teacher)
            active = running()
    except KeyRejected as e:
        raise SystemExit(f"teacher: the API refused the key or account ({str(e)[:160]})")
    if active:
        log(f"batch: {active} job(s) still running; run again to collect "
            f"({submitted} nights submitted this run)")
    return active


# -- 3b, Anthropic: the same requests as Message Batches -----------------------------
#
# Same ledger as the Gemini path (batches.jsonl). A Message Batch is
# "in_progress" until "ended"; its results come in any order and are keyed by
# custom_id (the night id). The system block is cached with the 1-hour TTL:
# a batch's requests can be processed over a long window, and a 5-minute entry
# can expire between them.

ANTHROPIC_DONE = "ended"


def anthropic_batch_collect(client, ledger, out, model):
    for bid, j in list(ledger.jobs.items()):
        if j.get("collected") or j.get("state") in ("FAILED", "CANCELLED", "EXPIRED"):
            continue
        batch = client.messages.batches.retrieve(bid)
        state = batch.processing_status
        if state != j.get("state"):
            ledger.note(bid, state=state)
            log(f"batch {bid}: {state}")
        if state != ANTHROPIC_DONE:
            continue
        n = 0
        with open(out / "teacher.jsonl", "a") as fh:
            for res in client.messages.batches.results(bid):
                kind = res.result.type
                if kind == "succeeded":
                    rec = anthropic_answer_of(res.custom_id, res.result.message, model)
                elif kind == "errored":
                    rec = {"id": res.custom_id, "error": f"errored: {str(res.result.error)[:300]}"}
                else:   # canceled, expired: asked again on the next submit
                    rec = {"id": res.custom_id, "error": kind}
                rec["batch"] = bid
                fh.write(json.dumps(rec) + "\n")
                n += 1
        ledger.note(bid, collected=True, answers=n,
                    created=str(batch.created_at), ended=str(batch.ended_at))
        log(f"batch {bid}: collected {n} answers")


def anthropic_batch_submit(client, ledger, out, args, teacher):
    from anthropic.types.message_create_params import MessageCreateParamsNonStreaming
    from anthropic.types.messages.batch_create_params import Request
    facts = read_jsonl(out / "facts.jsonl")
    records = read_jsonl(out / "teacher.jsonl")
    answered = {r["id"] for r in records if "text" in r}
    tries = Counter(r["id"] for r in records if "error" in r)
    held = ledger.held_ids(out)
    todo = [f for f in facts if f["id"] not in answered and f["id"] not in held
            and tries[f["id"]] < args.attempts]
    if not todo:
        return 0
    size = min(args.batch_size, ANTHROPIC_BATCH_MAX)
    for start in range(0, len(todo), size):
        chunk = todo[start:start + size]
        tag = f"{int(time.time())}-{start // size}"
        ids_path = out / f"batch-{tag}.ids.json"
        ids_path.write_text(json.dumps([f["id"] for f in chunk]))
        batch = client.messages.batches.create(requests=[
            Request(custom_id=f["id"],
                    params=MessageCreateParamsNonStreaming(**teacher.params(f["facts"], ttl="1h")))
            for f in chunk])
        # Recorded at once: creating a batch is not idempotent.
        ledger.note(batch.id, state=batch.processing_status, model=args.model,
                    ids_file=ids_path.name, count=len(chunk))
        log(f"batch {batch.id}: submitted {len(chunk)} nights")
    return len(todo)


def stage_teacher_batch_anthropic(args, out):
    import anthropic
    client = anthropic_client(args)
    teacher = AnthropicTeacher(args, client)
    ledger = Ledger(out / "batches.jsonl")
    running = lambda: sum(1 for j in ledger.jobs.values()
                          if not j.get("collected") and j.get("state") != ANTHROPIC_DONE)
    try:
        n = instruction_tokens(client, args.model)
        log(f"teacher: the cached instruction is {n} tokens")
        anthropic_batch_collect(client, ledger, out, args.model)
        submitted = anthropic_batch_submit(client, ledger, out, args, teacher)
        active = running()
        while args.wait and active:
            log(f"batch: {active} batch(es) running; next check in {args.poll_seconds:.0f} s")
            time.sleep(args.poll_seconds)
            anthropic_batch_collect(client, ledger, out, args.model)
            if not running():
                # Nights whose request errored or expired go again.
                submitted += anthropic_batch_submit(client, ledger, out, args, teacher)
            active = running()
    except anthropic.APIStatusError as e:
        if anthropic_refused(e):
            raise SystemExit(f"teacher: the API refused the key or account ({str(e)[:160]})")
        raise
    if active:
        log(f"batch: {active} batch(es) still running; run again to collect "
            f"({submitted} nights submitted this run)")
    return active


# -- 4, 5: check, split, report ----------------------------------------------------

FINISH_OK = (None, "STOP", "end_turn", "stop_sequence")
TOKEN_KINDS = ("input", "cache_write_5m", "cache_write_1h", "cache_read", "output")

# Per 1M tokens: standard (live) prices; the Batch APIs take half of each.
# Anthropic cache prices are multiples of the input price: reads 0.1x,
# 5-minute writes 1.25x, 1-hour writes 2x.
PRICES = {
    "gemini": {"input": 0.30, "output": 2.50},
    "anthropic": {"input": 0.10, "output": 0.50},   # claude-haiku-5-5, prompts <= 100K
}
CACHE_FACTORS = {"cache_read": 0.1, "cache_write_5m": 1.25, "cache_write_1h": 2.0}


def token_mix(usage):
    """A response's usage as billable token kinds, whichever API wrote it."""
    if "promptTokenCount" in usage or "candidatesTokenCount" in usage:   # Gemini
        return {"input": usage.get("promptTokenCount", 0), "cache_write_5m": 0,
                "cache_write_1h": 0, "cache_read": 0,
                "output": usage.get("candidatesTokenCount", 0) + usage.get("thoughtsTokenCount", 0)}
    write = usage.get("cache_creation_input_tokens", 0) or 0
    split = usage.get("cache_creation") or {}
    w1h = split.get("ephemeral_1h_input_tokens", 0) or 0
    w5m = split.get("ephemeral_5m_input_tokens", write - w1h) or 0
    return {"input": usage.get("input_tokens", 0) or 0, "cache_write_5m": w5m,
            "cache_write_1h": w1h, "cache_read": usage.get("cache_read_input_tokens", 0) or 0,
            "output": usage.get("output_tokens", 0) or 0}


def price_table(args, batch=None, defaults_only=False):
    """USD per 1M tokens of each kind. --price-* override the defaults for the
    run's own mode; defaults_only gives the provider's list prices."""
    batch = (args.teacher_mode == "batch") if batch is None else batch
    base = PRICES[args.teacher_provider]
    scale = 0.5 if batch else 1.0
    p_in = base["input"] * scale
    p_out = base["output"] * scale
    if not defaults_only:
        p_in = args.price_in if args.price_in is not None else p_in
        p_out = args.price_out if args.price_out is not None else p_out
    table = {"input": p_in, "output": p_out}
    for kind, factor in CACHE_FACTORS.items():
        given = None if defaults_only else getattr(args, f"price_{kind}")
        table[kind] = given if given is not None else round(p_in * factor, 6)
    return table


def token_report(tin, tout, tok):
    """input = every input token however billed (comparable across providers),
    input_uncached = the part billed at the plain input price."""
    r = lambda v: round(v, 1)
    return {"input": r(tin), "output_incl_thinking": r(tout), "input_uncached": r(tok["input"]),
            **{k: r(tok[k]) for k in ("cache_write_5m", "cache_write_1h", "cache_read")}}


def mix_cost(tokens, prices):
    return sum(tokens[k] * prices[k] for k in TOKEN_KINDS) / 1e6


def as_batch_mix(per, args):
    """The measured live mix as a batch would bill it: Anthropic batches cache
    with the 1-hour TTL, so a 5-minute write becomes a 1-hour write."""
    if args.teacher_provider != "anthropic":
        return per
    mix = dict(per)
    mix["cache_write_1h"] += mix["cache_write_5m"]
    mix["cache_write_5m"] = 0.0
    return mix


def batch_chunk(args):
    if args.teacher_provider == "anthropic":
        return min(args.batch_size, ANTHROPIC_BATCH_MAX)
    return args.batch_size


def sentences(text):
    return [s for s in re.split(r"(?<=[.!?])\s+", text.strip()) if s]


def split_of(nid, val, test):
    h = int(hashlib.sha256(nid.encode()).hexdigest()[:8], 16) / 0xFFFFFFFF
    return "test" if h < test else "val" if h < test + val else "train"


def stage_check_and_split(args, out):
    nights = {n["id"]: n for n in read_jsonl(out / "nights.jsonl")}
    facts = {f["id"]: f["facts"] for f in read_jsonl(out / "facts.jsonl")}
    latest = {}
    for r in read_jsonl(out / "teacher.jsonl"):
        if "text" in r or r["id"] not in latest:
            latest[r["id"]] = r   # an answer outranks an earlier error

    reasons = Counter()
    dropped = []
    candidates = []
    for nid, r in latest.items():
        if "error" in r:
            reasons["teacher_error"] += 1
            dropped.append({"id": nid, "reason": "teacher_error", "detail": r["error"]})
        elif r.get("finish") not in FINISH_OK:
            # Before the empty check: a refusal or a max_tokens cut is dropped
            # under its own reason, with or without text.
            reasons[f"finish_{r['finish']}"] += 1
            dropped.append({"id": nid, "reason": f"finish_{r['finish']}", "text": r["text"]})
        elif not r["text"]:
            reasons["empty"] += 1
            dropped.append({"id": nid, "reason": "empty", "detail": r.get("finish")})
        else:
            candidates.append({"id": nid, "facts": facts[nid], "text": r["text"]})

    checked = {c["id"]: c for c in run_tool(args.tool, "check", candidates)} if candidates else {}
    iso = re.compile(r"^\d{4}-\d{2}-\d{2}$")
    accepted = []
    for c in candidates:
        res = checked[c["id"]]
        if res.get("error"):
            reasons["check_error"] += 1
            dropped.append({"id": c["id"], "reason": "check_error", "detail": res["error"]})
        elif not res["ok"]:
            v = res["violations"]
            reason = "wrong_date" if any(iso.match(x) for x in v) else "ungrounded_number"
            reasons[reason] += 1
            dropped.append({"id": c["id"], "reason": reason, "violations": v, "text": c["text"]})
        else:
            accepted.append(c)

    splits = {"train": [], "val": [], "test": []}
    flags = Counter()
    for c in accepted:
        tags = nights[c["id"]]["tags"]
        n_sent = len(sentences(c["text"]))
        if not 3 <= n_sent <= 5:
            flags["sentences_outside_3_to_5"] += 1
        if tags["machine"] == "ungraded" and GRADING_WORDS.search(c["text"]):
            flags["ungraded_night_with_grading_word"] += 1
        splits[split_of(c["id"], args.val_frac, args.test_frac)].append(
            {"id": c["id"], "prompt": c["facts"], "completion": c["text"], "tags": tags})
    for name, rows in splits.items():
        with open(out / f"{name}.jsonl", "w") as fh:
            for row in rows:
                fh.write(json.dumps(row) + "\n")
    with open(out / "dropped.jsonl", "w") as fh:
        for d in dropped:
            fh.write(json.dumps(d) + "\n")

    # Cost and time, from what the API reported.
    answers = [r for r in latest.values() if r.get("usage")]
    mixes = [token_mix(r["usage"]) for r in answers]
    tok = {k: sum(m[k] for m in mixes) for k in TOKEN_KINDS}
    prices = price_table(args)
    cost = mix_cost(tok, prices)
    tin = tok["input"] + tok["cache_write_5m"] + tok["cache_write_1h"] + tok["cache_read"]
    tout = tok["output"]
    runs = read_jsonl(out / "teacher_runs.jsonl")
    secs = sum(r["seconds"] for r in runs)
    asked = sum(r["asked"] for r in runs)
    total = len(latest)
    report = {
        "provider": args.teacher_provider,
        "model": args.model,
        "nights": len(nights), "answered": total,
        "accepted": len(accepted), "dropped": len(dropped),
        "drop_rate": round(len(dropped) / total, 4) if total else None,
        "drop_reasons": dict(reasons),
        "flags_on_accepted": dict(flags),
        "splits": {k: len(v) for k, v in splits.items()},
        "tokens": token_report(tin, tout, tok),
        "price_per_1m": prices,
        "cost_usd": round(cost, 4),
        "cost_per_answer_usd": round(cost / len(answers), 6) if answers else None,
        "cost_per_accepted_pair_usd": round(cost / len(accepted), 6) if accepted else None,
        "teacher_seconds": round(secs, 1), "teacher_asked": asked,
        "answers_per_second": round(asked / secs, 3) if secs else None,
    }
    if args.teacher_provider == "anthropic" and answers:
        hits = sum(1 for m in mixes if m["cache_read"] > 0)
        writes = sum(1 for m in mixes if m["cache_write_5m"] or m["cache_write_1h"])
        report["cache"] = {
            "requests_reading_cache": hits,
            "requests_writing_cache": writes,
            "hit_rate": round(hits / len(mixes), 4),
            "input_share_read_from_cache": round(tok["cache_read"] / tin, 4) if tin else None,
        }
    jobs = [j for j in Ledger(out / "batches.jsonl").jobs.values() if j.get("collected")]
    if jobs:
        report["batch_jobs"] = [{k: j.get(k) for k in ("job", "count", "answers", "created", "ended")}
                                for j in jobs]
    if accepted and answers:
        need = args.project_pairs / (len(accepted) / total)
        per = {k: tok[k] / len(answers) for k in TOKEN_KINDS}
        batch_prices = price_table(args, batch=True, defaults_only=True)
        report["projection"] = {
            "pairs": args.project_pairs, "requests": round(need),
            "tokens_per_answer": token_report(tin / len(answers), tout / len(answers), per),
            "cost_usd_at_these_prices": round(need * cost / len(answers), 2),
            "batch_price_per_1m": batch_prices,
            "cost_usd_batch_api": round(need * mix_cost(as_batch_mix(per, args), batch_prices), 2),
            "batch_jobs_needed": math.ceil(need / batch_chunk(args)),
        }
        if args.teacher_provider == "anthropic":
            # A batch request may miss the cache: the dearest case writes the
            # whole instruction (1-hour TTL) on every request.
            worst = dict(per)
            worst["cache_write_1h"] = per["cache_write_5m"] + per["cache_write_1h"] + per["cache_read"]
            worst["cache_write_5m"] = worst["cache_read"] = 0.0
            report["projection"]["cost_usd_batch_api_if_no_cache_hits"] = round(
                need * mix_cost(worst, batch_prices), 2)
        if secs:
            report["projection"]["hours_live_at_measured_rate"] = round(need / (asked / secs) / 3600, 2)
    with open(out / "report.json", "w") as fh:
        json.dump(report, fh, indent=1)
    log(json.dumps(report, indent=1))


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--out", required=True, type=Path)
    ap.add_argument("--count", type=int, required=True)
    ap.add_argument("--seed", type=int, default=13)
    ap.add_argument("--distributions", type=Path, default=HERE / "data" / "distributions.json")
    ap.add_argument("--tool", type=Path, default=DEFAULT_TOOL)
    ap.add_argument("--teacher-provider", choices=tuple(DEFAULT_MODEL), default="anthropic",
                    help="anthropic (ANTHROPIC_API_KEY) or gemini (GOOGLE_AI_API_KEY)")
    ap.add_argument("--model", default=None,
                    help="default: claude-haiku-5-5 (anthropic), gemini-3.5-flash-lite (gemini)")
    ap.add_argument("--thinking-level", default="minimal",
                    help="gemini: thinkingConfig.thinkingLevel ('' sends none)")
    ap.add_argument("--temperature", type=float, default=0.7,
                    help="gemini only (Claude Haiku 5.5 takes no sampling parameters)")
    ap.add_argument("--max-tokens", type=int, default=400)
    ap.add_argument("--concurrency", type=int, default=4)
    ap.add_argument("--rpm", type=float, default=300)
    ap.add_argument("--attempts", type=int, default=5)
    ap.add_argument("--timeout", type=float, default=60)
    ap.add_argument("--teacher-mode", choices=("live", "batch"), default="live",
                    help="live calls (the pilot) or Batch API jobs (the full corpus)")
    ap.add_argument("--batch-size", type=int, default=50_000,
                    help="nights per batch job (anthropic: at most 100,000 per Message Batch)")
    ap.add_argument("--wait", action="store_true",
                    help="batch: keep checking until every job is collected")
    ap.add_argument("--poll-seconds", type=float, default=600,
                    help="batch: seconds between job checks while --wait runs")
    ap.add_argument("--price-in", type=float, default=None,
                    help="USD per 1M input tokens (default anthropic 0.10 live, 0.05 batch; "
                         "gemini 0.30 live, 0.15 batch)")
    ap.add_argument("--price-out", type=float, default=None,
                    help="USD per 1M output tokens (default anthropic 0.50 live, 0.25 batch; "
                         "gemini 2.50 live, 1.25 batch)")
    ap.add_argument("--price-cache-read", type=float, default=None,
                    help="USD per 1M cache-read tokens (default 0.1x the input price)")
    ap.add_argument("--price-cache-write-5m", type=float, default=None,
                    help="USD per 1M 5-minute cache-write tokens (default 1.25x the input price)")
    ap.add_argument("--price-cache-write-1h", type=float, default=None,
                    help="USD per 1M 1-hour cache-write tokens (default 2x the input price)")
    ap.add_argument("--val-frac", type=float, default=0.01)
    ap.add_argument("--test-frac", type=float, default=0.01)
    ap.add_argument("--project-pairs", type=int, default=250_000)
    ap.add_argument("--stages", default="nights,facts,teacher,check",
                    help="comma list of nights,facts,teacher,check")
    args = ap.parse_args()
    args.model = args.model or DEFAULT_MODEL[args.teacher_provider]
    batch = args.teacher_mode == "batch"

    args.out.mkdir(parents=True, exist_ok=True)
    stages = set(args.stages.split(","))
    if "nights" in stages:
        stage_nights(args, args.out)
    if "facts" in stages:
        stage_facts(args, args.out)
    if "teacher" in stages:
        if batch:
            if stage_teacher_batch(args, args.out):
                return   # jobs still running: check and split once they are in
        else:
            stage_teacher(args, args.out)
    if "check" in stages:
        stage_check_and_split(args, args.out)


if __name__ == "__main__":
    main()
