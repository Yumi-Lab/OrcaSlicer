#include "PresetManagementTools.hpp"

#ifdef SLIC3R_MCP_SERVER

#include "../MCPGUIBridge.hpp"

#include "mcp_server.h"
#include "mcp_tool.h"

#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/Plater.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/Config.hpp"

#include <boost/filesystem.hpp>
#include <boost/log/trivial.hpp>
#include <fstream>

namespace Slic3r { namespace GUI { namespace MCP {

// Helper to get preset collection by category
static PresetCollection* get_collection(const std::string& category) {
    auto* preset_bundle = GUI::wxGetApp().preset_bundle;
    if (!preset_bundle) return nullptr;

    if (category == "print")    return &preset_bundle->prints;
    if (category == "filament") return &preset_bundle->filaments;
    if (category == "printer")  return &preset_bundle->printers;

    return nullptr;
}

void register_preset_management_tools(mcp::server& srv)
{
    // Tool 1: create_preset
    {
        auto tool = mcp::tool_builder("create_preset")
            .with_description("Create a new preset (print/filament/printer) from scratch or based on existing preset")
            .with_string_param("category", "Category: print, filament, or printer", true)
            .with_string_param("name", "Name for the new preset", true)
            .with_string_param("base_preset", "Optional: base preset name to clone from", false)
            .with_object_param("settings", "Optional: settings to apply (key-value pairs)", mcp::json::object(), false)
            .build();

        srv.register_tool(tool, [](const mcp::json& params, const std::string& /*session_id*/) -> mcp::json {
            std::string category = params["category"];
            std::string name = params["name"];

            auto result = MCPGUIBridge::instance().execute_on_gui<mcp::json>(
                [category, name, params]() -> mcp::json {
                    PresetCollection* collection = get_collection(category);
                    if (!collection) {
                        return {{"error", "Invalid category: " + category}};
                    }

                    if (collection->find_preset(name, false)) {
                        return {{"error", "Preset name already exists: " + name}};
                    }

                    // Get base config
                    DynamicPrintConfig config;
                    if (params.contains("base_preset")) {
                        std::string base_name = params["base_preset"];
                        Preset* base = collection->find_preset(base_name, false);
                        if (!base) {
                            return {{"error", "Base preset not found: " + base_name}};
                        }
                        config = base->config;
                    } else {
                        config = collection->default_preset().config;
                    }

                    // Apply settings if provided
                    if (params.contains("settings") && params["settings"].is_object()) {
                        for (auto& [key, value] : params["settings"].items()) {
                            std::string val_str = value.is_string()
                                ? value.get<std::string>()
                                : value.dump();
                            try {
                                config.set_deserialize_strict(key, val_str);
                            } catch (const std::exception& e) {
                                BOOST_LOG_TRIVIAL(warning) << "MCP create_preset: skip invalid setting " << key << ": " << e.what();
                            }
                        }
                    }

                    // Create the preset via save_current_preset approach:
                    // First select the base, then save under new name
                    std::string base_name = params.contains("base_preset")
                        ? params["base_preset"].get<std::string>()
                        : collection->default_preset().name;
                    collection->select_preset_by_name(base_name, true);
                    collection->save_current_preset(name);

                    // Now apply custom settings to the new preset
                    if (params.contains("settings") && params["settings"].is_object()) {
                        Preset* new_preset = collection->find_preset(name, false);
                        if (new_preset) {
                            for (auto& [key, value] : params["settings"].items()) {
                                std::string val_str = value.is_string()
                                    ? value.get<std::string>()
                                    : value.dump();
                                try {
                                    new_preset->config.set_deserialize_strict(key, val_str);
                                } catch (...) {}
                            }
                            new_preset->save(nullptr);
                        }
                    }

                    collection->select_preset_by_name(name, true);

                    return {{"success", true}, {"message", "Preset created: " + name}};
                },
                10000
            );

            return result.value_or(mcp::json{{"error", "Timeout"}});
        });
    }

    // Tool 2: duplicate_preset
    {
        auto tool = mcp::tool_builder("duplicate_preset")
            .with_description("Duplicate an existing preset under a new name")
            .with_string_param("category", "Category: print, filament, or printer", true)
            .with_string_param("source_name", "Name of preset to duplicate", true)
            .with_string_param("new_name", "Name for the duplicated preset", true)
            .build();

        srv.register_tool(tool, [](const mcp::json& params, const std::string& /*session_id*/) -> mcp::json {
            std::string category = params["category"];
            std::string source_name = params["source_name"];
            std::string new_name = params["new_name"];

            auto result = MCPGUIBridge::instance().execute_on_gui<mcp::json>(
                [category, source_name, new_name]() -> mcp::json {
                    PresetCollection* collection = get_collection(category);
                    if (!collection) {
                        return {{"error", "Invalid category"}};
                    }

                    Preset* source = collection->find_preset(source_name, false);
                    if (!source) {
                        return {{"error", "Source preset not found: " + source_name}};
                    }

                    if (collection->find_preset(new_name, false)) {
                        return {{"error", "Preset name already exists: " + new_name}};
                    }

                    // Select source then save as new name
                    collection->select_preset_by_name(source_name, true);
                    collection->save_current_preset(new_name);
                    collection->select_preset_by_name(new_name, true);

                    return {{"success", true}, {"message", "Preset duplicated: " + source_name + " -> " + new_name}};
                },
                10000
            );

            return result.value_or(mcp::json{{"error", "Timeout"}});
        });
    }

    // Tool 3: delete_preset
    {
        auto tool = mcp::tool_builder("delete_preset")
            .with_description("Delete a user preset (cannot delete system presets)")
            .with_string_param("category", "Category: print, filament, or printer", true)
            .with_string_param("name", "Name of preset to delete", true)
            .build();

        srv.register_tool(tool, [](const mcp::json& params, const std::string& /*session_id*/) -> mcp::json {
            std::string category = params["category"];
            std::string name = params["name"];

            auto result = MCPGUIBridge::instance().execute_on_gui<mcp::json>(
                [category, name]() -> mcp::json {
                    PresetCollection* collection = get_collection(category);
                    if (!collection) {
                        return {{"error", "Invalid category"}};
                    }

                    Preset* preset = collection->find_preset(name, false);
                    if (!preset) {
                        return {{"error", "Preset not found: " + name}};
                    }

                    if (preset->is_system) {
                        return {{"error", "Cannot delete system preset: " + name}};
                    }

                    if (preset->is_default) {
                        return {{"error", "Cannot delete default preset: " + name}};
                    }

                    // If deleting the currently selected preset, switch to default first
                    // to avoid dangling m_idx_selected after erase
                    std::string selected_name = collection->get_selected_preset_name();
                    if (selected_name == name) {
                        collection->select_preset_by_name(collection->default_preset().name, true);
                    }

                    bool deleted = collection->delete_preset(name);
                    if (!deleted) {
                        return {{"error", "Failed to delete preset: " + name}};
                    }

                    return {{"success", true}, {"message", "Preset deleted: " + name}};
                },
                10000
            );

            return result.value_or(mcp::json{{"error", "Timeout"}});
        });
    }

    // Tool 4: export_preset
    {
        auto tool = mcp::tool_builder("export_preset")
            .with_description("Export a preset to a JSON file for sharing or backup")
            .with_string_param("category", "Category: print, filament, or printer", true)
            .with_string_param("name", "Preset name to export", true)
            .with_string_param("filepath", "Output file path (.json)", true)
            .build();

        srv.register_tool(tool, [](const mcp::json& params, const std::string& /*session_id*/) -> mcp::json {
            std::string category = params["category"];
            std::string name = params["name"];
            std::string filepath = params["filepath"];

            auto result = MCPGUIBridge::instance().execute_on_gui<mcp::json>(
                [category, name, filepath]() -> mcp::json {
                    PresetCollection* collection = get_collection(category);
                    if (!collection) {
                        return {{"error", "Invalid category"}};
                    }

                    Preset* preset = collection->find_preset(name, false);
                    if (!preset) {
                        return {{"error", "Preset not found: " + name}};
                    }

                    mcp::json export_data;
                    export_data["category"] = category;
                    export_data["name"] = preset->name;
                    export_data["settings"] = mcp::json::object();

                    for (const std::string& key : preset->config.keys()) {
                        export_data["settings"][key] = preset->config.opt_serialize(key);
                    }

                    try {
                        std::ofstream file(filepath);
                        if (!file.is_open()) {
                            return {{"error", "Failed to open file for writing: " + filepath}};
                        }
                        file << export_data.dump(2);
                        file.close();
                    } catch (const std::exception& e) {
                        return {{"error", std::string("File write error: ") + e.what()}};
                    }

                    return {
                        {"success", true},
                        {"message", "Exported to: " + filepath},
                        {"settings_count", (int)preset->config.keys().size()}
                    };
                },
                10000
            );

            return result.value_or(mcp::json{{"error", "Timeout"}});
        });
    }

    // Tool 5: import_preset
    {
        auto tool = mcp::tool_builder("import_preset")
            .with_description("Import a preset from a JSON file (previously exported)")
            .with_string_param("filepath", "Input JSON file path", true)
            .with_string_param("name", "Optional: override the preset name from the file", false)
            .build();

        srv.register_tool(tool, [](const mcp::json& params, const std::string& /*session_id*/) -> mcp::json {
            std::string filepath = params["filepath"];

            auto result = MCPGUIBridge::instance().execute_on_gui<mcp::json>(
                [filepath, params]() -> mcp::json {
                    if (!boost::filesystem::exists(filepath)) {
                        return {{"error", "File not found: " + filepath}};
                    }

                    mcp::json import_data;
                    try {
                        std::ifstream file(filepath);
                        if (!file.is_open()) {
                            return {{"error", "Failed to open file: " + filepath}};
                        }
                        file >> import_data;
                    } catch (const std::exception& e) {
                        return {{"error", std::string("JSON parse error: ") + e.what()}};
                    }

                    if (!import_data.contains("category") || !import_data.contains("settings")) {
                        return {{"error", "Invalid preset file format (missing category or settings)"}};
                    }

                    std::string category = import_data["category"];
                    std::string name = params.contains("name")
                        ? params["name"].get<std::string>()
                        : import_data.value("name", "Imported Preset");

                    PresetCollection* collection = get_collection(category);
                    if (!collection) {
                        return {{"error", "Invalid category in file: " + category}};
                    }

                    if (collection->find_preset(name, false)) {
                        return {{"error", "Preset name already exists: " + name}};
                    }

                    // Create from default, then apply imported settings
                    collection->select_preset_by_name(collection->default_preset().name, true);
                    collection->save_current_preset(name);

                    Preset* preset = collection->find_preset(name, false);
                    if (!preset) {
                        return {{"error", "Failed to create preset"}};
                    }

                    int applied = 0;
                    for (auto& [key, value] : import_data["settings"].items()) {
                        std::string val_str = value.is_string()
                            ? value.get<std::string>()
                            : value.dump();
                        try {
                            preset->config.set_deserialize_strict(key, val_str);
                            applied++;
                        } catch (const std::exception& e) {
                            BOOST_LOG_TRIVIAL(warning) << "MCP import_preset: skip " << key << ": " << e.what();
                        }
                    }

                    preset->save(nullptr);
                    collection->select_preset_by_name(name, true);

                    return {
                        {"success", true},
                        {"message", "Imported: " + name},
                        {"settings_applied", applied}
                    };
                },
                10000
            );

            return result.value_or(mcp::json{{"error", "Timeout"}});
        });
    }

    // Tool 6: get_all_settings
    {
        auto tool = mcp::tool_builder("get_all_settings")
            .with_description("Get ALL available settings for the currently selected preset in a category")
            .with_string_param("category", "Category: print, filament, or printer", true)
            .build();

        srv.register_tool(tool, [](const mcp::json& params, const std::string& /*session_id*/) -> mcp::json {
            std::string category = params["category"];

            auto result = MCPGUIBridge::instance().execute_on_gui<mcp::json>(
                [category]() -> mcp::json {
                    PresetCollection* collection = get_collection(category);
                    if (!collection) {
                        return {{"error", "Invalid category: " + category}};
                    }

                    const Preset& preset = collection->get_selected_preset();

                    mcp::json settings = mcp::json::array();
                    for (const std::string& key : preset.config.keys()) {
                        settings.push_back({
                            {"key", key},
                            {"value", preset.config.opt_serialize(key)}
                        });
                    }

                    return {
                        {"preset_name", preset.name},
                        {"category", category},
                        {"settings", settings},
                        {"total_settings", (int)settings.size()}
                    };
                },
                5000
            );

            return result.value_or(mcp::json{{"error", "Timeout"}});
        });
    }

    BOOST_LOG_TRIVIAL(info) << "MCP: Registered 6 preset management tools";
}

}}} // namespace Slic3r::GUI::MCP

#endif // SLIC3R_MCP_SERVER
