#include "WindowDetector.h"
#include <dwmapi.h>
#include <algorithm>
#include <cwchar>
#include <utility>
#pragma comment(lib, "dwmapi.lib")

namespace {

    struct ChildCtx { std::vector<RECT>* out; int ox, oy; RECT limit; };
    struct TopCtx { std::vector<WindowDetector::Entry>* out; int ox, oy; RECT limit; };
    struct MonCtx { std::vector<WindowDetector::Entry>* out; int ox, oy; };

    BOOL CALLBACK EnumChildProc(HWND hwnd, LPARAM lp)
    {
        auto* c = reinterpret_cast<ChildCtx*>(lp);
        if (!IsWindowVisible(hwnd)) return TRUE;

        RECT r;
        if (!GetWindowRect(hwnd, &r)) return TRUE;
        OffsetRect(&r, -c->ox, -c->oy);

        RECT clipped;
        if (!IntersectRect(&clipped, &r, &c->limit)) return TRUE;
        if (clipped.right - clipped.left < 4 || clipped.bottom - clipped.top < 4) return TRUE;

        c->out->push_back(clipped);
        return TRUE;
    }

    BOOL CALLBACK EnumTopProc(HWND hwnd, LPARAM lp)
    {
        auto* c = reinterpret_cast<TopCtx*>(lp);

        if (!IsWindowVisible(hwnd) || IsIconic(hwnd)) return TRUE;

        // Bỏ cửa sổ "xuyên chuột" (overlay của game, công cụ quay phim...)
        LONG_PTR ex = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
        if (ex & WS_EX_TRANSPARENT) return TRUE;

        // Bỏ cửa sổ bị ẩn bởi DWM (ứng dụng UWP đang tạm ngưng, desktop ảo...)
        DWORD cloaked = 0;
        DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, &cloaked, sizeof(cloaked));
        if (cloaked) return TRUE;

        // Desktop phủ kín màn hình, ta dùng khung màn hình thay thế
        wchar_t cls[64] = L"";
        GetClassNameW(hwnd, cls, 64);
        if (wcscmp(cls, L"Progman") == 0 || wcscmp(cls, L"WorkerW") == 0) return TRUE;

        // Khung thật của cửa sổ, không tính viền bóng đổ vô hình
        RECT r;
        if (FAILED(DwmGetWindowAttribute(hwnd, DWMWA_EXTENDED_FRAME_BOUNDS, &r, sizeof(r))))
            GetWindowRect(hwnd, &r);
        OffsetRect(&r, -c->ox, -c->oy);

        RECT clipped;
        if (!IntersectRect(&clipped, &r, &c->limit)) return TRUE;
        if (clipped.right - clipped.left < 4 || clipped.bottom - clipped.top < 4) return TRUE;

        WindowDetector::Entry e;
        e.rect = clipped;

        ChildCtx cc{ &e.children, c->ox, c->oy, clipped };
        EnumChildWindows(hwnd, EnumChildProc, reinterpret_cast<LPARAM>(&cc));

        c->out->push_back(std::move(e));
        return TRUE;
    }

    BOOL CALLBACK EnumMonProc(HMONITOR, HDC, LPRECT rc, LPARAM lp)
    {
        auto* c = reinterpret_cast<MonCtx*>(lp);
        WindowDetector::Entry e;
        e.rect = *rc;
        OffsetRect(&e.rect, -c->ox, -c->oy);
        c->out->push_back(std::move(e));
        return TRUE;
    }

} // namespace

void WindowDetector::Snapshot(int ox, int oy, int w, int h)
{
    entries_.clear();

    // EnumWindows duyệt từ cửa sổ nằm trên cùng xuống dưới (theo thứ tự Z)
    TopCtx tc{ &entries_, ox, oy, RECT{ 0, 0, w, h } };
    EnumWindows(EnumTopProc, reinterpret_cast<LPARAM>(&tc));

    // Khung từng màn hình đặt ở cuối: làm phương án dự phòng khi không trúng cửa sổ nào
    MonCtx mc{ &entries_, ox, oy };
    EnumDisplayMonitors(nullptr, nullptr, EnumMonProc, reinterpret_cast<LPARAM>(&mc));
}

bool WindowDetector::Detect(POINT pt, std::vector<RECT>& chain) const
{
    chain.clear();

    for (const Entry& e : entries_) {
        if (!PtInRect(&e.rect, pt)) continue;   // cửa sổ đầu tiên (trên cùng) chứa điểm

        std::vector<RECT> cands;
        for (const RECT& c : e.children)
            if (PtInRect(&c, pt)) cands.push_back(c);

        // Nhỏ nhất đứng trước
        std::sort(cands.begin(), cands.end(), [](const RECT& a, const RECT& b) {
            long long aa = (long long)(a.right - a.left) * (a.bottom - a.top);
            long long bb = (long long)(b.right - b.left) * (b.bottom - b.top);
            return aa < bb;
            });

        for (const RECT& c : cands)
            if (chain.empty() || !EqualRect(&chain.back(), &c))
                chain.push_back(c);

        if (chain.empty() || !EqualRect(&chain.back(), &e.rect))
            chain.push_back(e.rect);

        return true;
    }
    return false;
}