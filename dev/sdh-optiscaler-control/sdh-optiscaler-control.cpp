#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <bcrypt.h>
#include <dxgi.h>
#include <dxgi1_2.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "Config.h"
#include "State.h"

#pragma comment(lib, "bcrypt.lib")

namespace
{
constexpr uint32_t kProtocol = 1;
constexpr uintptr_t kConfigInstanceRva = 0x17C3D0;
constexpr uintptr_t kStateInstanceRva  = 0x03ECA0;
constexpr char kSupportedSha[] =
    "e903f639c05aaf9cda3bc6fd18d1f21b9fd8143b97dddd7f63146a53868aca6f";

using ConfigInstanceFn = Config* (__cdecl*)();
using StateInstanceFn = State& (__cdecl*)();
using PresentFn = HRESULT (STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);
using Present1Fn = HRESULT (STDMETHODCALLTYPE*)(IDXGISwapChain1*, UINT, UINT, const DXGI_PRESENT_PARAMETERS*);

HMODULE g_self = nullptr;
HMODULE g_opti = nullptr;
ConfigInstanceFn g_configInstance = nullptr;
StateInstanceFn g_stateInstance = nullptr;

struct HookRecord
{
    void** slot = nullptr;
    void* original = nullptr;
    int kind = 0; // 8 = Present, 22 = Present1
};

std::mutex g_hookMutex;
std::vector<HookRecord> g_hooks;
std::atomic<uint64_t> g_presentHits { 0 };
std::atomic<uint64_t> g_present1Hits { 0 };
std::atomic<bool> g_running { false };
std::atomic<uint64_t> g_lastCompletedSeq { 0 };
std::filesystem::path g_root;

struct Command
{
    uint64_t seq = 0;
    bool hasUpscaler = false;
    std::string upscaler;

    bool hasQuality = false;
    bool qualityEnabled = false;
    float qualityRatio = 1.0f;

    bool hasSharpness = false;
    bool sharpnessEnabled = false;
    float sharpness = 0.4f;

    bool hasFgEnabled = false;
    bool fgEnabled = false;

    bool hasHudFix = false;
    bool hudFix = false;

    bool hasHudLimit = false;
    int hudLimit = 1;
};

std::mutex g_commandMutex;
std::optional<Command> g_pending;
std::optional<Command> g_completed;
bool g_completedOk = false;
std::string g_completedError;

static std::string trim(std::string s)
{
    auto notSpace = [](unsigned char c) { return !std::isspace(c); };
    s.erase(s.begin(), std::find_if(s.begin(), s.end(), notSpace));
    s.erase(std::find_if(s.rbegin(), s.rend(), notSpace).base(), s.end());
    return s;
}

static bool parseBool(const std::string& value)
{
    auto s = value;
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return (char) std::tolower(c); });
    return s == "1" || s == "true" || s == "yes" || s == "on";
}

static std::map<std::string, std::string> readKv(const std::filesystem::path& path)
{
    std::map<std::string, std::string> out;
    std::ifstream in(path, std::ios::binary);
    std::string line;
    while (std::getline(in, line))
    {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        const auto pos = line.find('=');
        if (pos == std::string::npos)
            continue;
        out[trim(line.substr(0, pos))] = trim(line.substr(pos + 1));
    }
    return out;
}

static void atomicWrite(const std::filesystem::path& path, const std::string& text)
{
    const auto tmp = path.wstring() + L".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        out.write(text.data(), static_cast<std::streamsize>(text.size()));
        out.flush();
    }
    std::error_code ec;
    std::filesystem::remove(path, ec);
    ec.clear();
    std::filesystem::rename(tmp, path, ec);
}

static std::string sha256File(const std::filesystem::path& path)
{
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    DWORD objectLen = 0, hashLen = 0, cb = 0;
    std::vector<unsigned char> object;
    std::vector<unsigned char> digest;
    std::ifstream in(path, std::ios::binary);
    if (!in)
        return {};

    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0)
        return {};
    if (BCryptGetProperty(alg, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&objectLen),
                          sizeof(objectLen), &cb, 0) < 0 ||
        BCryptGetProperty(alg, BCRYPT_HASH_LENGTH, reinterpret_cast<PUCHAR>(&hashLen),
                          sizeof(hashLen), &cb, 0) < 0)
    {
        BCryptCloseAlgorithmProvider(alg, 0);
        return {};
    }
    object.resize(objectLen);
    digest.resize(hashLen);
    if (BCryptCreateHash(alg, &hash, object.data(), objectLen, nullptr, 0, 0) < 0)
    {
        BCryptCloseAlgorithmProvider(alg, 0);
        return {};
    }

    std::vector<unsigned char> buffer(1 << 20);
    while (in)
    {
        in.read(reinterpret_cast<char*>(buffer.data()), static_cast<std::streamsize>(buffer.size()));
        const auto got = in.gcount();
        if (got > 0 && BCryptHashData(hash, buffer.data(), static_cast<ULONG>(got), 0) < 0)
        {
            BCryptDestroyHash(hash);
            BCryptCloseAlgorithmProvider(alg, 0);
            return {};
        }
    }

    if (BCryptFinishHash(hash, digest.data(), hashLen, 0) < 0)
    {
        BCryptDestroyHash(hash);
        BCryptCloseAlgorithmProvider(alg, 0);
        return {};
    }

    BCryptDestroyHash(hash);
    BCryptCloseAlgorithmProvider(alg, 0);

    static constexpr char hex[] = "0123456789abcdef";
    std::string result;
    result.resize(digest.size() * 2);
    for (size_t i = 0; i < digest.size(); ++i)
    {
        result[i * 2] = hex[digest[i] >> 4];
        result[i * 2 + 1] = hex[digest[i] & 0x0f];
    }
    return result;
}

static bool matchMasked(const unsigned char* p, const unsigned char* bytes, const char* mask, size_t n)
{
    for (size_t i = 0; i < n; ++i)
        if (mask[i] == 'x' && p[i] != bytes[i])
            return false;
    return true;
}

static bool validateFunctions()
{
    if (!g_opti)
        return false;
    auto base = reinterpret_cast<unsigned char*>(g_opti);

    // Config::Instance(): sub rsp,28 / mov rax,[rip+...] / test rax / jne ...
    const unsigned char configSig[] = {
        0x48,0x83,0xEC,0x28,0x48,0x8B,0x05,0,0,0,0,0x48,0x85,0xC0,0x75
    };
    const char configMask[] = "xxxxxxx????xxxx";
    if (!matchMasked(base + kConfigInstanceRva, configSig, configMask, sizeof(configSig)))
        return false;

    // State::Instance(): push rbx / sub rsp,20 / mov ecx,[rip+...] / gs: mov ...
    const unsigned char stateSig[] = {
        0x40,0x53,0x48,0x83,0xEC,0x20,0x8B,0x0D,0,0,0,0,0x65,0x48,0x8B,0x04,0x25,0x58,0,0,0
    };
    const char stateMask[] = "xxxxxxxx????xxxxxxxxx";
    if (!matchMasked(base + kStateInstanceRva, stateSig, stateMask, sizeof(stateSig)))
        return false;

    g_configInstance = reinterpret_cast<ConfigInstanceFn>(base + kConfigInstanceRva);
    g_stateInstance = reinterpret_cast<StateInstanceFn>(base + kStateInstanceRva);
    return true;
}

static std::string currentConfiguredBackend(Config* cfg, State& state)
{
    switch (state.api)
    {
    case API::DX11:
        return cfg->Dx11Upscaler.value_or_default();
    case API::DX12:
        return cfg->Dx12Upscaler.value_or_default();
    case API::Vulkan:
        return cfg->VulkanUpscaler.value_or_default();
    default:
        return "";
    }
}

static void markBackendsChanged(State& state, const std::string& backend)
{
    state.newBackend = backend;
    for (auto& item : state.changeBackend)
        item.second = true;
}

static std::string targetForApi(State& state, const std::string& preset)
{
    if (preset == "fsr21")
        return state.api == API::DX11 ? "fsr21_12" : "fsr21";
    if (preset == "fsr22")
        return "fsr22";
    if (preset == "ffx234" || preset == "ffx315")
        return "fsr31";
    if (preset == "fsr411")
        return state.api == API::DX12 ? "fsr31" : "fsr31_12";
    if (preset == "xess")
        return state.api == API::DX11 ? "xess_12" : "xess";
    if (preset == "auto")
        return state.api == API::DX12 ? "xess" : "fsr22";
    return {};
}

static bool applyUpscaler(Config* cfg, State& state, const std::string& preset, std::string& error)
{
    const auto target = targetForApi(state, preset);
    if (target.empty())
    {
        error = "unsupported_upscaler";
        return false;
    }

    if (preset == "auto")
    {
        cfg->Dx11Upscaler.reset();
        cfg->Dx12Upscaler.reset();
        cfg->VulkanUpscaler.reset();
        cfg->FfxUpscalerIndex.reset();
        cfg->Fsr4Update.reset();
        cfg->Fsr4ForceEnableInt8.reset();
        cfg->Fsr4Preset.reset();
    }
    else if (preset == "fsr21")
    {
        cfg->Dx11Upscaler = "fsr21_12";
        cfg->Dx12Upscaler = "fsr21";
        cfg->VulkanUpscaler = "fsr21";
        cfg->FfxUpscalerIndex.reset();
        cfg->Fsr4Update = false;
        cfg->Fsr4ForceEnableInt8 = false;
        cfg->Fsr4Preset.reset();
    }
    else if (preset == "fsr22")
    {
        cfg->Dx11Upscaler = "fsr22";
        cfg->Dx12Upscaler = "fsr22";
        cfg->VulkanUpscaler = "fsr22";
        cfg->FfxUpscalerIndex.reset();
        cfg->Fsr4Update = false;
        cfg->Fsr4ForceEnableInt8 = false;
        cfg->Fsr4Preset.reset();
    }
    else if (preset == "ffx234" || preset == "ffx315" || preset == "fsr411")
    {
        cfg->Dx11Upscaler = preset == "fsr411" ? "fsr31_12" : "fsr31";
        cfg->Dx12Upscaler = "fsr31";
        cfg->VulkanUpscaler = preset == "fsr411" ? "fsr31_12" : "fsr31";
        cfg->FfxUpscalerIndex = preset == "ffx234" ? 2 : (preset == "ffx315" ? 1 : 0);
        if (preset == "fsr411") cfg->Fsr4Update.reset(); else cfg->Fsr4Update = false;
        cfg->Fsr4ForceEnableInt8 = preset == "fsr411";
        cfg->Fsr4Preset.reset(); // Default/auto
    }
    else if (preset == "xess")
    {
        cfg->Dx11Upscaler = "xess_12";
        cfg->Dx12Upscaler = "xess";
        cfg->VulkanUpscaler = "xess";
        cfg->FfxUpscalerIndex.reset();
        cfg->Fsr4Update = false;
        cfg->Fsr4ForceEnableInt8 = false;
        cfg->Fsr4Preset.reset();
    }

    markBackendsChanged(state, target);
    return true;
}

static bool applyCommand(const Command& cmd, std::string& error)
{
    if (!g_configInstance || !g_stateInstance)
    {
        error = "not_initialized";
        return false;
    }

    Config* cfg = g_configInstance();
    State& state = g_stateInstance();
    if (!cfg)
    {
        error = "config_unavailable";
        return false;
    }

    if (cmd.hasUpscaler && !applyUpscaler(cfg, state, cmd.upscaler, error))
        return false;

    if (cmd.hasQuality)
    {
        cfg->UpscaleRatioOverrideEnabled = cmd.qualityEnabled;
        if (cmd.qualityEnabled)
            cfg->UpscaleRatioOverrideValue = std::clamp(cmd.qualityRatio, 1.0f, 3.0f);
        const auto current = currentConfiguredBackend(cfg, state);
        if (!current.empty())
            markBackendsChanged(state, current);
    }

    if (cmd.hasSharpness)
    {
        cfg->OverrideSharpness = cmd.sharpnessEnabled;
        cfg->Sharpness = std::clamp(cmd.sharpness, 0.0f, 1.0f);
    }

    if (cmd.hasFgEnabled)
    {
        cfg->FGEnabled = cmd.fgEnabled;
        // Mirrors OptiScaler 0.9.5-pre4 menu behaviour.
        if (cmd.fgEnabled)
            state.FGchanged = true;
    }

    if (cmd.hasHudFix)
        cfg->FGHUDFix = cmd.hudFix;

    if (cmd.hasHudLimit)
        cfg->FGHUDLimit = std::clamp(cmd.hudLimit, 1, 9);

    return true;
}

static void completePendingOnPresent()
{
    std::optional<Command> command;
    {
        std::lock_guard lock(g_commandMutex);
        if (!g_pending.has_value())
            return;
        command = g_pending;
        g_pending.reset();
    }

    std::string error;
    const bool ok = applyCommand(*command, error);

    {
        std::lock_guard lock(g_commandMutex);
        g_completed = command;
        g_completedOk = ok;
        g_completedError = error;
    }
    g_lastCompletedSeq.store(command->seq, std::memory_order_release);
}

static void* originalForSlot(void** slot, int kind)
{
    std::lock_guard lock(g_hookMutex);
    for (const auto& record : g_hooks)
        if (record.slot == slot && record.kind == kind)
            return record.original;
    return nullptr;
}

static HRESULT STDMETHODCALLTYPE presentHook(IDXGISwapChain* self, UINT sync, UINT flags)
{
    g_presentHits.fetch_add(1, std::memory_order_relaxed);
    completePendingOnPresent();

    void** vtable = self ? *reinterpret_cast<void***>(self) : nullptr;
    auto original = vtable ? reinterpret_cast<PresentFn>(originalForSlot(&vtable[8], 8)) : nullptr;
    return original ? original(self, sync, flags) : E_FAIL;
}

static HRESULT STDMETHODCALLTYPE present1Hook(IDXGISwapChain1* self, UINT sync, UINT flags,
                                              const DXGI_PRESENT_PARAMETERS* params)
{
    g_present1Hits.fetch_add(1, std::memory_order_relaxed);
    completePendingOnPresent();

    void** vtable = self ? *reinterpret_cast<void***>(self) : nullptr;
    auto original = vtable ? reinterpret_cast<Present1Fn>(originalForSlot(&vtable[22], 22)) : nullptr;
    return original ? original(self, sync, flags, params) : E_FAIL;
}

static bool installHook(void** slot, void* hook, int kind)
{
    if (!slot)
        return false;

    {
        std::lock_guard lock(g_hookMutex);
        for (const auto& record : g_hooks)
        {
            if (record.slot != slot || record.kind != kind)
                continue;
            if (*slot == hook)
                return true;

            DWORD oldProtect = 0;
            if (!VirtualProtect(slot, sizeof(void*), PAGE_EXECUTE_READWRITE, &oldProtect))
                return false;
            InterlockedExchangePointer(reinterpret_cast<PVOID volatile*>(slot), hook);
            DWORD ignored = 0;
            VirtualProtect(slot, sizeof(void*), oldProtect, &ignored);
            return *slot == hook;
        }
    }

    DWORD oldProtect = 0;
    if (!VirtualProtect(slot, sizeof(void*), PAGE_EXECUTE_READWRITE, &oldProtect))
        return false;

    auto previous = InterlockedExchangePointer(reinterpret_cast<PVOID volatile*>(slot), hook);
    DWORD ignored = 0;
    VirtualProtect(slot, sizeof(void*), oldProtect, &ignored);

    if (!previous || previous == hook)
        return previous == hook;

    {
        std::lock_guard lock(g_hookMutex);
        g_hooks.push_back({slot, previous, kind});
    }
    return true;
}

static bool ensurePresentHooks()
{
    if (!g_stateInstance)
        return false;

    State& state = g_stateInstance();
    IDXGISwapChain* candidates[2] = { state.currentSwapchain, state.currentRealSwapchain };
    bool installed = false;

    for (auto* swap : candidates)
    {
        if (!swap)
            continue;

        void** vtable = *reinterpret_cast<void***>(swap);
        if (vtable)
            installed = installHook(&vtable[8], reinterpret_cast<void*>(&presentHook), 8) || installed;

        IDXGISwapChain1* swap1 = nullptr;
        if (SUCCEEDED(swap->QueryInterface(__uuidof(IDXGISwapChain1), reinterpret_cast<void**>(&swap1))) && swap1)
        {
            void** vtable1 = *reinterpret_cast<void***>(swap1);
            if (vtable1)
                installed = installHook(&vtable1[22], reinterpret_cast<void*>(&present1Hook), 22) || installed;
            swap1->Release();
        }
    }

    return installed;
}

static bool hooksStillInstalled()
{
    std::lock_guard lock(g_hookMutex);
    if (g_hooks.empty())
        return false;
    for (const auto& record : g_hooks)
    {
        if (!record.slot)
            continue;
        void* expected = record.kind == 22
            ? reinterpret_cast<void*>(&present1Hook)
            : reinterpret_cast<void*>(&presentHook);
        if (*record.slot == expected)
            return true;
    }
    return false;
}

static std::optional<Command> parseCommand(const std::filesystem::path& path)
{
    const auto kv = readKv(path);
    auto get = [&](const char* key) -> std::string {
        auto it = kv.find(key);
        return it == kv.end() ? std::string() : it->second;
    };

    Command c;
    try
    {
        c.seq = std::stoull(get("seq"));
        if (!c.seq)
            return std::nullopt;
        c.hasUpscaler = parseBool(get("has_upscaler"));
        c.upscaler = get("upscaler");

        c.hasQuality = parseBool(get("has_quality"));
        c.qualityEnabled = parseBool(get("quality_enabled"));
        if (c.hasQuality && c.qualityEnabled)
            c.qualityRatio = std::stof(get("quality_ratio"));

        c.hasSharpness = parseBool(get("has_sharpness"));
        c.sharpnessEnabled = parseBool(get("sharpness_enabled"));
        if (c.hasSharpness)
            c.sharpness = std::stof(get("sharpness"));

        c.hasFgEnabled = parseBool(get("has_fg_enabled"));
        c.fgEnabled = parseBool(get("fg_enabled"));

        c.hasHudFix = parseBool(get("has_hudfix"));
        c.hudFix = parseBool(get("hudfix"));

        c.hasHudLimit = parseBool(get("has_hud_limit"));
        if (c.hasHudLimit)
            c.hudLimit = std::stoi(get("hud_limit"));
    }
    catch (...)
    {
        return std::nullopt;
    }
    return c;
}

static void writeState(uint64_t seq, bool ok, const std::string& error, bool hookReady)
{
    Config* cfg = g_configInstance ? g_configInstance() : nullptr;
    State* state = g_stateInstance ? &g_stateInstance() : nullptr;

    std::ostringstream out;
    out << "protocol=" << kProtocol << "\n";
    out << "ready=" << ((cfg && state) ? 1 : 0) << "\n";
    out << "present_hook=" << (hookReady ? 1 : 0) << "\n";
    out << "present_hits=" << g_presentHits.load(std::memory_order_relaxed) << "\n";
    out << "present1_hits=" << g_present1Hits.load(std::memory_order_relaxed) << "\n";
    {
        std::lock_guard lock(g_hookMutex);
        out << "hook_count=" << g_hooks.size() << "\n";
    }
    out << "dxgi_sha=" << kSupportedSha << "\n";
    out << "seq=" << seq << "\n";
    out << "ok=" << (ok ? 1 : 0) << "\n";
    out << "error=" << error << "\n";
    if (cfg && state)
    {
        out << "api=" << static_cast<int>(state->api) << "\n";
        out << "backend=" << currentConfiguredBackend(cfg, *state) << "\n";
        out << "ffx_index=" << cfg->FfxUpscalerIndex.value_or_default() << "\n";
        out << "ffx_upscaler_count=" << state->ffxUpscalerVersionIds.size() << "\n";
        const auto ffxCount = (std::min)(state->ffxUpscalerVersionIds.size(), state->ffxUpscalerVersionNames.size());
        for (size_t i = 0; i < ffxCount && i < 16; ++i)
        {
            const char* name = state->ffxUpscalerVersionNames[i];
            out << "ffx_" << i << "_name=" << (name ? name : "") << "\n";
            out << "ffx_" << i << "_id=" << static_cast<unsigned long long>(state->ffxUpscalerVersionIds[i]) << "\n";
        }
        out << "quality_enabled=" << (cfg->UpscaleRatioOverrideEnabled.value_or_default() ? 1 : 0) << "\n";
        out << "quality_ratio=" << cfg->UpscaleRatioOverrideValue.value_or_default() << "\n";
        out << "sharpness_enabled=" << (cfg->OverrideSharpness.value_or_default() ? 1 : 0) << "\n";
        out << "sharpness=" << cfg->Sharpness.value_or_default() << "\n";
        out << "fg_enabled=" << (cfg->FGEnabled.value_or_default() ? 1 : 0) << "\n";
        out << "hudfix=" << (cfg->FGHUDFix.value_or_default() ? 1 : 0) << "\n";
        out << "hud_limit=" << cfg->FGHUDLimit.value_or_default() << "\n";
    }
    atomicWrite(g_root / L"sdh-control.state", out.str());
}

static DWORD WINAPI workerThread(void*)
{
    const auto cmdPath = g_root / L"sdh-control.cmd";
    uint64_t seen = 0;
    if (auto existing = parseCommand(cmdPath); existing)
        seen = existing->seq; // Never replay a command left by a previous process.

    bool hookReady = false;
    bool reportedHookReady = false;
    writeState(seen, true, "", false);

    while (g_running.load(std::memory_order_acquire))
    {
        if (!hookReady || !hooksStillInstalled())
            hookReady = ensurePresentHooks();

        if (hookReady != reportedHookReady)
        {
            reportedHookReady = hookReady;
            writeState(seen, true, "", hookReady);
        }

        auto command = parseCommand(cmdPath);
        if (command && command->seq > seen)
        {
            seen = command->seq;
            {
                std::lock_guard lock(g_commandMutex);
                g_pending = *command;
            }

            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
            while (g_running.load() && g_lastCompletedSeq.load(std::memory_order_acquire) < command->seq &&
                   std::chrono::steady_clock::now() < deadline)
                std::this_thread::sleep_for(std::chrono::milliseconds(10));

            bool ok = false;
            std::string error;
            {
                std::lock_guard lock(g_commandMutex);
                if (g_completed && g_completed->seq == command->seq)
                {
                    ok = g_completedOk;
                    error = g_completedError;
                }
                else
                {
                    const auto p0 = g_presentHits.load(std::memory_order_relaxed);
                    const auto p1 = g_present1Hits.load(std::memory_order_relaxed);
                    error = hookReady
                        ? (p0 == 0 && p1 == 0 ? "present_and_present1_timeout" : "render_callback_timeout")
                        : "present_hook_unavailable";
                }
            }
            writeState(command->seq, ok, error, hookReady);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    return 0;
}

static bool initialize()
{
    wchar_t selfPath[MAX_PATH] = {};
    if (!GetModuleFileNameW(g_self, selfPath, MAX_PATH))
        return false;
    g_root = std::filesystem::path(selfPath).parent_path().parent_path();

    g_opti = GetModuleHandleW(L"dxgi.dll");
    if (!g_opti)
        return false;

    wchar_t optiPath[MAX_PATH] = {};
    if (!GetModuleFileNameW(g_opti, optiPath, MAX_PATH))
        return false;

    const auto hash = sha256File(optiPath);
    if (hash != kSupportedSha)
    {
        std::ostringstream out;
        out << "protocol=" << kProtocol << "\nready=0\npresent_hook=0\nseq=0\nok=0\n"
            << "error=unsupported_dxgi_sha\n"
            << "dxgi_sha=" << hash << "\n";
        atomicWrite(g_root / L"sdh-control.state", out.str());
        return false;
    }

    if (!validateFunctions())
    {
        atomicWrite(g_root / L"sdh-control.state",
                    "protocol=1\nready=0\npresent_hook=0\nseq=0\nok=0\nerror=signature_mismatch\n");
        return false;
    }

    g_running.store(true, std::memory_order_release);
    HANDLE thread = CreateThread(nullptr, 0, workerThread, nullptr, 0, nullptr);
    if (!thread)
        return false;
    CloseHandle(thread);
    return true;
}
}

extern "C" __declspec(dllexport) void InitializeASI()
{
    initialize();
}

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        g_self = module;
        DisableThreadLibraryCalls(module);
    }
    else if (reason == DLL_PROCESS_DETACH)
    {
        g_running.store(false, std::memory_order_release);
    }
    return TRUE;
}
