#ifndef PLEX_RECENT_EPISODE_REWRITE_H
#define PLEX_RECENT_EPISODE_REWRITE_H
#include "plex_title_scope_rewrite.h"
#include <stdio.h>
/* Move each WHERE operand once, in original order, into a single-row CTE.
 * Reference CTE columns rather than introducing numbered references to bare ?.
 * SQLite variable count/names/indices therefore remain unchanged. The CTE has
 * no declared column affinity. Keep LIMIT/OFFSET binds after WHERE binds.
 * No INDEXED BY: schema changes must not invalidate a hard-coded index hint. */
static int plex_recent_operand(fts_lex *lx,fts_lex_token *out,int string_ok){
 fts_lex_token t=fts_lex_next_token(lx);size_t n=t.end-t.start,i;unsigned v=0;
 const char *s=lx->sql+t.start;
 /* The shared scanner splits 0AND into two tokens. Relocating only 0 would
  * repair invalid SQL. Check SQLite identifier bytes at the decimal boundary. */
 if(t.type==FTS_LEX_TOK_NUMBER){
  for(i=0;i<n;i++)if(s[i]<'0'||s[i]>'9')return 0;
  if(t.end<lx->len){unsigned char c=(unsigned char)lx->sql[t.end];
   if(fts_lex_is_ident_char(c)||c=='$'||c>=0x80)return 0;
  }
 }
 if(t.type==FTS_LEX_TOK_PARAM){
  if(!n||s[0]!='?')return 0;
  if(n>1){for(i=1;i<n;i++){if(s[i]<'0'||s[i]>'9'||v>32766u/10u)return 0;v=v*10u+(unsigned)(s[i]-'0');}if(!v||v>32766)return 0;}
 }else if(t.type!=FTS_LEX_TOK_NUMBER&&!(string_ok&&t.type==FTS_LEX_TOK_STRING))return 0;
 if(!n||n>128)return 0;
 *out=t;return 1;
}
static char *plex_build_recent_episode_sql(const char *sql,size_t bounded,size_t scan,size_t *outscan,size_t *outlen){
 static const char prefix[]="WITH recent_parameters(p0,p1,p2,p3,p4,p5) AS MATERIALIZED (SELECT ";
 static const char extra[]=" AND (SELECT p0 FROM recent_parameters) IS NOT NULL AND metadata_items.id IN (SELECT recent_c.parent_id FROM metadata_items recent_e JOIN metadata_items recent_c ON recent_c.id=recent_e.parent_id WHERE (SELECT p0 FROM recent_parameters) IS NOT NULL AND recent_e.added_at>(SELECT p3 FROM recent_parameters))";
 fts_lex lx;fts_lex_token t,values[6];size_t i,at,n,pos=0,last=0;char *r,ref[64];int k;
 if(!sql||!outscan||!outlen||bounded>65536||scan>bounded||bounded-scan>1||(bounded>scan&&sql[scan])||memchr(sql,0,scan))return NULL;
 for(i=1;i<scan;i++)if((sql[i-1]=='-'&&sql[i]=='-')||(sql[i-1]=='/'&&sql[i]=='*'))return NULL;
 fts_lex_init(&lx,sql,scan,FTS_LEX_PARAM_SQLITE_VARIABLE);
#define EXPECT(s) if(!plex_join_expect(&lx,s))return NULL
#define VALUE(i,st) if(!plex_recent_operand(&lx,&values[i],st))return NULL
 EXPECT("SELECT metadata_items.id FROM metadata_items JOIN metadata_items AS children ON children.parent_id=metadata_items.id JOIN metadata_items AS grandchildren ON grandchildren.parent_id=children.id WHERE metadata_items.library_section_id IN (");
 VALUE(0,0);EXPECT(") AND (metadata_items.title NOT LIKE");VALUE(1,1);
 EXPECT("AND metadata_items.id NOT IN (SELECT taggings.metadata_item_id FROM taggings JOIN tags ON tags.id=taggings.tag_id WHERE tags.id=");VALUE(2,0);
 EXPECT(") AND grandchildren.added_at>");VALUE(3,0);
 EXPECT("AND (metadata_items.metadata_type=");VALUE(4,0);EXPECT("OR metadata_items.metadata_type=");VALUE(5,0);EXPECT("))");at=lx.pos;
 EXPECT("ORDER BY metadata_items.title_sort COLLATE icu_root LIMIT");if(!plex_recent_operand(&lx,&t,0))return NULL;
 EXPECT("OFFSET");if(!plex_recent_operand(&lx,&t,0))return NULL;
 t=fts_lex_next_token(&lx);if(t.type!=FTS_LEX_TOK_EOF)return NULL;
#undef EXPECT
#undef VALUE
 /* Inputs capped at64KiB; extra capacity bounds all six scalar replacements. */
 n=bounded+sizeof(prefix)+sizeof(extra)+6*64+16;if(n>65536)return NULL;
 r=malloc(n);if(!r)return NULL;
 memcpy(r+pos,prefix,sizeof(prefix)-1);pos+=sizeof(prefix)-1;
 for(i=0;i<6;i++){if(i)r[pos++]=',';memcpy(r+pos,sql+values[i].start,values[i].end-values[i].start);pos+=values[i].end-values[i].start;}
 memcpy(r+pos,") ",2);pos+=2;
 for(i=0;i<6;i++){
  memcpy(r+pos,sql+last,values[i].start-last);pos+=values[i].start-last;
  k=snprintf(ref,sizeof(ref),"(SELECT p%u FROM recent_parameters)",(unsigned)i);
  memcpy(r+pos,ref,(size_t)k);pos+=(size_t)k;last=values[i].end;
 }
 memcpy(r+pos,sql+last,at-last);pos+=at-last;
 memcpy(r+pos,extra,sizeof(extra)-1);pos+=sizeof(extra)-1;
 memcpy(r+pos,sql+at,bounded-at);pos+=bounded-at;r[pos]=0;
 *outlen=pos;*outscan=pos-(bounded-scan);return r;
}
#endif
