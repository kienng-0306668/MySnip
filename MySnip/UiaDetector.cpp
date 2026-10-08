#include "UiaDetector.h"
#include <UIAutomation.h>
#include <dwmapi.h>
#include <wrl/client.h>
#include <climits>
#include <cwchar>
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "oleaut32.lib")

using Microsoft::WRL::ComPtr;

namespace {

    // ---- Tìm cửa sổ nằm dưới con trỏ (theo thứ tự Z, bỏ qua overlay) ----
    struct FindCtx { POINT pt; HWND skip; HWND found; RECT rect; };

    BOOL CALLBACK FindProc(HWND hwnd, LPARAM lp)
    {
        auto* c = reinterpret_cast<FindCtx*>(lp);

        if (hwnd == c->skip) return TRUE;
        if (!IsWindowVisible(hwnd) || IsIconic(hwnd)) return TRUE;

        LONG_PTR ex = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
        if (ex & WS_EX_TRANSPARENT) return TRUE;

        DWORD cloaked = 0;
        DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, &cloaked, sizeof(cloaked));
        if (cloaked) return TRUE;

        wchar_t cls[64] = L"";
        GetClassNameW(hwnd, cls, 64);
        if (wcscmp(cls, L"Progman") == 0 || wcscmp(cls, L"WorkerW") == 0) return TRUE;

        RECT r;
        if (FAILED(DwmGetWindowAttribute(hwnd, DWMWA_EXTENDED_FRAME_BOUNDS, &r, sizeof(r))))
            GetWindowRect(hwnd, &r);
        if (!PtInRect(&r, c->pt)) return TRUE;

        c->found = hwnd;
        c->rect = r;
        return FALSE;   // dừng duyệt
    }

    // ---- Đi xuống cây UIA, ở mỗi tầng chọn phần tử con (nhỏ nhất) chứa con trỏ ----
    void Resolve(IUIAutomation* uia, IUIAutomationTreeWalker* walker,
        POINT pt, HWND skip, std::vector<RECT>& out)
    {
        out.clear();

        FindCtx fc = { pt, skip, nullptr,{} };
        EnumWindows(FindProc, reinterpret_cast<LPARAM>(&fc));
        if (!fc.found) return;

        // Cửa sổ của chính MySnip (ảnh ghim...) thì để Win32 xử lý
        DWORD pid = 0;
        GetWindowThreadProcessId(fc.found, &pid);
        if (pid == GetCurrentProcessId()) return;

        ComPtr<IUIAutomationElement> cur;
        if (FAILED(uia->ElementFromHandle(fc.found, &cur)) || !cur.Get()) return;

        std::vector<RECT> down;   // từ lớn xuống nhỏ

        for (int depth = 0; depth < 40; ++depth) {
            ComPtr<IUIAutomationElement> best;
            RECT bestRect = {};
            long long bestArea = LLONG_MAX;

            ComPtr<IUIAutomationElement> child;
            if (FAILED(walker->GetFirstChildElement(cur.Get(), &child))) break;

            // Giới hạn 500 phần tử anh em mỗi tầng để danh sách khổng lồ không làm chậm
            for (int n = 0; child.Get() != nullptr && n < 500; ++n) {
                BOOL off = FALSE;
                RECT r = {};
                if (SUCCEEDED(child->get_CurrentIsOffscreen(&off)) && !off &&
                    SUCCEEDED(child->get_CurrentBoundingRectangle(&r)) &&
                    PtInRect(&r, pt))
                {
                    long long a = static_cast<long long>(r.right - r.left) * (r.bottom - r.top);
                    if (a > 0 && a < bestArea) {
                        bestArea = a;
                        bestRect = r;
                        best = child;
                    }
                }

                ComPtr<IUIAutomationElement> next;
                if (FAILED(walker->GetNextSiblingElement(child.Get(), &next))) break;
                child = next;
            }

            if (!best.Get()) break;
            down.push_back(bestRect);
            cur = best;
        }

        out.assign(down.rbegin(), down.rend());   // đảo thành nhỏ -> lớn
    }

} // namespace

// =====================================================================
bool UiaDetector::Start()
{
    if (th_.joinable()) return true;
    {
        std::lock_guard<std::mutex> lk(m_);
        quit_ = false;
        has_ = false;
    }
    th_ = std::thread(&UiaDetector::Worker, this);
    return true;
}

void UiaDetector::Shutdown()
{
    if (!th_.joinable()) return;
    {
        std::lock_guard<std::mutex> lk(m_);
        quit_ = true;
    }
    cv_.notify_one();
    th_.join();
}

unsigned UiaDetector::Request(POINT screenPt, HWND notify, UINT msg)
{
    std::lock_guard<std::mutex> lk(m_);
    ++gen_;
    req_.pt = screenPt;
    req_.notify = notify;
    req_.msg = msg;
    req_.gen = gen_;
    has_ = true;
    cv_.notify_one();
    return gen_;
}

bool UiaDetector::TakeResult(unsigned gen, std::vector<RECT>& chain)
{
    std::lock_guard<std::mutex> lk(m_);
    if (gen != gen_ || resGen_ != gen) return false;   // đã có yêu cầu mới hơn
    chain = res_;
    return true;
}

void UiaDetector::Cancel()
{
    std::lock_guard<std::mutex> lk(m_);
    ++gen_;       // vô hiệu hóa mọi kết quả cũ
    has_ = false;
}

void UiaDetector::Worker()
{
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    {
        ComPtr<IUIAutomation> uia;
        ComPtr<IUIAutomationTreeWalker> walker;

        HRESULT hr = CoCreateInstance(__uuidof(CUIAutomation), nullptr,
            CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&uia));
        if (SUCCEEDED(hr) && uia.Get()) {
            // Ứng dụng đích bị treo thì bỏ cuộc sau 300 ms thay vì đứng chờ
            ComPtr<IUIAutomation2> uia2;
            if (SUCCEEDED(uia.As(&uia2)) && uia2.Get()) {
                uia2->put_ConnectionTimeout(300);
                uia2->put_TransactionTimeout(300);
            }
            uia->get_ControlViewWalker(&walker);
        }

        for (;;) {
            Req rq;
            {
                std::unique_lock<std::mutex> lk(m_);
                cv_.wait(lk, [this] { return quit_ || has_; });
                if (quit_) break;
                rq = req_;
                has_ = false;
            }

            std::vector<RECT> chain;
            if (uia.Get() && walker.Get())
                Resolve(uia.Get(), walker.Get(), rq.pt, rq.notify, chain);

            {
                std::lock_guard<std::mutex> lk(m_);
                res_ = std::move(chain);
                resGen_ = rq.gen;
            }
            if (rq.notify && IsWindow(rq.notify))
                PostMessageW(rq.notify, rq.msg, static_cast<WPARAM>(rq.gen), 0);
        }

        walker.Reset();   // phải giải phóng COM trước khi CoUninitialize
        uia.Reset();
    }
    CoUninitialize();
}