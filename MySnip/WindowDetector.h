#pragma once
#include <windows.h>
#include <vector>

// Chụp "danh sách ô" trên màn hình tại một thời điểm, rồi tra cứu rất nhanh theo tọa độ.
// Toàn bộ tọa độ lưu theo hệ của overlay (gốc = góc trên-trái màn hình ảo).
class WindowDetector
{
public:
    struct Entry
    {
        RECT              rect = {};
        std::vector<RECT> children;   // các ô con (control) bên trong cửa sổ
    };

    // Gọi TRƯỚC khi overlay xuất hiện (nếu không overlay sẽ che mất cửa sổ thật)
    void Snapshot(int originX, int originY, int width, int height);
    void Clear() { entries_.clear(); }

    // Trả về chuỗi các ô chứa điểm pt, từ NHỎ nhất đến LỚN nhất (cửa sổ ngoài cùng ở cuối)
    bool Detect(POINT pt, std::vector<RECT>& chain) const;

private:
    std::vector<Entry> entries_;
};