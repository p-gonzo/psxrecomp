#pragma once

#include <filesystem>
#include <string>
#include <system_error>
#if defined(__3DS__)
#include <cerrno>
#include <unistd.h>
#endif

namespace PSXRecompV4 {

// Nintendo 3DS paths are device-rooted ("sdmc:/3ds/thps2"), getcwd() included.
// libstdc++'s POSIX grammar calls them relative, so std::filesystem::absolute
// would prefix the (already device-rooted) working directory again. Like UNC
// paths on Windows, "name:/..." is fully qualified here.
inline bool host_path_is_device_rooted(const std::string& s) {
    const auto colon = s.find(':');
    return colon != std::string::npos && colon > 0 && colon + 1 < s.size() &&
           s[colon + 1] == '/' && s.find('/') == colon + 1;
}

// generic_string() can collapse a MinGW UNC root from "//server" to
// "/server". Persist the native spelling, changing separators only on
// Windows; a backslash in a POSIX filename is an ordinary character.
inline std::string host_path_forward_slashes(const std::filesystem::path& path) {
    std::string result = path.string();
#ifdef _WIN32
    for (char& c : result) if (c == '\\') c = '/';
#endif
    return result;
}

// Some MinGW libstdc++ builds treat a readable UNC path as drive-relative:
// absolute("\\\\server\\share\\file") becomes "D:\\server\\share\\file".
// Windows already considers UNC and device namespace paths fully qualified.
// Preserve their spelling before asking std::filesystem to anchor a path.
inline bool host_path_is_absolute(const std::filesystem::path& path) {
#ifdef _WIN32
    const auto& s = path.native();
    const auto separator = [](wchar_t c) { return c == L'\\' || c == L'/'; };
    if (s.size() > 2 && separator(s[0]) && separator(s[1]) && !separator(s[2]))
        return true;
#endif
#if defined(__3DS__)
    if (host_path_is_device_rooted(path.string())) return true;
#endif
    return path.is_absolute();
}

#if defined(__3DS__)
// A relative path joins getcwd(); a "/..." path is the root of the current
// device. Returns an empty path when the working directory is unknown.
inline std::filesystem::path host_absolute_on_device(const std::filesystem::path& path) {
    char cwd[1024];
    if (!getcwd(cwd, sizeof cwd)) return {};
    const std::string base(cwd), s = path.string();
    if (!s.empty() && s[0] == '/') {
        const auto colon = base.find(':');
        return std::filesystem::path(colon == std::string::npos ? s : base.substr(0, colon + 1) + s);
    }
    return std::filesystem::path(base) / path;
}
#endif

inline std::filesystem::path host_absolute(const std::filesystem::path& path,
                                          std::error_code& ec) {
    if (host_path_is_absolute(path)) {
        ec.clear();
        return path;
    }
#if defined(__3DS__)
    std::filesystem::path abs = host_absolute_on_device(path);
    if (abs.empty()) ec = std::error_code(errno ? errno : EINVAL, std::generic_category());
    else ec.clear();
    return abs;
#else
    return std::filesystem::absolute(path, ec);
#endif
}

inline std::filesystem::path host_absolute(const std::filesystem::path& path) {
    if (host_path_is_absolute(path)) return path;
#if defined(__3DS__)
    std::filesystem::path abs = host_absolute_on_device(path);
    if (abs.empty())
        throw std::filesystem::filesystem_error(
            "cannot make absolute path", path,
            std::error_code(errno ? errno : EINVAL, std::generic_category()));
    return abs;
#else
    return std::filesystem::absolute(path);
#endif
}

// A fully qualified config value must not be prefixed with its config root.
inline std::filesystem::path host_resolve(const std::filesystem::path& root,
                                         const std::filesystem::path& path) {
    return host_absolute(host_path_is_absolute(path) ? path : root / path);
}

} // namespace PSXRecompV4
