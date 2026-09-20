#include <windows.h>
#include <bcrypt.h>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>
#include <atomic>
#include <chrono>
#include <iomanip>
#include <vector>

#pragma comment(lib, "bcrypt.lib")

namespace fs = std::filesystem;

static std::atomic_bool g_stop{false};
static HMODULE g_module = nullptr;

static std::string hex_bytes(const unsigned char* data, size_t size) {
    std::ostringstream ss;
    ss << std::hex << std::setfill('0');
    for (size_t i = 0; i < size; ++i) ss << std::setw(2) << static_cast<unsigned int>(data[i]);
    return ss.str();
}

static std::string sha256_file(const fs::path& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return "";
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    DWORD object_len = 0, cb = 0, hash_len = 0;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) != 0) return "";
    if (BCryptGetProperty(alg, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&object_len), sizeof(object_len), &cb, 0) != 0) { BCryptCloseAlgorithmProvider(alg, 0); return ""; }
    if (BCryptGetProperty(alg, BCRYPT_HASH_LENGTH, reinterpret_cast<PUCHAR>(&hash_len), sizeof(hash_len), &cb, 0) != 0) { BCryptCloseAlgorithmProvider(alg, 0); return ""; }
    std::vector<unsigned char> object(object_len);
    std::vector<unsigned char> digest(hash_len);
    if (BCryptCreateHash(alg, &hash, object.data(), object_len, nullptr, 0, 0) != 0) { BCryptCloseAlgorithmProvider(alg, 0); return ""; }
    std::vector<char> buffer(1 << 20);
    while (f) {
        f.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        auto got = f.gcount();
        if (got > 0) BCryptHashData(hash, reinterpret_cast<PUCHAR>(buffer.data()), static_cast<ULONG>(got), 0);
    }
    std::string result;
    if (BCryptFinishHash(hash, digest.data(), hash_len, 0) == 0) result = hex_bytes(digest.data(), digest.size());
    BCryptDestroyHash(hash);
    BCryptCloseAlgorithmProvider(alg, 0);
    return result;
}

static fs::path module_path(HMODULE mod) {
    std::wstring buf(32768, L'\0');
    DWORD n = GetModuleFileNameW(mod, buf.data(), static_cast<DWORD>(buf.size()));
    buf.resize(n);
    return fs::path(buf);
}

static void atomic_write(const fs::path& path, const std::string& text) {
    fs::path tmp = path;
    tmp += L".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        out.write(text.data(), static_cast<std::streamsize>(text.size()));
        out.flush();
    }
    std::error_code ec;
    fs::rename(tmp, path, ec);
    if (ec) {
        fs::remove(path, ec);
        ec.clear();
        fs::rename(tmp, path, ec);
    }
}

static std::string json_escape(const std::string& s) {
    std::ostringstream o;
    for (unsigned char c : s) {
        switch (c) {
            case '\\': o << "\\\\"; break;
            case '"': o << "\\\""; break;
            case '\n': o << "\\n"; break;
            case '\r': o << "\\r"; break;
            case '\t': o << "\\t"; break;
            default: if (c < 0x20) o << "?"; else o << c;
        }
    }
    return o.str();
}

static std::string narrow(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

static void write_state(const fs::path& state_path, const fs::path& root, const std::string& dxgi_hash,
                        unsigned long long seq, const std::string& command, bool ok, const std::string& message) {
    std::ostringstream ss;
    ss << "{\n"
       << "  \"api\": 1,\n"
       << "  \"helper\": \"sdh-optiscaler-control\",\n"
       << "  \"helper_version\": \"0.1.0-test\",\n"
       << "  \"pid\": " << GetCurrentProcessId() << ",\n"
       << "  \"dxgi_sha256\": \"" << dxgi_hash << "\",\n"
       << "  \"root\": \"" << json_escape(narrow(root.wstring())) << "\",\n"
       << "  \"seq\": " << seq << ",\n"
       << "  \"command\": \"" << json_escape(command) << "\",\n"
       << "  \"ok\": " << (ok ? "true" : "false") << ",\n"
       << "  \"message\": \"" << json_escape(message) << "\"\n"
       << "}\n";
    atomic_write(state_path, ss.str());
}

static unsigned long long parse_seq(const std::string& text) {
    const std::string key = "\"seq\"";
    auto p = text.find(key);
    if (p == std::string::npos) return 0;
    p = text.find(':', p + key.size());
    if (p == std::string::npos) return 0;
    ++p;
    while (p < text.size() && (text[p] == ' ' || text[p] == '\t')) ++p;
    try { return std::stoull(text.substr(p)); } catch (...) { return 0; }
}

static std::string parse_command(const std::string& text) {
    const std::string key = "\"command\"";
    auto p = text.find(key);
    if (p == std::string::npos) return "";
    p = text.find(':', p + key.size());
    if (p == std::string::npos) return "";
    p = text.find('"', p);
    if (p == std::string::npos) return "";
    auto e = text.find('"', p + 1);
    if (e == std::string::npos) return "";
    return text.substr(p + 1, e - p - 1);
}

static DWORD WINAPI worker(LPVOID) {
    const fs::path asi = module_path(g_module);
    const fs::path root = asi.parent_path().parent_path();
    const fs::path dxgi = root / L"dxgi.dll";
    const fs::path cmd_path = root / L"sdh-control.cmd.json";
    const fs::path state_path = root / L"sdh-control.state.json";
    const std::string dxgi_hash = sha256_file(dxgi);
    unsigned long long last_seq = 0;
    write_state(state_path, root, dxgi_hash, 0, "HELLO", true, "helper loaded");
    while (!g_stop.load()) {
        std::ifstream in(cmd_path, std::ios::binary);
        if (in) {
            std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
            auto seq = parse_seq(text);
            auto cmd = parse_command(text);
            if (seq > last_seq) {
                last_seq = seq;
                if (cmd == "PING" || cmd == "GET_INFO") {
                    write_state(state_path, root, dxgi_hash, seq, cmd, true, "runtime channel ready");
                } else {
                    write_state(state_path, root, dxgi_hash, seq, cmd, false,
                                "command transport ready; OptiScaler memory adapter not mapped for this dxgi build yet");
                }
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    return 0;
}

extern "C" __declspec(dllexport) void InitializeASI(void) {
    static std::atomic_bool started{false};
    bool expected = false;
    if (!started.compare_exchange_strong(expected, true)) return;
    HANDLE th = CreateThread(nullptr, 0, worker, nullptr, 0, nullptr);
    if (th) CloseHandle(th);
}

BOOL WINAPI DllMain(HINSTANCE hinst, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_module = hinst;
        DisableThreadLibraryCalls(hinst);
    } else if (reason == DLL_PROCESS_DETACH) {
        g_stop.store(true);
    }
    return TRUE;
}
