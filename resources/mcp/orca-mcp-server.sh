#!/bin/bash
# OrcaSlicer MCP Server Launcher
# Used by Claude Desktop / AI agents to spawn a dedicated OrcaSlicer instance
#
# Usage:
#   ./orca-mcp-server.sh                  # Uses default OrcaSlicer path
#   ORCASLICER_PATH=/custom/path ./orca-mcp-server.sh  # Custom path
#
# This script launches OrcaSlicer with the --mcp-server flag, which:
# - Starts the MCP server on port 13619 (localhost only)
# - Shows "[AI Controlled]" in the window title
# - Displays a persistent warning notification

ORCA_PATH="/Applications/OrcaSlicer.app/Contents/MacOS/OrcaSlicer"

# Allow override via environment variable
if [ -n "$ORCASLICER_PATH" ]; then
    ORCA_PATH="$ORCASLICER_PATH"
fi

if [ ! -f "$ORCA_PATH" ]; then
    echo "Error: OrcaSlicer not found at $ORCA_PATH" >&2
    echo "Set ORCASLICER_PATH environment variable to the correct path." >&2
    exit 1
fi

# Start OrcaSlicer with MCP server enabled
exec "$ORCA_PATH" --mcp-server
