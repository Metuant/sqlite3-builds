import os,json,pathlib as P,subprocess as S,collections
root=P.Path('/validation');root.mkdir(exist_ok=True);db=root/'com.plexapp.plugins.library.db';runner=root/'runner';assert not db.exists(),'fresh isolated fixture required'
base=dict(os.environ,SQLITE3_DISABLE_RUNTIME_OPTIMIZE='1',SQLITE3_DISABLE_AUTOPRAGMA='1',SQLITE3_DISABLE_OBSERVABILITY='1',SQLITE3_DISABLE_PLEX_FTS_REWRITE='0',SQLITE3_ENABLE_PLEX_FTS_JOIN_REWRITE='1',SQLITE3_ENABLE_PLEX_TITLE_SCOPE_REWRITE='1')
lookup='select tags.id,metadata_items.id,metadata_items.absolute_index from tags join taggings on taggings.tag_id=tags.id join metadata_items on metadata_items.id=taggings.metadata_item_id where library_section_id=? and tag_type=?'
aggregate='select tags.id,count(*),min(year),max(year),tags.tag,tags.extra_data from tags join taggings on taggings.tag_id=tags.id join metadata_items on metadata_items.id=taggings.metadata_item_id where library_section_id=? and metadata_items.metadata_type=? and tag_type=2 group by tags.id'
tagids='select tags.id from tags join taggings on taggings.tag_id=tags.id join metadata_items on metadata_items.id=taggings.metadata_item_id where library_section_id=? and metadata_items.metadata_type=? and tag_type=2 group by tags.id'
seed="""CREATE TABLE library_sections(id INTEGER PRIMARY KEY,name TEXT,section_type INTEGER); INSERT INTO library_sections VALUES(5,'TV',2),(1,'Movies',1);
CREATE TABLE metadata_items(id INTEGER PRIMARY KEY,library_section_id INTEGER,metadata_type INTEGER,year INTEGER,absolute_index INTEGER);
CREATE TABLE tags(id INTEGER PRIMARY KEY,tag_type INTEGER,tag TEXT,extra_data BLOB);
CREATE TABLE taggings(id INTEGER PRIMARY KEY,tag_id INTEGER,metadata_item_id INTEGER);
CREATE INDEX idx_dshadow_taggings_tag_id_metadata_item_id ON taggings(tag_id,metadata_item_id);
CREATE INDEX index_taggings_on_metadata_item_id ON taggings(metadata_item_id);
CREATE INDEX index_tags_on_tag_type ON tags(tag_type);
INSERT INTO tags VALUES(101,2,'Collection A',NULL),(102,2,'Collection B',X'00ff'),(103,1,'Genre','text'),(104,NULL,'Untyped',NULL),(105,2,'Empty',NULL);
INSERT INTO metadata_items VALUES(1,5,2,2020,NULL),(2,5,2,2022,10),(3,5,3,NULL,11),(4,5,4,2021,12),(5,1,1,1999,1),(6,9,2,2001,3),(7,NULL,2,2003,4),(8,5,NULL,2004,5),(9,5,4,NULL,NULL);
INSERT INTO taggings(tag_id,metadata_item_id) VALUES(101,1),(101,1),(101,2),(101,3),(101,4),(101,5),(101,6),(101,7),(101,8),(101,9),(102,2),(102,4),(102,5),(103,1),(104,1),(9999,1),(101,9999),(NULL,1),(101,NULL);"""
def run(q,args=(),enabled=False,api=2,mode='query',unset=False):
 p=root/'current.sql';p.write_text(q);e=dict(base)
 if unset:e.pop('SQLITE3_ENABLE_PLEX_COLLECTION_TAG_REWRITE',None)
 else:e['SQLITE3_ENABLE_PLEX_COLLECTION_TAG_REWRITE']='1' if enabled else '0'
 r=S.run([str(runner),mode,str(db),str(p),str(api),*args],env=e,capture_output=True,text=True,timeout=15)
 try:v=json.loads(r.stdout)
 except Exception:raise RuntimeError((r.returncode,r.stdout,r.stderr))
 v['exit']=r.returncode
 return v
assert run(seed,mode='init')['rc']==0
records=[]
def eq(a,b):
 assert a['rc']==b['rc']==101,(a,b)
 assert a['params']==b['params'] and a['tail']==b['tail'] and a['columns']==b['columns'],(a,b)
 assert collections.Counter(map(lambda r:json.dumps(r,sort_keys=True),a['rows']))==collections.Counter(map(lambda r:json.dumps(r,sort_keys=True),b['rows'])),(a,b)
for family,q in [('lookup',lookup),('aggregate',aggregate),('tagids',tagids)]:
 for syntax in ['bare','numbered','named']:
  if syntax=='numbered':sql=q.replace('?', '?1',1).replace('?', '?',1);sql=q.split('?')[0]+'?1'+q.split('?')[1]+'?2'+q.split('?')[2]
  elif syntax=='named':sql=q.split('?')[0]+':section'+q.split('?')[1]+':kind'+q.split('?')[2]
  else:sql=q
  for args in [('i:5','i:2'),('i:5','i:3'),('i:5','i:4'),('i:1','i:1'),('n:','i:2'),('i:5','n:'),('t:5','t:2')]:
   for api in [1,2,3]:
    a=run(sql,args,False,api);b=run(sql,args,True,api);eq(a,b)
    if api!=1:assert 'CROSS JOIN' in b['compiled_sql'],(family,syntax,b)
    records.append({'family':family,'syntax':syntax,'args':args,'api':api,'baseline':a,'candidate':b})
# Positive third-shape oracle, independent of baseline/candidate equivalence.
c=run(tagids,('i:5','i:2'),True);assert sorted(row[0]['value'] for row in c['rows'])==[101,102] and c['columns']==['id'],c
plan=run(tagids,('i:5','i:2'),True,mode='plan');assert plan['rc']==101,plan
pd=[row[3]['value'] for row in plan['rows']];assert 'tags' in pd[0] and 'idx_dshadow_taggings_tag_id_metadata_item_id' in pd[1] and 'metadata_items' in pd[2],pd
for sql,args in [(tagids,('i:5','i:2')),(tagids.replace('tag_type=2','tag_type=1'),('i:5','i:2')),(tagids+' HAVING count(*)>0',('i:5','i:2')),(tagids+' ORDER BY tags.id',('i:5','i:2')),(tagids+'; SELECT 123',('i:5','i:2'))]:
 a=run(sql,args,False);b=run(sql,args,True if sql!=tagids else False,unset=sql==tagids);eq(a,b);assert 'CROSS JOIN' not in b['compiled_sql'];records.append({'control':sql,'baseline':a,'candidate':b})
# Verify default-off, unsupported extra predicate and multi-statement fallback.
for q,args in [(lookup,('i:5','i:2')),(lookup+' AND metadata_items.id>1',('i:5','i:2')),(lookup+'; SELECT 123',('i:5','i:2'))]:
 a=run(q,args,False);b=run(q,args,True if q!=lookup else False,unset=q==lookup);eq(a,b);assert 'CROSS JOIN' not in b['compiled_sql'];records.append({'control':q,'baseline':a,'candidate':b})
# Drop the required index only in the disposable fixture; a fresh connection must fallback safely.
assert run('DROP INDEX idx_dshadow_taggings_tag_id_metadata_item_id;',mode='init')['rc']==0
for sql in [lookup,aggregate,tagids]:
 a=run(sql,('i:5','i:2'),False);b=run(sql,('i:5','i:2'),True);eq(a,b);assert 'CROSS JOIN' not in b['compiled_sql'];records.append({'control':'missing_index','baseline':a,'candidate':b})
(root/'results.json').write_text(json.dumps(records,indent=2));print(json.dumps({'status':'PASS','native_version':records[0]['candidate']['version'],'paired_cases':len(records),'nonempty_pairs':sum(bool(r['candidate']['rows']) for r in records),'all_typed_multisets_equal':True,'parameter_metadata_and_tails_equal':True,'default_off_and_missing_index_fallback':True}))
