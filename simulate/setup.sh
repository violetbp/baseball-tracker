#!/usr/bin/env bash
# Setup script: fetches Dear ImGui dependency
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
DEPS_DIR="$SCRIPT_DIR/../deps"
IMGUI_DIR="$DEPS_DIR/imgui"

mkdir -p "$DEPS_DIR"

if [ -d "$IMGUI_DIR/.git" ]; then
  echo "ImGui already cloned, updating..."
  (cd "$IMGUI_DIR" && git pull)
else
  echo "Cloning Dear ImGui..."
  git clone --depth 1 https://github.com/ocornut/imgui.git "$IMGUI_DIR"
fi

echo ""
echo "Dependencies ready."
echo ""
echo "To build:"
echo "  nix develop"
echo "  cd simulate && make"
echo ""
echo "To run:"
echo "  nix develop -c 'cd simulate && make && ./bin/baseball-tracker-sim'"
