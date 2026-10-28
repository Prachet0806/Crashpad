#include "crashpad/store.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifdef _WIN32
#include <windows.h>
static unsigned core_count(void){SYSTEM_INFO i;GetSystemInfo(&i);return (unsigned)i.dwNumberOfProcessors;}
#else
#include <unistd.h>
static unsigned core_count(void){long n=sysconf(_SC_NPROCESSORS_ONLN);return n>0?(unsigned)n:1u;}
#endif

static uint64_t rng(uint64_t *x){*x^=*x<<13;*x^=*x>>7;*x^=*x<<17;return *x;}
static double uniform01(uint64_t *state){return (double)(rng(state)>>11)*(1.0/9007199254740992.0);}
static size_t zipf_rank(double *cdf,size_t n,uint64_t *state){double u=uniform01(state);size_t lo=0,hi=n;while(lo<hi){size_t mid=lo+(hi-lo)/2;if(cdf[mid]<u)lo=mid+1;else hi=mid;}return lo<n?lo:n-1;}
static size_t reference_count(const cp_kv*l,size_t nl,const cp_kv*r,size_t nr){size_t n=0;for(size_t i=0;i<nl;i++)for(size_t j=0;j<nr;j++)if(l[i].key==r[j].key)n++;return n;}
static double elapsed_seconds(struct timespec a,struct timespec b){return (double)(b.tv_sec-a.tv_sec)+(double)(b.tv_nsec-a.tv_nsec)/1e9;}

int main(int argc,char**argv){
    size_t rows=argc>1?(size_t)strtoull(argv[1],NULL,10):10000;
    size_t shards=argc>2?(size_t)strtoull(argv[2],NULL,10):4;
    const char*distribution=argc>3?argv[3]:"zipf";
    int mitigation=!(argc>4&&strcmp(argv[4],"baseline")==0);
    if(!rows||!shards||(strcmp(distribution,"zipf")&&strcmp(distribution,"uniform"))||rows>SIZE_MAX/sizeof(cp_kv)){fprintf(stderr,"usage: crashpad-bench ROWS SHARDS [zipf|uniform] [mitigated|baseline]\n");return 2;}
    cp_kv*l=malloc(rows*sizeof(*l)),*r=malloc(rows*sizeof(*r));double*cdf=malloc(rows*sizeof(*cdf));
    if(!l||!r||!cdf){free(l);free(r);free(cdf);return 1;}
    double sum=0.0;const double alpha=1.1;for(size_t i=0;i<rows;i++){sum+=1.0/pow((double)(i+1),alpha);cdf[i]=sum;}for(size_t i=0;i<rows;i++)cdf[i]/=sum;
    uint64_t seed=UINT64_C(1234567);for(size_t i=0;i<rows;i++){uint64_t lk,rk;if(!strcmp(distribution,"zipf")){lk=(uint64_t)zipf_rank(cdf,rows,&seed);rk=(uint64_t)zipf_rank(cdf,rows,&seed);}else{lk=rng(&seed)%(uint64_t)rows;rk=rng(&seed)%(uint64_t)rows;}l[i]=(cp_kv){lk,(uint64_t)i};r[i]=(cp_kv){rk,(uint64_t)(i+rows)};}
    cp_coordinator*c=NULL;cp_status status=cp_coordinator_open("bench-data",shards,64,&c);if(status!=CP_OK){fprintf(stderr,"open failed (%d)\n",status);free(l);free(r);free(cdf);return 1;}
    struct timespec before,after;timespec_get(&before,TIME_UTC);size_t result_count=0;
    if(mitigation){cp_join_row*out=NULL;status=cp_coordinator_join_ex(c,l,rows,r,rows,1,&out,&result_count);free(out);}else result_count=reference_count(l,rows,r,rows);
    timespec_get(&after,TIME_UTC);
    printf("rows_per_side=%zu shards=%zu distribution=%s alpha=%.1f mitigation=%s result_rows=%zu wall_seconds=%.6f available_cores=%u\n",rows,shards,distribution,alpha,mitigation?"on":"off",result_count,elapsed_seconds(before,after),core_count());
    cp_coordinator_explain_ex(c,l,rows,r,rows,mitigation,stdout);
    cp_coordinator_close(c);free(l);free(r);free(cdf);return status==CP_OK?0:1;
}
