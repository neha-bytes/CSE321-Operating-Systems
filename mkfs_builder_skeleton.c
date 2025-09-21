// Build: gcc -O2 -std=c17 -Wall -Wextra mkfs_minivsfs.c -o mkfs_builder
#define _FILE_OFFSET_BITS 64
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <inttypes.h>
#include <errno.h>
#include <time.h>
#include <assert.h>
#include <unistd.h>
#include <getopt.h>

#define BS 4096u               // block size
#define INODE_SIZE 128u
#define ROOT_INO 1u
#define DIRECT_MAX 12

#pragma pack(push, 1)
typedef struct {
    uint32_t magic;                // 0x4D565346
    uint32_t version;              // 1
    uint32_t block_size;           // 4096
    uint64_t total_blocks;
    uint64_t inode_count;
    uint64_t inode_bitmap_start;
    uint64_t inode_bitmap_blocks;
    uint64_t data_bitmap_start;
    uint64_t data_bitmap_blocks;
    uint64_t inode_table_start;
    uint64_t inode_table_blocks;
    uint64_t data_region_start;
    uint64_t data_region_blocks;
    uint64_t root_inode;           // 1
    uint64_t mtime_epoch;
    uint32_t flags;                // 0
    uint32_t checksum;            // crc32(superblock[0..4091])
} superblock_t;
#pragma pack(pop)
_Static_assert(sizeof(superblock_t) == 116, "superblock must fit in one block");

#pragma pack(push,1)
typedef struct {
    uint16_t mode;
    uint16_t links;
    uint32_t uid;
    uint32_t gid;
    uint64_t size_bytes;
    uint64_t atime;
    uint64_t mtime;
    uint64_t ctime;
    uint32_t direct[12];
    uint32_t reserved_0;
    uint32_t reserved_1;
    uint32_t reserved_2;
    uint32_t proj_id;
    uint32_t uid16_gid16;
    uint64_t xattr_ptr;
    uint64_t inode_crc;   // low 4 bytes store crc32 of bytes [0..119]; high 4 bytes 0
} inode_t;
#pragma pack(pop)
_Static_assert(sizeof(inode_t)==INODE_SIZE, "inode size mismatch");

#pragma pack(push,1)
typedef struct {
    uint32_t inode_no;
    uint8_t type;
    char name[58];
    uint8_t checksum; // XOR of bytes 0..62
} dirent64_t;
#pragma pack(pop)
_Static_assert(sizeof(dirent64_t)==64, "dirent size mismatch");

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

void print_usage() {
    printf("Usage: mkfs_builder --image <filename> --size-kib <size> --inodes <count>\n");
}

int main(int argc, char *argv[]) {
    crc32_init();
    
    char *image_filename = NULL;
    uint64_t size_kib = 0;
    uint64_t inode_count = 0;
    
    // Parse command line arguments
    int opt;
    static struct option long_options[] = {
        {"image", required_argument, 0, 'i'},
        {"size-kib", required_argument, 0, 's'},
        {"inodes", required_argument, 0, 'n'},
        {0, 0, 0, 0}
    };
    
    int option_index = 0;
    while ((opt = getopt_long(argc, argv, "i:s:n:", long_options, &option_index)) != -1) {
        switch (opt) {
            case 'i':
                image_filename = optarg;
                break;
            case 's':
                size_kib = atoi(optarg);
                break;
            case 'n':
                inode_count = atoi(optarg);
                break;
            default:
                print_usage();
                return 1;
        }
    }
    
    if (!image_filename || size_kib == 0 || inode_count == 0) {
        print_usage();
        return 1;
    }
    
    // Calculate basic values
    uint64_t total_blocks = (size_kib * 1024) / BS;
    uint64_t inode_table_blocks = (inode_count * INODE_SIZE + BS - 1) / BS;
    
    // Create superblock
    superblock_t sb = {
        .magic = 0x4D565346,
        .version = 1,
        .block_size = BS,
        .total_blocks = total_blocks,
        .inode_count = inode_count,
        .inode_bitmap_start = 1,
        .inode_bitmap_blocks = 1,
        .data_bitmap_start = 2,
        .data_bitmap_blocks = 1,
        .inode_table_start = 3,
        .inode_table_blocks = inode_table_blocks,
        .data_region_start = 3 + inode_table_blocks,
        .data_region_blocks = total_blocks - 3 - inode_table_blocks,
        .root_inode = ROOT_INO,
        .mtime_epoch = time(NULL),
        .flags = 0
    };
    
    superblock_crc_finalize(&sb);
    
    // Create root inode
    inode_t root_inode = {
        .mode = 0040000,  // directory
        .links = 2,       // . and ..
        .uid = 0,
        .gid = 0,
        .size_bytes = BS, // one block for directory entries
        .atime = time(NULL),
        .mtime = time(NULL),
        .ctime = time(NULL),
        .direct = {sb.data_region_start}, // first data block
        .reserved_0 = 0,
        .reserved_1 = 0,
        .reserved_2 = 0,
        .proj_id = 6,     // replace with your group ID
        .uid16_gid16 = 0,
        .xattr_ptr = 0
    };
    
    inode_crc_finalize(&root_inode);
    
    // Create directory entries for root
    dirent64_t dot_entry = {
        .inode_no = ROOT_INO,
        .type = 2,        // directory
        .name = "."
    };
    dirent_checksum_finalize(&dot_entry);
    
    dirent64_t dotdot_entry = {
        .inode_no = ROOT_INO,
        .type = 2,        // directory
        .name = ".."
    };
    dirent_checksum_finalize(&dotdot_entry);
    
    // Create file and write everything
    FILE *fp = fopen(image_filename, "wb");
    if (!fp) {
        printf("Error creating file: %s\n", image_filename);
        return 1;
    }
    
    // Write superblock
    fwrite(&sb, sizeof(sb), 1, fp);
    
    // Write inode bitmap (mark inode 1 as used)
    uint8_t inode_bitmap[BS] = {0};
    inode_bitmap[0] = 0x80; // first bit set (inode 1)
    fwrite(inode_bitmap, BS, 1, fp);
    
    // Write data bitmap (mark first data block as used)
    uint8_t data_bitmap[BS] = {0};
    data_bitmap[0] = 0x80; // first bit set
    fwrite(data_bitmap, BS, 1, fp);
    
    // Write inode table
    // First, write empty inodes
    inode_t empty_inode = {0};
    for (uint64_t i = 0; i < inode_count; i++) {
        if (i == 0) { // root inode at position 0 (inode number 1)
            fwrite(&root_inode, sizeof(root_inode), 1, fp);
        } else {
            fwrite(&empty_inode, sizeof(empty_inode), 1, fp);
        }
    }
    
    // Write data blocks
    // First data block contains directory entries
    uint8_t data_block[BS] = {0};
    memcpy(data_block, &dot_entry, sizeof(dot_entry));
    memcpy(data_block + sizeof(dot_entry), &dotdot_entry, sizeof(dotdot_entry));
    fwrite(data_block, BS, 1, fp);
    
    // Write remaining empty data blocks
    uint8_t empty_block[BS] = {0};
    uint64_t remaining_blocks = sb.data_region_blocks - 1;
    for (uint64_t i = 0; i < remaining_blocks; i++) {
        fwrite(empty_block, BS, 1, fp);
    }
    
    fclose(fp);
    printf("File system created successfully: %s\n", image_filename);
    
    return 0;
}
