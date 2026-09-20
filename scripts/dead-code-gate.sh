#!/usr/bin/env bash
#
# SDD-045 D6: fail on NEW dead-code candidates, never on what is already known.
#
# The gate deliberately does NOT try to decide what is dead. cppcheck
# --enable=unusedFunction produced 111 hits on this repo and 8 of them were
# real, because it cannot see a caller it did not parse: not the gtest files
# (macro bodies), and not the Qt layer, which builds in a separate tree absent
# from the main compile database. Every function whose only callers are tests,
# and every pure supervisor function the Qt UI calls, came back as "never used".
#
# Automating that judgement is where this would go wrong, so it is not
# automated. The baseline lists EVERY candidate the tools currently raise. A new
# one fails the gate, and a person decides which it is:
#
#   dead            -> delete it, and the candidate disappears on its own
#   test seam       -> baseline it, saying which suite needs it
#   API surface     -> baseline it, saying who is expected to call it
#   platform-only   -> baseline it, saying which build reaches it
#
# That way the gate cannot be wrong about your code; it can only tell you
# something new appeared that nobody has looked at yet.
#
# Usage:
#   scripts/dead-code-gate.sh                 # check against the baseline
#   scripts/dead-code-gate.sh --update        # rewrite the baseline deliberately
#
set -uo pipefail

cd "$(dirname "$0")/.."
BASELINE="scripts/dead-code-baseline.txt"
UPDATE=0
[ "${1:-}" = "--update" ] && UPDATE=1

if ! command -v cppcheck >/dev/null 2>&1; then
    echo "dead-code-gate: cppcheck not found (brew install cppcheck / apt-get install cppcheck)" >&2
    exit 2
fi

DB=build/compile_commands.json
if [ ! -f "$DB" ]; then
    echo "dead-code-gate: $DB missing; run cmake -S . -B build first" >&2
    exit 2
fi

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

# Two runs, because there are two build trees. -j is deliberately absent:
# cppcheck SILENTLY DISABLES unusedFunction when threaded, which is how an
# earlier run of this check reported zero findings and looked like good news.
SUPPRESS=(--suppress=missingInclude --suppress=missingIncludeSystem
          --suppress=syntaxError --suppress=internalAstError
          --inline-suppr --quiet)

cppcheck --enable=unusedFunction --project="$DB" "${SUPPRESS[@]}" \
         2> "$WORK/cc-main.txt" || true

# The Qt supervisor, whose sources the main database does not contain. Missing
# Qt headers are fine here: the symbol names are what matter, not a full parse.
cppcheck --enable=unusedFunction desktop/qt -I desktop/qt -I include \
         "${SUPPRESS[@]}" 2> "$WORK/cc-qt.txt" || true

# First-party only. Third-party headers pulled in by FetchContent are not ours.
cat "$WORK/cc-main.txt" "$WORK/cc-qt.txt" \
  | grep "unusedFunction" \
  | grep -E "^(src|include|tests|desktop)/" \
  | sed -E "s|^([^:]+):[0-9]+:[0-9]+:.*The function '([A-Za-z0-9_]+)'.*|\2\t\1|" \
  | sort -u > "$WORK/candidates.tsv"

cut -f1 "$WORK/candidates.tsv" | sort -u > "$WORK/candidates.txt"

if [ "$UPDATE" = "1" ]; then
    # MERGE, never replace. The candidate list is platform-dependent: a Linux
    # cppcheck raises gtest SetUp/TearDown and the QWizardPage virtuals that a
    # macOS run does not, and vice versa. A replacing --update run on one
    # machine would silently drop the other machine's entries and turn CI red
    # for something nobody changed -- which is exactly what happened on the
    # v5.4.2 run. Dropping an entry is therefore a deliberate edit of this file.
    {
        echo "# SDD-045 D6 baseline: every symbol the tools currently raise as"
        echo "# uncalled, on ANY platform. Most are NOT dead -- see"
        echo "# scripts/dead-code-gate.sh for why the gate does not try to tell"
        echo "# the difference."
        echo "#"
        echo "# A new entry appearing means nobody has classified it yet. Delete the"
        echo "# code, or add it here with a note saying what reaches it."
        echo "#"
        echo "# --update MERGES: it adds what this machine sees and keeps what it"
        echo "# does not, because the list differs between macOS and Linux. To"
        echo "# remove an entry, delete the line by hand."
        echo "#"
        echo "# Last touched $(date -u +%Y-%m-%d) on $(uname -s) from $(git rev-parse --short HEAD 2>/dev/null || echo 'a dirty tree')"
        echo
        {
            # Existing lines keep their note; new ones get the file they came from.
            sed 's/#.*//' "$BASELINE" 2>/dev/null | tr -d ' \t' | grep -v '^$' \
              | while read -r n; do
                    old=$(grep -E "^$n[[:space:]]" "$BASELINE" 2>/dev/null | head -1)
                    if [ -n "$old" ]; then echo "$old"; else printf '%-36s\n' "$n"; fi
                done
            while IFS=$'\t' read -r name file; do
                grep -qE "^$name[[:space:]]" "$BASELINE" 2>/dev/null || \
                    printf '%-36s # %s\n' "$name" "$file"
            done < "$WORK/candidates.tsv"
        } | sort -u
    } > "$WORK/baseline.new"
    mv "$WORK/baseline.new" "$BASELINE"
    echo "dead-code-gate: baseline merged, $(sed 's/#.*//' "$BASELINE" | tr -d ' \t' | grep -cv '^$') symbols"
    exit 0
fi

touch "$BASELINE"
sed 's/#.*//' "$BASELINE" | tr -d ' \t' | grep -v '^$' | sort -u > "$WORK/known.txt"

NEW=$(comm -23 "$WORK/candidates.txt" "$WORK/known.txt")
GONE=$(comm -13 "$WORK/candidates.txt" "$WORK/known.txt")

if [ -n "$GONE" ]; then
    # Informational only, and never a failure: on macOS this lists the entries
    # only a Linux cppcheck raises (and the reverse on Linux). It is worth
    # printing because it also catches an entry whose code was deleted, but you
    # have to look at which before removing a line.
    echo "dead-code-gate: baseline entries this platform ($(uname -s)) does not raise."
    echo "Either another platform raises them, or the code is gone; check before removing:"
    echo "$GONE" | sed 's/^/    /'
    echo
fi

if [ -n "$NEW" ]; then
    echo "dead-code-gate: NEW uncalled symbols, not yet classified:" >&2
    while read -r n; do
        [ -z "$n" ] && continue
        printf '    %-36s %s\n' "$n" "$(awk -F'\t' -v n="$n" '$1==n {print $2; exit}' "$WORK/candidates.tsv")" >&2
    done <<< "$NEW"
    echo >&2
    echo "Each is dead, a test seam, API surface, or platform-only code. Delete it," >&2
    echo "or run scripts/dead-code-gate.sh --update and note what reaches it." >&2
    exit 1
fi

echo "dead-code-gate: clean ($(wc -l < "$WORK/candidates.txt" | tr -d ' ') candidates, all classified)"
