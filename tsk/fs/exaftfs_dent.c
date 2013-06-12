/*
** The Sleuth Kit
**
** Copyright (c) 2013 Basis Technology Corp.  All rights reserved
** Contact: Brian Carrier [carrier <at> sleuthkit [dot] org]
**
** This software is distributed under the Common Public License 1.0
**
*/

/*
 * This code makes use of research presented in the following paper:
 * "Reverse Engineering the exFAT File System" by Robert Shullich
 * Retrieved May 2013 from: 
 * http://www.sans.org/reading_room/whitepapers/forensics/reverse-engineering-microsoft-exfat-file-system_33274
 *
 * Some additional details concerning TexFAT were obtained in May 2013
 * from:
 * http://msdn.microsoft.com/en-us/library/ee490643(v=winembedded.60).aspx
*/

/**
 * \file exfatfs_dent.c
 * Contains the internal TSK exFAT file system code to handle name category 
 * processing. 
 */

#include "tsk_exfatfs.h" /* Include first to make sure it stands alone. */
#include "tsk_fs_i.h"
#include "tsk_fatfs.h"
#include <assert.h>

/**
 * \internal
 * \struct
 * Bundles a TSK_FS_NAME object and a TSK_FS_DIR object with additional data 
 * required when assembling a name from file directory entry set. If the
 * TSK_FS_NAME is successfully populated, it is added to the TSK_FS_DIR.
 */
typedef struct {
    FATFS_INFO *fatfs;
    int8_t sector_is_allocated;
    EXFATFS_DIR_ENTRY_TYPE_ENUM last_dentry_type;
    uint8_t expected_secondary_entry_count;
    uint8_t actual_secondary_entry_count;
    uint16_t expected_check_sum;
    uint16_t actual_check_sum;
    uint8_t expected_name_length;
    uint8_t actual_name_length;
    TSK_FS_NAME *fs_name;
    TSK_FS_DIR *fs_dir;
} EXFATFS_FS_NAME_INFO;

/**
 * \internal
 * Adds the bytes of a directory entry from a file directory entry set to the 
 * the entry set check sum stored in a EXFATFS_FS_NAME_INFO object.
 *
 * @param a_name_info The name info object.
 * @param a_dentry A buffer containing a file directory entry.
 */
static void
exfatfs_update_file_entry_set_checksum(EXFATFS_FS_NAME_INFO *a_name_info, 
    FATFS_DENTRY *a_dentry)
{
    EXFATFS_DIR_ENTRY_TYPE_ENUM dentry_type = EXFATFS_DIR_ENTRY_TYPE_NONE;
    uint8_t index = 0;
    uint16_t byte_to_add = 0; /* uint16_t is data type of check sum. */

    assert(a_name_info != NULL);
    assert(a_dentry != NULL);
    
    dentry_type = (EXFATFS_DIR_ENTRY_TYPE_ENUM)a_dentry->data[0];
    assert(dentry_type == EXFATFS_DIR_ENTRY_TYPE_FILE ||
           dentry_type == EXFATFS_DIR_ENTRY_TYPE_DELETED_FILE ||
           dentry_type == EXFATFS_DIR_ENTRY_TYPE_FILE_STREAM ||
           dentry_type == EXFATFS_DIR_ENTRY_TYPE_DELETED_FILE_STREAM ||
           dentry_type == EXFATFS_DIR_ENTRY_TYPE_FILE_NAME ||
           dentry_type == EXFATFS_DIR_ENTRY_TYPE_DELETED_FILE_NAME);

    for (index = 0; index < sizeof(a_dentry->data); ++index) {
        /* Skip the expected check sum, found in the file entry. */
        if ((dentry_type == EXFATFS_DIR_ENTRY_TYPE_FILE ||
             dentry_type == EXFATFS_DIR_ENTRY_TYPE_DELETED_FILE) &&
            (index == 2 || index == 3)) {
            continue;
        }

        // RJCTODO: Confirm this.
        /* The file system does not update the check sum when an entry set is 
         * marked as no longer in use. Compensate for this. */
        if (index == 0) {
            switch (dentry_type) {
            case EXFATFS_DIR_ENTRY_TYPE_DELETED_FILE:
                byte_to_add = (uint16_t)EXFATFS_DIR_ENTRY_TYPE_FILE;
                break;
            case EXFATFS_DIR_ENTRY_TYPE_DELETED_FILE_STREAM:
                byte_to_add = (uint16_t)EXFATFS_DIR_ENTRY_TYPE_FILE_STREAM;
                break;
            case EXFATFS_DIR_ENTRY_TYPE_DELETED_FILE_NAME:
                byte_to_add = (uint16_t)EXFATFS_DIR_ENTRY_TYPE_FILE_NAME;
                break;
            default:
                byte_to_add = (uint16_t)dentry_type;
                break;
            }
        }
        else {
            byte_to_add = (uint16_t)a_dentry->data[index];
        }
        
        a_name_info->actual_check_sum = 
            ((a_name_info->actual_check_sum << 15) | 
             (a_name_info->actual_check_sum >> 1)) + 
             byte_to_add;
    }
}

/**
 * \internal
 * Reset the fields of a EXFATFS_FS_NAME_INFO to their initialized state. This
 * allows for reuse of the object.
 *
 * @param a_name_info The name info object.
 */
static void
exfatfs_reset_name_info(EXFATFS_FS_NAME_INFO *a_name_info)
{
    assert(a_name_info != NULL);
    assert(a_name_info->fs_name != NULL);
    assert(a_name_info->fs_name->name != NULL);
    assert(a_name_info->fs_name->name_size == EXFATFS_MAX_NAME_LEN_UTF8);

    a_name_info->last_dentry_type = EXFATFS_DIR_ENTRY_TYPE_NONE;
    a_name_info->expected_secondary_entry_count = 0;
    a_name_info->actual_secondary_entry_count = 0;
    a_name_info->expected_check_sum = 0;
    a_name_info->actual_check_sum = 0;
    a_name_info->expected_name_length = 0;
    a_name_info->actual_name_length = 0;
    a_name_info->fs_name->name[0] = '\0';
    a_name_info->fs_name->meta_addr = 0;
    a_name_info->fs_name->type = TSK_FS_NAME_TYPE_UNDEF;
    a_name_info->fs_name->flags = TSK_FS_NAME_FLAG_ALLOC;
}

/**
 * \internal
 * Add the TSK_FS_NAME object of an EXFATFS_FS_NAME_INFO object to its
 * TSK_FS_DIR object and reset the fields of a EXFATFS_FS_NAME_INFO to their
 * initialized state. This allows for reuse of the object.
 *
 * @param a_name_info The name info object.
 */
static void
exfatfs_add_name_to_dir_and_reset_info(EXFATFS_FS_NAME_INFO *a_name_info)
{
    assert(a_name_info != NULL);
    assert(a_name_info->fs_name != NULL);
    assert(a_name_info->fs_name->name != NULL);
    assert(a_name_info->fs_name->name_size == EXFATFS_MAX_NAME_LEN_UTF8);
    assert(a_name_info->fs_dir != NULL);

    /* If the parsing of the directory entry or directory entry set produced
     * a name, add the TSK_FS_NAME object to the TSK_FS_DIR object. */
    if (strlen(a_name_info->fs_name->name) > 0) {
        tsk_fs_dir_add(a_name_info->fs_dir, a_name_info->fs_name);
    }

    exfatfs_reset_name_info(a_name_info);
}

/**
 * \internal
 * Populates an EXFATFS_FS_NAME_INFO object with data parsed from a file
 * directory entry. Since this is the beginning of a new name, the name
 * previously stored on the EXFATFS_FS_NAME_INFO, if any, is saved.
 *
 * @param a_name_info The name info object.
 * @param a_dentry A buffer containing a file directory entry.
 * @param a_inum The inode address associated with the directory entry.
 */
static void
exfats_parse_file_dentry(EXFATFS_FS_NAME_INFO *a_name_info, FATFS_DENTRY *a_dentry, TSK_INUM_T a_inum)
{
    EXFATFS_FILE_DIR_ENTRY *dentry = (EXFATFS_FILE_DIR_ENTRY*)a_dentry;

    assert(a_name_info != NULL);
    assert(a_name_info->fatfs != NULL);
    assert(a_name_info->fs_name != NULL);
    assert(a_name_info->fs_name->name != NULL);
    assert(a_name_info->fs_name->name_size == EXFATFS_MAX_NAME_LEN_UTF8);
    assert(a_name_info->fs_dir != NULL);
    assert(dentry != NULL);
    assert(dentry->entry_type == EXFATFS_DIR_ENTRY_TYPE_FILE ||
           dentry->entry_type == EXFATFS_DIR_ENTRY_TYPE_DELETED_FILE);
    assert(fatfs_is_inum_in_range(a_name_info->fatfs, a_inum));
    
    /* Starting parse of a new name, so save the current name, if any. */
    exfatfs_add_name_to_dir_and_reset_info(a_name_info);

    /* Set the current entry type. This is used to check the sequence and 
     * in-use state of the entries in the set. */
    a_name_info->last_dentry_type = (EXFATFS_DIR_ENTRY_TYPE_ENUM)dentry->entry_type;

    /* The number of secondary entries and the check sum for the entry set are
     * stored in the file entry. */
    a_name_info->expected_secondary_entry_count = 
        dentry->secondary_entries_count;
    a_name_info->expected_check_sum = 
        tsk_getu16(a_name_info->fatfs->fs_info.endian, dentry->check_sum);
    
    /* The file type (regular file, directory) is stored in the file entry. */
    if (dentry->attrs[0] & FATFS_ATTR_DIRECTORY) {
        a_name_info->fs_name->type = TSK_FS_NAME_TYPE_DIR;
    }
    else {
        a_name_info->fs_name->type = TSK_FS_NAME_TYPE_REG;
    }
   
    /* If the in-use bit of the type byte is not set, the entry set is for a 
     * deleted or renamed file. However, trust and verify - to be marked as 
     * allocated, the inode must also be in an allocated sector. */
    if (a_name_info->sector_is_allocated && dentry->entry_type == EXFATFS_DIR_ENTRY_TYPE_FILE) {
        a_name_info->fs_name->flags = TSK_FS_NAME_FLAG_ALLOC;    
    }
    else {
        a_name_info->fs_name->flags = TSK_FS_NAME_FLAG_UNALLOC;    
    }

    /* Make the inum of the file entry the inode address for the entry set. */
    a_name_info->fs_name->meta_addr = a_inum;

    /* Add the file entry bytes to the entry set check sum. */
    exfatfs_update_file_entry_set_checksum(a_name_info, a_dentry);
}

/**
 * \internal
 * Populates an EXFATFS_FS_NAME_INFO object with data parsed from a file
 * stream directory entry. 
 *
 * @param a_name_info The name info object.
 * @param a_dentry A buffer containing a file stream directory entry.
 * @param a_inum The inode address associated with the directory entry.
 */
static void
exfats_parse_file_stream_dentry(EXFATFS_FS_NAME_INFO *a_name_info, FATFS_DENTRY *a_dentry, TSK_INUM_T a_inum)
{
    EXFATFS_FILE_STREAM_DIR_ENTRY *dentry = (EXFATFS_FILE_STREAM_DIR_ENTRY*)a_dentry;

    assert(a_name_info != NULL);
    assert(a_name_info->fatfs != NULL);
    assert(a_name_info->fs_name != NULL);
    assert(a_name_info->fs_name->name != NULL);
    assert(a_name_info->fs_name->name_size == EXFATFS_MAX_NAME_LEN_UTF8);
    assert(a_name_info->fs_dir != NULL);
    assert(dentry != NULL);
    assert(dentry->entry_type == EXFATFS_DIR_ENTRY_TYPE_FILE_STREAM ||
           dentry->entry_type == EXFATFS_DIR_ENTRY_TYPE_DELETED_FILE_STREAM);
    assert(fatfs_is_inum_in_range(a_name_info->fatfs, a_inum));

    if ((a_name_info->last_dentry_type != EXFATFS_DIR_ENTRY_TYPE_FILE) && 
        (a_name_info->last_dentry_type != EXFATFS_DIR_ENTRY_TYPE_DELETED_FILE)) {
        /* A file stream entry must follow a file entry, so this entry is a
         * false positive or there is corruption. Save the current name, 
         * if any, and ignore this buffer. */ 
        exfatfs_add_name_to_dir_and_reset_info(a_name_info);
        return;
    }

    if ((a_name_info->last_dentry_type == EXFATFS_DIR_ENTRY_TYPE_FILE &&
         dentry->entry_type == EXFATFS_DIR_ENTRY_TYPE_DELETED_FILE_STREAM) || 
        (a_name_info->last_dentry_type == EXFATFS_DIR_ENTRY_TYPE_DELETED_FILE &&
         dentry->entry_type == EXFATFS_DIR_ENTRY_TYPE_FILE_STREAM)) {
        /* The in-use bits of all of the entries in an entry set should be 
         * same, so this entry is a false positive or there is corruption. 
         * Save the current name, if any, and ignore this buffer. */ 
        exfatfs_add_name_to_dir_and_reset_info(a_name_info);
        return;
    }

    /* Set the current entry type. This is used to check the sequence and 
     * in-use state of the entries in the set. */
    a_name_info->last_dentry_type = 
        (EXFATFS_DIR_ENTRY_TYPE_ENUM)dentry->entry_type;

    /* The file stream entry contains the length of the file name. */
    a_name_info->expected_name_length = dentry->file_name_length;

    /* Add the stream entry bytes to the entry set check sum. */
    exfatfs_update_file_entry_set_checksum(a_name_info, a_dentry);

    /* If all of the secondary entries for the set are present, save the name,
     * if any. Note that if this condition is satisfied here, the directory is
     * corrupted or this is a degenerate case - there should be at least one 
     * file name entry in a directory entry set. */
    // RJCTODO: Verify the check sum?
    ++a_name_info->actual_secondary_entry_count;
    if (a_name_info->actual_secondary_entry_count == 
        a_name_info->expected_secondary_entry_count) {
        exfatfs_add_name_to_dir_and_reset_info(a_name_info);
    }
}

/**
 * \internal
 * Populates an EXFATFS_FS_NAME_INFO object with data parsed from a file
 * name directory entry. 
 *
 * @param a_name_info The name info object.
 * @param a_dentry A buffer containing a file name directory entry.
 * @param a_inum The inode address associated with the directory entry.
 */
static void
exfats_parse_file_name_dentry(EXFATFS_FS_NAME_INFO *a_name_info, FATFS_DENTRY *a_dentry, TSK_INUM_T a_inum)
{
    EXFATFS_FILE_NAME_DIR_ENTRY *dentry = (EXFATFS_FILE_NAME_DIR_ENTRY*)a_dentry;
    size_t num_chars_to_copy = 0;

    assert(a_name_info != NULL);
    assert(a_name_info->fatfs != NULL);
    assert(a_name_info->fs_name != NULL);
    assert(a_name_info->fs_name->name != NULL);
    assert(a_name_info->fs_name->name_size == EXFATFS_MAX_NAME_LEN_UTF8);
    assert(a_name_info->fs_dir != NULL);
    assert(dentry != NULL);
    assert(dentry->entry_type == EXFATFS_DIR_ENTRY_TYPE_FILE_NAME ||
           dentry->entry_type == EXFATFS_DIR_ENTRY_TYPE_DELETED_FILE_NAME);
    assert(fatfs_is_inum_in_range(a_name_info->fatfs, a_inum));

    if (a_name_info->last_dentry_type != EXFATFS_DIR_ENTRY_TYPE_FILE_STREAM && 
        a_name_info->last_dentry_type != EXFATFS_DIR_ENTRY_TYPE_DELETED_FILE_STREAM &&
        a_name_info->last_dentry_type != EXFATFS_DIR_ENTRY_TYPE_FILE_NAME &&
        a_name_info->last_dentry_type != EXFATFS_DIR_ENTRY_TYPE_DELETED_FILE_NAME) {
        /* A file name entry must follow a stream or name entry, so this entry is
         * is a false positive or there is corruption. Save the current name, 
         * if any, and ignore this buffer. */ 
        exfatfs_add_name_to_dir_and_reset_info(a_name_info);
        return;
    }

    if (((a_name_info->last_dentry_type == EXFATFS_DIR_ENTRY_TYPE_FILE_STREAM || 
          a_name_info->last_dentry_type == EXFATFS_DIR_ENTRY_TYPE_FILE_NAME) && 
         dentry->entry_type == EXFATFS_DIR_ENTRY_TYPE_DELETED_FILE_NAME) ||
        ((a_name_info->last_dentry_type == EXFATFS_DIR_ENTRY_TYPE_DELETED_FILE_STREAM || 
         a_name_info->last_dentry_type == EXFATFS_DIR_ENTRY_TYPE_DELETED_FILE_NAME) &&
         dentry->entry_type == EXFATFS_DIR_ENTRY_TYPE_FILE_NAME)) {
        /* The in-use bits of all of the entries in an entry set should be 
         * same, so this entry is a false positive or there is corruption. 
         * Save the current name, if any, and ignore this buffer. */ 
        exfatfs_add_name_to_dir_and_reset_info(a_name_info);
        return;
    }

    /* Set the current entry type. This is used to check the sequence and 
     * in-use state of the entries in the set. */
    a_name_info->last_dentry_type = 
        (EXFATFS_DIR_ENTRY_TYPE_ENUM)dentry->entry_type;

    /* Determine how many name chars remain according to the name length from
     * the file stream entry and how many chars can be obtained from this
     * name entry. */
    num_chars_to_copy = a_name_info->expected_name_length - a_name_info->actual_name_length;
    if (num_chars_to_copy > EXFATFS_MAX_FILE_NAME_SEGMENT_LENGTH) {
        num_chars_to_copy = EXFATFS_MAX_FILE_NAME_SEGMENT_LENGTH;
    }

    /* If there is enough space remaining in the name object, convert the
     * name chars to UTF-8 and save them. */
    if ((size_t)(a_name_info->actual_name_length + num_chars_to_copy) < 
        a_name_info->fs_name->name_size - 1) {
        if (fatfs_utf16_inode_str_2_utf8(a_name_info->fatfs, 
            (UTF16*)dentry->utf16_name_chars, num_chars_to_copy,
            (UTF8*)a_name_info->fs_name->name, a_name_info->fs_name->name_size,
            a_inum, "file name segment") != TSKconversionOK) {
            /* Discard whatever was written by the failed conversion and save
             * whatever has been found to this point, if anything. */
            a_name_info->fs_name->name[a_name_info->actual_name_length] = '\0';
            exfatfs_add_name_to_dir_and_reset_info(a_name_info);
            return;
        }

        /* Update the actual name length and null-terminate the name so far. */
        a_name_info->actual_name_length += num_chars_to_copy;
        a_name_info->fs_name->name[a_name_info->actual_name_length] = '\0';
    }

    /* If all of the secondary entries for the set are present, save the name,
     * if any. */
    // RJCTODO: Verify the check sum?
    ++a_name_info->actual_secondary_entry_count;
    if (a_name_info->actual_secondary_entry_count == 
        a_name_info->expected_secondary_entry_count) {
        exfatfs_add_name_to_dir_and_reset_info(a_name_info);
    }
}

/**
 * \internal
 * Populates an EXFATFS_FS_NAME_INFO object with data parsed from a volume
 * label directory entry. 
 *
 * @param a_name_info The name info object.
 * @param a_dentry A buffer containing a volume label directory entry.
 * @param a_inum The inode address associated with the directory entry.
 */
static void
exfats_parse_vol_label_dentry(EXFATFS_FS_NAME_INFO *a_name_info, FATFS_DENTRY *a_dentry, TSK_INUM_T a_inum)
{
    EXFATFS_VOL_LABEL_DIR_ENTRY *dentry = (EXFATFS_VOL_LABEL_DIR_ENTRY*)a_dentry;
    const char *tag = " (Volume Label Entry)";
    size_t tag_length = 0;

    assert(a_name_info != NULL);
    assert(a_name_info->fatfs != NULL);
    assert(a_name_info->fs_name != NULL);
    assert(a_name_info->fs_name->name != NULL);
    assert(a_name_info->fs_name->name_size == EXFATFS_MAX_NAME_LEN_UTF8);
    assert(a_name_info->fs_dir != NULL);
    assert(dentry != NULL);
    assert(dentry->entry_type == EXFATFS_DIR_ENTRY_TYPE_VOLUME_LABEL ||
           dentry->entry_type == EXFATFS_DIR_ENTRY_TYPE_VOLUME_LABEL_EMPTY);
    assert(fatfs_is_inum_in_range(a_name_info->fatfs, a_inum));

    /* Starting parse of a new name, save the previous name, if any. */
    exfatfs_add_name_to_dir_and_reset_info(a_name_info);

    /* Set the current entry type. This is used to check the sequence and 
     * in-use state of the entries in the set. */
    a_name_info->last_dentry_type = 
        (EXFATFS_DIR_ENTRY_TYPE_ENUM)dentry->entry_type;

    if (dentry->entry_type != EXFATFS_DIR_ENTRY_TYPE_VOLUME_LABEL_EMPTY) {
        if (fatfs_utf16_inode_str_2_utf8(a_name_info->fatfs, 
            (UTF16*)dentry->volume_label, (size_t)dentry->utf16_char_count + 1, 
            (UTF8*)a_name_info->fs_name->name, a_name_info->fs_name->name_size,
            a_inum, "volume label") != TSKconversionOK) {
            /* Discard whatever was written by the failed conversion. */
            exfatfs_reset_name_info(a_name_info);
            return;
        }

        a_name_info->actual_name_length += dentry->utf16_char_count;
        a_name_info->fs_name->name[a_name_info->actual_name_length] = '\0';

        tag_length = strlen(tag);
        if ((size_t)a_name_info->actual_name_length + tag_length < 
            EXFATFS_MAX_NAME_LEN_UTF8) {
            strcat(a_name_info->fs_name->name, tag);
        }

        /* Record the inum associated with this name. */
        a_name_info->fs_name->meta_addr =  a_inum;

        /* Save the volume label. */
        exfatfs_add_name_to_dir_and_reset_info(a_name_info);
    }
}

/**
 * \internal
 * Populates an EXFATFS_FS_NAME_INFO object with data parsed from a 
 * special file directory entry. 
 *
 * @param a_name_info The name info object.
 * @param a_dentry A buffer containing a special file directory entry.
 * @param a_inum The inode address associated with the directory entry.
 */
static void
exfats_parse_special_file_dentry(EXFATFS_FS_NAME_INFO *a_name_info, FATFS_DENTRY *a_dentry, TSK_INUM_T a_inum)
{
    assert(a_name_info != NULL);
    assert(a_name_info->fatfs != NULL);
    assert(a_name_info->fs_name != NULL);
    assert(a_name_info->fs_name->name != NULL);
    assert(a_name_info->fs_name->name_size == EXFATFS_MAX_NAME_LEN_UTF8);
    assert(a_name_info->fs_dir != NULL);
    assert(a_dentry != NULL);
    assert(a_dentry->data[0] == EXFATFS_DIR_ENTRY_TYPE_VOLUME_GUID ||
           a_dentry->data[0] == EXFATFS_DIR_ENTRY_TYPE_ALLOC_BITMAP ||
           a_dentry->data[0] == EXFATFS_DIR_ENTRY_TYPE_UPCASE_TABLE ||
           a_dentry->data[0] == EXFATFS_DIR_ENTRY_TYPE_ACT);
    assert(fatfs_is_inum_in_range(a_name_info->fatfs, a_inum));

    /* Starting parse of a new name, save the previous name, if any. */
    exfatfs_add_name_to_dir_and_reset_info(a_name_info);

    /* Record the inum associated with this name. */
    a_name_info->fs_name->meta_addr = a_inum;

    /* Set the current entry type. This is used to check the sequence and 
     * in-use state of the entries in the set. */
    a_name_info->last_dentry_type = 
        (EXFATFS_DIR_ENTRY_TYPE_ENUM)a_dentry->data[0];

    switch (a_dentry->data[0]) {
        case EXFATFS_DIR_ENTRY_TYPE_VOLUME_GUID:
            strcpy(a_name_info->fs_name->name, EXFATFS_VOLUME_GUID_VIRT_FILENAME);
            break;
        case EXFATFS_DIR_ENTRY_TYPE_ALLOC_BITMAP:
            strcpy(a_name_info->fs_name->name, EXFATFS_ALLOC_BITMAP_VIRT_FILENAME);
            break;
        case EXFATFS_DIR_ENTRY_TYPE_UPCASE_TABLE:
            strcpy(a_name_info->fs_name->name, EXFATFS_UPCASE_TABLE_VIRT_FILENAME);
            break;
        case EXFATFS_DIR_ENTRY_TYPE_TEX_FAT:
            strcpy(a_name_info->fs_name->name, EXFATFS_TEX_FAT_VIRT_FILENAME);
            break;
        case EXFATFS_DIR_ENTRY_TYPE_ACT:
            strcpy(a_name_info->fs_name->name, EXFATFS_ACT_VIRT_FILENAME);
            break;
    }

    /* Save the virtual file name. */
    exfatfs_add_name_to_dir_and_reset_info(a_name_info);
}

/**
 * /internal
 * Parse a buffer containing the contents of a directory and add TSK_FS_NAME 
 * objects for each named file found to the TSK_FS_DIR representation of the 
 * directory.
 *
 * @param a_fatfs File system information structure for file system that
 * contains the directory.
 * @param a_fs_dir Directory structure into to which parsed file metadata will
 * be added.
 * @param a_buf Buffer that contains the directory contents.
 * @param a_buf_len Length of buffer in bytes (must be a multiple of sector
*  size).
 * @param a_sector_addrs Array where each element is the original address of
 * the corresponding sector in a_buf (size of array is number of sectors in
 * the directory).
 * @return TSK_RETVAL_ENUM
*/
TSK_RETVAL_ENUM
exfatfs_dent_parse_buf(FATFS_INFO *a_fatfs, TSK_FS_DIR *a_fs_dir, char *a_buf,
    TSK_OFF_T a_buf_len, TSK_DADDR_T *a_sector_addrs)
{
    const char *func_name = "exfatfs_parse_directory_buf";
    TSK_FS_INFO *fs = NULL;
    TSK_OFF_T num_sectors = 0;
    TSK_OFF_T sector_index = 0;
    TSK_INUM_T base_inum_of_sector = 0;
    EXFATFS_FS_NAME_INFO name_info;
    TSK_OFF_T dentry_index = 0;
    FATFS_DENTRY *dentry = NULL;
    int entries_count = 0;
    int invalid_entries_count = 0;
    uint8_t is_corrupt_dir = 0;

    tsk_error_reset();
    if (fatfs_is_ptr_arg_null(a_fatfs, "a_fatfs", func_name) ||
        fatfs_is_ptr_arg_null(a_fs_dir, "a_fs_dir", func_name) ||
        fatfs_is_ptr_arg_null(a_buf, "a_buf", func_name) ||
        fatfs_is_ptr_arg_null(a_sector_addrs, "a_sector_addrs", func_name)) {
        return TSK_ERR; 
    }

    assert(a_buf_len > 0);
    if (a_buf_len < 0) {
        tsk_error_reset();
        tsk_error_set_errno(TSK_ERR_FS_ARG);
        tsk_error_set_errstr("%s: invalid buffer length", func_name);
        return TSK_ERR; 
    }

    fs = (TSK_FS_INFO*)a_fatfs;

    memset((void*)&name_info, 0, sizeof(EXFATFS_FS_NAME_INFO));
    name_info.fatfs = a_fatfs;
    if ((name_info.fs_name = tsk_fs_name_alloc(EXFATFS_MAX_NAME_LEN_UTF8, 0)) == NULL) {
        return TSK_ERR;
    }
    name_info.fs_name->name[0] = '\0';
    name_info.fs_dir = a_fs_dir;

    // RJCTODO: Does this need to be set here? If not, when is it set?
    // name_info->fs_name->par_addr = a_inum; 

    /* Loop through the sectors in the buffer. */ 
    dentry = (FATFS_DENTRY*)a_buf;
    num_sectors = a_buf_len / a_fatfs->ssize;
    for (sector_index = 0; sector_index < num_sectors; ++sector_index) {
        /* Convert the address of the current sector into an inode address. */
        base_inum_of_sector = 
            FATFS_SECT_2_INODE(a_fatfs, a_sector_addrs[sector_index]);
        if (base_inum_of_sector > fs->last_inum) {
            tsk_error_reset();
            tsk_error_set_errno(TSK_ERR_FS_ARG);
            tsk_error_set_errstr("%s: inode address for sector address %" 
                PRIuDADDR " at addresses array index %" PRIuDADDR 
                " is too large", func_name, base_inum_of_sector, sector_index);
            tsk_fs_name_free(name_info.fs_name);
            return TSK_COR;
        }

        if (tsk_verbose) {
            tsk_fprintf(stderr,"%s: Parsing sector %" PRIuDADDR " for dir %" 
                PRIuINUM "\n", func_name, a_sector_addrs[sector_index], a_fs_dir->addr);
        }

        /* Get the allocation status of the current sector. */
        if ((name_info.sector_is_allocated = 
            fatfs_is_sectalloc(a_fatfs, a_sector_addrs[sector_index])) == -1) {
            if (tsk_verbose) {
                tsk_fprintf(stderr, 
                    "%s: Error looking up allocation status of sector : %"
                    PRIuDADDR "\n", func_name, a_sector_addrs[sector_index]);
                tsk_error_print(stderr);
            }
            tsk_error_reset();
            continue;
        }

        /* Loop through the putative directory entries in the current sector. */
        for (dentry_index = 0; dentry_index < a_fatfs->dentry_cnt_se; ++dentry_index, ++dentry) {
            FATFS_DENTRY *current_dentry = dentry;
            TSK_INUM_T current_inum = base_inum_of_sector + dentry_index;
            EXFATFS_DIR_ENTRY_TYPE_ENUM dentry_type = EXFATFS_DIR_ENTRY_TYPE_NONE;

            ++entries_count; // RJCTODO: Should this be reset for each iteration of this loop?

            if (!fatfs_is_inum_in_range(a_fatfs, current_inum)) {
                tsk_fs_name_free(name_info.fs_name);
                return TSK_ERR; // RJCTODO: Is this right? More error reporting?
            }

            dentry_type = exfatfs_is_dentry(a_fatfs, current_dentry, 
                (!is_corrupt_dir && name_info.sector_is_allocated)); 

            switch (dentry_type) {
            case EXFATFS_DIR_ENTRY_TYPE_FILE:
            case EXFATFS_DIR_ENTRY_TYPE_DELETED_FILE:
                exfats_parse_file_dentry(&name_info, current_dentry, current_inum);                 
                break;
            case EXFATFS_DIR_ENTRY_TYPE_FILE_STREAM:
            case EXFATFS_DIR_ENTRY_TYPE_DELETED_FILE_STREAM:
                exfats_parse_file_stream_dentry(&name_info, current_dentry, current_inum);                 
                break;
            case EXFATFS_DIR_ENTRY_TYPE_FILE_NAME:
            case EXFATFS_DIR_ENTRY_TYPE_DELETED_FILE_NAME:
                exfats_parse_file_name_dentry(&name_info, current_dentry, current_inum);                 
                break;
            case EXFATFS_DIR_ENTRY_TYPE_VOLUME_LABEL_EMPTY:
            case EXFATFS_DIR_ENTRY_TYPE_VOLUME_LABEL:
                exfats_parse_vol_label_dentry(&name_info, current_dentry, current_inum);
                break;
            case EXFATFS_DIR_ENTRY_TYPE_VOLUME_GUID:
            case EXFATFS_DIR_ENTRY_TYPE_ALLOC_BITMAP:
            case EXFATFS_DIR_ENTRY_TYPE_UPCASE_TABLE:
            case EXFATFS_DIR_ENTRY_TYPE_TEX_FAT:
            case EXFATFS_DIR_ENTRY_TYPE_ACT:
                exfats_parse_special_file_dentry(&name_info, current_dentry, current_inum);                 
                break;
            case EXFATFS_DIR_ENTRY_TYPE_NONE:
            default:
                ++invalid_entries_count;
                if (entries_count == 4 && invalid_entries_count == 4) {
                    /* If the first four putative entries in the buffer are not
                     * entries, set the corrupt directory flag to make entry tests
                     * more in-depth, even for allocated sectors. */
                    is_corrupt_dir = 1;
                }

                /* Starting parse of a new name, save the previous name, 
                 * if any. */
                exfatfs_add_name_to_dir_and_reset_info(&name_info);

                break;
            }
        }
    }

     /* Save the last parsed name, if any. */
    exfatfs_add_name_to_dir_and_reset_info(&name_info);
    tsk_fs_name_free(name_info.fs_name);

    return TSK_OK;
}