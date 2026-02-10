#include "SceneTools.hpp"

#ifdef SLIC3R_MCP_SERVER

#include "../MCPGUIBridge.hpp"

#include "mcp_server.h"
#include "mcp_tool.h"

#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/Plater.hpp"
#include "slic3r/GUI/Selection.hpp"
#include "slic3r/GUI/Tab.hpp"
#include "slic3r/GUI/PresetComboBoxes.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/Geometry.hpp"
#include "libslic3r/Config.hpp"

#include <boost/filesystem.hpp>
#include <boost/log/trivial.hpp>
#include <sstream>

namespace Slic3r { namespace GUI { namespace MCP {

// Helper to find object by ID or name, returns object and its index
static std::pair<ModelObject*, size_t> find_object_by_id_or_name(Plater* plater, const mcp::json& params) {
    if (!plater) {
        return {nullptr, 0};
    }

    Model& model = plater->model();

    // Try by ID first
    if (params.contains("object_id")) {
        int obj_id = params["object_id"];
        for (size_t i = 0; i < model.objects.size(); i++) {
            if ((int)model.objects[i]->id().id == obj_id) {
                return {model.objects[i], i};
            }
        }
    }

    // Try by name
    if (params.contains("object_name")) {
        std::string name = params["object_name"];
        for (size_t i = 0; i < model.objects.size(); i++) {
            if (model.objects[i]->name == name) {
                return {model.objects[i], i};
            }
        }
    }

    return {nullptr, 0};
}

void register_scene_tools(mcp::server& srv)
{
    // Tool 1: load_model
    {
        auto tool = mcp::tool_builder("load_model")
            .with_description("Load a 3D model file onto the build plate. "
                              "Supported formats: STL (recommended), OBJ, AMF. "
                              "AVOID loading 3MF files as they may trigger blocking UI dialogs. "
                              "Make sure the app is on the 'prepare' tab first (use switch_tab).")
            .with_string_param("filepath", "Path to model file", true)
            .with_boolean_param("center", "Center on build plate (default: true)", false)
            .with_boolean_param("auto_orient", "Auto-orient for printing (default: false)", false)
            .with_number_param("instances", "Number of copies (default: 1)", false)
            .build();

        srv.register_tool(tool, [](const mcp::json& params, const std::string& /*session_id*/) -> mcp::json {
            std::string filepath = params["filepath"];
            bool center = params.value("center", true);
            bool do_auto_orient = params.value("auto_orient", false);
            int instances = params.value("instances", 1);

            auto result = MCPGUIBridge::instance().execute_on_gui<mcp::json>(
                [filepath, do_auto_orient, instances]() -> mcp::json {
                    auto* plater = GUI::wxGetApp().plater();
                    if (!plater) {
                        return {{"error", "Plater not available"}};
                    }

                    if (!boost::filesystem::exists(filepath)) {
                        return {{"error", "File not found: " + filepath}};
                    }

                    std::string ext = boost::filesystem::extension(filepath);
                    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);

                    if (ext != ".stl" && ext != ".3mf" && ext != ".obj" && ext != ".amf") {
                        return {{"error", "Unsupported file format: " + ext}};
                    }

                    auto loaded_indices = plater->load_files(std::vector<std::string>{filepath});
                    if (loaded_indices.empty()) {
                        return {{"error", "Failed to load model"}};
                    }

                    Model& model = plater->model();
                    if (model.objects.empty()) {
                        return {{"error", "No objects loaded"}};
                    }

                    ModelObject* obj = model.objects.back();

                    if (do_auto_orient) {
                        plater->optimize_rotation();
                    }

                    if (instances > 1) {
                        for (int i = 1; i < instances; i++) {
                            obj->add_instance();
                        }
                        plater->arrange();
                    }

                    return {
                        {"object_id", (int)obj->id().id},
                        {"object_name", obj->name},
                        {"instances", (int)obj->instances.size()}
                    };
                },
                30000
            );

            return result.value_or(mcp::json{{"error", "Timeout loading model"}});
        });
    }

    // Tool 2: move_object
    {
        auto tool = mcp::tool_builder("move_object")
            .with_description("Move an object to a specific position. Supports Z>0 for stacking objects. "
                              "Use object_id (from get_model_info) to target a specific object.")
            .with_number_param("object_id", "Object ID", false)
            .with_string_param("object_name", "Object name", false)
            .with_number_param("x", "X position in mm", true)
            .with_number_param("y", "Y position in mm", true)
            .with_number_param("z", "Z position in mm (default: 0)", false)
            .with_boolean_param("relative", "Relative movement (default: false)", false)
            .build();

        srv.register_tool(tool, [](const mcp::json& params, const std::string& /*session_id*/) -> mcp::json {
            auto result = MCPGUIBridge::instance().execute_on_gui<mcp::json>(
                [params]() -> mcp::json {
                    auto* plater = GUI::wxGetApp().plater();
                    auto [obj, idx] = find_object_by_id_or_name(plater, params);

                    if (!obj) {
                        return {{"error", "Object not found"}};
                    }

                    if (obj->instances.empty()) {
                        return {{"error", "Object has no instances"}};
                    }

                    double x = params["x"];
                    double y = params["y"];
                    double z = params.value("z", 0.0);
                    bool relative = params.value("relative", false);

                    ModelInstance* instance = obj->instances[0];
                    Vec3d new_pos(x, y, z);

                    if (relative) {
                        new_pos += instance->get_offset();
                    }

                    instance->set_offset(new_pos);
                    obj->invalidate_bounding_box();
                    plater->changed_object(idx);

                    // changed_object() auto-snaps Z to bed surface.
                    // Re-apply Z if explicitly requested above bed level.
                    if (new_pos.z() != 0.0 && instance->get_offset().z() != new_pos.z()) {
                        instance->set_offset(new_pos);
                        obj->invalidate_bounding_box();
                        plater->update();
                    }

                    return {
                        {"object_name", obj->name},
                        {"position", {
                            {"x", instance->get_offset().x()},
                            {"y", instance->get_offset().y()},
                            {"z", instance->get_offset().z()}
                        }}
                    };
                },
                5000
            );

            return result.value_or(mcp::json{{"error", "Timeout"}});
        });
    }

    // Tool 3: rotate_object
    {
        auto tool = mcp::tool_builder("rotate_object")
            .with_description("Rotate an object around X/Y/Z axes (degrees)")
            .with_number_param("object_id", "Object ID", false)
            .with_string_param("object_name", "Object name", false)
            .with_number_param("x", "Rotation around X axis (degrees)", false)
            .with_number_param("y", "Rotation around Y axis (degrees)", false)
            .with_number_param("z", "Rotation around Z axis (degrees)", false)
            .with_boolean_param("relative", "Relative rotation (default: false)", false)
            .build();

        srv.register_tool(tool, [](const mcp::json& params, const std::string& /*session_id*/) -> mcp::json {
            auto result = MCPGUIBridge::instance().execute_on_gui<mcp::json>(
                [params]() -> mcp::json {
                    auto* plater = GUI::wxGetApp().plater();
                    auto [obj, idx] = find_object_by_id_or_name(plater, params);

                    if (!obj || obj->instances.empty()) {
                        return {{"error", "Object not found or has no instances"}};
                    }

                    double x_deg = params.value("x", 0.0);
                    double y_deg = params.value("y", 0.0);
                    double z_deg = params.value("z", 0.0);
                    bool relative = params.value("relative", false);

                    Vec3d rotation(
                        Geometry::deg2rad(x_deg),
                        Geometry::deg2rad(y_deg),
                        Geometry::deg2rad(z_deg)
                    );

                    ModelInstance* instance = obj->instances[0];

                    if (relative) {
                        rotation += instance->get_rotation();
                    }

                    instance->set_rotation(rotation);
                    obj->invalidate_bounding_box();
                    obj->ensure_on_bed(false);
                    plater->update();

                    return {
                        {"object_name", obj->name},
                        {"rotation_degrees", {
                            {"x", Geometry::rad2deg(instance->get_rotation().x())},
                            {"y", Geometry::rad2deg(instance->get_rotation().y())},
                            {"z", Geometry::rad2deg(instance->get_rotation().z())}
                        }}
                    };
                },
                5000
            );

            return result.value_or(mcp::json{{"error", "Timeout"}});
        });
    }

    // Tool 4: scale_object
    {
        auto tool = mcp::tool_builder("scale_object")
            .with_description("Scale an object (uniform or per-axis). Provide 'scale' for uniform or 'x/y/z' for per-axis.")
            .with_number_param("object_id", "Object ID", false)
            .with_string_param("object_name", "Object name", false)
            .with_number_param("scale", "Uniform scale factor (e.g., 2.0 = double size)", false)
            .with_number_param("x", "Scale X factor", false)
            .with_number_param("y", "Scale Y factor", false)
            .with_number_param("z", "Scale Z factor", false)
            .build();

        srv.register_tool(tool, [](const mcp::json& params, const std::string& /*session_id*/) -> mcp::json {
            auto result = MCPGUIBridge::instance().execute_on_gui<mcp::json>(
                [params]() -> mcp::json {
                    auto* plater = GUI::wxGetApp().plater();
                    auto [obj, idx] = find_object_by_id_or_name(plater, params);

                    if (!obj || obj->instances.empty()) {
                        return {{"error", "Object not found"}};
                    }

                    ModelInstance* instance = obj->instances[0];
                    Vec3d scale_factors;

                    if (params.contains("scale")) {
                        double scale = params["scale"];
                        scale_factors = Vec3d(scale, scale, scale);
                    } else if (params.contains("x") || params.contains("y") || params.contains("z")) {
                        scale_factors = Vec3d(
                            params.value("x", 1.0),
                            params.value("y", 1.0),
                            params.value("z", 1.0)
                        );
                    } else {
                        return {{"error", "Must provide 'scale' or 'x/y/z'"}};
                    }

                    instance->set_scaling_factor(scale_factors);
                    obj->invalidate_bounding_box();
                    plater->changed_object(idx);

                    return {
                        {"object_name", obj->name},
                        {"scale", {
                            {"x", instance->get_scaling_factor().x()},
                            {"y", instance->get_scaling_factor().y()},
                            {"z", instance->get_scaling_factor().z()}
                        }}
                    };
                },
                5000
            );

            return result.value_or(mcp::json{{"error", "Timeout"}});
        });
    }

    // Tool 5: duplicate_object
    {
        auto tool = mcp::tool_builder("duplicate_object")
            .with_description("Duplicate an object on the build plate")
            .with_number_param("object_id", "Object ID", false)
            .with_string_param("object_name", "Object name", false)
            .with_number_param("count", "Number of copies to create (default: 1)", false)
            .with_boolean_param("auto_arrange", "Auto-arrange after duplicating (default: true)", false)
            .build();

        srv.register_tool(tool, [](const mcp::json& params, const std::string& /*session_id*/) -> mcp::json {
            auto result = MCPGUIBridge::instance().execute_on_gui<mcp::json>(
                [params]() -> mcp::json {
                    auto* plater = GUI::wxGetApp().plater();
                    auto [obj, idx] = find_object_by_id_or_name(plater, params);

                    if (!obj) {
                        return {{"error", "Object not found"}};
                    }

                    int count = params.value("count", 1);
                    bool auto_arrange = params.value("auto_arrange", true);

                    Model& model = plater->model();
                    for (int i = 0; i < count; i++) {
                        model.add_object(*obj);
                    }

                    if (auto_arrange) {
                        plater->arrange();
                    }

                    return {
                        {"original_object", obj->name},
                        {"copies_created", count},
                        {"total_objects", (int)model.objects.size()}
                    };
                },
                10000
            );

            return result.value_or(mcp::json{{"error", "Timeout"}});
        });
    }

    // Tool 6: auto_orient
    {
        auto tool = mcp::tool_builder("auto_orient")
            .with_description("Automatically optimize orientation of all objects for better printing")
            .build();

        srv.register_tool(tool, [](const mcp::json& /*params*/, const std::string& /*session_id*/) -> mcp::json {
            auto result = MCPGUIBridge::instance().execute_on_gui<mcp::json>(
                []() -> mcp::json {
                    auto* plater = GUI::wxGetApp().plater();
                    if (!plater) {
                        return {{"error", "Plater not available"}};
                    }

                    plater->optimize_rotation();

                    return {{"success", true}, {"message", "Orientation optimized"}};
                },
                15000
            );

            return result.value_or(mcp::json{{"error", "Timeout"}});
        });
    }

    // Tool 7: arrange_objects
    {
        auto tool = mcp::tool_builder("arrange_objects")
            .with_description("Automatically arrange all objects on the build plate for optimal placement")
            .build();

        srv.register_tool(tool, [](const mcp::json& /*params*/, const std::string& /*session_id*/) -> mcp::json {
            auto result = MCPGUIBridge::instance().execute_on_gui<mcp::json>(
                []() -> mcp::json {
                    auto* plater = GUI::wxGetApp().plater();
                    if (!plater) {
                        return {{"error", "Plater not available"}};
                    }

                    plater->arrange();

                    return {{"objects_arranged", (int)plater->model().objects.size()}};
                },
                20000
            );

            return result.value_or(mcp::json{{"error", "Timeout"}});
        });
    }

    // Tool 8: delete_object
    {
        auto tool = mcp::tool_builder("delete_object")
            .with_description("Remove an object from the build plate by ID or name")
            .with_number_param("object_id", "Object ID", false)
            .with_string_param("object_name", "Object name", false)
            .build();

        srv.register_tool(tool, [](const mcp::json& params, const std::string& /*session_id*/) -> mcp::json {
            auto result = MCPGUIBridge::instance().execute_on_gui<mcp::json>(
                [params]() -> mcp::json {
                    auto* plater = GUI::wxGetApp().plater();
                    auto [obj, obj_idx] = find_object_by_id_or_name(plater, params);

                    if (!obj) {
                        return {{"error", "Object not found"}};
                    }

                    std::string obj_name = obj->name;
                    plater->remove(obj_idx);

                    return {
                        {"deleted_object", obj_name},
                        {"remaining_objects", (int)plater->model().objects.size()}
                    };
                },
                5000
            );

            return result.value_or(mcp::json{{"error", "Timeout"}});
        });
    }

    // Tool 9: set_filament_color
    {
        auto tool = mcp::tool_builder("set_filament_color")
            .with_description("Set the color of a filament slot for preview. Changes the display color in the 3D view. Provide hex color (e.g. #FF0000) or RGB values.")
            .with_number_param("filament_id", "Filament ID (1-based)", true)
            .with_string_param("color", "Hex color (e.g., #FF5733)", false)
            .with_number_param("r", "Red (0-255)", false)
            .with_number_param("g", "Green (0-255)", false)
            .with_number_param("b", "Blue (0-255)", false)
            .build();

        srv.register_tool(tool, [](const mcp::json& params, const std::string& /*session_id*/) -> mcp::json {
            auto result = MCPGUIBridge::instance().execute_on_gui<mcp::json>(
                [params]() -> mcp::json {
                    int filament_id = params["filament_id"];
                    if (filament_id < 1) {
                        return {{"error", "Invalid filament_id, must be >= 1"}};
                    }

                    std::string color_str;
                    if (params.contains("color")) {
                        color_str = params["color"];
                    } else if (params.contains("r") && params.contains("g") && params.contains("b")) {
                        int r = params["r"];
                        int g = params["g"];
                        int b = params["b"];
                        char hex[8];
                        snprintf(hex, sizeof(hex), "#%02X%02X%02X", r, g, b);
                        color_str = hex;
                    } else {
                        return {{"error", "Must provide 'color' or 'r,g,b'"}};
                    }

                    auto* preset_bundle = GUI::wxGetApp().preset_bundle;
                    if (!preset_bundle) {
                        return {{"error", "Preset bundle not available"}};
                    }

                    auto* plater = GUI::wxGetApp().plater();
                    if (!plater) {
                        return {{"error", "Plater not available"}};
                    }

                    // Get the filament colour option from project config
                    ConfigOptionStrings* filament_color = preset_bundle->project_config.option<ConfigOptionStrings>("filament_colour");
                    if (!filament_color) {
                        return {{"error", "filament_colour config not found"}};
                    }

                    int idx = filament_id - 1; // Convert to 0-based
                    if (idx >= (int)filament_color->values.size()) {
                        return {{"error", "filament_id " + std::to_string(filament_id) + " exceeds current filament count (" + std::to_string(filament_color->values.size()) + "). Use set_filament_count first."}};
                    }

                    // Set the color
                    std::string old_color = filament_color->values[idx];
                    filament_color->values[idx] = color_str;

                    // Also update multi-color to keep in sync
                    ConfigOptionStrings* filament_multi_color = preset_bundle->project_config.option<ConfigOptionStrings>("filament_multi_colour");
                    if (filament_multi_color && idx < (int)filament_multi_color->values.size()) {
                        filament_multi_color->values[idx] = color_str;
                    }

                    // Propagate: update full config and refresh UI
                    plater->update_filament_colors_in_full_config();
                    plater->sidebar().obj_list()->update_filament_colors();
                    plater->sidebar().update_dynamic_filament_list();
                    plater->update();

                    return {
                        {"filament_id", filament_id},
                        {"old_color", old_color},
                        {"new_color", color_str}
                    };
                },
                10000
            );

            return result.value_or(mcp::json{{"error", "Timeout"}});
        });
    }

    // Tool 10: assign_filament
    {
        auto tool = mcp::tool_builder("assign_filament")
            .with_description("Assign a filament/extruder to a specific volume of an object")
            .with_number_param("object_id", "Object ID", false)
            .with_string_param("object_name", "Object name", false)
            .with_number_param("volume_id", "Volume index (0 for first volume)", false)
            .with_number_param("filament_id", "Filament ID (1-based)", true)
            .build();

        srv.register_tool(tool, [](const mcp::json& params, const std::string& /*session_id*/) -> mcp::json {
            auto result = MCPGUIBridge::instance().execute_on_gui<mcp::json>(
                [params]() -> mcp::json {
                    auto* plater = GUI::wxGetApp().plater();
                    auto [obj, idx] = find_object_by_id_or_name(plater, params);

                    if (!obj) {
                        return {{"error", "Object not found"}};
                    }

                    int volume_id = params.value("volume_id", 0);
                    int filament_id = params["filament_id"];

                    if (filament_id < 1) {
                        return {{"error", "Invalid filament_id, must be >= 1"}};
                    }

                    if (volume_id < 0 || volume_id >= (int)obj->volumes.size()) {
                        return {{"error", "Invalid volume_id"}};
                    }

                    ModelVolume* volume = obj->volumes[volume_id];
                    // Set extruder via config - extruder is stored as a config option
                    volume->config.set_key_value("extruder", new ConfigOptionInt(filament_id));

                    return {
                        {"object_name", obj->name},
                        {"volume_id", volume_id},
                        {"volume_name", volume->name},
                        {"filament_id", filament_id}
                    };
                },
                5000
            );

            return result.value_or(mcp::json{{"error", "Timeout"}});
        });
    }

    // Tool 11: get_plate_info
    {
        auto tool = mcp::tool_builder("get_plate_info")
            .with_description("Get detailed information about all objects on the build plate including position, rotation, scale, volumes")
            .build();

        srv.register_tool(tool, [](const mcp::json& /*params*/, const std::string& /*session_id*/) -> mcp::json {
            auto result = MCPGUIBridge::instance().execute_on_gui<mcp::json>(
                []() -> mcp::json {
                    auto* plater = GUI::wxGetApp().plater();
                    if (!plater) {
                        return {{"error", "Plater not available"}};
                    }

                    Model& model = plater->model();
                    mcp::json objects = mcp::json::array();

                    for (size_t i = 0; i < model.objects.size(); i++) {
                        ModelObject* obj = model.objects[i];

                        mcp::json obj_info;
                        obj_info["index"] = i;
                        obj_info["id"] = (int)obj->id().id;
                        obj_info["name"] = obj->name;
                        obj_info["instances"] = (int)obj->instances.size();

                        if (!obj->instances.empty()) {
                            ModelInstance* inst = obj->instances[0];

                            obj_info["position"] = {
                                {"x", inst->get_offset().x()},
                                {"y", inst->get_offset().y()},
                                {"z", inst->get_offset().z()}
                            };

                            obj_info["rotation"] = {
                                {"x", Geometry::rad2deg(inst->get_rotation().x())},
                                {"y", Geometry::rad2deg(inst->get_rotation().y())},
                                {"z", Geometry::rad2deg(inst->get_rotation().z())}
                            };

                            obj_info["scale"] = {
                                {"x", inst->get_scaling_factor().x()},
                                {"y", inst->get_scaling_factor().y()},
                                {"z", inst->get_scaling_factor().z()}
                            };
                        }

                        BoundingBoxf3 bbox = obj->raw_bounding_box();
                        obj_info["bounding_box"] = {
                            {"min", {{"x", bbox.min.x()}, {"y", bbox.min.y()}, {"z", bbox.min.z()}}},
                            {"max", {{"x", bbox.max.x()}, {"y", bbox.max.y()}, {"z", bbox.max.z()}}},
                            {"size", {{"x", bbox.size().x()}, {"y", bbox.size().y()}, {"z", bbox.size().z()}}}
                        };

                        mcp::json volumes = mcp::json::array();
                        for (size_t v = 0; v < obj->volumes.size(); v++) {
                            ModelVolume* vol = obj->volumes[v];
                            volumes.push_back({
                                {"id", (int)v},
                                {"name", vol->name},
                                {"type", vol->is_model_part() ? "model_part" : "modifier"},
                                {"filament_id", vol->extruder_id()}
                            });
                        }
                        obj_info["volumes"] = volumes;

                        objects.push_back(obj_info);
                    }

                    return {
                        {"objects", objects},
                        {"total_objects", (int)model.objects.size()}
                    };
                },
                5000
            );

            return result.value_or(mcp::json{{"error", "Timeout"}});
        });
    }

    // Tool 12: set_filament_count
    {
        auto tool = mcp::tool_builder("set_filament_count")
            .with_description("Set the number of filament slots. Use this to add more filament slots for multi-color printing. New slots get a default color or you can provide colors.")
            .with_number_param("count", "Number of filament slots (1-16)", true)
            .with_string_param("colors", "Comma-separated hex colors for new slots (e.g., '#FF0000,#00FF00,#0000FF'). If fewer colors than new slots, defaults are used.", false)
            .build();

        srv.register_tool(tool, [](const mcp::json& params, const std::string& /*session_id*/) -> mcp::json {
            auto result = MCPGUIBridge::instance().execute_on_gui<mcp::json>(
                [params]() -> mcp::json {
                    int count = params["count"];
                    if (count < 1 || count > 16) {
                        return {{"error", "count must be between 1 and 16"}};
                    }

                    auto* preset_bundle = GUI::wxGetApp().preset_bundle;
                    if (!preset_bundle) {
                        return {{"error", "Preset bundle not available"}};
                    }

                    auto* plater = GUI::wxGetApp().plater();
                    if (!plater) {
                        return {{"error", "Plater not available"}};
                    }

                    int old_count = (int)preset_bundle->filament_presets.size();

                    // Parse colors for new slots
                    std::vector<std::string> new_colors;
                    if (params.contains("colors") && params["colors"].is_string()) {
                        std::string colors_str = params["colors"];
                        std::stringstream ss(colors_str);
                        std::string color;
                        while (std::getline(ss, color, ',')) {
                            size_t start = color.find_first_not_of(" \t");
                            size_t end = color.find_last_not_of(" \t");
                            if (start != std::string::npos) {
                                new_colors.push_back(color.substr(start, end - start + 1));
                            }
                        }
                    }

                    // Add filaments one by one, replicating Sidebar::add_custom_filament()
                    // This ensures all config options are properly resized and UI combos created
                    if (count > old_count) {
                        for (int i = old_count; i < count; i++) {
                            int new_filament_count = i + 1;
                            // Pick color: from user-provided list or OrcaSlicer default
                            std::string color_str;
                            if ((i - old_count) < (int)new_colors.size()) {
                                color_str = new_colors[i - old_count];
                            } else {
                                wxColour def_col = Plater::get_next_color_for_filament();
                                color_str = def_col.GetAsString(wxC2S_HTML_SYNTAX).ToStdString();
                            }

                            // Use single-string overload which properly resizes ALL config options:
                            // filament_colour, filament_multi_colour, filament_colour_type, filament_map
                            preset_bundle->set_num_filaments((unsigned int)new_filament_count, color_str);
                            plater->get_partplate_list().on_filament_added(new_filament_count);
                            plater->on_filament_count_change(new_filament_count);
                        }
                    } else if (count < old_count) {
                        // Remove filaments from the end
                        for (int i = old_count; i > count; i--) {
                            plater->sidebar().delete_filament((size_t)(i - 1));
                        }
                    }

                    // Final UI refresh
                    auto* tab = GUI::wxGetApp().get_tab(Preset::TYPE_PRINT);
                    if (tab) tab->update();
                    preset_bundle->export_selections(*GUI::wxGetApp().app_config);

                    // Update all filament combos
                    for (auto& combo : plater->sidebar().combos_filament())
                        combo->update();

                    // Return the current filament colors
                    ConfigOptionStrings* filament_color = preset_bundle->project_config.option<ConfigOptionStrings>("filament_colour");
                    mcp::json color_list = mcp::json::array();
                    if (filament_color) {
                        for (size_t i = 0; i < filament_color->values.size(); i++) {
                            color_list.push_back({
                                {"filament_id", (int)i + 1},
                                {"color", filament_color->values[i]}
                            });
                        }
                    }

                    return {
                        {"old_count", old_count},
                        {"new_count", count},
                        {"filaments", color_list}
                    };
                },
                10000
            );

            return result.value_or(mcp::json{{"error", "Timeout"}});
        });
    }

    BOOST_LOG_TRIVIAL(info) << "MCP: Registered 13 scene management tools";
}

}}} // namespace Slic3r::GUI::MCP

#endif // SLIC3R_MCP_SERVER
