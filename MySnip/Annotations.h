#pragma once
#include <windows.h>
#include <memory>
#include <string>
#include <vector>

enum ToolType
{
    TOOL_NONE = 0,
    TOOL_RECT,      // chữ nhật hoặc elip (xem cờ ellipse)
    TOOL_ARROW,
    TOOL_PEN,
    TOOL_MARKER,
    TOOL_MOSAIC,
    TOOL_TEXT,
    TOOL_ERASER
};

// Một nét chú thích đã hoàn thành. Tọa độ theo hệ của overlay.
struct Annotation
{
    ToolType           tool = TOOL_NONE;
    bool               ellipse = false;
    POINT              a = {};     // RECT/ARROW/MOSAIC: điểm đầu; TEXT: góc trên-trái
    POINT              b = {};     // RECT/ARROW/MOSAIC: điểm cuối
    std::vector<POINT> pts;              // PEN/MARKER
    COLORREF           color = RGB(255, 59, 48);
    int                width = 3;
    std::wstring       text;
    int                fontPx = 0;
    SIZE               textSize = {};
};

class Annotations
{
public:
    typedef std::shared_ptr<const Annotation> Ptr;

    // --- Công cụ đang chọn ---
    void     SetTool(ToolType t) { tool_ = t; }
    ToolType Tool() const { return tool_; }
    void     SetEllipse(bool e) { ellipse_ = e; }
    bool     Ellipse() const { return ellipse_; }
    void     SetColor(COLORREF c) { color_ = c; }
    COLORREF Color() const { return color_; }
    void     SetWidth(int w);                  // 1..20
    int      Width() const { return width_; }
    int      FontPx() const;                   // cỡ chữ suy ra từ độ dày

    // --- Vẽ một nét (các hàm trả về vùng cần vẽ lại) ---
    RECT BeginStroke(POINT pt);
    RECT UpdateStroke(POINT pt);
    void EndStroke();
    bool IsDrawing() const { return drawing_; }

    // --- Tẩy ---
    bool EraseAt(POINT pt);                    // true nếu xóa được gì đó
    void EndErase() { erasing_ = false; }

    // --- Chữ ---
    void AddText(POINT pos, const std::wstring& text);

    // --- Lịch sử ---
    bool CanUndo() const { return !undo_.empty(); }
    bool CanRedo() const { return !redo_.empty(); }
    void Undo();
    void Redo();
    bool Empty() const { return items_.empty(); }
    void Reset();                              // xóa hết, bỏ chọn công cụ (giữ màu và độ dày)

    // --- Vẽ ---
    // dst: nơi vẽ; base: ảnh gốc (cho mosaic, tọa độ tuyệt đối);
    // đích = tọa độ tuyệt đối - (offX, offY); clip tính theo tọa độ đích.
    void Render(HDC dst, HDC base, int offX, int offY, const RECT& clip) const;

    static HFONT MakeFont(int px);             // người gọi tự DeleteObject

private:
    void Commit(const Ptr& p);
    void PushUndo() { undo_.push_back(items_); }

    ToolType tool_ = TOOL_NONE;
    bool     ellipse_ = false;
    COLORREF color_ = RGB(255, 59, 48);
    int      width_ = 3;

    std::vector<Ptr>              items_;
    std::vector<std::vector<Ptr>> undo_;
    std::vector<std::vector<Ptr>> redo_;

    Annotation live_;           // nét đang vẽ dở
    bool       drawing_ = false;
    bool       erasing_ = false;
};
