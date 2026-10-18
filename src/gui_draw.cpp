#include "gui_draw.h"

#include <algorithm>
#include <vector>

#include <shlwapi.h>

// gdiplus.h uses min and max: take them from the standard library instead of the windows.h macros
using std::max;
using std::min;
#include <gdiplus.h>

namespace vrml {
namespace gui {

namespace {

Gdiplus::Color Rgb(COLORREF c, BYTE a = 255) { return Gdiplus::Color(a, GetRValue(c), GetGValue(c), GetBValue(c)); }

// A rounded rectangle as a path; `radius` is the corner radius in pixels.
void RoundRectPath(Gdiplus::GraphicsPath* path, float x, float y, float w, float h, float radius) {
    const float d = std::min(radius * 2, std::min(w, h));
    path->AddArc(x, y, d, d, 180, 90);
    path->AddArc(x + w - d, y, d, d, 270, 90);
    path->AddArc(x + w - d, y + h - d, d, d, 0, 90);
    path->AddArc(x, y + h - d, d, d, 90, 90);
    path->CloseFigure();
}

// A decoded picture the caller owns. Decoding from a stream leaves the bitmap tied to the stream, so it is copied into memory first.
Gdiplus::Bitmap* Detach(Gdiplus::Bitmap* src) {
    if (!src) return nullptr;
    if (src->GetLastStatus() != Gdiplus::Ok || src->GetWidth() == 0 || src->GetHeight() == 0) {
        delete src;
        return nullptr;
    }
    Gdiplus::Bitmap* copy = src->Clone(0, 0, static_cast<INT>(src->GetWidth()), static_cast<INT>(src->GetHeight()), PixelFormat32bppPARGB);
    delete src;
    if (copy && copy->GetLastStatus() != Gdiplus::Ok) {
        delete copy;
        return nullptr;
    }
    return copy;
}

bool EncoderFor(const wchar_t* mime, CLSID* clsid) {
    UINT count = 0, size = 0;
    if (Gdiplus::GetImageEncodersSize(&count, &size) != Gdiplus::Ok || size == 0) return false;
    std::vector<BYTE> buf(size);
    auto* info = reinterpret_cast<Gdiplus::ImageCodecInfo*>(buf.data());
    if (Gdiplus::GetImageEncoders(count, size, info) != Gdiplus::Ok) return false;
    for (UINT i = 0; i < count; ++i)
        if (wcscmp(info[i].MimeType, mime) == 0) {
            *clsid = info[i].Clsid;
            return true;
        }
    return false;
}

}  // namespace

GdiplusSession::GdiplusSession() {
    Gdiplus::GdiplusStartupInput in;
    Gdiplus::GdiplusStartup(&token_, &in, nullptr);
}
GdiplusSession::~GdiplusSession() {
    if (token_) Gdiplus::GdiplusShutdown(token_);
}

Gdiplus::Bitmap* LoadResourcePng(int resourceId) {
    HRSRC res = FindResourceW(nullptr, MAKEINTRESOURCEW(resourceId), L"PNG");
    if (!res) return nullptr;
    HGLOBAL mem = LoadResource(nullptr, res);
    const void* data = mem ? LockResource(mem) : nullptr;
    const DWORD size = SizeofResource(nullptr, res);
    if (!data || !size) return nullptr;
    IStream* stream = SHCreateMemStream(static_cast<const BYTE*>(data), size);
    if (!stream) return nullptr;
    Gdiplus::Bitmap* bmp = Detach(Gdiplus::Bitmap::FromStream(stream));
    stream->Release();
    return bmp;
}

Gdiplus::Bitmap* LoadPictureFile(const fs::path& file) {
    // Unreadable or huge files are not worth a card icon: a mod's picture is a few KB.
    std::error_code ec;
    const auto size = fs::file_size(file, ec);
    if (ec || size == 0 || size > 8u * 1024 * 1024) return nullptr;
    return Detach(Gdiplus::Bitmap::FromFile(file.wstring().c_str()));
}

void DrawPicture(HDC dc, Gdiplus::Bitmap* bmp, const RECT& dst) {
    if (!bmp) return;
    const float bw = static_cast<float>(bmp->GetWidth()), bh = static_cast<float>(bmp->GetHeight());
    const float dw = static_cast<float>(dst.right - dst.left), dh = static_cast<float>(dst.bottom - dst.top);
    if (dw <= 0 || dh <= 0) return;
    const float scale = std::min(dw / bw, dh / bh);
    const float w = bw * scale, h = bh * scale;
    const float x = static_cast<float>(dst.left) + (dw - w) / 2, y = static_cast<float>(dst.top) + (dh - h) / 2;
    Gdiplus::Graphics g(dc);
    g.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
    g.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);
    g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    Gdiplus::ImageAttributes attr;
    attr.SetWrapMode(Gdiplus::WrapModeTileFlipXY);       // no half-transparent fringe at the edges when scaling down
    g.DrawImage(bmp, Gdiplus::RectF(x, y, w, h), 0, 0, bw, bh, Gdiplus::UnitPixel, &attr);
}

void DrawPlaceholder(HDC dc, const RECT& dst, wchar_t letter) {
    const float w = static_cast<float>(dst.right - dst.left), h = static_cast<float>(dst.bottom - dst.top);
    if (w <= 0 || h <= 0) return;
    Gdiplus::Graphics g(dc);
    g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    g.SetTextRenderingHint(Gdiplus::TextRenderingHintAntiAliasGridFit);
    Gdiplus::GraphicsPath path;
    RoundRectPath(&path, static_cast<float>(dst.left), static_cast<float>(dst.top), w, h, w * 0.22f);
    Gdiplus::SolidBrush fill(Rgb(RGB(0x6B, 0x77, 0x8A)));      // a neutral slate: no mod's colours
    g.FillPath(&fill, &path);
    Gdiplus::FontFamily fam(L"Segoe UI");
    Gdiplus::FontFamily fallback(L"Arial");
    Gdiplus::Font font(fam.GetLastStatus() == Gdiplus::Ok ? &fam : &fallback, std::min(w, h) * 0.52f, Gdiplus::FontStyleBold, Gdiplus::UnitPixel);
    Gdiplus::StringFormat fmt;
    fmt.SetAlignment(Gdiplus::StringAlignmentCenter);
    fmt.SetLineAlignment(Gdiplus::StringAlignmentCenter);
    Gdiplus::SolidBrush ink(Gdiplus::Color(255, 255, 255, 255));
    wchar_t text[2] = {letter, 0};
    // a glyph sits a little high in its line box: nudge it so the letter looks centred
    g.DrawString(text, 1, &font, Gdiplus::RectF(static_cast<float>(dst.left), static_cast<float>(dst.top) + h * 0.03f, w, h), &fmt, &ink);
}

void DrawRoundRect(HDC dc, const RECT& r, int radius, COLORREF fill, COLORREF border, int borderWidth) {
    Gdiplus::Graphics g(dc);
    g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    const float half = static_cast<float>(borderWidth) / 2.0f;
    Gdiplus::GraphicsPath path;
    RoundRectPath(&path, static_cast<float>(r.left) + half, static_cast<float>(r.top) + half, static_cast<float>(r.right - r.left) - borderWidth,
                  static_cast<float>(r.bottom - r.top) - borderWidth, static_cast<float>(radius));
    Gdiplus::SolidBrush b(Rgb(fill));
    g.FillPath(&b, &path);
    if (borderWidth > 0) {
        Gdiplus::Pen pen(Rgb(border), static_cast<float>(borderWidth));
        g.DrawPath(&pen, &path);
    }
}

void DrawDot(HDC dc, const RECT& r, COLORREF color) {
    Gdiplus::Graphics g(dc);
    g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    Gdiplus::SolidBrush b(Rgb(color));
    g.FillEllipse(&b, static_cast<float>(r.left), static_cast<float>(r.top), static_cast<float>(r.right - r.left), static_cast<float>(r.bottom - r.top));
}

bool SaveClientPng(HWND hwnd, const std::wstring& file, std::string* error) {
    auto fail = [&](const char* why) {
        if (error) *error = why;
        return false;
    };
    RECT rc{};
    GetClientRect(hwnd, &rc);
    const int w = rc.right - rc.left, h = rc.bottom - rc.top;
    if (w <= 0 || h <= 0) return fail("the window has no client area");
    HDC screen = GetDC(nullptr);
    HDC mem = CreateCompatibleDC(screen);
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;                    // top-down
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HBITMAP dib = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    ReleaseDC(nullptr, screen);
    if (!dib || !bits) {
        DeleteDC(mem);
        return fail("could not create the picture buffer");
    }
    HGDIOBJ old = SelectObject(mem, dib);
    // PW_CLIENTONLY: the window paints itself and its children into our bitmap (WM_PRINT). Not PW_RENDERFULLCONTENT: that one copies what the
    // compositor has shown so far, and owner-drawn children were sometimes missing from it.
    const BOOL printed = PrintWindow(hwnd, mem, 0x1);
    bool ok = false;
    if (!printed) {
        if (error) *error = "PrintWindow failed";
    } else {
        // 32bppRGB ignores the alpha byte (PrintWindow leaves it 0): the PNG is opaque
        Gdiplus::Bitmap bmp(w, h, w * 4, PixelFormat32bppRGB, static_cast<BYTE*>(bits));
        CLSID png;
        if (!EncoderFor(L"image/png", &png)) {
            if (error) *error = "no PNG encoder";
        } else if (bmp.Save(file.c_str(), &png, nullptr) != Gdiplus::Ok) {
            if (error) *error = "could not write the file";
        } else {
            ok = true;
        }
    }
    SelectObject(mem, old);
    DeleteObject(dib);
    DeleteDC(mem);
    return ok;
}

}  // namespace gui
}  // namespace vrml
