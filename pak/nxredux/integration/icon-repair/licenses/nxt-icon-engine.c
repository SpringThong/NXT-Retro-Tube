/* NXT Icon Repair 1.2 - table-only ARM64 ELF patcher, GPL-3.0-or-later.
 * Never replaces a launcher with a build from a different firmware.
 * Only --create/--unpatch write, exclusively to a new output file.
 */
#define _POSIX_C_SOURCE 200809L
#include <elf.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

typedef struct { unsigned adrp, add; } Ref;
typedef struct { const char *name; const uint32_t *words,*masks; unsigned size,nrefs; Ref refs[2]; unsigned count; } Pattern;
#include "patterns.h"
typedef struct {uint64_t off; uint32_t word, reserved;} Change;
typedef struct {
    char magic[16]; uint64_t original_size, function_offset;
    uint32_t original_crc, file_crc, ph_index, nchanges;
    Elf64_Phdr original_ph; Change changes[8];
} Footer;
static const char magic[16]="NXTICONTABLE1";
/* Keep the original footer ABI: BookNest's compatible repair can recognise it
 * and leaves the common table alone once its own BookNest entry is present. */
static const char *names[]={
    "NXT Retro Music","NXT Retro Music Soloist","NXT Music Soloist",
    "NXT Retro Tube","NXT Tube","NXT Retro Movie","NXT Movie",
    "NXT Retro BookNest","NXT BookNest","NXT Retro Comic",
    "NXT Retro BrickTok","NXT BrickTok","NXT Retro BrickTok Login1",
    "NXT Retro File","NXT File","NXT Retro Files","NXT Files",
    "NXT Icon Repair","NXT Icon Restore"
};
static const char *icons[]={
    "menu_icon_nxt.png","menu_icon_nxt.png","menu_icon_nxt.png",
    "menu_icon_nxt_tube.png","menu_icon_nxt_tube.png",
    "menu_icon_nxt_retro_movie.png","menu_icon_nxt_retro_movie.png",
    "menu_icon_nxt_booknest.png","menu_icon_nxt_booknest.png","menu_icon_nxt_booknest.png",
    "menu_icon_nxt_bricktok.png","menu_icon_nxt_bricktok.png","menu_icon_nxt_bricktok.png",
    "menu_icon_nxt_file.png","menu_icon_nxt_file.png","menu_icon_nxt_file.png","menu_icon_nxt_file.png",
    "menu_icon_nxt_repair.png","menu_icon_nxt_repair.png"
};
typedef struct {unsigned char *b; size_t n; Elf64_Ehdr *eh; Elf64_Phdr *ph;} Image;
static int fail(const char *s) {fprintf(stderr,"UNSUPPORTED: %s\n",s);return 2;}
static int range(size_t total,uint64_t off,uint64_t n) {return off<=total && n<=total-off;}
static uint32_t get32(const void *p) {uint32_t n;memcpy(&n,p,4);return n;}
static uint64_t get64(const void *p) {uint64_t n;memcpy(&n,p,8);return n;}
static void put32(void *p,uint32_t n) {memcpy(p,&n,4);}
static void put64(void *p,uint64_t n) {memcpy(p,&n,8);}
static uint32_t crc32(const void *data,size_t n) {
    const unsigned char *p=data;uint32_t v=~0u;
    while(n--){v^=*p++;for(unsigned j=0;j<8;j++)v=(v>>1)^(0xedb88320u&-(v&1u));}
    return ~v;
}
static int load(const char *path,Image *im) {
    struct stat st;int fd=open(path,O_RDONLY|O_NOFOLLOW);
    if(fd<0 || fstat(fd,&st) || !S_ISREG(st.st_mode) || st.st_size<64 || st.st_size>32*1024*1024){if(fd>=0)close(fd);return fail("not a regular launcher ELF (64 B..32 MiB)");}
    im->n=(size_t)st.st_size;im->b=malloc(im->n);
    if(!im->b){close(fd);return fail("memory");}
    size_t done=0;while(done<im->n){ssize_t r=read(fd,im->b+done,im->n-done);if(r<=0){close(fd);return fail("read");}done+=(size_t)r;}close(fd);
    im->eh=(Elf64_Ehdr*)im->b;Elf64_Ehdr *e=im->eh;
    if(memcmp(e->e_ident,ELFMAG,4)||e->e_ident[EI_CLASS]!=ELFCLASS64||e->e_ident[EI_DATA]!=ELFDATA2LSB||e->e_machine!=EM_AARCH64||e->e_type!=ET_EXEC||e->e_ehsize!=64||e->e_phentsize!=56||e->e_phnum<2||e->e_phnum>64||e->e_phoff%8||!range(im->n,e->e_phoff,(uint64_t)e->e_phnum*56))return fail("requires little-endian ARM64 ET_EXEC with standard headers");
    im->ph=(Elf64_Phdr*)(im->b+e->e_phoff);
    for(unsigned j=0;j<e->e_phnum;j++) {
        Elf64_Phdr *p=&im->ph[j];
        if(!range(im->n,p->p_offset,p->p_filesz)||p->p_memsz<p->p_filesz||p->p_vaddr>UINT64_MAX-p->p_memsz)return fail("invalid segment bounds");
    }
    return 0;
}
static uint64_t va_to_off(Image *im,uint64_t va,uint64_t n) {
    for(unsigned j=0;j<im->eh->e_phnum;j++) {Elf64_Phdr *p=&im->ph[j];if(p->p_type==PT_LOAD&&va>=p->p_vaddr&&va-p->p_vaddr<=p->p_filesz&&n<=p->p_filesz-(va-p->p_vaddr))return p->p_offset+va-p->p_vaddr;}
    return UINT64_MAX;
}
static uint64_t off_to_va(Image *im,uint64_t off) {
    for(unsigned j=0;j<im->eh->e_phnum;j++) {Elf64_Phdr *p=&im->ph[j];if(p->p_type==PT_LOAD&&off>=p->p_offset&&off-p->p_offset<p->p_filesz)return p->p_vaddr+off-p->p_offset;}
    return UINT64_MAX;
}
static const char *string_at(Image *im,uint64_t va) {
    uint64_t off=va_to_off(im,va,1);if(off==UINT64_MAX)return NULL;
    for(unsigned j=0;j<129;j++){if(!range(im->n,off+j,1))return NULL;unsigned char c=im->b[off+j];if(c==0)return j?(const char*)(im->b+off):NULL;if(c<32||c>126)return NULL;}
    return NULL;
}
static uint64_t address(Image *im,uint64_t start,Ref r) {
    uint32_t a=get32(im->b+start+r.adrp), b=get32(im->b+start+r.add);
    if((a&0x9f000000u)!=0x90000000u||(b&0xffc00000u)!=0x91000000u||(a&31)!=((b>>5)&31))return UINT64_MAX;
    int64_t page=(((int64_t)((a>>5)&0x7ffff)<<2)|((a>>29)&3));
    if(page&(1<<20))page-=1<<21;
    return (uint64_t)((int64_t)(off_to_va(im,start+r.adrp)&~4095ull)+page*4096+((b>>10)&4095));
}
static int installed(Image *im,Footer *out) {
    if(im->n<sizeof(Footer))return 0;
    Footer f;memcpy(&f,im->b+im->n-sizeof f,sizeof f);
    if(memcmp(f.magic,magic,16))return 0;
    if(f.original_size>=im->n||f.original_size<64||f.nchanges>8||f.ph_index>=im->eh->e_phnum)return -1;
    size_t o=im->n-sizeof f+offsetof(Footer,file_crc);uint32_t saved=get32(im->b+o);put32(im->b+o,0);uint32_t actual=crc32(im->b,im->n);put32(im->b+o,saved);
    if(saved!=actual)return -1;
    if(out)*out=f;
    return 1;
}
static int write_new(const char *path,const void *data,size_t n) {
    int fd=open(path,O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW,0755);if(fd<0){perror("output (must not exist)");return 1;}
    size_t done=0;while(done<n){ssize_t w=write(fd,(const char*)data+done,n-done);if(w<=0){close(fd);unlink(path);return 1;}done+=(size_t)w;}
    if(fsync(fd)||close(fd)){unlink(path);return 1;}return 0;
}
static int record(Image *im,Footer *f,uint64_t off,uint32_t word) {
    if(f->nchanges>=8||!range(im->n,off,4))return 1;
    f->changes[f->nchanges++]=(Change){off,get32(im->b+off),0};put32(im->b+off,word);return 0;
}
static int set_address(Image *im,Footer *f,uint64_t start,Ref r,uint64_t dst) {
    int64_t delta=(int64_t)(dst&~4095ull)-(int64_t)(off_to_va(im,start+r.adrp)&~4095ull);
    if(delta%4096||delta/4096<-(1<<20)||delta/4096>=(1<<20))return 1;
    uint32_t imm=(uint32_t)(delta/4096)&0x1fffff, a=get32(im->b+start+r.adrp),b=get32(im->b+start+r.add);
    a=(a&0x9f00001fu)|((imm&3)<<29)|((imm>>2)<<5);
    b=(b&0xffc003ffu)|((dst&4095)<<10);
    return record(im,f,start+r.adrp,a)||record(im,f,start+r.add,b);
}
static int complete_map(Image *im,const Footer *f) {
    const Pattern *pat=NULL;
    for(unsigned p=0;p<sizeof patterns/sizeof *patterns;p++) {
        const Pattern *t=&patterns[p];
        if(!range(im->n,f->function_offset,t->size))continue;
        unsigned k=0;
        for(;k<t->size/4;k++)if((get32(im->b+f->function_offset+k*4)&t->masks[k])!=(t->words[k]&t->masks[k]))break;
        if(k==t->size/4){if(pat)return 0;pat=t;}
    }
    if(!pat)return 0;
    uint64_t table=address(im,f->function_offset,pat->refs[0]);
    for(unsigned r=1;r<pat->nrefs;r++)if(address(im,f->function_offset,pat->refs[r])!=table)return 0;
    unsigned rows=(get32(im->b+f->function_offset+pat->count)>>10)&4095;
    uint64_t off=va_to_off(im,table,(uint64_t)rows*16);
    if(rows<10||rows>160||off==UINT64_MAX)return 0;
    for(unsigned a=0;a<sizeof names/sizeof *names;a++) {
        int found=0;
        for(unsigned r=0;r<rows;r++) {
            const char *name=string_at(im,get64(im->b+off+r*16));
            const char *icon=string_at(im,get64(im->b+off+r*16+8));
            if(!name||!icon)return 0;
            if(!strcmp(name,names[a])){found=!strcmp(icon,icons[a]);break;}
        }
        if(!found)return 0;
    }
    return 1;
}
int main(int argc,char **argv) {
    if(argc<3||argc>4){fprintf(stderr,"Usage: nxt-icon-engine --probe INPUT | --create INPUT OUTPUT | --unpatch INPUT OUTPUT\n");return 1;}
    int probe=!strcmp(argv[1],"--probe"), undo=!strcmp(argv[1],"--unpatch"), create=!strcmp(argv[1],"--create");
    if((!probe&&!undo&&!create)||(probe?argc!=3:argc!=4))return 1;
    Image im={0};if(load(argv[2],&im))return 2;
    Footer old;int has=installed(&im,&old);
    if(has<0)return fail("NXT footer corrupt; launcher left untouched");
    if(undo) {
        if(!has)return fail("not an NXT Icon Repair launcher");
        for(unsigned i=0;i<old.nchanges;i++) {
            if(!range(old.original_size,old.changes[i].off,4))return fail("undo bounds");
            put32(im.b+old.changes[i].off,old.changes[i].word);
        }
        memcpy(&im.ph[old.ph_index],&old.original_ph,sizeof(Elf64_Phdr));
        if(crc32(im.b,old.original_size)!=old.original_crc)return fail("undo checksum mismatch");
        int ret=write_new(argv[3],im.b,old.original_size);if(!ret)puts("RESTORED: exact pre-repair launcher");return ret;
    }
    if(has && complete_map(&im,&old)) {
        puts("INSTALLED: Music + Tube + Movie + BookNest + BrickTok + File table intact");
        return create?3:0;
    }
    if(has) {
        /* Upgrade either the old three-app tool or BookNest's nine-entry table
         * in memory, preserving this ROM's exact original undo target. */
        for(unsigned i=0;i<old.nchanges;i++) {
            if(!range(old.original_size,old.changes[i].off,4))return fail("upgrade undo bounds");
            put32(im.b+old.changes[i].off,old.changes[i].word);
        }
        memcpy(&im.ph[old.ph_index],&old.original_ph,sizeof(Elf64_Phdr));
        if(crc32(im.b,old.original_size)!=old.original_crc)return fail("upgrade original checksum mismatch");
        im.n=old.original_size;
    }
    const Pattern *pat=NULL;uint64_t found=0;unsigned matches=0;
    for(unsigned j=0;j<im.eh->e_phnum;j++) {
        Elf64_Phdr *seg=&im.ph[j];if(seg->p_type!=PT_LOAD||!(seg->p_flags&PF_X))continue;
        for(uint64_t at=seg->p_offset;at+420<=seg->p_offset+seg->p_filesz;at+=4) {
            for(unsigned p=0;p<sizeof patterns/sizeof *patterns;p++) {
                const Pattern *t=&patterns[p];unsigned k=0;
                for(;k<t->size/4;k++)if((get32(im.b+at+k*4)&t->masks[k])!=(t->words[k]&t->masks[k]))break;
                if(k==t->size/4){matches++;pat=t;found=at;}
            }
        }
    }
    if(matches!=1)return fail("icon lookup function changed or ambiguous; no modification");
    uint64_t table=address(&im,found,pat->refs[0]);
    for(unsigned r=1;r<pat->nrefs;r++)if(address(&im,found,pat->refs[r])!=table)return fail("table references disagree");
    unsigned rows=(get32(im.b+found+pat->count)>>10)&4095;
    uint64_t toff=va_to_off(&im,table,(uint64_t)rows*16);
    if(rows<10||rows>128||toff==UINT64_MAX)return fail("table bounds");
    unsigned settings=0,files=0;
    for(unsigned r=0;r<rows;r++) {
        const char *name=string_at(&im,get64(im.b+toff+r*16));
        const char *icon=string_at(&im,get64(im.b+toff+r*16+8));
        if(!name||!icon||strncmp(icon,"menu_icon_",10)||strchr(icon,'/'))return fail("invalid icon table row");
        if(r==0&&(strcmp(name,"RetroAchievements")||strcmp(icon,"menu_icon_achievements.png")))return fail("first row changed");
        settings+=!strcmp(name,"Settings")&&!strcmp(icon,"menu_icon_settings.png");
        files+=!strcmp(name,"Files")&&!strcmp(icon,"menu_icon_files.png");
    }
    if(settings!=1||files!=1)return fail("launcher identity checks failed");
    int slot=-1,extend=0;uint64_t maxend=0;
    for(unsigned j=0;j<im.eh->e_phnum;j++) {
        Elf64_Phdr *p=&im.ph[j];if(p->p_type==PT_NOTE){if(slot>=0)return fail("multiple NOTE headers");slot=(int)j;}
        if(p->p_type==PT_LOAD && p->p_vaddr+p->p_memsz>maxend)maxend=p->p_vaddr+p->p_memsz;
    }
    if(maxend>UINT64_MAX-0x10000)return fail("virtual address range");
    if(slot<0) {
        /* Legacy Music/Tube installers already use NOTE for an appended icon
         * table. Extend that existing read-only tail instead of replacing it.
         * Require the validated table to live there, no BSS, no permissions
         * changes, and both file/virtual ends to be last. Other layouts stop. */
        for(unsigned j=0;j<im.eh->e_phnum;j++) {
            Elf64_Phdr *p=&im.ph[j];
            if(p->p_type!=PT_LOAD||p->p_flags!=PF_R||p->p_filesz==0||p->p_filesz!=p->p_memsz||
               p->p_offset+p->p_filesz!=im.n||p->p_vaddr+p->p_memsz!=maxend||
               (p->p_align!=0x1000&&p->p_align!=0x10000)||
               p->p_vaddr%p->p_align!=p->p_offset%p->p_align||
               toff<p->p_offset||toff-p->p_offset>p->p_filesz||
               (uint64_t)rows*16>p->p_filesz-(toff-p->p_offset))continue;
            if(slot>=0)return fail("ambiguous read-only icon tail");
            slot=(int)j;
        }
        if(slot<0)return fail("no NOTE header or validated read-only icon tail");
        extend=1;
    }
    for(unsigned j=(unsigned)slot+1;j<im.eh->e_phnum;j++)if(im.ph[j].p_type==PT_LOAD)return fail("LOAD header order");
    printf("%s: %s; %u existing tools preserved; function=0x%"PRIx64"; storage=%s\n",has?"UPGRADE":"READY",pat->name,rows,off_to_va(&im,found),extend?"extend-readonly-icons":"new-readonly-segment");
    if(probe)return 0;
    unsigned addrows=sizeof names/sizeof *names;
    Elf64_Phdr storage=im.ph[slot];
    uint64_t append=extend?((im.n+7)&~7ull):((im.n+0xffff)&~0xffffull);
    uint64_t dest=extend?(storage.p_vaddr+append-storage.p_offset):((maxend+0xffff)&~0xffffull);
    size_t n=append+(rows+addrows)*16;
    for(unsigned r=0;r<addrows;r++)n+=strlen(names[r])+strlen(icons[r])+2;
    n=(n+7)&~(size_t)7;n+=sizeof(Footer);
    if(n>32*1024*1024||dest>UINT64_MAX-(n-append))return fail("expanded image bounds");
    unsigned char *bytes=calloc(1,n);if(!bytes)return fail("memory");
    memcpy(bytes,im.b,im.n);free(im.b);im.b=bytes;im.eh=(Elf64_Ehdr*)bytes;im.ph=(Elf64_Phdr*)(bytes+im.eh->e_phoff);
    Footer f={0};memcpy(f.magic,magic,16);f.original_size=im.n;f.original_crc=crc32(bytes,im.n);f.function_offset=found;f.ph_index=slot;f.original_ph=storage;
    memcpy(bytes+append,bytes+toff,rows*16);
    uint64_t strings=append+(rows+addrows)*16;
    for(unsigned r=0;r<addrows;r++) {
        put64(bytes+append+(rows+r)*16,dest+strings-append);strcpy((char*)bytes+strings,names[r]);strings+=strlen(names[r])+1;
        put64(bytes+append+(rows+r)*16+8,dest+strings-append);strcpy((char*)bytes+strings,icons[r]);strings+=strlen(icons[r])+1;
    }
    for(unsigned r=0;r<pat->nrefs;r++)if(set_address(&im,&f,found,pat->refs[r],dest))return fail("address encoding");
    uint64_t c=found+pat->count;uint32_t w=(get32(bytes+c)&0xffc003ffu)|((rows+addrows)<<10);if(record(&im,&f,c,w))return fail("change log");
    if(extend) {
        im.ph[slot].p_filesz=n-storage.p_offset;
        im.ph[slot].p_memsz=n-storage.p_offset;
    } else {
        im.ph[slot]=(Elf64_Phdr){.p_type=PT_LOAD,.p_flags=PF_R,.p_offset=append,.p_vaddr=dest,.p_paddr=dest,.p_filesz=n-append,.p_memsz=n-append,.p_align=0x10000};
    }
    memcpy(bytes+n-sizeof f,&f,sizeof f);f.file_crc=crc32(bytes,n);memcpy(bytes+n-sizeof f,&f,sizeof f);
    int ret=write_new(argv[3],bytes,n);if(!ret)puts("CREATED: current launcher + NXT entries (read-only data segment)");free(bytes);return ret;
}
