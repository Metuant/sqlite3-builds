#!/usr/bin/env python3
"""Exercise the actual C prepare wrapper and real SQLite, only on scratch DBs.
Usage: python3 tests/plex_fts_join_test.py /absolute/path/join-test.so
The adapter stubs telemetry/path eligibility, not SQL execution or rewriting.
"""
import ctypes as C
import json
import os
import re
from pathlib import Path
import sqlite3
import subprocess
import sys
import tempfile

SO = str(Path(sys.argv[1]).resolve())
worker = len(sys.argv) > 2
if not worker:
    os.environ['SQLITE3_ENABLE_PLEX_FTS_JOIN_REWRITE'] = '1'
os.environ.setdefault('SQLITE3_DISABLE_PLEX_FTS_REWRITE', '0')
lib = C.CDLL(SO)
ptr = C.c_void_p

def signature(name, result, *args):
    fn = getattr(lib, name)
    fn.restype, fn.argtypes = result, list(args)
    return fn

open_db = signature('sqlite3_open', C.c_int, C.c_char_p, C.POINTER(ptr))
close_db = signature('sqlite3_close', C.c_int, ptr)
prepare = signature('test_prepare', C.c_int, ptr, ptr, C.c_int, C.c_uint, C.POINTER(ptr), C.POINTER(ptr), C.c_int)
step = signature('sqlite3_step', C.c_int, ptr)
finalize = signature('sqlite3_finalize', C.c_int, ptr)
sql_text = signature('sqlite3_sql', C.c_char_p, ptr)
bind_count = signature('sqlite3_bind_parameter_count', C.c_int, ptr)
bind_name = signature('sqlite3_bind_parameter_name', C.c_char_p, ptr, C.c_int)
bind_text = signature('sqlite3_bind_text', C.c_int, ptr, C.c_int, C.c_char_p, C.c_int, ptr)
bind_int = signature('sqlite3_bind_int64', C.c_int, ptr, C.c_int, C.c_longlong)
bind_null = signature('sqlite3_bind_null', C.c_int, ptr, C.c_int)
column_int = signature('sqlite3_column_int64', C.c_longlong, ptr, C.c_int)
next_stmt = signature('sqlite3_next_stmt', ptr, ptr, ptr)
build = signature('test_build', ptr, ptr, C.c_size_t, C.c_size_t, C.POINTER(C.c_size_t), C.POINTER(C.c_size_t))
release = signature('test_free', None, ptr)
set_fault = signature('test_fault', None, C.c_int)
set_path = signature('test_path_allowed', None, C.c_int)
get_kind = signature('test_kind', C.c_int)
get_flags = signature('test_flags', C.c_uint)
get_skip = signature('test_last_skip', C.c_char_p)
checks = 0

def check(condition, detail):
    global checks
    checks += 1
    assert condition, detail

BASE = ("select distinct(tags.id) from metadata_items join taggings on "
        "taggings.metadata_item_id=metadata_items.id join tags on tags.id=taggings.tag_id "
        "join fts4_tag_titles_icu on fts4_tag_titles_icu.rowid=tags.id where "
        "fts4_tag_titles_icu.tag match {term} and {tag} and "
        "metadata_items.library_section_id in ({sections}) and metadata_items.metadata_type={type} "
        "group by tags.id order by count(*) desc limit {limit}")

def query(term="'Th*'", tag='tag_type=1', sections='1', kind='1', limit='100'):
    return BASE.format(term=term, tag=tag, sections=sections, type=kind, limit=limit)

def seed(path):
    d = sqlite3.connect(path)
    d.executescript('''
      CREATE TABLE tags(id INTEGER PRIMARY KEY,tag TEXT,tag_type INTEGER);
      CREATE TABLE metadata_items(id INTEGER PRIMARY KEY,library_section_id INTEGER,metadata_type INTEGER,remote INTEGER);
      CREATE TABLE taggings(tag_id INTEGER,metadata_item_id INTEGER);
      CREATE INDEX idx_dshadow_taggings_tag_id_metadata_item_id ON taggings(tag_id,metadata_item_id);
      CREATE INDEX metadata_section_type ON metadata_items(library_section_id,metadata_type);
      CREATE VIRTUAL TABLE fts4_tag_titles_icu USING fts4(content='tags',tag,tokenize=unicode61);
    ''')
    words = ['theater mystery', 'comedy romance', 'theatre', 'mystery', 'théâtre', 'café', "O Connor", 'theater comedy', None]
    d.executemany('INSERT INTO tags VALUES(?,?,?)', [(i, words[(i-1) % len(words)], 1 if i % 5 else 6) for i in range(1, 181)])
    d.executemany('INSERT INTO metadata_items(id,library_section_id,metadata_type) VALUES(?,?,?)', [(i, 2 if i % 7 == 0 else 1, 2 if i % 11 == 0 else 1) for i in range(1, 501)])
    links = [(i, j) for i in range(1, 181) for j in range(1, 501) if (j + i) % (i % 13 + 3) == 0]
    links += links[:30] + [(None, 1), (1, None), (9999, 2), (1, 9999)]
    d.executemany('INSERT INTO metadata_items VALUES(?,?,?,?)', [(i, 19, 9, None if i % 3 == 0 else i % 2) for i in range(600,681)])
    links += [(tag,i) for tag in range(1,181) for i in range(600,681) if (tag+i)%7==0]
    d.executemany('INSERT INTO taggings VALUES(?,?)', links)
    d.execute("INSERT INTO fts4_tag_titles_icu(fts4_tag_titles_icu) VALUES('rebuild')")
    d.commit()
    return d

def raw_build(sql, bounded=None, scan=None):
    data = sql.encode() if isinstance(sql, str) else sql
    b = C.create_string_buffer(data)
    n = len(data) if bounded is None else bounded
    s = n if scan is None else scan
    out_scan, out_len = C.c_size_t(), C.c_size_t()
    out = build(C.addressof(b), n, s, C.byref(out_scan), C.byref(out_len))
    if not out:
        return None
    try:
        text = C.string_at(out, out_len.value)
        check(out_scan.value <= out_len.value, 'output bounds')
        return text
    finally:
        release(out)

def run(db, sql, params=(), kind=1, nbyte=-1, flags=0):
    data = sql.encode() if isinstance(sql, str) else sql
    b = C.create_string_buffer(data)
    stmt, tail = ptr(), ptr()
    rc = prepare(db, C.addressof(b), nbyte, flags, C.byref(stmt), C.byref(tail), kind)
    if rc or not stmt:
        if stmt:
            finalize(stmt)
        return rc, "", [], [], tail.value - C.addressof(b) if tail.value else -1
    try:
        text = sql_text(stmt).decode()
        names = [bind_name(stmt, i) for i in range(1, bind_count(stmt)+1)]
        check(len(params) == len(names), ('binding count', names, params, text))
        for i, value in enumerate(params, 1):
            if value is None: rc = bind_null(stmt, i)
            elif isinstance(value, int): rc = bind_int(stmt, i, value)
            else: rc = bind_text(stmt, i, value.encode(), -1, ptr(-1))
            check(rc == 0, 'binding failed')
        rows = []
        while True:
            rc = step(stmt)
            if rc != 100: break
            rows.append((column_int(stmt, 0),))
        return rc, text, rows, names, tail.value - C.addressof(b) if tail.value else -1
    finally:
        finalize(stmt)

with tempfile.TemporaryDirectory(prefix='plex-fts-join-test-') as temp:
    path = Path(temp) / 'com.plexapp.plugins.library.db'
    reference = seed(path)
    db = ptr()
    check(open_db(os.fsencode(path), C.byref(db)) == 0, 'open')
    try:
        if worker:
            rc, rewritten, *_ = run(db, query())
            enabled = os.environ.get('SQLITE3_ENABLE_PLEX_FTS_JOIN_REWRITE') == '1' and os.environ.get('SQLITE3_DISABLE_PLEX_FTS_REWRITE') != '1'
            check(rc == 101 and ('CROSS JOIN' in rewritten) == enabled, ('gate', enabled, rewritten))
            print(json.dumps({'gate': sys.argv[2], 'checks': checks}))
            sys.exit(0)

        terms = ['Th*', 'Th* AND mys*', 'Th* OR com*', 'Th* NOT mys*', '(Th* OR com*) AND mys*', '"theater mystery"', 'Th* NEAR/3 mys*', 'thé*', 'caf*', 'theater NOT "O Connor"', 'Th* OR (com* AND rom*)', '*', 'NOT Th*', "Th*' OR 1=1 --"]
        for term in terms:
            for wrapped in (False, True):
                q = query("'" + term.replace("'", "''") + "'", tag='unlikely(tag_type=1)' if wrapped else 'tag_type=1')
                rc, rewritten, rows, _, tail = run(db, q)
                check('CROSS JOIN' in rewritten, ('not rewritten', q))
                check(tail == len(q.encode()), 'tail mapping')
                check(q[q.index(' where '):].encode() == rewritten[rewritten.index(' WHERE'):].replace(' WHERE', ' where', 1).encode(), 'suffix changed')
                try: expected = reference.execute(q).fetchall()
                except sqlite3.Error:
                    check(rc == 1, ('expected MATCH error', term, rc))
                else:
                    check(rc == 101 and rows == expected, ('rows', term, rc, rows, expected))
                    # Compare aggregate counts too, not just selected tag IDs.
                    original_counts = q.replace('select distinct(tags.id)', 'select tags.id,count(*)')
                    rewritten_counts = rewritten.replace('select distinct(tags.id)', 'select tags.id,count(*)')
                    check(reference.execute(original_counts).fetchall() == reference.execute(rewritten_counts).fetchall(), 'counts changed')
                check(raw_build(rewritten) is None, 'not idempotent')

        # Exact captured audiobook predicate, including non-NULL remote exclusions.
        for term in terms:
            for bound in (False, True):
                q = query('?' if bound else "'"+term.replace("'", "''")+"'", tag='unlikely(tag_type=1)', sections='19', kind='9')
                q = q.replace('metadata_items.metadata_type=9', '(metadata_items.metadata_type=9 and (metadata_items.remote is null))')
                params = (term,) if bound else ()
                for api_kind in (0,1,2):
                    rc, rewritten, rows, names, tail = run(db, q, params, api_kind, len(q.encode()))
                    check('CROSS JOIN' in rewritten, ('album not rewritten',q))
                    check(tail == len(q.encode()), 'album tail')
                    check(q[q.index(' where '):] == rewritten[rewritten.index(' WHERE'):].replace(' WHERE',' where',1), 'album suffix changed')
                    try: expected = reference.execute(q, params).fetchall()
                    except sqlite3.Error: check(rc == 1, ('album expected error',term))
                    else:
                        check(rc == 101 and rows == expected, ('album rows', term))
                        a=q.replace('select distinct(tags.id)','select tags.id,count(*)')
                        b=rewritten.replace('select distinct(tags.id)','select tags.id,count(*)')
                        check(reference.execute(a,params).fetchall()==reference.execute(b,params).fetchall(),'album counts')
        album = query(sections='19',kind='9').replace('metadata_items.metadata_type=9','(metadata_items.metadata_type=9 and (metadata_items.remote is null))')
        for change in ['is not null','is null OR 1=1','is null) OR (1=1','=', 'is false']:
            check(raw_build(album.replace('is null',change)) is None,('unsupported remote predicate',change))
        for n in range(len(album)):
            check((raw_build(album.encode()[:n]) is not None)==(n>=len(album)-2),('album bounded prefix',n))

        variants = [
            (query('?', 'tag_type=?', '?', '?', '?'), ('Th*', 1, 1, 1, 70)),
            (query('?5', 'tag_type=?4', '?3', '?2', '?1'), (70, 1, 1, 1, 'Th*')),
            (query(':q', 'tag_type=:v', ':v', ':v', ':lim'), ('Th*', 1, 70)),
            (query('@q', 'tag_type=$v', '$v', '$v', '@lim'), ('Th*', 1, 70)),
            (query('?', 'tag_type=?', '?', '?', '?'), ('Th*', None, 1, 1, 70)),
            (query(sections='1,2', limit='0'), ()),
            (query(sections='1,2', limit='1'), ()),
            (query(sections='1,2', limit='70'), ()),
            (query(tag='tag_type=6'), ()),
            (query(kind='2'), ()),
            (query(sections='99'), ()),
            (query()+' \n /* trailing comment */ ', ()),
            (query(tag="tag_type='1'", limit="'100'"), ()),
            (query().upper(), ()),
            (query().replace(' join ', '\n/* join */ JOIN\t'), ()),
        ]
        for q, params in variants:
            for kind in (0, 1, 2):
                rc, rewritten, rows, names, tail = run(db, q, params, kind, len(q.encode()), flags=1 if kind == 2 else 0)
                check(rc == 101 and 'CROSS JOIN' in rewritten, ('variant', q, rc))
                check(rows == reference.execute(q, params).fetchall(), 'variant results')
                check(tail == len(q.encode()), 'positive-nByte tail')
                check(get_kind() == kind and get_flags() == (1 if kind == 2 else 0), 'prepare API/flags changed')
                if ':q' in q: check(names == [b':q', b':v', b':lim'], 'named binding identity')

        q = query()
        generated = run(db, q)[1]
        plan = [row[3] for row in reference.execute('EXPLAIN QUERY PLAN '+generated)]
        chain = ['fts4_tag_titles_icu', 'tags', 'taggings', 'metadata_items']
        positions = [next(i for i, line in enumerate(plan) if re.search(r'(?:SCAN|SEARCH) '+name+r'\b', line)) for name in chain]
        check(positions == sorted(positions), ('join order', plan))
        check('idx_dshadow_taggings_tag_id_metadata_item_id' in plan[positions[2]], ('covering index', plan))
        check('rowid=?' in plan[positions[3]] or 'id=?' in plan[positions[3]], ('metadata lookup', plan))
        for bad in [q+';', q+'; SELECT 2', q+' OFFSET 1', q+' UNION SELECT 9', q.replace('join tags', 'left join tags'), q.replace('tag_type=1', 'tag_type=1 OR 1=1'), q.replace('distinct(tags.id)', '*'), q.replace('limit 100', 'limit -1'), q.replace("'Th*'", "'Th*'||'x'"), q.replace('tag_type=1', 'tags.tag_type=1'), q.replace('metadata_items.id join tags', 'metadata_items.id AND 1=1 join tags'), query(sections=','.join(['1']*65))]:
            check(raw_build(bad) is None, ('unsupported accepted', bad))
        for n in range(len(q)):
            # LIMIT 1 and LIMIT 10 are complete SQL, not malformed truncations.
            result = raw_build(q.encode()[:n])
            check((result is not None) == (n >= len(q)-2), ('truncation boundary', n))
        check(raw_build(q+' '*65536) is None, 'size guard')
        check(raw_build(q, len(q)-1, len(q)) is None, 'invalid bounds')
        check(raw_build(q.encode()+b'\0ignored', len(q)+8, len(q)) is None, 'reject ignored suffix')
        check(raw_build(q.encode()+b'\0ignored') is None, 'embedded NUL in scan')
        rc, rewritten, rows, _, tail = run(db, q.encode()+b'\0ignored', nbyte=len(q)+8)
        check(rc == 101 and 'CROSS JOIN' not in rewritten and tail == len(q), 'NUL-suffix fallback')
        rc, rewritten, rows, _, tail = run(db, q.encode()+b'\0', nbyte=len(q)+1)
        check(rc == 101 and 'CROSS JOIN' in rewritten and tail == len(q), 'terminating NUL real wrapper')

        def outstanding():
            seen = set()
            stmt = next_stmt(db, None)
            while stmt:
                seen.add(stmt)
                stmt = next_stmt(db, stmt)
            return seen
        # FTS4 retains its own prepared statements. Compare against the warm
        # cache baseline rather than falsely treating internal caches as leaks.
        baseline_statements = outstanding()
        for fault in (1, 2, 3):
            set_fault(fault)
            rc, rewritten, rows, _, _ = run(db, q)
            check(rc == 101 and rewritten == q, ('prepare fallback', fault, rewritten))
            expected_reason = {1: b'rewritten_prepare_failed', 2: b'tail_mismatch', 3: b'column_count_mismatch'}[fault]
            check(get_skip() == expected_reason, ('fallback reason', fault, get_skip()))
            check(rows == reference.execute(q).fetchall(), 'fallback results')
            check(outstanding() == baseline_statements, ('fallback leaked statement', fault, outstanding()-baseline_statements))
        set_fault(0)
        set_path(0)
        check(run(db, q)[1] == q, 'path gate')
        set_path(1)
        other = ptr()
        other_path = Path(temp)/'other.db'
        import shutil
        reference.commit()
        shutil.copy2(path, other_path)
        check(open_db(os.fsencode(other_path), C.byref(other)) == 0, 'other open')
        try: check(run(other, q)[1] == q, 'basename gate')
        finally: check(close_db(other) == 0, 'other close')
        reference.execute('DROP INDEX idx_dshadow_taggings_tag_id_metadata_item_id')
        reference.commit()
        check('CROSS JOIN' not in run(db, q)[1], 'missing-index fallback')
        check(len(outstanding()) <= len(baseline_statements), 'statement leak')
    finally:
        reference.close()
        check(close_db(db) == 0, 'close/statement leak')

if not worker:
    for value in (None, '0', 'true', '1'):
        env = dict(os.environ)
        if value is None: env.pop('SQLITE3_ENABLE_PLEX_FTS_JOIN_REWRITE', None)
        else: env['SQLITE3_ENABLE_PLEX_FTS_JOIN_REWRITE'] = value
        subprocess.run([sys.executable, __file__, SO, str(value)], env=env, check=True)
    env = dict(os.environ, SQLITE3_ENABLE_PLEX_FTS_JOIN_REWRITE='1', SQLITE3_DISABLE_PLEX_FTS_REWRITE='1')
    subprocess.run([sys.executable, __file__, SO, 'global-disable'], env=env, check=True)
    print(json.dumps({'status': 'PASS', 'assertions': checks, 'reference_sqlite': sqlite3.sqlite_version, 'wrapper_sqlite': signature('sqlite3_libversion', C.c_char_p)().decode(), 'boolean_wrapped_cases': len(terms)*2, 'prepare_variant_cases': len(variants)*3, 'gate_processes': 5}))
