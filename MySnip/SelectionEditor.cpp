#include "SelectionEditor.h"
#include <cstdlib>

static const int GRAB = 6;   // vùng bắt chuột quanh cạnh (px)
static const int MIN_SIZE = 4;   // kích thước tối thiểu của vùng chọn

static int ClampInt(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }
static int MaxInt(int a, int b) { return a > b ? a : b; }
static int MinInt(int a, int b) { return a < b ? a : b; }

HitZone SelectionEditor::HitTest(const RECT& r, POINT pt)
{
    bool inX = pt.x >= r.left - GRAB && pt.x <= r.right + GRAB;
    bool inY = pt.y >= r.top - GRAB && pt.y <= r.bottom + GRAB;
    if (!inX || !inY) return HIT_NONE;

    bool nearL = abs(pt.x - r.left) <= GRAB;
    bool nearR = abs(pt.x - r.right) <= GRAB;
    bool nearT = abs(pt.y - r.top) <= GRAB;
    bool nearB = abs(pt.y - r.bottom) <= GRAB;

    // Góc ưu tiên trước, rồi tới cạnh
    if (nearL && nearT) return HIT_LEFT_TOP;
    if (nearR && nearT) return HIT_RIGHT_TOP;
    if (nearL && nearB) return HIT_LEFT_BOTTOM;
    if (nearR && nearB) return HIT_RIGHT_BOTTOM;
    if (nearT) return HIT_TOP;
    if (nearB) return HIT_BOTTOM;
    if (nearL) return HIT_LEFT;
    if (nearR) return HIT_RIGHT;
    return HIT_INSIDE;   // nằm trong khung mở rộng mà không gần cạnh nào = bên trong
}

LPCWSTR SelectionEditor::CursorFor(HitZone zone)
{
    switch (zone) {
    case HIT_LEFT_TOP:
    case HIT_RIGHT_BOTTOM: return IDC_SIZENWSE;
    case HIT_RIGHT_TOP:
    case HIT_LEFT_BOTTOM:  return IDC_SIZENESW;
    case HIT_TOP:
    case HIT_BOTTOM:       return IDC_SIZENS;
    case HIT_LEFT:
    case HIT_RIGHT:        return IDC_SIZEWE;
    case HIT_INSIDE:       return IDC_SIZEALL;
    default:               return IDC_CROSS;
    }
}

void SelectionEditor::DrawHandles(HDC hdc, const RECT& r)
{
    int cx = (r.left + r.right) / 2;
    int cy = (r.top + r.bottom) / 2;
    POINT pts[8] = {
        { r.left,  r.top },{ cx, r.top },{ r.right, r.top },
        { r.right, cy },
        { r.right, r.bottom },{ cx, r.bottom },{ r.left,  r.bottom },
        { r.left,  cy }
    };

    HPEN   pen = CreatePen(PS_SOLID, 1, RGB(0, 174, 255));
    HGDIOBJ oldPen = SelectObject(hdc, pen);
    HGDIOBJ oldBrush = SelectObject(hdc, GetStockObject(WHITE_BRUSH));

    for (const POINT& p : pts)
        Rectangle(hdc, p.x - 4, p.y - 4, p.x + 5, p.y + 5);

    SelectObject(hdc, oldBrush);
    SelectObject(hdc, oldPen);
    DeleteObject(pen);
}

void SelectionEditor::Begin(HitZone zone, POINT pt, const RECT& rect)
{
    zone_ = zone;
    startPt_ = pt;
    startRect_ = rect;
}

RECT SelectionEditor::Update(POINT pt, int bw, int bh) const
{
    int dx = pt.x - startPt_.x;
    int dy = pt.y - startPt_.y;
    RECT r = startRect_;

    // Di chuyển: giữ nguyên kích thước, chỉ giới hạn trong màn hình
    if (zone_ == HIT_INSIDE) {
        int w = r.right - r.left;
        int h = r.bottom - r.top;
        int l = ClampInt(r.left + dx, 0, MaxInt(0, bw - w));
        int t = ClampInt(r.top + dy, 0, MaxInt(0, bh - h));
        return { l, t, l + w, t + h };
    }

    // Co giãn: tay nắm nào thì dịch cạnh đó
    bool L = zone_ == HIT_LEFT_TOP || zone_ == HIT_LEFT || zone_ == HIT_LEFT_BOTTOM;
    bool R = zone_ == HIT_RIGHT_TOP || zone_ == HIT_RIGHT || zone_ == HIT_RIGHT_BOTTOM;
    bool T = zone_ == HIT_LEFT_TOP || zone_ == HIT_TOP || zone_ == HIT_RIGHT_TOP;
    bool B = zone_ == HIT_LEFT_BOTTOM || zone_ == HIT_BOTTOM || zone_ == HIT_RIGHT_BOTTOM;

    if (L) r.left += dx;
    if (R) r.right += dx;
    if (T) r.top += dy;
    if (B) r.bottom += dy;

    r.left = ClampInt(r.left, 0, bw);
    r.right = ClampInt(r.right, 0, bw);
    r.top = ClampInt(r.top, 0, bh);
    r.bottom = ClampInt(r.bottom, 0, bh);

    // Kéo cạnh này vượt qua cạnh đối diện thì lật khung (vẫn là hình chữ nhật hợp lệ)
    if (r.left > r.right) { LONG t = r.left; r.left = r.right; r.right = t; }
    if (r.top > r.bottom) { LONG t = r.top;  r.top = r.bottom; r.bottom = t; }

    // Không cho nhỏ hơn MIN_SIZE
    if (r.right - r.left < MIN_SIZE) {
        r.right = MinInt(bw, r.left + MIN_SIZE);
        r.left = r.right - MIN_SIZE;
    }
    if (r.bottom - r.top < MIN_SIZE) {
        r.bottom = MinInt(bh, r.top + MIN_SIZE);
        r.top = r.bottom - MIN_SIZE;
    }
    return r;
}