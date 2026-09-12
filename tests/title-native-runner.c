#define _POSIX_C_SOURCE 200809L
#include "sqlite3.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
static double deadline;
static double now(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return t.tv_sec+t.tv_nsec/1e9;}
static int stop(void*x){(void)x;return now()>deadline;}
static void js(const char*s){putchar('"');for(;s&&*s;s++){unsigned char c=*s;if(c=='"'||c=='\\')printf("\\%c",c);else if(c<32)printf("\\u%04x",c);else putchar(c);}putchar('"');}
static char *readtext(const char*p){FILE*f=fopen(p,"rb");if(!f)exit(90);fseek(f,0,SEEK_END);long n=ftell(f);if(n<0||n>2000000)exit(91);rewind(f);char*s=malloc(n+1);if(!s)exit(92);if(fread(s,1,n,f)!=(size_t)n)exit(93);s[n]=0;fclose(f);return s;}
int main(int ac,char**av){
 if(ac<4)return 89;
 sqlite3*db=0;int rc=sqlite3_open_v2(av[2],&db,!strcmp(av[1],"init")?(SQLITE_OPEN_READWRITE|SQLITE_OPEN_CREATE):SQLITE_OPEN_READONLY,0);
 if(rc){fprintf(stderr,"open: %s\n",sqlite3_errmsg(db));return 1;}
 rc=sqlite3_exec(db,"SELECT icu_load_collation('root','icu_root'); PRAGMA cache_size=-16384; PRAGMA mmap_size=0;",0,0,0);
 if(rc){fprintf(stderr,"collation: %s\n",sqlite3_errmsg(db));return 1;}
 char*q=readtext(av[3]);double start=now();deadline=start+30.;sqlite3_progress_handler(db,1000,stop,0);
 if(!strcmp(av[1],"init")){
   rc=sqlite3_exec(db,q,0,0,0);printf("{\"rc\":%d,\"error\":",rc);js(sqlite3_errmsg(db));puts("}");sqlite3_close(db);free(q);return rc?1:0;
 }
 sqlite3_stmt*p=0;rc=sqlite3_prepare_v2(db,q,-1,&p,0);
 printf("{\"version\":");js(sqlite3_libversion());printf(",\"sql\":");js(p?sqlite3_sql(p):q);
 if(!rc&&ac>4)rc=sqlite3_bind_text(p,1,av[4],-1,SQLITE_TRANSIENT);
 if(!rc&&!strcmp(av[1],"plan"))rc=sqlite3_stmt_explain(p,2);
 printf(",\"rows\":[");int first=1,n=0;
 if(!rc){while((rc=sqlite3_step(p))==SQLITE_ROW){
   if(!first)putchar(',');first=0;putchar('[');
   for(int i=0;i<sqlite3_column_count(p);i++){if(i)putchar(',');if(sqlite3_column_type(p,i)==SQLITE_NULL)printf("null");else js((const char*)sqlite3_column_text(p,i));}
   putchar(']');if(++n>5000){rc=SQLITE_TOOBIG;break;}
 }}
 printf("],\"rc\":%d,\"seconds\":%.6f,\"vmsteps\":%d,\"error\":",rc,now()-start,p?sqlite3_stmt_status(p,SQLITE_STMTSTATUS_VM_STEP,0):0);
 js(sqlite3_errmsg(db));puts("}");if(p)sqlite3_finalize(p);int close_rc=sqlite3_close(db);free(q);return rc==SQLITE_DONE&&close_rc==SQLITE_OK?0:1;
}
