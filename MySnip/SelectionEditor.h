#pragma once
#include <windows.h>

// Vùng mà con trỏ chuột đang chỉ vào so với khung chọn
enum HitZone
{
    HIT_NONE = 0,
    HIT_INSIDE,
    HIT_LEFT_TOP, HIT_TOP, HIT_RIGHT_TOP,
    HIT_RIGHT, HIT_RIGHT_BOTTOM, HIT_BOTTOM,
    HIT_LEFT_BOTTOM, HIT_LEFT
};

class SelectionEditor
{
public:
    // --- Không cần trạng thái ---
    static HitZone HitTest(const RECT& r, POINT pt);
    static LPCWSTR CursorFor(HitZone zone);
    static void    DrawHandles(HDC hdc, const RECT& r);

    // --- Một lần thao tác kéo ---
    void Begin(HitZone zone, POINT pt, const RECT& rect);
    // Trả về hình chữ nhật mới khi chuột ở vị trí pt (đã giới hạn trong màn hình)
    RECT Update(POINT pt, int boundsW, int boundsH) const;
    HitZone Zone() const { return zone_; }

private:
    HitZone zone_ = HIT_NONE;
    POINT   startPt_ = {};
    RECT    startRect_ = {};
};