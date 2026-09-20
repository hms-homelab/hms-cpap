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
    {
        echo "# SDD-045 D6 baseline: every symbol the tools currently raise as"
        echo "# uncalled. Most are NOT dead -- see scripts/dead-code-gate.sh for why"
        echo "# the gate does not try to tell the difference."
        echo "#"
        echo "# A new entry appearing means nobody has classified it yet. Delete the"
        echo "# code, or add it here with a note saying what reaches it."
        echo "#"
        echo "# Regenerate:  scripts/dead-code-gate.sh --update"
        echo "# Generated $(date -u +%Y-%m-%d) from $(git rev-parse --short HEAD 2>/dev/null || echo 'a dirty tree')"
        echo
        while IFS=$'\t' read -r name file; do
            printf '%-36s # %s\n' "$name" "$file"
        done < "$WORK/candidates.tsv"
    } > "$BASELINE"
    echo "dead-code-gate: baseline rewritten, $(wc -l < "$WORK/candidates.txt" | tr -d ' ') symbols"
    exit 0
fi

touch "$BASELINE"
sed 's/#.*//' "$BASELINE" | tr -d ' \t' | grep -v '^$' | sort -u > "$WORK/known.txt"

NEW=$(comm -23 "$WORK/candidates.txt" "$WORK/known.txt")
GONE=$(comm -13 "$WORK/candidates.txt" "$WORK/known.txt")

if [ -n "$GONE" ]; then
    echo "dead-code-gate: these baseline entries no longer come up (deleted, or now called):"
    echo "$GONE" | sed 's/^/    /'
    echo "    -> scripts/dead-code-gate.sh --update to drop them"
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
