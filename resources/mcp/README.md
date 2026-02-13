# OrcaSlicer MCP Server — AI Agent Control

OrcaSlicer includes a built-in [MCP (Model Context Protocol)](https://modelcontextprotocol.io/) server that allows AI agents like Claude to control the slicer programmatically — loading models, adjusting settings, slicing, taking screenshots, and more.

## Prerequisites

- **OrcaSlicer** (built with MCP support — enabled by default)
- **Node.js 18+** ([download](https://nodejs.org/))
- **Claude Code** or **Claude Desktop** ([download](https://claude.ai/code))

## Quick Setup

### Step 1: Launch OrcaSlicer with MCP enabled

```bash
# macOS (from source build)
./build/arm64/src/RelWithDebInfo/OrcaSlicer --mcp-server

# macOS (from .app bundle)
/Applications/OrcaSlicer.app/Contents/MacOS/OrcaSlicer --mcp-server

# Windows
OrcaSlicer.exe --mcp-server

# Linux
./OrcaSlicer --mcp-server
```

When MCP is active, a discreet "AI Controlled" indicator appears in the 3D viewport.

### Step 2: Register the bridge with Claude

**Option A: One-liner command (recommended)**
```bash
claude mcp add orcaslicer -- node /path/to/OrcaSlicer/resources/mcp/orcaslicer-mcp-bridge/dist/bridge.mjs
```

**Option B: Setup script**
```bash
cd resources/mcp/orcaslicer-mcp-bridge
./setup.sh
```

**Option C: Claude Desktop (JSON config)**

Add to your Claude Desktop config (`~/Library/Application Support/Claude/claude_desktop_config.json` on macOS):
```json
{
  "mcpServers": {
    "orcaslicer": {
      "command": "node",
      "args": ["/path/to/OrcaSlicer/resources/mcp/orcaslicer-mcp-bridge/dist/bridge.mjs"]
    }
  }
}
```

### Step 3: Start using Claude

Launch Claude Code or Claude Desktop. Claude can now control OrcaSlicer through native tool calls.

## Available Tools (31)

### Scene Management (13 tools)
| Tool | Description |
|------|-------------|
| `load_model` | Load a 3D model file (STL, OBJ, STEP, 3MF) |
| `move_object` | Move an object to a specific position |
| `rotate_object` | Rotate an object around X/Y/Z axes |
| `scale_object` | Scale an object (uniform or per-axis) |
| `duplicate_object` | Duplicate an object on the build plate |
| `delete_object` | Remove an object from the build plate |
| `auto_orient` | Optimize orientation for better printing |
| `arrange_objects` | Auto-arrange objects on the build plate |
| `get_plate_info` | Get detailed info about all objects |
| `set_filament_color` | Set filament color for preview |
| `set_filament_count` | Set number of filament slots |
| `assign_filament` | Assign filament to a specific volume |
| `get_model_info` | Get model information |

### Presets & Settings (10 tools)
| Tool | Description |
|------|-------------|
| `get_print_settings` | Get current print settings |
| `set_print_settings` | Modify print settings |
| `list_presets` | List available presets (printer/filament/process) |
| `select_preset` | Select a preset by name |
| `get_all_settings` | Get all settings with descriptions |
| `create_preset` | Create a new preset |
| `duplicate_preset` | Duplicate an existing preset |
| `delete_preset` | Delete a user preset |
| `export_preset` | Export a preset to file |
| `import_preset` | Import a preset from file |

### Slicing (2 tools)
| Tool | Description |
|------|-------------|
| `slice` | Slice the current plate |
| `get_slice_stats` | Get statistics from last slice |

### UI & Camera (5 tools)
| Tool | Description |
|------|-------------|
| `take_screenshot` | Take a screenshot of the 3D viewport |
| `set_camera_view` | Control camera (preset views, zoom, rotate) |
| `switch_tab` | Switch between tabs (prepare/preview/monitor) |
| `get_current_tab` | Get the currently active tab |
| `get_app_status` | Get application status info |

### Connection (1 tool)
| Tool | Description |
|------|-------------|
| `connection_status` | Check bridge connection (shown when OrcaSlicer is offline) |

## Architecture

```
Claude Code/Desktop  <--stdio MCP-->  Bridge (Node.js)  <--HTTP/SSE MCP-->  OrcaSlicer
```

- **Bridge**: Translates between stdio MCP (used by Claude) and HTTP/SSE MCP (used by OrcaSlicer's built-in server)
- **Lazy connection**: Bridge starts even if OrcaSlicer isn't running yet, retries automatically
- **Auto-reconnection**: Detects lost connections and reconnects on next tool call
- **Port**: 13619 (localhost only)

## Troubleshooting

| Problem | Solution |
|---------|----------|
| "Cannot connect to OrcaSlicer" | Make sure OrcaSlicer is running with `--mcp-server` |
| "Node.js not found" | Install Node.js 18+ from [nodejs.org](https://nodejs.org/) |
| Timeout on tool calls | OrcaSlicer might be busy (slicing, loading). Wait and retry. |
| Bridge starts but no tools | Check that port 13619 is not blocked by firewall |

## Development

To modify the bridge source and rebuild:

```bash
cd resources/mcp/orcaslicer-mcp-bridge
npm install          # Install dependencies (first time only)
# Edit bridge.mjs
npm run build        # Rebuild bundled dist/bridge.mjs
```

To remove the bridge from Claude:
```bash
claude mcp remove orcaslicer
```
