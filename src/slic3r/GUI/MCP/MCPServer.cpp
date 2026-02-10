#include "MCPServer.hpp"

#ifdef SLIC3R_MCP_SERVER

#include "MCPGUIBridge.hpp"
#include "Tools/PresetTools.hpp"
#include "Tools/ModelTools.hpp"
#include "Tools/SlicingTools.hpp"
#include "Tools/SceneTools.hpp"
#include "Tools/PresetManagementTools.hpp"
#include "Tools/UITools.hpp"

#include "mcp_server.h"

#include <boost/log/trivial.hpp>

namespace Slic3r { namespace GUI { namespace MCP {

MCPServer::MCPServer() = default;

MCPServer::~MCPServer()
{
    stop();
}

bool MCPServer::start(int port)
{
    if (m_running.load(std::memory_order_acquire)) {
        BOOST_LOG_TRIVIAL(warning) << "MCP server already running on port " << m_port;
        return true;
    }

    m_port = port;

    try {
        mcp::server::configuration conf;
        conf.host = "127.0.0.1"; // localhost only for security
        conf.port = m_port;
        conf.name = "OrcaSlicer MCP Server";
        conf.version = "1.0.0";
        conf.sse_endpoint = "/sse";
        conf.msg_endpoint = "/message";

        m_server = std::make_unique<mcp::server>(conf);

        // Set instructions for AI agents (returned in initialize response)
        m_server->set_instructions(get_agent_instructions());

        // Reset the GUI bridge
        MCPGUIBridge::instance().reset();

        // Register all tools and resources
        register_tools();
        register_resources();

        // Start in non-blocking mode (runs on its own thread)
        bool ok = m_server->start(false);
        if (ok) {
            m_running.store(true, std::memory_order_release);
            BOOST_LOG_TRIVIAL(info) << "MCP server started on port " << m_port;
        } else {
            BOOST_LOG_TRIVIAL(error) << "MCP server failed to start on port " << m_port;
            m_server.reset();
        }
        return ok;
    } catch (const std::exception& e) {
        BOOST_LOG_TRIVIAL(error) << "MCP server start exception: " << e.what();
        m_server.reset();
        return false;
    }
}

void MCPServer::stop()
{
    if (!m_running.load(std::memory_order_acquire))
        return;

    BOOST_LOG_TRIVIAL(info) << "Stopping MCP server...";

    // Shutdown the GUI bridge first
    MCPGUIBridge::instance().shutdown();

    // Stop the mcp server
    if (m_server) {
        m_server->stop();
        m_server.reset();
    }

    m_running.store(false, std::memory_order_release);
    BOOST_LOG_TRIVIAL(info) << "MCP server stopped";
}

bool MCPServer::is_running() const
{
    return m_running.load(std::memory_order_acquire);
}

void MCPServer::register_tools()
{
    if (!m_server) return;

    // Phase 1 MVP tools (7 tools)
    PresetTools::register_tools(*m_server);
    ModelTools::register_tools(*m_server);
    SlicingTools::register_tools(*m_server);

    // Phase 2: 3D Scene Management (11 tools)
    register_scene_tools(*m_server);

    // Phase 2.5: Preset Management (6 tools)
    register_preset_management_tools(*m_server);

    // Phase 3: UI Tools - screenshot, camera, tab navigation, app status (5 tools)
    register_ui_tools(*m_server);

    BOOST_LOG_TRIVIAL(info) << "MCP: All tools registered (31 tools total)";
}

void MCPServer::register_resources()
{
    if (!m_server) return;

    // Resources will be added in a later phase
    BOOST_LOG_TRIVIAL(info) << "MCP resources registered";
}

std::string MCPServer::get_agent_instructions()
{
    return R"(# OrcaSlicer MCP Server - AI Agent Instructions

You are connected to OrcaSlicer, an open-source 3D printing slicer application.
This MCP server lets you control the slicer: load 3D models, configure print settings,
slice models into G-code, manage presets, and control the 3D viewport.

## IMPORTANT: Getting Started

1. **ALWAYS call `get_app_status` first** to understand the current state:
   what tab is active, what objects are loaded, which presets are selected.
2. **Switch to the correct tab** before performing operations:
   - Use `switch_tab` with tab="prepare" before any 3D scene operations
   - Use `switch_tab` with tab="preview" to see sliced results
3. **Use `get_model_info`** to get object IDs before move/rotate/scale/delete operations.

## Tool Categories

### Scene Management (load, manipulate, arrange models)
- `load_model` - Load STL/OBJ/AMF files (AVOID 3MF - may cause blocking dialogs)
- `get_model_info` / `get_plate_info` - Get object IDs, names, positions, volumes
- `move_object` / `rotate_object` / `scale_object` - Transform objects (use object_id from get_model_info)
- `duplicate_object` / `delete_object` / `delete_all_objects` - Manage objects
- `arrange_objects` - Auto-arrange all objects on the build plate
- `auto_orient` - Optimize object orientation for printing

### Multi-Color / Multi-Material
- `set_filament_count` - Set number of filament slots (1-16)
- `set_filament_color` - Set color of a filament slot (hex color e.g. #FF0000, or RGB values)
- `assign_filament` - Assign a filament to a specific object volume (filament_id is 1-based)

### Print Settings & Presets
- `get_print_settings` / `set_print_settings` - Read/modify settings (category: print, filament, or printer)
- `list_presets` / `select_preset` - Browse and activate presets
- `get_all_settings` - Get complete settings dump for a category
- `create_preset` / `duplicate_preset` / `delete_preset` - Manage user presets
- `export_preset` / `import_preset` - Share presets via JSON files

### Slicing
- `slice` - Start slicing (runs asynchronously, may take 10-300 seconds)
- `get_slice_statistics` - Get results after slicing: print time, filament usage, cost

### UI & Camera Control
- `switch_tab` - Navigate tabs: home, prepare, preview, device, project
- `get_current_tab` / `get_app_status` - Check current application state
- `take_screenshot` - Capture the 3D viewport (OpenGL render to PNG file)
- `set_camera_view` - Control camera: preset views (iso/top/front/left/right/rear), zoom, rotate, pan

## Common Workflows

### Load and slice a model:
1. switch_tab(tab="prepare")
2. load_model(filepath="/path/to/model.stl")
3. arrange_objects()
4. slice()
5. Wait ~30 seconds, then get_slice_statistics()

### Multi-color setup:
1. set_filament_count(count=3)
2. set_filament_color(filament_id=1, color="#FF0000")
3. set_filament_color(filament_id=2, color="#00FF00")
4. set_filament_color(filament_id=3, color="#0000FF")
5. assign_filament(object_name="Part1", filament_id=1)

### Take a screenshot:
1. switch_tab(tab="prepare")
2. set_camera_view(view="iso", zoom_to="volumes")
3. take_screenshot(filepath="/tmp/screenshot.png", width=1920, height=1080)

### Modify printer settings:
1. get_print_settings(category="printer", keys=["print_host", "machine_start_gcode"])
2. set_print_settings(category="printer", settings={"print_host": "http://192.168.1.106"})

## Key Notes
- **scale_object**: scale parameter is a FACTOR (0.5 = half size, 2.0 = double), NOT a percentage
- **filament_id**: 1-based indexing (first filament = 1, not 0)
- **object_id**: Get from get_model_info before using move/rotate/scale/delete
- **slicing is async**: After calling slice(), wait before calling get_slice_statistics()
- **take_screenshot**: Only captures the 3D viewport, not the full application window
- **3MF files**: Avoid loading 3MF files, use STL instead (3MF may trigger blocking UI dialogs)
)";
}

}}} // namespace Slic3r::GUI::MCP

#endif // SLIC3R_MCP_SERVER
