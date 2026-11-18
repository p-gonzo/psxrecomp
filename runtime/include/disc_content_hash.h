#pragma once

#include "disc_path.h"
#include "disc_toc_fingerprint.h"
#include "iso_reader.h"
#include "psx_sha256.h"
#include "psx_sha256_file.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <string>
#include <vector>

namespace PSXRecompV4 {

// One byte identity for package targets and mandatory original-disc policy:
// CUE hashes its data-track file; CHD hashes every decoded raw 2352-byte sector.
// Host crypto and its portable fallback are shared through psx_sha256_file.
inline bool disc_content_sha256_uncached(const std::filesystem::path& path, std::string& out,
                 std::string* error) {
    out.clear();
    if (path.empty()) return true;
    const DiscPathResolution resolved = resolve_disc_path(path);
    const std::filesystem::path input = resolved.data;
    std::string extension = resolved.mount.extension().string();
    std::transform(extension.begin(), extension.end(), extension.begin(),
        [](unsigned char c) { return (char)std::tolower(c); });
    if (extension == ".chd") {
        psx_sha256_ctx hash;
        psx_sha256_init(&hash);
        PS1::ISOReader disc;
        if (!disc.Open(resolved.mount.string())) {
            if (error) *error =
                "cannot decode image fingerprint: " + resolved.mount.string();
            return false;
        }
        std::array<uint8_t, 2352> sector{};
        for (uint32_t lba = 0; lba < disc.GetSectorCount(); ++lba) {
            if (!disc.ReadRawSector(lba, sector.data())) {
                if (error) *error =
                    "cannot finish decoding image fingerprint: " +
                    resolved.mount.string();
                return false;
            }
            psx_sha256_update(&hash, sector.data(), sector.size());
        }
        uint8_t digest[32];
        psx_sha256_final(&hash, digest);
        std::ostringstream text;
        for (uint8_t byte : digest)
            text << std::hex << std::setw(2) << std::setfill('0')
                 << (unsigned)byte;
        out = text.str();
        return true;
    }

    uint8_t digest[32];
    const auto result = psx_sha256_file(input, digest);
    if (result == PsxFileHashResult::OpenError) {
        if (error) *error = "cannot fingerprint image: " + input.string();
        return false;
    }
    if (result != PsxFileHashResult::Ok) {
        if (error) *error =
            "cannot finish fingerprinting image: " + input.string();
        return false;
    }
    std::ostringstream text;
    for (uint8_t byte : digest)
        text << std::hex << std::setw(2) << std::setfill('0') << (unsigned)byte;
    out = text.str();
    return true;
}

#ifndef PSX_DISC_DIGEST_CACHE
#define PSX_DISC_DIGEST_CACHE 0
#endif

// disc_content_sha256 with an optional per-image digest cache
// (PSX_DISC_DIGEST_CACHE; runtime.cmake turns it on for the Nintendo 3DS).
//
// Hashing a whole image is about a second on a desktop and over a minute on a
// 3DS reading its SD card, on every launch. With the cache, the image that is
// actually hashed (a CUE's data file, or the CHD) gets a sidecar
// "<image>.psxdigest" holding "psxdigest v1 <size> <mtime_ns> <sha256>". A
// later call whose image has the same size and modification time reuses the
// digest; any change, an unreadable or malformed sidecar, or a missing image
// hashes in full and rewrites it. A sidecar that cannot be written only costs
// the next launch a full hash.
inline bool disc_content_sha256(const std::filesystem::path& path, std::string& out,
                 std::string* error) {
#if PSX_DISC_DIGEST_CACHE
    out.clear();
    if (path.empty()) return true;
    const DiscPathResolution resolved = resolve_disc_path(path);
    std::string extension = resolved.mount.extension().string();
    std::transform(extension.begin(), extension.end(), extension.begin(),
        [](unsigned char c) { return (char)std::tolower(c); });
    const std::filesystem::path hashed =
        extension == ".chd" ? resolved.mount : resolved.data;
    std::error_code size_ec, time_ec;
    const auto size = std::filesystem::file_size(hashed, size_ec);
    const auto mtime = std::filesystem::last_write_time(hashed, time_ec);
    const bool keyed = !size_ec && !time_ec;
    std::filesystem::path sidecar = hashed;
    sidecar += ".psxdigest";
    std::ostringstream key;
    if (keyed)
        key << "psxdigest v1 " << (unsigned long long)size << ' '
            << (long long)std::chrono::duration_cast<std::chrono::nanoseconds>(
                   mtime.time_since_epoch()).count() << ' ';
    if (keyed) {
        std::ifstream in(sidecar);
        std::string line;
        if (in && std::getline(in, line) && line.size() == key.str().size() + 64 &&
            line.compare(0, key.str().size(), key.str()) == 0) {
            const std::string digest = line.substr(key.str().size());
            if (std::all_of(digest.begin(), digest.end(), [](unsigned char c) {
                    return std::isdigit(c) || (c >= 'a' && c <= 'f');
                })) {
                out = digest;
                return true;
            }
        }
    }
    const bool ok = disc_content_sha256_uncached(path, out, error);
    if (ok && keyed && out.size() == 64) {
        std::ofstream sidecar_out(sidecar, std::ios::trunc);
        if (sidecar_out) sidecar_out << key.str() << out << '\n';
    }
    return ok;
#else
    return disc_content_sha256_uncached(path, out, error);
#endif
}

// Configured once from the title's [game] required_disc_sha256, before any
// launcher/provider call. This is title policy, independent of mod selections.
inline std::vector<std::string>& required_disc_sha256() {
    static std::vector<std::string> hashes;
    return hashes;
}

inline std::vector<std::string>& required_disc_fps() {
    static std::vector<std::string> fingerprints;
    return fingerprints;
}

inline bool required_disc_content_matches(const std::filesystem::path& path,
        const std::vector<std::string>& expected, std::string* error = nullptr,
        const std::vector<std::string>& expected_fps = {}) {
    if (expected.empty() && expected_fps.empty()) return true;
    if (!expected_fps.empty()) {
        const auto mount = resolve_disc_path(path).mount;
        PS1::ISOReader reader;
        std::array<uint8_t,2048> pvd{};
        // Geometry alone does not describe every container's data-sector
        // decoding. Require a readable mounted ISO data view as well.
        if (path.empty() || !reader.Open(mount.string()) ||
            !reader.ReadSector(16,pvd.data()) || pvd[0]!=1 ||
            !std::equal(pvd.begin()+1,pvd.begin()+6,"CD001") || pvd[6]!=1) {
            if(error) *error="Cannot verify the required original disc layout. Select the original supported disc image.";
            return false;
        }
        const auto actual_fp=disc_toc_fingerprint(reader);
        if (std::find(expected_fps.begin(),expected_fps.end(),actual_fp)==expected_fps.end()) {
            if(error) *error="Unsupported disc layout. Use the original disc and track layout; apply changes through Mods.";
            return false;
        }
    }
    if(expected.empty()) return true;
    std::string actual, detail;
    if (path.empty() || !disc_content_sha256(path, actual, &detail)) {
        if (error) *error = "Cannot verify the required original disc. " +
            (detail.empty() ? std::string("Select the original supported disc image.") : detail);
        return false;
    }
    if (std::find(expected.begin(), expected.end(), actual) != expected.end()) return true;
    if (error) *error = "Unsupported disc image. Use the verified original disc; "
        "apply changes through Mods. Disc SHA-256: " + actual;
    return false;
}

inline bool required_original_disc_matches(const std::filesystem::path& path,
                                           std::string* error = nullptr) {
    return required_disc_content_matches(path, required_disc_sha256(), error, required_disc_fps());
}

} // namespace PSXRecompV4
