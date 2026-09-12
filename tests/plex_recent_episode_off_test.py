import os,sys,pathlib
src=pathlib.Path(__file__).with_name('plex_recent_episode_test.py').read_text().split('count=0;')[0]
exec(src.replace("os.environ['SQLITE3_ENABLE_PLEX_RECENT_EPISODE_REWRITE']='1'","os.environ.pop('SQLITE3_ENABLE_PLEX_RECENT_EPISODE_REWRITE',None)"))
with tempfile.TemporaryDirectory() as tmp:
 p=Path(tmp)/'com.plexapp.plugins.library.db';d=sqlite3.connect(p);d.executescript('CREATE TABLE metadata_items(id,parent_id,library_section_id,metadata_type,added_at,title,title_sort);CREATE TABLE tags(id);CREATE TABLE taggings(metadata_item_id,tag_id);');d.close()
 db=ptr();check(open_db(os.fsencode(p),C.byref(db))==0,'open');coll(db,b'icu_root',1,None,cmp)
 r=run(db,q,(5,'Exclude',10,0,2,2,7,0));check(r[0]==101 and r[1]==q,'disabled default');check(close_db(db)==0,'close')
print('PASS: unset feature flag retains original SQL')
