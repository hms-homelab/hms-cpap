#!/usr/bin/env python3
"""SDD-013: offline tests of the corpus teacher's request construction, answer
handling and cost math, against a fake Anthropic client (no network, no key).

    .venv/bin/python -m unittest test_build_corpus -v
"""

import argparse
import json
import tempfile
import unittest
from pathlib import Path
from types import SimpleNamespace as NS

import anthropic
import httpx2 as httpx   # the HTTP library of the anthropic SDK 1.x+

import build_corpus as bc

FACTS = "Night: 2027-06-24\nUsage: 11.00 hours (137.50% of 8h target)\nAHI: 14.27 events/hour\n"


def args(**over):
    a = dict(teacher_provider="anthropic", teacher_mode="live", model="claude-haiku-5-5",
             max_tokens=400, rpm=6000, attempts=3, timeout=60, batch_size=50_000,
             price_in=None, price_out=None, price_cache_read=None,
             price_cache_write_5m=None, price_cache_write_1h=None,
             thinking_level="minimal", temperature=0.7)
    a.update(over)
    return argparse.Namespace(**a)


def message(text="On 2027-06-24 you slept.", stop="end_turn", usage=None):
    usage = usage or {"input_tokens": 300, "cache_creation_input_tokens": 0,
                      "cache_read_input_tokens": 900, "output_tokens": 80}
    return NS(stop_reason=stop, model="claude-haiku-5-5",
              content=[NS(type="text", text=text)] if text is not None else [],
              usage=NS(model_dump=lambda exclude_none=True: dict(usage)))


def api_error(cls, status, msg="nope"):
    resp = httpx.Response(status, request=httpx.Request("POST", "https://api.anthropic.com/v1/messages"))
    return cls(msg, response=resp, body=None)


class FakeMessages:
    def __init__(self, replies):
        self.replies = list(replies)
        self.sent = []
        self.batches = FakeBatches()

    def create(self, **params):
        self.sent.append(params)
        r = self.replies.pop(0)
        if isinstance(r, Exception):
            raise r
        return r


class FakeBatches:
    def __init__(self):
        self.created = []
        self.status = "in_progress"
        self.results_list = []

    def create(self, requests):
        self.created.append(requests)
        return NS(id=f"msgbatch_{len(self.created)}", processing_status="in_progress")

    def retrieve(self, bid):
        return NS(id=bid, processing_status=self.status, created_at="t0", ended_at="t1")

    def results(self, bid):
        return iter(self.results_list)


class RequestShape(unittest.TestCase):
    def test_live_params(self):
        p = bc.anthropic_params("claude-haiku-5-5", FACTS, 400)
        self.assertEqual(p["model"], "claude-haiku-5-5")
        self.assertEqual(p["thinking"], {"type": "disabled"})
        self.assertEqual(p["output_config"], {"effort": "low"})
        for banned in ("temperature", "top_p", "top_k"):
            self.assertNotIn(banned, p)
        self.assertEqual(len(p["system"]), 1)
        self.assertEqual(p["system"][0]["text"], bc.TEACHER_INSTRUCTION)
        self.assertEqual(p["system"][0]["cache_control"], {"type": "ephemeral"})
        # One user turn holding the night; no assistant prefill.
        self.assertEqual([m["role"] for m in p["messages"]], ["user"])
        self.assertIn(FACTS, p["messages"][0]["content"])
        self.assertNotIn(FACTS, p["system"][0]["text"])

    def test_batch_params_cache_one_hour(self):
        p = bc.anthropic_params("claude-haiku-5-5", FACTS, 400, ttl="1h")
        self.assertEqual(p["system"][0]["cache_control"], {"type": "ephemeral", "ttl": "1h"})

    def test_system_identical_across_nights(self):
        a = bc.anthropic_params("m", FACTS, 400)["system"]
        b = bc.anthropic_params("m", FACTS.replace("2027-06-24", "2025-01-01"), 400)["system"]
        self.assertEqual(a, b)

    def test_sdk_accepts_batch_request(self):
        from anthropic.types.message_create_params import MessageCreateParamsNonStreaming
        from anthropic.types.messages.batch_create_params import Request
        r = Request(custom_id="n0000001",
                    params=MessageCreateParamsNonStreaming(**bc.anthropic_params("m", FACTS, 400, "1h")))
        self.assertEqual(r["custom_id"], "n0000001")

    def test_instruction_has_no_em_dash_and_no_band_phrases(self):
        t = bc.TEACHER_INSTRUCTION
        self.assertNotIn("—", t)
        for ex in t.split("Summary: ")[1:]:
            summary = ex.split("\n")[0]
            for bad in ("5 to 15", "15 to 30", "under 5", "4-hour", "minimum"):
                self.assertNotIn(bad, summary)

    def test_gemini_body_still_built(self):
        t = bc.Teacher(args(teacher_provider="gemini", model="gemini-3.5-flash-lite"), "k")
        body = t.body(FACTS)
        text = body["contents"][0]["parts"][0]["text"]
        self.assertTrue(text.startswith(bc.TEACHER_INSTRUCTION))
        self.assertIn(FACTS, text)
        self.assertEqual(body["generationConfig"]["thinkingConfig"], {"thinkingLevel": "minimal"})


class LiveCalls(unittest.TestCase):
    def call(self, reply):
        fake = NS(messages=FakeMessages([reply]))
        return bc.AnthropicTeacher(args(), fake).call({"id": "n1", "facts": FACTS}), fake

    def test_text_answer(self):
        res, fake = self.call(message())
        self.assertEqual(res["text"], "On 2027-06-24 you slept.")
        self.assertEqual(res["finish"], "end_turn")
        self.assertEqual(res["usage"]["cache_read_input_tokens"], 900)
        self.assertNotIn("temperature", fake.messages.sent[0])

    def test_refusal_and_max_tokens_keep_their_reason(self):
        for stop in ("refusal", "max_tokens"):
            res, _ = self.call(message(text="half a sente", stop=stop))
            self.assertEqual(res["finish"], stop)
            self.assertEqual(res["text"], "")
            self.assertNotIn(res["finish"], bc.FINISH_OK)

    def test_bad_key_stops_the_run(self):
        with self.assertRaises(bc.KeyRejected):
            self.call(api_error(anthropic.AuthenticationError, 401))

    def test_no_credit_stops_the_run(self):
        with self.assertRaises(bc.KeyRejected):
            self.call(api_error(anthropic.BadRequestError, 400,
                                "Your credit balance is too low to access the Anthropic API."))

    def test_other_400_is_one_nights_error(self):
        res, _ = self.call(api_error(anthropic.BadRequestError, 400, "bad"))
        self.assertTrue(res["error"].startswith("http 400"))


class Batch(unittest.TestCase):
    def test_submit_then_collect_out_of_order(self):
        with tempfile.TemporaryDirectory() as d:
            out = Path(d)
            with open(out / "facts.jsonl", "w") as fh:
                for i in range(3):
                    fh.write(json.dumps({"id": f"n{i}", "facts": FACTS}) + "\n")
            a = args(teacher_mode="batch", batch_size=2)
            fake = NS(messages=FakeMessages([]))
            ledger = bc.Ledger(out / "batches.jsonl")
            teacher = bc.AnthropicTeacher(a, fake)
            self.assertEqual(bc.anthropic_batch_submit(fake, ledger, out, a, teacher), 3)
            created = fake.messages.batches.created
            self.assertEqual([len(c) for c in created], [2, 1])
            sys_block = created[0][0]["params"]["system"][0]
            self.assertEqual(sys_block["cache_control"]["ttl"], "1h")
            # Held nights are not submitted again.
            self.assertEqual(bc.anthropic_batch_submit(fake, ledger, out, a, teacher), 0)

            b = fake.messages.batches
            b.status = "ended"
            b.results_list = [
                NS(custom_id="n2", result=NS(type="succeeded", message=message(text="two"))),
                NS(custom_id="n0", result=NS(type="expired")),
                NS(custom_id="n1", result=NS(type="succeeded",
                                            message=message(text="x", stop="refusal"))),
            ]
            bc.anthropic_batch_collect(fake, ledger, out, a.model)
            recs = {r["id"]: r for r in bc.read_jsonl(out / "teacher.jsonl")}
            self.assertEqual(recs["n2"]["text"], "two")
            self.assertEqual(recs["n0"]["error"], "expired")
            self.assertEqual(recs["n1"]["finish"], "refusal")
            self.assertTrue(all(j.get("collected") for j in ledger.jobs.values()))


class Cost(unittest.TestCase):
    def test_anthropic_prices(self):
        live = bc.price_table(args())
        self.assertEqual(live["input"], 0.10)
        self.assertEqual(live["output"], 0.50)
        self.assertAlmostEqual(live["cache_read"], 0.01)
        self.assertAlmostEqual(live["cache_write_5m"], 0.125)
        self.assertAlmostEqual(live["cache_write_1h"], 0.20)
        batch = bc.price_table(args(teacher_mode="batch"))
        self.assertEqual((batch["input"], batch["output"]), (0.05, 0.25))
        self.assertAlmostEqual(batch["cache_read"], 0.005)
        self.assertAlmostEqual(batch["cache_write_1h"], 0.10)

    def test_gemini_prices_unchanged(self):
        self.assertEqual(bc.price_table(args(teacher_provider="gemini"))["input"], 0.30)
        self.assertEqual(bc.price_table(args(teacher_provider="gemini", teacher_mode="batch"))["output"], 1.25)

    def test_token_mix_and_cost(self):
        m = bc.token_mix({"input_tokens": 300, "cache_creation_input_tokens": 0,
                          "cache_read_input_tokens": 900, "output_tokens": 80})
        self.assertEqual(m["cache_read"], 900)
        cost = bc.mix_cost(m, bc.price_table(args()))
        self.assertAlmostEqual(cost, (300 * 0.10 + 900 * 0.01 + 80 * 0.50) / 1e6)
        w = bc.token_mix({"input_tokens": 300, "cache_creation_input_tokens": 900,
                          "cache_creation": {"ephemeral_1h_input_tokens": 900,
                                             "ephemeral_5m_input_tokens": 0},
                          "cache_read_input_tokens": 0, "output_tokens": 80})
        self.assertEqual((w["cache_write_1h"], w["cache_write_5m"]), (900, 0))

    def test_gemini_usage(self):
        m = bc.token_mix({"promptTokenCount": 800, "candidatesTokenCount": 150, "thoughtsTokenCount": 40})
        self.assertEqual((m["input"], m["output"], m["cache_read"]), (800, 190, 0))


if __name__ == "__main__":
    unittest.main()
