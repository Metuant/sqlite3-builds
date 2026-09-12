import os,sys,ast,collections
os.environ['SQLITE3_ENABLE_PLEX_COLLECTION_TAG_REWRITE']='1'
import plex_prepare_test_support as h
from plex_prepare_test_support import *
h.build=signature('test_collection_build',ptr,ptr,C.c_size_t,C.c_size_t,C.POINTER(C.c_size_t),C.POINTER(C.c_size_t))
q='select tags.id,metadata_items.id,metadata_items.absolute_index from tags join taggings on taggings.tag_id=tags.id join metadata_items on metadata_items.id=taggings.metadata_item_id where library_section_id=? and tag_type=?'
a='select tags.id,count(*),min(year),max(year),tags.tag,tags.extra_data from tags join taggings on taggings.tag_id=tags.id join metadata_items on metadata_items.id=taggings.metadata_item_id where library_section_id=? and metadata_items.metadata_type=? and tag_type=2 group by tags.id'
t='select tags.id from tags join taggings on taggings.tag_id=tags.id join metadata_items on metadata_items.id=taggings.metadata_item_id where library_section_id=? and metadata_items.metadata_type=? and tag_type=2 group by tags.id'
with tempfile.TemporaryDirectory() as temp:
 path=Path(temp)/'com.plexapp.plugins.library.db';ref=sqlite3.connect(path)
 ref.executescript('CREATE TABLE tags(id INTEGER PRIMARY KEY,tag_type INTEGER,tag TEXT,extra_data TEXT);CREATE TABLE metadata_items(id INTEGER PRIMARY KEY,library_section_id INTEGER,metadata_type INTEGER,year INTEGER,absolute_index INTEGER);CREATE TABLE taggings(tag_id INTEGER,metadata_item_id INTEGER);CREATE INDEX idx_dshadow_taggings_tag_id_metadata_item_id ON taggings(tag_id,metadata_item_id);INSERT INTO tags VALUES(1,2,"A",NULL),(2,2,"B","X");INSERT INTO metadata_items VALUES(1,5,2,2020,NULL),(2,5,4,NULL,7);INSERT INTO taggings VALUES(1,1),(1,1),(1,2),(2,2),(99,1),(1,99);')
 db=ptr();check(open_db(os.fsencode(path),C.byref(db))==0,'open')
 count=0
 for sql in [q,a,t]:
  for s,params in [(sql,(5,2)),(sql.replace('?', '5',1).replace('?', '2',1),()),(sql.upper(),(5,2)),(sql.replace(' ', '\n'),(5,2)),(sql.replace('?', '$section',1).replace('?', '@kind',1),(5,2)),(sql,(None,2)),(sql,(5,None)),(sql.replace('?',':x',1).replace('?',':y',1),(5,2)),(sql.replace('?', '?2',1).replace('?', '?',1),(None,5,2)),(sql.replace('?',':x'),(2,))]:
   # Numbered ?2 followed by bare ? allocates indices 2,3.
   expected=[(r[0],) for r in ref.execute(s,params)]
   for kind in [0,1,2]:
    for nb in [-1,len(s),len(s)+1]:
     rc,out,rows,names,tail=run(db,s,params,kind,nb,1 if kind==2 else 0)
     check(rc==101 and 'CROSS JOIN' in out and collections.Counter(rows)==collections.Counter(expected),(s,rc,out,rows))
     check(tail==len(s),(tail,len(s)));count+=1
  for fault in [1,2,3]:
   set_fault(fault);rc,out,rows,_,_=run(db,sql,(5,2));set_fault(0)
   check(rc==101 and out==sql and rows==[(r[0],) for r in ref.execute(sql,(5,2))],('fallback',fault,rc,out,rows))
   check(get_skip()=={1:b'rewritten_prepare_failed',2:b'tail_mismatch',3:b'column_count_mismatch'}[fault],('exact_fault_branch',fault,get_skip()))
  check(raw_build(sql) is not None,'build')
  for bad in [sql+';',sql+'; SELECT 2',sql+' OR 1=1',sql.replace('join taggings','left join taggings'),sql.replace('from tags','from tags t'),sql.replace('select','select /* hi */',1),sql.replace('?', '"library_section_id"',1),sql.replace('?', 'NULL',1)]:check(raw_build(bad) is None,('reject',bad))
  check(raw_build(sql.encode()+b'\0junk') is None,'embeddednul')
  set_path(0);check('CROSS JOIN' not in run(db,sql,(5,2))[1],'path');set_path(1)
 for bad in [t.replace('tag_type=2','tag_type=1'),t.replace('tag_type=2','tag_type=?'),t.replace('group by tags.id',''),t+' HAVING count(*)>0',t+' ORDER BY tags.id',t+' LIMIT 1',t.replace('select tags.id','select distinct tags.id'),t.replace('select tags.id','select tags.id AS id'),t.replace('metadata_items.metadata_type=','metadata_type=')]:check(raw_build(bad) is None,('third_shape_reject',bad))
 check(close_db(db)==0,'close');ref.close()
 print(json.dumps({'status':'PASS','prepare_boundary_cases':count,'fault_branches_each_shape':3,'assertions':h.checks}))
