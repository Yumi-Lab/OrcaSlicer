#pragma once

#ifdef SLIC3R_MCP_SERVER

namespace mcp { class server; }

namespace Slic3r { namespace GUI { namespace MCP {

// Phase 1 MVP tool for model information:
//   - get_model_info: Get info about loaded 3D models (count, names, bounding boxes, triangles)
struct ModelTools {
    static void register_tools(mcp::server& srv);
};

}}} // namespace Slic3r::GUI::MCP

#endif // SLIC3R_MCP_SERVER
