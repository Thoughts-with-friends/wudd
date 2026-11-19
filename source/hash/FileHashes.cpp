#include "FileHashes.h"

#include <coreinit/cache.h>

#include <utility>

#include <cstring>
#include <type_traits>

static_assert(std::is_trivially_copyable_v<CRC32>);
static_assert(std::is_trivially_copyable_v<MD5>);
static_assert(std::is_trivially_copyable_v<SHA1>);
static_assert(std::is_trivially_copyable_v<SHA256>);

FileHashes::FileHashes() {
    reset();
}

FileHashes::~FileHashes() = default;

void FileHashes::reset() {
    mDigestCrc32.reset();
    mDigestMd5.reset();
    mDigestSha1.reset();
    mDigestSha2.reset();

    mHashThread = std::jthread(&FileHashes::hashWorker, this);
    OSMemoryBarrier();
}

// Background worker loop
void FileHashes::hashWorker(const std::stop_token &stoken) {
    while (true) {
        HashJob job;
        {
            std::unique_lock lock(mQueueMutex);

            // Wait until there is work in the queue OR a stop is requested.
            // cv.wait returns false if a stop was requested AND the predicate (queue not empty) is false.
            const bool hasWork = mQueueCv.wait(lock, stoken, [this] { return !mHashQueue.empty(); });

            if (!hasWork) {
                // Thread was asked to stop, and the queue is completely empty. We are done.
                break;
            }

            // Grab the job and remove it from the queue
            job = std::move(mHashQueue.front());
            mHashQueue.pop();
        }

        mDigestCrc32.add(job.buffer.get(), job.size);
        mDigestMd5.add(job.buffer.get(), job.size);
        mDigestSha1.add(job.buffer.get(), job.size);
        mDigestSha2.add(job.buffer.get(), job.size);
        OSMemoryBarrier();
    }
}

void FileHashes::processHashes(const uint8_t *buffer, uint32_t bufferSize) {
    auto memory_cpy = std::make_unique<uint8_t[]>(bufferSize);
    memcpy(memory_cpy.get(), buffer, bufferSize);
    {
        std::lock_guard lock(mQueueMutex);
        mHashQueue.push(HashJob{std::move(memory_cpy), bufferSize});
        OSMemoryBarrier();
    }
    mQueueCv.notify_one();
}

void FileHashes::finalize() {
    mHashThread.request_stop();

    if (mHashThread.joinable()) {
        mHashThread.join();
    }

    mHashCRC32Opt = mDigestCrc32.getHash();
    mHashMD5Opt   = mDigestMd5.getHash();
    mHashSHA1Opt  = mDigestSha1.getHash();
    mHashSHA2Opt  = mDigestSha2.getHash();
}

std::map<FileHashes::HashType, std::string> FileHashes::getHashes() {
    std::map<HashType, std::string> result;
    if (mHashCRC32Opt) {
        result[HashType::CRC32] = mDigestCrc32.getHash();
    }
    if (mHashMD5Opt) {
        result[HashType::MD5] = mDigestMd5.getHash();
    }
    if (mHashSHA1Opt) {
        result[HashType::SHA1] = mDigestSha1.getHash();
    }
    if (mHashSHA2Opt) {
        result[HashType::SHA256] = mDigestSha2.getHash();
    }
    return result;
}
std::vector<uint8_t> FileHashes::exportState() const {
    std::vector<uint8_t> out(sizeof(CRC32) + sizeof(MD5) + sizeof(SHA1) + sizeof(SHA256));
    auto *ptr = out.data();
    memcpy(ptr, &mDigestCrc32, sizeof(CRC32));
    ptr += sizeof(CRC32);
    memcpy(ptr, &mDigestMd5, sizeof(MD5));
    ptr += sizeof(MD5);
    memcpy(ptr, &mDigestSha1, sizeof(SHA1));
    ptr += sizeof(SHA1);
    memcpy(ptr, &mDigestSha2, sizeof(SHA256));
    return out;
}

bool FileHashes::importState(const std::vector<uint8_t> &state) {
    if (state.size() != sizeof(CRC32) + sizeof(MD5) + sizeof(SHA1) + sizeof(SHA256)) {
        return false;
    }
    std::lock_guard lock(mQueueMutex);
    if (!mHashQueue.empty()) {
        return false;
    }
    const auto *ptr = state.data();
    memcpy(&mDigestCrc32, ptr, sizeof(CRC32));
    ptr += sizeof(CRC32);
    memcpy(&mDigestMd5, ptr, sizeof(MD5));
    ptr += sizeof(MD5);
    memcpy(&mDigestSha1, ptr, sizeof(SHA1));
    ptr += sizeof(SHA1);
    memcpy(&mDigestSha2, ptr, sizeof(SHA256));
    OSMemoryBarrier();
    return true;
}
