"""Spec section 5: bounded real prepares, unchanged real stats and first ANALYZE."""
import os
import sys

GATE = 'SQLITE3_DISABLE_PLEX_APP_ANALYZE_REWRITE'
gate_worker = '--gate' in sys.argv
if not gate_worker:
    os.environ[GATE] = '0'
# A successful ANALYZE must return before even the fuzzy no_match producer.
os.environ['SQLITE3_DISABLE_PLEX_FUZZY_MATCH_REWRITE'] = '0'
import plex_prepare_test_support as h
from plex_prepare_test_support import *

miss = signature('test_analyze_miss', C.c_char_p)
applied = signature('test_analyze_applied', C.c_int)
fuzzy_miss = signature('test_fuzzy_miss', C.c_char_p)
calls = signature('test_calls', C.c_int)
REPLACEMENT = b'ANALYZE sqlite_stat1'


def prepared(db, sql, kind=1, nbyte=-1):
    data = sql.encode() if isinstance(sql, str) else sql
    buf = C.create_string_buffer(data)
    stmt, tail = ptr(), ptr()
    rc = prepare(db, C.addressof(buf), nbyte, 1 if kind == 2 else 0,
                 C.byref(stmt), C.byref(tail), kind)
    result = (rc, sql_text(stmt) if stmt else None,
              tail.value-C.addressof(buf) if tail.value else None,
              column_count(stmt) if stmt else None,
              bind_count(stmt) if stmt else None)
    return stmt, result


with tempfile.TemporaryDirectory() as tmp:
    path = Path(tmp)/'com.plexapp.plugins.library.db'
    d = sqlite3.connect(path)
    d.executescript('CREATE TABLE t(x); CREATE INDEX tx ON t(x);'
                   'INSERT INTO t VALUES(1),(1),(2); ANALYZE;')
    before = d.execute('SELECT * FROM sqlite_stat1 ORDER BY tbl,idx').fetchall()
    check(before == [('t', 'tx', '3 2')], 'fixture has real index stats')
    # Stale real stats distinguish the rewrite from a full ANALYZE.
    d.executescript('INSERT INTO t VALUES(3),(4),(5),(6),(7);')
    db = ptr()
    check(open_db(os.fsencode(path), C.byref(db)) == 0, 'open')
    if gate_worker:
        enabled = os.environ.get(GATE) == '0'
        stmt, result = prepared(db, 'ANALYZE main')
        check(result[:2] == (0, REPLACEMENT if enabled else b'ANALYZE main'), 'literal gate')
        check(applied() == int(enabled) and miss() == b'', 'gate telemetry')
        check(step(stmt) == 101 and finalize(stmt) == 0, 'gate execution')
        after = d.execute('SELECT * FROM sqlite_stat1 ORDER BY tbl,idx').fetchall()
        check((after == before) == enabled, 'gate controls real stats refresh')
        check(close_db(db) == 0, 'close'); d.close()
        print('PASS: analyze gate '+repr(os.environ.get(GATE)))
        sys.exit(0)

    hits = ('ANALYZE', 'ANALYZE;', '  analyze main ;', 'Analyze MAIN',
            '\t\r\nanalyze\fmain; \t\n', 'ANALYZE ')
    for q in hits:
        for kind in (0, 1, 2):
            for n in (-1, len(q), len(q)+1):
                stmt, result = prepared(db, q, kind, n)
                check(result == (0, REPLACEMENT, len(q), 0, 0), ('exact interface/tail', q, result))
                check(calls() == 1 and applied() == 1 and fuzzy_miss() == b'', 'first dispatch')
                check(get_kind() == kind and get_flags() == (1 if kind == 2 else 0), 'API/flags')
                check(step(stmt) == 101 and finalize(stmt) == 0, 'DONE, no rows')
                check(d.execute('SELECT * FROM sqlite_stat1 ORDER BY tbl,idx').fetchall() == before,
                      'real stats unchanged')

    misses = [('ANALYZE t', 'targeted'), ('ANALYZE main.t', 'targeted'),
              ('ANALYZE sqlite_schema', 'targeted'), ('ANALYZE sqlite_master', 'targeted'),
              ('ANALYZE "main"', 'dqs'), ('ANALYZE main; select 1', 'trailing'),
              ('ANALYZE /*x*/', 'comment'), ('-- c\nANALYZE', 'comment'),
              ('/* c */ ANALYZE', 'comment'), ('ANALYZE main x', 'trailing'),
              ('ANALYZEX', 'trailing'), ('PRAGMA optimize', ''),
              ('ANALYZE;;', 'trailing'), ('ANALYZE main -- x', 'comment'),
              ('ANALYZE mainx', 'targeted'), ('ANALYZE temp', 'targeted'),
              ('ANALYZE "t"', 'dqs'), (b'ANALYZE\0junk', 'trailing')]
    for q, why in misses:
        for kind in (0, 1, 2):
            n = len(q) if isinstance(q, bytes) else -1
            set_path(0); native, expected = prepared(db, q, kind, n)
            if native: finalize(native)
            set_path(1); stmt, result = prepared(db, q, kind, n)
            check(result == expected, ('original rc/text/tail unchanged', q, result, expected))
            check(miss().decode() == why and applied() == 0, ('miss reason', q, miss()))
            if stmt: finalize(stmt)

    for fault, why in ((1, b'rewritten_prepare_failed'), (2, b'tail_mismatch'),
                       (3, b'column_count_mismatch'), (4, b'bind_count_mismatch')):
        set_fault(fault); stmt, result = prepared(db, 'ANALYZE'); set_fault(0)
        check(result[:2] == (0, b'ANALYZE') and calls() == 2, 'candidate retry')
        check(get_skip() == why and applied() == 0, 'verification failure telemetry')
        finalize(stmt)

    # Explicit nByte excludes a suffix, while embedded NULs remain fail-open.
    stmt, result = prepared(db, b'ANALYZEjunk', nbyte=7)
    check(result == (0, REPLACEMENT, 7, 0, 0), 'bounded prefix only'); finalize(stmt)
    check(close_db(db) == 0, 'close'); d.close()

    for filename in ('other.db', 'library.db', 'prefixcom.plexapp.plugins.library.db'):
        path = Path(tmp)/filename
        check(open_db(os.fsencode(path), C.byref(db)) == 0, 'non-target open')
        stmt, result = prepared(db, 'ANALYZE')
        check(result[:2] == (0, b'ANALYZE') and applied() == 0 and miss() == b'', 'basename gate')
        finalize(stmt); check(close_db(db) == 0, 'non-target close')

    # Each API gets its own never-analyzed database; retry must create stats.
    for kind in (0, 1, 2):
        folder = Path(tmp)/str(kind); folder.mkdir()
        path = folder/'com.plexapp.plugins.library.db'
        d = sqlite3.connect(path)
        d.executescript('CREATE TABLE t(x); CREATE INDEX tx ON t(x); INSERT INTO t VALUES(1),(2);')
        check(not d.execute("SELECT 1 FROM sqlite_schema WHERE name='sqlite_stat1'").fetchall(), 'no stats')
        check(open_db(os.fsencode(path), C.byref(db)) == 0, 'fresh open')
        stmt, result = prepared(db, 'ANALYZE', kind)
        check(result == (0, b'ANALYZE', 7, 0, 0) and calls() == 2, 'missing-stat1 original prepare')
        check(get_skip() == b'rewritten_prepare_failed' and not applied(), 'fresh fail-open telemetry')
        check(step(stmt) == 101 and finalize(stmt) == 0, 'first ANALYZE succeeds')
        check(d.execute('SELECT * FROM sqlite_stat1').fetchall() == [('t', 'tx', '2 1')], 'first stats created')
        check(close_db(db) == 0, 'fresh close'); d.close()

for value in (None, '', '1', '00', ' 0', 'false', '0'):
    env = os.environ.copy()
    if value is None: env.pop(GATE, None)
    else: env[GATE] = value
    env.update(SQLITE3_DISABLE_AUTOPRAGMA='1', SQLITE3_DISABLE_RUNTIME_OPTIMIZE='1',
               SQLITE3_DISABLE_PLEX_FTS_REWRITE='1', SQLITE3_ANALYSIS_LIMIT='400')
    subprocess.run([sys.executable, __file__, SO, '--gate'], env=env, check=True)
print(json.dumps({'status': 'PASS', 'hits': len(hits), 'misses': len(misses), 'assertions': h.checks}))
