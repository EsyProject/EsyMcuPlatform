/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ESY_FS_FATFS_FFCONF_H
#define ESY_FS_FATFS_FFCONF_H

/* FatFs R0.16 configuration: all API consumers share this native ABI. */
#define FFCONF_DEF 80386
#define FF_FS_READONLY 0
#define FF_FS_MINIMIZE 0
#define FF_USE_FIND 0
#define FF_USE_MKFS 1
#define FF_USE_FASTSEEK 0
#define FF_USE_EXPAND 0
#define FF_USE_CHMOD 0
#define FF_USE_LABEL 0
#define FF_USE_FORWARD 0
#define FF_USE_STRFUNC 0
#define FF_PRINT_LLI 0
#define FF_PRINT_FLOAT 0
#define FF_STRF_ENCODE 0
#define FF_CODE_PAGE 437
/* LFN working storage uses the calling task stack, never heap or shared BSS. */
#define FF_USE_LFN 2
#define FF_MAX_LFN 255
#define FF_LFN_UNICODE 2
/* UTF-8 can require three bytes for each UTF-16 LFN unit. */
#define FF_LFN_BUF 765
#define FF_SFN_BUF 34
#define FF_FS_RPATH 0
#define FF_PATH_DEPTH 10
#define FF_VOLUMES 4
#define FF_STR_VOLUME_ID 0
#define FF_MULTI_PARTITION 0
#define FF_MIN_SS 512
#define FF_MAX_SS 512
#define FF_LBA64 0
#define FF_MIN_GPT 0x10000000
#define FF_USE_TRIM 0
#define FF_FS_TINY 0
#define FF_FS_EXFAT 0
/* No implicit RTC dependency; the facade does not expose timestamps. */
#define FF_FS_NORTC 1
#define FF_NORTC_MON 1
#define FF_NORTC_MDAY 1
#define FF_NORTC_YEAR 2026
#define FF_FS_CRTIME 0
#define FF_FS_NOFSINFO 1
/* Shared provider lock table checks conflicting opens/removes/renames. */
#define FF_FS_LOCK 16
/* The owner serializes every volume and management call, including callbacks. */
#define FF_FS_REENTRANT 0
#define FF_FS_TIMEOUT 0

#endif /* ESY_FS_FATFS_FFCONF_H */
