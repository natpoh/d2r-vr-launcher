// gui_draw.h - the pictures of the launcher window: PNGs from the exe's resources, a placeholder for a mod without art, rounded cards, and
// the screenshot (`--screenshot`). GDI+ only (part of Windows), no other library.
#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <string>

#include "util.h"

namespace Gdiplus {
class Bitmap;
}

namespace vrml {
namespace gui {

// GDI+ is started once for the whole process.
class GdiplusSession {
public:
    GdiplusSession();
    ~GdiplusSession();
    GdiplusSession(const GdiplusSession&) = delete;
    GdiplusSession& operator=(const GdiplusSession&) = delete;

private:
    ULONG_PTR token_ = 0;
};

// A picture stored in the exe as a "PNG" resource, or a PNG / ICO file. Premultiplied ARGB; the caller deletes it. nullptr when it is
// missing or not a picture (a mod's launcher_icon.png may be anything).
Gdiplus::Bitmap* LoadResourcePng(int resourceId);
Gdiplus::Bitmap* LoadPictureFile(const fs::path& file);

// Draws the picture inside `dst`, whole, centred, keeping its proportions.
void DrawPicture(HDC dc, Gdiplus::Bitmap* bmp, const RECT& dst);
// A neutral rounded square with one letter: what a mod without art of its own gets. Nothing of any brand.
void DrawPlaceholder(HDC dc, const RECT& dst, wchar_t letter);
// An anti-aliased rounded rectangle; the border is drawn inside `r`.
void DrawRoundRect(HDC dc, const RECT& r, int radius, COLORREF fill, COLORREF border, int borderWidth);
// A filled circle (the dot before "Installed").
void DrawDot(HDC dc, const RECT& r, COLORREF color);

// The client area of `hwnd` as a PNG file (PrintWindow, so it is what the window shows). False with *error when it failed.
bool SaveClientPng(HWND hwnd, const std::wstring& file, std::string* error);

}  // namespace gui
}  // namespace vrml
