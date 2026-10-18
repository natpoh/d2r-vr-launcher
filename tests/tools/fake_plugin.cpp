// fake_plugin.dll - a stand-in for a mod's plugin (d2rl-pcvr.dll, d2rl-vrcam.dll). It does nothing: only its version resource matters. CMakeLists.txt builds several of
// them from this file and fake_plugin.rc.in, each with its own ProductVersion and Comments ("mod-id=d2r-pcvr; ..."), for tests/mods_test.cpp, tests/releases_test.cpp,
// tests/gui_model_test.cpp and tools/install_view_smoke.py.
extern "C" __declspec(dllexport) int fake_plugin_version_only() { return 0; }
