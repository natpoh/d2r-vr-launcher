// gui_uninstall.h - starting a mod's OWN uninstaller from a window (the cards' Uninstall button and the install view's row button share this). The launcher never
// deletes a mod's folder itself: it runs the uninstaller the mod's Setup left (D2R VR Authentic's unins000.exe in the game folder, D2R VR Unleashed's in
// %LOCALAPPDATA%\Programs\D2R PCVR), and when that program ends the window looks at the disk again.
#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <functional>
#include <string>

#include "gui_model.h"

namespace vrml {
namespace gui {

// Starts `offer` (kind Run): ShellExecuteEx, "runas" when the manifest says the uninstaller needs administrator rights (the uninstaller of a program in Program Files asks
// Windows itself otherwise). On success a worker waits for the process to end and then posts `message` (wParam 0, lParam 0) to `notify`; if there is no process handle to
// wait on, nothing is posted. false when it could not be started: *cancelled says the player declined the Windows prompt.
// *finished (when given) answers "has the uninstaller's process ended?" from any window, also after `notify` is gone: Model::BeginUninstall takes it, so
// Play, Install and a second Uninstall wait for it. Without a process handle to wait on it answers true at once.
bool StartUninstaller(HWND owner, const Model::UninstallOffer& offer, HWND notify, UINT message, bool* cancelled, std::function<bool()>* finished = nullptr);

}  // namespace gui
}  // namespace vrml
