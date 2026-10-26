#!/bin/bash
# SessionStart: the graphify CLI and a current code graph (graphify-out/,
# git-ignored) for this checkout, so every agent can query the graph
# instead of reading files (CLAUDE.md "graphify"). Never fails the session.
set -u
cd "${CLAUDE_PROJECT_DIR:-$(dirname "$0")/../..}" 2>/dev/null || exit 0
PATH="$HOME/.local/bin:$PATH"
if ! command -v graphify >/dev/null 2>&1; then
    # a venv of its own: distributions with an externally managed Python (PEP 668, Arch, Debian 12+)
    # refuse a plain pip install, which left every session without the graph
    V="$HOME/.local/share/rdna3-graphify-venv"
    { [ -x "$V/bin/graphify" ] || { python3 -m venv "$V" && "$V/bin/pip" install --quiet graphifyy==0.9.92; } >/dev/null 2>&1; } &&
        mkdir -p "$HOME/.local/bin" && ln -sf "$V/bin/graphify" "$HOME/.local/bin/graphify" ||
        pip install --quiet graphifyy==0.9.92 >/dev/null 2>&1 ||
        { echo "graphify: install failed, agents read files directly this session" >&2; exit 0; }
fi
if [ -f graphify-out/graph.json ]; then
    graphify update . >/dev/null 2>&1 || echo "graphify: update failed" >&2
else
    { graphify extract . --code-only && graphify cluster-only . --no-viz --no-label; } >/dev/null 2>&1 ||
        echo "graphify: graph build failed" >&2
fi
exit 0
