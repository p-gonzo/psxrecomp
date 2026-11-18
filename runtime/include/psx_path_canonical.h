#pragma once
/* psx_path_canonical.h - std::filesystem absolute / canonical / weakly_canonical
 * that also accept device-rooted paths.
 *
 * On the Nintendo 3DS every file path is device-rooted ("sdmc:/3ds/thps2"),
 * and so is getcwd(). libstdc++'s path grammar is POSIX: without a leading '/'
 * such a path is relative ("sdmc:" is just its first component), so
 * std::filesystem::absolute, canonical and weakly_canonical cannot produce an
 * absolute form and fail with EINVAL before touching the card.
 *
 * host_path.h decides what is absolute ("name:/..." is) and anchors relative
 * paths on getcwd(). The 3DS SD card
 * is FAT, which has no symbolic links, so the lexically normal absolute path
 * is the canonical path. canonical() additionally requires the path to exist,
 * as std::filesystem::canonical does.
 *
 * Every other host forwards to std::filesystem unchanged.
 */
#include <filesystem>
#include <string>
#include <system_error>
#include "host_path.h"

namespace PSXRecompV4 {
namespace paths {

#if defined(__3DS__)

inline std::filesystem::path absolute(const std::filesystem::path& p) {
    return ::PSXRecompV4::host_absolute(p);   /* host_path.h: device-rooted aware */
}

inline std::filesystem::path weakly_canonical(const std::filesystem::path& p) {
    return paths::absolute(p).lexically_normal();
}
inline std::filesystem::path weakly_canonical(const std::filesystem::path& p,
                                              std::error_code& ec) {
    ec.clear();
    return paths::weakly_canonical(p);
}
inline std::filesystem::path canonical(const std::filesystem::path& p,
                                       std::error_code& ec) {
    ec.clear();
    std::filesystem::path r = paths::weakly_canonical(p);
    if (!std::filesystem::exists(r, ec)) {
        if (!ec) ec = std::make_error_code(std::errc::no_such_file_or_directory);
        return {};
    }
    return r;
}

#else

inline std::filesystem::path absolute(const std::filesystem::path& p) {
    return std::filesystem::absolute(p);
}
inline std::filesystem::path weakly_canonical(const std::filesystem::path& p) {
    return std::filesystem::weakly_canonical(p);
}
inline std::filesystem::path weakly_canonical(const std::filesystem::path& p,
                                              std::error_code& ec) {
    return std::filesystem::weakly_canonical(p, ec);
}
inline std::filesystem::path canonical(const std::filesystem::path& p,
                                       std::error_code& ec) {
    return std::filesystem::canonical(p, ec);
}

#endif

}  // namespace paths
}  // namespace PSXRecompV4
