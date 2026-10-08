#include "Toolbar.h"
#include <gdiplus.h>

namespace gp = Gdiplus;

static int ClampInt(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }
static int MaxInt(int a, int b) { return a > b ? a : b; }

static gp::Color ToGp(COLORREF c, BYTE a = 255)
{
    return gp::Color(a, GetRValue(c), GetGValue(c), GetBValue(c));
}

// Vẽ một icon trong ô 20x20, tâm tại (cx, cy)
static void PaintIconGlyph(gp::Graphics& g, ToolbarIcon icon, float cx, float cy,
    COLORREF fgRef, COLORREF bgRef)
{
    const float ox = cx - 10.0f, oy = cy - 10.0f;

    gp::Pen pen(ToGp(fgRef), 1.6f);
    pen.SetLineJoin(gp::LineJoinRound);
    pen.SetStartCap(gp::LineCapRound);
    pen.SetEndCap(gp::LineCapRound);
    gp::SolidBrush fgBrush(ToGp(fgRef));
    gp::SolidBrush bgBrush(ToGp(bgRef));

    auto P = [&](float x, float y) { return gp::PointF(ox + x, oy + y); };

    switch (icon) {
    case IC_RECT:
        g.DrawRectangle(&pen, ox + 3.0f, oy + 4.5f, 14.0f, 11.0f);
        break;
    case IC_ELLIPSE:
        g.DrawEllipse(&pen, ox + 3.0f, oy + 4.5f, 14.0f, 11.0f);
        break;
    case IC_ARROW: {
        g.DrawLine(&pen, P(4, 16), P(16, 4));
        gp::PointF head[3] = { P(9.5f, 4), P(16, 4), P(16, 10.5f) };
        g.DrawLines(&pen, head, 3);
        break;
    }
    case IC_PEN: {
        gp::PointF body[5] = { P(3.5f, 16.5f), P(4.5f, 12), P(13, 3.5f), P(16.5f, 7), P(8, 15.5f) };
        g.DrawPolygon(&pen, body, 5);
        g.DrawLine(&pen, P(11, 5.5f), P(14.5f, 9));
        break;
    }
    case IC_MARKER: {
        gp::PointF body[4] = { P(8, 3), P(16.5f, 11.5f), P(11.5f, 16.5f), P(3, 8) };
        g.DrawPolygon(&pen, body, 4);
        g.DrawLine(&pen, P(3.5f, 18), P(9.5f, 18));
        break;
    }
    case IC_MOSAIC:
        g.FillRectangle(&fgBrush, ox + 3.0f, oy + 3.0f, 7.0f, 7.0f);
        g.FillRectangle(&fgBrush, ox + 10.0f, oy + 10.0f, 7.0f, 7.0f);
        g.DrawRectangle(&pen, ox + 3.0f, oy + 3.0f, 14.0f, 14.0f);
        break;
    case IC_TEXT:
        g.DrawLine(&pen, P(4, 5), P(16, 5));
        g.DrawLine(&pen, P(10, 5), P(10, 16));
        g.DrawLine(&pen, P(7.5f, 16), P(12.5f, 16));
        break;
    case IC_ERASER: {
        gp::PointF body[4] = { P(4, 12), P(11, 4), P(17, 9.5f), P(10, 17.5f) };
        g.DrawPolygon(&pen, body, 4);
        g.DrawLine(&pen, P(7.5f, 8), P(13.5f, 13.5f));
        break;
    }
    case IC_UNDO: {
        g.DrawBezier(&pen, P(6, 8), P(9, 4), P(17, 4), P(17, 12));
        gp::PointF head[3] = { P(6, 3.5f), P(6, 8), P(10.5f, 8) };
        g.DrawLines(&pen, head, 3);
        break;
    }
    case IC_REDO: {
        g.DrawBezier(&pen, P(14, 8), P(11, 4), P(3, 4), P(3, 12));
        gp::PointF head[3] = { P(14, 3.5f), P(14, 8), P(9.5f, 8) };
        g.DrawLines(&pen, head, 3);
        break;
    }
    case IC_CLOSE:
        g.DrawLine(&pen, P(5, 5), P(15, 15));
        g.DrawLine(&pen, P(15, 5), P(5, 15));
        break;
    case IC_PIN: {
        gp::PointF body[6] = { P(7, 3), P(13, 3), P(12.5f, 9), P(15.5f, 12), P(4.5f, 12), P(7.5f, 9) };
        g.DrawPolygon(&pen, body, 6);
        g.DrawLine(&pen, P(10, 12), P(10, 18));
        break;
    }
    case IC_SAVE:
        g.DrawRectangle(&pen, ox + 3.0f, oy + 3.0f, 14.0f, 14.0f);
        g.DrawRectangle(&pen, ox + 6.0f, oy + 3.0f, 8.0f, 5.0f);
        g.DrawRectangle(&pen, ox + 6.0f, oy + 11.0f, 8.0f, 6.0f);
        break;
    case IC_COPY:
        g.DrawRectangle(&pen, ox + 7.0f, oy + 3.0f, 9.0f, 10.0f);
        g.FillRectangle(&bgBrush, ox + 4.0f, oy + 7.0f, 9.0f, 10.0f);   // che phần giao nhau
        g.DrawRectangle(&pen, ox + 4.0f, oy + 7.0f, 9.0f, 10.0f);
        break;
    default:
        break;
    }
}

// ===================== Cấu hình =====================
void Toolbar::AddButton(int id, const wchar_t* label, int width)
{
    Item it;
    it.id = id;
    it.label = label;
    it.width = width > 0 ? width : theme_.buttonW;
    items_.push_back(it);
}

void Toolbar::AddIconButton(int id, ToolbarIcon icon, int width)
{
    Item it;
    it.id = id;
    it.icon = icon;
    it.width = width > 0 ? width : theme_.buttonW;
    items_.push_back(it);
}

void Toolbar::AddSeparator()
{
    Item it;
    it.separator = true;
    it.width = theme_.separatorW;
    items_.push_back(it);
}

void Toolbar::Clear()
{
    items_.clear();
    hoverId_ = 0;
}

Toolbar::Item* Toolbar::Find(int id)
{
    for (auto& it : items_)
        if (!it.separator && it.id == id) return &it;
    return nullptr;
}

void Toolbar::SetEnabled(int id, bool enabled) { if (Item* it = Find(id)) it->enabled = enabled; }
void Toolbar::SetChecked(int id, bool checked) { if (Item* it = Find(id)) it->checked = checked; }
void Toolbar::SetIcon(int id, ToolbarIcon icon) { if (Item* it = Find(id)) it->icon = icon; }

void Toolbar::SetBadge(int id, bool show, COLORREF color)
{
    if (Item* it = Find(id)) {
        it->badge = show;
        it->badgeColor = color;
    }
}

// ===================== Bố cục =====================
void Toolbar::Layout(const RECT& anchor, int boundsW, int boundsH)
{
    int total = theme_.padding * 2;
    for (size_t i = 0; i < items_.size(); ++i) {
        total += items_[i].width;
        if (i > 0) total += theme_.gap;
    }
    int h = theme_.padding * 2 + theme_.buttonH;

    // Canh phải theo vùng chọn, ưu tiên nằm dưới
    int left = ClampInt(anchor.right - total, 0, MaxInt(0, boundsW - total));
    int top = anchor.bottom + theme_.margin;
    if (top + h > boundsH) {
        top = anchor.top - h - theme_.margin;                             // hết chỗ: nhảy lên trên
        if (top < 0) top = MaxInt(0, anchor.bottom - h - theme_.margin);  // vẫn không đủ: vào trong
    }
    bounds_ = { left, top, left + total, top + h };

    int x = left + theme_.padding;
    int y = top + theme_.padding;
    for (auto& it : items_) {
        it.rect = { x, y, x + it.width, y + theme_.buttonH };
        x += it.width + theme_.gap;
    }
}

bool Toolbar::Contains(POINT pt) const
{
    return visible_ && PtInRect(&bounds_, pt);
}

int Toolbar::HitTest(POINT pt) const
{
    if (!visible_) return 0;
    for (const auto& it : items_) {
        if (it.separator || !it.enabled) continue;
        if (PtInRect(&it.rect, pt)) return it.id;
    }
    return 0;
}

bool Toolbar::SetHover(POINT pt)
{
    int id = HitTest(pt);
    if (id == hoverId_) return false;
    hoverId_ = id;
    return true;
}

// ===================== Vẽ =====================
void Toolbar::Paint(HDC hdc) const
{
    if (!visible_) return;

    // Nền và viền
    HBRUSH bg = CreateSolidBrush(theme_.background);
    FillRect(hdc, &bounds_, bg);
    DeleteObject(bg);
    HBRUSH bd = CreateSolidBrush(theme_.border);
    FrameRect(hdc, &bounds_, bd);
    DeleteObject(bd);

    auto colorsFor = [&](const Item& it, COLORREF& fill, COLORREF& fg) {
        fill = theme_.button;
        fg = theme_.text;
        if (!it.enabled) { fill = theme_.buttonDisabled; fg = theme_.textDisabled; }
        else if (it.checked) { fill = theme_.buttonChecked; }
        else if (it.id == hoverId_) { fill = theme_.buttonHover; }
        };

    // Lượt 1 (GDI): nền nút, chữ, vạch phân cách
    HGDIOBJ  oldFont = font_ ? SelectObject(hdc, font_) : nullptr;
    int      oldMode = SetBkMode(hdc, TRANSPARENT);
    COLORREF oldText = GetTextColor(hdc);

    for (const auto& it : items_) {
        if (it.separator) {
            int x = (it.rect.left + it.rect.right) / 2;
            RECT r = { x, it.rect.top + 6, x + 1, it.rect.bottom - 6 };
            HBRUSH b = CreateSolidBrush(theme_.separator);
            FillRect(hdc, &r, b);
            DeleteObject(b);
            continue;
        }

        COLORREF fill, fg;
        colorsFor(it, fill, fg);

        HBRUSH b = CreateSolidBrush(fill);
        FillRect(hdc, &it.rect, b);
        DeleteObject(b);

        if (it.icon == IC_NONE) {
            SetTextColor(hdc, fg);
            RECT tr = it.rect;
            DrawTextW(hdc, it.label.c_str(), -1, &tr, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        }
    }

    SetTextColor(hdc, oldText);
    SetBkMode(hdc, oldMode);
    if (oldFont) SelectObject(hdc, oldFont);

    // Lượt 2 (GDI+): icon và chấm màu
    gp::Graphics g(hdc);
    g.SetSmoothingMode(gp::SmoothingModeAntiAlias);

    for (const auto& it : items_) {
        if (it.separator || it.icon == IC_NONE) continue;

        COLORREF fill, fg;
        colorsFor(it, fill, fg);

        float cx = (it.rect.left + it.rect.right) / 2.0f;
        float cy = (it.rect.top + it.rect.bottom) / 2.0f;
        PaintIconGlyph(g, it.icon, cx, cy, fg, fill);

        if (it.badge) {
            float bx = static_cast<float>(it.rect.right) - 8.0f;
            float by = static_cast<float>(it.rect.top) + 8.0f;
            gp::SolidBrush bb(ToGp(it.badgeColor));
            g.FillEllipse(&bb, bx - 3.5f, by - 3.5f, 7.0f, 7.0f);
            gp::Pen ring(gp::Color(255, 140, 140, 140), 0.8f);
            g.DrawEllipse(&ring, bx - 3.5f, by - 3.5f, 7.0f, 7.0f);
        }
    }
}