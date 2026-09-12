"""Additional hub bounds, SQLite bind numbering and prepare-recovery checks.

Spec cases 1-17 live in plex_fts_rewrite_smoke.c (the real linked library).
This adapter suite exercises byte bounds and injected candidate failures.
"""
import os
os.environ['SQLITE3_DISABLE_PLEX_HUB_TYPE_OR_REWRITE'] = '0'
os.environ['SQLITE3_DISABLE_PLEX_FTS_REWRITE'] = '1'
os.environ['SQLITE3_DISABLE_PLEX_TAGGINGS_REWRITE'] = '1'
import plex_prepare_test_support as h
from plex_prepare_test_support import *

HEAD = ('select metadata_items.id from metadata_items  join metadata_items as children on children.parent_id=metadata_items.id'
        '  join metadata_items as grandchildren on grandchildren.parent_id=children.id  where metadata_items.library_section_id ')
OR = ' and (metadata_items.metadata_type=2 or metadata_items.metadata_type=10)'
Q = HEAD + "in (5) and (metadata_items.title NOT LIKE 'Naked News' and grandchildren.added_at>0" + OR + ')  and metadata_items.metadata_type=2  order by metadata_items.added_at desc,metadata_items.title_sort  limit 39 offset 0'

with tempfile.TemporaryDirectory() as tmp:
    path = Path(tmp)/'com.plexapp.plugins.library.db'
    d = sqlite3.connect(path)
    d.executescript('''CREATE TABLE metadata_items(id INTEGER PRIMARY KEY,parent_id INTEGER,
      library_section_id INTEGER,metadata_type INTEGER,title TEXT,title_sort TEXT,added_at INTEGER);
      CREATE INDEX parent_idx ON metadata_items(parent_id);
      CREATE INDEX hub_idx ON metadata_items(library_section_id,metadata_type,added_at DESC,title_sort);
      INSERT INTO metadata_items VALUES(1,NULL,5,2,'Show','same',10),
        (2,NULL,5,2,'Show','same',10),(1001,1,5,3,NULL,NULL,0),(1002,2,5,3,NULL,NULL,0),
        (2001,1001,5,4,NULL,NULL,1),(2000,1002,5,4,NULL,NULL,2); ANALYZE;''')
    db = ptr(); check(open_db(os.fsencode(path), C.byref(db)) == 0, 'open')
    set_path(1)
    expected = Q.replace(OR, '')
    r = run(db, Q)
    check(r[0] == 101 and r[1] == expected, 'hub hit')
    # The earlier matcher must not recognize this statement even when enabled.
    recent_build = signature('test_recent_build', ptr, ptr, C.c_size_t, C.c_size_t, C.POINTER(C.c_size_t), C.POINTER(C.c_size_t))
    h.build = recent_build
    check(raw_build(Q) is None, 'recent matcher misses hub')
    for value in ('?', '?1', '?8'):
        s = Q.replace('in (5)', 'in ('+value+')')
        params = ((None,)*7+(5,)) if value == '?8' else (5,)
        check(run(db, s, params)[1] == s.replace(OR, ''), 'section numbered slots')
    s = Q.replace('in (5)', 'in (?)').replace("'Naked News'", ':p').replace('grandchildren.added_at>0',
        'grandchildren.added_at>0 and :p IS NOT NULL').replace('limit 39 offset 0', 'limit ?7 offset ?')
    for kind in (0, 1, 2):
        r = run(db, s, (5,'Naked News',None,None,None,None,39,0), kind)
        check(r[1] == s.replace(OR, '') and len(r[3]) == 8 and r[3][1] == b':p', 'all bind sites')
    limit = signature('sqlite3_limit',C.c_int,ptr,C.c_int,C.c_int)
    previous_limit = limit(db,9,64)  # SQLITE_LIMIT_VARIABLE_NUMBER
    for section in ('in ()', 'in (?,5)', 'in (5,?)', '= :section', 'in (?01)', 'in (?0)', 'in (?32767)', '= -5', 'in (5+0)'):
        s = Q.replace('in (5)', section)
        # Prepare only: invalid variables and expressions remain SQLite's decision.
        buf=C.create_string_buffer(s.encode());stmt,tail=ptr(),ptr()
        rc=prepare(db,C.addressof(buf),-1,0,C.byref(stmt),C.byref(tail),1)
        if stmt:
            check(sql_text(stmt).decode()==s,'section fail open');finalize(stmt)
        else: check(rc != 0,'invalid original remains an error')
    limit(db,9,previous_limit)
    # Every prefix is compared with native preparation. Complete shorter
    # ORDER/LIMIT tails may rewrite; no byte past nByte may be inspected.
    for n in range(len(Q)):
        buf=C.create_string_buffer(Q.encode());stmts=[];results=[]
        for enabled in (0,1):
            set_path(enabled);stmt,tail=ptr(),ptr()
            rc=prepare(db,C.addressof(buf),n,0,C.byref(stmt),C.byref(tail),1)
            results.append((rc,sql_text(stmt) if stmt else None));stmts.append(stmt)
        check(results[0][0]==results[1][0],('truncated prefix rc changed',n,results))
        if results[1][1] is not None:
            check(results[1][1] in (results[0][1], results[0][1].replace(OR.encode(),b'')),('bounded text',n))
        for stmt in stmts:
            if stmt:finalize(stmt)
    set_path(1)
    for suffix in (b'\0junk', b'\0', b' '*65536):
        sql=Q.encode()+suffix
        r=run(db,sql,nbyte=len(sql))
        check(r[0]==101,'bounded original succeeds')
        check((r[1]==expected)==(suffix==b'\0'),'bounded eligibility')
    for fault in (1,2,3,4):
        set_fault(fault);r=run(db,Q);set_fault(0)
        check(r[0]==101 and r[1]==Q,('hub prepare fallback',fault,r))
        check(get_skip()=={1:b'rewritten_prepare_failed',2:b'tail_mismatch',3:b'column_count_mismatch',4:b'bind_count_mismatch'}[fault],'verification reason')
    for bad in (Q.replace(')  and metadata_items.metadata_type=', ') and metadata_items.metadata_type='),
                Q.replace('  order by ', ' order by '), Q.replace(OR,'')):
        check(run(db,bad)[1]==bad,'tail/no-group miss')
    # Best effort STAT4 planner reproduction; text/results remain mandatory.
    plans = [[row[3] for row in d.execute('EXPLAIN QUERY PLAN '+sql)] for sql in (Q,expected)]
    sorts = [any('TEMP B-TREE' in line and 'ORDER BY' in line for line in plan) for plan in plans]
    if sorts[0]:
        check(not sorts[1], 'hub rewrite retained the reproduced ORDER BY sort')
        plan_status='PASS: original sort removed'
    else: plan_status='NOT REPRODUCED: synthetic fixture did not produce the original-only sort'
    print(json.dumps({'STAT4_plan':plan_status,'original':plans[0],'rewritten':plans[1]}))
    check(close_db(db)==0,'close');d.close()
print(json.dumps({'status':'PASS','hub_extra_assertions':h.checks}))
