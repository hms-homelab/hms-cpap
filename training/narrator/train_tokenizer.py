#!/usr/bin/env python3
"""SDD-013: the narrator's BPE tokenizer and the corpus token histogram.

Trains a sentencepiece BPE model on the corpus (fact blocks and summaries of
train.jsonl), and writes the histogram the vocabulary size is decided from
(SDD-013 open question 1): for each candidate size, tokens per pair, how much
of the corpus the most frequent tokens cover, and how many tokens the corpus
barely uses. The default is 8k; the histogram can move it.

Digits are split one per token, so a figure is spelled digit by digit and the
model copies it from the facts instead of learning "7.25" as one symbol.

The training text is laid out the way the model is trained (train_colab.ipynb):
    <s>{fact block, line breaks as <n>}<|summary|>{summary}</s>

Usage:
    python train_tokenizer.py --corpus out/full --vocab 8000 --compare 4000,16000
"""

import argparse
import json
import sys
import tempfile
from collections import Counter
from pathlib import Path

import numpy as np
import sentencepiece as spm

SEPARATOR = "<|summary|>"
NEWLINE = "<n>"   # a fact block's line break, one token
COVERAGE_POINTS = [0.5, 0.9, 0.95, 0.99, 0.999]
RARE_BELOW = [1, 5, 20]


def corpus_texts(corpus):
    rows = []
    with open(corpus / "train.jsonl") as fh:
        for line in fh:
            r = json.loads(line)
            rows.append((r["prompt"], r["completion"]))
    return rows


def train(rows, vocab, prefix):
    with tempfile.NamedTemporaryFile("w", suffix=".txt", delete=False) as fh:
        for prompt, completion in rows:
            # sentencepiece reads lines; the fact block's newlines are kept as
            # one symbol the model learns (and the runtime restores).
            fh.write(prompt.replace("\n", NEWLINE) + "\n")
            fh.write(completion.replace("\n", " ") + "\n")
        text_path = fh.name
    spm.SentencePieceTrainer.train(
        input=text_path, model_prefix=str(prefix), model_type="bpe", vocab_size=vocab,
        character_coverage=1.0, split_digits=True, byte_fallback=True,
        pad_id=0, unk_id=1, bos_id=2, eos_id=3, user_defined_symbols=[SEPARATOR, NEWLINE],
        normalization_rule_name="identity", remove_extra_whitespaces=False,
        input_sentence_size=2_000_000, shuffle_input_sentence=True,
        max_sentence_length=16384, num_threads=4, minloglevel=2)
    Path(text_path).unlink()
    return spm.SentencePieceProcessor(model_file=f"{prefix}.model")


def histogram(sp, rows, vocab):
    counts = Counter()
    prompt_len, completion_len = [], []
    for prompt, completion in rows:
        p = sp.encode(prompt.replace("\n", NEWLINE))
        c = sp.encode(completion)
        counts.update(p)
        counts.update(c)
        prompt_len.append(len(p))
        completion_len.append(len(c))
    freq = np.array(sorted(counts.values(), reverse=True), dtype=float)
    total = freq.sum()
    cum = np.cumsum(freq) / total
    pair = np.array(prompt_len) + np.array(completion_len) + 3   # <s>, separator, </s>
    q = lambda a: {f"p{p}": int(np.percentile(a, p)) for p in (50, 90, 99, 100)}
    return {
        "vocab": vocab,
        "tokens_used": len(counts),
        "unused_tokens": vocab - len(counts),
        "tokens_in_corpus": int(total),
        "tokens_per_pair": q(pair),
        "prompt_tokens": q(prompt_len),
        "completion_tokens": q(completion_len),
        "top_tokens_for_coverage": {str(c): int(np.searchsorted(cum, c) + 1)
                                    for c in COVERAGE_POINTS},
        "tokens_seen_fewer_than": {str(n): int((freq < n).sum()) + (vocab - len(counts))
                                   for n in RARE_BELOW},
        "frequency_by_rank": [int(freq[i]) for i in
                              (0, 9, 99, 499, 999, 1999, 3999, 7999, 15999) if i < len(freq)],
    }


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--corpus", required=True, type=Path, help="dir holding train.jsonl")
    ap.add_argument("--vocab", type=int, default=8000)
    ap.add_argument("--compare", default="4000,16000",
                    help="other vocabulary sizes to put in the histogram ('' for none)")
    ap.add_argument("--out", type=Path, default=None, help="default: <corpus>/tokenizer")
    args = ap.parse_args()

    out = args.out or args.corpus / "tokenizer"
    out.mkdir(parents=True, exist_ok=True)
    rows = corpus_texts(args.corpus)
    sizes = [args.vocab] + [int(s) for s in args.compare.split(",") if s]
    report = {"pairs": len(rows), "separator": SEPARATOR, "sizes": []}
    for vocab in sizes:
        prefix = out / f"narrator-bpe-{vocab}"
        try:
            sp = train(rows, vocab, prefix)
        except RuntimeError as e:   # a corpus too small for this many pieces
            report["sizes"].append({"vocab": vocab, "error": str(e).splitlines()[-1]})
            continue
        report["sizes"].append(histogram(sp, rows, vocab))
        if vocab != args.vocab:
            for ext in (".model", ".vocab"):
                Path(f"{prefix}{ext}").unlink()
    with open(out / "token_histogram.json", "w") as fh:
        json.dump(report, fh, indent=1)
    print(json.dumps(report, indent=1), file=sys.stderr)


if __name__ == "__main__":
    main()
