#!/usr/bin/env python3
"""SDD-013: the narrator corpus, night facts -> summary pairs.

Stages, each resumable (a half-done run continues where it stopped):

  1. nights   generate_nights.py writes the synthetic nights (seeded, so a
              resumed run regenerates the same ones if the file is missing).
  2. facts    hms_cpap_narrator_facts render: each night through the
              runtime's NightFacts::from() and model::factBlock(), so the
              corpus input is byte for byte what the model sees at runtime.
  3. teacher  Gemini Flash-Lite, prompt = llm_prompt.txt (the machine-neutral
              behavioural target) with the fact block as {metrics}, plus the
              corpus instruction below. Batched concurrency with a
              requests-per-minute limit; every answer is appended to
              teacher.jsonl as it arrives, and a resumed run asks only for the
              nights not yet answered.
  4. check    hms_cpap_narrator_facts check: model::violations(), SDD-072's
              number check plus the ISO date check, the same check the nightly
              job applies to the model. A failing answer is dropped; the drop
              rate and reasons go to report.json.
  5. split    train / val / test JSONL by a hash of the night id.

The API key is read from the environment (GOOGLE_AI_API_KEY) and never
written anywhere.

Usage:
    python build_corpus.py --out out/pilot --count 500
    python build_corpus.py --out out/full --count 250000 --concurrency 16 --rpm 3000
"""

import argparse
import concurrent.futures as cf
import hashlib
import json
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
DEFAULT_PROMPT = REPO / "llm_prompt.txt"
API = "https://generativelanguage.googleapis.com/v1beta/models/{model}:generateContent"

# Added after llm_prompt.txt for the corpus: the shape the student must learn.
CORPUS_INSTRUCTION = """
Write in English, as plain prose: 3 to 5 sentences, no lists, no headings.
Describe the night; do not give medical advice and do not tell the reader to
change settings or equipment. If the data says no AHI is available, do not
grade the index or the events in any way. Write any date exactly as the data
writes it."""

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
    """The API refused the key: no further request can succeed."""


class Teacher:
    def __init__(self, args, key):
        self.args = args
        self.key = key
        self.url = API.format(model=args.model)
        self.limit = RateLimit(args.rpm)
        self.thinking = args.thinking_level or None
        self.template = Path(args.prompt).read_text()
        if "{metrics}" not in self.template:
            raise SystemExit(f"{args.prompt} has no {{metrics}} placeholder")

    def prompt(self, facts):
        return self.template.replace("{metrics}", facts) + "\n" + CORPUS_INSTRUCTION.strip() + "\n"

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
                if e.code in (401, 403) or "API_KEY_INVALID" in detail:
                    raise KeyRejected(last)
                return {"id": rec["id"], "error": last}
            except (urllib.error.URLError, TimeoutError, OSError) as e:
                last = f"network: {e}"
                time.sleep(min(60.0, 2.0 ** attempt + random.random()))
                continue
            cands = data.get("candidates") or []
            usage = data.get("usageMetadata", {})
            if not cands:
                return {"id": rec["id"], "error": "no candidate: " +
                        json.dumps(data.get("promptFeedback", {}))[:200], "usage": usage}
            parts = cands[0].get("content", {}).get("parts", [])
            text = "".join(p.get("text", "") for p in parts if not p.get("thought")).strip()
            return {"id": rec["id"], "text": text, "finish": cands[0].get("finishReason"),
                    "usage": usage, "model": data.get("modelVersion", self.args.model),
                    "seconds": round(time.monotonic() - t0, 3)}
        return {"id": rec["id"], "error": f"gave up after {self.args.attempts} tries: {last}"}


def stage_teacher(args, out):
    key = os.environ.get("GOOGLE_AI_API_KEY", "")
    if not key:
        raise SystemExit("GOOGLE_AI_API_KEY is not set")
    path = out / "teacher.jsonl"
    facts = read_jsonl(out / "facts.jsonl")
    answered = {r["id"] for r in read_jsonl(path) if "text" in r}
    todo = [f for f in facts if f["id"] not in answered]
    log(f"teacher: {len(answered)} answered, {len(todo)} to ask ({args.model}, "
        f"concurrency {args.concurrency}, {args.rpm} rpm)")
    if not todo:
        return
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
                raise SystemExit(f"teacher: the API rejected the key ({str(e)[:120]})")
            fh.write(json.dumps(res) + "\n")
            fh.flush()
            done += 1
            if done % 100 == 0 or done == len(todo):
                rate = done / max(time.monotonic() - t0, 1e-9)
                log(f"teacher: {done}/{len(todo)} ({rate:.2f}/s)")
    with open(out / "teacher_runs.jsonl", "a") as fh:
        fh.write(json.dumps({"asked": len(todo), "seconds": round(time.monotonic() - t0, 2),
                             "concurrency": args.concurrency, "rpm": args.rpm,
                             "model": args.model}) + "\n")


# -- 4, 5: check, split, report ----------------------------------------------------

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
        elif not r["text"]:
            reasons["empty"] += 1
            dropped.append({"id": nid, "reason": "empty", "detail": r.get("finish")})
        elif r.get("finish") not in (None, "STOP"):
            reasons[f"finish_{r['finish']}"] += 1
            dropped.append({"id": nid, "reason": f"finish_{r['finish']}", "text": r["text"]})
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
    answers = [r for r in latest.values() if "usage" in r]
    tin = sum(r["usage"].get("promptTokenCount", 0) for r in answers)
    tout = sum(r["usage"].get("candidatesTokenCount", 0) + r["usage"].get("thoughtsTokenCount", 0)
               for r in answers)
    cost = tin / 1e6 * args.price_in + tout / 1e6 * args.price_out
    runs = read_jsonl(out / "teacher_runs.jsonl")
    secs = sum(r["seconds"] for r in runs)
    asked = sum(r["asked"] for r in runs)
    total = len(latest)
    report = {
        "model": args.model,
        "nights": len(nights), "answered": total,
        "accepted": len(accepted), "dropped": len(dropped),
        "drop_rate": round(len(dropped) / total, 4) if total else None,
        "drop_reasons": dict(reasons),
        "flags_on_accepted": dict(flags),
        "splits": {k: len(v) for k, v in splits.items()},
        "tokens": {"input": tin, "output_incl_thinking": tout},
        "price_per_1m": {"input": args.price_in, "output": args.price_out},
        "cost_usd": round(cost, 4),
        "cost_per_answer_usd": round(cost / len(answers), 6) if answers else None,
        "cost_per_accepted_pair_usd": round(cost / len(accepted), 6) if accepted else None,
        "teacher_seconds": round(secs, 1), "teacher_asked": asked,
        "answers_per_second": round(asked / secs, 3) if secs else None,
    }
    if accepted and answers and secs:
        need = args.project_pairs / (len(accepted) / total)
        report["projection"] = {
            "pairs": args.project_pairs, "requests": round(need),
            "cost_usd": round(need * cost / len(answers), 2),
            "hours_at_measured_rate": round(need / (asked / secs) / 3600, 2),
            "hours_at_rpm_limit": round(need / args.rpm / 60, 2),
        }
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
    ap.add_argument("--prompt", type=Path, default=DEFAULT_PROMPT)
    ap.add_argument("--model", default="gemini-3.5-flash-lite")
    ap.add_argument("--thinking-level", default="minimal",
                    help="thinkingConfig.thinkingLevel ('' sends none)")
    ap.add_argument("--temperature", type=float, default=0.7)
    ap.add_argument("--max-tokens", type=int, default=400)
    ap.add_argument("--concurrency", type=int, default=4)
    ap.add_argument("--rpm", type=float, default=300)
    ap.add_argument("--attempts", type=int, default=5)
    ap.add_argument("--timeout", type=float, default=60)
    ap.add_argument("--price-in", type=float, default=0.30, help="USD per 1M input tokens")
    ap.add_argument("--price-out", type=float, default=2.50, help="USD per 1M output tokens")
    ap.add_argument("--val-frac", type=float, default=0.01)
    ap.add_argument("--test-frac", type=float, default=0.01)
    ap.add_argument("--project-pairs", type=int, default=250_000)
    ap.add_argument("--stages", default="nights,facts,teacher,check",
                    help="comma list of nights,facts,teacher,check")
    args = ap.parse_args()

    args.out.mkdir(parents=True, exist_ok=True)
    stages = set(args.stages.split(","))
    if "nights" in stages:
        stage_nights(args, args.out)
    if "facts" in stages:
        stage_facts(args, args.out)
    if "teacher" in stages:
        stage_teacher(args, args.out)
    if "check" in stages:
        stage_check_and_split(args, args.out)


if __name__ == "__main__":
    main()
