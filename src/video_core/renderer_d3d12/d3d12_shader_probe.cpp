// SPDX-FileCopyrightText: Copyright 2026 NSPX
// SPDX-License-Identifier: GPL-3.0-or-later
#include "video_core/renderer_d3d12/d3d12_shader_probe.h"

#include <chrono>
#include <condition_variable>
#include <fstream>
#include <iterator>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <utility>
#include <fmt/format.h>
#include <windows.h>
#include "common/scope_exit.h"
#include "common/thread.h"
#include "video_core/renderer_d3d12/d3d12_context.h"
#include "video_core/renderer_d3d12/d3d12_shader.h"
#ifdef NXBOX_UWP
#include <winrt/Windows.ApplicationModel.Core.h>
#include <winrt/Windows.Storage.h>
#include <winrt/Windows.System.h>
#endif

namespace D3D12 {
namespace {
using Clock = std::chrono::steady_clock;

double Milliseconds(Clock::time_point start) {
    return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}

std::string Memory() {
#ifdef NXBOX_UWP
    return fmt::format("{:.1f}",
                       winrt::Windows::System::MemoryManager::AppMemoryUsage() / (1024.0 * 1024.0));
#else
    return {};
#endif
}

struct PsoState {
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12RootSignature> root;
    std::vector<u8> dxil;
    ComPtr<ID3D12PipelineState> pso;
    std::mutex mutex;
    std::condition_variable ready;
    bool done{};
    HRESULT hr{E_PENDING};
    double ms{};
};

void ProbeFile(ID3D12Device* device, ShaderCompiler& compiler, const std::filesystem::path& file,
               int timeout_s, ShaderProbe::Counts& counts) {
    const auto before = Memory();
    std::string source, error, status{"dxc_fail"}, pso_ms{"-"};
    size_t dxil_bytes{};
    double dxc_ms{};
    s32 hr{E_FAIL};
    auto result = ShaderProbe::Result::Fail;
    try {
        std::ifstream input{file, std::ios::binary};
        if (!input)
            throw std::runtime_error("Cannot open HLSL file");
        source.assign(std::istreambuf_iterator<char>{input}, {});
        if (input.bad())
            throw std::runtime_error("Cannot read HLSL file");
        const auto target = ShaderProbe::Target(source);
        const std::wstring wide_target{target.begin(), target.end()};
        const auto dxc_start = Clock::now();
        std::vector<u8> dxil;
        try {
            // Direct DXC invocation, deliberately bypassing DxcCache and all disk caches. A failure
            // also logs the DXC error text on its own "D3D12 DXC" line.
            dxil = compiler.Compile(source, L"main", wide_target.c_str());
        } catch (...) {
            dxc_ms = Milliseconds(dxc_start);
            throw;
        }
        dxc_ms = Milliseconds(dxc_start);
        dxil_bytes = dxil.size();
        status = "rs_fail";
        auto state = std::make_shared<PsoState>();
        state->device = device;
        state->dxil = std::move(dxil);
        // CreateRootSignature extracts the embedded RTS0 part from the DXIL container.
        hr = device->CreateRootSignature(0, state->dxil.data(), state->dxil.size(),
                                         IID_PPV_ARGS(&state->root));
        if (SUCCEEDED(hr)) {
            status = "pso_fail";
            const auto pso_start = Clock::now();
            std::thread worker{[state] {
                SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
                D3D12_COMPUTE_PIPELINE_STATE_DESC desc{};
                desc.pRootSignature = state->root.Get();
                desc.CS = {state->dxil.data(), state->dxil.size()};
                const auto start = Clock::now();
                const auto hr =
                    state->device->CreateComputePipelineState(&desc, IID_PPV_ARGS(&state->pso));
                const auto ms = Milliseconds(start);
                {
                    const std::lock_guard lock{state->mutex};
                    state->hr = hr;
                    state->ms = ms;
                    state->done = true;
                }
                state->ready.notify_one();
            }};
            SCOPE_EXIT {
                if (worker.joinable())
                    worker.detach();
            };
            std::unique_lock lock{state->mutex};
            if (state->ready.wait_for(lock, std::chrono::seconds{timeout_s},
                                      [&] { return state->done; })) {
                hr = state->hr;
                const auto ms = state->ms;
                lock.unlock();
                worker.join();
                pso_ms = fmt::format("{:.1f}", ms);
                if (SUCCEEDED(hr)) {
                    status = "ok";
                    result = ShaderProbe::Result::Ok;
                }
            } else {
                lock.unlock();
                worker.detach();
                // Do not access state again. The driver thread retains every COM object and DXIL.
                status = "timeout";
                hr = HRESULT_FROM_WIN32(WAIT_TIMEOUT);
                pso_ms = fmt::format("{:.1f}", Milliseconds(pso_start));
                result = ShaderProbe::Result::Timeout;
            }
        }
    } catch (const std::exception& exception) {
        if (SUCCEEDED(hr))
            hr = E_FAIL;
        if (error.empty())
            error = exception.what();
    }
    counts.Record(result);
    auto line = fmt::format(
        "SHADER_PROBE file={} hlsl_bytes={} dxc_ms={:.1f} dxil_bytes={} pso_ms={} status={} "
        "hr={:#010x}",
        file.filename().string(), source.size(), dxc_ms, dxil_bytes, pso_ms, status,
        static_cast<u32>(hr));
#ifdef NXBOX_UWP
    line += fmt::format(" mem_mib={}->{}", before, Memory());
#else
    (void)before;
#endif
    if (status == "dxc_fail") {
        line += fmt::format(" error=\"{}\"", ShaderProbe::ErrorSnippet(error));
    }
    Diagnostic(line);
}

void Run(ComPtr<ID3D12Device> device, ShaderProbe::Config config,
         const std::shared_ptr<std::atomic<bool>>& stop) {
    ShaderProbe::Counts counts;
    try {
        Common::SetCurrentThreadName("D3D12 shader probe");
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
#ifdef NXBOX_UWP
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
        SCOPE_EXIT {
            winrt::uninit_apartment();
        };
        const auto suspending = winrt::Windows::ApplicationModel::Core::CoreApplication::Suspending(
            winrt::auto_revoke, [stop](const auto&, const auto&) { stop->store(true); });
        if (config.directory.is_relative()) {
            config.directory =
                std::filesystem::path{winrt::Windows::Storage::ApplicationData::Current()
                                          .LocalFolder()
                                          .Path()
                                          .c_str()} /
                config.directory;
        }
#endif
        const auto files = ShaderProbe::Files(config.directory);
        ShaderCompiler compiler;
        for (const auto& file : files) {
            if (stop->load())
                break;
            ProbeFile(device.Get(), compiler, file, config.timeout_s, counts);
            if (counts.Stopped()) {
                Diagnostic("SHADER_PROBE stopped after 4 timeouts");
                break;
            }
        }
    } catch (const std::exception& error) {
        Diagnostic(fmt::format("SHADER_PROBE setup failed: {}", error.what()));
    } catch (...) {
        Diagnostic("SHADER_PROBE setup failed: unknown exception");
    }
    Diagnostic(fmt::format("SHADER_PROBE done ok={} timeout={} fail={}", counts.ok, counts.timeout,
                           counts.fail));
}
} // namespace

std::shared_ptr<std::atomic<bool>> StartShaderProbe(ID3D12Device* device, const char* directory,
                                                    const char* timeout) noexcept {
    try {
        auto config = ShaderProbe::Config::Parse(directory, timeout);
        if (!config.Enabled())
            return {};
        static std::atomic_flag started{};
        if (started.test_and_set())
            return {};
        auto stop = std::make_shared<std::atomic<bool>>(false);
        std::thread{[device = ComPtr<ID3D12Device>{device}, config = std::move(config), stop] {
            Run(device, config, stop);
        }}.detach();
        return stop;
    } catch (...) {
        Diagnostic("SHADER_PROBE could not start worker");
        return {};
    }
}
} // namespace D3D12
