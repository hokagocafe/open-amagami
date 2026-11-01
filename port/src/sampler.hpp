#pragma once
// AMG_SAMPLE=FILE: where the machine's thread spends its time. A helper reads that thread's instruction pointer every millisecond
// (the thread that makes the Sampler) and, when the Sampler ends, writes the addresses (minus the module's base) to FILE;
// tools/sample_map.py names them from the linker's map file. Windows only; elsewhere it does nothing.
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <mmsystem.h>
#pragma comment(lib, "winmm.lib")
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <vector>

namespace amg {

class Sampler {
public:
    Sampler() {
        file_ = std::getenv("AMG_SAMPLE");
        if (!file_) return;
        DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &self_, 0, FALSE, DUPLICATE_SAME_ACCESS);
        running_ = true;
        thread_ = std::thread([this] {
            timeBeginPeriod(1);
            const uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
            while (running_) {
                Sleep(1);
                if (SuspendThread(self_) == DWORD(-1)) continue;
                CONTEXT ctx;
                ctx.ContextFlags = CONTEXT_CONTROL;
                if (GetThreadContext(self_, &ctx)) samples_.push_back(
#ifdef _WIN64
                    static_cast<unsigned long>(ctx.Rip - base));
#else
                    static_cast<unsigned long>(ctx.Eip - base));
#endif
                ResumeThread(self_);
                if (samples_.size() % 500 == 0) write();  // the window is often killed, not closed
            }
            timeEndPeriod(1);
        });
    }
    ~Sampler() {
        if (!file_) return;
        running_ = false;
        thread_.join();
        write();
    }

private:
    void write() {
        if (std::FILE* f = std::fopen(file_, "w")) {
            for (unsigned long a : samples_) std::fprintf(f, "%lx\n", a);
            std::fclose(f);
        }
    }
    const char* file_ = nullptr;
    HANDLE self_ = nullptr;
    std::atomic<bool> running_{false};
    std::thread thread_;
    std::vector<unsigned long> samples_;
};

}  // namespace amg
#else
namespace amg {
struct Sampler {};
}  // namespace amg
#endif
