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
#include "GMPartitionsDumperState.h"
#include <WUD/DiscReaderDiscDrive.h>
#include <WUD/NUSTitle.h>
#include <WUD/content/partitions/WiiUGMPartition.h>
#include <WUD/header/WiiUDiscHeader.h>
#include <common/common.h>
#include <fs/FSUtils.h>
#include <memory>
#include <mocha/fsa.h>
#include <mocha/mocha.h>
#include <cstdio>
#include <utils/StringTools.h>
#include <utils/utils.h>

#define READ_BUFFER_SIZE (SECTOR_SIZE * 128)

GMPartitionsDumperState::GMPartitionsDumperState(eDumpTarget pTargetDevice) : targetDevice(pTargetDevice) {
    this->sectorBufSize = SECTOR_SIZE;
    this->state         = STATE_OPEN_ODD1;
    gBlockHomeButton    = true;
    dumpStartDate       = OSGetTime();
}

GMPartitionsDumperState::~GMPartitionsDumperState() {
    if (this->oddFd != -1) {
        FSAEx_RawCloseEx(gFSAClientHandle, this->oddFd);
        this->oddFd = -1;
    }
    free(this->sectorBuf);
    this->sectorBuf = nullptr;
    free(this->readBuffer);
    this->readBuffer = nullptr;
    gBlockHomeButton = false;
}

void GMPartitionsDumperState::render() {
    WiiUScreen::clearScreen();
    ApplicationState::printHeader();

    if (this->state == STATE_ERROR) {
        WiiUScreen::drawLine();
        WiiUScreen::drawLinef("Error:       %s", ErrorMessage().c_str());
        WiiUScreen::drawLinef("Description: %s", ErrorDescription().c_str());
        WiiUScreen::drawLine();
        WiiUScreen::drawLine("Press A to return.");
    } else if (this->state == STATE_OPEN_ODD1) {
        WiiUScreen::drawLine("Open /dev/odd01");
    } else if (this->state == STATE_PLEASE_INSERT_DISC) {
        WiiUScreen::drawLine("Please insert a Wii U disc and try again.\n\nPress A to return");
    } else if (this->state == STATE_READ_DISC_INFO) {
        WiiUScreen::drawLine("Read disc information");
    } else if (this->state == STATE_READ_DISC_INFO_DONE) {
        WiiUScreen::drawLine("Read disc information done");
    } else if (this->state == STATE_READ_COMMON_KEY) {
        WiiUScreen::drawLine("Read common key");
    } else if (this->state == STATE_CREATE_DISC_READER) {
        WiiUScreen::drawLine("Create disc reader");
    } else if (this->state == STATE_PARSE_DISC_HEADER) {
        WiiUScreen::drawLine("Parse disc header");
    } else if (this->state == STATE_PROCESS_GM_PARTITIONS) {
        WiiUScreen::drawLine("Process partitions");
    } else if (this->state == STATE_CHOOSE_PARTITION_TO_DUMP) {
        WiiUScreen::drawLine("Choose the partition to dump:");
        WiiUScreen::drawLine();
        if (gmPartitionPairs.empty()) {
            WiiUScreen::drawLine("This disc has no dumpable GM partitions ");
        } else {
            uint32_t index = 0;
            for (auto &partitionPair : gmPartitionPairs) {
                uint64_t size = 0;
                for (auto &content : partitionPair.second->tmd->contentList) {
                    size += ROUNDUP(content->encryptedFileSize, 16);
                }
                std::string titleId = partitionPair.first->getVolumeId().substr(2, 18);
                std::string appType = "Other ";
                if (titleId.starts_with("00050000")) {
                    appType = "Game  ";
                } else if (titleId.starts_with("0005000C")) {
                    appType = "DLC   ";
                } else if (titleId.starts_with("0005000E")) {
                    appType = "Update";
                }
                WiiUScreen::drawLinef("%s %s - %s (~%0.2f GiB) (%s)", index == (uint32_t) selectedOptionY ? ">" : " ", appType.c_str(),
                                      partitionPair.second->getShortnameEn().c_str(),
                                      (float) ((float) size / 1024.0f / 1024.0f / 1024.0f), titleId.c_str());
                index++;
            }
            WiiUScreen::drawLine();
            WiiUScreen::drawLinef("%s Back", index == (uint32_t) selectedOptionY ? ">" : " ");
        }
    } else if (this->state == STATE_PLAN_DUMP) {
        WiiUScreen::drawLine("Checking free space...");
    } else if (this->state == STATE_ASK_USE_CACHE) {
        WiiUScreen::drawLinef("Partition %s", curPartition->getVolumeId().c_str());
        WiiUScreen::drawLine();
        WiiUScreen::drawLine("A resume cache for this partition was found:");
        WiiUScreen::drawLinef(" Run %d, continue at %.2f MiB / %.2f MiB", this->foundCache->nextRun,
                              positionOf(this->foundCache->nextContentIndex, this->foundCache->nextOffset) / 1024.0f / 1024.0f,
                              partitionTotalBytes / 1024.0f / 1024.0f);
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
        const uint64_t startPos = positionOf(runStartContentIndex, runStartOffset);
        const uint64_t endPos   = positionOf(runEndContentIndex, runEndOffset);
        WiiUScreen::drawLinef("Partition %s", curPartition->getVolumeId().c_str());
        WiiUScreen::drawLine();
        WiiUScreen::drawLinef("Dump size:  %10.2f MiB", (endPos - startPos) / 1024.0f / 1024.0f);
        if (freeSpaceKnown) {
            WiiUScreen::drawLinef("Free space: %10.2f MiB (of %.2f MiB)", freeBytesAtStart / 1024.0f / 1024.0f, deviceTotalBytes / 1024.0f / 1024.0f);
        } else {
            WiiUScreen::drawLine("Free space: unknown");
        }
        WiiUScreen::drawLinef("Range: %.2f - %.2f MiB of %.2f MiB", startPos / 1024.0f / 1024.0f, endPos / 1024.0f / 1024.0f, partitionTotalBytes / 1024.0f / 1024.0f);
        WiiUScreen::drawLine();
        if (this->runIsPartial) {
            WiiUScreen::drawLinef("Not enough space for the whole partition. This is run %d.", this->runNumber);
            WiiUScreen::drawLine("A resume cache will be saved to the drive when it ends.");
        } else if (startPos > 0) {
            WiiUScreen::drawLinef("Run %d: this is the final run.", this->runNumber);
        }
        WiiUScreen::drawLine();
        WiiUScreen::drawLine("Press A to start, B to return");
    } else if (this->state == STATE_CREATE_DATA_PROVIDER) {
        WiiUScreen::drawLine("Create data provider from partition");
    } else if (this->state == STATE_DUMP_PARTITION_TMD) {
        WiiUScreen::drawLine("Dump title.tmd");
    } else if (this->state == STATE_DUMP_PARTITION_TICKET) {
        WiiUScreen::drawLine("Dump title.tik");
    } else if (this->state == STATE_DUMP_PARTITION_CERT) {
        WiiUScreen::drawLine("Dump title.cert");
    } else if (this->state == STATE_DUMP_PARTITION_CONTENTS) {
        if (curPartition != nullptr) {
            WiiUScreen::drawLinef("Dumping Partition %s", curPartition->getVolumeId().c_str());
            WiiUScreen::drawLinef("Name:    %s", curNUSTitle->getLongnameEn().c_str(), curNUSTitle->tmd->titleId);
            WiiUScreen::drawLinef("TitleID: %016llX", curNUSTitle->tmd->titleId);
        } else {
            WiiUScreen::drawLine("Dumping Partition");
        }

        uint64_t partitionSize   = 0;
        uint64_t partitionOffset = curOffsetInContent;
        if (curNUSTitle != nullptr) {
            for (auto &content : curNUSTitle->tmd->contentList) {
                partitionSize += ROUNDUP(content->encryptedFileSize, 16);
                if (content->index < curContentIndex) {
                    partitionOffset += ROUNDUP(content->encryptedFileSize, 16);
                }
            }
            WiiUScreen::drawLinef("Total Progress: %.2f MiB / %.2f MiB (%0.2f%%)", partitionOffset / 1024.0f / 1024.0f,
                                  partitionSize / 1024.0f / 1024.0f, ((partitionOffset * 1.0f) / partitionSize) * 100.0f);
            WiiUScreen::drawLine();
        } else {
            WiiUScreen::drawLine();
            WiiUScreen::drawLine();
        }

        if (curNUSTitle != nullptr) {
            WiiUScreen::drawLinef("Dumping Content %d / %d", curContentIndex, curNUSTitle->tmd->contentList.size());
        } else {
            WiiUScreen::drawLine("Dumping Content ?? / ??");
        }

        auto offset = curOffsetInContent;
        auto size   = curContent != nullptr ? ROUNDUP(curContent->encryptedFileSize, 16) : 0;

        if (size > 0) {
            WiiUScreen::drawLinef("Progress: %.2f MiB / %.2f MiB (%0.2f%%)", offset / 1024.0f / 1024.0f,
                                  size / 1024.0f / 1024.0f, ((offset * 1.0f) / size) * 100.0f);
        } else {
            WiiUScreen::drawLine();
        }
        if (freeSpaceKnown) {
            WiiUScreen::drawLinef("%s: %.2f MiB free / %.2f MiB total", targetDevice == TARGET_NTFS ? "USB" : "SD",
                                  getLiveFreeBytes() / 1024.0f / 1024.0f, deviceTotalBytes / 1024.0f / 1024.0f);
        } else {
            WiiUScreen::drawLinef("%s: free space unknown", targetDevice == TARGET_NTFS ? "USB" : "SD");
        }
        if (this->runIsPartial) {
            WiiUScreen::drawLinef("Run %d stops at %.2f MiB -> resume cache will be saved", this->runNumber,
                                  positionOf(runEndContentIndex, runEndOffset) / 1024.0f / 1024.0f);
        } else if (runStartContentIndex > 0 || runStartOffset > 0) {
            WiiUScreen::drawLinef("Run %d (resumed), final run", this->runNumber);
        }
        WiiUScreen::drawLine();
        WiiUScreen::drawLine("Press X to abort the dumping");

    } else if (this->state == STATE_DUMP_DONE) {
        if (this->runIsPartial) {
            WiiUScreen::drawLinef("Run %d done (%.2f MiB / %.2f MiB). Press A to return.", this->runNumber,
                                  positionOf(runEndContentIndex, runEndOffset) / 1024.0f / 1024.0f, partitionTotalBytes / 1024.0f / 1024.0f);
            WiiUScreen::drawLine();
            WiiUScreen::drawLine("Resume cache saved: /" RESUME_CACHE_SUBDIR);
            if (!this->lastCutFileName.empty()) {
                WiiUScreen::drawLinef("Split file: %s", this->lastCutFileName.c_str());
            }
            WiiUScreen::drawLine("PC: move /wudump into a new folder per run (1, 2, ...).");
            WiiUScreen::drawLine("Keep /dumpling on the USB, then dump the same partition again.");
            WiiUScreen::drawLine("After the last run: start merge_wudd.bat on the PC.");
        } else {
            WiiUScreen::drawLine("Dumping done. Press A to return.");
            if (this->runNumber > 1) {
                WiiUScreen::drawLine();
                WiiUScreen::drawLine("After the last run: start merge_wudd.bat on the PC.");
            }
        }
    } else if (this->state == STATE_ABORT_CONFIRMATION) {
        WiiUScreen::drawLinef("Do you really want to abort the disc dumping?");
        WiiUScreen::drawLinef("");
        if (selectedOptionX == 0) {
            WiiUScreen::drawLinef("> Continue dumping     Abort dumping");
        } else {
            WiiUScreen::drawLinef("  Continue dumping   > Abort dumping");
        }
    }

    ApplicationState::printFooter();
    WiiUScreen::flipBuffers();
}

ApplicationState::eSubState GMPartitionsDumperState::update(Input *input) {
    if (this->state == STATE_RETURN) {
        return ApplicationState::SUBSTATE_RETURN;
    }
    if (this->state == STATE_ERROR) {
        if (entrySelected(input)) {
            return ApplicationState::SUBSTATE_RETURN;
        }
        return ApplicationState::SUBSTATE_RUNNING;
    }

    if (this->state == STATE_OPEN_ODD1) {
        auto ret = FSAEx_RawOpenEx(gFSAClientHandle, "/dev/odd01", &(this->oddFd));
        if (ret >= 0) {
            if (this->sectorBuf == nullptr) {
                this->sectorBuf = (void *) memalign(0x100, this->sectorBufSize);
                if (this->sectorBuf == nullptr) {
                    this->setError(ERROR_MALLOC_FAILED);
                    return ApplicationState::SUBSTATE_RUNNING;
                }
            }
            this->state = STATE_READ_DISC_INFO;
        } else {
            this->state = STATE_PLEASE_INSERT_DISC;
        }
    } else if (this->state == STATE_PLEASE_INSERT_DISC) {
        if (entrySelected(input)) {
            return ApplicationState::SUBSTATE_RETURN;
        }
    } else if (this->state == STATE_READ_DISC_INFO) {
        if (FSAEx_RawReadEx(gFSAClientHandle, this->sectorBuf, READ_SECTOR_SIZE, 1, 0, this->oddFd) >= 0) {
            this->discId[10] = '\0';
            memcpy(this->discId.data(), sectorBuf, 10);
            calculateHash256(static_cast<unsigned char *>(this->sectorBuf), SECTOR_SIZE, this->firstSectorHash.data());
            this->state = STATE_READ_DISC_INFO_DONE;
            return ApplicationState::SUBSTATE_RUNNING;
        }
        FSAEx_RawCloseEx(gFSAClientHandle, this->oddFd);
        this->oddFd = -1;

        this->setError(ERROR_READ_FIRST_SECTOR);
        return ApplicationState::SUBSTATE_RUNNING;
    } else if (this->state == STATE_READ_DISC_INFO_DONE) {
        this->state = STATE_READ_COMMON_KEY;
    } else if (this->state == STATE_READ_COMMON_KEY) {
        WiiUConsoleOTP otp;
        Mocha_ReadOTP(&otp);
        memcpy(cKey.data(), otp.wiiUBank.wiiUCommonKey, 0x10);
        this->state = STATE_CREATE_DISC_READER;
    } else if (this->state == STATE_CREATE_DISC_READER) {
        auto discReaderOpt = DiscReaderDiscDrive::make_unique();
        if (!discReaderOpt) {
            this->setError(ERROR_OPEN_ODD1);
            return SUBSTATE_RUNNING;
        }
        this->discReader = std::move(discReaderOpt.value());
        this->state      = STATE_PARSE_DISC_HEADER;
    } else if (this->state == STATE_PARSE_DISC_HEADER) {
        auto discHeaderOpt = WiiUDiscHeader::make_unique(discReader);
        if (!discHeaderOpt.has_value()) {
            DEBUG_FUNCTION_LINE_ERR("Failed to read DiscHeader");
            this->setError(ERROR_PARSE_DISC_HEADER);
            return SUBSTATE_RUNNING;
        }
        this->discHeader = std::move(discHeaderOpt.value());
        this->state      = STATE_PROCESS_GM_PARTITIONS;
    } else if (this->state == STATE_PROCESS_GM_PARTITIONS) {
        this->gmPartitionPairs.clear();
        for (auto &partition : discHeader->wiiUContentsInformation->partitions->partitions) {
            auto gmPartition = std::dynamic_pointer_cast<WiiUGMPartition>(partition);
            if (gmPartition != nullptr) {
                auto nusTitleOpt = NUSTitle::loadTitleFromGMPartition(gmPartition, discReader, cKey);
                if (!nusTitleOpt.has_value()) {
                    this->setError(ERROR_FAILED_TO_GET_NUSTITLE);
                    return SUBSTATE_RUNNING;
                }

                this->gmPartitionPairs.emplace_back(gmPartition, std::move(nusTitleOpt.value()));
            }
        }

        this->state = STATE_CHOOSE_PARTITION_TO_DUMP;
    } else if (this->state == STATE_CHOOSE_PARTITION_TO_DUMP) {
        if (gmPartitionPairs.empty()) {
            if (entrySelected(input)) {
                return SUBSTATE_RETURN;
            }
        }

        if (buttonPressed(input, Input::BUTTON_B)) {
            return SUBSTATE_RETURN;
        }

        proccessMenuNavigationY(input, (int32_t) gmPartitionPairs.size() + 1);
        if (entrySelected(input)) {
            if (selectedOptionY >= (int32_t) gmPartitionPairs.size()) {
                return SUBSTATE_RETURN;
            }
            auto gmPartitionPair = gmPartitionPairs[selectedOptionY];
            if (gmPartitionPair.first != nullptr) {
                this->curPartition = gmPartitionPair.first;
                this->curNUSTitle  = gmPartitionPair.second;
                this->dataProvider = this->curNUSTitle->dataProcessor->getDataProvider();
            } else {
                DEBUG_FUNCTION_LINE_ERR("Failed to find a GM partition");
                this->setError(ERROR_NO_GM_PARTITION);
                return SUBSTATE_RUNNING;
            }

            this->targetPath = string_format("%swudump/%s/%s", getPathForDevice(targetDevice).c_str(), getPathNameForDisc().c_str(), curPartition->getVolumeId().c_str());
            if (!FSUtils::CreateSubfolder(targetPath.c_str())) {
                this->setError(ERROR_CREATE_DIR);
                return SUBSTATE_RUNNING;
            }

            this->state = STATE_PLAN_DUMP;
        }
    } else if (this->state == STATE_PLAN_DUMP) {
        this->contentSizes.clear();
        this->partitionTotalBytes = 0;
        for (size_t i = 0; i < curNUSTitle->tmd->contentList.size(); i++) {
            auto contentOpt = curNUSTitle->tmd->getContentByIndex(i);
            if (!contentOpt) {
                this->setError(ERROR_FIND_CONTENT_BY_INDEX);
                return SUBSTATE_RUNNING;
            }
            const uint64_t size = ROUNDUP(contentOpt.value()->encryptedFileSize, 16);
            this->contentSizes.push_back(size);
            this->partitionTotalBytes += size;
        }

        this->freeSpaceKnown = FSUtils::getFreeSpace(getPathForDevice(targetDevice).c_str(), this->freeBytesAtStart, this->deviceTotalBytes);

        this->foundCache = AppResumeCache::load(getResumeCachePath());
        if (this->foundCache) {
            const auto &c = this->foundCache.value();
            if (c.discId != getPathNameForDisc() || c.volumeId != curPartition->getVolumeId() || c.firstSectorHash != this->firstSectorHash ||
                c.titleId != curNUSTitle->tmd->titleId || c.totalBytes != this->partitionTotalBytes ||
                c.nextContentIndex >= this->contentSizes.size() || c.nextOffset >= this->contentSizes[c.nextContentIndex]) {
                this->foundCache.reset();
            }
        }
        if (this->foundCache) {
            this->selectedOptionX = 0;
            this->state           = STATE_ASK_USE_CACHE;
            return SUBSTATE_RUNNING;
        }
        if (!planRun(0, 0)) {
            this->setError(ERROR_NOT_ENOUGH_SPACE);
            return SUBSTATE_RUNNING;
        }
        this->state = STATE_CONFIRM_PLAN;
    } else if (this->state == STATE_ASK_USE_CACHE) {
        if (buttonPressed(input, Input::BUTTON_B)) {
            return SUBSTATE_RETURN;
        }
        proccessMenuNavigationX(input, 2);
        if (entrySelected(input)) {
            const bool useCache   = this->selectedOptionX == 0;
            this->selectedOptionX = 0;
            bool ok;
            if (useCache) {
                ok = planRun(this->foundCache->nextContentIndex, this->foundCache->nextOffset);
                this->runStartPiece = this->foundCache->nextPiece;
                this->runNumber     = this->foundCache->nextRun;
            } else {
                ok = planRun(0, 0);
            }
            if (!ok) {
                this->setError(ERROR_NOT_ENOUGH_SPACE);
                return SUBSTATE_RUNNING;
            }
            this->state = STATE_CONFIRM_PLAN;
        }
    } else if (this->state == STATE_CONFIRM_PLAN) {
        if (buttonPressed(input, Input::BUTTON_B)) {
            return SUBSTATE_RETURN;
        }
        if (entrySelected(input)) {
            this->state = STATE_DUMP_PARTITION_TMD;
        }
    } else if (this->state == STATE_DUMP_PARTITION_TMD) {
        std::vector<uint8_t> wBuffer;
        if (dataProvider->getRawTMD(wBuffer)) {
            std::string fileName = targetPath + "/" + WUD_TMD_FILENAME;
            if (FSUtils::saveBufferToFile(fileName.c_str(), wBuffer.data(), wBuffer.size()) != (int32_t) wBuffer.size()) {
                this->setError(ERROR_FAILED_WRITE_TMD);
                return SUBSTATE_RUNNING;
            }
            wBuffer.clear();
        }
        this->state = STATE_DUMP_PARTITION_TICKET;
    } else if (this->state == STATE_DUMP_PARTITION_TICKET) {
        std::vector<uint8_t> wBuffer;
        if (dataProvider->getRawTicket(wBuffer)) {
            std::string fileName = targetPath + "/" + WUD_TICKET_FILENAME;
            if (FSUtils::saveBufferToFile(fileName.c_str(), wBuffer.data(), wBuffer.size()) != (int32_t) wBuffer.size()) {
                this->setError(ERROR_FAILED_WRITE_TICKET);
                return SUBSTATE_RUNNING;
            }
            wBuffer.clear();
        } else {
            this->setError(ERROR_FAILED_WRITE_TICKET);
            return SUBSTATE_RUNNING;
        }
        this->state = STATE_DUMP_PARTITION_CERT;
    } else if (this->state == STATE_DUMP_PARTITION_CERT) {
        std::vector<uint8_t> wBuffer;
        if (dataProvider->getRawCert(wBuffer)) {
            std::string fileName = targetPath + "/" + WUD_CERT_FILENAME;
            if (FSUtils::saveBufferToFile(fileName.c_str(), wBuffer.data(), wBuffer.size()) != (int32_t) wBuffer.size()) {
                this->setError(ERROR_FAILED_WRITE_CERT);
                return SUBSTATE_RUNNING;
            }
            wBuffer.clear();
        } else {
            this->setError(ERROR_FAILED_WRITE_TICKET);
            return SUBSTATE_RUNNING;
        }
        this->curContentIndex     = this->runStartContentIndex;
        this->curOffsetInContent  = this->runStartOffset;
        this->bytesWrittenThisRun = 0;
        this->state               = STATE_DUMP_PARTITION_CONTENTS;
    } else if (this->state == STATE_DUMP_PARTITION_CONTENTS) {
        if (buttonPressed(input, Input::BUTTON_X)) {
            this->state = STATE_ABORT_CONFIRMATION;
            return ApplicationState::SUBSTATE_RUNNING;
        }
        if (this->runIsPartial && curContentIndex == this->runEndContentIndex && curOffsetInContent >= this->runEndOffset) {
            if (this->contentFile != nullptr) {
                this->contentFile->close();
                this->contentFile.reset();
            }
            this->curContent.reset();
            if (!saveResumeCache()) {
                this->setError(ERROR_CACHE_WRITE_FAILED);
                return SUBSTATE_RUNNING;
            }
            this->state = STATE_DUMP_DONE;
            return SUBSTATE_RUNNING;
        }
        // Get current content by index.
        if (curContent == nullptr) {
            auto curContentOpt = curNUSTitle->tmd->getContentByIndex(curContentIndex);
            if (!curContentOpt.has_value()) {
                this->setError(ERROR_FIND_CONTENT_BY_INDEX);
                return SUBSTATE_RUNNING;
            }
            this->curContent = curContentOpt.value();
        }

        // Check if we're done reading the file.
        uint64_t size = ROUNDUP(curContent->encryptedFileSize, 16);
        if (curOffsetInContent >= size) {
            // Close current file
            this->contentFile->close();
            // Free content
            this->contentFile.reset();
            this->curContent.reset();

            curOffsetInContent = 0;

            auto contentListSize = curNUSTitle->tmd->contentList.size();
            if (curContentIndex + 1 < (int32_t) contentListSize) {
                curContentIndex++;
                return SUBSTATE_RUNNING;
            } else {
                // Partition complete, a resume cache for it is not needed anymore.
                remove(getResumeCachePath().c_str());
                this->state = STATE_DUMP_DONE;
                return SUBSTATE_RUNNING;
            }
        }

        // Open the file if needed
        if (this->contentFile == nullptr) {
            char bufApp[32];
            if (curOffsetInContent > 0) {
                // continuation of a content that was started in a previous run
                this->curPieceNumber = this->runStartPiece;
            } else if (this->runIsPartial && curContentIndex == this->runEndContentIndex && this->runEndOffset > 0) {
                // this content will be cut at the end of this run
                this->curPieceNumber = 1;
            } else {
                this->curPieceNumber = 0;
            }
            if (this->curPieceNumber > 0) {
                snprintf(bufApp, 31, "%08X.app.part%02d", curContent->ID, this->curPieceNumber);
            } else {
                snprintf(bufApp, 31, "%08X.app", curContent->ID);
            }
            std::string appFileName = targetPath + "/" + bufApp;

            this->contentFile = std::make_unique<CFile>(appFileName, CFile::WriteOnly);
            if (!contentFile->isOpen()) {
                this->contentFile.reset();
                this->setError(ERROR_FAILED_CREATE_FILE);
                return SUBSTATE_RUNNING;
            }

            // dump .h3 file as well (only once per content)
            std::vector<uint8_t> h3Data;
            if (curOffsetInContent == 0 && dataProvider->getContentH3Hash(this->curContent, h3Data)) {
                snprintf(bufApp, 31, "%08X.h3", curContent->ID);
                std::string h3FileName = targetPath + "/" + bufApp;
                if (FSUtils::saveBufferToFile(h3FileName.c_str(), h3Data.data(), h3Data.size()) != (int32_t) h3Data.size()) {
                    this->setError(ERROR_FAILED_WRITE_H3);
                    return SUBSTATE_RUNNING;
                }
            }
        }

        // alloc readBuffer if needed
        if (this->readBuffer == nullptr) {
            readBuffer = (uint8_t *) memalign(0x40, ROUNDUP(READ_BUFFER_SIZE, 0x40));
            if (readBuffer == nullptr) {
                this->setError(ERROR_MALLOC_FAILED);
                return SUBSTATE_RUNNING;
            }
        }

        uint64_t readEnd = size;
        if (this->runIsPartial && curContentIndex == this->runEndContentIndex && this->runEndOffset < readEnd) {
            readEnd = this->runEndOffset;
        }
        uint32_t toRead = readEnd - curOffsetInContent > READ_BUFFER_SIZE ? READ_BUFFER_SIZE : (uint32_t) (readEnd - curOffsetInContent);

        if (!dataProvider->readRawContent(curContent, readBuffer, curOffsetInContent, toRead)) {
            this->setError(ERROR_READ_CONTENT);
            return SUBSTATE_RUNNING;
        }

        if (contentFile->write((const uint8_t *) readBuffer, toRead) != (int32_t) toRead) {
            this->setError(ERROR_WRITE_CONTENT);
            return SUBSTATE_RUNNING;
        }
        curOffsetInContent += toRead;
        this->bytesWrittenThisRun += toRead;

        // Go on!
        this->state = STATE_DUMP_PARTITION_CONTENTS;
        return ApplicationState::SUBSTATE_RUNNING;
    } else if (this->state == STATE_ABORT_CONFIRMATION) {
        if (buttonPressed(input, Input::BUTTON_B)) {
            this->state = STATE_DUMP_PARTITION_CONTENTS;
            return ApplicationState::SUBSTATE_RUNNING;
        }
        proccessMenuNavigationX(input, 2);
        if (buttonPressed(input, Input::BUTTON_A)) {
            if (selectedOptionX == 0) {
                this->state = STATE_DUMP_PARTITION_CONTENTS;
                return ApplicationState::SUBSTATE_RUNNING;
            } else {
                return ApplicationState::SUBSTATE_RETURN;
            }
        }
    } else if (state == STATE_DUMP_DONE) {
        if (entrySelected(input)) {
            return ApplicationState::SUBSTATE_RETURN;
        }
    }

    return ApplicationState::SUBSTATE_RUNNING;
}

void GMPartitionsDumperState::setError(GMPartitionsDumperState::eErrorState err) {
    this->state      = STATE_ERROR;
    this->errorState = err;
    //OSEnableHomeButtonMenu(true);
}

std::string GMPartitionsDumperState::getPathForDevice(eDumpTarget target) const {
    if (target == TARGET_NTFS) {
        return "ntfs0:/";
    }
    return "fs:/vol/external01/";
}

std::string GMPartitionsDumperState::ErrorMessage() const {
    switch (this->errorState) {
        case ERROR_NONE:
            return "ERROR_NONE";
        case ERROR_MALLOC_FAILED:
            return "ERROR_MALLOC_FAILED";
        case ERROR_READ_FIRST_SECTOR:
            return "ERROR_READ_FIRST_SECTOR";
        case ERROR_OPEN_ODD1:
            return "ERROR_OPEN_ODD1";
        case ERROR_PARSE_DISC_HEADER:
            return "ERROR_PARSE_DISC_HEADER";
        case ERROR_NO_GM_PARTITION:
            return "ERROR_NO_GM_PARTITION";
        case ERROR_CREATE_DIR:
            return "ERROR_CREATE_DIR";
        case ERROR_FAILED_TO_GET_NUSTITLE:
            return "ERROR_FAILED_TO_GET_NUSTITLE";
        case ERROR_FAILED_WRITE_TMD:
            return "ERROR_FAILED_WRITE_TMD";
        case ERROR_FAILED_WRITE_TICKET:
            return "ERROR_FAILED_WRITE_TICKET";
        case ERROR_FAILED_WRITE_CERT:
            return "ERROR_FAILED_WRITE_CERT";
        case ERROR_FIND_CONTENT_BY_INDEX:
            return "ERROR_FIND_CONTENT_BY_INDEX";
        case ERROR_FAILED_CREATE_FILE:
            return "ERROR_FAILED_CREATE_FILE";
        case ERROR_FAILED_WRITE_H3:
            return "ERROR_FAILED_WRITE_H3";
        case ERROR_READ_CONTENT:
            return "ERROR_READ_CONTENT";
        case ERROR_WRITE_CONTENT:
            return "ERROR_WRITE_CONTENT";
        case ERROR_NOT_ENOUGH_SPACE:
            return "ERROR_NOT_ENOUGH_SPACE";
        case ERROR_CACHE_WRITE_FAILED:
            return "ERROR_CACHE_WRITE_FAILED";
    }
    return "UNKNOWN_ERROR";
}

std::string GMPartitionsDumperState::ErrorDescription() const {
    switch (this->errorState) {
        case ERROR_NONE:
            break;
        case ERROR_MALLOC_FAILED:
            return "Failed to allocate memory.";
        case ERROR_READ_FIRST_SECTOR:
            return "Failed to read first sector.";
        case ERROR_OPEN_ODD1:
            return "Failed to read from disc.";
        case ERROR_PARSE_DISC_HEADER:
            return "Failed to parse the disc header.";
        case ERROR_NO_GM_PARTITION:
            return "No games were found on this disc.";
        case ERROR_CREATE_DIR:
            return "Failed to create a directory. \nMake sure to have enough space on the storage device.";
        case ERROR_FAILED_TO_GET_NUSTITLE:
            return "Failed to parse partition as NUSTitle";
        case ERROR_FAILED_WRITE_TMD:
            return "Failed to dump the title.tmd";
        case ERROR_FAILED_WRITE_TICKET:
            return "Failed to dump the title.tik";
        case ERROR_FAILED_WRITE_CERT:
            return "Failed to dump the title.cert";
        case ERROR_FIND_CONTENT_BY_INDEX:
            return "Failed to find requested index";
        case ERROR_FAILED_CREATE_FILE:
            return "Failed to create a file. \nMake sure to have enough space on the storage device.";
        case ERROR_FAILED_WRITE_H3:
            return "Failed to dump the a .h3 file";
        case ERROR_READ_CONTENT:
            return "Failed to read the a .app file from disc";
        case ERROR_WRITE_CONTENT:
            return "Failed to dump a .app. \nMake sure to have enough space on the storage device.";
        case ERROR_NOT_ENOUGH_SPACE:
            return "Not enough free space on the target device.";
        case ERROR_CACHE_WRITE_FAILED:
            return "Failed to save the resume cache to the target drive.";
    }
    return "UNKNOWN_ERROR";
}

std::string GMPartitionsDumperState::getPathNameForDisc() {
    if (this->discId[0] == '\0') {
        OSCalendarTime tm;
        OSTicksToCalendarTime(this->dumpStartDate, &tm);
        return string_format("DISC-%04d-%02d-%02d-%02d-%02d-%02d",
                             tm.tm_year, tm.tm_mon + 1, tm.tm_mday,
                             tm.tm_hour, tm.tm_min, tm.tm_sec);
    }
    return std::string((char *) &discId[0]);
}

uint64_t GMPartitionsDumperState::positionOf(uint16_t index, uint64_t offset) const {
    uint64_t pos = 0;
    for (size_t i = 0; i < index && i < contentSizes.size(); i++) {
        pos += contentSizes[i];
    }
    return pos + offset;
}

bool GMPartitionsDumperState::planRun(uint16_t startIndex, uint64_t startOffset) {
    this->runStartContentIndex = startIndex;
    this->runStartOffset       = startOffset;
    this->runEndContentIndex   = (uint16_t) contentSizes.size();
    this->runEndOffset         = 0;
    this->runIsPartial         = false;
    this->runStartPiece        = 1;
    this->runNumber            = 1;
    this->lastCutFileName.clear();

    const uint64_t startPos = positionOf(startIndex, startOffset);
    const uint64_t remaining = partitionTotalBytes - startPos;
    if (!freeSpaceKnown || remaining + FREE_SPACE_MARGIN <= freeBytesAtStart) {
        return true;
    }
    const uint64_t usable = freeBytesAtStart > FREE_SPACE_MARGIN ? freeBytesAtStart - FREE_SPACE_MARGIN : 0;
    const uint64_t endPos = startPos + usable;

    // Find the content that contains endPos and cut it at a multiple of the read size,
    // so the next run reads exactly the same blocks a normal dump would.
    uint64_t contentStart = 0;
    for (size_t i = 0; i < contentSizes.size(); i++) {
        if (endPos < contentStart + contentSizes[i]) {
            uint64_t offset = endPos - contentStart;
            offset -= offset % READ_BUFFER_SIZE;
            if (i < startIndex || (i == startIndex && offset <= startOffset)) {
                return false; // not even one block fits
            }
            this->runEndContentIndex = (uint16_t) i;
            this->runEndOffset       = offset;
            this->runIsPartial       = true;
            return true;
        }
        contentStart += contentSizes[i];
    }
    return true;
}

bool GMPartitionsDumperState::saveResumeCache() {
    AppResumeCache cache;
    cache.discId           = getPathNameForDisc();
    cache.volumeId         = curPartition->getVolumeId();
    cache.firstSectorHash  = this->firstSectorHash;
    cache.titleId          = curNUSTitle->tmd->titleId;
    cache.totalBytes       = this->partitionTotalBytes;
    cache.nextContentIndex = this->runEndContentIndex;
    cache.nextOffset       = this->runEndOffset;
    cache.nextPiece        = this->runEndOffset > 0 ? this->curPieceNumber + 1 : 1;
    cache.nextRun          = this->runNumber + 1;
    if (this->runEndOffset > 0) {
        auto contentOpt = curNUSTitle->tmd->getContentByIndex(this->runEndContentIndex);
        if (contentOpt) {
            this->lastCutFileName = string_format("%08X.app.part%02d", contentOpt.value()->ID, this->curPieceNumber);
        }
    }
    if (!FSUtils::CreateSubfolder((getPathForDevice(targetDevice) + RESUME_CACHE_SUBDIR).c_str())) {
        return false;
    }
    return cache.save(getResumeCachePath());
}

std::string GMPartitionsDumperState::getResumeCachePath() {
    return string_format("%s%s/%s_%s.app.bin", getPathForDevice(targetDevice).c_str(), RESUME_CACHE_SUBDIR, getPathNameForDisc().c_str(), curPartition->getVolumeId().c_str());
}

uint64_t GMPartitionsDumperState::getLiveFreeBytes() const {
    return freeBytesAtStart > bytesWrittenThisRun ? freeBytesAtStart - bytesWrittenThisRun : 0;
}
