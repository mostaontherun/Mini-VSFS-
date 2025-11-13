// Build: gcc -O2 -std=c17 -Wall -Wextra mkfs_builder.c -o mkfs_builder
#define _FILE_OFFSET_BITS 64
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <inttypes.h>
#include <errno.h>
#include <time.h>
#include <assert.h>

#define BS 4096u               // block size
#define INODE_SIZE 128u
#define ROOT_INO 1u
#define DIRECT_MAX 12

// ====================== On-disk structures ======================
#pragma pack(push, 1)
typedef struct {
    uint32_t magic;               // 0x4D565346 ("MVSF")
    uint32_t version;             // 1
    uint32_t block_size;          // 4096
    uint64_t total_blocks;

    uint64_t inode_count;
    uint64_t inode_bitmap_start;  // block index
    uint64_t inode_bitmap_blocks; // = 1
    uint64_t data_bitmap_start;   // block index
    uint64_t data_bitmap_blocks;  // = 1
    uint64_t inode_table_start;   // block index
    uint64_t inode_table_blocks;  // ceil(inode_count*128 / 4096)
    uint64_t data_region_start;   // block index
    uint64_t data_region_blocks;  // remaining
    uint64_t root_inode;          // = 1
    uint64_t mtime_epoch;         // build time
    uint32_t flags;               // 0

    uint32_t checksum;            // crc32(superblock[0..4091]) (must be last)
} superblock_t;
#pragma pack(pop)
_Static_assert(sizeof(superblock_t) == 116, "superblock must fit in one block");

#pragma pack(push,1)
typedef struct {
    uint16_t mode;          // 0x8000=file, 0x4000=dir
    uint16_t links;         // root=2 (., ..); files=1
    uint32_t uid;           // 0
    uint32_t gid;           // 0
    uint64_t size_bytes;    // file/dir size
    uint64_t atime;         // now
    uint64_t mtime;         // now
    uint64_t ctime;         // now
    uint32_t direct[12];    // MiniVSFS: use RELATIVE index inside data region
    uint32_t reserved_0;    // 0
    uint32_t reserved_1;    // 0
    uint32_t reserved_2;    // 0
    uint32_t proj_id;       // group id if you want; keep 0
    uint32_t uid16_gid16;   // 0
    uint64_t xattr_ptr;     // 0

    uint64_t inode_crc;     // low 4 bytes = crc32 of bytes [0..119]
} inode_t;
#pragma pack(pop)
_Static_assert(sizeof(inode_t)==INODE_SIZE, "inode size mismatch");

#pragma pack(push,1)
typedef struct {
    uint32_t inode_no;           // 0 if free (NOTE: inodes are 1-based)
    uint8_t  type;               // 1=file, 2=dir
    char     name[58];           // not null-terminated if full
    uint8_t  checksum;           // XOR of bytes 0..62
} dirent64_t;
#pragma pack(pop)
_Static_assert(sizeof(dirent64_t)==64, "dirent size mismatch");

// ========================== CRC32 helpers ==========================
static uint32_t CRC32_TAB[256];
static void crc32_init(void){
    for (uint32_t i=0;i<256;i++){
        uint32_t c=i;
        for(int j=0;j<8;j++) c = (c&1)?(0xEDB88320u^(c>>1)):(c>>1);
        CRC32_TAB[i]=c;
    }
}
static uint32_t crc32(const void* data, size_t n){
    const uint8_t* p=(const uint8_t*)data; uint32_t c=0xFFFFFFFFu;
    for(size_t i=0;i<n;i++) c = CRC32_TAB[(c^p[i])&0xFF] ^ (c>>8);
    return c ^ 0xFFFFFFFFu;
}

static uint32_t superblock_crc_finalize(superblock_t *sb) {
    sb->checksum = 0;
    // spec: compute over exactly one block minus checksum field
    uint8_t block[BS]; memset(block, 0, BS);
    memcpy(block, sb, sizeof(*sb));
    // checksum is last 4 bytes of the block; zero already
    uint32_t s = crc32(block, BS - 4);
    sb->checksum = s;
    return s;
}
static void inode_crc_finalize(inode_t* ino){
    uint8_t tmp[INODE_SIZE]; memcpy(tmp, ino, INODE_SIZE);
    memset(&tmp[120], 0, 8);
    uint32_t c = crc32(tmp, 120);
    ino->inode_crc = (uint64_t)c;
}
static void dirent_checksum_finalize(dirent64_t* de) {
    const uint8_t* p = (const uint8_t*)de;
    uint8_t x = 0;
    for (int i = 0; i < 63; i++) x ^= p[i];   // covers ino(4) + type(1) + name(58)
    de->checksum = x;
}

// ========================== Bitmap helpers =========================
static inline void bit_set(uint8_t *bm, uint64_t idx){ bm[idx/8] |=  (1u << (idx%8)); }
// static inline int  bit_get(const uint8_t *bm, uint64_t idx){ return (bm[idx/8] >> (idx%8)) & 1; }

// ============================= Main ================================
static int parse_u64(const char* s, uint64_t* out){
    char* end=NULL; errno=0;
    unsigned long long v = strtoull(s,&end,10);
    if(errno!=0 || end==s || *end!='\0') return -1;
    *out = (uint64_t)v; return 0;
}

int main(int argc, char** argv){
    crc32_init();

    const char* image   = NULL;
    uint64_t size_kib   = 0;
    uint64_t inode_cnt  = 0;

    // ---------------- Parse CLI ----------------
    for(int i=1;i<argc;i++){
        if(strcmp(argv[i],"--image")==0 && i+1<argc)      image = argv[++i];
        else if(strcmp(argv[i],"--size-kib")==0 && i+1<argc) {
            if(parse_u64(argv[++i], &size_kib)!=0){ fprintf(stderr,"Invalid --size-kib\n"); return EXIT_FAILURE; }
        }
        else if(strcmp(argv[i],"--inodes")==0 && i+1<argc) {
            if(parse_u64(argv[++i], &inode_cnt)!=0){ fprintf(stderr,"Invalid --inodes\n"); return EXIT_FAILURE; }
        }
        else {
            fprintf(stderr,"Unknown parameter %s\n", argv[i]); return EXIT_FAILURE;
        }
    }
    if(!image || !size_kib || !inode_cnt){
        fprintf(stderr,"Usage: %s --image out.img --size-kib <180..4096,multiple of 4> --inodes <128..512>\n", argv[0]);
        return EXIT_FAILURE;
    }

    // ---------------- Validate ----------------
    if(size_kib < 180 || size_kib > 4096 || (size_kib % 4)!=0){
        fprintf(stderr,"--size-kib must be 180..4096 and a multiple of 4\n");
        return EXIT_FAILURE;
    }
    if(inode_cnt < 128 || inode_cnt > 512){
        fprintf(stderr,"--inodes must be 128..512\n");
        return EXIT_FAILURE;
    }

    const uint64_t total_blocks = (size_kib * 1024u) / BS; // size_kib / 4
    if(total_blocks < 8){
        fprintf(stderr,"image too small\n"); return EXIT_FAILURE;
    }

    // Layout:
    // 0: superblock
    // 1: inode bitmap (1 block)
    // 2: data bitmap  (1 block)
    // [3 .. 3+inode_tbl_blocks-1]: inode table
    // [data_region_start .. end]: data blocks
    const uint64_t inode_tbl_bytes   = inode_cnt * INODE_SIZE;
    const uint64_t inode_tbl_blocks  = (inode_tbl_bytes + BS - 1)/BS;

    const uint64_t inode_bitmap_start = 1;
    const uint64_t data_bitmap_start  = 2;
    const uint64_t inode_table_start  = 3;
    const uint64_t data_region_start  = inode_table_start + inode_tbl_blocks;

    if(data_region_start >= total_blocks){
        fprintf(stderr,"Not enough space for data region (increase --size-kib or reduce --inodes)\n");
        return EXIT_FAILURE;
    }
    const uint64_t data_region_blocks = total_blocks - data_region_start;

    // ---------------- Allocate image buffer ----------------
    const uint64_t img_bytes = total_blocks * (uint64_t)BS;
    uint8_t* img = (uint8_t*)calloc(1, img_bytes);
    if(!img){ perror("calloc image"); return EXIT_FAILURE; }

    // ---------------- Build Superblock ----------------
    superblock_t sb;
    memset(&sb, 0, sizeof(sb));
    sb.magic               = 0x4D565346u; // "MVSF"
    sb.version             = 1;
    sb.block_size          = BS;
    sb.total_blocks        = total_blocks;

    sb.inode_count         = inode_cnt;
    sb.inode_bitmap_start  = inode_bitmap_start;
    sb.inode_bitmap_blocks = 1;
    sb.data_bitmap_start   = data_bitmap_start;
    sb.data_bitmap_blocks  = 1;
    sb.inode_table_start   = inode_table_start;
    sb.inode_table_blocks  = inode_tbl_blocks;
    sb.data_region_start   = data_region_start;
    sb.data_region_blocks  = data_region_blocks;

    sb.root_inode          = ROOT_INO; // 1
    sb.mtime_epoch         = (uint64_t)time(NULL);
    sb.flags               = 0;
    superblock_crc_finalize(&sb);

    // write superblock to block 0 (and keep rest of the block zero)
    memcpy(img, &sb, sizeof(sb));

    // ---------------- Bitmaps ----------------
    uint8_t* inode_bm = img + inode_bitmap_start * BS;
    uint8_t* data_bm  = img + data_bitmap_start  * BS;
    memset(inode_bm, 0, BS);
    memset(data_bm,  0, BS);

    // Mark inode #1 (root) allocated -> bit index 0
    bit_set(inode_bm, 0);

    // We'll allocate first data block in data region for root directory
    // Convention: direct[] stores RELATIVE data-region index, so root uses index 0
    bit_set(data_bm, 0);

    // ---------------- Inode table ----------------
    uint8_t* inode_tbl = img + inode_table_start * BS;
    memset(inode_tbl, 0, inode_tbl_blocks * BS);

    inode_t root;
    memset(&root, 0, sizeof(root));
    root.mode       = 0x4000; // directory
    root.links      = 2;      // "." and ".."
    root.uid        = 0;
    root.gid        = 0;
    root.size_bytes = 2 * sizeof(dirent64_t); // two entries
    uint64_t now = (uint64_t)time(NULL);
    root.atime = root.mtime = root.ctime = now;
    root.direct[0]  = 0;   // RELATIVE index into data region (0 => first data block)
    // others remain 0 (unused)
    inode_crc_finalize(&root);

    // place root inode at index 0 (inode #1)
    memcpy(inode_tbl + 0*INODE_SIZE, &root, sizeof(root));

    // ---------------- Root directory data block ----------------
    uint8_t* data_region = img + data_region_start * BS;

    // clear first data block
    memset(data_region + 0*BS, 0, BS);

    // dirent for "."
    dirent64_t dot;
    memset(&dot, 0, sizeof(dot));
    dot.inode_no = ROOT_INO; // 1
    dot.type     = 2;        // dir
    strncpy(dot.name, ".", sizeof(dot.name)-1);
    dirent_checksum_finalize(&dot);

    // dirent for ".." (points to itself in root)
    dirent64_t dotdot;
    memset(&dotdot, 0, sizeof(dotdot));
    dotdot.inode_no = ROOT_INO;
    dotdot.type     = 2;
    strncpy(dotdot.name, "..", sizeof(dotdot.name)-1);
    dirent_checksum_finalize(&dotdot);

    // write the two entries at start of the block
    memcpy(data_region + 0*BS + 0*sizeof(dirent64_t), &dot,    sizeof(dot));
    memcpy(data_region + 0*BS + 1*sizeof(dirent64_t), &dotdot, sizeof(dotdot));
    // rest remain zero (free entries)

    // ---------------- Write image to disk ----------------
    FILE* f = fopen(image, "wb");
    if(!f){ perror("fopen image"); free(img); return EXIT_FAILURE; }
    if(fwrite(img, 1, img_bytes, f) != img_bytes){ perror("write image"); fclose(f); free(img); return EXIT_FAILURE; }
    fclose(f);
    free(img);

    printf("MiniVSFS image '%s' created successfully.\n", image);
    printf("  size_kib=%" PRIu64 "  total_blocks=%" PRIu64 "\n", size_kib, total_blocks);
    printf("  inodes=%" PRIu64 "  inode_table_blocks=%" PRIu64 "  data_region_blocks=%" PRIu64 "\n",
           inode_cnt, inode_tbl_blocks, data_region_blocks);
    return 0;
}

