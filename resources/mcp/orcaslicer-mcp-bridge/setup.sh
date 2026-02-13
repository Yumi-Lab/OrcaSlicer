#!/bin/bash
# OrcaSlicer MCP Bridge — Setup script
#
# Registers the MCP bridge with Claude Code so that Claude
# can control OrcaSlicer natively via tool calls.
#
# Usage:
#   ./setup.sh              # Auto-detect OrcaSlicer location
#   ./setup.sh /path/to    # Specify OrcaSlicer source root

set -e

# Resolve the bridge script location
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
BRIDGE="$SCRIPT_DIR/dist/bridge.mjs"

if [ ! -f "$BRIDGE" ]; then
  echo "Error: Bundled bridge not found at $BRIDGE"
  echo "Run 'npm run build' in $SCRIPT_DIR first (requires: npm install)"
  exit 1
fi

# Check Node.js is available
if ! command -v node &>/dev/null; then
  echo "Error: Node.js is required. Install it from https://nodejs.org/"
  exit 1
fi

NODE_VERSION=$(node -v | sed 's/v//' | cut -d. -f1)
if [ "$NODE_VERSION" -lt 18 ]; then
  echo "Error: Node.js 18+ required (found v$NODE_VERSION)"
  exit 1
fi

# Check claude CLI is available
if ! command -v claude &>/dev/null; then
  echo "Error: Claude Code CLI not found."
  echo "Install it with: npm install -g @anthropic-ai/claude-code"
  exit 1
fi

echo "=== OrcaSlicer MCP Bridge Setup ==="
echo ""
echo "Bridge:  $BRIDGE"
echo "Node:    $(node -v)"
echo ""

# Register with Claude Code
claude mcp add orcaslicer -- node "$BRIDGE"

echo ""
echo "Done! The 'orcaslicer' MCP server is now registered with Claude Code."
echo ""
echo "To use it:"
echo "  1. Launch OrcaSlicer with:  OrcaSlicer --mcp-server"
echo "  2. Start Claude Code:       claude"
echo "  3. Claude can now control OrcaSlicer via native tool calls"
echo ""
echo "To remove:  claude mcp remove orcaslicer"
