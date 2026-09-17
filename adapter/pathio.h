#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <windows.h>
#include <shellapi.h>
#include <string>
#include <vector>

#pragma comment(lib, "shell32.lib")

namespace sl {

inline std::wstring widenUtf8(const std::string &s) {
    if (s.empty()) return std::wstring();
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    if (n <= 0) return std::wstring();
    std::wstring w((size_t)n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &w[0], n);
    return w;
}

inline std::string narrowUtf8(const std::wstring &w) {
    if (w.empty()) return std::string();
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(),
                                      nullptr, 0, nullptr, nullptr);
    if (n <= 0) return std::string();
    std::string s((size_t)n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &s[0], n, nullptr, nullptr);
    return s;
}

class Utf8Args {
public:
    Utf8Args(int fallbackArgc, char **fallbackArgv) {
        int n = 0;
        wchar_t **wide = CommandLineToArgvW(GetCommandLineW(), &n);
        if (!wide || n <= 0) {

            if (wide) LocalFree(wide);
            _argc = fallbackArgc;
            _argv = fallbackArgv;
            return;
        }

        _storage.reserve((size_t)n);
        for (int i = 0; i < n; i++) _storage.push_back(narrowUtf8(wide[i]));
        LocalFree(wide);

        _pointers.reserve((size_t)n + 1);
        for (std::string &s : _storage) _pointers.push_back(&s[0]);
        _pointers.push_back(nullptr);
        _argc = n;
        _argv = _pointers.data();
    }

    Utf8Args(const Utf8Args &) = delete;
    Utf8Args &operator=(const Utf8Args &) = delete;

    int argc() const { return _argc; }
    char **argv() const { return _argv; }

private:
    std::vector<std::string> _storage;
    std::vector<char *> _pointers;
    int _argc = 0;
    char **_argv = nullptr;
};

}
