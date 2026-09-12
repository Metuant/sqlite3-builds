#ifndef PLEX_COLLECTION_TAG_REWRITE_H
#define PLEX_COLLECTION_TAG_REWRITE_H
#include "plex_title_scope_rewrite.h"
/* Exact inner-join shapes only. Change loop order, not predicates/projection.
 * FROM contains no binds; preserve all original scalar bytes and bind ordering.
 * NULL constants are conservatively rejected (bound NULL retains SQL semantics).
 */
static char *plex_build_collection_tag_sql(const char *sql,size_t bounded_len,
 size_t scan_len,size_t *out_scan,size_t *out_len,int *columns) {
 static const char from[]=" FROM tags CROSS JOIN taggings ON taggings.tag_id=tags.id CROSS JOIN metadata_items ON metadata_items.id=taggings.metadata_item_id WHERE";
 fts_lex lx,look;fts_lex_token t;size_t a,b,n,i;int shape=0;char *r;
 if(!sql||!out_scan||!out_len||!columns||bounded_len>64u*1024u||scan_len>bounded_len||bounded_len-scan_len>1||(bounded_len>scan_len&&sql[scan_len]!=0)||memchr(sql,0,scan_len))return NULL;
 /* Reject comments, including conservatively inside string scalars. */
 for(i=1;i<scan_len;i++)if((sql[i-1]=='-'&&sql[i]=='-')||(sql[i-1]=='/'&&sql[i]=='*'))return NULL;
 fts_lex_init(&lx,sql,scan_len,FTS_LEX_PARAM_SQLITE_VARIABLE);look=lx;
 if(plex_join_expect(&look,"SELECT tags.id,metadata_items.id,metadata_items.absolute_index"))lx=look;
 else {
  look=lx;
  if(plex_join_expect(&look,"SELECT tags.id,count(*),min(year),max(year),tags.tag,tags.extra_data")){lx=look;shape=1;}
  else if(plex_join_expect(&lx,"SELECT tags.id"))shape=2;
  else return NULL;
 }
 a=lx.pos;
 if(!plex_join_expect(&lx,"FROM tags JOIN taggings ON taggings.tag_id=tags.id JOIN metadata_items ON metadata_items.id=taggings.metadata_item_id WHERE"))return NULL;
 b=lx.pos;
 if(!plex_join_expect(&lx,"library_section_id=")||!plex_title_scope_scalar(&lx,0)||!plex_join_expect(&lx,"AND"))return NULL;
 if(shape && (!plex_join_expect(&lx,"metadata_items.metadata_type=")||!plex_title_scope_scalar(&lx,0)||!plex_join_expect(&lx,"AND")))return NULL;
 if(!plex_join_expect(&lx,"tag_type="))return NULL;
 if(shape){t=fts_lex_next_token(&lx);if(t.type!=FTS_LEX_TOK_NUMBER||t.end-t.start!=1||sql[t.start]!='2'||!plex_join_expect(&lx,"GROUP BY tags.id"))return NULL;}
 else if(!plex_title_scope_scalar(&lx,0))return NULL;
 t=fts_lex_next_token(&lx);if(t.type!=FTS_LEX_TOK_EOF)return NULL;
 n=a+sizeof(from)-1+bounded_len-b;if(n>64u*1024u)return NULL;
 r=malloc(n+1);if(!r)return NULL;
 memcpy(r,sql,a);memcpy(r+a,from,sizeof(from)-1);memcpy(r+a+sizeof(from)-1,sql+b,bounded_len-b);r[n]=0;
 *out_len=n;*out_scan=a+sizeof(from)-1+scan_len-b;*columns=shape==1?6:(shape==2?1:3);return r;
}
#endif
