#define _POSIX_C_SOURCE 200809L
#include "crashpad/blockdev.h"
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#ifdef _WIN32
#include <io.h>
#include <windows.h>
#endif

typedef struct { int fd;
#ifdef _WIN32
 CRITICAL_SECTION lock;
#endif
} file_ctx;
static int io_valid(uint64_t o,size_t n){return o%CP_SECTOR_SIZE==0&&n%CP_SECTOR_SIZE==0&&o<=INT64_MAX&&(uint64_t)n<=(uint64_t)INT64_MAX-o;}
static cp_status file_read(cp_blockdev *d,uint64_t o,void *b,size_t n){if(!d||!b||!io_valid(o,n))return CP_E_RANGE;file_ctx*c=d->ctx;size_t k=0;
#ifdef _WIN32
 EnterCriticalSection(&c->lock);if(_lseeki64(c->fd,(__int64)o,SEEK_SET)<0){LeaveCriticalSection(&c->lock);return CP_E_IO;}while(k<n){size_t left=n-k;unsigned chunk=left>INT_MAX?(unsigned)INT_MAX:(unsigned)left;int r=_read(c->fd,(char*)b+k,chunk);if(r<=0){LeaveCriticalSection(&c->lock);return CP_E_IO;}k+=(size_t)r;}LeaveCriticalSection(&c->lock);
#else
 while(k<n){ssize_t r=pread(c->fd,(char*)b+k,n-k,(off_t)(o+k));if(r<0&&errno==EINTR)continue;if(r<=0)return CP_E_IO;k+=(size_t)r;}
#endif
 return CP_OK;}
static cp_status file_write(cp_blockdev*d,uint64_t o,const void*b,size_t n){if(!d||!b||!io_valid(o,n))return CP_E_RANGE;file_ctx*c=d->ctx;size_t k=0;
#ifdef _WIN32
 EnterCriticalSection(&c->lock);if(_lseeki64(c->fd,(__int64)o,SEEK_SET)<0){LeaveCriticalSection(&c->lock);return CP_E_IO;}while(k<n){size_t left=n-k;unsigned chunk=left>INT_MAX?(unsigned)INT_MAX:(unsigned)left;int r=_write(c->fd,(const char*)b+k,chunk);if(r<=0){LeaveCriticalSection(&c->lock);return CP_E_IO;}k+=(size_t)r;}LeaveCriticalSection(&c->lock);
#else
 while(k<n){ssize_t r=pwrite(c->fd,(const char*)b+k,n-k,(off_t)(o+k));if(r<0&&errno==EINTR)continue;if(r<=0)return CP_E_IO;k+=(size_t)r;}
#endif
 return CP_OK;}
static cp_status file_sync(cp_blockdev*d){
#ifdef _WIN32
 file_ctx*c=d->ctx;EnterCriticalSection(&c->lock);int r=_commit(c->fd);LeaveCriticalSection(&c->lock);return r==0?CP_OK:CP_E_IO;
#else
 return fsync(((file_ctx*)d->ctx)->fd)==0?CP_OK:CP_E_IO;
#endif
}
static cp_status file_size(cp_blockdev*d,uint64_t*out){
#ifdef _WIN32
 struct _stat64 s;if(_fstat64(((file_ctx*)d->ctx)->fd,&s))return CP_E_IO;*out=(uint64_t)s.st_size;
#else
 struct stat s;if(fstat(((file_ctx*)d->ctx)->fd,&s))return CP_E_IO;*out=(uint64_t)s.st_size;
#endif
 return CP_OK;}
static void file_close(cp_blockdev*d){if(d){file_ctx*c=d->ctx;if(c){
#ifdef _WIN32
 DeleteCriticalSection(&c->lock);_close(c->fd);
#else
 close(c->fd);
#endif
 free(c);}free(d);}}
static const cp_blockdev_ops file_ops={file_read,file_write,file_sync,file_size,file_close};
cp_status cp_file_dev_open(const char*path,int create,cp_blockdev**out){if(!path||!out)return CP_E_RANGE;int flags=O_RDWR|(create?O_CREAT|O_TRUNC:0);
#ifdef _WIN32
 flags|=O_BINARY;
#endif
 int fd=open(path,flags,0644);if(fd<0)return CP_E_IO;
#ifdef _WIN32
 struct _stat64 st;if(_fstat64(fd,&st)){_close(fd);return CP_E_IO;}
#else
 struct stat st;if(fstat(fd,&st)){close(fd);return CP_E_IO;}
#endif
 cp_blockdev*d=calloc(1,sizeof(*d));file_ctx*c=calloc(1,sizeof(*c));if(!d||!c){free(d);free(c);
#ifdef _WIN32
 _close(fd);
#else
 close(fd);
#endif
 return CP_E_NOMEM;}c->fd=fd;
#ifdef _WIN32
 InitializeCriticalSection(&c->lock);
#endif
 d->ops=&file_ops;d->ctx=c;*out=d;return CP_OK;}

typedef struct { cp_trace_op op; unsigned char *data; } sim_op;
struct cp_sim_dev { unsigned char *disk,*durable;uint64_t bytes;sim_op *ops;size_t n,cap,synced_ops;int crashed; };
void cp_sim_dev_destroy(cp_sim_dev*s);
typedef struct {cp_sim_dev*s;int owns;} sim_ctx;
static cp_status sim_read(cp_blockdev*d,uint64_t o,void*b,size_t n){if(!d||!b||!io_valid(o,n))return CP_E_RANGE;sim_ctx*c=d->ctx;if(c->s->crashed)return CP_E_CRASHED;if((uint64_t)n>c->s->bytes-o)return CP_E_RANGE;memcpy(b,c->s->disk+o,n);return CP_OK;}
static cp_status sim_add(cp_sim_dev*s,cp_trace_op op,const void*data){if(s->n==s->cap){size_t z=s->cap?s->cap*2:32;sim_op*p=realloc(s->ops,z*sizeof(*p));if(!p)return CP_E_NOMEM;s->ops=p;s->cap=z;}sim_op*x=&s->ops[s->n];x->op=op;x->data=NULL;if(data&&op.len){x->data=malloc(op.len);if(!x->data)return CP_E_NOMEM;memcpy(x->data,data,op.len);}s->n++;return CP_OK;}
static cp_status sim_write(cp_blockdev*d,uint64_t o,const void*b,size_t n){if(!d||!b||!io_valid(o,n)||n>UINT32_MAX)return CP_E_RANGE;sim_ctx*c=d->ctx;cp_sim_dev*s=c->s;if(s->crashed)return CP_E_CRASHED;if(o>s->bytes||(uint64_t)n>s->bytes-o)return CP_E_RANGE;cp_trace_op op={CP_OP_WRITE,o,(uint32_t)n,(uint64_t)s->n};cp_status r=sim_add(s,op,b);if(r==CP_OK)memcpy(s->disk+o,b,n);return r;}
static cp_status sim_sync(cp_blockdev*d){sim_ctx*c=d->ctx;if(c->s->crashed)return CP_E_CRASHED;cp_sim_dev*s=c->s;if(!s->durable){s->durable=calloc(1,(size_t)s->bytes);if(!s->durable)return CP_E_NOMEM;memcpy(s->durable,s->disk,(size_t)s->bytes);}size_t end=s->n;cp_status r=sim_add(s,(cp_trace_op){CP_OP_SYNC,0,0,0},NULL);if(r!=CP_OK)return r;for(size_t i=s->synced_ops;i<end;i++){sim_op*x=&s->ops[i];if(x->op.kind==CP_OP_WRITE)memcpy(s->durable+x->op.off,x->data,x->op.len);}s->synced_ops=s->n;return CP_OK;}
static cp_status sim_size(cp_blockdev*d,uint64_t*out){*out=((sim_ctx*)d->ctx)->s->bytes;return CP_OK;}
static void sim_dev_close(cp_blockdev*d){if(d){sim_ctx*c=d->ctx;if(c&&c->owns)cp_sim_dev_destroy(c->s);free(c);free(d);}}
static const cp_blockdev_ops sim_ops={sim_read,sim_write,sim_sync,sim_size,sim_dev_close};
cp_status cp_sim_dev_create(uint64_t bytes,cp_sim_dev**out_s,cp_blockdev**out_d){if(!out_s||!out_d||!bytes||bytes>SIZE_MAX||bytes%CP_SECTOR_SIZE)return CP_E_RANGE;cp_sim_dev*s=calloc(1,sizeof(*s));cp_blockdev*d=calloc(1,sizeof(*d));sim_ctx*c=calloc(1,sizeof(*c));if(!s||!d||!c){free(s);free(d);free(c);return CP_E_NOMEM;}s->disk=calloc(1,(size_t)bytes);s->durable=calloc(1,(size_t)bytes);if(!s->disk||!s->durable){free(s->disk);free(s->durable);free(s);free(d);free(c);return CP_E_NOMEM;}s->bytes=bytes;c->s=s;d->ops=&sim_ops;d->ctx=c;*out_s=s;*out_d=d;return CP_OK;}
void cp_sim_dev_destroy(cp_sim_dev*s){if(s){for(size_t i=0;i<s->n;i++)free(s->ops[i].data);free(s->ops);free(s->disk);free(s->durable);free(s);}}
size_t cp_sim_trace_len(const cp_sim_dev*s){return s?s->n:0;}
const cp_trace_op*cp_sim_trace_at(const cp_sim_dev*s,size_t i){return s&&i<s->n?&s->ops[i].op:NULL;}
static uint64_t rng_next(uint64_t*x){*x^=*x<<13;*x^=*x>>7;*x^=*x<<17;return *x;}
cp_status cp_sim_crash_image(const cp_sim_dev*s,const cp_crash_spec*sp,cp_blockdev**out){if(!s||!sp||!out||sp->crash_point>s->n||sp->crash_point>=SIZE_MAX)return CP_E_RANGE;cp_sim_dev*img=calloc(1,sizeof(*img));cp_blockdev*d=calloc(1,sizeof(*d));sim_ctx*ctx=calloc(1,sizeof(*ctx));if(!img||!d||!ctx){free(img);free(d);free(ctx);return CP_E_NOMEM;}img->disk=calloc(1,(size_t)s->bytes);if(!img->disk){free(img);free(d);free(ctx);return CP_E_NOMEM;}img->bytes=s->bytes;ctx->s=img;ctx->owns=1;d->ops=&sim_ops;d->ctx=ctx;size_t *pending=calloc((size_t)sp->crash_point+1,sizeof(size_t));if(!pending){cp_dev_close(d);return CP_E_NOMEM;}size_t np=0;for(size_t i=0;i<(size_t)sp->crash_point;i++){const sim_op*x=&s->ops[i];if(x->op.kind==CP_OP_WRITE)pending[np++]=i;else if(x->op.kind==CP_OP_SYNC){for(size_t j=0;j<np;j++){const sim_op*w=&s->ops[pending[j]];memcpy(img->disk+w->op.off,w->data,w->op.len);}np=0;}}uint64_t seed=sp->seed?sp->seed:1;size_t *chosen=calloc(np?np:1,sizeof(size_t));if(!chosen){free(pending);cp_dev_close(d);return CP_E_NOMEM;}size_t nc=0,last=(size_t)-1;for(size_t j=0;j<np;j++){int keep=sp->policy==CP_UNSYNCED_KEEP_ALL||(sp->policy==CP_UNSYNCED_SUBSET&&(rng_next(&seed)&1));if(keep){chosen[nc++]=pending[j];last=pending[j];}}size_t window_start=sp->reorder_window&&nc>sp->reorder_window?nc-sp->reorder_window:0;for(size_t pass=0;pass<2;pass++){size_t begin=pass==0?0:window_start,end=pass==0?window_start:nc;for(size_t step=0;step<end-begin;step++){size_t j=pass==0?begin+step:end-1-step;size_t oi=chosen[j];const sim_op*w=&s->ops[oi];size_t n=w->op.len;if(sp->tear_last_write&&oi==last&&n>CP_SECTOR_SIZE){uint32_t sectors=w->op.len/CP_SECTOR_SIZE;n=(size_t)(1+rng_next(&seed)%(sectors-1))*CP_SECTOR_SIZE;}memcpy(img->disk+w->op.off,w->data,n);}}free(chosen);free(pending);*out=d;return CP_OK;}
