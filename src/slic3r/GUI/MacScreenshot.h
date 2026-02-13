#pragma once

#include <string>

namespace Slic3r { namespace GUI {

// Capture the full application window using native macOS CGWindowListCreateImage API.
// nswindow_handle: the NSWindow* handle from wxTopLevelWindow::GetHandle()
// filepath: path to save the PNG screenshot
// Returns true on success.
bool capture_window_screenshot(void* nswindow_handle, const std::string& filepath);

}} // namespace Slic3r::GUI
