#pragma once

#ifdef SLIC3R_MCP_SERVER

#include <atomic>
#include <chrono>
#include <functional>
#include <future>
#include <mutex>
#include <optional>
#include <string>

namespace Slic3r { namespace GUI { namespace MCP {

// Thread-safe bridge between MCP server (httplib thread) and wxWidgets GUI thread.
// Mirrors the pattern from BackgroundSlicingProcess::execute_ui_task().
//
// Usage from MCP tool handlers (running on httplib thread):
//   auto result = MCPGUIBridge::instance().execute_on_gui<json>([](){ return ...; }, timeout_ms);
//
class MCPGUIBridge {
public:
    static MCPGUIBridge& instance();

    // Execute a function on the GUI thread and wait for the result.
    // Returns std::nullopt on timeout or if the bridge is shut down.
    template<typename R>
    std::optional<R> execute_on_gui(std::function<R()> fn, int timeout_ms = 5000);

    // Execute a void function on the GUI thread and wait for completion.
    bool execute_on_gui_void(std::function<void()> fn, int timeout_ms = 5000);

    // Non-blocking fire-and-forget on the GUI thread.
    void post_to_gui(std::function<void()> fn);

    // Shutdown the bridge. After this, all execute calls return immediately.
    void shutdown();

    // Reset after shutdown (for restart).
    void reset();

    bool is_shutdown() const { return m_shutdown.load(std::memory_order_acquire); }

private:
    MCPGUIBridge() = default;
    ~MCPGUIBridge() = default;
    MCPGUIBridge(const MCPGUIBridge&) = delete;
    MCPGUIBridge& operator=(const MCPGUIBridge&) = delete;

    std::atomic<bool> m_shutdown{false};
};

// Template implementation
template<typename R>
std::optional<R> MCPGUIBridge::execute_on_gui(std::function<R()> fn, int timeout_ms)
{
    if (m_shutdown.load(std::memory_order_acquire))
        return std::nullopt;

    auto promise = std::make_shared<std::promise<R>>();
    auto future = promise->get_future();

    // Post to GUI thread via wxWidgets CallAfter
    post_to_gui([fn = std::move(fn), promise]() {
        try {
            promise->set_value(fn());
        } catch (...) {
            promise->set_exception(std::current_exception());
        }
    });

    // Wait with timeout
    if (future.wait_for(std::chrono::milliseconds(timeout_ms)) == std::future_status::ready) {
        try {
            return future.get();
        } catch (...) {
            return std::nullopt;
        }
    }
    return std::nullopt;
}

}}} // namespace Slic3r::GUI::MCP

#endif // SLIC3R_MCP_SERVER
