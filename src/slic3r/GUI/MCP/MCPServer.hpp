#pragma once

#ifdef SLIC3R_MCP_SERVER

#include <memory>
#include <string>
#include <atomic>

// Forward declare cpp-mcp server
namespace mcp { class server; }

namespace Slic3r { namespace GUI { namespace MCP {

// Default MCP server port (next to HTTP OAuth server on 13618)
constexpr int MCP_DEFAULT_PORT = 13619;

// OrcaSlicer MCP Server lifecycle wrapper.
// Owns the mcp::server instance and manages start/stop.
class MCPServer {
public:
    MCPServer();
    ~MCPServer();

    // Start the MCP server on the given port (non-blocking).
    // Registers all tools and resources, then starts listening.
    bool start(int port = MCP_DEFAULT_PORT);

    // Stop the MCP server and clean up.
    void stop();

    // Check if the server is running.
    bool is_running() const;

    // Get the port the server is listening on.
    int get_port() const { return m_port; }

private:
    void register_tools();
    void register_resources();
    static std::string get_agent_instructions();

    std::unique_ptr<mcp::server> m_server;
    int                          m_port{MCP_DEFAULT_PORT};
    std::atomic<bool>            m_running{false};
};

}}} // namespace Slic3r::GUI::MCP

#endif // SLIC3R_MCP_SERVER
