#pragma once

#include <array>
#include <cstdint>
#include <string>

#ifdef _WIN32
#include <winsock2.h>
#include <windows.h>
#include <intrin.h>
#endif

namespace wm_sec {

constexpr uint64_t mixSeed(uint64_t line, uint64_t counter) {
    uint64_t z = 0x9E3779B97F4A7C15ULL ^ (line * 0xBF58476D1CE4E5B9ULL) ^ (counter * 0x94D049BB133111EBULL);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}

constexpr uint8_t keyStreamByte(uint64_t seed, size_t idx) {
    uint64_t x = seed + static_cast<uint64_t>(idx) * 0x9E3779B97F4A7C15ULL;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ULL;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBULL;
    x = x ^ (x >> 31);
    return static_cast<uint8_t>((x >> ((idx & 7u) * 8u)) & 0xFFu) ^ 0xA5u;
}

template <size_t N, uint64_t Seed>
struct ObfString {
    char data[N];

    constexpr ObfString(const char (&lit)[N]) : data{} {
        for (size_t i = 0; i < N; ++i) {
            data[i] = static_cast<char>(static_cast<uint8_t>(lit[i]) ^ keyStreamByte(Seed, i));
        }
    }

    std::string reveal() const {
        std::string out(N > 0 ? N - 1 : 0, '\0');
        const volatile char* vp = data;
        for (size_t i = 0; i + 1 < N; ++i) {
            out[i] = static_cast<char>(static_cast<uint8_t>(vp[i]) ^ keyStreamByte(Seed, i));
        }
        return out;
    }
};

template <size_t N, uint64_t Seed>
struct ObfWString {
    wchar_t data[N];

    constexpr ObfWString(const wchar_t (&lit)[N]) : data{} {
        for (size_t i = 0; i < N; ++i) {
            uint16_t k = static_cast<uint16_t>(keyStreamByte(Seed, i * 2)) |
                         (static_cast<uint16_t>(keyStreamByte(Seed, i * 2 + 1)) << 8);
            data[i] = static_cast<wchar_t>(static_cast<uint16_t>(lit[i]) ^ k);
        }
    }

    std::wstring reveal() const {
        std::wstring out(N > 0 ? N - 1 : 0, L'\0');
        const volatile wchar_t* vp = data;
        for (size_t i = 0; i + 1 < N; ++i) {
            uint16_t k = static_cast<uint16_t>(keyStreamByte(Seed, i * 2)) |
                         (static_cast<uint16_t>(keyStreamByte(Seed, i * 2 + 1)) << 8);
            out[i] = static_cast<wchar_t>(static_cast<uint16_t>(vp[i]) ^ k);
        }
        return out;
    }
};

inline void wipeString(std::string& s) {
    if (s.empty()) return;
    volatile char* p = &s[0];
    for (size_t i = 0; i < s.size(); ++i) p[i] = '\0';
    s.clear();
}

inline void wipeWString(std::wstring& s) {
    if (s.empty()) return;
    volatile wchar_t* p = &s[0];
    for (size_t i = 0; i < s.size(); ++i) p[i] = L'\0';
    s.clear();
}

#define WM_OBF(str) ([]() -> std::string { \
    constexpr uint64_t kSeed = ::wm_sec::mixSeed(__LINE__, __COUNTER__); \
    constexpr ::wm_sec::ObfString<sizeof(str), kSeed> kEnc(str); \
    return kEnc.reveal(); \
}())

#define WM_OBF_W(str) ([]() -> std::wstring { \
    constexpr uint64_t kSeed = ::wm_sec::mixSeed(__LINE__, __COUNTER__); \
    constexpr ::wm_sec::ObfWString<sizeof(str) / sizeof(wchar_t), kSeed> kEnc(str); \
    return kEnc.reveal(); \
}())

#ifdef _WIN32

inline HMODULE getHiddenModuleW(const std::wstring& modName) {
    HMODULE h = GetModuleHandleW(modName.c_str());
    return h ? h : LoadLibraryW(modName.c_str());
}

template <typename Fn>
inline Fn resolveHiddenApi(const std::wstring& modName, const std::string& procName) {
    HMODULE mod = getHiddenModuleW(modName);
    if (!mod) return nullptr;
    return reinterpret_cast<Fn>(GetProcAddress(mod, procName.c_str()));
}

inline void hideCurrentThread() {
    // Kept as a no-op: NtSetInformationThread(ThreadHideFromDebugger) terminates threads on
    // benign internal COM/DXGI/MediaFoundation first-chance SEH exceptions in retail drivers.
}

inline bool isDebuggerAttached() {
#if defined(_M_X64) || defined(__x86_64__)
    const uint64_t peb = __readgsqword(0x60);
    if (peb && *reinterpret_cast<const volatile uint8_t*>(peb + 2) != 0) {
        return true;
    }
#endif

    std::wstring k32 = WM_OBF_W(L"kernel32.dll");
    std::string fnIsDbg = WM_OBF("IsDebuggerPresent");
    using PfnIsDbg = BOOL(WINAPI*)();
    if (auto pIsDbg = resolveHiddenApi<PfnIsDbg>(k32, fnIsDbg)) {
        if (pIsDbg()) {
            wipeWString(k32);
            wipeString(fnIsDbg);
            return true;
        }
    }
    wipeString(fnIsDbg);

    std::string fnRem = WM_OBF("CheckRemoteDebuggerPresent");
    using PfnRemDbg = BOOL(WINAPI*)(HANDLE, PBOOL);
    if (auto pRem = resolveHiddenApi<PfnRemDbg>(k32, fnRem)) {
        BOOL rem = FALSE;
        if (pRem(GetCurrentProcess(), &rem) && rem) {
            wipeWString(k32);
            wipeString(fnRem);
            return true;
        }
    }
    wipeWString(k32);
    wipeString(fnRem);

    std::wstring ntdll = WM_OBF_W(L"ntdll.dll");
    std::string fnQip = WM_OBF("NtQueryInformationProcess");
    using PfnNtQip = LONG(NTAPI*)(HANDLE, ULONG, PVOID, ULONG, PULONG);
    if (auto pQip = resolveHiddenApi<PfnNtQip>(ntdll, fnQip)) {
        ULONG_PTR dbgPort = 0;
        if (pQip(GetCurrentProcess(), 7 /* ProcessDebugPort */, &dbgPort, sizeof(dbgPort), nullptr) == 0 && dbgPort != 0) {
            wipeWString(ntdll);
            wipeString(fnQip);
            return true;
        }
    }
    wipeWString(ntdll);
    wipeString(fnQip);

    return false;
}

inline void enforceAntiReverse() {
    if (isDebuggerAttached()) {
        ExitProcess(0);
    }
}

#endif // _WIN32

} // namespace wm_sec
