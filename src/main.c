#define _POSIX_C_SOURCE 200809L
#include "crashpad/store.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int emit(uint64_t key,uint64_t value,void*context){(void)context;printf("%llu %llu\n",(unsigned long long)key,(unsigned long long)value);return 1;}
static int parse_u64(const char*text,uint64_t*out){if(!text||!*text||*text=='-')return 0;errno=0;char*end=NULL;unsigned long long value=strtoull(text,&end,0);if(errno||end==text||*end!='\0')return 0;*out=(uint64_t)value;return 1;}
static int read_relation(const char*path,cp_kv**rows,size_t*count){FILE*f=fopen(path,"r");if(!f)return 0;cp_kv*items=NULL;size_t n=0,cap=0;char line[512];int ok=1;while(fgets(line,sizeof line,f)){char*save=NULL;char*a=strtok_r(line," ,\t\r\n",&save),*b=strtok_r(NULL," ,\t\r\n",&save),*extra=strtok_r(NULL," ,\t\r\n",&save);uint64_t key,value;if(!a&&!b)continue;if(!parse_u64(a,&key)||!parse_u64(b,&value)||extra){ok=0;break;}if(n==cap){size_t next=cap?cap*2:64;if(next<cap||next>SIZE_MAX/sizeof(*items)){ok=0;break;}cp_kv*p=realloc(items,next*sizeof(*p));if(!p){ok=0;break;}items=p;cap=next;}items[n++]=(cp_kv){key,value};}if(ferror(f))ok=0;fclose(f);if(!ok){free(items);return 0;}*rows=items;*count=n;return 1;}
static void usage(void){puts("commands: put KEY VALUE | get KEY | scan FIRST LAST | join LEFT_FILE RIGHT_FILE | explain LEFT_FILE RIGHT_FILE | quit");puts("relation files contain one unsigned KEY VALUE pair per line (comma or whitespace separated)");}

int main(int argc,char**argv){
    const char*dir=argc>1?argv[1]:"crashpad-data";cp_coordinator*coordinator=NULL;
    cp_status status=cp_coordinator_open(dir,4,64,&coordinator);
    if(status!=CP_OK){fprintf(stderr,"open failed (%d)\n",status);return 1;}
    char line[512];usage();
    while(fputs("crashpad> ",stdout),fflush(stdout),fgets(line,sizeof line,stdin)){
        char*save=NULL;char*command=strtok_r(line," \t\r\n",&save);if(!command)continue;
        if(!strcmp(command,"quit")||!strcmp(command,"exit"))break;
        if(!strcmp(command,"put")){
            char*a=strtok_r(NULL," \t\r\n",&save),*b=strtok_r(NULL," \t\r\n",&save);uint64_t key,value;
            if(!parse_u64(a,&key)||!parse_u64(b,&value)){puts("usage: put KEY VALUE (unsigned decimal or 0x-prefixed hex)");continue;}
            status=cp_coordinator_put(coordinator,key,value);puts(status==CP_OK?"ok":"write failed");
        }else if(!strcmp(command,"get")){
            char*a=strtok_r(NULL," \t\r\n",&save);uint64_t key,value;
            if(!parse_u64(a,&key)){puts("usage: get KEY");continue;}
            status=cp_coordinator_get(coordinator,key,&value);if(status==CP_OK)printf("%llu\n",(unsigned long long)value);else puts("not found");
        }else if(!strcmp(command,"scan")){
            char*a=strtok_r(NULL," \t\r\n",&save),*b=strtok_r(NULL," \t\r\n",&save);uint64_t first,last;
            if(!parse_u64(a,&first)||!parse_u64(b,&last)){puts("usage: scan FIRST LAST");continue;}
            status=cp_coordinator_scan(coordinator,first,last,emit,NULL);if(status!=CP_OK)puts("scan failed");
        }else if(!strcmp(command,"join")){
            char*a=strtok_r(NULL," \t\r\n",&save),*b=strtok_r(NULL," \t\r\n",&save);if(!a||!b){puts("usage: join LEFT_FILE RIGHT_FILE");continue;}cp_kv*left=NULL,*right=NULL;size_t nl=0,nr=0;if(!read_relation(a,&left,&nl)||!read_relation(b,&right,&nr)){puts("could not read relation files");free(left);free(right);continue;}cp_join_row*joined=NULL;size_t n=0;status=cp_coordinator_join(coordinator,left,nl,right,nr,&joined,&n);if(status==CP_OK)for(size_t i=0;i<n;i++)printf("%llu %llu %llu\n",(unsigned long long)joined[i].key,(unsigned long long)joined[i].left_value,(unsigned long long)joined[i].right_value);else puts("join failed");free(joined);free(left);free(right);
        }else if(!strcmp(command,"explain")){
            char*a=strtok_r(NULL," \t\r\n",&save),*b=strtok_r(NULL," \t\r\n",&save);if(!a||!b){puts("usage: explain LEFT_FILE RIGHT_FILE");continue;}cp_kv*left=NULL,*right=NULL;size_t nl=0,nr=0;if(!read_relation(a,&left,&nl)||!read_relation(b,&right,&nr)){puts("could not read relation files");free(left);free(right);continue;}cp_coordinator_explain(coordinator,left,nl,right,nr,stdout);free(left);free(right);
        }else usage();
    }
    cp_coordinator_close(coordinator);return 0;
}
