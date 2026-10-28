#define _POSIX_C_SOURCE 200809L
#include "crashpad/store.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
static double monotonic_seconds(void){LARGE_INTEGER n,f;QueryPerformanceFrequency(&f);QueryPerformanceCounter(&n);return (double)n.QuadPart/(double)f.QuadPart;}
#else
#include <time.h>
static double monotonic_seconds(void){struct timespec t;if(clock_gettime(CLOCK_MONOTONIC,&t)!=0)return 0.0;return (double)t.tv_sec+(double)t.tv_nsec/1e9;}
#endif
typedef struct { uint64_t bytes; } memdev_ctx;
static cp_status mem_read(cp_blockdev*d,uint64_t off,void*buf,size_t len){memdev_ctx*c=d->ctx;if(off>c->bytes||len>c->bytes-off)return CP_E_RANGE;memset(buf,0,len);return CP_OK;}
static cp_status mem_write(cp_blockdev*d,uint64_t off,const void*buf,size_t len){(void)buf;memdev_ctx*c=d->ctx;return off<=c->bytes&&len<=c->bytes-off?CP_OK:CP_E_RANGE;}
static cp_status mem_sync(cp_blockdev*d){(void)d;return CP_OK;}
static cp_status mem_size(cp_blockdev*d,uint64_t*out){*out=((memdev_ctx*)d->ctx)->bytes;return CP_OK;}
static void mem_close(cp_blockdev*d){if(d){free(d->ctx);free(d);}}
static const cp_blockdev_ops mem_ops={mem_read,mem_write,mem_sync,mem_size,mem_close};
static int compare_double(const void*a,const void*b){double x=*(const double*)a,y=*(const double*)b;return (x>y)-(x<y);}
int main(int argc,char**argv){size_t max=argc>1?(size_t)strtoull(argv[1],NULL,10):2048;if(!max||max>4096){fprintf(stderr,"usage: crashpad-store-bench [max_puts:1..4096]\n");return 2;}const size_t counts[]={128,256,512,1024,2048,4096};enum{RUNS=3};for(size_t ci=0;ci<sizeof counts/sizeof counts[0];ci++){size_t n=counts[ci];if(n>max)continue;double times[RUNS];for(size_t run=0;run<RUNS;run++){cp_blockdev*d=calloc(1,sizeof(*d));memdev_ctx*ctx=calloc(1,sizeof(*ctx));if(!d||!ctx){free(d);free(ctx);return 1;}ctx->bytes=16u*1024u*1024u;d->ctx=ctx;d->ops=&mem_ops;cp_store*s=NULL;cp_status r=cp_store_open(d,8,&s);if(r!=CP_OK){cp_dev_close(d);return 1;}double before=monotonic_seconds();for(size_t i=0;i<n;i++){r=cp_store_put(s,(uint64_t)i,(uint64_t)(i*3+1));if(r!=CP_OK)break;}times[run]=monotonic_seconds()-before;cp_store_close(s);cp_dev_close(d);if(r!=CP_OK)return 1;}qsort(times,RUNS,sizeof(*times),compare_double);printf("puts=%zu runs=%d median_seconds=%.6f puts_per_second=%.0f\n",n,RUNS,times[RUNS/2],(double)n/times[RUNS/2]);}return 0;}
