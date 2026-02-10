#include "ModelTools.hpp"

#ifdef SLIC3R_MCP_SERVER

#include "../MCPGUIBridge.hpp"

#include "mcp_server.h"
#include "mcp_tool.h"

#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/Plater.hpp"
#include "libslic3r/Model.hpp"

#include <boost/log/trivial.hpp>

namespace Slic3r { namespace GUI { namespace MCP {

void ModelTools::register_tools(mcp::server& srv)
{
    // ---- get_model_info ----
    {
        auto tool = mcp::tool_builder("get_model_info")
            .with_description("Get information about 3D models currently loaded on the build plate. "
                              "Returns object_id, name, position, bounding box, and volume details for each object. "
                              "Use object_id to target specific objects in move/rotate/scale/delete tools.")
            .build();

        srv.register_tool(tool, [](const mcp::json& /*params*/, const std::string& /*session_id*/) -> mcp::json {
            auto result = MCPGUIBridge::instance().execute_on_gui<mcp::json>([]() -> mcp::json {
                const auto& model = wxGetApp().plater()->model();

                mcp::json objects = mcp::json::array();
                for (size_t i = 0; i < model.objects.size(); ++i) {
                    const auto* obj = model.objects[i];
                    if (!obj) continue;

                    // Compute bounding box
                    auto bb = obj->bounding_box_exact();
                    auto size = bb.size();

                    // Count total triangles across all volumes
                    size_t total_triangles = 0;
                    mcp::json volumes = mcp::json::array();
                    for (size_t vi = 0; vi < obj->volumes.size(); ++vi) {
                        const auto* vol = obj->volumes[vi];
                        if (!vol) continue;
                        size_t tri_count = vol->mesh().its.indices.size();
                        total_triangles += tri_count;
                        volumes.push_back({
                            {"index", vi},
                            {"name", vol->name},
                            {"triangles", tri_count},
                            {"type", vol->is_model_part() ? "model_part" :
                                     vol->is_modifier() ? "modifier" :
                                     vol->is_support_enforcer() ? "support_enforcer" :
                                     vol->is_support_blocker() ? "support_blocker" : "unknown"}
                        });
                    }

                    objects.push_back({
                        {"index", i},
                        {"object_id", (int)obj->id().id},
                        {"name", obj->name},
                        {"instances", obj->instances.size()},
                        {"position", obj->instances.empty() ? mcp::json(nullptr) : mcp::json{
                            {"x", obj->instances[0]->get_offset().x()},
                            {"y", obj->instances[0]->get_offset().y()},
                            {"z", obj->instances[0]->get_offset().z()}
                        }},
                        {"volumes", volumes},
                        {"total_triangles", total_triangles},
                        {"bounding_box", {
                            {"min", {{"x", bb.min.x()}, {"y", bb.min.y()}, {"z", bb.min.z()}}},
                            {"max", {{"x", bb.max.x()}, {"y", bb.max.y()}, {"z", bb.max.z()}}},
                            {"size", {{"x", size.x()}, {"y", size.y()}, {"z", size.z()}}}
                        }}
                    });
                }

                mcp::json info = {
                    {"object_count", model.objects.size()},
                    {"objects", objects}
                };

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

    BOOST_LOG_TRIVIAL(debug) << "MCP ModelTools registered (1 tool)";
}

}}} // namespace Slic3r::GUI::MCP

#endif // SLIC3R_MCP_SERVER
