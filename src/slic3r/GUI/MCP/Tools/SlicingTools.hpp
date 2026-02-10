#pragma once

#ifdef SLIC3R_MCP_SERVER

namespace mcp { class server; }

namespace Slic3r { namespace GUI { namespace MCP {

// Phase 1 MVP tools for slicing:
//   - slice: Trigger slicing of the current plate
//   - get_slice_statistics: Get statistics from the last completed slice
struct SlicingTools {
    static void register_tools(mcp::server& srv);
};

}}} // namespace Slic3r::GUI::MCP

#endif // SLIC3R_MCP_SERVER
