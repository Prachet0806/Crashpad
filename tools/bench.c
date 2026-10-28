#define _POSIX_C_SOURCE 200809L
#include "crashpad/store.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifdef _WIN32
#include <windows.h>
static unsigned core_count(void){SYSTEM_INFO i;GetSystemInfo(&i);return (unsigned)i.dwNumberOfProcessors;}
static double monotonic_seconds(void){LARGE_INTEGER now,frequency;QueryPerformanceFrequency(&frequency);QueryPerformanceCounter(&now);return (double)now.QuadPart/(double)frequency.QuadPart;}
#else
#include <unistd.h>
#include <time.h>
static unsigned core_count(void){long n=sysconf(_SC_NPROCESSORS_ONLN);return n>0?(unsigned)n:1u;}
static double monotonic_seconds(void){struct timespec t;if(clock_gettime(CLOCK_MONOTONIC,&t)!=0)return 0.0;return (double)t.tv_sec+(double)t.tv_nsec/1e9;}
#endif

static uint64_t rng(uint64_t *x){*x^=*x<<13;*x^=*x>>7;*x^=*x<<17;return *x;}
static double uniform01(uint64_t *state){return (double)(rng(state)>>11)*(1.0/9007199254740992.0);}
static size_t zipf_rank(double *cdf,size_t n,uint64_t *state){double u=uniform01(state);size_t lo=0,hi=n;while(lo<hi){size_t mid=lo+(hi-lo)/2;if(cdf[mid]<u)lo=mid+1;else hi=mid;}return lo<n?lo:n-1;}
static size_t reference_count(const cp_kv*l,size_t nl,const cp_kv*r,size_t nr){size_t n=0;for(size_t i=0;i<nl;i++)for(size_t j=0;j<nr;j++)if(l[i].key==r[j].key)n++;return n;}
static int compare_double(const void*a,const void*b){double x=*(const double*)a,y=*(const double*)b;return (x>y)-(x<y);}
static int compare_join_row(const void*a,const void*b){const cp_join_row*x=a,*y=b;if(x->key!=y->key)return x->key<y->key?-1:1;if(x->left_value!=y->left_value)return x->left_value<y->left_value?-1:1;if(x->right_value!=y->right_value)return x->right_value<y->right_value?-1:1;return 0;}
static int equal_rows(const cp_join_row*a,const cp_join_row*b,size_t n){for(size_t i=0;i<n;i++)if(a[i].key!=b[i].key||a[i].left_value!=b[i].left_value||a[i].right_value!=b[i].right_value)return 0;return 1;}

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
    enum{RUNS=7};double samples[RUNS];size_t result_count=reference_count(l,rows,r,rows);cp_join_row*reference=result_count&&result_count<=SIZE_MAX/sizeof(cp_join_row)?malloc(result_count*sizeof(cp_join_row)):NULL;if(result_count&&!reference)status=CP_E_NOMEM;size_t ri=0;if(status==CP_OK){for(size_t i=0;i<rows;i++)for(size_t j=0;j<rows;j++)if(l[i].key==r[j].key)reference[ri++]=(cp_join_row){l[i].key,l[i].value,r[j].value};if(ri!=result_count)status=CP_E_CORRUPT;else if(result_count)qsort(reference,result_count,sizeof(*reference),compare_join_row);}
    for(size_t run=0;run<RUNS&&status==CP_OK;run++){cp_join_row*out=NULL;size_t count=0;double before=monotonic_seconds();status=cp_coordinator_join_ex(c,l,rows,r,rows,mitigation,&out,&count);double after=monotonic_seconds();if(status!=CP_OK){free(out);break;}samples[run]=after-before;if(count!=result_count){status=CP_E_CORRUPT;free(out);break;}if(count)qsort(out,count,sizeof(*out),compare_join_row);if(!equal_rows(out,reference,count))status=CP_E_CORRUPT;free(out);}
    if(status==CP_OK){qsort(samples,RUNS,sizeof(*samples),compare_double);printf("rows_per_side=%zu shards=%zu distribution=%s alpha=%.1f mitigation=%s runs=%d result_rows=%zu reference_rows=%zu median_seconds=%.6f available_cores=%u\n",rows,shards,distribution,alpha,mitigation?"on":"off",RUNS,result_count,result_count,samples[RUNS/2],core_count());}
    cp_coordinator_explain_ex(c,l,rows,r,rows,mitigation,stdout);
    cp_coordinator_close(c);free(l);free(r);free(cdf);free(reference);if(status!=CP_OK)fprintf(stderr,"join or reference validation failed (%d)\n",status);return status==CP_OK?0:1;
}
