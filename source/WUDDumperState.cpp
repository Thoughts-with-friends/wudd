/****************************************************************************
 * Copyright (C) 2021 Maschell
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 ****************************************************************************/
#include "WUDDumperState.h"

#include "fs/IWUDFileWriter.h"
#include "fs/StubWudFileWriter.h"

#include <WUD/content/WiiUDiscContentsHeader.h>
#include <common/common.h>
#include <fs/FSUtils.h>
#include <cstdio>
#include <malloc.h>
#include <mocha/fsa.h>
#include <mocha/mocha.h>
#include <utils/StringTools.h>
#include <utils/WiiUScreen.h>
#include <utils/utils.h>

namespace {
    std::string hashToStr(const FileHashes::HashType type) {
        switch (type) {
            case FileHashes::HashType::CRC32:
                return "CRC32";
            case FileHashes::HashType::MD5:
                return "MD5";
            case FileHashes::HashType::SHA1:
                return "SHA1";
            case FileHashes::HashType::SHA256:
                return "SHA256";
            default:
                return "UNKWN";
        }
    }
} // namespace

WUDDumperState::WUDDumperState(WUDDumperState::eDumpTargetFormat pTargetFormat, eDumpTarget pTargetDevice)
    : targetFormat(pTargetFormat), targetDevice(pTargetDevice) {
    this->sectorBufSize = READ_SECTOR_SIZE * READ_NUM_SECTORS;
    this->state         = STATE_OPEN_ODD1;
    gBlockHomeButton    = true;
}

WUDDumperState::~WUDDumperState() {
    if (this->oddFd >= 0) {
        FSAEx_RawCloseEx(gFSAClientHandle, oddFd);
    }
    free(sectorBuf);
    free(emptySector);
    gBlockHomeButton = false;
}

ApplicationState::eSubState WUDDumperState::update(Input *input) {
    const auto getGameKeyPath = [&]() -> std::string {
        return string_format("%swudump/%s/game.key", getPathForDevice(targetDevice).c_str(), getPathNameForDisc().c_str());
    };

    if (this->state == STATE_ERROR) {
        if (entrySelected(input)) {
            return ApplicationState::SUBSTATE_RETURN;
        }
        return ApplicationState::SUBSTATE_RUNNING;
    }
    if (this->state == STATE_OPEN_ODD1) {
        if (this->currentSector > 0) {
            auto ret = FSAEx_RawOpenEx(gFSAClientHandle, "/dev/odd01", &(this->oddFd));
            if (ret >= 0) {
                // continue!
                this->state = STATE_DUMP_DISC;
            } else {
                this->oddFd = -1;
                this->state = STATE_WAIT_USER_ERROR_CONFIRM;
            }
            return ApplicationState::SUBSTATE_RUNNING;
        }
        if (this->retryCount-- <= 0) {
            this->state = STATE_PLEASE_INSERT_DISC;
            return ApplicationState::SUBSTATE_RUNNING;
        }
        auto ret = FSAEx_RawOpenEx(gFSAClientHandle, "/dev/odd01", &(this->oddFd));
        if (ret >= 0) {
            if (this->sectorBuf == nullptr) {
                this->sectorBuf = (void *) memalign(0x100, this->sectorBufSize);
                if (this->sectorBuf == nullptr) {
                    DEBUG_FUNCTION_LINE_ERR("ERROR_MALLOC_FAILED");
                    this->setError(ERROR_MALLOC_FAILED);
                    return ApplicationState::SUBSTATE_RUNNING;
                }
            }
            DEBUG_FUNCTION_LINE("Opened /dev/odd01 %d", this->oddFd);
            this->state = STATE_READ_DISC_INFO;
        }
    } else if (this->state == STATE_PLEASE_INSERT_DISC) {
        if (entrySelected(input)) {
            return SUBSTATE_RETURN;
        }
    } else if (this->state == STATE_READ_DISC_INFO) {
        if (FSAEx_RawReadEx(gFSAClientHandle, this->sectorBuf, READ_SECTOR_SIZE, 1, 0, this->oddFd) >= 0) {
            this->discId[10] = '\0';
            memcpy(this->discId.data(), sectorBuf, 10);
            calculateHash256(static_cast<unsigned char *>(this->sectorBuf), READ_SECTOR_SIZE, this->firstSectorHash.data());
            this->state = STATE_READ_DISC_INFO_DONE;
            return ApplicationState::SUBSTATE_RUNNING;
        }

        this->setError(ERROR_READ_FIRST_SECTOR);
        return ApplicationState::SUBSTATE_RUNNING;
    } else if (this->state == STATE_READ_DISC_INFO_DONE) {
        this->state = STATE_DUMP_DISC_KEY;
    } else if (this->state == STATE_DUMP_DISC_KEY) {
        this->dumpStartTicks = OSGetTime();

        // Read the WiiUDiscContentsHeader to determine if we need disckey and if it's the correct one.
        auto res         = FSAEx_RawReadEx(gFSAClientHandle, this->sectorBuf, READ_SECTOR_SIZE, 1, 3, this->oddFd);
        this->hasDiscKey = false;
        memset(this->currentDiscKey.key, 0, sizeof(this->currentDiscKey.key));
        if (res >= 0) {
            if (((uint32_t *) this->sectorBuf)[0] != WiiUDiscContentsHeader::MAGIC) {
                auto discKeyRes = Mocha_ODMGetDiscKey(&this->currentDiscKey);
                if (discKeyRes == MOCHA_RESULT_SUCCESS) {
                    this->hasDiscKey = true;
                }
            }
        }

        if (this->hasDiscKey && targetFormat != DUMP_STUB) {
            if (!FSUtils::CreateSubfolder(string_format("%swudump/%s", getPathForDevice(targetDevice).c_str(), getPathNameForDisc().c_str()).c_str())) {
                setError(ERROR_WRITE_FAILED);
                return SUBSTATE_RUNNING;
            }
            if (!FSUtils::saveBufferToFile(getGameKeyPath().c_str(), this->currentDiscKey.key, 16)) {
                setError(ERROR_WRITE_FAILED);
                return SUBSTATE_RUNNING;
            }
        }
        this->state = STATE_PLAN_DUMP;
    } else if (this->state == STATE_PLAN_DUMP) {
        this->totalSectorCount = (WUD_FILE_SIZE / SECTOR_SIZE);
        this->useFoundCache    = false;
        this->foundCache.reset();
        if (targetFormat == DUMP_STUB) {
            planRun(0);
            this->state = STATE_DUMP_DISC_START;
            return ApplicationState::SUBSTATE_RUNNING;
        }
        queryFreeSpace();
        if (targetFormat == DUMP_AS_WUD) {
            auto cache = ResumeCache::load(getResumeCachePath());
            if (cache && cache->targetFormat == static_cast<uint32_t>(targetFormat) && cache->discId == getPathNameForDisc() &&
                cache->firstSectorHash == this->firstSectorHash && cache->totalSectorCount == this->totalSectorCount) {
                this->foundCache      = std::move(cache);
                this->selectedOptionX = 0;
                this->state           = STATE_ASK_USE_CACHE;
                return ApplicationState::SUBSTATE_RUNNING;
            }
        }
        if (!planRun(0)) {
            setError(ERROR_NOT_ENOUGH_SPACE);
            return ApplicationState::SUBSTATE_RUNNING;
        }
        this->state = STATE_CONFIRM_PLAN;
    } else if (this->state == STATE_ASK_USE_CACHE) {
        if (buttonPressed(input, Input::BUTTON_B)) {
            return ApplicationState::SUBSTATE_RETURN;
        }
        proccessMenuNavigationX(input, 2);
        if (entrySelected(input)) {
            this->useFoundCache   = this->selectedOptionX == 0;
            this->selectedOptionX = 0;
            if (!planRun(this->useFoundCache ? this->foundCache->nextSector : 0)) {
                setError(ERROR_NOT_ENOUGH_SPACE);
                return ApplicationState::SUBSTATE_RUNNING;
            }
            if (this->useFoundCache) {
                this->runStartPart = this->foundCache->nextPart;
                this->runNumber    = this->foundCache->nextRun;
            }
            this->state = STATE_CONFIRM_PLAN;
        }
    } else if (this->state == STATE_CONFIRM_PLAN) {
        if (buttonPressed(input, Input::BUTTON_B)) {
            return ApplicationState::SUBSTATE_RETURN;
        }
        if (entrySelected(input)) {
            this->state = STATE_DUMP_DISC_START;
        }
    } else if (this->state == STATE_DUMP_DISC_START) {
        if (!FSUtils::CreateSubfolder(string_format("%swudump/%s", getPathForDevice(targetDevice).c_str(), getPathNameForDisc().c_str()).c_str())) {
            setError(ERROR_WRITE_FAILED);
            return ApplicationState::SUBSTATE_RUNNING;
        }
        this->skippedSectors.clear();
        if (this->useFoundCache) {
            this->skippedSectors = this->foundCache->skippedSectors;
        }
        if (targetFormat == DUMP_AS_WUX) {
            this->fileHandle = std::make_unique<WUXFileWriter>(string_format("%swudump/%s/game.wux", getPathForDevice(targetDevice).c_str(), getPathNameForDisc().c_str()).c_str(), READ_SECTOR_SIZE * WRITE_BUFFER_NUM_SECTORS,
                                                               SECTOR_SIZE, targetDevice == TARGET_SD);
        } else if (targetFormat == DUMP_AS_WUD) {
            this->fileHandle = std::make_unique<WUDFileWriter>(string_format("%swudump/%s/game.wud", getPathForDevice(targetDevice).c_str(), getPathNameForDisc().c_str()).c_str(), READ_SECTOR_SIZE * WRITE_BUFFER_NUM_SECTORS,
                                                               SECTOR_SIZE, this->forceSplit, this->runStartPart);
        } else if (targetFormat == DUMP_STUB) {
            this->fileHandle = std::make_unique<StubWudFileWriter>();
        }
        wudFileHashes.reset();
        this->hashesValid = true;
        if (this->useFoundCache) {
            // Continue the hashes of the previous runs.
            this->hashesValid = wudFileHashes.importState(this->foundCache->hashState);
        }
        if (!this->fileHandle->isReady()) {
            DEBUG_FUNCTION_LINE_ERR("File is not ready.");
            this->setError(ERROR_FILE_OPEN_FAILED);
            return ApplicationState::SUBSTATE_RUNNING;
        }

        this->state            = STATE_DUMP_DISC;
        this->currentSector    = this->runStartSector;
        this->writtenSectors   = 0;
        this->runStartTicks    = OSGetTime();
        this->resumeCacheSaved = false;
        this->retryCount       = 10;
        this->logFileName      = "";
        this->logFileSaved     = false;
    } else if (this->state == STATE_DUMP_DISC) {
        if (buttonPressed(input, Input::BUTTON_X)) {
            this->state = STATE_ABORT_CONFIRMATION;
            return ApplicationState::SUBSTATE_RUNNING;
        }

        size_t numSectors = this->currentSector + READ_NUM_SECTORS > this->runEndSector ? this->runEndSector - this->currentSector : READ_NUM_SECTORS;
        if ((this->readResult = FSAEx_RawReadEx(gFSAClientHandle, sectorBuf, READ_SECTOR_SIZE, numSectors, this->currentSector, this->oddFd)) >= 0) {
            auto curWrittenSectors = fileHandle->writeSector((const uint8_t *) this->sectorBuf, numSectors);
            if (curWrittenSectors < 0) {
                this->setError(ERROR_WRITE_FAILED);
                return ApplicationState::SUBSTATE_RUNNING;
            }
            wudFileHashes.processHashes(static_cast<const uint8_t *>(this->sectorBuf), numSectors * SECTOR_SIZE);
            currentSector += numSectors;
            this->writtenSectors += curWrittenSectors;

            this->retryCount = 10;
            if (this->currentSector >= this->runEndSector) {
                finishRun();
                return ApplicationState::SUBSTATE_RUNNING;
            }
        } else {
            this->state = STATE_WAIT_USER_ERROR_CONFIRM;
            if (this->oddFd >= 0) {
                FSAEx_RawCloseEx(gFSAClientHandle, this->oddFd);
                this->oddFd = -1;
            }
            return ApplicationState::SUBSTATE_RUNNING;
        }
    } else if (this->state == STATE_ABORT_CONFIRMATION) {
        if (buttonPressed(input, Input::BUTTON_B)) {
            this->state = STATE_DUMP_DISC;
            return ApplicationState::SUBSTATE_RUNNING;
        }
        proccessMenuNavigationX(input, 2);
        if (buttonPressed(input, Input::BUTTON_A)) {
            if (selectedOptionX == 0) {
                this->state = STATE_DUMP_DISC;
                return ApplicationState::SUBSTATE_RUNNING;
            } else {
                return ApplicationState::SUBSTATE_RETURN;
            }
        }
    } else if (this->state == STATE_WAIT_USER_ERROR_CONFIRM) {
        if (this->autoSkipOnError) {
            if (this->oddFd >= 0) {
                FSAEx_RawCloseEx(gFSAClientHandle, this->oddFd);
                this->oddFd = -1;
            }
        }
        if (this->autoSkipOnError || (input->data.buttons_d & Input::BUTTON_A)) {
            // this->log.fwrite("Skipped sector %d : 0x%ll016X-0x%ll016X, filled with 0's\n", this->currentSector, this->currentSector * READ_SECTOR_SIZE, (this->currentSector + 1) * READ_SECTOR_SIZE);
            this->state = STATE_OPEN_ODD1;
            this->skippedSectors.push_back(this->currentSector);
            // We can't use seek because we may have cached values.

            if (this->emptySector == nullptr) {
                this->emptySector = memalign(0x100, READ_SECTOR_SIZE);
                if (this->emptySector == nullptr) {
                    this->setError(ERROR_MALLOC_FAILED);
                    return ApplicationState::SUBSTATE_RUNNING;
                }
                memset(this->emptySector, 0, READ_SECTOR_SIZE);
            }

            auto curWrittenSectors = fileHandle->writeSector((uint8_t *) emptySector, 1);
            if (curWrittenSectors < 0) {
                this->setError(ERROR_WRITE_FAILED);
                return ApplicationState::SUBSTATE_RUNNING;
            }
            wudFileHashes.processHashes((uint8_t *) emptySector, SECTOR_SIZE);

            this->currentSector += 1;
            this->writtenSectors += curWrittenSectors;
            this->readResult = 0;
            if (this->currentSector >= this->runEndSector) {
                finishRun();
                return ApplicationState::SUBSTATE_RUNNING;
            }
        } else if (buttonPressed(input, Input::BUTTON_B)) {
            this->state      = STATE_OPEN_ODD1;
            this->readResult = 0;
        } else if (buttonPressed(input, Input::BUTTON_Y)) {
            this->autoSkipOnError = true;
        } else if (buttonPressed(input, Input::BUTTON_X)) {
            this->state = STATE_ABORT_CONFIRMATION;
            return ApplicationState::SUBSTATE_RUNNING;
        }
    } else if (this->state == STATE_DUMP_DISC_DONE_SAVING_REPORT) {
        WuddLog log;
        OSCalendarTime startTimeCalendar;
        OSTicksToCalendarTime(this->dumpStartTicks, &startTimeCalendar);
        log.startDatetime   = startTimeCalendar;
        log.targetFormat    = this->targetFormat;
        log.wuddVersion     = VERSION_STR " " VERSION_EXTRA;
        const auto curTime  = OSGetTime();
        const auto dumpTime = OSTicksToMilliseconds(curTime - runStartTicks);
        log.durationSeconds = static_cast<uint32_t>(dumpTime / 1000);

        if (this->hashesValid && !this->runIsPartial) {
            log.disc.hashes = this->wudFileHashes.getHashes();
        }
        log.disc.partial = this->runIsPartial;
        if (this->runIsPartial || this->runStartSector > 0) {
            log.disc.runInfo = string_format("Run %d: bytes 0x%llX - 0x%llX of 0x%llX%s", this->runNumber,
                                             static_cast<unsigned long long>(this->runStartSector * SECTOR_SIZE),
                                             static_cast<unsigned long long>(this->currentSector * SECTOR_SIZE),
                                             static_cast<unsigned long long>(this->totalSectorCount * SECTOR_SIZE),
                                             this->runIsPartial ? " (resume cache saved)" : " (final run)");
        }

        if (this->discId[0] != '\0') {
            log.disc.discIdOpt = std::string((char *) &discId[0]);
        } else {
            log.disc.discIdOpt = {};
        }

        log.disc.sizeBytes = this->currentSector * SECTOR_SIZE;
        log.disc.filepaths = this->fileHandle->getPaths();

        OSCalendarTime tm;
        OSTicksToCalendarTime(this->dumpStartTicks, &tm);
        const auto filename = string_format("%swudump/%s/%04d-%02d-%02d-%02d-%02d-%02d.txt", getPathForDevice(targetDevice).c_str(), getPathNameForDisc().c_str(),
                                            tm.tm_year, tm.tm_mon + 1, tm.tm_mday,
                                            tm.tm_hour, tm.tm_min, tm.tm_sec);

        log.disc.skippedSectors = this->skippedSectors;

        if (this->hasDiscKey) {
            FileHashes hashes;
            hashes.processHashes(this->currentDiscKey.key, sizeof(this->currentDiscKey.key));
            hashes.finalize();

            log.discKeyOpt = KeyInfo{
                    .filepath  = getGameKeyPath(),
                    .sizeBytes = 16,
                    .hashes    = hashes.getHashes()};
        } else {
            log.discKeyOpt = {};
        }
        this->logFileName = filename;
        StringTools::ReplaceStringInPlace(this->logFileName, "fs:/vol/external01", "sd:");
        this->logFileSaved = writeLogFile(filename.c_str(), log);

        this->state = STATE_DUMP_DISC_DONE;
        return ApplicationState::SUBSTATE_RUNNING;
    } else if (this->state == STATE_DUMP_DISC_DONE) {
        if (entrySelected(input)) {
            return SUBSTATE_RETURN;
        }

        return ApplicationState::SUBSTATE_RUNNING;
    }
    return ApplicationState::SUBSTATE_RUNNING;
}

void WUDDumperState::render() {
    const auto modeStr = [&]() -> const char * {
        switch (this->targetFormat) {
            case DUMP_AS_WUX:
            case DUMP_AS_WUD:
                break;
            case DUMP_STUB:
                return "Hashing";
        }
        return "Dumping";
    };
    WiiUScreen::clearScreen();
    ApplicationState::printHeader();
    if (this->state == STATE_ERROR) {
        WiiUScreen::drawLine();
        WiiUScreen::drawLinef("Error:       %s", ErrorMessage().c_str());
        WiiUScreen::drawLinef("Description: %s", ErrorDescription().c_str());
        WiiUScreen::drawLine();
        WiiUScreen::drawLine();
        WiiUScreen::drawLine("Press A to return.");
    } else if (this->state == STATE_OPEN_ODD1) {
        WiiUScreen::drawLine("Open /dev/odd01");
    } else if (this->state == STATE_PLEASE_INSERT_DISC) {
        WiiUScreen::drawLine("Please insert a Wii U disc and try again.\n\nPress A to return");
    } else if (this->state == STATE_DUMP_DISC_KEY) {
        WiiUScreen::drawLine("Read disc key");
    } else if (this->state == STATE_READ_DISC_INFO) {
        WiiUScreen::drawLine("Read disc information");
    } else if (this->state == STATE_READ_DISC_INFO_DONE) {
        WiiUScreen::drawLinef("%s: %s", modeStr(), getPathNameForDisc().c_str());
    } else if (this->state == STATE_PLAN_DUMP) {
        WiiUScreen::drawLine("Checking free space...");
    } else if (this->state == STATE_ASK_USE_CACHE) {
        WiiUScreen::drawLinef("%s: %s", modeStr(), getPathNameForDisc().c_str());
        WiiUScreen::drawLine();
        WiiUScreen::drawLine("A resume cache for this disc was found:");
        WiiUScreen::drawLinef(" Run %d, continue at %.2f MiB / %.2f MiB (part%02d)", this->foundCache->nextRun,
                              this->foundCache->nextSector * (SECTOR_SIZE / 1024.0f / 1024.0f), WUD_FILE_SIZE / 1024.0f / 1024.0f, this->foundCache->nextPart);
        WiiUScreen::drawLine();
        WiiUScreen::drawLine("Do you want to use the cache?");
        WiiUScreen::drawLine();
        if (selectedOptionX == 0) {
            WiiUScreen::drawLine("> Yes (continue)     No (start from 0)");
        } else {
            WiiUScreen::drawLine("  Yes (continue)   > No (start from 0)");
        }
        WiiUScreen::drawLine();
        WiiUScreen::drawLine("Press B to return");
    } else if (this->state == STATE_CONFIRM_PLAN) {
        WiiUScreen::drawLinef("%s: %s", modeStr(), getPathNameForDisc().c_str());
        WiiUScreen::drawLine();
        WiiUScreen::drawLinef("Dump size:  %10.2f MiB%s", getRunBytes() / 1024.0f / 1024.0f, targetFormat == DUMP_AS_WUX ? " (max, before compression)" : "");
        if (freeSpaceKnown) {
            WiiUScreen::drawLinef("Free space: %10.2f MiB (of %.2f MiB)", freeBytesAtStart / 1024.0f / 1024.0f, deviceTotalBytes / 1024.0f / 1024.0f);
        } else {
            WiiUScreen::drawLine("Free space: unknown");
        }
        WiiUScreen::drawLinef("Disc range: %.2f - %.2f MiB of %.2f MiB", this->runStartSector * (SECTOR_SIZE / 1024.0f / 1024.0f),
                              this->runEndSector * (SECTOR_SIZE / 1024.0f / 1024.0f), WUD_FILE_SIZE / 1024.0f / 1024.0f);
        WiiUScreen::drawLine();
        if (this->runIsPartial) {
            WiiUScreen::drawLinef("Not enough space for the whole disc. This is run %d.", this->runNumber);
            WiiUScreen::drawLine("A resume cache will be saved to the drive when it ends.");
        } else if (this->runStartSector > 0) {
            WiiUScreen::drawLinef("Run %d: this is the final run.", this->runNumber);
        }
        if (this->forceSplit && targetFormat == DUMP_AS_WUD) {
            WiiUScreen::drawLinef("Files: game.wud.part%02d, ...", this->runStartPart);
        }
        if (targetFormat == DUMP_AS_WUX && freeSpaceKnown && freeBytesAtStart < WUD_FILE_SIZE + FREE_SPACE_MARGIN) {
            WiiUScreen::drawLine("Warning: the WUX may not fit and can not be split");
            WiiUScreen::drawLine("over multiple drives. Use WUD mode in that case.");
        }
        WiiUScreen::drawLine();
        WiiUScreen::drawLine("Press A to start, B to return");
    } else if (this->state == STATE_DUMP_DISC_START || this->state == STATE_DUMP_DISC || this->state == STATE_WAIT_USER_ERROR_CONFIRM) {
        WiiUScreen::drawLinef("%s: %s", modeStr(), getPathNameForDisc().c_str());

        float percent = this->currentSector / (WUD_FILE_SIZE / READ_SECTOR_SIZE * 1.0f) * 100.0f;
        WiiUScreen::drawLinef("Progress: %0.2f MiB / %5.2f MiB (%2.1f %%)", this->currentSector * (READ_SECTOR_SIZE / 1024.0f / 1024.0f), WUD_FILE_SIZE / 1024.0f / 1024.0f, percent);
        if (targetFormat == DUMP_AS_WUX) {
            WiiUScreen::drawLinef("Written %0.2f MiB. Compression ratio 1:%0.2f", writtenSectors * (READ_SECTOR_SIZE / 1024.0f / 1024.0f),
                                  1.0f / (writtenSectors / (float) this->currentSector));
        }
        if (targetFormat != DUMP_STUB) {
            const char *deviceName = targetDevice == TARGET_NTFS ? "USB" : "SD";
            if (freeSpaceKnown) {
                WiiUScreen::drawLinef("%s: %.2f MiB free / %.2f MiB total", deviceName, getLiveFreeBytes() / 1024.0f / 1024.0f, deviceTotalBytes / 1024.0f / 1024.0f);
            } else {
                WiiUScreen::drawLinef("%s: free space unknown", deviceName);
            }
        }
        if (this->runIsPartial) {
            WiiUScreen::drawLinef("Run %d stops at %.2f MiB -> resume cache will be saved", this->runNumber, this->runEndSector * (SECTOR_SIZE / 1024.0f / 1024.0f));
        } else if (this->runStartSector > 0) {
            WiiUScreen::drawLinef("Run %d (resumed at %.2f MiB), final run", this->runNumber, this->runStartSector * (SECTOR_SIZE / 1024.0f / 1024.0f));
        }

        if (this->readResult < 0 || this->oddFd < 0) {
            WiiUScreen::drawLine();

            if (this->oddFd < 0) {
                WiiUScreen::drawLine("Failed to open disc, try again.");
            } else {
                WiiUScreen::drawLinef("Error: Failed to read sector - Error %d", this->readResult);
            }
            WiiUScreen::drawLine();
            WiiUScreen::drawLine("Press A to skip this sector (will be replaced by 0's)");
            WiiUScreen::drawLine("Press Y to activate auto-skip mode");
            WiiUScreen::drawLine("Press B to try again");
        } else {
            OSTime curTime       = OSGetTime();
            float remaining      = (this->runEndSector - this->currentSector) * (READ_SECTOR_SIZE / 1024.0f / 1024.0f);
            float elapsedSec     = OSTicksToMilliseconds(curTime - runStartTicks) / 1000.0f;
            float curSpeed       = elapsedSec > 0 ? (this->currentSector - this->runStartSector) * (READ_SECTOR_SIZE / 1024.0f / 1024.0f) / elapsedSec : 0.0f;
            int32_t remainingSec = curSpeed > 0 ? remaining / curSpeed : 0;
            int32_t minutes      = (remainingSec / 60) % 60;
            int32_t seconds      = remainingSec % 60;
            int32_t hours        = remainingSec / 3600;

            WiiUScreen::drawLinef("Speed: %.2f MiB/s ETA: %02dh %02dm %02ds", curSpeed, hours, minutes, seconds);
        }

        WiiUScreen::drawLine();
        if (!this->skippedSectors.empty()) {
            WiiUScreen::drawLinef("Skipped dumping %d sectors", this->skippedSectors.size());
            WiiUScreen::drawLine();
        }
        WiiUScreen::drawLinef("Press X to abort");
    } else if (this->state == STATE_ABORT_CONFIRMATION) {
        WiiUScreen::drawLinef("Do you really want to abort the disc dumping?");
        WiiUScreen::drawLinef("");
        if (selectedOptionX == 0) {
            WiiUScreen::drawLinef("> Continue dumping     Abort dumping");
        } else {
            WiiUScreen::drawLinef("  Continue dumping   > Abort dumping");
        }
    } else if (this->state == STATE_DUMP_DISC_DONE_SAVING_REPORT) {
        WiiUScreen::drawLine("Dumping done! Saving logs...");
    } else if (this->state == STATE_DUMP_DISC_DONE) {
        if (targetFormat == DUMP_STUB) {
            WiiUScreen::drawLinef("Hashing done! Press A to continue.");
        } else if (this->runIsPartial) {
            WiiUScreen::drawLinef("Run %d done (%.2f MiB / %.2f MiB). Press A to continue", this->runNumber,
                                  this->currentSector * (SECTOR_SIZE / 1024.0f / 1024.0f), WUD_FILE_SIZE / 1024.0f / 1024.0f);
            WiiUScreen::drawLine();
            WiiUScreen::drawLinef("Resume cache saved. Files: game.wud.part%02d - part%02d", this->runStartPart, this->fileHandle->getNextPart() - 1);
            WiiUScreen::drawLine("PC: move /wudump into a new folder per run (1, 2, ...).");
            WiiUScreen::drawLine("Keep /dumpling on the USB, then select \"Dump as WUD\" again.");
        } else {
            WiiUScreen::drawLinef("Dumping done! Press A to continue");
        }

        WiiUScreen::drawLine();

        if (logFileSaved) {
            WiiUScreen::drawLinef("Log saved:");
            WiiUScreen::drawLinef("%s", logFileName.c_str());
            WiiUScreen::drawLine();
        }

        if (this->runIsPartial) {
            ApplicationState::printFooter();
            WiiUScreen::flipBuffers();
            return;
        }
        if (!this->hashesValid) {
            WiiUScreen::drawLine("Hashes of uncompressed wud: not available");
        } else {
            WiiUScreen::drawLinef("Hashes of uncompressed wud:");
        }
        for (const auto &[fst, snd] : this->hashesValid ? this->wudFileHashes.getHashes() : std::map<FileHashes::HashType, std::string>{}) {
            if (fst != FileHashes::HashType::SHA256) {
                WiiUScreen::drawLinef("%s: \t %s", hashToStr(fst).c_str(), snd.c_str());
            } else {
                WiiUScreen::drawLinef("%s:", hashToStr(fst).c_str());
                WiiUScreen::drawLinef(" %s", snd.c_str());
            }
        }
    }

    ApplicationState::printFooter();
    WiiUScreen::flipBuffers();
}

void WUDDumperState::setError(WUDDumperState::eErrorState err) {
    this->state      = STATE_ERROR;
    this->errorState = err;
    //OSEnableHomeButtonMenu(true);
}

std::string WUDDumperState::ErrorMessage() const {
    switch (this->errorState) {
        case ERROR_READ_FIRST_SECTOR:
            return "ERROR_READ_FIRST_SECTOR";
        case ERROR_NONE:
            return "ERROR_NONE";
        case ERROR_FILE_OPEN_FAILED:
            return "ERROR_FILE_OPEN_FAILED";
        case ERROR_MALLOC_FAILED:
            return "ERROR_MALLOC_FAILED";
        case ERROR_WRITE_FAILED:
            return "ERROR_WRITE_FAILED";
        case ERROR_NO_DISC_FOUND:
            return "ERROR_NO_DISC_FOUND";
        case ERROR_NOT_ENOUGH_SPACE:
            return "ERROR_NOT_ENOUGH_SPACE";
        case ERROR_CACHE_WRITE_FAILED:
            return "ERROR_CACHE_WRITE_FAILED";
    }
    return "UNKNOWN_ERROR";
}

std::string WUDDumperState::ErrorDescription() const {
    switch (this->errorState) {
        case ERROR_READ_FIRST_SECTOR:
            return "Failed to read first sector.";
        case ERROR_NONE:
            return "ERROR_NONE";
        case ERROR_FILE_OPEN_FAILED:
            return "Failed to create file \nMake sure to have enough space on the storage device.";
        case ERROR_MALLOC_FAILED:
            return "Failed to allocate data.";
        case ERROR_WRITE_FAILED:
            return "Failed to write the file. \nMake sure to have enough space on the storage device.";
        case ERROR_NO_DISC_FOUND:
            return "Please insert a Wii U disc.";
        case ERROR_NOT_ENOUGH_SPACE:
            return "Not enough free space on the target device.";
        case ERROR_CACHE_WRITE_FAILED:
            return "Failed to save the resume cache to the target drive.";
    }
    return "UNKNOWN_ERROR";
}

std::string WUDDumperState::getPathForDevice(eDumpTarget target) const {
    if (target == TARGET_NTFS) {
        return "ntfs0:/";
    }
    return "fs:/vol/external01/";
}

std::string WUDDumperState::getPathNameForDisc() {
    if (this->discId[0] == '\0') {
        OSCalendarTime tm;
        OSTicksToCalendarTime(this->dumpStartTicks, &tm);
        return string_format("DISC-%04d-%02d-%02d-%02d-%02d-%02d",
                             tm.tm_year, tm.tm_mon + 1, tm.tm_mday,
                             tm.tm_hour, tm.tm_min, tm.tm_sec);
    }
    return std::string((char *) &discId[0]);
}

bool WUDDumperState::writeLogFile(const char *filepath, const WuddLog &log) {
    // Open the file in write mode ("w")
    FILE *file = fopen(filepath, "w");
    if (!file) {
        return false;
    }

    // Write Header
    fprintf(file, "-WUDD log file-\n");
    OSCalendarTime tm = log.startDatetime;
    const auto time   = string_format("%04d-%02d-%02d %02d:%02d:%02d",
                                      tm.tm_year, tm.tm_mon + 1, tm.tm_mday,
                                      tm.tm_hour, tm.tm_min, tm.tm_sec);
    fprintf(file, "Dump start datetime: %s\n", time.c_str());
    fprintf(file, "Duration: %d minutes %d seconds\n", log.durationSeconds / 60, log.durationSeconds % 60);
    fprintf(file, "WUDD version: %s\n\n", log.wuddVersion.c_str());

    fprintf(file, "--Disc info--\n");
    if (!log.disc.filepaths.empty()) {
        fprintf(file, "Filepaths:\n");
        for (const auto &path : log.disc.filepaths) {
            fprintf(file, "\t%s\n", path.c_str());
        }
    } else {
        fprintf(file, "No files written.\n");
    }
    fprintf(file, "Disc ID: %s\n", log.disc.discIdOpt.value_or("-").c_str());
    fprintf(file, "Size of original WUD in bytes: %llu\n", static_cast<unsigned long long>(log.disc.sizeBytes));

    if (!log.disc.runInfo.empty()) {
        fprintf(file, "%s\n", log.disc.runInfo.c_str());
    }

    fprintf(file, "Hashes of uncompressed WUD:\n");
    if (log.disc.hashes.empty()) {
        fprintf(file, log.disc.partial ? " Not available yet, the dump is not complete.\n" : " Not available.\n");
    }
    for (const auto &[fst, snd] : log.disc.hashes) {
        fprintf(file, " %s: %s\n", hashToStr(fst).c_str(), snd.c_str());
    }

    if (log.discKeyOpt) {
        fprintf(file, "\n--Disc key info--\n");
        if (log.targetFormat != DUMP_STUB) {
            fprintf(file, "Filename: %s\n", log.discKeyOpt->filepath.c_str());
        }
        fprintf(file, "Size: %lld\n", log.discKeyOpt->sizeBytes);
        fprintf(file, "Hashes:\n");
        for (const auto &[fst, snd] : log.discKeyOpt->hashes) {
            fprintf(file, " %s: %s\n", hashToStr(fst).c_str(), snd.c_str());
        }
    } else {
        fprintf(file, "\n--Disc key info--\n");
        fprintf(file, "No disc key saved\n");
    }

    if (!log.disc.skippedSectors.empty()) {
        fprintf(file, "\nSkipped sectors:\n");
        for (auto &sector : log.disc.skippedSectors) {
            fprintf(file, " Skipped sector %lld : 0x%ll016X-0x%ll016X, filled with 0's\n", sector, sector * READ_SECTOR_SIZE, (sector + 1) * READ_SECTOR_SIZE);
        }
    }

    // Always close the file to flush the buffer and release the handle
    return fclose(file) == 0;
}
void WUDDumperState::queryFreeSpace() {
    this->freeSpaceKnown = FSUtils::getFreeSpace(getPathForDevice(targetDevice).c_str(), this->freeBytesAtStart, this->deviceTotalBytes);
    DEBUG_FUNCTION_LINE("Free space known: %d, free: %llu, total: %llu", this->freeSpaceKnown, this->freeBytesAtStart, this->deviceTotalBytes);
}

bool WUDDumperState::planRun(uint64_t startSector) {
    this->runStartSector = startSector;
    this->runEndSector   = this->totalSectorCount;
    this->runIsPartial   = false;
    this->runStartPart   = 1;
    this->runNumber      = 1;
    this->forceSplit     = targetDevice == TARGET_SD;
    if (targetFormat != DUMP_AS_WUD) {
        return true;
    }
    const uint64_t remainingBytes = (this->totalSectorCount - startSector) * SECTOR_SIZE;
    if (this->freeSpaceKnown && remainingBytes + FREE_SPACE_MARGIN > this->freeBytesAtStart) {
        const uint64_t usable  = this->freeBytesAtStart > FREE_SPACE_MARGIN ? this->freeBytesAtStart - FREE_SPACE_MARGIN : 0;
        const uint64_t sectors = usable / SECTOR_SIZE;
        if (sectors == 0) {
            return false;
        }
        this->runEndSector = startSector + sectors;
        this->runIsPartial = true;
    }
    // Multiple runs always use numbered parts, so all files can simply be put together (in order) later.
    if (this->runIsPartial || startSector > 0) {
        this->forceSplit = true;
    }
    return true;
}

void WUDDumperState::finishRun() {
    if (!this->fileHandle->finalize()) {
        DEBUG_FUNCTION_LINE_ERR("Final flush failed");
        this->setError(ERROR_WRITE_FAILED);
        return;
    }
    this->wudFileHashes.finalize();

    if (this->runIsPartial) {
        ResumeCache cache;
        cache.targetFormat     = static_cast<uint32_t>(this->targetFormat);
        cache.discId           = getPathNameForDisc();
        cache.firstSectorHash  = this->firstSectorHash;
        cache.totalSectorCount = this->totalSectorCount;
        cache.nextSector       = this->currentSector;
        cache.nextPart         = this->fileHandle->getNextPart();
        cache.nextRun          = this->runNumber + 1;
        cache.skippedSectors   = this->skippedSectors;
        if (this->hashesValid) {
            cache.hashState = this->wudFileHashes.exportState();
        }
        if (!FSUtils::CreateSubfolder((getPathForDevice(targetDevice) + RESUME_CACHE_SUBDIR).c_str()) || !cache.save(getResumeCachePath())) {
            DEBUG_FUNCTION_LINE_ERR("Failed to save resume cache");
            this->setError(ERROR_CACHE_WRITE_FAILED);
            return;
        }
        this->resumeCacheSaved = true;
    } else if (this->targetFormat == DUMP_AS_WUD) {
        // The disc is complete, an old cache for it is not needed anymore.
        remove(getResumeCachePath().c_str());
    }
    this->state = STATE_DUMP_DISC_DONE_SAVING_REPORT;
}

std::string WUDDumperState::getResumeCachePath() {
    return string_format("%s%s/%s.bin", getPathForDevice(targetDevice).c_str(), RESUME_CACHE_SUBDIR, getPathNameForDisc().c_str());
}

uint64_t WUDDumperState::getRunBytes() const {
    return (this->runEndSector - this->runStartSector) * SECTOR_SIZE;
}

uint64_t WUDDumperState::getLiveFreeBytes() const {
    const uint64_t written = static_cast<uint64_t>(this->writtenSectors) * SECTOR_SIZE;
    return this->freeBytesAtStart > written ? this->freeBytesAtStart - written : 0;
}
