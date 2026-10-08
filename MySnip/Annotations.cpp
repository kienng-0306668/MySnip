#include "Annotations.h"
#include <gdiplus.h>
#include <cmath>

namespace gp = Gdiplus;

static int MinI(int a, int b) { return a < b ? a : b; }
static int MaxI(int a, int b) { return a > b ? a : b; }
static int AbsI(int a) { return a < 0 ? -a : a; }

static RECT NormRect(POINT a, POINT b)
{
    RECT r;
    r.left = MinI(a.x, b.x);   r.top = MinI(a.y, b.y);
    r.right = MaxI(a.x, b.x);  r.bottom = MaxI(a.y, b.y);
    return r;
}

static RECT UnionR(const RECT& a, const RECT& b)
{
    RECT r;
    r.left = MinI(a.left, b.left);     r.top = MinI(a.top, b.top);
    r.right = MaxI(a.right, b.right);  r.bottom = MaxI(a.bottom, b.bottom);
    return r;
}

static int PadOf(const Annotation& it) { return it.width * 5 + 16; }

static gp::Color ToGp(COLORREF c, BYTE a = 255)
{
    return gp::Color(a, GetRValue(c), GetGValue(c), GetBValue(c));
}

static RECT BoundsOf(const Annotation& it)
{
    switch (it.tool) {
    case TOOL_PEN:
    case TOOL_MARKER: {
        if (it.pts.empty()) return RECT{};
        RECT r = { it.pts[0].x, it.pts[0].y, it.pts[0].x, it.pts[0].y };
        for (const POINT& p : it.pts) {
            r.left = MinI(r.left, p.x);   r.top = MinI(r.top, p.y);
            r.right = MaxI(r.right, p.x); r.bottom = MaxI(r.bottom, p.y);
        }
        return r;
    }
    case TOOL_TEXT:
        return RECT{ it.a.x, it.a.y, it.a.x + it.textSize.cx, it.a.y + it.textSize.cy };
    default:
        return NormRect(it.a, it.b);
    }
}

HFONT Annotations::MakeFont(int px)
{
    return CreateFontW(-px, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        ANTIALIASED_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
}

static SIZE MeasureText(const std::wstring& s, int px)
{
    HDC dc = GetDC(nullptr);
    HFONT f = Annotations::MakeFont(px);
    HGDIOBJ old = SelectObject(dc, f);
    SIZE sz = {};
    GetTextExtentPoint32W(dc, s.c_str(), static_cast<int>(s.size()), &sz);
    SelectObject(dc, old);
    DeleteObject(f);
    ReleaseDC(nullptr, dc);
    return sz;
}

// ===================== Kiểm tra trúng (cho công cụ tẩy) =====================
static double DistToSegment(POINT p, POINT a, POINT b)
{
    double dx = b.x - a.x, dy = b.y - a.y;
    double len2 = dx * dx + dy * dy;
    double t = (len2 == 0.0) ? 0.0 : ((p.x - a.x) * dx + (p.y - a.y) * dy) / len2;
    if (t < 0.0) t = 0.0;
    if (t > 1.0) t = 1.0;
    double cx = a.x + t * dx, cy = a.y + t * dy;
    return std::sqrt((p.x - cx) * (p.x - cx) + (p.y - cy) * (p.y - cy));
}

static bool NearPolyline(const POINT* pts, size_t n, POINT p, double tol)
{
    if (n == 0) return false;
    if (n == 1) {
        double dx = p.x - pts[0].x, dy = p.y - pts[0].y;
        return std::sqrt(dx * dx + dy * dy) <= tol;
    }
    for (size_t i = 0; i + 1 < n; ++i)
        if (DistToSegment(p, pts[i], pts[i + 1]) <= tol) return true;
    return false;
}

static bool HitAnnotation(const Annotation& it, POINT p)
{
    switch (it.tool) {
    case TOOL_RECT: {
        RECT r = NormRect(it.a, it.b);
        double tol = it.width / 2.0 + 6.0;
        if (!it.ellipse) {
            POINT c[5] = { { r.left, r.top },{ r.right, r.top },{ r.right, r.bottom },
                           { r.left, r.bottom },{ r.left, r.top } };
            return NearPolyline(c, 5, p, tol);
        }
        POINT e[49];
        double cx = (r.left + r.right) / 2.0, cy = (r.top + r.bottom) / 2.0;
        double rx = (r.right - r.left) / 2.0, ry = (r.bottom - r.top) / 2.0;
        for (int i = 0; i <= 48; ++i) {
            double t = i * 6.283185307179586 / 48.0;
            e[i].x = static_cast<LONG>(cx + rx * std::cos(t));
            e[i].y = static_cast<LONG>(cy + ry * std::sin(t));
        }
        return NearPolyline(e, 49, p, tol);
    }
    case TOOL_ARROW:
        return DistToSegment(p, it.a, it.b) <= it.width / 2.0 + 8.0;
    case TOOL_PEN:
        return NearPolyline(it.pts.data(), it.pts.size(), p, it.width / 2.0 + 6.0);
    case TOOL_MARKER:
        return NearPolyline(it.pts.data(), it.pts.size(), p, (it.width * 5 + 5) / 2.0 + 3.0);
    case TOOL_MOSAIC:
    case TOOL_TEXT: {
        RECT r = BoundsOf(it);
        InflateRect(&r, 3, 3);
        return PtInRect(&r, p) != 0;
    }
    default:
        return false;
    }
}

// ===================== Vẽ từng loại =====================
static void PaintMosaic(HDC dst, HDC base, const RECT& r, int offX, int offY, int cell)
{
    int w = r.right - r.left, h = r.bottom - r.top;
    if (w <= 0 || h <= 0) return;
    int sw = MaxI(1, w / cell), sh = MaxI(1, h / cell);

    HDC screen = GetDC(nullptr);
    HDC tmp = CreateCompatibleDC(screen);
    HBITMAP bmp = CreateCompatibleBitmap(screen, sw, sh);
    ReleaseDC(nullptr, screen);
    HGDIOBJ old = SelectObject(tmp, bmp);

    // Thu nhỏ (lấy màu trung bình) rồi phóng lại bằng COLORONCOLOR: ra các khối vuông
    SetStretchBltMode(tmp, HALFTONE);
    SetBrushOrgEx(tmp, 0, 0, nullptr);
    StretchBlt(tmp, 0, 0, sw, sh, base, r.left, r.top, w, h, SRCCOPY);

    int oldMode = SetStretchBltMode(dst, COLORONCOLOR);
    StretchBlt(dst, r.left - offX, r.top - offY, w, h, tmp, 0, 0, sw, sh, SRCCOPY);
    SetStretchBltMode(dst, oldMode);

    SelectObject(tmp, old);
    DeleteObject(bmp);
    DeleteDC(tmp);
}

static void PaintText(HDC dst, const Annotation& it, int offX, int offY)
{
    HFONT f = Annotations::MakeFont(it.fontPx);
    HGDIOBJ old = SelectObject(dst, f);
    int oldMode = SetBkMode(dst, TRANSPARENT);
    COLORREF oldColor = SetTextColor(dst, it.color);
    TextOutW(dst, it.a.x - offX, it.a.y - offY, it.text.c_str(),
        static_cast<int>(it.text.size()));
    SetTextColor(dst, oldColor);
    SetBkMode(dst, oldMode);
    SelectObject(dst, old);
    DeleteObject(f);
}

static void PaintOne(HDC dst, HDC base, int offX, int offY, const RECT& clip, const Annotation& it)
{
    if (it.tool == TOOL_MOSAIC) {
        RECT r = NormRect(it.a, it.b);
        PaintMosaic(dst, base, r, offX, offY, MaxI(8, it.width * 3));
        return;
    }
    if (it.tool == TOOL_TEXT) {
        PaintText(dst, it, offX, offY);
        return;
    }

    gp::Graphics g(dst);
    g.SetSmoothingMode(gp::SmoothingModeAntiAlias);
    g.SetClip(gp::Rect(clip.left, clip.top, clip.right - clip.left, clip.bottom - clip.top));
    g.TranslateTransform(static_cast<gp::REAL>(-offX), static_cast<gp::REAL>(-offY));

    const gp::REAL w = static_cast<gp::REAL>(it.width);

    switch (it.tool) {
    case TOOL_RECT: {
        RECT r = NormRect(it.a, it.b);
        gp::Pen pen(ToGp(it.color), w);
        pen.SetLineJoin(gp::LineJoinRound);
        gp::REAL x = static_cast<gp::REAL>(r.left);
        gp::REAL y = static_cast<gp::REAL>(r.top);
        gp::REAL rw = static_cast<gp::REAL>(r.right - r.left);
        gp::REAL rh = static_cast<gp::REAL>(r.bottom - r.top);
        if (it.ellipse) g.DrawEllipse(&pen, x, y, rw, rh);
        else            g.DrawRectangle(&pen, x, y, rw, rh);
        break;
    }
    case TOOL_ARROW: {
        double dx = it.b.x - it.a.x, dy = it.b.y - it.a.y;
        double len = std::sqrt(dx * dx + dy * dy);
        if (len < 2.0) break;
        double ux = dx / len, uy = dy / len;
        double hl = it.width * 4.0 + 8.0;           // chiều dài đầu mũi tên
        if (hl > len) hl = len;
        double hw = hl * 0.45;                      // nửa bề rộng đầu mũi tên

        double bx = it.b.x - ux * hl, by = it.b.y - uy * hl;   // chân đầu mũi tên
        gp::PointF head[3] = {
            gp::PointF(static_cast<gp::REAL>(it.b.x), static_cast<gp::REAL>(it.b.y)),
            gp::PointF(static_cast<gp::REAL>(bx - uy * hw), static_cast<gp::REAL>(by + ux * hw)),
            gp::PointF(static_cast<gp::REAL>(bx + uy * hw), static_cast<gp::REAL>(by - ux * hw))
        };
        gp::PointF start(static_cast<gp::REAL>(it.a.x), static_cast<gp::REAL>(it.a.y));
        gp::PointF lineEnd(static_cast<gp::REAL>(it.b.x - ux * hl * 0.7),
            static_cast<gp::REAL>(it.b.y - uy * hl * 0.7));

        gp::Pen pen(ToGp(it.color), w);
        pen.SetStartCap(gp::LineCapRound);
        g.DrawLine(&pen, start, lineEnd);
        gp::SolidBrush br(ToGp(it.color));
        g.FillPolygon(&br, head, 3);
        break;
    }
    case TOOL_PEN: {
        if (it.pts.empty()) break;
        if (it.pts.size() < 2) {                    // chỉ chấm một điểm
            gp::SolidBrush br(ToGp(it.color));
            g.FillEllipse(&br, static_cast<gp::REAL>(it.pts[0].x) - w / 2,
                static_cast<gp::REAL>(it.pts[0].y) - w / 2, w, w);
            break;
        }
        gp::Pen pen(ToGp(it.color), w);
        pen.SetLineCap(gp::LineCapRound, gp::LineCapRound, gp::DashCapRound);
        pen.SetLineJoin(gp::LineJoinRound);
        std::vector<gp::PointF> v;
        v.reserve(it.pts.size());
        for (const POINT& p : it.pts)
            v.push_back(gp::PointF(static_cast<gp::REAL>(p.x), static_cast<gp::REAL>(p.y)));
        g.DrawLines(&pen, v.data(), static_cast<INT>(v.size()));
        break;
    }
    case TOOL_MARKER: {
        if (it.pts.size() < 2) break;
        gp::Pen pen(ToGp(it.color, 110), static_cast<gp::REAL>(it.width * 5 + 5));
        pen.SetLineJoin(gp::LineJoinRound);
        std::vector<gp::PointF> v;
        v.reserve(it.pts.size());
        for (const POINT& p : it.pts)
            v.push_back(gp::PointF(static_cast<gp::REAL>(p.x), static_cast<gp::REAL>(p.y)));
        g.DrawLines(&pen, v.data(), static_cast<INT>(v.size()));
        break;
    }
    default:
        break;
    }
}

// ===================== Annotations =====================
void Annotations::SetWidth(int w) { width_ = w < 1 ? 1 : (w > 20 ? 20 : w); }
int  Annotations::FontPx() const { return 10 + width_ * 3; }

void Annotations::Commit(const Ptr& p)
{
    PushUndo();
    items_.push_back(p);
    redo_.clear();
}

RECT Annotations::BeginStroke(POINT pt)
{
    live_ = Annotation();
    live_.tool = tool_;
    live_.ellipse = ellipse_;
    live_.color = color_;
    live_.width = width_;
    live_.a = pt;
    live_.b = pt;
    if (tool_ == TOOL_PEN || tool_ == TOOL_MARKER) live_.pts.push_back(pt);
    drawing_ = true;

    RECT d = { pt.x, pt.y, pt.x, pt.y };
    int pad = PadOf(live_);
    InflateRect(&d, pad, pad);
    return d;
}

RECT Annotations::UpdateStroke(POINT pt)
{
    if (!drawing_) return RECT{};
    int pad = PadOf(live_);
    RECT dirty;

    if (live_.tool == TOOL_PEN || live_.tool == TOOL_MARKER) {
        POINT prev = live_.pts.back();
        if (prev.x == pt.x && prev.y == pt.y) return RECT{};
        live_.pts.push_back(pt);
        dirty = NormRect(prev, pt);
    }
    else {
        RECT oldB = NormRect(live_.a, live_.b);
        live_.b = pt;
        RECT newB = NormRect(live_.a, live_.b);
        dirty = UnionR(oldB, newB);
    }
    InflateRect(&dirty, pad, pad);
    return dirty;
}

void Annotations::EndStroke()
{
    if (!drawing_) return;
    drawing_ = false;

    bool keep = true;
    switch (live_.tool) {
    case TOOL_RECT:
    case TOOL_ARROW:
    case TOOL_MOSAIC:
        if (AbsI(live_.a.x - live_.b.x) < 3 && AbsI(live_.a.y - live_.b.y) < 3) keep = false;
        break;
    case TOOL_MARKER:
        if (live_.pts.size() < 2) keep = false;
        break;
    default:
        break;
    }
    if (keep) Commit(std::make_shared<Annotation>(live_));
}

bool Annotations::EraseAt(POINT pt)
{
    for (int i = static_cast<int>(items_.size()) - 1; i >= 0; --i) {
        if (!HitAnnotation(*items_[i], pt)) continue;

        if (!erasing_) {            // cả lần kéo tẩy chỉ tính là một bước undo
            PushUndo();
            redo_.clear();
            erasing_ = true;
        }
        items_.erase(items_.begin() + i);
        return true;
    }
    return false;
}

void Annotations::AddText(POINT pos, const std::wstring& text)
{
    if (text.empty()) return;
    auto a = std::make_shared<Annotation>();
    a->tool = TOOL_TEXT;
    a->a = pos;
    a->color = color_;
    a->width = width_;
    a->text = text;
    a->fontPx = FontPx();
    a->textSize = MeasureText(text, a->fontPx);
    Commit(a);
}

void Annotations::Undo()
{
    if (undo_.empty()) return;
    redo_.push_back(items_);
    items_ = undo_.back();
    undo_.pop_back();
}

void Annotations::Redo()
{
    if (redo_.empty()) return;
    undo_.push_back(items_);
    items_ = redo_.back();
    redo_.pop_back();
}

void Annotations::Reset()
{
    items_.clear();
    undo_.clear();
    redo_.clear();
    drawing_ = false;
    erasing_ = false;
    tool_ = TOOL_NONE;
    ellipse_ = false;
}

void Annotations::Render(HDC dst, HDC base, int offX, int offY, const RECT& clip) const
{
    int saved = SaveDC(dst);
    IntersectClipRect(dst, clip.left, clip.top, clip.right, clip.bottom);

    for (const Ptr& p : items_)
        PaintOne(dst, base, offX, offY, clip, *p);
    if (drawing_)
        PaintOne(dst, base, offX, offY, clip, live_);

    RestoreDC(dst, saved);
}