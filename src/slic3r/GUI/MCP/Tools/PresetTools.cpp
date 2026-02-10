#include "PresetTools.hpp"

#ifdef SLIC3R_MCP_SERVER

#include "../MCPGUIBridge.hpp"

#include "mcp_server.h"
#include "mcp_tool.h"

#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/Plater.hpp"
#include "slic3r/GUI/Tab.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/AppConfig.hpp"

#include <boost/log/trivial.hpp>

namespace Slic3r { namespace GUI { namespace MCP {

namespace {

// Helper: get the PresetCollection for a given category name
PresetCollection* preset_collection_by_category(const std::string& category)
{
    auto* bundle = wxGetApp().preset_bundle;
    if (!bundle) return nullptr;

    if (category == "print")     return &bundle->prints;
    if (category == "filament")  return &bundle->filaments;
    if (category == "printer")   return &bundle->printers;
    if (category == "sla_print") return &bundle->sla_prints;
    if (category == "sla_material") return &bundle->sla_materials;
    return nullptr;
}

// Helper: convert category string to Preset::Type for Tab lookup
Preset::Type category_to_preset_type(const std::string& category)
{
    if (category == "print")     return Preset::TYPE_PRINT;
    if (category == "filament")  return Preset::TYPE_FILAMENT;
    if (category == "printer")   return Preset::TYPE_PRINTER;
    return Preset::TYPE_INVALID;
}

} // anonymous namespace

void PresetTools::register_tools(mcp::server& srv)
{
    // ---- get_print_settings ----
    {
        auto tool = mcp::tool_builder("get_print_settings")
            .with_description("Get current print settings. Optionally filter by specific keys. "
                              "Category can be: print, filament, printer.")
            .with_string_param("category", "Settings category: print, filament, or printer", true)
            .with_array_param("keys", "Optional list of setting keys to retrieve. If empty, returns all settings.", "string", false)
            .build();

        srv.register_tool(tool, [](const mcp::json& params, const std::string& /*session_id*/) -> mcp::json {
            // Capture by value to avoid dangling references on timeout
            std::string category = params.value("category", "print");
            mcp::json keys_json = params.contains("keys") ? params["keys"] : mcp::json::array();

            auto result = MCPGUIBridge::instance().execute_on_gui<mcp::json>(
                [category, keys_json]() -> mcp::json {
                auto* collection = preset_collection_by_category(category);
                if (!collection)
                    throw mcp::mcp_exception(mcp::error_code::invalid_params,
                        "Unknown category: " + category);

                const auto& config = collection->get_edited_preset().config;
                mcp::json settings = mcp::json::object();

                if (keys_json.empty() || !keys_json.is_array()) {
                    for (const auto& key : config.keys())
                        settings[key] = config.opt_serialize(key);
                } else {
                    for (const auto& k : keys_json) {
                        std::string key = k.get<std::string>();
                        if (config.has(key))
                            settings[key] = config.opt_serialize(key);
                        else
                            settings[key] = nullptr;
                    }
                }

                return mcp::json::array({{
                    {"type", "text"},
                    {"text", settings.dump(2)}
                }});
            });

            if (!result.has_value())
                throw mcp::mcp_exception(mcp::error_code::internal_error, "GUI bridge timeout");
            return result.value();
        });
    }

    // ---- set_print_settings ----
    {
        auto tool = mcp::tool_builder("set_print_settings")
            .with_description("Modify print settings. Provide key-value pairs. "
                              "Category can be: print, filament, printer.")
            .with_string_param("category", "Settings category: print, filament, or printer", true)
            .with_object_param("settings", "Key-value pairs of settings to modify", mcp::json::object(), true)
            .build();

        srv.register_tool(tool, [](const mcp::json& params, const std::string& /*session_id*/) -> mcp::json {
            std::string category = params.value("category", "print");
            mcp::json settings = params.value("settings", mcp::json::object());

            if (settings.empty())
                throw mcp::mcp_exception(mcp::error_code::invalid_params, "No settings provided");

            auto result = MCPGUIBridge::instance().execute_on_gui<mcp::json>(
                [category, settings]() -> mcp::json {
                auto* collection = preset_collection_by_category(category);
                if (!collection)
                    throw mcp::mcp_exception(mcp::error_code::invalid_params,
                        "Unknown category: " + category);

                auto& config = collection->get_edited_preset().config;
                ConfigSubstitutionContext ctx(ForwardCompatibilitySubstitutionRule::Disable);

                mcp::json applied = mcp::json::object();
                mcp::json errors = mcp::json::object();

                for (auto it = settings.begin(); it != settings.end(); ++it) {
                    const std::string& key = it.key();
                    const std::string  val = it.value().is_string() ? it.value().get<std::string>()
                                                                    : it.value().dump();
                    try {
                        config.set_deserialize(key, val, ctx);
                        applied[key] = val;
                    } catch (const std::exception& e) {
                        errors[key] = e.what();
                    }
                }

                // Notify the UI about config changes
                if (!applied.empty()) {
                    DynamicPrintConfig changed_config;
                    for (auto it = applied.begin(); it != applied.end(); ++it) {
                        const auto* opt = config.option(it.key());
                        if (opt)
                            changed_config.set_key_value(it.key(), opt->clone());
                    }
                    wxGetApp().plater()->on_config_change(changed_config);

                    // Also refresh the Tab sidebar UI so fields update visually
                    Tab* tab = wxGetApp().get_tab(category_to_preset_type(category));
                    if (tab) {
                        tab->update_dirty();
                        tab->reload_config();
                        tab->update();
                    }
                }

                mcp::json result_obj = {
                    {"applied", applied},
                    {"errors", errors}
                };
                return mcp::json::array({{
                    {"type", "text"},
                    {"text", result_obj.dump(2)}
                }});
            }, 10000);

            if (!result.has_value())
                throw mcp::mcp_exception(mcp::error_code::internal_error, "GUI bridge timeout");
            return result.value();
        });
    }

    // ---- list_presets ----
    {
        auto tool = mcp::tool_builder("list_presets")
            .with_description("List available presets for a category (print, filament, printer).")
            .with_string_param("category", "Preset category: print, filament, or printer", true)
            .build();

        srv.register_tool(tool, [](const mcp::json& params, const std::string& /*session_id*/) -> mcp::json {
            std::string category = params.value("category", "print");

            auto result = MCPGUIBridge::instance().execute_on_gui<mcp::json>(
                [category]() -> mcp::json {
                auto* collection = preset_collection_by_category(category);
                if (!collection)
                    throw mcp::mcp_exception(mcp::error_code::invalid_params,
                        "Unknown category: " + category);

                mcp::json presets_list = mcp::json::array();
                for (const auto& preset : collection->get_presets()) {
                    if (preset.is_visible) {
                        presets_list.push_back({
                            {"name", preset.name},
                            {"is_default", preset.is_default},
                            {"is_system", preset.is_system},
                            {"is_selected", (&preset == &collection->get_selected_preset())}
                        });
                    }
                }

                return mcp::json::array({{
                    {"type", "text"},
                    {"text", presets_list.dump(2)}
                }});
            });

            if (!result.has_value())
                throw mcp::mcp_exception(mcp::error_code::internal_error, "GUI bridge timeout");
            return result.value();
        });
    }

    // ---- select_preset ----
    {
        auto tool = mcp::tool_builder("select_preset")
            .with_description("Select/activate a preset by name for a given category. "
                              "For filament on multi-extruder printers, use the 'extruder' "
                              "parameter to specify which extruder slot (1-based). Default is 1.")
            .with_string_param("category", "Preset category: print, filament, or printer", true)
            .with_string_param("name", "Name of the preset to select", true)
            .with_number_param("extruder", "Extruder slot (1-based) for filament presets on multi-extruder printers. Default: 1", false)
            .build();

        srv.register_tool(tool, [](const mcp::json& params, const std::string& /*session_id*/) -> mcp::json {
            std::string category = params.value("category", "print");
            std::string name = params.value("name", "");
            int extruder = params.value("extruder", 1);

            if (name.empty())
                throw mcp::mcp_exception(mcp::error_code::invalid_params, "Preset name is required");

            auto result = MCPGUIBridge::instance().execute_on_gui<mcp::json>(
                [category, name, extruder]() -> mcp::json {
                auto* collection = preset_collection_by_category(category);
                if (!collection)
                    throw mcp::mcp_exception(mcp::error_code::invalid_params,
                        "Unknown category: " + category);

                // Verify preset exists
                const Preset* preset = collection->find_preset(name, false);
                if (!preset)
                    throw mcp::mcp_exception(mcp::error_code::invalid_params,
                        "Preset not found: " + name);

                // For filament on multi-extruder, use set_filament_preset
                if (category == "filament") {
                    auto* bundle = wxGetApp().preset_bundle;
                    size_t idx = static_cast<size_t>(extruder - 1); // Convert 1-based to 0-based
                    if (idx >= bundle->filament_presets.size())
                        throw mcp::mcp_exception(mcp::error_code::invalid_params,
                            "Extruder " + std::to_string(extruder) + " out of range (max: "
                            + std::to_string(bundle->filament_presets.size()) + ")");

                    bundle->set_filament_preset(idx, name);
                    // Also select it as active editing preset
                    collection->select_preset_by_name(name, false);
                    bundle->export_selections(*wxGetApp().app_config);

                    // Update sidebar filament combo boxes
                    wxGetApp().plater()->sidebar().update_presets(Preset::TYPE_FILAMENT);
                } else {
                    bool ok = collection->select_preset_by_name(name, false);
                    if (!ok)
                        throw mcp::mcp_exception(mcp::error_code::invalid_params,
                            "Failed to select preset: " + name);
                }

                // Refresh the Tab UI to reflect the newly selected preset
                Tab* tab = wxGetApp().get_tab(category_to_preset_type(category));
                if (tab) {
                    tab->load_current_preset();
                }

                return mcp::json::array({{
                    {"type", "text"},
                    {"text", "Preset '" + name + "' selected for category '" + category
                             + "'" + (category == "filament" ? " (extruder " + std::to_string(extruder) + ")" : "")}
                }});
            }, 10000);

            if (!result.has_value())
                throw mcp::mcp_exception(mcp::error_code::internal_error, "GUI bridge timeout");
            return result.value();
        });
    }

    BOOST_LOG_TRIVIAL(debug) << "MCP PresetTools registered (4 tools)";
}

}}} // namespace Slic3r::GUI::MCP

#endif // SLIC3R_MCP_SERVER
