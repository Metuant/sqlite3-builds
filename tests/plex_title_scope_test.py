#!/usr/bin/env python3
"""Real C prepare-hook tests on scratch SQLite/unicode61 (not native Plex ICU)."""
import os,sys
if len(sys.argv)==2:os.environ['SQLITE3_ENABLE_PLEX_TITLE_SCOPE_REWRITE']='1'
import plex_prepare_test_support as h
from plex_prepare_test_support import *
h.build=signature('test_title_build',ptr,ptr,C.c_size_t,C.c_size_t,C.POINTER(C.c_size_t),C.POINTER(C.c_size_t))
COLL=C.CFUNCTYPE(C.c_int,ptr,C.c_int,ptr,C.c_int,ptr)
@COLL
def compare(_,na,a,nb,b):
    x=C.string_at(a,na).decode().casefold();y=C.string_at(b,nb).decode().casefold()
    return (x>y)-(x<y)
register=signature('sqlite3_create_collation',C.c_int,ptr,C.c_char_p,C.c_int,ptr,COLL)
PARENTS=' join metadata_items as parents on parents.id=metadata_items.parent_id join metadata_items as grandparents on grandparents.id=parents.parent_id'
MOVIE='metadata_items.title_sort collate icu_root asc,metadata_items.originally_available_at asc,metadata_items.id asc'
TV='grandparents.title_sort collate icu_root asc,parents.`index` IS NULL,parents.`index` asc,metadata_items.`index` IS NULL,metadata_items.`index` asc,metadata_items.originally_available_at IS NULL,metadata_items.originally_available_at asc,metadata_items.title_sort collate icu_root asc,metadata_items.id asc'
def q(tv=False,col='title',term="'Th*'",typ=None,scope='11',limit='101',offset='0'):
    if typ is None:typ='4' if tv else '1'
    return ('select metadata_items.id from metadata_items'+(PARENTS if tv else '')+' join fts4_metadata_titles_icu on metadata_items.id=fts4_metadata_titles_icu.rowid left join taggings on taggings.metadata_item_id=metadata_items.id left join tags on taggings.tag_id=tags.id where fts4_metadata_titles_icu.'+col+' match '+term+' and metadata_items.metadata_type='+typ+' and tags.id='+scope+' order by '+(TV if tv else MOVIE)+' limit '+limit+' offset '+offset)
def seed_scope(path,no_affinity=False):
    d=sqlite3.connect(path);d.create_collation('icu_root',lambda a,b:(a.casefold()>b.casefold())-(a.casefold()<b.casefold()))
    aff='' if no_affinity else 'INTEGER'
    d.executescript('''CREATE TABLE metadata_items(id INTEGER PRIMARY KEY,metadata_type INTEGER,parent_id INTEGER,title TEXT,title_sort TEXT,original_title TEXT,originally_available_at TEXT,"index" INTEGER);
    CREATE TABLE tags(id INTEGER PRIMARY KEY,tag TEXT,tag_type INTEGER);
    CREATE TABLE taggings(tag_id '''+aff+''',metadata_item_id '''+aff+''');
    CREATE INDEX idx_dshadow_taggings_tag_id_metadata_item_id ON taggings(tag_id,metadata_item_id);
    CREATE INDEX index_taggings_on_metadata_item_id ON taggings(metadata_item_id);
    CREATE VIRTUAL TABLE fts4_metadata_titles_icu USING fts4(content='metadata_items',title,title_sort,original_title,tokenize=unicode61);''')
    words=['The mystery','The theatre','Comedy cafe','théâtre mystery','café',None,"O'Connor",'The theatre']
    rows=[(100001+i,2,None,'Show '+str(i%3),'Show '+str(i%3),None,None,i) for i in range(20)]
    rows += [(200001+i,3,100001+i%20,'Season','Season',None,None,None if i%7==0 else i%4) for i in range(60)]
    rows += [(i,4 if i%2==0 else 1,None if i%11==0 else (999999 if i%19==0 else 200001+i%60),words[i%8],words[(i+1)%8],None,None if i%5==0 else '2020-01-01',None if i%7==0 else i%3) for i in range(1,241)]
    rows += [(i,12 if i%2 else 13,None,'The extra','The extra',None,None,None) for i in range(300,330)]
    d.executemany('INSERT INTO metadata_items VALUES(?,?,?,?,?,?,?,?)',rows)
    d.executemany('INSERT INTO tags VALUES(?,?,?)',[(11,'scope one',500),(12,'scope two',500),(13,'empty scope',500)])
    links=[(tag,i) for tag in (11,12) for i in range(1,241) if i%(3 if tag==11 else 5)]
    links += [(11,i) for i in range(300,330)]
    links+=links[:40]+[(None,1),(11,None),(9999,2),(11,999999),('0011','00002'),('11.0','4.0'),(b'11',6),(11,b'8'),('abc',10)]
    d.executemany('INSERT INTO taggings VALUES(?,?)',links);d.execute("INSERT INTO fts4_metadata_titles_icu(fts4_metadata_titles_icu) VALUES('rebuild')");d.commit();return d
cases=0
duplicate_cases=0
with tempfile.TemporaryDirectory(prefix='plex-title-scope-test-') as temp:
 for aff in (False,True):
    path=Path(temp)/('noaff' if aff else 'normal');path.mkdir();path/='com.plexapp.plugins.library.db'
    ref=seed_scope(path,aff);db=ptr();check(open_db(os.fsencode(path),C.byref(db))==0,'open');check(register(db,b'icu_root',1,None,compare)==0,'collation')
    try:
      if len(sys.argv)>2:
        text=q();rc,rewrite,rows,_,_=run(db,text)
        enabled=os.environ.get('SQLITE3_ENABLE_PLEX_TITLE_SCOPE_REWRITE')=='1' and os.environ.get('SQLITE3_DISABLE_PLEX_FTS_REWRITE')!='1'
        check(rc==101 and ('CROSS JOIN' in rewrite)==enabled,('gate',enabled,rewrite));continue
      terms=['Th*','the*','Th* AND mys*','Th* OR caf*','Th* NOT mys*','"the theatre"','thé*','caf*','unfindable*','"unterminated']
      for tv in (False,True):
       for col in ('title','title_sort'):
        for term in terms:
         for scope in (11,12,13,9999,None,'0011','11 OR 1=1',11.0,b'11'):
          text=q(tv,col,'?',typ='?',scope='?',limit='?',offset='?');params=(term,4 if tv else 1,scope,7,2)
          rc,rewrite,rows,names,tail=run(db,text,params)
          try:expected=ref.execute(text,params).fetchall();expected_rc=101
          except sqlite3.Error:expected=[];expected_rc=1
          check(rc==expected_rc,('rc',term,rc,expected_rc));check(rows==expected,('rows',tv,col,term,scope));check('CROSS JOIN' in rewrite,('not rewritten',rewrite));check(tail==len(text),'tail');check(names==[None]*5,'anonymous binds');cases+=1
      # Named/numbered bindings, NULLs, negative bound limit, all prepare APIs and positive nByte.
      for tv in (False,True):
       for text,params in [(q(tv,term=':q',typ=':t',scope=':s',limit=':n',offset=':o'),('Th*',4 if tv else 1,11,-1,0)),(q(tv,term='?2',typ='?3',scope='?1',limit='?4',offset='?5'),(11,'Th*',4 if tv else 1,101,0)),(q(tv,term='?',typ='?',scope='?',limit='?',offset='?'),('Th*',None,11,101,0))]:
        expected=ref.execute(text,params).fetchall()
        if len(expected)>len(set(expected)):duplicate_cases+=1
        for kind in (0,1,2):
         for nbyte in (-1,len(text),len(text)+1):
          rc,rewrite,rows,names,tail=run(db,text,params,kind,nbyte,1 if kind==2 else 0)
          check(rc==101 and rows==expected,'variant results');check('CROSS JOIN' in rewrite,'variant eligibility');check(tail==len(text),'variant tail');cases+=1
      for typ in ('12','13'):
       text=q(typ=typ);expected=ref.execute(text).fetchall();check(len(expected)>0,'extra type exercised');rc,rewrite,rows,_,_=run(db,text);check(rc==101 and rows==expected and 'CROSS JOIN' in rewrite,'extra types');cases+=1
      text=q();base=raw_build(text);check(base is not None,'builder')
      for bad in [text.replace("'Th*'",'"tag"'),text.replace('tags.id=11','tags.id="tag_id"'),text+';',text+' UNION SELECT 1',text+'; SELECT 1',text.replace('tags.id=11','tags.id=11 OR 1=1'),text.replace('tags.id=11','tags.id IS NULL'),text.replace('tags.id=11','tags.id IN (11,12)'),text.replace('metadata_items.id from','metadata_items.* from'),text.replace('left join tags','right join tags'),text.replace('limit 101','limit -1'),text.replace('asc limit','desc limit'),text.replace('title match','original_title match'),text.replace('from metadata_items','from metadata_items m'),text.replace("'Th*'","'Th*'||'x'"),text.replace('metadata_items.metadata_type=1','metadata_items.metadata_type=1 AND metadata_items.remote IS NULL')]:
       check(raw_build(bad) is None,('unsafe shape accepted',bad))
      for n in range(len(text)):check(raw_build(text.encode()[:n]) is None,('truncated accepted',n))
      check(raw_build(text+' '*65536) is None,'oversize');check(raw_build(text,len(text)-1,len(text)) is None,'bounds');check(raw_build(text.encode()+b'\0ignored') is None,'NUL');check(raw_build(text.encode()+b'\0',len(text)+1,len(text)) is not None,'positive NUL')
      for comment in [text.replace(' from ',' /* scope */ from '),text+' -- trailing comment',text.replace("'Th*'","'Th* /* not SQL */'")]:check(raw_build(comment) is not None,'comment-safe parsing')
      expected=ref.execute(text).fetchall()
      for fault in (1,2,3):
       set_fault(fault);rc,rewritten,rows,_,_=run(db,text);check(rc==101 and rows==expected and rewritten==text,('fallback',fault))
      set_fault(0);set_path(0);check(run(db,text)[1]==text,'path gate');set_path(1)
      ref.execute('DROP INDEX idx_dshadow_taggings_tag_id_metadata_item_id');ref.commit();check('CROSS JOIN' not in run(db,text)[1],'missing-index fallback')
    finally:ref.close();check(close_db(db)==0,'close/no leaked statements')
if len(sys.argv)==2:
 check(duplicate_cases>0,'duplicate links actually exercised')
 for val in (None,'0','true','1'):
    env=dict(os.environ)
    if val is None:env.pop('SQLITE3_ENABLE_PLEX_TITLE_SCOPE_REWRITE',None)
    else:env['SQLITE3_ENABLE_PLEX_TITLE_SCOPE_REWRITE']=val
    subprocess.run([sys.executable,__file__,SO,'gate'],env=env,check=True)
 env=dict(os.environ,SQLITE3_ENABLE_PLEX_TITLE_SCOPE_REWRITE='1',SQLITE3_DISABLE_PLEX_FTS_REWRITE='1');subprocess.run([sys.executable,__file__,SO,'gate'],env=env,check=True)
 print(json.dumps({'status':'PASS','cases':cases,'duplicate_cases':duplicate_cases,'assertions':h.checks,'gate_processes':5,'tokenizer':'scratch unicode61; collation casefold, NOT native Plex ICU'}))
