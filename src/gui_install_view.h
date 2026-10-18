// gui_install_view.h - the install view: one window over the launcher's main window with the three pieces (D2RLoader, D2R VR Authentic, D2R VR Unleashed),
// what is installed, what is newest, boxes to tick, Install selected / Cancel / Close, a progress bar and a log. What it decides is gui_install_model.*; what it
// does is install_run.* on a worker thread (so the window never freezes: the question to the servers and the install both run off the UI thread).
#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <vector>

#include "gui_model.h"

namespace vrml {
namespace gui {

struct InstallViewResult {
    bool changed = false;          // something was installed (or tried): the main window looks at the disk again
    bool shot_failed = false;      // --screenshot could not be written
};

// Opens the view over `owner` and runs its own message loop until it is closed; `owner` is disabled meanwhile. `tick` (when `explicit_tick`) are the
// pieces to tick at first - what a card's Install / Update button asks for; otherwise the boxes follow the facts (missing or outdated = ticked).
InstallViewResult ShowInstallView(HINSTANCE instance, HWND owner, Model& model, const std::vector<Piece>& tick, bool explicit_tick);

}  // namespace gui
}  // namespace vrml
