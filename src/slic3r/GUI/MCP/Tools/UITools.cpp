#include "UITools.hpp"

#ifdef SLIC3R_MCP_SERVER

#include "../MCPGUIBridge.hpp"

#include "mcp_server.h"
#include "mcp_tool.h"

#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/Plater.hpp"
#include "slic3r/GUI/MainFrame.hpp"
#include "slic3r/GUI/Notebook.hpp"
#include "slic3r/GUI/GLCanvas3D.hpp"
#include "slic3r/GUI/Camera.hpp"
#include "slic3r/GUI/OpenGLManager.hpp"
#include "libslic3r/GCode/ThumbnailData.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/Model.hpp"

#include <boost/log/trivial.hpp>
#include <boost/filesystem.hpp>
#include <wx/image.h>
#include <wx/glcanvas.h>
#include <algorithm>
#include <thread>

#ifdef __APPLE__
#include "slic3r/GUI/MacScreenshot.h"
#endif

namespace Slic3r { namespace GUI { namespace MCP {

// Helper: wrap a JSON object as a proper MCP content block array
// MCP spec requires tool results to be [{type: "text", text: "..."}]
static mcp::json make_text_result(const mcp::json& data)
{
    return mcp::json::array({{
        {"type", "text"},
        {"text", data.dump(2)}
    }});
}

// Helper: convert Camera::ViewAngleType string to enum
static bool parse_view_angle(const std::string& view, Camera::ViewAngleType& out)
{
    if (view == "iso")          { out = Camera::ViewAngleType::Iso; return true; }
    if (view == "top")          { out = Camera::ViewAngleType::Top; return true; }
    if (view == "bottom")       { out = Camera::ViewAngleType::Bottom; return true; }
    if (view == "front")        { out = Camera::ViewAngleType::Front; return true; }
    if (view == "rear")         { out = Camera::ViewAngleType::Rear; return true; }
    if (view == "left")         { out = Camera::ViewAngleType::Left; return true; }
    if (view == "right")        { out = Camera::ViewAngleType::Right; return true; }
    if (view == "topfront")     { out = Camera::ViewAngleType::Top_Front; return true; }
    return false;
}

void register_ui_tools(mcp::server& srv)
{
    // ---- take_screenshot ----
    {
        auto tool = mcp::tool_builder("take_screenshot")
            .with_description("Take a screenshot. Two capture modes available: "
                              "'viewport' renders the 3D view via GL framebuffer (works behind other windows), "
                              "'window' captures the full application window including sidebar, menus, toolbars "
                              "(uses native OS API, macOS only). Use 'window' mode to see UI state.")
            .with_string_param("filepath", "Output PNG file path (e.g. /tmp/screenshot.png)", true)
            .with_string_param("capture_mode", "Capture mode: 'viewport' (3D GL only, default) or 'window' (full app window)", false)
            .with_number_param("width", "Image width in pixels (default: 1280, viewport mode only)", false)
            .with_number_param("height", "Image height in pixels (default: 720, viewport mode only)", false)
            .with_string_param("view", "Camera view angle: iso, top, front, rear, left, right, bottom, topfront (viewport mode only)", false)
            .build();

        srv.register_tool(tool, [](const mcp::json& params, const std::string& /*session_id*/) -> mcp::json {
            std::string filepath = params["filepath"];
            std::string capture_mode = params.value("capture_mode", "viewport");

            // ---- Window capture mode (native OS) ----
            if (capture_mode == "window") {
#ifdef __APPLE__
                auto result = MCPGUIBridge::instance().execute_on_gui<mcp::json>(
                    [filepath]() -> mcp::json {
                        auto* mainframe = wxGetApp().mainframe;
                        if (!mainframe)
                            return {{"error", "MainFrame not available"}};

                        void* handle = mainframe->GetHandle();
                        if (!handle)
                            return {{"error", "Could not get native window handle"}};

                        bool ok = Slic3r::GUI::capture_window_screenshot(handle, filepath);
                        if (!ok)
                            return {{"error", "Native window capture failed"}};

                        return {
                            {"success", true},
                            {"filepath", filepath},
                            {"method", "native_window_capture"}
                        };
                    },
                    10000  // 10s timeout
                );
                if (!result.has_value())
                    throw mcp::mcp_exception(mcp::error_code::internal_error, "GUI bridge timeout");
                return make_text_result(result.value());
#else
                throw mcp::mcp_exception(mcp::error_code::invalid_params,
                    "Window capture mode is only available on macOS. Use 'viewport' mode.");
#endif
            }

            // ---- Viewport capture mode (GL FBO) ----
            unsigned int width = params.value("width", 1280);
            unsigned int height = params.value("height", 720);
            std::string view = params.value("view", "");

            // Parse view angle if specified
            Camera::ViewAngleType view_angle = Camera::ViewAngleType::Iso;
            bool use_custom_view = false;
            if (!view.empty()) {
                if (!parse_view_angle(view, view_angle))
                    throw mcp::mcp_exception(mcp::error_code::invalid_params,
                        "Unknown view: " + view + ". Use: iso, top, bottom, front, rear, left, right, topfront");
                use_custom_view = true;
            }

            auto result = MCPGUIBridge::instance().execute_on_gui<mcp::json>(
                [filepath, width, height, view_angle, use_custom_view]() -> mcp::json {
                    auto* plater = wxGetApp().plater();
                    if (!plater)
                        return {{"error", "Plater not available"}};

                    auto* canvas = plater->get_view3D_canvas3D();
                    if (!canvas)
                        return {{"error", "3D canvas not available"}};

                    // Make GL context current
                    if (!canvas->make_current_for_postinit())
                        return {{"error", "Failed to make GL context current"}};

                    // If view specified, set it on the camera before rendering
                    if (use_custom_view) {
                        plater->get_camera().select_view(view_angle);
                    }

                    // Render thumbnail via FBO
                    ThumbnailData thumbnail;
                    ThumbnailsParams tparams{
                        {Vec2d((double)width, (double)height)},  // sizes
                        false,   // printable_only
                        false,   // parts_only
                        true,    // show_bed
                        false,   // transparent_background
                        0,       // plate_id (first plate)
                        true     // use_plate_box
                    };

                    canvas->render_thumbnail(thumbnail, width, height, tparams,
                        Camera::EType::Perspective, view_angle, false, false);

                    if (!thumbnail.is_valid() || thumbnail.pixels.empty())
                        return {{"error", "Thumbnail rendering failed - no pixel data"}};

                    // Convert RGBA pixels to wxImage (flip vertically - GL origin is bottom-left)
                    wxImage image(thumbnail.width, thumbnail.height);
                    image.InitAlpha();

                    for (unsigned int r = 0; r < thumbnail.height; ++r) {
                        unsigned int src_row = (thumbnail.height - 1 - r) * thumbnail.width;
                        for (unsigned int c = 0; c < thumbnail.width; ++c) {
                            unsigned char* px = thumbnail.pixels.data() + 4 * (src_row + c);
                            image.SetRGB((int)c, (int)r, px[0], px[1], px[2]);
                            image.SetAlpha((int)c, (int)r, px[3]);
                        }
                    }

                    // Save as PNG
                    wxString wx_path = wxString::FromUTF8(filepath);
                    if (!image.SaveFile(wx_path, wxBITMAP_TYPE_PNG))
                        return {{"error", "Failed to save PNG to: " + filepath}};

                    return {
                        {"success", true},
                        {"filepath", filepath},
                        {"width", thumbnail.width},
                        {"height", thumbnail.height},
                        {"method", "gl_framebuffer"}
                    };
                },
                30000  // 30s timeout for GL rendering
            );

            if (!result.has_value())
                throw mcp::mcp_exception(mcp::error_code::internal_error, "GUI bridge timeout");
            return make_text_result(result.value());
        });
    }

    // ---- switch_tab ----
    {
        auto tool = mcp::tool_builder("switch_tab")
            .with_description("Switch between OrcaSlicer main view tabs. "
                              "Available tabs: home, prepare, preview, device, project")
            .with_string_param("tab", "Tab name: home, prepare, preview, device, or project", true)
            .build();

        srv.register_tool(tool, [](const mcp::json& params, const std::string& /*session_id*/) -> mcp::json {
            std::string tab_name = params["tab"];

            auto result = MCPGUIBridge::instance().execute_on_gui<mcp::json>(
                [tab_name]() -> mcp::json {
                    auto* mainframe = wxGetApp().mainframe;
                    if (!mainframe)
                        return {{"error", "MainFrame not available"}};

                    size_t tab_idx;
                    if (tab_name == "home")           tab_idx = MainFrame::tpHome;
                    else if (tab_name == "prepare")   tab_idx = MainFrame::tp3DEditor;
                    else if (tab_name == "preview")   tab_idx = MainFrame::tpPreview;
                    else if (tab_name == "device")    tab_idx = MainFrame::tpMonitor;
                    else if (tab_name == "project")   tab_idx = MainFrame::tpProject;
                    else
                        return {{"error", "Unknown tab: " + tab_name + ". Use: home, prepare, preview, device, project"}};

                    mainframe->select_tab(tab_idx);

                    return {
                        {"success", true},
                        {"tab", tab_name}
                    };
                },
                5000
            );

            if (!result.has_value())
                throw mcp::mcp_exception(mcp::error_code::internal_error, "GUI bridge timeout");
            return make_text_result(result.value());
        });
    }

    // ---- set_camera_view ----
    {
        auto tool = mcp::tool_builder("set_camera_view")
            .with_description("Control the 3D camera: change view angle, zoom, rotate, and pan. "
                              "Use this before take_screenshot to frame the scene properly. "
                              "All parameters are optional - only set the ones you need.")
            .with_string_param("view", "Preset view: iso, top, bottom, front, rear, left, right, topfront", false)
            .with_string_param("zoom_to", "Zoom target: bed, volumes (all objects), plate", false)
            .with_number_param("zoom", "Absolute zoom level (1.0 = default, 2.0 = 2x closer, 0.5 = 2x farther)", false)
            .with_number_param("rotate_azimuth", "Rotate left/right in degrees (positive = right)", false)
            .with_number_param("rotate_elevation", "Rotate up/down in degrees (positive = up, clamped -90 to 90)", false)
            .with_number_param("pan_x", "Pan camera left/right in mm (positive = right)", false)
            .with_number_param("pan_y", "Pan camera up/down in mm (positive = up)", false)
            .build();

        srv.register_tool(tool, [](const mcp::json& params, const std::string& /*session_id*/) -> mcp::json {
            std::string view = params.value("view", "");
            std::string zoom_to = params.value("zoom_to", "");
            double zoom = params.value("zoom", 0.0);
            double rot_azimuth = params.value("rotate_azimuth", 0.0);
            double rot_elevation = params.value("rotate_elevation", 0.0);
            double pan_x = params.value("pan_x", 0.0);
            double pan_y = params.value("pan_y", 0.0);

            auto result = MCPGUIBridge::instance().execute_on_gui<mcp::json>(
                [view, zoom_to, zoom, rot_azimuth, rot_elevation, pan_x, pan_y]() -> mcp::json {
                    auto* plater = wxGetApp().plater();
                    if (!plater)
                        return {{"error", "Plater not available"}};

                    auto* canvas = plater->get_view3D_canvas3D();
                    if (!canvas)
                        return {{"error", "3D canvas not available"}};

                    Camera& camera = plater->get_camera();
                    mcp::json actions = mcp::json::array();

                    // 1. Set preset view (resets camera orientation)
                    if (!view.empty()) {
                        Camera::ViewAngleType vat;
                        if (!parse_view_angle(view, vat))
                            return {{"error", "Unknown view: " + view + ". Use: iso, top, bottom, front, rear, left, right, topfront"}};
                        camera.select_view(vat);
                        actions.push_back("view=" + view);
                    }

                    // 2. Zoom to target
                    if (!zoom_to.empty()) {
                        if (zoom_to == "bed") {
                            canvas->zoom_to_bed();
                            actions.push_back("zoom_to=bed");
                        } else if (zoom_to == "volumes") {
                            canvas->zoom_to_volumes();
                            actions.push_back("zoom_to=volumes");
                        } else if (zoom_to == "plate") {
                            canvas->zoom_to_plate(-1); // current plate
                            actions.push_back("zoom_to=plate");
                        } else {
                            return {{"error", "Unknown zoom_to: " + zoom_to + ". Use: bed, volumes, plate"}};
                        }
                    }

                    // 3. Set absolute zoom
                    if (zoom > 0.0) {
                        camera.set_zoom(zoom);
                        actions.push_back("zoom=" + std::to_string(zoom));
                    }

                    // 4. Rotate camera (spherical coordinates)
                    if (rot_azimuth != 0.0 || rot_elevation != 0.0) {
                        double az_rad = rot_azimuth * M_PI / 180.0;
                        double el_rad = rot_elevation * M_PI / 180.0;
                        camera.rotate_on_sphere(az_rad, el_rad, true);
                        actions.push_back("rotate: az=" + std::to_string(rot_azimuth) +
                                          " el=" + std::to_string(rot_elevation));
                    }

                    // 5. Pan camera
                    if (pan_x != 0.0 || pan_y != 0.0) {
                        camera.translate_world(Vec3d(pan_x, pan_y, 0.0));
                        actions.push_back("pan: x=" + std::to_string(pan_x) +
                                          " y=" + std::to_string(pan_y));
                    }

                    // Report final camera state
                    mcp::json cam_state = {
                        {"zoom", camera.get_zoom()},
                        {"distance", camera.get_distance()},
                        {"zenit", camera.get_zenit()},
                        {"target", {
                            {"x", camera.get_target().x()},
                            {"y", camera.get_target().y()},
                            {"z", camera.get_target().z()}
                        }},
                        {"position", {
                            {"x", camera.get_position().x()},
                            {"y", camera.get_position().y()},
                            {"z", camera.get_position().z()}
                        }}
                    };

                    return {
                        {"success", true},
                        {"actions", actions},
                        {"camera", cam_state}
                    };
                },
                5000
            );

            if (!result.has_value())
                throw mcp::mcp_exception(mcp::error_code::internal_error, "GUI bridge timeout");
            return make_text_result(result.value());
        });
    }

    // ---- get_current_tab ----
    {
        auto tool = mcp::tool_builder("get_current_tab")
            .with_description("Get the name of the currently active OrcaSlicer tab")
            .build();

        srv.register_tool(tool, [](const mcp::json& /*params*/, const std::string& /*session_id*/) -> mcp::json {
            auto result = MCPGUIBridge::instance().execute_on_gui<mcp::json>(
                []() -> mcp::json {
                    auto* mainframe = wxGetApp().mainframe;
                    if (!mainframe)
                        return {{"error", "MainFrame not available"}};

                    int sel = mainframe->m_tabpanel->GetSelection();
                    std::string tab_name;
                    switch (sel) {
                        case MainFrame::tpHome:       tab_name = "home"; break;
                        case MainFrame::tp3DEditor:   tab_name = "prepare"; break;
                        case MainFrame::tpPreview:    tab_name = "preview"; break;
                        case MainFrame::tpMonitor:    tab_name = "device"; break;
                        case MainFrame::tpProject:    tab_name = "project"; break;
                        default:                      tab_name = "unknown(" + std::to_string(sel) + ")"; break;
                    }

                    return {
                        {"tab", tab_name},
                        {"tab_index", sel}
                    };
                },
                5000
            );

            if (!result.has_value())
                throw mcp::mcp_exception(mcp::error_code::internal_error, "GUI bridge timeout");
            return make_text_result(result.value());
        });
    }

    // ---- get_app_status ----
    {
        auto tool = mcp::tool_builder("get_app_status")
            .with_description(
                "IMPORTANT: Call this tool FIRST when starting a new session. "
                "Returns the full application state: current tab, loaded objects, "
                "active presets, build plate info, and usage tips. "
                "This is essential to understand the current context before performing any action.")
            .build();

        srv.register_tool(tool, [](const mcp::json& /*params*/, const std::string& /*session_id*/) -> mcp::json {
            auto result = MCPGUIBridge::instance().execute_on_gui<mcp::json>(
                []() -> mcp::json {
                    auto& app = wxGetApp();
                    auto* mainframe = app.mainframe;
                    auto* plater = app.plater();
                    auto* bundle = app.preset_bundle;

                    if (!mainframe || !plater || !bundle)
                        return {{"error", "Application not fully initialized"}};

                    mcp::json status = mcp::json::object();

                    // --- Current tab ---
                    int sel = mainframe->m_tabpanel->GetSelection();
                    std::string tab_name;
                    switch (sel) {
                        case MainFrame::tpHome:       tab_name = "home"; break;
                        case MainFrame::tp3DEditor:   tab_name = "prepare"; break;
                        case MainFrame::tpPreview:    tab_name = "preview"; break;
                        case MainFrame::tpMonitor:    tab_name = "device"; break;
                        case MainFrame::tpProject:    tab_name = "project"; break;
                        default:                      tab_name = "unknown"; break;
                    }
                    status["current_tab"] = tab_name;

                    // --- Active presets ---
                    mcp::json presets = mcp::json::object();
                    presets["printer"] = bundle->printers.get_selected_preset_name();
                    presets["print_profile"] = bundle->prints.get_selected_preset_name();

                    // Filaments (one per extruder)
                    mcp::json filaments = mcp::json::array();
                    for (size_t i = 0; i < bundle->filament_presets.size(); ++i) {
                        filaments.push_back({
                            {"extruder", i + 1},
                            {"preset", bundle->filament_presets[i]}
                        });
                    }
                    presets["filaments"] = filaments;
                    status["presets"] = presets;

                    // --- Key print settings ---
                    const auto& print_config = bundle->prints.get_edited_preset().config;
                    mcp::json settings = mcp::json::object();
                    auto safe_get = [&](const std::string& key) -> std::string {
                        return print_config.has(key) ? print_config.opt_serialize(key) : "";
                    };
                    settings["layer_height"] = safe_get("layer_height");
                    settings["initial_layer_print_height"] = safe_get("initial_layer_print_height");
                    settings["wall_loops"] = safe_get("wall_loops");
                    settings["sparse_infill_density"] = safe_get("sparse_infill_density");
                    settings["sparse_infill_pattern"] = safe_get("sparse_infill_pattern");
                    settings["support_type"] = safe_get("support_type");
                    settings["enable_support"] = safe_get("enable_support");
                    status["key_settings"] = settings;

                    // --- Objects on build plate ---
                    const Model& model = plater->model();
                    mcp::json objects = mcp::json::array();
                    for (size_t i = 0; i < model.objects.size(); ++i) {
                        const auto* obj = model.objects[i];
                        if (!obj) continue;
                        auto bb = obj->bounding_box_exact();
                        mcp::json obj_info = {
                            {"index", i},
                            {"object_id", (int)obj->id().id},
                            {"name", obj->name},
                            {"instances", obj->instances.size()}
                        };
                        if (!obj->instances.empty()) {
                            auto offset = obj->instances[0]->get_offset();
                            obj_info["position"] = {{"x", offset.x()}, {"y", offset.y()}, {"z", offset.z()}};
                        }
                        obj_info["size"] = {
                            {"x", bb.size().x()},
                            {"y", bb.size().y()},
                            {"z", bb.size().z()}
                        };
                        objects.push_back(obj_info);
                    }
                    status["objects"] = objects;
                    status["object_count"] = model.objects.size();

                    // --- Build plate info ---
                    const auto& printer_config = bundle->printers.get_edited_preset().config;
                    mcp::json build_plate = mcp::json::object();
                    if (printer_config.has("printable_area")) {
                        build_plate["printable_area"] = printer_config.opt_serialize("printable_area");
                    }
                    if (printer_config.has("printable_height")) {
                        build_plate["printable_height"] = printer_config.opt_serialize("printable_height");
                    }
                    status["build_plate"] = build_plate;

                    // --- Warnings / actionable tips ---
                    mcp::json warnings = mcp::json::array();
                    if (tab_name == "home") {
                        warnings.push_back("App is on the 'home' tab. Call switch_tab with tab='prepare' "
                                           "before loading models or modifying the scene.");
                    }
                    if (model.objects.empty()) {
                        warnings.push_back("No objects on the build plate. Use load_model to add a 3D model (STL recommended).");
                    }
                    status["warnings"] = warnings;

                    // --- Tips for AI agents ---
                    status["tips"] = mcp::json::array({
                        "Always call get_app_status first to understand the current state.",
                        "Use switch_tab to 'prepare' before scene operations (load, move, rotate, etc.).",
                        "After modifying settings, the UI updates automatically. Check with get_print_settings.",
                        "Use object_id (not index) to target specific objects in move/rotate/scale/delete tools.",
                        "For multi-extruder printers, use the 'extruder' parameter in select_preset for filament.",
                        "load_model supports STL, OBJ, AMF formats. Avoid 3MF as it may trigger UI dialogs.",
                        "After slicing, use get_slice_statistics to review the results.",
                        "move_object supports Z positioning for stacking objects above the bed.",
                        "Use new_project to reset the build plate and start fresh.",
                        "Use open_project to load a 3MF project file (preserves presets, plate layout, etc.)."
                    });

                    return status;
                },
                5000
            );

            if (!result.has_value())
                throw mcp::mcp_exception(mcp::error_code::internal_error, "GUI bridge timeout");
            return make_text_result(result.value());
        });
    }

    // ---- new_project ----
    {
        auto tool = mcp::tool_builder("new_project")
            .with_description("Create a new empty project. Clears all objects from the build plate, "
                              "resets project state, and switches to the prepare tab. "
                              "Current presets (printer, filament, print profile) are preserved.")
            .build();

        srv.register_tool(tool, [](const mcp::json& /*params*/, const std::string& /*session_id*/) -> mcp::json {
            auto result = MCPGUIBridge::instance().execute_on_gui<mcp::json>(
                []() -> mcp::json {
                    auto* plater = wxGetApp().plater();
                    if (!plater)
                        return {{"error", "Plater not available"}};

                    // skip_confirm=true to avoid blocking dialog, silent=false to switch to prepare tab
                    int res = plater->new_project(/*skip_confirm=*/true, /*silent=*/false);

                    return {
                        {"success", true},
                        {"message", "New project created. Build plate is empty."}
                    };
                },
                10000  // 10s timeout (project reset can take a moment)
            );

            if (!result.has_value())
                throw mcp::mcp_exception(mcp::error_code::internal_error, "GUI bridge timeout");
            return make_text_result(result.value());
        });
    }

    // ---- open_project ----
    {
        auto tool = mcp::tool_builder("open_project")
            .with_description("Open a 3MF project file. This loads the full project including objects, "
                              "presets, plate layout, and all settings. Unlike load_model which just adds "
                              "a model to the scene, open_project replaces the entire project state. "
                              "The filepath must point to a .3mf file.")
            .with_string_param("filepath", "Path to the 3MF project file to open", true)
            .build();

        srv.register_tool(tool, [](const mcp::json& params, const std::string& /*session_id*/) -> mcp::json {
            std::string filepath = params["filepath"];

            // Validate file exists and is .3mf
            if (filepath.empty())
                throw mcp::mcp_exception(mcp::error_code::invalid_params, "filepath is required");

            auto result = MCPGUIBridge::instance().execute_on_gui<mcp::json>(
                [filepath]() -> mcp::json {
                    auto* plater = wxGetApp().plater();
                    if (!plater)
                        return {{"error", "Plater not available"}};

                    // Check file exists
                    if (!boost::filesystem::exists(filepath))
                        return {{"error", "File not found: " + filepath}};

                    // Check extension
                    std::string ext = boost::filesystem::extension(filepath);
                    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
                    if (ext != ".3mf")
                        return {{"error", "open_project only supports .3mf files. Got: " + ext + ". Use load_model for STL/OBJ/AMF."}};

                    wxString wx_path = wxString::FromUTF8(filepath);
                    plater->load_project(wx_path);

                    // Report what was loaded
                    const Model& model = plater->model();
                    mcp::json objects = mcp::json::array();
                    for (size_t i = 0; i < model.objects.size(); ++i) {
                        const auto* obj = model.objects[i];
                        if (!obj) continue;
                        objects.push_back({
                            {"object_id", (int)obj->id().id},
                            {"name", obj->name},
                            {"instances", (int)obj->instances.size()}
                        });
                    }

                    return {
                        {"success", true},
                        {"filepath", filepath},
                        {"objects_loaded", (int)model.objects.size()},
                        {"objects", objects}
                    };
                },
                30000  // 30s timeout for project loading
            );

            if (!result.has_value())
                throw mcp::mcp_exception(mcp::error_code::internal_error, "GUI bridge timeout");
            return make_text_result(result.value());
        });
    }

    BOOST_LOG_TRIVIAL(info) << "MCP: Registered 7 UI tools (screenshot, camera, tab navigation, app status, new/open project)";
}

}}} // namespace Slic3r::GUI::MCP

#endif // SLIC3R_MCP_SERVER
