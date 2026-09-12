import ast,json,os,pathlib,subprocess
w=pathlib.Path('/work');ns={}
tree=ast.parse((w/'tests/plex_title_scope_test.py').read_text())
# Reuse just pure query templates, without loading the adapter/shared library.
for node in tree.body:
 if isinstance(node,ast.Assign) and any(isinstance(t,ast.Name) and t.id in ('PARENTS','MOVIE','TV') for t in node.targets):exec(compile(ast.Module(body=[node],type_ignores=[]),'templates','exec'),ns)
 if isinstance(node,ast.FunctionDef) and node.name=='q':exec(compile(ast.Module(body=[node],type_ignores=[]),'templates','exec'),ns)
q=ns['q'];out=w/'native-full-library';out.mkdir(exist_ok=True);db=out/'com.plexapp.plugins.library.db'
assert not db.exists(),'Use a fresh scratch directory, not an existing database'
env=dict(os.environ)
for k in ['RUNTIME_OPTIMIZE','AUTOPRAGMA','OBSERVABILITY','PLEX_TAGGINGS_REWRITE','PLEX_GUID_LIKE_REWRITE','PLEX_ONDECK_REWRITE']:env['SQLITE3_DISABLE_'+k]='1'
env['SQLITE3_DISABLE_PLEX_FTS_REWRITE']='0';env['SQLITE3_ENABLE_PLEX_FTS_JOIN_REWRITE']='1'
def run(mode,sql,enabled=False,param=None):
 p=out/'current.sql';p.write_text(sql);e=dict(env,SQLITE3_ENABLE_PLEX_TITLE_SCOPE_REWRITE='1' if enabled else '0');cmd=[str(w/'build/title-native-runner'),mode,str(db),str(p)]
 if param is not None:cmd.append(param)
 proc=subprocess.run(cmd,env=e,capture_output=True,text=True,timeout=35)
 try:return json.loads(proc.stdout)
 except Exception:raise RuntimeError((proc.returncode,proc.stdout,proc.stderr[-1000:]))
assert run('init',(w/'tests/title-native-seed.sql').read_text())['rc']==0
records=[]
for tv in (False,True):
 for col in ('title','title_sort'):
  for term in ['Th*','Th* OR caf*','Th* AND mys*','Th* NOT mys*','"the theatre"','thé*','caf*','"unterminated']:
   for scope in ('11','12'):
    for bound in (False,True):
     text=q(tv,col,term='?1' if bound else "'"+term.replace("'","''")+"'",scope=scope,limit='1000')
     a=run('query',text,False,term if bound else None);b=run('query',text,True,term if bound else None)
     assert a['version']==b['version']=='3.53.3'
     assert 'CROSS JOIN' not in a['sql'] and 'CROSS JOIN' in b['sql'],(a,b)
     assert a['rows']==b['rows'] and a['rc']==b['rc'],(a,b)
     if a['rc']!=101:assert a['error']==b['error'],(a,b)
     records.append(dict(tv=tv,column=col,term=term,scope=scope,bound=bound,baseline=a,candidate=b))
for scope in ('13','9999'):
 a=run('query',q(scope=scope));b=run('query',q(scope=scope),True);assert a['rows']==b['rows']==[] and a['rc']==b['rc']==101
for typ in ('12','13'):
 a=run('query',q(typ=typ));b=run('query',q(typ=typ),True);assert a['rows']==b['rows'] and len(a['rows'])>0 and a['rc']==b['rc']==101
assert 'CROSS JOIN' not in run('query',q()+';',True)['sql']
assert run('init','DROP INDEX idx_dshadow_taggings_tag_id_metadata_item_id;')['rc']==0
assert 'CROSS JOIN' not in run('query',q(),True)['sql']
(out/'results.json').write_text(json.dumps(records,indent=2))
print(json.dumps(dict(status='PASS',full_library='3.53.3',cases=len(records),extra_scope_cases=4,fallbacks=['semicolon','missing index'],tokenizer='ICU root, not PMS application collating',complete_ordered_results_equal=True)))
