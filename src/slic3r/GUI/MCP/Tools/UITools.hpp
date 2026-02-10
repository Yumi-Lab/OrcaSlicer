#pragma once

#ifdef SLIC3R_MCP_SERVER

#include "../vendor/cpp-mcp/include/mcp_server.h"

namespace Slic3r { namespace GUI { namespace MCP {

void register_ui_tools(mcp::server& srv);

}}} // namespace Slic3r::GUI::MCP

#endif // SLIC3R_MCP_SERVER
