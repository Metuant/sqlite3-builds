#define _POSIX_C_SOURCE 200809L
#include "sqlite3.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
static double end;static double now(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return t.tv_sec+t.tv_nsec/1e9;}static int stop(void*p){(void)p;return now()>end;}
static void js(const char*s){putchar('"');for(;s&&*s;s++){unsigned char c=*s;if(c=='"'||c=='\\')printf("\\%c",c);else if(c<32)printf("\\u%04x",c);else putchar(c);}putchar('"');}
static char* text(const char*p){FILE*f=fopen(p,"rb");if(!f)exit(90);fseek(f,0,SEEK_END);long n=ftell(f);if(n<0||n>1000000)exit(91);rewind(f);char*s=malloc(n+1);if(!s)exit(92);if(fread(s,1,n,f)!=(size_t)n)exit(93);s[n]=0;fclose(f);return s;}
int main(int ac,char**av){if(ac<5)return 89;sqlite3*db=0;int init=!strcmp(av[1],"init"),rc=sqlite3_open_v2(av[2],&db,init?(SQLITE_OPEN_READWRITE|SQLITE_OPEN_CREATE):SQLITE_OPEN_READONLY,NULL);if(rc)return 1;char*q=text(av[3]);end=now()+10;sqlite3_progress_handler(db,1000,stop,0);
if(init){rc=sqlite3_exec(db,q,0,0,0);printf("{\"rc\":%d,\"error\":",rc);js(sqlite3_errmsg(db));puts("}");free(q);sqlite3_close(db);return rc?1:0;}
sqlite3_stmt*p=0;const char*tail=0;int api=atoi(av[4]);if(api==1)rc=sqlite3_prepare(db,q,-1,&p,&tail);else if(api==2)rc=sqlite3_prepare_v2(db,q,-1,&p,&tail);else rc=sqlite3_prepare_v3(db,q,-1,SQLITE_PREPARE_PERSISTENT,&p,&tail);
printf("{\"version\":");js(sqlite3_libversion());printf(",\"compiled_sql\":");js(p?sqlite3_sql(p):q);printf(",\"params\":[");int n=p?sqlite3_bind_parameter_count(p):0;for(int i=1;i<=n;i++){if(i>1)putchar(',');const char*s=sqlite3_bind_parameter_name(p,i);if(s)js(s);else printf("null");}printf("],\"tail\":");js(tail?tail:"");
printf(",\"columns\":[");for(int i=0;p&&i<sqlite3_column_count(p);i++){if(i)putchar(',');js(sqlite3_column_name(p,i));}putchar(']');
for(int i=5;!rc&&i<ac;i++){int j=i-4;char*s=av[i];if(s[0]=='n')rc=sqlite3_bind_null(p,j);else if(s[0]=='i')rc=sqlite3_bind_int64(p,j,strtoll(s+2,0,10));else rc=sqlite3_bind_text(p,j,s+2,-1,SQLITE_TRANSIENT);}
if(!rc&&!strcmp(av[1],"plan"))rc=sqlite3_stmt_explain(p,2);
printf(",\"rows\":[");int first=1,count=0;double start=now();if(!rc)while((rc=sqlite3_step(p))==SQLITE_ROW){if(!first)putchar(',');first=0;putchar('[');for(int i=0;i<sqlite3_column_count(p);i++){if(i)putchar(',');int t=sqlite3_column_type(p,i);printf("{\"type\":%d,\"value\":",t);if(t==SQLITE_NULL)printf("null");else if(t==SQLITE_INTEGER)printf("%lld",(long long)sqlite3_column_int64(p,i));else if(t==SQLITE_FLOAT)printf("%.17g",sqlite3_column_double(p,i));else if(t==SQLITE_BLOB){putchar('"');const unsigned char*b=sqlite3_column_blob(p,i);for(int k=0;k<sqlite3_column_bytes(p,i);k++)printf("%02x",b[k]);putchar('"');}else js((const char*)sqlite3_column_text(p,i));putchar('}');}putchar(']');if(++count>10000){rc=SQLITE_TOOBIG;break;}}
printf("],\"rc\":%d,\"vmsteps\":%d,\"seconds\":%.6f,\"error\":",rc,p?sqlite3_stmt_status(p,SQLITE_STMTSTATUS_VM_STEP,0):0,now()-start);js(sqlite3_errmsg(db));puts("}");if(p)sqlite3_finalize(p);int closed=sqlite3_close(db);free(q);return rc==SQLITE_DONE&&closed==0?0:1;}
