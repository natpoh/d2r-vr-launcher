// gui_window.h - the launcher window (plain Win32). Everything it decides comes from the Model (gui_model.h).
#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include "gui_model.h"

namespace vrml {
namespace gui {

// Creates the window and runs its message loop. Returns the process exit code: 0, or what `--screenshot` failed with.
int RunWindow(HINSTANCE instance, Model& model);

}  // namespace gui
}  // namespace vrml
