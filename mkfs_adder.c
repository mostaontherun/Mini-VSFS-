// mkfs_adder.c
#define _FILE_OFFSET_BITS 64
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <inttypes.h>
#include <errno.h>
#include <time.h>
#include <sys/stat.h>
#include <assert.h>

#define BS 4096u
#define INODE_SIZE 128u
#define ROOT_INO 1u
#define DIRECT_MAX 12
#define MAX_FILENAME 58

#pragma pack(push, 1)
typedef struct {
    uint32_t magic;              // 0x4D565346 ("MVSF")
    uint32_t version;            // 1
    uint32_t block_size;         // 4096
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
    uint64_t root_inode;
    uint64_t mtime_epoch;
    uint32_t flags;

    uint32_t checksum;           // last field
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
    uint32_t direct[DIRECT_MAX];
    uint32_t reserved_0;
    uint32_t reserved_1;
    uint32_t reserved_2;
    uint32_t proj_id;
    uint32_t uid16_gid16;
    uint64_t xattr_ptr;
    uint64_t inode_crc;          // low 4 bytes = crc32 of bytes [0..119]
} inode_t;
#pragma pack(pop)
_Static_assert(sizeof(inode_t) == INODE_SIZE, "inode size mismatch");

#pragma pack(push,1)
typedef struct {
    uint32_t inode_no;           // 0 if free
    uint8_t type;                // 1=file, 2=dir
    char name[MAX_FILENAME];     // not null-terminated if full
    uint8_t checksum;            // XOR of bytes 0..62
} dirent64_t;
#pragma pack(pop)
_Static_assert(sizeof(dirent64_t) == 64, "dirent size mismatch");

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
    uint8_t block[BS]; memset(block, 0, BS);
    memcpy(block, sb, sizeof(*sb));
    uint32_t s = crc32(block, BS-4);
    sb->checksum = s;
    return s;
}

static void inode_crc_finalize(inode_t* ino){
    uint8_t tmp[INODE_SIZE]; memcpy(tmp, ino, INODE_SIZE);
    memset(&tmp[120], 0, 8);
    uint32_t c = crc32(tmp, 120);
    ino->inode_crc = (uint64_t)c;
}

static void dirent_checksum_finalize(dirent64_t* de){
    const uint8_t* p = (const uint8_t*)de;
    uint8_t x = 0;
    for(int i=0;i<63;i++) x ^= p[i];
    de->checksum = x;
}

// ========================== Bitmap helpers ==========================
static inline int bit_get(const uint8_t *bm, uint64_t idx){ return (bm[idx/8] >> (idx%8)) & 1; }
static inline void bit_set(uint8_t *bm, uint64_t idx){ bm[idx/8] |=  (1u << (idx%8)); }
static inline void bit_clear(uint8_t *bm, uint64_t idx){ bm[idx/8] &= ~(1u << (idx%8)); }

// ========================== File helpers ==========================
static void *read_file_all(const char *path, size_t *out_size){
    FILE *f = fopen(path,"rb");
    if(!f) return NULL;
    if(fseek(f,0,SEEK_END)!=0){ fclose(f); return NULL; }
    long s = ftell(f);
    if(s<0){ fclose(f); return NULL; }
    if(fseek(f,0,SEEK_SET)!=0){ fclose(f); return NULL; }
    size_t alloc_size = (size_t)s ? (size_t)s : 1;
    void *buf = malloc(alloc_size);
    if(!buf){ fclose(f); return NULL; }
    if(s>0 && fread(buf,1,(size_t)s,f)!=(size_t)s){ free(buf); fclose(f); return NULL; }
    fclose(f);
    *out_size = (size_t)s;
    return buf;
}

// ========================== Main ==========================
int main(int argc, char **argv){
    crc32_init();

    const char *input_img=NULL, *output_img=NULL, *host_file=NULL;
    // parse CLI
    for(int i=1;i<argc;i++){
        if(strcmp(argv[i],"--input")==0 && i+1<argc) input_img=argv[++i];
        else if(strcmp(argv[i],"--output")==0 && i+1<argc) output_img=argv[++i];
        else if(strcmp(argv[i],"--file")==0 && i+1<argc) host_file=argv[++i];
        else { fprintf(stderr,"Unknown parameter %s\n",argv[i]); return EXIT_FAILURE;}
    }
    if(!input_img || !output_img || !host_file){
        fprintf(stderr,"Usage: %s --input in.img --output out.img --file filename\n",argv[0]);
        return EXIT_FAILURE;
    }

    // read input image
    size_t img_size;
    uint8_t *img = read_file_all(input_img,&img_size);
    if(!img){ perror("reading input image"); return EXIT_FAILURE; }
    if(img_size < BS){ fprintf(stderr,"input image too small\n"); free(img); return EXIT_FAILURE; }

    // parse superblock
    superblock_t sb;
    memcpy(&sb,img,sizeof(sb));
    if(sb.magic != 0x4D565346u){ fprintf(stderr,"bad magic\n"); free(img); return EXIT_FAILURE; }

    // pointers
    uint8_t *inode_bm = img + sb.inode_bitmap_start * BS;
    uint8_t *data_bm  = img + sb.data_bitmap_start  * BS;
    uint8_t *inode_tbl = img + sb.inode_table_start * BS;
    uint8_t *data_region = img + sb.data_region_start * BS;

    // read host file
    struct stat st;
    if(stat(host_file,&st)!=0){ perror("stat host file"); free(img); return EXIT_FAILURE; }
    if(!S_ISREG(st.st_mode)){ fprintf(stderr,"host file is not regular\n"); free(img); return EXIT_FAILURE; }
    uint64_t file_size = (uint64_t)st.st_size;
    uint64_t need_blocks = (file_size+BS-1)/BS;
    if(need_blocks==0) need_blocks=1;
    if(need_blocks>DIRECT_MAX){ fprintf(stderr,"file too large\n"); free(img); return EXIT_FAILURE; }

    // find free inode
    int found_inode=-1;
    for(uint64_t i=0;i<sb.inode_count;i++){
        if(!bit_get(inode_bm,i)){ found_inode=(int)i; break; }
    }
    if(found_inode<0){ fprintf(stderr,"no free inode\n"); free(img); return EXIT_FAILURE; }
    uint32_t new_ino = (uint32_t)(found_inode+1);

    // find free data blocks
    uint32_t blocks_found[DIRECT_MAX];
    uint64_t found=0;
    for(uint64_t i=0;i<sb.data_region_blocks && found<need_blocks;i++){
        if(!bit_get(data_bm,i)) blocks_found[found++] = (uint32_t)i;
    }
    if(found<need_blocks){ fprintf(stderr,"not enough free data blocks\n"); free(img); return EXIT_FAILURE; }

    // read host file content
    size_t fsize;
    uint8_t *file_buf = read_file_all(host_file,&fsize);
    if(!file_buf){ perror("reading host file"); free(img); return EXIT_FAILURE; }

    // create inode
    inode_t ino;
    memset(&ino,0,sizeof(ino));
    ino.mode=0x8000; ino.links=1; ino.uid=0; ino.gid=0; ino.size_bytes=file_size;
    time_t now = time(NULL); ino.atime=ino.mtime=ino.ctime=(uint64_t)now;
    for(uint64_t i=0;i<need_blocks;i++) ino.direct[i]=blocks_found[i];
    inode_crc_finalize(&ino);

    // mark inode and data blocks
    bit_set(inode_bm,(uint64_t)found_inode);
    for(uint64_t i=0;i<need_blocks;i++){
        bit_set(data_bm,blocks_found[i]);
        uint64_t copy_off=i*BS;
        uint64_t remain = file_size>copy_off ? file_size-copy_off:0;
        uint64_t tocopy = remain>BS ? BS:remain;
        if(tocopy) memcpy(data_region+((uint64_t)blocks_found[i]*BS),file_buf+copy_off,tocopy);
        if(tocopy<BS) memset(data_region+((uint64_t)blocks_found[i]*BS)+tocopy,0,BS-tocopy);
    }

    // write inode
    memcpy(inode_tbl + ((uint64_t)found_inode*INODE_SIZE), &ino, INODE_SIZE);

    // update root directory
    inode_t root_inode;
    memcpy(&root_inode,inode_tbl + ((ROOT_INO-1)*INODE_SIZE),INODE_SIZE);
    uint32_t root_rel = root_inode.direct[0];
    if(root_rel>=sb.data_region_blocks){ fprintf(stderr,"root data block invalid\n"); free(file_buf); free(img); return EXIT_FAILURE; }

    uint8_t *root_block = data_region + ((uint64_t)root_rel*BS);
    int added=0;
    for(size_t off=0; off+sizeof(dirent64_t)<=BS; off+=sizeof(dirent64_t)){
        dirent64_t *de = (dirent64_t *)(root_block+off);
        if(de->inode_no==0){
            dirent64_t nde; memset(&nde,0,sizeof(nde));
            nde.inode_no=new_ino; nde.type=1;
            const char *slash=strrchr(host_file,'/');
            const char *fname=slash?slash+1:host_file;
            strncpy(nde.name,fname,MAX_FILENAME-1);
            dirent_checksum_finalize(&nde);
            memcpy(de,&nde,sizeof(nde));
            added=1; break;
        }
    }
    if(!added){ fprintf(stderr,"no free dirent slot in root\n"); bit_clear(inode_bm,(uint64_t)found_inode);
        for(uint64_t i=0;i<need_blocks;i++) bit_clear(data_bm,blocks_found[i]);
        free(file_buf); free(img); return EXIT_FAILURE;
    }

    root_inode.links+=1;
    root_inode.size_bytes+=sizeof(dirent64_t);
    inode_crc_finalize(&root_inode);
    memcpy(inode_tbl + ((ROOT_INO-1)*INODE_SIZE), &root_inode, INODE_SIZE);

    // update superblock
    sb.mtime_epoch=(uint64_t)time(NULL);
    superblock_crc_finalize(&sb);
    memcpy(img,&sb,sizeof(sb));

    // write output
    FILE *of=fopen(output_img,"wb");
    if(!of){ perror("fopen output"); free(file_buf); free(img); return EXIT_FAILURE; }
    if(fwrite(img,1,img_size,of)!=img_size){ perror("write output"); fclose(of); free(file_buf); free(img); return EXIT_FAILURE; }
    fclose(of);

    printf("File '%s' added as inode %u (%" PRIu64 " bytes) into '%s'.\n", host_file,new_ino,file_size,output_img);

    free(file_buf); free(img);
    return EXIT_SUCCESS;
}

