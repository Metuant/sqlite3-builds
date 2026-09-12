import pathlib as P,subprocess as S,json,os,time,re,statistics
os.umask(0o077)
out=P.Path('/tmp/brutus-c-title-scope-validation-readonly-20260907');out.mkdir(exist_ok=True)
backup=P.Path('/opt/brutus/title-prefix-rollback/20260907T135624Z/Databases');db=backup/'com.plexapp.plugins.library.db'
assert not P.Path(str(db)+'-wal').exists()
before=(db.stat().st_size,db.stat().st_mtime_ns)
pairs=json.loads(P.Path('/tmp/brutus-c-title-scope-pairs.json').read_text())[:4]
variants={}
for i,p in enumerate(pairs):
 variants[f'{i}:original']=p['original'];variants[f'{i}:tag_first']=p['candidate']
(out/'queries.json').write_text(json.dumps(variants,indent=2))
lines=['.bail on','.mode list','.headers off','PRAGMA query_only=ON;','PRAGMA cache_size=-32768;','PRAGMA mmap_size=0;','SELECT sqlite_version();']
# Synthetic user-scope fixture, not a reconstruction of E-Wass permissions.
# Only TEMP tables change; the original DB remains immutable and bind-mounted read-only.
fixture=[
 'CREATE TEMP TABLE fixture_ids(id INTEGER PRIMARY KEY);',
 'INSERT INTO fixture_ids SELECT id FROM main.metadata_items WHERE library_section_id=1 ORDER BY id LIMIT 25000;',
 'INSERT OR IGNORE INTO fixture_ids SELECT id FROM main.metadata_items WHERE library_section_id=5 ORDER BY id LIMIT 25000;',
 'CREATE TEMP TABLE tags(id INTEGER PRIMARY KEY);',
 'INSERT INTO temp.tags VALUES(8353788);',
 'CREATE TEMP TABLE taggings(tag_id INTEGER,metadata_item_id INTEGER);',
 'INSERT INTO temp.taggings SELECT tag_id,metadata_item_id FROM main.taggings WHERE metadata_item_id IN (SELECT id FROM fixture_ids);',
 'INSERT INTO temp.taggings SELECT 8353788,id FROM fixture_ids WHERE id%3=0;',
 'CREATE INDEX temp.index_taggings_on_metadata_item_id ON taggings(metadata_item_id);',
 'CREATE INDEX temp.idx_dshadow_taggings_tag_id_metadata_item_id ON taggings(tag_id,metadata_item_id);',
 'PRAGMA query_only=ON;',
 '.print FIXTURE_COUNTS',
 'SELECT count(*) FROM fixture_ids;',
 'SELECT count(*) FROM temp.taggings;'
]
lines=[l for l in lines if l!='PRAGMA query_only=ON;']+fixture+['PRAGMA query_only=ON;']
for key,q in variants.items():lines+=['.print PLAN '+key,'EXPLAIN QUERY PLAN '+q+';']
lines+=['.timer on','.stats vmstep']
for rep in range(2):
 for i in range(4):
  for v in (['original','tag_first'] if rep==0 else ['tag_first','original']):
   key=str(i)+':'+v;lines+=['.print CASE '+key+':'+str(rep),variants[key]+';']
sql='\n'.join(lines)+'\n';(out/'experiment.sql').write_text(sql)
x=json.loads(S.check_output(['docker','inspect','brutus']))[0];runtime=x['GraphDriver']['Data']['MergedDir']+'/usr/lib/plexmediaserver'
a=['docker','run','--rm','-i','--name','brutus-c-title-scope-bench','--network','none','--read-only','--cpus','0.5','--memory','768m','--memory-swap','768m','--pids-limit','64','--tmpfs','/tmp:rw,size=32m','--tmpfs','/run/plex-temp:rw,size=16m','--tmpfs','/config:rw,size=32m','-v',runtime+':/usr/lib/plexmediaserver:ro','-v',str(backup)+':/snapshot:ro','-e','SQLITE3_ENABLE_PLEX_FTS_JOIN_REWRITE=1','-e','SQLITE3_DISABLE_RUNTIME_OPTIMIZE=1','-e','SQLITE3_DISABLE_AUTOPRAGMA=1','-e','SQLITE3_DISABLE_OBSERVABILITY=1','--entrypoint','/usr/bin/timeout',x['Image'],'-k','2','90','/usr/lib/plexmediaserver/Plex SQLite','file:/snapshot/com.plexapp.plugins.library.db?mode=ro&immutable=1']
r=S.run(a,input=sql,text=True,capture_output=True,timeout=105)
(out/'stdout.log').write_text(r.stdout);(out/'stderr.log').write_text(r.stderr)
assert before==(db.stat().st_size,db.stat().st_mtime_ns)
assert not P.Path(str(db)+'-wal').exists()
records={};key=None
for line in r.stdout.splitlines():
 if line.startswith('CASE '):key=line[5:];records[key]={'ids':[]}
 elif key:
  if re.fullmatch(r'\d+',line):records[key]['ids'].append(int(line))
  elif line.startswith('Run Time:'):
   m=re.search(r'real ([\d.]+)',line);records[key]['seconds']=float(m[1])
  elif line.startswith('VM-steps:'):records[key]['vm_steps']=int(line.split(':')[1])
result={'exit':r.returncode,'records':records,'backup_unchanged':True,'limitations':['Pre-prefix stopped snapshot; no prefix-index migration in this experiment.','Isolated runtime capped at half a CPU and 768MiB; timings are not production latency.','Synthetic permission fixture: 50000 metadata IDs, original ambient tags for those IDs, user membership id%3=0; NOT live E-Wass scope.','Two repetitions only; no claim of broad-query equivalence beyond these cases.']}
if r.returncode==0:
 assert len(records)==16
 for i in range(4):
  expected=records[f'{i}:original:0']['ids'];assert len(expected)==101;assert all(records[f'{i}:{v}:{rep}']['ids']==expected for v in ('original','tag_first') for rep in range(2))
 result['ordered_results_equal']=True
(out/'results.json').write_text(json.dumps(result,indent=2))
print(json.dumps({'exit':r.returncode,'ordered_results_equal':result.get('ordered_results_equal'),'cases':{k:{**{a:b for a,b in v.items() if a!='ids'},'rows':len(v['ids'])} for k,v in records.items()},'artifact':str(out)}))
if r.returncode:print(r.stderr[-1000:])

raise SystemExit(r.returncode)
