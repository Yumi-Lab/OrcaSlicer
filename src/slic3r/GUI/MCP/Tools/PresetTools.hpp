#pragma once

#ifdef SLIC3R_MCP_SERVER

namespace mcp { class server; }

namespace Slic3r { namespace GUI { namespace MCP {

// Phase 1 MVP tools for preset/settings management:
//   - get_print_settings: Read current print settings (or a subset by keys)
//   - set_print_settings: Modify print settings
//   - list_presets: List available presets (print, filament, printer)
//   - select_preset: Activate a preset by name
struct PresetTools {
    static void register_tools(mcp::server& srv);
};

}}} // namespace Slic3r::GUI::MCP

#endif // SLIC3R_MCP_SERVER
