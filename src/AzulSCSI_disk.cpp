// This file implements the main SCSI disk emulation and data streaming.
// It is derived from disk.c in SCSI2SD V6.
//
//    Licensed under GPL v3.
//    Copyright (C) 2013 Michael McMaster <michael@codesrc.com>
//    Copyright (C) 2014 Doug Brown <doug@downtowndougbrown.com>
//    Copyright (C) 2022 Rabbit Hole Computing

#include "AzulSCSI_disk.h"
#include "AzulSCSI_log.h"
#include <minIni.h>
#include <string.h>
#include <SdFat.h>

extern "C" {
#include <time.h>
#include <sd.h>
}

/***********************/
/* Backing image files */
/***********************/

extern SdFs SD;
SdDevice sdDev = {2, 256 * 1024 * 1024 * 2}; /* For SCSI2SD */

struct image_config_t: public S2S_TargetCfg
{
    FsFile file;    
};

static image_config_t g_DiskImages[S2S_MAX_TARGETS];

bool scsiDiskOpenHDDImage(int target_idx, const char *filename, int scsi_id, int scsi_lun, int blocksize)
{
    image_config_t &img = g_DiskImages[target_idx];
    img.file = SD.open(filename, O_RDWR);

    if (img.file.isOpen())
    {
        img.bytesPerSector = blocksize;
        img.scsiSectors = img.file.size() / blocksize;
        img.scsiId = scsi_id | S2S_CFG_TARGET_ENABLED;
        img.sdSectorStart = 0;
        
        if (img.scsiSectors == 0)
        {
            azlog("---- Error: image file ", filename, " is empty");
            img.file.close();
            return false;
        }

        if (img.file.contiguousRange(NULL, NULL))
        {
            azlog("---- Image file is contiguous.");
        }
        else
        {
            azlog("---- WARNING: file ", filename, " is not contiguous. This will increase read latency.");
        }

        return true;
    }

    return false;
}

void scsiDiskLoadConfig(int target_idx)
{
    image_config_t &img = g_DiskImages[target_idx];
    char section[6] = "SCSI0";
    section[4] = '0' + target_idx;

    img.deviceType = ini_getl(section, "Type", S2S_CFG_FIXED, CONFIGFILE);
    img.deviceTypeModifier = ini_getl(section, "TypeModifier", 0, CONFIGFILE);
    img.sectorsPerTrack = ini_getl(section, "SectorsPerTrack", 18, CONFIGFILE);
    img.headsPerCylinder = ini_getl(section, "HeadsPerCylinder", 255, CONFIGFILE);
    img.quirks = ini_getl(section, "Quirks", S2S_CFG_QUIRKS_NONE, CONFIGFILE);
    
    char tmp[32];
    memset(tmp, 0, sizeof(tmp));
    ini_gets(section, "Vendor", DEFAULT_VENDOR, tmp, sizeof(tmp), CONFIGFILE);
    memcpy(img.vendor, tmp, 8);

    memset(tmp, 0, sizeof(tmp));
    ini_gets(section, "Product", DEFAULT_PRODUCT, tmp, sizeof(tmp), CONFIGFILE);
    memcpy(img.prodId, tmp, 16);

    memset(tmp, 0, sizeof(tmp));
    ini_gets(section, "Version", DEFAULT_VERSION, tmp, sizeof(tmp), CONFIGFILE);
    memcpy(img.revision, tmp, 4);
    
    memset(tmp, 0, sizeof(tmp));
    ini_gets(section, "Serial", DEFAULT_SERIAL, tmp, sizeof(tmp), CONFIGFILE);
    memcpy(img.serial, tmp, 16);
}

/*******************************/
/* Config handling for SCSI2SD */
/*******************************/

extern "C"
void s2s_configInit(S2S_BoardCfg* config)
{
    memset(config, 0, sizeof(S2S_BoardCfg));
    memcpy(config->magic, "BCFG", 4);
    config->flags = ini_getl("SCSI", "Flags", 0, CONFIGFILE);
    config->startupDelay = 0;
    config->selectionDelay = ini_getl("SCSI", "SelectionDelay", 255, CONFIGFILE);
    config->flags6 = ini_getl("SCSI", "Flags6", 0, CONFIGFILE);
    config->scsiSpeed = ini_getl("SCSI", "SCSISpeed", S2S_CFG_SPEED_ASYNC_50, CONFIGFILE);
}

extern "C"
void s2s_debugInit(void)
{
}

extern "C"
void s2s_configPoll(void)
{
}

extern "C"
void s2s_configSave(int scsiId, uint16_t byesPerSector)
{
    // Modification of config over SCSI bus is not implemented.
}

extern "C"
const S2S_TargetCfg* s2s_getConfigByIndex(int index)
{
    if (index < 0 || index >= S2S_MAX_TARGETS)
    {
        return NULL;
    }
    else
    {
        return &g_DiskImages[index];
    }
}

extern "C"
const S2S_TargetCfg* s2s_getConfigById(int scsiId)
{
    int i;
    for (i = 0; i < S2S_MAX_TARGETS; ++i)
    {
        const S2S_TargetCfg* tgt = s2s_getConfigByIndex(i);
        if ((tgt->scsiId & S2S_CFG_TARGET_ID_BITS) == scsiId)
        {
            return tgt;
        }
    }
    return NULL;
}

/**********************/
/* FormatUnit command */
/**********************/

// Callback once all data has been read in the data out phase.
static void doFormatUnitComplete(void)
{
    scsiDev.phase = STATUS;
}

static void doFormatUnitSkipData(int bytes)
{
    // We may not have enough memory to store the initialisation pattern and
    // defect list data.  Since we're not making use of it yet anyway, just
    // discard the bytes.
    scsiEnterPhase(DATA_OUT);
    int i;
    for (i = 0; i < bytes; ++i)
    {
        scsiReadByte();
    }
}

// Callback from the data out phase.
static void doFormatUnitPatternHeader(void)
{
    int defectLength =
        ((((uint16_t)scsiDev.data[2])) << 8) +
            scsiDev.data[3];

    int patternLength =
        ((((uint16_t)scsiDev.data[4 + 2])) << 8) +
        scsiDev.data[4 + 3];

        doFormatUnitSkipData(defectLength + patternLength);
        doFormatUnitComplete();
}

// Callback from the data out phase.
static void doFormatUnitHeader(void)
{
    int IP = (scsiDev.data[1] & 0x08) ? 1 : 0;
    int DSP = (scsiDev.data[1] & 0x04) ? 1 : 0;

    if (! DSP) // disable save parameters
    {
        // Save the "MODE SELECT savable parameters"
        s2s_configSave(
            scsiDev.target->targetId,
            scsiDev.target->liveCfg.bytesPerSector);
    }

    if (IP)
    {
        // We need to read the initialisation pattern header first.
        scsiDev.dataLen += 4;
        scsiDev.phase = DATA_OUT;
        scsiDev.postDataOutHook = doFormatUnitPatternHeader;
    }
    else
    {
        // Read the defect list data
        int defectLength =
            ((((uint16_t)scsiDev.data[2])) << 8) +
            scsiDev.data[3];
        doFormatUnitSkipData(defectLength);
        doFormatUnitComplete();
    }
}

/************************/
/* ReadCapacity command */
/************************/

static void doReadCapacity()
{
    uint32_t lba = (((uint32_t) scsiDev.cdb[2]) << 24) +
        (((uint32_t) scsiDev.cdb[3]) << 16) +
        (((uint32_t) scsiDev.cdb[4]) << 8) +
        scsiDev.cdb[5];
    int pmi = scsiDev.cdb[8] & 1;

    uint32_t capacity = getScsiCapacity(
        scsiDev.target->cfg->sdSectorStart,
        scsiDev.target->liveCfg.bytesPerSector,
        scsiDev.target->cfg->scsiSectors);

    if (!pmi && lba)
    {
        // error.
        // We don't do anything with the "partial medium indicator", and
        // assume that delays are constant across each block. But the spec
        // says we must return this error if pmi is specified incorrectly.
        scsiDev.status = CHECK_CONDITION;
        scsiDev.target->sense.code = ILLEGAL_REQUEST;
        scsiDev.target->sense.asc = INVALID_FIELD_IN_CDB;
        scsiDev.phase = STATUS;
    }
    else if (capacity > 0)
    {
        uint32_t highestBlock = capacity - 1;

        scsiDev.data[0] = highestBlock >> 24;
        scsiDev.data[1] = highestBlock >> 16;
        scsiDev.data[2] = highestBlock >> 8;
        scsiDev.data[3] = highestBlock;

        uint32_t bytesPerSector = scsiDev.target->liveCfg.bytesPerSector;
        scsiDev.data[4] = bytesPerSector >> 24;
        scsiDev.data[5] = bytesPerSector >> 16;
        scsiDev.data[6] = bytesPerSector >> 8;
        scsiDev.data[7] = bytesPerSector;
        scsiDev.dataLen = 8;
        scsiDev.phase = DATA_IN;
    }
    else
    {
        scsiDev.status = CHECK_CONDITION;
        scsiDev.target->sense.code = NOT_READY;
        scsiDev.target->sense.asc = MEDIUM_NOT_PRESENT;
        scsiDev.phase = STATUS;
    }
}

/*************************/
/* TestUnitReady command */
/*************************/

static int doTestUnitReady()
{
    int ready = 1;
    if (likely(blockDev.state == (DISK_PRESENT | DISK_INITIALISED) &&
		scsiDev.target->started))
    {
        // nothing to do.
    }
    else if (unlikely(!scsiDev.target->started))
    {
        ready = 0;
        scsiDev.status = CHECK_CONDITION;
        scsiDev.target->sense.code = NOT_READY;
        scsiDev.target->sense.asc = LOGICAL_UNIT_NOT_READY_INITIALIZING_COMMAND_REQUIRED;
        scsiDev.phase = STATUS;
    }
    else if (unlikely(!(blockDev.state & DISK_PRESENT)))
    {
        ready = 0;
        scsiDev.status = CHECK_CONDITION;
        scsiDev.target->sense.code = NOT_READY;
        scsiDev.target->sense.asc = MEDIUM_NOT_PRESENT;
        scsiDev.phase = STATUS;
    }
    else if (unlikely(!(blockDev.state & DISK_INITIALISED)))
    {
        ready = 0;
        scsiDev.status = CHECK_CONDITION;
        scsiDev.target->sense.code = NOT_READY;
        scsiDev.target->sense.asc = LOGICAL_UNIT_NOT_READY_CAUSE_NOT_REPORTABLE;
        scsiDev.phase = STATUS;
    }
    return ready;
}

/****************/
/* Seek command */
/****************/

static void doSeek(uint32_t lba)
{
    if (lba >=
        getScsiCapacity(
            scsiDev.target->cfg->sdSectorStart,
            scsiDev.target->liveCfg.bytesPerSector,
            scsiDev.target->cfg->scsiSectors)
        )
    {
        scsiDev.status = CHECK_CONDITION;
        scsiDev.target->sense.code = ILLEGAL_REQUEST;
        scsiDev.target->sense.asc = LOGICAL_BLOCK_ADDRESS_OUT_OF_RANGE;
        scsiDev.phase = STATUS;
    }
    else
    {
        if (unlikely(scsiDev.target->cfg->deviceType == S2S_CFG_FLOPPY_14MB) ||
            scsiDev.compatMode < COMPAT_SCSI2)
        {
            s2s_delay_ms(10);
        }
        else
        {
            s2s_delay_us(10);
        }
    }
}

/********************************************/
/* Transfer state for read / write commands */
/********************************************/

BlockDevice blockDev;
Transfer transfer;

/*****************/
/* Write command */
/*****************/

static void doWrite(uint32_t lba, uint32_t blocks)
{
    if (unlikely(scsiDev.target->cfg->deviceType == S2S_CFG_FLOPPY_14MB)) {
        // Floppies are supposed to be slow. Some systems can't handle a floppy
        // without an access time
        s2s_delay_ms(10);
    }

    uint32_t bytesPerSector = scsiDev.target->liveCfg.bytesPerSector;

    if (unlikely(blockDev.state & DISK_WP) ||
        unlikely(scsiDev.target->cfg->deviceType == S2S_CFG_OPTICAL))

    {
        scsiDev.status = CHECK_CONDITION;
        scsiDev.target->sense.code = ILLEGAL_REQUEST;
        scsiDev.target->sense.asc = WRITE_PROTECTED;
        scsiDev.phase = STATUS;
    }
    else if (unlikely(((uint64_t) lba) + blocks >
        getScsiCapacity(
            scsiDev.target->cfg->sdSectorStart,
            bytesPerSector,
            scsiDev.target->cfg->scsiSectors
            )
        ))
    {
        scsiDev.status = CHECK_CONDITION;
        scsiDev.target->sense.code = ILLEGAL_REQUEST;
        scsiDev.target->sense.asc = LOGICAL_BLOCK_ADDRESS_OUT_OF_RANGE;
        scsiDev.phase = STATUS;
    }
    else
    {
        transfer.multiBlock = true;
        transfer.lba = lba;
        transfer.blocks = blocks;
        transfer.currentBlock = 0;
        scsiDev.phase = DATA_OUT;
        scsiDev.dataLen = bytesPerSector;
        scsiDev.dataPtr = bytesPerSector;

        image_config_t &img = *(image_config_t*)scsiDev.target->cfg;
        if (!img.file.seek(transfer.lba * img.bytesPerSector))
        {
            azlog("Seek to ", transfer.lba, " failed for ", scsiDev.target->targetId);
            scsiDev.status = CHECK_CONDITION;
            scsiDev.target->sense.code = MEDIUM_ERROR;
            scsiDev.target->sense.asc = NO_SEEK_COMPLETE;
            scsiDev.phase = STATUS;
        }
    }
}

void diskDataOut_callback(uint32_t bytes_complete)
{
    if (scsiDev.dataPtr < scsiDev.dataLen)
    {
        // DMA is now writing to SD card.
        // We can use this time to transfer next block from SCSI bus.
        uint32_t len = scsiDev.dataLen - scsiDev.dataPtr;
        if (len > 512) len = 512;
        
        int parityError = 0;
        scsiRead(scsiDev.data + scsiDev.dataPtr, len, &parityError);
        scsiDev.dataPtr += len;

        if (parityError)
        {
            scsiDev.status = CHECK_CONDITION;
            scsiDev.target->sense.code = ABORTED_COMMAND;
            scsiDev.target->sense.asc = SCSI_PARITY_ERROR;
            scsiDev.phase = STATUS;
        }
    }
}

void diskDataOut()
{
    scsiEnterPhase(DATA_OUT);

    // Figure out how many blocks we can fit in buffer
    uint32_t blockcount = (transfer.blocks - transfer.currentBlock);
    uint32_t maxblocks = sizeof(scsiDev.data) / scsiDev.target->liveCfg.bytesPerSector;
    if (blockcount > maxblocks) blockcount = maxblocks;
    uint32_t transferlen = blockcount * scsiDev.target->liveCfg.bytesPerSector;

    // Read first block from SCSI bus
    scsiDev.dataLen = transferlen;
    scsiDev.dataPtr = 0;
    diskDataOut_callback(0);
    
    // Start writing blocks to SD card.
    // The callback will simultaneously read the next block from SCSI bus.
    image_config_t &img = *(image_config_t*)scsiDev.target->cfg;
    uint32_t written = 0;
    while (written < transferlen)
    {
        uint8_t *buf = scsiDev.data + written;
        uint32_t buflen = scsiDev.dataPtr - written;
        azplatform_set_sd_callback(&diskDataOut_callback, buf);
        if (img.file.write(buf, buflen) != buflen)
        {
            azlog("SD card write failed: ", SD.sdErrorCode());
            scsiDev.status = CHECK_CONDITION;
            scsiDev.target->sense.code = MEDIUM_ERROR;
            scsiDev.target->sense.asc = WRITE_ERROR_AUTO_REALLOCATION_FAILED;
            scsiDev.phase = STATUS;
        }

        written += buflen;
    }

    azplatform_set_sd_callback(NULL, NULL);

    transfer.currentBlock += blockcount;
}

/*****************/
/* Read command */
/*****************/

static void doRead(uint32_t lba, uint32_t blocks)
{
    if (unlikely(scsiDev.target->cfg->deviceType == S2S_CFG_FLOPPY_14MB)) {
        // Floppies are supposed to be slow. Some systems can't handle a floppy
        // without an access time
        s2s_delay_ms(10);
    }

    uint32_t capacity = getScsiCapacity(
        scsiDev.target->cfg->sdSectorStart,
        scsiDev.target->liveCfg.bytesPerSector,
        scsiDev.target->cfg->scsiSectors);
    if (unlikely(((uint64_t) lba) + blocks > capacity))
    {
        scsiDev.status = CHECK_CONDITION;
        scsiDev.target->sense.code = ILLEGAL_REQUEST;
        scsiDev.target->sense.asc = LOGICAL_BLOCK_ADDRESS_OUT_OF_RANGE;
        scsiDev.phase = STATUS;
    }
    else
    {
        transfer.multiBlock = 1;
        transfer.lba = lba;
        transfer.blocks = blocks;
        transfer.currentBlock = 0;
        scsiDev.phase = DATA_IN;
        scsiDev.dataLen = 0;
        scsiDev.dataPtr = 0;

        image_config_t &img = *(image_config_t*)scsiDev.target->cfg;
        if (!img.file.seek(transfer.lba * img.bytesPerSector))
        {
            azlog("Seek to ", transfer.lba, " failed for ", scsiDev.target->targetId);
            scsiDev.status = CHECK_CONDITION;
            scsiDev.target->sense.code = MEDIUM_ERROR;
            scsiDev.target->sense.asc = NO_SEEK_COMPLETE;
            scsiDev.phase = STATUS;
        }
    }
}

void diskDataIn_callback(uint32_t bytes_complete)
{
    // For best performance, do writes in blocks of 4 or more bytes
    bytes_complete &= ~3;

    if (bytes_complete > scsiDev.dataPtr)
    {
        // DMA is reading from SD card, bytes_complete bytes have already been read.
        // Send them to SCSI bus now.
        
        uint32_t len = bytes_complete - scsiDev.dataPtr;
        scsiWrite(scsiDev.data + scsiDev.dataPtr, len);
        scsiDev.dataPtr += len;
    }
}

static void diskDataIn()
{
    scsiEnterPhase(DATA_IN);

    // Figure out how many blocks we can fit in buffer
    uint32_t blockcount = (transfer.blocks - transfer.currentBlock);
    uint32_t maxblocks = sizeof(scsiDev.data) / scsiDev.target->liveCfg.bytesPerSector;
    if (blockcount > maxblocks) blockcount = maxblocks;
    uint32_t transferlen = blockcount * scsiDev.target->liveCfg.bytesPerSector;

    // Start reading from SD card.
    // The callback will write to SCSI bus.
    image_config_t &img = *(image_config_t*)scsiDev.target->cfg;
    azplatform_set_sd_callback(&diskDataIn_callback, scsiDev.data);

    if (img.file.read(scsiDev.data, transferlen) != transferlen)
    {
        azlog("SD card read failed: ", SD.sdErrorCode());
        scsiDev.status = CHECK_CONDITION;
        scsiDev.target->sense.code = MEDIUM_ERROR;
        scsiDev.target->sense.asc = UNRECOVERED_READ_ERROR;
        scsiDev.phase = STATUS;
    }

    azplatform_set_sd_callback(NULL, NULL);
    transfer.currentBlock += blockcount;
}


/********************/
/* Command dispatch */
/********************/

// Handle direct-access scsi device commands
extern "C"
int scsiDiskCommand()
{
    int commandHandled = 1;

    uint8_t command = scsiDev.cdb[0];
    if (unlikely(command == 0x1B))
    {
        // START STOP UNIT
        // Enable or disable media access operations.
        //int immed = scsiDev.cdb[1] & 1;
        int start = scsiDev.cdb[4] & 1;
	    int loadEject = scsiDev.cdb[4] & 2;
	
        if (loadEject)
        {
            // Ignore load/eject requests. We can't do that.
        }
        else if (start)
        {
            scsiDev.target->started = 1;
            blockDev.state = DISK_INITIALISED;
        }
        else
        {
            scsiDev.target->started = 0;
        }
    }
    else if (unlikely(command == 0x00))
    {
        // TEST UNIT READY
        doTestUnitReady();
    }
    else if (unlikely(!doTestUnitReady()))
    {
        // Status and sense codes already set by doTestUnitReady
    }
    else if (likely(command == 0x08))
    {
        // READ(6)
        uint32_t lba =
            (((uint32_t) scsiDev.cdb[1] & 0x1F) << 16) +
            (((uint32_t) scsiDev.cdb[2]) << 8) +
            scsiDev.cdb[3];
        uint32_t blocks = scsiDev.cdb[4];
        if (unlikely(blocks == 0)) blocks = 256;
        doRead(lba, blocks);
    }
    else if (likely(command == 0x28))
    {
        // READ(10)
        // Ignore all cache control bits - we don't support a memory cache.

        uint32_t lba =
            (((uint32_t) scsiDev.cdb[2]) << 24) +
            (((uint32_t) scsiDev.cdb[3]) << 16) +
            (((uint32_t) scsiDev.cdb[4]) << 8) +
            scsiDev.cdb[5];
        uint32_t blocks =
            (((uint32_t) scsiDev.cdb[7]) << 8) +
            scsiDev.cdb[8];

        doRead(lba, blocks);
    }
    else if (likely(command == 0x0A))
    {
        // WRITE(6)
        uint32_t lba =
            (((uint32_t) scsiDev.cdb[1] & 0x1F) << 16) +
            (((uint32_t) scsiDev.cdb[2]) << 8) +
            scsiDev.cdb[3];
        uint32_t blocks = scsiDev.cdb[4];
        if (unlikely(blocks == 0)) blocks = 256;
        doWrite(lba, blocks);
    }
    else if (likely(command == 0x2A) || // WRITE(10)
        unlikely(command == 0x2E)) // WRITE AND VERIFY
    {
        // Ignore all cache control bits - we don't support a memory cache.
        // Don't bother verifying either. The SD card likely stores ECC
        // along with each flash row.

        uint32_t lba =
            (((uint32_t) scsiDev.cdb[2]) << 24) +
            (((uint32_t) scsiDev.cdb[3]) << 16) +
            (((uint32_t) scsiDev.cdb[4]) << 8) +
            scsiDev.cdb[5];
        uint32_t blocks =
            (((uint32_t) scsiDev.cdb[7]) << 8) +
            scsiDev.cdb[8];

        doWrite(lba, blocks);
    }
    else if (unlikely(command == 0x04))
    {
        // FORMAT UNIT
        // We don't really do any formatting, but we need to read the correct
        // number of bytes in the DATA_OUT phase to make the SCSI host happy.

        int fmtData = (scsiDev.cdb[1] & 0x10) ? 1 : 0;
        if (fmtData)
        {
            // We need to read the parameter list, but we don't know how
            // big it is yet. Start with the header.
            scsiDev.dataLen = 4;
            scsiDev.phase = DATA_OUT;
            scsiDev.postDataOutHook = doFormatUnitHeader;
        }
        else
        {
            // No data to read, we're already finished!
        }
    }
    else if (unlikely(command == 0x25))
    {
        // READ CAPACITY
        doReadCapacity();
    }
    else if (unlikely(command == 0x0B))
    {
        // SEEK(6)
        uint32_t lba =
            (((uint32_t) scsiDev.cdb[1] & 0x1F) << 16) +
            (((uint32_t) scsiDev.cdb[2]) << 8) +
            scsiDev.cdb[3];

        doSeek(lba);
    }

    else if (unlikely(command == 0x2B))
    {
        // SEEK(10)
        uint32_t lba =
            (((uint32_t) scsiDev.cdb[2]) << 24) +
            (((uint32_t) scsiDev.cdb[3]) << 16) +
            (((uint32_t) scsiDev.cdb[4]) << 8) +
            scsiDev.cdb[5];

        doSeek(lba);
    }
    else if (unlikely(command == 0x36))
    {
        // LOCK UNLOCK CACHE
        // We don't have a cache to lock data into. do nothing.
    }
    else if (unlikely(command == 0x34))
    {
        // PRE-FETCH.
        // We don't have a cache to pre-fetch into. do nothing.
    }
    else if (unlikely(command == 0x1E))
    {
        // PREVENT ALLOW MEDIUM REMOVAL
        // Not much we can do to prevent the user removing the SD card.
        // do nothing.
    }
    else if (unlikely(command == 0x01))
    {
        // REZERO UNIT
        // Set the lun to a vendor-specific state. Ignore.
    }
    else if (unlikely(command == 0x35))
    {
        // SYNCHRONIZE CACHE
        // We don't have a cache. do nothing.
    }
    else if (unlikely(command == 0x2F))
    {
        // VERIFY
        // TODO: When they supply data to verify, we should read the data and
        // verify it. If they don't supply any data, just say success.
        if ((scsiDev.cdb[1] & 0x02) == 0)
        {
            // They are asking us to do a medium verification with no data
            // comparison. Assume success, do nothing.
        }
        else
        {
            // TODO. This means they are supplying data to verify against.
            // Technically we should probably grab the data and compare it.
            scsiDev.status = CHECK_CONDITION;
            scsiDev.target->sense.code = ILLEGAL_REQUEST;
            scsiDev.target->sense.asc = INVALID_FIELD_IN_CDB;
            scsiDev.phase = STATUS;
        }
    }
    else if (unlikely(command == 0x37))
    {
        // READ DEFECT DATA
        uint32_t allocLength = (((uint16_t)scsiDev.cdb[7]) << 8) |
            scsiDev.cdb[8];

        scsiDev.data[0] = 0;
        scsiDev.data[1] = scsiDev.cdb[1];
        scsiDev.data[2] = 0;
        scsiDev.data[3] = 0;
        scsiDev.dataLen = 4;

        if (scsiDev.dataLen > allocLength)
        {
            scsiDev.dataLen = allocLength;
        }

        scsiDev.phase = DATA_IN;
    }
    else
    {
        commandHandled = 0;
    }

    return commandHandled;
}

extern "C"
void scsiDiskPoll()
{
    if (scsiDev.phase == DATA_IN &&
        transfer.currentBlock != transfer.blocks)
    {
        diskDataIn();
     }
    else if (scsiDev.phase == DATA_OUT &&
        transfer.currentBlock != transfer.blocks)
    {
        diskDataOut();
    }
}

extern "C"
void scsiDiskReset()
{
    scsiDev.dataPtr = 0;
    scsiDev.savedDataPtr = 0;
    scsiDev.dataLen = 0;
    // transfer.lba = 0; // Needed in Request Sense to determine failure
    transfer.blocks = 0;
    transfer.currentBlock = 0;
    transfer.multiBlock = 0;
}

extern "C"
void scsiDiskInit()
{
    scsiDiskReset();
}
