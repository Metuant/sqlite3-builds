import os,sys,random
from collections import Counter
os.environ['SQLITE3_ENABLE_PLEX_RECENT_EPISODE_REWRITE']='1'
import plex_prepare_test_support as h
from plex_prepare_test_support import *
h.build=signature('test_recent_build',ptr,ptr,C.c_size_t,C.c_size_t,C.POINTER(C.c_size_t),C.POINTER(C.c_size_t))
q='select metadata_items.id from metadata_items join metadata_items as children on children.parent_id=metadata_items.id join metadata_items as grandchildren on grandchildren.parent_id=children.id where metadata_items.library_section_id in (?1) and (metadata_items.title NOT LIKE ?2 and metadata_items.id not in (select taggings.metadata_item_id from taggings join tags on tags.id=taggings.tag_id where tags.id=?3) and grandchildren.added_at>?4 and (metadata_items.metadata_type=?5 or metadata_items.metadata_type=?6)) order by metadata_items.title_sort collate icu_root limit ?7 offset ?8'
CMP=C.CFUNCTYPE(C.c_int,ptr,C.c_int,ptr,C.c_int,ptr)
@CMP
def cmp(ctx,na,a,nb,b):
 a=C.string_at(a,na);b=C.string_at(b,nb);return (a>b)-(a<b)
coll=signature('sqlite3_create_collation',C.c_int,ptr,C.c_char_p,C.c_int,ptr,CMP)
count=0;meta=[]
with tempfile.TemporaryDirectory() as tmp:
 for seed in range(40):
  rng=random.Random(seed);path=Path(tmp)/'com.plexapp.plugins.library.db'
  if path.exists():path.unlink()
  d=sqlite3.connect(path);d.executescript('CREATE TABLE metadata_items(id INTEGER PRIMARY KEY,parent_id,library_section_id,metadata_type,added_at,title,title_sort);CREATE TABLE tags(id INTEGER PRIMARY KEY);CREATE TABLE taggings(metadata_item_id,tag_id);CREATE INDEX index_metadata_items_on_added_at ON metadata_items(added_at);CREATE INDEX parent_idx ON metadata_items(parent_id);')
  if seed%2:d.execute('CREATE INDEX show_idx ON metadata_items(library_section_id,metadata_type,title_sort DESC)')
  for i in range(1,31):
   d.execute('INSERT INTO metadata_items VALUES(?,NULL,5,2,0,?,?)',(i,'Exclude' if i%9==0 else 'Show',rng.choice(['same','same','different',None])))
   for j in range(2):
    c=100+i*2+j;d.execute('INSERT INTO metadata_items VALUES(?,?,5,3,0,NULL,NULL)',(c,i))
    for k in range(rng.randrange(5)):d.execute('INSERT INTO metadata_items VALUES(?,?,5,4,?,NULL,NULL)',(1000+i*20+j*5+k,c,rng.randrange(100)))
  d.execute('INSERT INTO tags VALUES(10)');d.execute('INSERT INTO taggings VALUES(4,10)');d.commit();d.close()
  db=ptr();check(open_db(os.fsencode(path),C.byref(db))==0,'open');check(coll(db,b'icu_root',1,None,cmp)==0,'collation')
  for cutoff,lim in [(0,5000),(50,7),(99,3),(0,0)]:
   # Ties may select different IDs on an individual page. Compare every page's
   # complete typed rows as one multiset; any missing/extra row fails the suite.
   bags=[Counter(),Counter()];off=0
   while True:
    params=(5,'Exclude',10,cutoff,2,10,lim,off)
    set_path(0);base=run(db,q,params,typed=True);set_path(1);new=run(db,q,params,typed=True)
    check(base[0]==101 and new[0]==101,'execute');check('WITH recent_parameters' in new[1],'rewrite hit');count+=1
    check(base[3]==new[3],'bind names')
    bags[0].update(base[2]);bags[1].update(new[2])
    if not base[2] and not new[2]:break
    off+=lim
    check(off<10000,'pagination terminates')
   check(bags[0]==bags[1],('full typed bag differs',seed,cutoff,lim,bags))
  if seed==0:
   bare=__import__('re').sub(r'\?\d+','?',q);params=(5,'Exclude',10,50,2,10,5000,0)
   set_path(0);base=run(db,bare,params);set_path(1);new=run(db,bare,params);meta.append(dict(original=base[3],candidate=new[3]));check(base[3]==new[3] and base[2]==new[2] and new[1].startswith('WITH recent_parameters'),'bare metadata preserved')
   for bad in [q+';',q+'; SELECT 1',q.replace('select metadata_items.id','select distinct metadata_items.id'),q.replace('join metadata_items as children','left join metadata_items as children'),q.replace('?1',':section'),q.replace('?4',':date'),q.replace('>?4','>=?4'),q.replace('?7','-1'),q.replace('order by','/* x */ order by'),q.replace('select metadata_items.id','select count(*)')]:check(raw_build(bad) is None,('reject',bad))
   check(raw_build(q.encode()+b'\0junk') is None,'embedded nul')
   for kind in [0,1,2]:
    for nb in [-1,len(q),len(q)+1]:
     r=run(db,q,params,kind,nb,1 if kind==2 else 0);check(r[0]==101 and r[4]==len(q),'boundary')
   for fault in [1,2,3]:
    set_fault(fault);r=run(db,q,params);set_fault(0);check(r[1]==q,'fallback');check(get_skip()=={1:b'rewritten_prepare_failed',2:b'tail_mismatch',3:b'column_count_mismatch'}[fault],'fault reason')
   drop=sqlite3.connect(path);drop.execute('DROP INDEX index_metadata_items_on_added_at');drop.commit();drop.close()
   set_path(0);postbase=run(db,q,params);set_path(1)
   r=run(db,q,params);check(r[0]==101 and r[1].startswith('WITH recent_parameters'),'concurrent drop survives');check(r[2]==postbase[2],'post-drop results')
  check(close_db(db)==0,'close')
print(json.dumps(dict(status='PASS',page_comparisons=count,tied_pages='full typed bag asserted, page identity unspecified',bare_bind_metadata=meta,assertions=h.checks),default=lambda x:x.decode()))
