#define _FILE_OFFSET_BITS 64
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>

#define BS 4096u
#define INODE_SIZE 128u
#define ROOT_INO 1u
#define DIRECT_MAX 12
#pragma pack(push, 1)

typedef struct {
    uint32_t magic;             // 0x4D565346
    uint32_t version;           // 1
    uint32_t block_size;        // 4096
    uint64_t total_blocks;      // 8
    uint64_t inode_count;       // 8
    uint64_t inode_bitmap_start;// 8
    uint64_t inode_bitmap_blocks;// 8
    uint64_t data_bitmap_start; // 8
    uint64_t data_bitmap_blocks;// 8
    uint64_t inode_table_start; // 8
    uint64_t inode_table_blocks;// 8
    uint64_t data_region_start; // 8
    uint64_t data_region_blocks;// 8
    uint64_t root_inode;        // 8
    uint64_t mtime_epoch;       // 8
    uint32_t flags;             // 4
    uint32_t checksum;          // 4
} superblock_t;
#pragma pack(pop)
_Static_assert(sizeof(superblock_t) == 116, "superblock must fit in one block");

#pragma pack(push,1)
typedef struct {
    uint16_t mode;              // 2
    uint16_t links;             // 2
    uint32_t uid;               // 4
    uint32_t gid;               // 4
    uint64_t size_bytes;        // 8
    uint64_t atime;             // 8
    uint64_t mtime;             // 8
    uint64_t ctime;             // 8
    uint32_t direct[DIRECT_MAX];// 48
    uint32_t reserved_0;        // 4
    uint32_t reserved_1;        // 4
    uint32_t reserved_2;        // 4
    uint32_t proj_id;           // 4
    uint32_t uid16_gid16;       // 4
    uint64_t xattr_ptr;         // 8
    uint64_t inode_crc;         // 8
} inode_t;
#pragma pack(pop)
_Static_assert(sizeof(inode_t)==INODE_SIZE, "inode size mismatch");

#pragma pack(push,1)
typedef struct {
    uint32_t inode_no;          // 4
    uint8_t type;               // 1
    char name[58];              // 58
    uint8_t checksum;           // 1
} dirent64_t;
#pragma pack(pop)
_Static_assert(sizeof(dirent64_t)==64, "dirent size mismatch");


// ==========================DO NOT CHANGE THIS PORTION=========================
uint32_t CRC32_TAB[256];
void crc32_init(void){
    for (uint32_t i=0;i<256;i++){
        uint32_t c=i;
        for(int j=0;j<8;j++) c = (c&1)?(0xEDB88320u^(c>>1)):(c>>1);
        CRC32_TAB[i]=c;
    }
}
uint32_t crc32(const void* data, size_t n){
    const uint8_t* p=(const uint8_t*)data; uint32_t c=0xFFFFFFFFu;
    for(size_t i=0;i<n;i++) c = CRC32_TAB[(c^p[i])&0xFF] ^ (c>>8);
    return c ^ 0xFFFFFFFFu;
}

static uint32_t superblock_crc_finalize(superblock_t *sb) {
    sb->checksum = 0;
    uint32_t s = crc32((void *) sb, BS - 4);
    sb->checksum = s;
    return s;
}

void inode_crc_finalize(inode_t* ino){
    uint8_t tmp[INODE_SIZE];
    memcpy(tmp, ino, INODE_SIZE);
    memset(&tmp[120], 0, 8);
    uint32_t c = crc32(tmp, 120);
    ino->inode_crc = (uint64_t)c;
}

void dirent_checksum_finalize(dirent64_t* de) {
    const uint8_t* p = (const uint8_t*)de;
    uint8_t x = 0;
    for (int i = 0; i < 63; i++) x ^= p[i];
    de->checksum = x;
}
// ============================================================================

void usage() {
    printf("Usage: mkfs_adder --input <input.img> --output <output.img> --file <filename>\n");
    exit(1);
}

int find_free_inode(uint8_t *inode_bitmap, uint64_t inode_count) {
    for (uint64_t i = 0; i < (inode_count + 7) / 8; i++) {
        if (inode_bitmap[i] != 0xFF) {
            for (int j = 0; j < 8; j++) {
                if (!(inode_bitmap[i] & (1 << j))) {
                    return (i * 8) + j + 1;
                }
            }
        }
    }
    return -1;
}

int find_free_data_block(uint8_t *data_bitmap, uint64_t data_blocks) {
    for (uint64_t i = 0; i < (data_blocks + 7) / 8; i++) {
        if (data_bitmap[i] != 0xFF) {
            for (int j = 0; j < 8; j++) {
                if (!(data_bitmap[i] & (1 << j))) {
                    return i * 8 + j;
                }
            }
        }
    }
    return -1;
}

int main(int argc, char *argv[]) {
    crc32_init();
   
    char *input_filename = NULL;
    char *output_filename = NULL;
    char *file_to_add = NULL;
   
    // Parse command line arguments
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--input") == 0) {
            if (i + 1 < argc) {
                input_filename = argv[++i];
            } else {
                usage();
            }
        } else if (strcmp(argv[i], "--output") == 0) {
            if (i + 1 < argc) {
                output_filename = argv[++i];
            } else {
                usage();
            }
        } else if (strcmp(argv[i], "--file") == 0) {
            if (i + 1 < argc) {
                file_to_add = argv[++i];
            } else {
                usage();
            }
        }
    }
   
    if (!input_filename || !output_filename || !file_to_add) {
        usage();
    }
   
    // Open input file
    FILE *input_fp = fopen(input_filename, "rb+");
    if (!input_fp) {
        perror("Failed to open input image");
        exit(1);
    }
   
    // Read superblock
    superblock_t superblock;
    if (fread(&superblock, sizeof(superblock_t), 1, input_fp) != 1) {
        perror("Failed to read superblock");
        fclose(input_fp);
        exit(1);
    }
   
    if (superblock.magic != 0x4D565346) {
        fprintf(stderr, "Invalid file system magic number\n");
        fclose(input_fp);
        exit(1);
    }
   
    // Read inode bitmap
    fseek(input_fp, superblock.inode_bitmap_start * BS, SEEK_SET);
    uint8_t *inode_bitmap = malloc(BS);
    fread(inode_bitmap, BS, 1, input_fp);
   
    // Read data bitmap
    fseek(input_fp, superblock.data_bitmap_start * BS, SEEK_SET);
    uint8_t *data_bitmap = malloc(BS);
    fread(data_bitmap, BS, 1, input_fp);
   
    // Find free inode
    int free_inode = find_free_inode(inode_bitmap, superblock.inode_count);
    if (free_inode == -1) {
        fprintf(stderr, "No free inodes available\n");
        free(inode_bitmap);
        free(data_bitmap);
        fclose(input_fp);
        exit(1);
    }
   
    // Open file to add
    FILE *file_fp = fopen(file_to_add, "rb");
    if (!file_fp) {
        perror("Failed to open file to add");
        free(inode_bitmap);
        free(data_bitmap);
        fclose(input_fp);
        exit(1);
    }
   
    // Get file size
    fseek(file_fp, 0, SEEK_END);
    uint64_t file_size = ftell(file_fp);
    fseek(file_fp, 0, SEEK_SET);
   
    // Check if file fits in direct blocks
    uint64_t blocks_needed = (file_size + BS - 1) / BS;
    if (blocks_needed > DIRECT_MAX) {
        fprintf(stderr, "File too large (requires %lu blocks, max is %d)\n", blocks_needed, DIRECT_MAX);
        fclose(file_fp);
        free(inode_bitmap);
        free(data_bitmap);
        fclose(input_fp);
        exit(1);
    }
   
    // Find free data blocks
    uint32_t data_blocks[DIRECT_MAX] = {0};
    for (uint64_t i = 0; i < blocks_needed; i++) {
        int free_block = find_free_data_block(data_bitmap, superblock.data_region_blocks);
        if (free_block == -1) {
            fprintf(stderr, "No free data blocks available\n");
            fclose(file_fp);
            free(inode_bitmap);
            free(data_bitmap);
            fclose(input_fp);
            exit(1);
        }
        data_blocks[i] = free_block;
        data_bitmap[free_block / 8] |= (1 << (free_block % 8));
    }
   
    // Read root inode
    fseek(input_fp, superblock.inode_table_start * BS + (ROOT_INO - 1) * INODE_SIZE, SEEK_SET);
    inode_t root_inode;
    fread(&root_inode, sizeof(inode_t), 1, input_fp);
   
    // Read root directory data
    fseek(input_fp, superblock.data_region_start * BS + root_inode.direct[0] * BS, SEEK_SET);
    dirent64_t root_entries[BS / sizeof(dirent64_t)];
    fread(root_entries, BS, 1, input_fp);
   
    // Find free directory entry
    int free_entry = -1;
    for (int i = 0; i < BS / sizeof(dirent64_t); i++) {
        if (root_entries[i].inode_no == 0) {
            free_entry = i;
            break;
        }
    }
   
    if (free_entry == -1) {
        fprintf(stderr, "No free directory entries in root\n");
        fclose(file_fp);
        free(inode_bitmap);
        free(data_bitmap);
        fclose(input_fp);
        exit(1);
    }
   
    // Create new inode
    inode_t new_inode = {0};
    new_inode.mode = 0x8000; // Regular file
    new_inode.links = 1;
    new_inode.size_bytes = file_size;
    time_t now = time(NULL);
    new_inode.atime = now;
    new_inode.mtime = now;
    new_inode.ctime = now;
    new_inode.proj_id = 1234;
   
    for (uint64_t i = 0; i < blocks_needed; i++) {
        new_inode.direct[i] = data_blocks[i];
    }
   
    inode_crc_finalize(&new_inode);
   
    // Write new inode to inode table
    fseek(input_fp, superblock.inode_table_start * BS + (free_inode - 1) * INODE_SIZE, SEEK_SET);
    fwrite(&new_inode, sizeof(inode_t), 1, input_fp);
   
    // Update inode bitmap
    inode_bitmap[(free_inode - 1) / 8] |= (1 << ((free_inode - 1) % 8));
    fseek(input_fp, superblock.inode_bitmap_start * BS, SEEK_SET);
    fwrite(inode_bitmap, BS, 1, input_fp);
   
    // Update data bitmap
    fseek(input_fp, superblock.data_bitmap_start * BS, SEEK_SET);
    fwrite(data_bitmap, BS, 1, input_fp);
   
    // Write file data to data blocks
    uint8_t buffer[BS];
    for (uint64_t i = 0; i < blocks_needed; i++) {
        size_t bytes_read = fread(buffer, 1, BS, file_fp);
        fseek(input_fp, superblock.data_region_start * BS + data_blocks[i] * BS, SEEK_SET);
        fwrite(buffer, bytes_read, 1, input_fp);
        if (bytes_read < BS) {
            // Pad remaining bytes with zeros
            memset(buffer + bytes_read, 0, BS - bytes_read);
            fwrite(buffer + bytes_read, BS - bytes_read, 1, input_fp);
        }
    }
   
    fclose(file_fp);
   
    // Add directory entry to root
    root_entries[free_entry].inode_no = free_inode;
    root_entries[free_entry].type = 1; // File
    strncpy(root_entries[free_entry].name, file_to_add, sizeof(root_entries[free_entry].name) - 1);
    root_entries[free_entry].name[sizeof(root_entries[free_entry].name) - 1] = '\0';
    dirent_checksum_finalize(&root_entries[free_entry]);
   
    fseek(input_fp, superblock.data_region_start * BS + root_inode.direct[0] * BS, SEEK_SET);
    fwrite(root_entries, BS, 1, input_fp);
   
    // Update root inode links count and mtime
    root_inode.links++;
    root_inode.mtime = now;
    inode_crc_finalize(&root_inode);
    fseek(input_fp, superblock.inode_table_start * BS + (ROOT_INO - 1) * INODE_SIZE, SEEK_SET);
    fwrite(&root_inode, sizeof(inode_t), 1, input_fp);
   
    // Update superblock mtime
    superblock.mtime_epoch = now;
    superblock_crc_finalize(&superblock);
    fseek(input_fp, 0, SEEK_SET);
    fwrite(&superblock, sizeof(superblock_t), 1, input_fp);
   
    free(inode_bitmap);
    free(data_bitmap);
   
    // Copy to output file if different from input
    if (strcmp(input_filename, output_filename) != 0) {
        fseek(input_fp, 0, SEEK_SET);
        FILE *output_fp = fopen(output_filename, "wb");
        if (!output_fp) {
            perror("Failed to create output file");
            fclose(input_fp);
            exit(1);
        }
       
        uint8_t copy_buffer[BS];
        size_t bytes_read;
        while ((bytes_read = fread(copy_buffer, 1, BS, input_fp)) > 0) {
            fwrite(copy_buffer, 1, bytes_read, output_fp);
        }
       
        fclose(output_fp);
        fclose(input_fp);
    } else {
        fclose(input_fp);
    }
   
    printf("File %s added successfully as inode %d\n", file_to_add, free_inode);
   
    return 0;
}
