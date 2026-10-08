#pragma once
#include <windows.h>
#include <string>
#include <vector>
#include "Commands.h"

// Toàn bộ kích thước và màu sắc nằm ở đây: muốn đổi giao diện chỉ sửa struct này
struct ToolbarTheme
{
    COLORREF background = RGB(255, 250, 250);
    COLORREF border     = RGB(200, 200, 200);
    COLORREF button     = RGB(255, 250, 250);
    COLORREF buttonHover = RGB(0, 120, 215);
    COLORREF buttonChecked = RGB(0, 174, 255);
    COLORREF buttonDisabled = RGB(48, 48, 48);
    COLORREF text = RGB(53, 62, 67);
    COLORREF textDisabled = RGB(120, 120, 120);
    COLORREF separator = RGB(100, 100, 100);

    int buttonW = 32;   // chiều rộng nút mặc định
    int buttonH = 32;   // chiều cao nút mặc định
    int gap = 4;    // khoảng cách giữa các phần tử
    int padding = 1;    // lề trong toolbar
    int separatorW = 1;    // bề rộng đường phân cách
    int margin = 2;    // khoảng cách giữa toolbar và vùng chọn
};

class Toolbar
{
public:
    explicit Toolbar(HFONT font = nullptr) : font_(font) {}

    // --- Cấu hình ---
    void SetFont(HFONT font) { font_ = font; }
    void SetTheme(const ToolbarTheme& t) { theme_ = t; }
    void AddButton(int id, const wchar_t* label, int width = 0);   // width = 0: dùng mặc định
    void AddIconButton(int id, ToolbarIcon icon, int width = 0);
    void AddSeparator();
    void Clear();

    // --- Trạng thái nút ---
    void SetEnabled(int id, bool enabled);
    void SetChecked(int id, bool checked);   // nút dạng bật/tắt (công cụ vẽ...)
    void SetIcon(int id, ToolbarIcon icon);
    void SetBadge(int id, bool show, COLORREF color);

    // --- Hiển thị ---
    // anchor: vùng chọn; boundsW/H: kích thước màn hình ảo để toolbar không tràn ra ngoài
    void Layout(const RECT& anchor, int boundsW, int boundsH);
    void Show(bool visible) { visible_ = visible; }
    bool IsVisible() const { return visible_; }
    RECT Bounds() const { return bounds_; }
    void Paint(HDC hdc) const;

    // --- Chuột ---
    bool Contains(POINT pt) const;   // chuột có nằm trong toolbar không (kể cả khoảng trống)
    int  HitTest(POINT pt) const;    // trả về ID nút bị trúng, 0 nếu không (bỏ qua nút bị khóa)
    bool SetHover(POINT pt);         // cập nhật nút đang rê chuột; true nếu có thay đổi
    void ClearHover() { hoverId_ = 0; }

private:
    struct Item
    {
        int          id = 0;
        std::wstring label;
        ToolbarIcon  icon = IC_NONE;
        int          width = 0;
        bool         separator = false;
        bool         enabled = true;
        bool         checked = false;
        bool         badge = false;
        COLORREF     badgeColor = RGB(255, 0, 0);
        RECT         rect = {};
    };

    Item* Find(int id);

    std::vector<Item> items_;
    ToolbarTheme      theme_;
    HFONT             font_ = nullptr;
    RECT              bounds_ = {};
    bool              visible_ = false;
    int               hoverId_ = 0;
};