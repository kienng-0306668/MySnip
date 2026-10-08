#include <windows.h>
#include <windowsx.h>
#include <gdiplus.h>
#include <cstdio>
#include <cstdlib>
#include <cwchar>
#include <vector>
#include <shellapi.h>
#include "Commands.h"
#include "Toolbar.h"
#include <algorithm>
#include "UiaDetector.h"
#include "WindowDetector.h"
#include "SelectionEditor.h"
#include "Annotations.h"
#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "msimg32.lib")   // cho AlphaBlend

using namespace Gdiplus;

// ===== Cấu hình phím tắt =====
// F1: chụp | Ctrl + Alt + `: thoát chương trình
// ESC: hủy overlay (hoặc hủy ô chữ đang gõ)

static HHOOK       g_hKeyboardHook = nullptr;
static HWND        g_hWnd = nullptr;        // cửa sổ ẩn nhận message
static HWND        g_hOverlay = nullptr;    // overlay chọn vùng
static HFONT       g_uiFont = nullptr;      // font cho nhãn kích thước
static Toolbar     g_toolbar;               // toolbar (class riêng)
static Annotations g_ann;                   // chú thích (class riêng)

enum {
    WM_MY_CAPTURE = WM_USER + 1, WM_MY_QUIT = WM_USER + 2,
    WM_MY_CANCEL = WM_USER + 3, WM_MY_TRAY = WM_USER + 4,
    WM_MY_TEXT_DONE = WM_USER + 5, WM_MY_UIA = WM_USER + 6 };
enum { ID_TRAY_CAPTURE = 1001, ID_TRAY_EXIT = 1002 };
enum { IDC_TEXTEDIT = 2001 };

static HANDLE          g_hMutex = nullptr;
static NOTIFYICONDATAW g_nid = {};
static UINT            g_msgTaskbarCreated = 0;

// ===== Trạng thái overlay =====
enum SelState { SEL_IDLE, SEL_DRAGGING, SEL_DONE, SEL_MOVING, SEL_RESIZING, SEL_ANNOTATING };

static HBITMAP  g_hFrozen = nullptr;   // ảnh màn hình gốc
static HBITMAP  g_hDimmed = nullptr;   // ảnh màn hình đã làm tối
static int      g_vx = 0, g_vy = 0, g_vw = 0, g_vh = 0;   // màn hình ảo
static SelState g_state = SEL_IDLE;
static POINT    g_start = {};
static RECT     g_sel = {};

// Nhận diện ô và chỉnh sửa vùng chọn
static WindowDetector    g_detector;
static UiaDetector       g_uia;
static std::vector<RECT> g_uiaCache;       // phần tử UIA của lần nhận diện gần nhất (tọa độ overlay)
static RECT              g_uiaTop = {};    // cửa sổ mà cache thuộc về
static POINT             g_lastHoverPt = {};
static SelectionEditor   g_editor;
static std::vector<RECT> g_chain;       // các ô chứa con trỏ, từ nhỏ đến lớn
static int               g_level = 0;   // đang chọn ô thứ mấy trong chuỗi
static RECT              g_hoverRect = {};

// Ô nhập chữ (công cụ Text)
static HWND    g_edit = nullptr;
static HFONT   g_editFont = nullptr;
static POINT   g_editPos = {};
static WNDPROC g_editOldProc = nullptr;

// Bảng màu: phím 1..7
static const COLORREF PALETTE[7] = {
    RGB(255, 59, 48),  RGB(255, 149, 0), RGB(255, 214, 10), RGB(52, 199, 89),
    RGB(0, 122, 255),  RGB(255, 255, 255), RGB(0, 0, 0)
};

static int MinI(int a, int b) { return a < b ? a : b; }
static int MaxI(int a, int b) { return a > b ? a : b; }
static int ClampI(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

static void RefreshToolbarState();   // khai báo trước

// ===== Các hàm vẽ lại =====
static void InvalidateToolbar(HWND hWnd)
{
    if (!hWnd) return;
    RECT tb = g_toolbar.Bounds();
    InflateRect(&tb, 2, 2);
    InvalidateRect(hWnd, &tb, FALSE);
}

static void InvalidateSel()
{
    if (!g_hOverlay) return;
    RECT r = g_sel;
    InflateRect(&r, 40, 40);
    InvalidateRect(g_hOverlay, &r, FALSE);
}

// Nhãn kích thước có thể dài hơn vùng chọn nên cần vẽ lại riêng
static void InvalidateLabel()
{
    if (!g_hOverlay || IsRectEmpty(&g_sel)) return;
    RECT l = { g_sel.left - 4, g_sel.top - 36, g_sel.left + 320, g_sel.top + 30 };
    InvalidateRect(g_hOverlay, &l, FALSE);
}

// ===== Tiện ích PNG =====
static bool GetPngEncoderClsid(CLSID* clsid)
{
    UINT num = 0, size = 0;
    if (GetImageEncodersSize(&num, &size) != Ok || size == 0)
        return false;

    std::vector<BYTE> buffer(size);
    auto* info = reinterpret_cast<ImageCodecInfo*>(buffer.data());
    if (GetImageEncoders(num, size, info) != Ok)
        return false;

    for (UINT i = 0; i < num; ++i) {
        if (wcscmp(info[i].MimeType, L"image/png") == 0) {
            *clsid = info[i].Clsid;
            return true;
        }
    }
    return false;
}

static void SaveBitmapPng(HBITMAP hBmp, const wchar_t* path)
{
    Bitmap bmp(hBmp, nullptr);
    CLSID png;
    if (GetPngEncoderClsid(&png))
        bmp.Save(path, &png, nullptr);
}

static bool BuildSavePath(wchar_t* path, size_t count)
{
    wchar_t profile[MAX_PATH] = L"";
    DWORD r = GetEnvironmentVariableW(L"USERPROFILE", profile, MAX_PATH);
    if (r == 0 || r >= MAX_PATH) return false;

    SYSTEMTIME st;
    GetLocalTime(&st);
    swprintf_s(path, count, L"%s\\Pictures\\snip_%04d%02d%02d_%02d%02d%02d.png",
        profile, st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    return true;
}

// ===== Chụp màn hình ảo vào bitmap =====
static HBITMAP GrabScreen()
{
    g_vx = GetSystemMetrics(SM_XVIRTUALSCREEN);
    g_vy = GetSystemMetrics(SM_YVIRTUALSCREEN);
    g_vw = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    g_vh = GetSystemMetrics(SM_CYVIRTUALSCREEN);

    HDC hdcScreen = GetDC(nullptr);
    HDC hdcMem = CreateCompatibleDC(hdcScreen);
    HBITMAP hBmp = CreateCompatibleBitmap(hdcScreen, g_vw, g_vh);
    HGDIOBJ old = SelectObject(hdcMem, hBmp);

    BitBlt(hdcMem, 0, 0, g_vw, g_vh, hdcScreen, g_vx, g_vy, SRCCOPY | CAPTUREBLT);

    SelectObject(hdcMem, old);
    DeleteDC(hdcMem);
    ReleaseDC(nullptr, hdcScreen);
    return hBmp;
}

// Tạo bản sao đã phủ lớp đen mờ (làm tối)
static HBITMAP MakeDimmed(HBITMAP frozen)
{
    HDC hdcScreen = GetDC(nullptr);
    HDC dcSrc = CreateCompatibleDC(hdcScreen);
    HDC dcDst = CreateCompatibleDC(hdcScreen);
    HDC dcBlack = CreateCompatibleDC(hdcScreen);

    HBITMAP dimmed = CreateCompatibleBitmap(hdcScreen, g_vw, g_vh);
    HBITMAP black = CreateCompatibleBitmap(hdcScreen, 1, 1);

    HGDIOBJ o1 = SelectObject(dcSrc, frozen);
    HGDIOBJ o2 = SelectObject(dcDst, dimmed);
    HGDIOBJ o3 = SelectObject(dcBlack, black);

    PatBlt(dcBlack, 0, 0, 1, 1, BLACKNESS);
    BitBlt(dcDst, 0, 0, g_vw, g_vh, dcSrc, 0, 0, SRCCOPY);

    BLENDFUNCTION bf = { AC_SRC_OVER, 0, 120, 0 };   // 120/255 ≈ 47% đen
    AlphaBlend(dcDst, 0, 0, g_vw, g_vh, dcBlack, 0, 0, 1, 1, bf);

    SelectObject(dcSrc, o1);
    SelectObject(dcDst, o2);
    SelectObject(dcBlack, o3);
    DeleteObject(black);
    DeleteDC(dcSrc);
    DeleteDC(dcDst);
    DeleteDC(dcBlack);
    ReleaseDC(nullptr, hdcScreen);
    return dimmed;
}

// ===== Đóng overlay, giải phóng bộ nhớ =====
static void CloseOverlay()
{
    g_edit = nullptr;   // cửa sổ con (ô nhập chữ) sẽ bị hủy cùng overlay
    if (g_hOverlay) {
        HWND h = g_hOverlay;
        g_hOverlay = nullptr;
        DestroyWindow(h);
    }
    if (g_editFont) { DeleteObject(g_editFont); g_editFont = nullptr; }
    if (g_hFrozen) { DeleteObject(g_hFrozen); g_hFrozen = nullptr; }
    if (g_hDimmed) { DeleteObject(g_hDimmed); g_hDimmed = nullptr; }

    g_state = SEL_IDLE;
    g_sel = {};
    g_hoverRect = {};
    g_chain.clear();
    g_level = 0;
    g_detector.Clear();
    g_uia.Cancel();
    g_uiaCache.clear();
    g_uiaTop = {};
    g_ann.Reset();
    g_toolbar.Show(false);
    g_toolbar.ClearHover();
    RefreshToolbarState();
}

// ===================================================================
//  CỬA SỔ GHIM (PIN)
// ===================================================================
struct PinData {
    HBITMAP bmp;
    int     w, h;      // kích thước gốc
    double  scale;     // tỉ lệ phóng hiện tại
    BYTE    alpha;     // độ đục 0..255
};

static LRESULT CALLBACK PinProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    PinData* d = reinterpret_cast<PinData*>(GetWindowLongPtrW(hWnd, GWLP_USERDATA));

    switch (msg) {
    case WM_ERASEBKGND:
        return 1;

    case WM_NCHITTEST:
        return HTCAPTION;   // kéo bất cứ đâu để di chuyển

    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hWnd, &ps);
        if (d) {
            RECT rc;
            GetClientRect(hWnd, &rc);

            HDC mem = CreateCompatibleDC(hdc);
            HGDIOBJ old = SelectObject(mem, d->bmp);
            SetStretchBltMode(hdc, HALFTONE);
            SetBrushOrgEx(hdc, 0, 0, nullptr);
            StretchBlt(hdc, 0, 0, rc.right, rc.bottom, mem, 0, 0, d->w, d->h, SRCCOPY);
            SelectObject(mem, old);
            DeleteDC(mem);

            HBRUSH br = CreateSolidBrush(RGB(0, 174, 255));
            FrameRect(hdc, &rc, br);
            DeleteObject(br);
        }
        EndPaint(hWnd, &ps);
        return 0;
    }

    case WM_MOUSEWHEEL: {
        if (!d) return 0;
        int delta = GET_WHEEL_DELTA_WPARAM(wParam);
        bool ctrl = (GET_KEYSTATE_WPARAM(wParam) & MK_CONTROL) != 0;

        if (ctrl) {
            int a = d->alpha + (delta > 0 ? 15 : -15);
            d->alpha = static_cast<BYTE>(ClampI(a, 40, 255));
            SetLayeredWindowAttributes(hWnd, 0, d->alpha, LWA_ALPHA);
        }
        else {
            d->scale *= (delta > 0) ? 1.1 : (1.0 / 1.1);
            if (d->scale < 0.1) d->scale = 0.1;
            if (d->scale > 8.0) d->scale = 8.0;

            int nw = MaxI(20, static_cast<int>(d->w * d->scale));
            int nh = MaxI(20, static_cast<int>(d->h * d->scale));

            RECT r;
            GetWindowRect(hWnd, &r);
            int cx = (r.left + r.right) / 2;
            int cy = (r.top + r.bottom) / 2;
            SetWindowPos(hWnd, nullptr, cx - nw / 2, cy - nh / 2, nw, nh,
                SWP_NOZORDER | SWP_NOACTIVATE);
            InvalidateRect(hWnd, nullptr, FALSE);
        }
        return 0;
    }

    case WM_NCLBUTTONDBLCLK:
    case WM_NCRBUTTONUP:
        DestroyWindow(hWnd);
        return 0;

    case WM_NCDESTROY:
        if (d) {
            DeleteObject(d->bmp);
            delete d;
            SetWindowLongPtrW(hWnd, GWLP_USERDATA, 0);
        }
        break;
    }
    return DefWindowProcW(hWnd, msg, wParam, lParam);
}

// Tạo cửa sổ ghim; hàm này nhận quyền sở hữu bitmap
static void CreatePinWindow(HBITMAP bmp, int x, int y)
{
    BITMAP bm = {};
    GetObjectW(bmp, sizeof(bm), &bm);

    PinData* d = new PinData{ bmp, bm.bmWidth, bm.bmHeight, 1.0, 255 };

    HWND h = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_LAYERED,
        L"MySnipPin", L"", WS_POPUP,
        x, y, d->w, d->h,
        nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);

    if (!h) {
        DeleteObject(bmp);
        delete d;
        return;
    }

    SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(d));
    SetLayeredWindowAttributes(h, 0, 255, LWA_ALPHA);
    ShowWindow(h, SW_SHOWNOACTIVATE);
    UpdateWindow(h);
}

// ===================================================================
//  HÀNH ĐỘNG SAU KHI CHỌN VÙNG
// ===================================================================

// Cắt vùng chọn ra bitmap mới, đã vẽ kèm chú thích
static HBITMAP CropSelection()
{
    int w = g_sel.right - g_sel.left;
    int h = g_sel.bottom - g_sel.top;
    if (w <= 0 || h <= 0 || !g_hFrozen) return nullptr;

    HDC hdcScreen = GetDC(nullptr);
    HDC dcSrc = CreateCompatibleDC(hdcScreen);
    HDC dcDst = CreateCompatibleDC(hdcScreen);
    HBITMAP crop = CreateCompatibleBitmap(hdcScreen, w, h);

    HGDIOBJ o1 = SelectObject(dcSrc, g_hFrozen);
    HGDIOBJ o2 = SelectObject(dcDst, crop);
    BitBlt(dcDst, 0, 0, w, h, dcSrc, g_sel.left, g_sel.top, SRCCOPY);

    if (!g_ann.Empty()) {
        RECT clip = { 0, 0, w, h };
        g_ann.Render(dcDst, dcSrc, g_sel.left, g_sel.top, clip);   // dcSrc = ảnh gốc cho mosaic
    }

    SelectObject(dcSrc, o1);
    SelectObject(dcDst, o2);
    DeleteDC(dcSrc);
    DeleteDC(dcDst);
    ReleaseDC(nullptr, hdcScreen);
    return crop;
}

static void ActionSave()
{
    HBITMAP crop = CropSelection();
    if (!crop) return;

    wchar_t path[MAX_PATH];
    if (BuildSavePath(path, MAX_PATH))
        SaveBitmapPng(crop, path);

    DeleteObject(crop);
    CloseOverlay();
}

static void ActionCopy()
{
    HBITMAP crop = CropSelection();
    if (!crop) return;

    if (OpenClipboard(g_hOverlay)) {
        EmptyClipboard();
        if (SetClipboardData(CF_BITMAP, crop))
            crop = nullptr;           // clipboard đã sở hữu bitmap
        CloseClipboard();
    }
    if (crop) DeleteObject(crop);
    CloseOverlay();
}

static void ActionPin()
{
    HBITMAP crop = CropSelection();
    if (!crop) return;

    int sx = g_vx + g_sel.left;
    int sy = g_vy + g_sel.top;

    CloseOverlay();                   // CloseOverlay không đụng tới "crop"
    CreatePinWindow(crop, sx, sy);    // cửa sổ ghim nhận quyền sở hữu bitmap
}

// ===================================================================
//  CÔNG CỤ CHỮ: ô nhập liệu
// ===================================================================
static LRESULT CALLBACK EditProc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    if (m == WM_KEYDOWN && w == VK_RETURN) {
        PostMessageW(g_hOverlay, WM_MY_TEXT_DONE, 0, 0);
        return 0;
    }
    if (m == WM_CHAR && w == VK_RETURN) return 0;   // chặn tiếng "bíp"
    return CallWindowProcW(g_editOldProc, h, m, w, l);
}

static void BeginTextEdit(POINT pt)
{
    int px = g_ann.FontPx();
    g_editFont = Annotations::MakeFont(px);
    g_editPos = pt;

    g_edit = CreateWindowExW(0, L"EDIT", L"", WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
        pt.x, pt.y, 140, px + 6, g_hOverlay,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_TEXTEDIT)),
        GetModuleHandleW(nullptr), nullptr);
    if (!g_edit) {
        DeleteObject(g_editFont);
        g_editFont = nullptr;
        return;
    }

    SendMessageW(g_edit, WM_SETFONT, reinterpret_cast<WPARAM>(g_editFont), TRUE);
    SendMessageW(g_edit, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, MAKELONG(0, 0));
    g_editOldProc = reinterpret_cast<WNDPROC>(
        SetWindowLongPtrW(g_edit, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(EditProc)));
    SetFocus(g_edit);
}

static void EndTextEdit(bool commit)
{
    if (!g_edit) return;

    wchar_t buf[512] = L"";
    GetWindowTextW(g_edit, buf, 512);

    HWND e = g_edit;
    g_edit = nullptr;
    DestroyWindow(e);
    if (g_editFont) { DeleteObject(g_editFont); g_editFont = nullptr; }

    if (commit && buf[0]) {
        g_ann.AddText(g_editPos, buf);
        RefreshToolbarState();
    }
    if (g_hOverlay) {
        SetFocus(g_hOverlay);
        InvalidateSel();
    }
}

static void CommitText() { EndTextEdit(true); }
static void CancelText() { EndTextEdit(false); }

// ESC: đang gõ chữ thì chỉ hủy ô chữ, ngược lại đóng overlay
static void HandleCancel()
{
    if (g_edit) CancelText();
    else        CloseOverlay();
}

// ===================================================================
//  TOOLBAR: khai báo nút và xử lý lệnh
// ===================================================================
struct ToolMap { int cmd; ToolType tool; };
static const ToolMap TOOL_MAP[] = {
    { CMD_TOOL_RECT,   TOOL_RECT },
    { CMD_TOOL_ARROW,  TOOL_ARROW },
    { CMD_TOOL_PEN,    TOOL_PEN },
    { CMD_TOOL_MARKER, TOOL_MARKER },
    { CMD_TOOL_MOSAIC, TOOL_MOSAIC },
    { CMD_TOOL_TEXT,   TOOL_TEXT },
    { CMD_TOOL_ERASER, TOOL_ERASER },
};

// Nơi "thiết kế" toolbar: thứ tự gọi chính là thứ tự nút từ trái sang phải
static void BuildToolbar()
{
    g_toolbar.SetFont(g_uiFont);

    g_toolbar.AddIconButton(CMD_TOOL_RECT, IC_RECT);
    g_toolbar.AddIconButton(CMD_TOOL_ARROW, IC_ARROW);
    g_toolbar.AddIconButton(CMD_TOOL_PEN, IC_PEN);
    g_toolbar.AddIconButton(CMD_TOOL_MARKER, IC_MARKER);
    g_toolbar.AddIconButton(CMD_TOOL_MOSAIC, IC_MOSAIC);
    g_toolbar.AddIconButton(CMD_TOOL_TEXT, IC_TEXT);
    g_toolbar.AddIconButton(CMD_TOOL_ERASER, IC_ERASER);
    g_toolbar.AddSeparator();
    g_toolbar.AddIconButton(CMD_UNDO, IC_UNDO);
    g_toolbar.AddIconButton(CMD_REDO, IC_REDO);
    g_toolbar.AddSeparator();
    g_toolbar.AddIconButton(CMD_CANCEL, IC_CLOSE);
    g_toolbar.AddIconButton(CMD_PIN, IC_PIN);
    g_toolbar.AddIconButton(CMD_SAVE, IC_SAVE);
    g_toolbar.AddIconButton(CMD_COPY, IC_COPY);

    g_toolbar.SetEnabled(CMD_UNDO, false);
    g_toolbar.SetEnabled(CMD_REDO, false);
}

// Đồng bộ giao diện toolbar với trạng thái của g_ann
static void RefreshToolbarState()
{
    ToolType cur = g_ann.Tool();
    for (const ToolMap& m : TOOL_MAP) {
        bool on = (cur == m.tool);
        g_toolbar.SetChecked(m.cmd, on);
        g_toolbar.SetBadge(m.cmd, on && m.tool != TOOL_ERASER, g_ann.Color());
    }
    g_toolbar.SetIcon(CMD_TOOL_RECT, g_ann.Ellipse() ? IC_ELLIPSE : IC_RECT);
    g_toolbar.SetEnabled(CMD_UNDO, g_ann.CanUndo());
    g_toolbar.SetEnabled(CMD_REDO, g_ann.CanRedo());

    InvalidateToolbar(g_hOverlay);
    InvalidateLabel();
}

static void OnToolButton(int cmd)
{
    ToolType t = TOOL_NONE;
    for (const ToolMap& m : TOOL_MAP)
        if (m.cmd == cmd) t = m.tool;

    ToolType cur = g_ann.Tool();
    if (t == TOOL_RECT) {
        // Chu kỳ: tắt -> chữ nhật -> elip -> tắt
        if (cur != TOOL_RECT) { g_ann.SetTool(TOOL_RECT); g_ann.SetEllipse(false); }
        else if (!g_ann.Ellipse()) { g_ann.SetEllipse(true); }
        else { g_ann.SetTool(TOOL_NONE); g_ann.SetEllipse(false); }
    }
    else {
        g_ann.SetTool(cur == t ? TOOL_NONE : t);
    }
    RefreshToolbarState();
}

static void DoUndo()
{
    CommitText();
    g_ann.Undo();
    RefreshToolbarState();
    InvalidateSel();
}

static void DoRedo()
{
    CommitText();
    g_ann.Redo();
    RefreshToolbarState();
    InvalidateSel();
}

static void SetColorIndex(int i)
{
    g_ann.SetColor(PALETTE[ClampI(i, 0, 6)]);
    RefreshToolbarState();
}

// Nơi duy nhất quyết định "bấm nút nào thì làm gì"
static void ExecuteCommand(int cmd)
{
    if (g_edit && cmd != CMD_CANCEL) CommitText();   // chốt chữ đang gõ trước khi làm gì khác

    switch (cmd) {
    case CMD_TOOL_RECT:
    case CMD_TOOL_ARROW:
    case CMD_TOOL_PEN:
    case CMD_TOOL_MARKER:
    case CMD_TOOL_MOSAIC:
    case CMD_TOOL_TEXT:
    case CMD_TOOL_ERASER: OnToolButton(cmd); break;

    case CMD_UNDO:   DoUndo();       break;
    case CMD_REDO:   DoRedo();       break;

    case CMD_PIN:    ActionPin();    break;
    case CMD_SAVE:   ActionSave();   break;
    case CMD_COPY:   ActionCopy();   break;
    case CMD_CANCEL: CloseOverlay(); break;
    }
}

// ===================================================================
//  OVERLAY
// ===================================================================

// Vùng đang được hiển thị sáng: ô đang rê chuột (IDLE) hoặc vùng chọn
static RECT CurrentShowRect()
{
    return (g_state == SEL_IDLE) ? g_hoverRect : g_sel;
}

// Vẽ lại một vùng, nới thêm lề cho viền, nhãn và tay nắm
static void InvalidateShow(HWND hWnd, RECT r)
{
    InflateRect(&r, 40, 40);
    InvalidateRect(hWnd, &r, FALSE);
}

static long long AreaOf(const RECT& r)
{
    return static_cast<long long>(r.right - r.left) * (r.bottom - r.top);
}

// Gộp các ô UIA (chứa con trỏ, cắt theo cửa sổ) vào chuỗi Win32, xếp từ nhỏ đến lớn
static void MergeUia(std::vector<RECT>& chain, const RECT& top, POINT pt)
{
    for (const RECT& u : g_uiaCache) {
        RECT c;
        if (!PtInRect(&u, pt)) continue;
        if (!IntersectRect(&c, &u, &top)) continue;
        if (c.right - c.left < 4 || c.bottom - c.top < 4) continue;
        chain.push_back(c);
    }

    std::stable_sort(chain.begin(), chain.end(),
        [](const RECT& a, const RECT& b) { return AreaOf(a) < AreaOf(b); });

    std::vector<RECT> uniq;
    for (const RECT& r : chain) {
        bool dup = false;
        for (const RECT& o : uniq)
            if (EqualRect(&o, &r)) { dup = true; break; }
        if (!dup) uniq.push_back(r);
    }
    chain.swap(uniq);
}

// Gửi yêu cầu nhận diện UIA cho điểm pt (tọa độ overlay)
static void RequestUia(POINT pt)
{
    if (!g_hOverlay) return;
    POINT sp = { pt.x + g_vx, pt.y + g_vy };
    g_uia.Request(sp, g_hOverlay, WM_MY_UIA);
}

// keepSelection = true khi chỉ làm mới chuỗi (kết quả UIA vừa về), không phải chuột di chuyển:
// giữ nguyên ô người dùng đã lăn chuột chọn nếu nó còn trong chuỗi mới
static bool UpdateHover(POINT pt, bool keepSelection = false)
{
    g_lastHoverPt = pt;

    std::vector<RECT> chain;
    if (!g_detector.Detect(pt, chain) || chain.empty()) {
        bool had = !IsRectEmpty(&g_hoverRect);
        g_chain.clear();
        g_hoverRect = {};
        g_uiaCache.clear();
        g_uiaTop = {};
        return had;
    }

    // Sang cửa sổ khác thì bỏ cache UIA của cửa sổ cũ
    const RECT top = chain.back();
    if (!EqualRect(&top, &g_uiaTop)) {
        g_uiaCache.clear();
        g_uiaTop = top;
    }
    MergeUia(chain, top, pt);

    if (keepSelection && g_level > 0) {
        int idx = -1;
        for (size_t i = 0; i < chain.size(); ++i)
            if (EqualRect(&chain[i], &g_hoverRect)) { idx = static_cast<int>(i); break; }
        g_level = (idx >= 0) ? idx : 0;
    }
    else if (g_chain.empty() || !EqualRect(&g_chain[0], &chain[0])) {
        g_level = 0;
    }

    g_chain = chain;
    if (g_level >= static_cast<int>(g_chain.size()))
        g_level = static_cast<int>(g_chain.size()) - 1;

    RECT nr = g_chain[g_level];
    bool changed = !EqualRect(&nr, &g_hoverRect);
    g_hoverRect = nr;
    return changed;
}

static void DrawSelectionDecor(HDC hdc, const RECT& rc)
{
    HPEN pen = CreatePen(PS_SOLID, 2, RGB(0, 174, 255));
    HGDIOBJ oldPen = SelectObject(hdc, pen);
    HGDIOBJ oldBrush = SelectObject(hdc, GetStockObject(NULL_BRUSH));
    Rectangle(hdc, rc.left - 1, rc.top - 1, rc.right + 1, rc.bottom + 1);
    SelectObject(hdc, oldBrush);
    SelectObject(hdc, oldPen);
    DeleteObject(pen);

    HGDIOBJ oldFont = SelectObject(hdc, g_uiFont);

    // Khi đang chọn công cụ vẽ thì nhãn hiện thêm độ dày nét (lăn chuột để đổi)
    bool showWidth = (g_state == SEL_DONE || g_state == SEL_ANNOTATING) &&
        g_ann.Tool() != TOOL_NONE && g_ann.Tool() != TOOL_ERASER;
    wchar_t label[64];
    int len;
    if (showWidth)
        len = swprintf_s(label, L"%d x %d   |   nét %d",
            rc.right - rc.left, rc.bottom - rc.top, g_ann.Width());
    else
        len = swprintf_s(label, L"%d x %d", rc.right - rc.left, rc.bottom - rc.top);

    SIZE sz;
    GetTextExtentPoint32W(hdc, label, len, &sz);

    int lx = rc.left;
    int ly = rc.top - sz.cy - 6;
    if (ly < 0) ly = rc.top + 4;   // sát mép trên thì đặt nhãn vào trong

    SetBkMode(hdc, OPAQUE);
    SetBkColor(hdc, RGB(0, 0, 0));
    SetTextColor(hdc, RGB(255, 255, 255));
    TextOutW(hdc, lx, ly, label, len);

    SelectObject(hdc, oldFont);

    // Tay nắm chỉ hiện khi vùng chọn còn kéo giãn được (chưa có chú thích)
    bool canEdit = (g_state == SEL_DONE || g_state == SEL_MOVING || g_state == SEL_RESIZING)
        && g_ann.Empty();
    if (canEdit)
        SelectionEditor::DrawHandles(hdc, rc);
}

// Bấm chuột vào vùng không thuộc toolbar sẽ làm gì
enum ClickAction { CA_IGNORE, CA_DRAW, CA_EDIT_SEL, CA_NEW_SEL };

static ClickAction Classify(POINT pt, HitZone& zone)
{
    bool locked = !g_ann.Empty();            // đã có chú thích: khóa vùng chọn
    ToolType t = g_ann.Tool();
    bool inSel = PtInRect(&g_sel, pt) != 0;
    zone = SelectionEditor::HitTest(g_sel, pt);

    if (t != TOOL_NONE && inSel && (locked || zone == HIT_INSIDE)) return CA_DRAW;
    if (!locked && zone != HIT_NONE && (zone != HIT_INSIDE || t == TOOL_NONE)) return CA_EDIT_SEL;
    if (locked || t != TOOL_NONE) return CA_IGNORE;
    return CA_NEW_SEL;
}

static POINT ClampToSel(POINT pt)
{
    pt.x = ClampI(pt.x, g_sel.left, g_sel.right);
    pt.y = ClampI(pt.y, g_sel.top, g_sel.bottom);
    return pt;
}

static void StartDrawing(HWND hWnd, POINT pt)
{
    pt = ClampToSel(pt);
    ToolType t = g_ann.Tool();

    if (t == TOOL_TEXT) {
        BeginTextEdit(pt);
        return;
    }

    g_state = SEL_ANNOTATING;
    SetCapture(hWnd);

    if (t == TOOL_ERASER) {
        if (g_ann.EraseAt(pt)) InvalidateSel();
    }
    else {
        RECT d = g_ann.BeginStroke(pt);
        InvalidateRect(hWnd, &d, FALSE);
    }
}

// Xử lý nhấn chuột trái (dùng chung cho WM_LBUTTONDOWN và WM_LBUTTONDBLCLK)
static void HandleLButtonDown(HWND hWnd, POINT pt)
{
    if (g_edit) CommitText();    // bấm đâu đó = chốt chữ đang gõ, rồi xử lý tiếp cú bấm

    if (g_state == SEL_DONE) {
        int cmd = g_toolbar.HitTest(pt);
        if (cmd != 0) { ExecuteCommand(cmd); return; }
        if (g_toolbar.Contains(pt)) return;      // bấm vào khoảng trống của toolbar

        HitZone zone;
        switch (Classify(pt, zone)) {
        case CA_IGNORE:
            return;
        case CA_DRAW:
            StartDrawing(hWnd, pt);
            return;
        case CA_EDIT_SEL:
            InvalidateToolbar(hWnd);
            g_toolbar.Show(false);
            g_toolbar.ClearHover();
            g_editor.Begin(zone, pt, g_sel);
            g_state = (zone == HIT_INSIDE) ? SEL_MOVING : SEL_RESIZING;
            SetCapture(hWnd);
            return;
        case CA_NEW_SEL:
            break;
        }
    }

    // Bắt đầu chọn vùng mới
    InvalidateToolbar(hWnd);
    g_toolbar.Show(false);
    g_toolbar.ClearHover();

    RECT old = CurrentShowRect();
    g_start = pt;
    g_sel = { pt.x, pt.y, pt.x, pt.y };
    g_state = SEL_DRAGGING;
    SetCapture(hWnd);

    InvalidateShow(hWnd, old);
}

static LRESULT CALLBACK OverlayProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg) {
    case WM_MY_UIA: {
        if (g_state != SEL_IDLE) return 0;

        std::vector<RECT> r;
        if (!g_uia.TakeResult(static_cast<unsigned>(wParam), r))
            return 0;                                  // kết quả cũ, chuột đã đi chỗ khác

        g_uiaCache.clear();
        for (RECT x : r) {                             // đổi từ tọa độ màn hình sang tọa độ overlay
            OffsetRect(&x, -g_vx, -g_vy);
            g_uiaCache.push_back(x);
        }

        RECT old = g_hoverRect;
        if (UpdateHover(g_lastHoverPt, true)) {
            RECT u;
            UnionRect(&u, &old, &g_hoverRect);
            InvalidateShow(hWnd, u);
        }
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;

    case WM_CTLCOLOREDIT: {
        // Ô nhập chữ: chữ có màu đang chọn; nền đổi sang xám đậm nếu màu quá sáng
        HDC dc = reinterpret_cast<HDC>(wParam);
        COLORREF c = g_ann.Color();
        int lum = (GetRValue(c) * 299 + GetGValue(c) * 587 + GetBValue(c) * 114) / 1000;
        bool dark = lum > 170;
        SetTextColor(dc, c);
        SetBkColor(dc, dark ? RGB(64, 64, 64) : RGB(255, 255, 255));
        return reinterpret_cast<LRESULT>(GetStockObject(dark ? DKGRAY_BRUSH : WHITE_BRUSH));
    }

    case WM_COMMAND:
        if (LOWORD(wParam) == IDC_TEXTEDIT && HIWORD(wParam) == EN_CHANGE && g_edit && g_editFont) {
            // Ô nhập tự giãn theo độ dài chữ
            wchar_t buf[512];
            int n = GetWindowTextW(g_edit, buf, 512);
            HDC dc = GetDC(g_edit);
            HGDIOBJ old = SelectObject(dc, g_editFont);
            SIZE sz = {};
            GetTextExtentPoint32W(dc, buf, n, &sz);
            SelectObject(dc, old);
            ReleaseDC(g_edit, dc);

            int w = MaxI(60, sz.cx + 20);
            w = MinI(w, MaxI(60, g_vw - g_editPos.x));
            SetWindowPos(g_edit, nullptr, 0, 0, w, g_ann.FontPx() + 6,
                SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
        }
        return 0;

    case WM_MY_TEXT_DONE:
        CommitText();
        return 0;

    case WM_SETCURSOR: {
        if (LOWORD(lParam) != HTCLIENT) break;

        POINT pt;
        GetCursorPos(&pt);
        ScreenToClient(hWnd, &pt);

        if (g_state == SEL_DONE) {
            if (g_toolbar.HitTest(pt) != 0) {
                SetCursor(LoadCursorW(nullptr, IDC_HAND));
                return TRUE;
            }
            if (g_toolbar.Contains(pt)) {
                SetCursor(LoadCursorW(nullptr, IDC_ARROW));
                return TRUE;
            }
            HitZone zone;
            switch (Classify(pt, zone)) {
            case CA_DRAW:
                SetCursor(LoadCursorW(nullptr,
                    g_ann.Tool() == TOOL_TEXT ? IDC_IBEAM : IDC_CROSS));
                return TRUE;
            case CA_EDIT_SEL:
                SetCursor(LoadCursorW(nullptr, SelectionEditor::CursorFor(zone)));
                return TRUE;
            case CA_IGNORE:
                SetCursor(LoadCursorW(nullptr, IDC_ARROW));
                return TRUE;
            default:
                break;
            }
        }
        else if (g_state == SEL_ANNOTATING) {
            SetCursor(LoadCursorW(nullptr, IDC_CROSS));
            return TRUE;
        }
        else if (g_state == SEL_MOVING || g_state == SEL_RESIZING) {
            SetCursor(LoadCursorW(nullptr, SelectionEditor::CursorFor(g_editor.Zone())));
            return TRUE;
        }
        break;
    }

    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hWnd, &ps);
        if (g_hFrozen && g_hDimmed) {
            HDC mem = CreateCompatibleDC(hdc);
            HGDIOBJ old = SelectObject(mem, g_hDimmed);

            RECT show = CurrentShowRect();
            bool has = show.right > show.left && show.bottom > show.top;

            // 1) Vẽ ảnh tối, trừ vùng đang hiển thị sáng
            SaveDC(hdc);
            if (has)
                ExcludeClipRect(hdc, show.left, show.top, show.right, show.bottom);
            BitBlt(hdc, 0, 0, g_vw, g_vh, mem, 0, 0, SRCCOPY);
            RestoreDC(hdc, -1);

            // 2) Vẽ ảnh sáng trong vùng đó, rồi vẽ chú thích lên trên
            if (has) {
                SelectObject(mem, g_hFrozen);
                BitBlt(hdc, show.left, show.top,
                    show.right - show.left, show.bottom - show.top,
                    mem, show.left, show.top, SRCCOPY);

                if (g_state != SEL_IDLE && (!g_ann.Empty() || g_ann.IsDrawing()))
                    g_ann.Render(hdc, mem, 0, 0, show);   // mem đang chứa ảnh gốc cho mosaic
            }
            SelectObject(mem, old);
            DeleteDC(mem);

            if (has) DrawSelectionDecor(hdc, show);
            if (has && (g_state == SEL_DONE || g_state == SEL_ANNOTATING))
                g_toolbar.Paint(hdc);
        }
        EndPaint(hWnd, &ps);
        return 0;
    }

    case WM_LBUTTONDOWN:
        HandleLButtonDown(hWnd, { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) });
        return 0;

    case WM_LBUTTONDBLCLK: {
        POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        // Chỉ khi không chọn công cụ vẽ thì double-click mới là "copy"
        if (g_state == SEL_DONE && g_ann.Tool() == TOOL_NONE && PtInRect(&g_sel, pt))
            ActionCopy();
        else
            HandleLButtonDown(hWnd, pt);
        return 0;
    }

    case WM_MOUSEMOVE: {
        POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };

        if (g_state == SEL_IDLE) {
            RECT old = g_hoverRect;
            if (UpdateHover(pt)) {
                RECT u;
                UnionRect(&u, &old, &g_hoverRect);
                InvalidateShow(hWnd, u);
            }
        }
        else if (g_state == SEL_DRAGGING) {
            int x = ClampI(pt.x, 0, g_vw);
            int y = ClampI(pt.y, 0, g_vh);

            RECT old = g_sel;
            g_sel.left = MinI(g_start.x, x);
            g_sel.right = MaxI(g_start.x, x);
            g_sel.top = MinI(g_start.y, y);
            g_sel.bottom = MaxI(g_start.y, y);

            RECT dirty;
            UnionRect(&dirty, &old, &g_sel);
            InvalidateShow(hWnd, dirty);
        }
        else if (g_state == SEL_MOVING || g_state == SEL_RESIZING) {
            RECT old = g_sel;
            g_sel = g_editor.Update(pt, g_vw, g_vh);

            RECT dirty;
            UnionRect(&dirty, &old, &g_sel);
            InvalidateShow(hWnd, dirty);
        }
        else if (g_state == SEL_ANNOTATING) {
            pt = ClampToSel(pt);
            if (g_ann.Tool() == TOOL_ERASER) {
                if (g_ann.EraseAt(pt)) InvalidateSel();
            }
            else {
                RECT d = g_ann.UpdateStroke(pt);
                if (!IsRectEmpty(&d)) InvalidateRect(hWnd, &d, FALSE);
            }
        }
        else if (g_state == SEL_DONE) {
            if (g_toolbar.SetHover(pt))
                InvalidateToolbar(hWnd);
        }
        return 0;
    }

    case WM_MOUSEWHEEL: {
        int delta = GET_WHEEL_DELTA_WPARAM(wParam);

        if (g_state == SEL_IDLE && !g_chain.empty()) {
            // Đang nhận diện ô: lên = ô cha (lớn hơn), xuống = ô con (nhỏ hơn)
            int nl = ClampI(g_level + (delta > 0 ? 1 : -1),
                0, static_cast<int>(g_chain.size()) - 1);
            if (nl != g_level) {
                g_level = nl;
                RECT old = g_hoverRect;
                g_hoverRect = g_chain[g_level];

                RECT u;
                UnionRect(&u, &old, &g_hoverRect);
                InvalidateShow(hWnd, u);
            }
        }
        else if (g_state == SEL_DONE &&
            g_ann.Tool() != TOOL_NONE && g_ann.Tool() != TOOL_ERASER) {
            // Đang chọn công cụ vẽ: lăn chuột đổi độ dày nét / cỡ chữ
            g_ann.SetWidth(g_ann.Width() + (delta > 0 ? 1 : -1));
            InvalidateLabel();
        }
        return 0;
    }

    case WM_LBUTTONUP:
        if (g_state == SEL_DRAGGING) {
            ReleaseCapture();
            int w = g_sel.right - g_sel.left;
            int h = g_sel.bottom - g_sel.top;

            if (w < 3 || h < 3) {
                // Chỉ click, không kéo: chọn luôn ô đang nhận diện
                POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
                UpdateHover(pt);

                if (!IsRectEmpty(&g_hoverRect)) {
                    g_sel = g_hoverRect;
                    g_state = SEL_DONE;
                    g_toolbar.Layout(g_sel, g_vw, g_vh);
                    g_toolbar.Show(true);
                }
                else {
                    g_sel = {};
                    g_state = SEL_IDLE;
                }
                InvalidateRect(hWnd, nullptr, FALSE);
            }
            else {
                g_state = SEL_DONE;
                g_toolbar.Layout(g_sel, g_vw, g_vh);
                g_toolbar.Show(true);
                InvalidateShow(hWnd, g_sel);
                InvalidateToolbar(hWnd);
            }
        }
        else if (g_state == SEL_MOVING || g_state == SEL_RESIZING) {
            ReleaseCapture();
            g_state = SEL_DONE;
            g_toolbar.Layout(g_sel, g_vw, g_vh);   // toolbar theo vùng chọn mới
            g_toolbar.Show(true);
            InvalidateShow(hWnd, g_sel);
            InvalidateToolbar(hWnd);
        }
        else if (g_state == SEL_ANNOTATING) {
            ReleaseCapture();
            if (g_ann.Tool() == TOOL_ERASER) g_ann.EndErase();
            else                             g_ann.EndStroke();
            g_state = SEL_DONE;
            RefreshToolbarState();
            InvalidateSel();
        }
        return 0;

    case WM_RBUTTONDOWN:
        CloseOverlay();
        return 0;

    case WM_KEYDOWN: {
        bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
        bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;

        if (wParam == VK_ESCAPE) { HandleCancel(); return 0; }

        if (g_state == SEL_DONE) {
            if (wParam == VK_RETURN) { ActionCopy(); return 0; }
            if (wParam == VK_F3) { ActionPin();  return 0; }
            if (ctrl && wParam == 'S') { ActionSave(); return 0; }
            if (ctrl && wParam == 'C') { ActionCopy(); return 0; }
            if (ctrl && shift && wParam == 'Z') { DoRedo();     return 0; }
            if (ctrl && wParam == 'Z') { DoUndo();     return 0; }
            if (ctrl && wParam == 'Y') { DoRedo();     return 0; }
            if (!ctrl && wParam >= '1' && wParam <= '7') {
                SetColorIndex(static_cast<int>(wParam - '1'));
                return 0;
            }
        }
        return 0;
    }
    }
    return DefWindowProcW(hWnd, msg, wParam, lParam);
}

static void StartCapture()
{
    if (g_hOverlay) return;   // đang mở rồi

    g_hFrozen = GrabScreen();
    g_hDimmed = MakeDimmed(g_hFrozen);

    // Phải chụp danh sách ô TRƯỚC khi overlay xuất hiện, nếu không overlay sẽ che mất cửa sổ thật
    g_detector.Snapshot(g_vx, g_vy, g_vw, g_vh);

    g_state = SEL_IDLE;
    g_sel = {};
    g_hoverRect = {};
    g_chain.clear();
    g_level = 0;
    g_toolbar.Show(false);
    g_toolbar.ClearHover();

    g_hOverlay = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW,
        L"MySnipOverlay", L"", WS_POPUP,
        g_vx, g_vy, g_vw, g_vh,
        nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);

    if (!g_hOverlay) { CloseOverlay(); return; }

    // Nhận diện ngay ô dưới con trỏ, không cần chờ rê chuột
    POINT cp;
    GetCursorPos(&cp);
    cp.x -= g_vx;
    cp.y -= g_vy;
    UpdateHover(cp);
    RequestUia(cp);
    g_uiaCache.clear();
    g_uiaTop = {};

    ShowWindow(g_hOverlay, SW_SHOW);
    SetForegroundWindow(g_hOverlay);
    SetFocus(g_hOverlay);
    UpdateWindow(g_hOverlay);
}

// ===== Low-Level Keyboard Hook =====
static LRESULT CALLBACK LowLevelKeyboardProc(int nCode, WPARAM wParam, LPARAM lParam)
{
    if (nCode == HC_ACTION && (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN)) {
        KBDLLHOOKSTRUCT* pKb = reinterpret_cast<KBDLLHOOKSTRUCT*>(lParam);

        bool ctrl = (GetAsyncKeyState(VK_LCONTROL) & 0x8000) ||
            (GetAsyncKeyState(VK_RCONTROL) & 0x8000);
        bool alt = (GetAsyncKeyState(VK_LMENU) & 0x8000) ||
            (GetAsyncKeyState(VK_RMENU) & 0x8000);

        // ESC khi overlay đang mở → hủy (ô chữ hoặc cả overlay)
        if (g_hOverlay && pKb->vkCode == VK_ESCAPE) {
            PostMessageW(g_hWnd, WM_MY_CANCEL, 0, 0);
            return 1;
        }

        // F1 → chụp
        if (pKb->vkCode == VK_F1 && !ctrl && !alt) {
            PostMessageW(g_hWnd, WM_MY_CAPTURE, 0, 0);
            return 1;
        }

        // Ctrl + Alt + ` → thoát chương trình
        if (pKb->vkCode == VK_OEM_3 && ctrl && alt) {
            PostMessageW(g_hWnd, WM_MY_QUIT, 0, 0);
            return 1;
        }
    }
    return CallNextHookEx(g_hKeyboardHook, nCode, wParam, lParam);
}

// ===== Icon khay hệ thống =====
static void AddTrayIcon(HWND hWnd)
{
    g_nid = {};
    g_nid.cbSize = sizeof(g_nid);
    g_nid.hWnd = hWnd;
    g_nid.uID = 1;
    g_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    g_nid.uCallbackMessage = WM_MY_TRAY;
    g_nid.hIcon = LoadIconW(nullptr, IDI_APPLICATION);   // sau này thay bằng icon riêng
    wcscpy_s(g_nid.szTip, L"MySnip - F1 để chụp");
    Shell_NotifyIconW(NIM_ADD, &g_nid);
}

static void RemoveTrayIcon()
{
    Shell_NotifyIconW(NIM_DELETE, &g_nid);
}

static void ShowTrayMenu(HWND hWnd)
{
    POINT pt;
    GetCursorPos(&pt);

    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING, ID_TRAY_CAPTURE, L"Chụp màn hình (F1)");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, ID_TRAY_EXIT, L"Thoát");

    SetForegroundWindow(hWnd);   // bắt buộc, nếu không menu không tự đóng khi bấm ra ngoài
    int cmd = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y, 0, hWnd, nullptr);
    PostMessageW(hWnd, WM_NULL, 0, 0);
    DestroyMenu(menu);

    if (cmd == ID_TRAY_CAPTURE)   PostMessageW(hWnd, WM_MY_CAPTURE, 0, 0);
    else if (cmd == ID_TRAY_EXIT) PostQuitMessage(0);
}

// ===== Chỉ cho một bản chạy: bản mới yêu cầu bản cũ thoát rồi thay thế =====
static bool EnsureSingleInstance()
{
    g_hMutex = CreateMutexW(nullptr, TRUE, L"MySnip_SingleInstance_Mutex");
    if (GetLastError() != ERROR_ALREADY_EXISTS)
        return true;                                   // không có bản cũ

    HWND old = FindWindowW(L"MySnipHidden", nullptr);
    if (old) PostMessageW(old, WM_MY_QUIT, 0, 0);      // nhờ bản cũ tự thoát

    // Chờ tối đa 3 giây để bản cũ thoát hẳn (mutex được nhả ra)
    DWORD r = WaitForSingleObject(g_hMutex, 3000);
    if (r == WAIT_OBJECT_0 || r == WAIT_ABANDONED)
        return true;

    CloseHandle(g_hMutex);
    g_hMutex = nullptr;
    return false;
}

LRESULT CALLBACK WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg) {
    case WM_MY_CAPTURE:
        StartCapture();
        return 0;
    case WM_MY_CANCEL:
        HandleCancel();
        return 0;
    case WM_MY_QUIT:
        PostQuitMessage(0);
        return 0;
    case WM_MY_TRAY:
        if (lParam == WM_RBUTTONUP || lParam == WM_CONTEXTMENU)
            ShowTrayMenu(hWnd);
        else if (lParam == WM_LBUTTONDBLCLK)
            StartCapture();
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }

    // Explorer bị khởi động lại thì icon khay mất, phải thêm lại
    if (g_msgTaskbarCreated != 0 && msg == g_msgTaskbarCreated) {
        AddTrayIcon(hWnd);
        return 0;
    }
    return DefWindowProcW(hWnd, msg, wParam, lParam);
}

int APIENTRY wWinMain(_In_ HINSTANCE hInst, _In_opt_ HINSTANCE, _In_ LPWSTR, _In_ int)
{
    SetProcessDPIAware();

    if (!EnsureSingleInstance()) {
        MessageBoxW(nullptr, L"Không thay thế được bản MySnip đang chạy.\n"
            L"Hãy tắt MySnip.exe trong Task Manager.",
            L"MySnip", MB_ICONERROR);
        return 1;
    }
    g_msgTaskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");

    GdiplusStartupInput gdiInput;
    ULONG_PTR gdiToken;
    GdiplusStartup(&gdiToken, &gdiInput, nullptr);

    g_uiFont = CreateFontW(-14, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");

    BuildToolbar();

    // Lớp cửa sổ ẩn
    WNDCLASSW wc = {};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    g_uia.Start();
    wc.lpszClassName = L"MySnipHidden";
    RegisterClassW(&wc);

    // Lớp cửa sổ overlay
    WNDCLASSW oc = {};
    oc.style = CS_DBLCLKS;
    oc.lpfnWndProc = OverlayProc;
    oc.hInstance = hInst;
    oc.hCursor = LoadCursorW(nullptr, IDC_CROSS);
    oc.lpszClassName = L"MySnipOverlay";
    RegisterClassW(&oc);

    // Lớp cửa sổ ghim
    WNDCLASSW pc = {};
    pc.style = CS_DBLCLKS;
    pc.lpfnWndProc = PinProc;
    pc.hInstance = hInst;
    pc.hCursor = LoadCursorW(nullptr, IDC_SIZEALL);
    pc.lpszClassName = L"MySnipPin";
    RegisterClassW(&pc);

    // Cửa sổ ẩn thường (không dùng HWND_MESSAGE): vô hình nhưng FindWindow tìm được
    g_hWnd = CreateWindowExW(WS_EX_TOOLWINDOW, wc.lpszClassName, L"MySnip", WS_POPUP,
        0, 0, 0, 0, nullptr, nullptr, hInst, nullptr);
    if (!g_hWnd) {
        g_uia.Shutdown();
        GdiplusShutdown(gdiToken);
        return 1;
    }

    g_hKeyboardHook = SetWindowsHookExW(WH_KEYBOARD_LL, LowLevelKeyboardProc, hInst, 0);
    if (!g_hKeyboardHook) {
        MessageBoxW(nullptr, L"Không thể cài đặt keyboard hook.", L"MySnip", MB_ICONERROR);
        DestroyWindow(g_hWnd);
        GdiplusShutdown(gdiToken);
        return 1;
    }

    AddTrayIcon(g_hWnd);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    RemoveTrayIcon();
    CloseOverlay();
    UnhookWindowsHookEx(g_hKeyboardHook);
    DeleteObject(g_uiFont);
    GdiplusShutdown(gdiToken);

    if (g_hMutex) {
        ReleaseMutex(g_hMutex);
        CloseHandle(g_hMutex);
    }
    return 0;
}

