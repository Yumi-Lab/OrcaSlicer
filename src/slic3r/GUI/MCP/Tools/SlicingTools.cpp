#include "SlicingTools.hpp"

#ifdef SLIC3R_MCP_SERVER

#include "../MCPGUIBridge.hpp"

#include "mcp_server.h"
#include "mcp_tool.h"

#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/Plater.hpp"
#include "libslic3r/Print.hpp"

#include <boost/log/trivial.hpp>

namespace Slic3r { namespace GUI { namespace MCP {

void SlicingTools::register_tools(mcp::server& srv)
{
    // ---- slice ----
    {
        auto tool = mcp::tool_builder("slice")
            .with_description("Trigger slicing of the current build plate. "
                              "The slicing runs asynchronously in the background. "
                              "Use get_slice_statistics to check results after slicing completes.")
            .build();

        srv.register_tool(tool, [](const mcp::json& /*params*/, const std::string& /*session_id*/) -> mcp::json {
            bool ok = MCPGUIBridge::instance().execute_on_gui_void([]() {
                wxGetApp().plater()->reslice();
            }, 10000);

            if (!ok)
                throw mcp::mcp_exception(mcp::error_code::internal_error, "Failed to trigger slicing");

            return mcp::json::array({{
                {"type", "text"},
                {"text", "Slicing triggered. Use get_slice_statistics to check results."}
            }});
        });
    }

    // ---- get_slice_statistics ----
    {
        auto tool = mcp::tool_builder("get_slice_statistics")
            .with_description("Get statistics from the last completed slice: "
                              "estimated print time, filament usage, weight, cost, and more.")
            .build();

        srv.register_tool(tool, [](const mcp::json& /*params*/, const std::string& /*session_id*/) -> mcp::json {
            auto result = MCPGUIBridge::instance().execute_on_gui<mcp::json>([]() -> mcp::json {
                const auto& print = wxGetApp().plater()->fff_print();
                const auto& stats = print.print_statistics();

                mcp::json info = {
                    {"estimated_print_time_normal", stats.estimated_normal_print_time},
                    {"estimated_print_time_silent", stats.estimated_silent_print_time},
                    {"total_filament_used_g", stats.total_used_filament},
                    {"total_extruded_volume_mm3", stats.total_extruded_volume},
                    {"total_weight_g", stats.total_weight},
                    {"total_cost", stats.total_cost},
                    {"total_toolchanges", stats.total_toolchanges},
                    {"total_wipe_tower_filament", stats.total_wipe_tower_filament},
                    {"total_wipe_tower_cost", stats.total_wipe_tower_cost},
                    {"initial_tool", stats.initial_tool}
                };

                // Per-extruder filament stats
                mcp::json filament_per_extruder = mcp::json::object();
                for (const auto& [extruder_id, usage] : stats.filament_stats)
                    filament_per_extruder[std::to_string(extruder_id)] = usage;
                info["filament_per_extruder"] = filament_per_extruder;

                return mcp::json::array({{
                    {"type", "text"},
                    {"text", info.dump(2)}
                }});
            });

            if (!result.has_value())
                throw mcp::mcp_exception(mcp::error_code::internal_error, "GUI bridge timeout");
            return result.value();
        });
    }

    BOOST_LOG_TRIVIAL(debug) << "MCP SlicingTools registered (2 tools)";
}

}}} // namespace Slic3r::GUI::MCP

#endif // SLIC3R_MCP_SERVER
