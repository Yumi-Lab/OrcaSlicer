#include "MCPGUIBridge.hpp"

#ifdef SLIC3R_MCP_SERVER

#include <wx/app.h>
#include "slic3r/GUI/GUI_App.hpp"
#include <boost/log/trivial.hpp>

namespace Slic3r { namespace GUI { namespace MCP {

MCPGUIBridge& MCPGUIBridge::instance()
{
    static MCPGUIBridge s_instance;
    return s_instance;
}

bool MCPGUIBridge::execute_on_gui_void(std::function<void()> fn, int timeout_ms)
{
    if (m_shutdown.load(std::memory_order_acquire))
        return false;

    auto promise = std::make_shared<std::promise<void>>();
    auto future = promise->get_future();

    post_to_gui([fn = std::move(fn), promise]() {
        try {
            fn();
            promise->set_value();
        } catch (...) {
            promise->set_exception(std::current_exception());
        }
    });

    if (future.wait_for(std::chrono::milliseconds(timeout_ms)) == std::future_status::ready) {
        try {
            future.get();
            return true;
        } catch (const std::exception& e) {
            BOOST_LOG_TRIVIAL(error) << "MCPGUIBridge: GUI task threw: " << e.what();
            return false;
        }
    }

    BOOST_LOG_TRIVIAL(warning) << "MCPGUIBridge: GUI task timed out after " << timeout_ms << "ms";
    return false;
}

void MCPGUIBridge::post_to_gui(std::function<void()> fn)
{
    if (m_shutdown.load(std::memory_order_acquire))
        return;

    wxTheApp->CallAfter(std::move(fn));
}

void MCPGUIBridge::shutdown()
{
    m_shutdown.store(true, std::memory_order_release);
    BOOST_LOG_TRIVIAL(info) << "MCPGUIBridge: shutdown";
}

void MCPGUIBridge::reset()
{
    m_shutdown.store(false, std::memory_order_release);
    BOOST_LOG_TRIVIAL(info) << "MCPGUIBridge: reset";
}

}}} // namespace Slic3r::GUI::MCP

#endif // SLIC3R_MCP_SERVER
