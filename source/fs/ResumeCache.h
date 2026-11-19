#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

// Space that is kept free on the target device when a dump has to be split over multiple runs.
#define FREE_SPACE_MARGIN (64ULL * 1024 * 1024)
// Resume caches are stored on the dump target (relative to its root). Keep this folder when moving the dump files away.
#define RESUME_CACHE_SUBDIR "dumpling/wudd_cache"

// State needed to continue a WUD dump that was split over multiple runs (e.g. because the target drive is too small).
// Stored on the dump target in RESUME_CACHE_SUBDIR.
struct ResumeCache {
    static constexpr uint32_t MAGIC   = 0x57524331; // "WRC1"
    static constexpr uint32_t VERSION = 1;

    uint32_t targetFormat = 0;
    std::string discId;
    std::array<uint8_t, 32> firstSectorHash{}; // SHA256 of disc sector 0, identifies the disc
    uint64_t totalSectorCount = 0;
    uint64_t nextSector       = 0; // first sector the next run has to read
    int32_t nextPart          = 1; // part number the next run has to write first
    int32_t nextRun           = 2; // number of the next run (1-based)
    std::vector<uint64_t> skippedSectors;
    std::vector<uint8_t> hashState; // FileHashes::exportState()

    [[nodiscard]] bool save(const std::string &path) const;
    static std::optional<ResumeCache> load(const std::string &path);
};

// Same idea for the "Dump partition as .app" mode. Position = content index (TMD order) + offset inside that content.
struct AppResumeCache {
    static constexpr uint32_t MAGIC   = 0x57524341; // "WRCA"
    static constexpr uint32_t VERSION = 1;

    std::string discId;
    std::string volumeId;
    std::array<uint8_t, 32> firstSectorHash{};
    uint64_t titleId          = 0;
    uint64_t totalBytes       = 0;
    uint32_t nextContentIndex = 0;
    uint64_t nextOffset       = 0; // offset inside the content at nextContentIndex
    int32_t nextPiece         = 1; // piece number of the .app.partNN the next run has to write first
    int32_t nextRun           = 2;

    [[nodiscard]] bool save(const std::string &path) const;
    static std::optional<AppResumeCache> load(const std::string &path);
};
