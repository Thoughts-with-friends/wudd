#include "ResumeCache.h"
#include <cstdio>
#include <optional>
#include <memory>

namespace {
    using FilePtr = std::unique_ptr<FILE, decltype(&fclose)>;

    template<typename T>
    bool writeVal(FILE *f, const T &val) {
        return fwrite(&val, sizeof(T), 1, f) == 1;
    }

    template<typename T>
    bool readVal(FILE *f, T &val) {
        return fread(&val, sizeof(T), 1, f) == 1;
    }

    bool writeBytes(FILE *f, const void *data, uint32_t size) {
        return writeVal(f, size) && (size == 0 || fwrite(data, 1, size, f) == size);
    }

    template<typename Container>
    bool readBytes(FILE *f, Container &out, uint32_t maxSize) {
        uint32_t size = 0;
        if (!readVal(f, size) || size > maxSize || size % sizeof(typename Container::value_type) != 0) {
            return false;
        }
        out.resize(size / sizeof(typename Container::value_type));
        return size == 0 || fread(out.data(), 1, size, f) == size;
    }
} // namespace

bool ResumeCache::save(const std::string &path) const {
    const auto tmpPath = path + ".tmp";
    {
        FILE *raw = fopen(tmpPath.c_str(), "wb");
        if (!raw) {
            return false;
        }
        FilePtr f(raw, &fclose);
        bool ok = writeVal(raw, MAGIC) && writeVal(raw, VERSION) &&
                  writeVal(raw, targetFormat) &&
                  writeBytes(raw, discId.data(), discId.size()) &&
                  writeVal(raw, firstSectorHash) &&
                  writeVal(raw, totalSectorCount) &&
                  writeVal(raw, nextSector) &&
                  writeVal(raw, nextPart) &&
                  writeVal(raw, nextRun) &&
                  writeBytes(raw, skippedSectors.data(), skippedSectors.size() * sizeof(uint64_t)) &&
                  writeBytes(raw, hashState.data(), hashState.size()) &&
                  writeVal(raw, MAGIC);
        ok             = ok && fflush(raw) == 0;
        const bool cok = fclose(f.release()) == 0;
        if (!ok || !cok) {
            remove(tmpPath.c_str());
            return false;
        }
    }
    // FAT does not support renaming onto an existing file.
    remove(path.c_str());
    return rename(tmpPath.c_str(), path.c_str()) == 0;
}

std::optional<ResumeCache> ResumeCache::load(const std::string &path) {
    FILE *raw = fopen(path.c_str(), "rb");
    if (!raw) {
        return {};
    }
    FilePtr f(raw, &fclose);
    ResumeCache c;
    uint32_t magic = 0, version = 0, endMagic = 0;
    if (!readVal(raw, magic) || magic != MAGIC || !readVal(raw, version) || version != VERSION) {
        return {};
    }
    if (!readVal(raw, c.targetFormat) ||
        !readBytes(raw, c.discId, 64) ||
        !readVal(raw, c.firstSectorHash) ||
        !readVal(raw, c.totalSectorCount) ||
        !readVal(raw, c.nextSector) ||
        !readVal(raw, c.nextPart) ||
        !readVal(raw, c.nextRun) ||
        !readBytes(raw, c.skippedSectors, 0x1000000) ||
        !readBytes(raw, c.hashState, 0x10000) ||
        !readVal(raw, endMagic) || endMagic != MAGIC) {
        return {};
    }
    if (c.nextSector == 0 || c.nextSector >= c.totalSectorCount || c.nextPart < 1) {
        return {};
    }
    return c;
}

bool AppResumeCache::save(const std::string &path) const {
    const auto tmpPath = path + ".tmp";
    {
        FILE *raw = fopen(tmpPath.c_str(), "wb");
        if (!raw) {
            return false;
        }
        FilePtr f(raw, &fclose);
        bool ok = writeVal(raw, MAGIC) && writeVal(raw, VERSION) &&
                  writeBytes(raw, discId.data(), discId.size()) &&
                  writeBytes(raw, volumeId.data(), volumeId.size()) &&
                  writeVal(raw, firstSectorHash) &&
                  writeVal(raw, titleId) &&
                  writeVal(raw, totalBytes) &&
                  writeVal(raw, nextContentIndex) &&
                  writeVal(raw, nextOffset) &&
                  writeVal(raw, nextPiece) &&
                  writeVal(raw, nextRun) &&
                  writeVal(raw, MAGIC);
        ok             = ok && fflush(raw) == 0;
        const bool cok = fclose(f.release()) == 0;
        if (!ok || !cok) {
            remove(tmpPath.c_str());
            return false;
        }
    }
    remove(path.c_str());
    return rename(tmpPath.c_str(), path.c_str()) == 0;
}

std::optional<AppResumeCache> AppResumeCache::load(const std::string &path) {
    FILE *raw = fopen(path.c_str(), "rb");
    if (!raw) {
        return {};
    }
    FilePtr f(raw, &fclose);
    AppResumeCache c;
    uint32_t magic = 0, version = 0, endMagic = 0;
    if (!readVal(raw, magic) || magic != MAGIC || !readVal(raw, version) || version != VERSION) {
        return {};
    }
    if (!readBytes(raw, c.discId, 64) ||
        !readBytes(raw, c.volumeId, 64) ||
        !readVal(raw, c.firstSectorHash) ||
        !readVal(raw, c.titleId) ||
        !readVal(raw, c.totalBytes) ||
        !readVal(raw, c.nextContentIndex) ||
        !readVal(raw, c.nextOffset) ||
        !readVal(raw, c.nextPiece) ||
        !readVal(raw, c.nextRun) ||
        !readVal(raw, endMagic) || endMagic != MAGIC) {
        return {};
    }
    if (c.nextPiece < 1 || c.nextRun < 2) {
        return {};
    }
    return c;
}
