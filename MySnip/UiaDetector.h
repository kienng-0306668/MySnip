#pragma once
#include <windows.h>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <vector>

// Nhận diện phần tử giao diện (nút, tab, ô nhập...) bằng UI Automation.
// Mọi lời gọi UIA chạy ở một luồng nền riêng để không làm đơ overlay.
// Tọa độ vào/ra đều là tọa độ MÀN HÌNH.
class UiaDetector
{
public:
    UiaDetector() = default;
    ~UiaDetector() { Shutdown(); }
    UiaDetector(const UiaDetector&) = delete;
    UiaDetector& operator=(const UiaDetector&) = delete;

    bool Start();       // tạo luồng nền, gọi một lần lúc khởi động chương trình
    void Shutdown();    // dừng luồng nền, gọi lúc thoát

    // Gửi yêu cầu bất đồng bộ. Khi xong, luồng nền gọi PostMessage(notify, msg, gen, 0).
    // 'notify' đồng thời là cửa sổ overlay: sẽ bị bỏ qua khi tìm cửa sổ nằm dưới con trỏ.
    // Trả về số thứ tự (gen) của yêu cầu.
    unsigned Request(POINT screenPt, HWND notify, UINT msg);

    // Lấy kết quả nếu 'gen' vẫn là yêu cầu mới nhất. chain: từ phần tử NHỎ đến LỚN.
    bool TakeResult(unsigned gen, std::vector<RECT>& chain);

    // Hủy mọi yêu cầu đang chờ và vô hiệu hóa kết quả cũ
    void Cancel();

private:
    struct Req
    {
        POINT    pt = {};
        HWND     notify = nullptr;
        UINT     msg = 0;
        unsigned gen = 0;
    };

    void Worker();

    std::thread             th_;
    std::mutex              m_;
    std::condition_variable cv_;
    bool                    quit_ = false;
    bool                    has_ = false;
    Req                     req_;
    unsigned                gen_ = 0;     // số thứ tự yêu cầu mới nhất
    unsigned                resGen_ = 0;     // số thứ tự của kết quả đang lưu
    std::vector<RECT>       res_;
};
