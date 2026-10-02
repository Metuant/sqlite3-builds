"""Exact related-count bytes, ordered typed rows, plan, gates and fail-open.

Run through plex_private_rewrites_test.sh against real SQLite and production C.
"""
import os
import sys

GATE = 'SQLITE3_DISABLE_PLEX_RELATED_TAG_ORDER_REWRITE'
gate_worker = '--gate' in sys.argv
if not gate_worker:
    os.environ[GATE] = '0'
import plex_prepare_test_support as h
from plex_prepare_test_support import *

HEAD = ('select taggings.id, count(*) as taggings_count from tags'
        '   join taggings on taggings.tag_id=tags.id'
        '   join taggings as related on taggings.tag_id=related.tag_id'
        '   join metadata_items on metadata_items.id=related.metadata_item_id  where ')
REPLACEMENT = ('select taggings.id, count(*) as taggings_count from taggings'
               ' cross join tags on taggings.tag_id=tags.id'
               ' cross join taggings as related on taggings.tag_id=related.tag_id'
               ' cross join metadata_items on metadata_items.id=related.metadata_item_id  where ')
BODY = ("taggings.metadata_item_id={item} and metadata_items.library_section_id={section}"
        "  and tags.tag != ''  and tags.tag_type not in ({excluded}) group by taggings.id")
Q = HEAD + BODY.format(item='1459870', section='11', excluded='313, 312') + ';'
miss = signature('test_related_miss', C.c_char_p)
applied = signature('test_related_applied', C.c_int)
other_applied = signature('test_other_applied', C.c_int)


def prepared(db, sql, kind=1, nbyte=-1):
    data = sql.encode() if isinstance(sql, str) else sql
    buf = C.create_string_buffer(data)
    stmt, tail = ptr(), ptr()
    rc = prepare(db, C.addressof(buf), nbyte, 1 if kind == 2 else 0,
                 C.byref(stmt), C.byref(tail), kind)
    result = (rc, sql_text(stmt) if stmt else None,
              tail.value-C.addressof(buf) if tail.value else None,
              bind_count(stmt) if stmt else None,
              column_count(stmt) if stmt else None)
    if stmt:
        finalize(stmt)
    return result


with tempfile.TemporaryDirectory() as tmp:
    path = Path(tmp)/'com.plexapp.plugins.library.db'
    d = sqlite3.connect(path)
    d.executescript('''
      CREATE TABLE metadata_items(id INTEGER PRIMARY KEY,library_section_id INTEGER);
      CREATE TABLE tags(id INTEGER PRIMARY KEY,tag TEXT,tag_type INTEGER);
      CREATE TABLE taggings(id INTEGER PRIMARY KEY,metadata_item_id INTEGER,tag_id INTEGER);
      CREATE INDEX index_taggings_on_metadata_item_id ON taggings(metadata_item_id);
      CREATE INDEX idx_dshadow_taggings_tag_id_metadata_item_id ON taggings(tag_id,metadata_item_id);
      INSERT INTO metadata_items VALUES(1459870,11),(638005,10),(1,11),(2,10);
      INSERT INTO tags VALUES(1,'Drama',1),(2,'Collection',2),(3,'',9),
        (4,'excluded 313',313),(5,'excluded 312',312),(6,'only section A',1);
      INSERT INTO taggings VALUES(10,1459870,1),(11,1459870,2),(12,1459870,3),
        (13,1459870,4),(14,1459870,5),(15,1459870,6),(16,1459870,1),
        (20,1,1),(21,1,2),(22,638005,1),(23,2,2);
    ''')
    d.executemany('INSERT INTO metadata_items VALUES(?,?)', [(i, 10+i%2) for i in range(100, 500)])
    d.executemany('INSERT INTO taggings(metadata_item_id,tag_id) VALUES(?,?)',
                  [(i, tag) for i in range(100, 500) for tag in (3, 4, 5)])
    d.execute('ANALYZE'); d.commit()
    db = ptr(); check(open_db(os.fsencode(path), C.byref(db)) == 0, 'open')

    if gate_worker:
        enabled = os.environ.get(GATE) == '0'
        r = run(db, Q, typed=True)
        check(r[0] == 101 and r[1] == (Q.replace(HEAD, REPLACEMENT) if enabled else Q), 'gate text')
        check(applied() == int(enabled) and miss() == b'' and other_applied() == 0,
              'gate silence; every other enabled matcher misses related counts')
        check(close_db(db) == 0, 'close'); d.close()
        print('PASS: related gate', repr(os.environ.get(GATE)))
        sys.exit(0)

    cases = [
        ('?', '?', '?, ?', (1459870, 11, 313, 312)),
        ('?', '?', '?, ?', (1459870, 10, 313, 312)),
        ('1459870', '11', '313, 312', ()),
        ('?1', '?2', '?3, ?4', (1459870, 11, 313, 312)),
        ('?3', '?', '?1, ?2', (313, 312, 1459870, 11)),
        ('1459870', '11', '?2, ?2', (None, 313)),
        ('1459870', '11', '313', ()),
        ('1459870', '11', '313, 312, 9', ()),
        ('1459870', '11', ', '.join(str(i) for i in range(300, 316)), ()),
    ]
    for item, section, excluded, params in cases:
        for suffix in ('', ';', '; \t\r\n', ' \t\n'):
            sql = HEAD + BODY.format(item=item, section=section, excluded=excluded) + suffix
            for kind in (0, 1, 2):
                for nbyte in (-1, len(sql), len(sql)+1):
                    set_path(0); before = run(db, sql, params, kind, nbyte, typed=True)
                    set_path(1); after = run(db, sql, params, kind, nbyte, typed=True)
                    expected = before[1].replace(HEAD, REPLACEMENT)
                    check(after[0] == 101 and after[1] == expected, ('exact bytes', sql, after))
                    check(after[2:] == before[2:], ('ordered typed rows, bind names, tail', sql))
                    check(applied() == 1 and miss() == b'', 'hit telemetry')
    # Do not merely compare two empty outputs; duplicate taggings and section
    # exclusion are observable in both projected columns.
    check(d.execute(Q).fetchall() == [(10, 3), (11, 2), (15, 1), (16, 3)], 'section A rows')
    check(d.execute(Q.replace('section_id=11', 'section_id=10')).fetchall() ==
          [(10, 1), (11, 1), (16, 1)], 'section B omits unshared tag')

    bad = [
        (Q.replace(' group by', ' and tags.id>0 group by'), 'filter'),
        (Q.replace("!= ''", '!= ""'), 'dqs'),
        (Q.replace('313, 312', '313, /* comment */ 312'), 'comment'),
        (Q + ' select 1', 'trailing'),
        (Q.replace('tags   join', 'tags  join'), ''),
        (Q.replace('1459870', ':item'), 'value'),
        (Q.replace('section_id=11', 'section_id=-11'), 'value'),
        (Q.replace('1459870', '?01'), 'value'),
        (Q.replace('1459870', '?0'), 'value'),
        (Q.replace('313, 312', ''), 'not_in'),
        (Q.replace('313, 312', '313,312'), 'not_in'),
        (Q.replace('313, 312', ', '.join(str(i) for i in range(17))), 'not_in'),
        (Q.replace('313, 312', ':excluded'), 'not_in'),
        (Q.replace("!= ''", "<> ''"), 'filter'),
        (Q + '-- comment', 'comment'),
        (Q + ';', 'trailing'),
        (Q + ' order by taggings.id', 'trailing'),
        (Q.encode() + b'\0junk', ''),
        (Q + ' '*65536, ''),
    ]
    limit = signature('sqlite3_limit', C.c_int, ptr, C.c_int, C.c_int)
    previous = limit(db, 9, 64)
    bad += [(Q.replace('1459870', '?65'), 'value'),
            (Q.replace('1459870', '?64').replace('section_id=11', 'section_id=?'), 'value')]
    for sql, reason in bad:
        for kind in (0, 1, 2):
            nbyte = len(sql) if isinstance(sql, bytes) else -1
            set_path(0); before = prepared(db, sql, kind, nbyte)
            set_path(1); after = prepared(db, sql, kind, nbyte)
            check(after == before, ('miss preserves SQLite result/text/tail', sql, after, before))
            check(miss().decode() == reason and applied() == 0, ('sub_reason', sql, miss()))
    limit(db, 9, previous)
    for n in range(len(Q)):
        set_path(0); before = prepared(db, Q, nbyte=n)
        set_path(1); after = prepared(db, Q, nbyte=n)
        check(after[0] == before[0] and after[2:] == before[2:], ('bounded prefix', n))
        if after[1]:
            check(after[1] in (before[1], before[1].replace(HEAD.encode(), REPLACEMENT.encode())), 'bounded text')
    for fault, reason in ((1, b'rewritten_prepare_failed'), (2, b'tail_mismatch'),
                          (3, b'column_count_mismatch'), (4, b'bind_count_mismatch')):
        set_fault(fault); after = run(db, Q); set_fault(0)
        check(after[0] == 101 and after[1] == Q and get_skip() == reason and not applied(),
              ('candidate failure retries original', fault, get_skip()))

    # The spec permits asserting the fixed rewritten order when the synthetic
    # cost model does not put tags last. Assert all four SEARCH/SCAN positions.
    plans = [[row[3] for row in d.execute('EXPLAIN QUERY PLAN '+sql)]
             for sql in (Q, Q.replace(HEAD, REPLACEMENT))]
    order = [line.split()[1] for line in plans[1] if line.startswith(('SEARCH ', 'SCAN '))]
    check(order == ['taggings', 'tags', 'related', 'metadata_items'], ('join order', plans))
    print(json.dumps({'plan_check': 'fixed rewritten order', 'original': plans[0], 'rewritten': plans[1]}))

    # Reuse the public smoke SQL corpus (including FTS/GUID/taggings/On-Deck/
    # hub), plus every private matcher head. No schema is needed to prove the
    # related matcher silently rejects these disjoint heads before preparation.
    import ast
    sources = [Path(__file__).with_name('plex_fts_rewrite_smoke.c')]
    sources += list((Path(__file__).parent.parent/'src').glob('plex_*rewrite.h'))
    foreign = ['ANALYZE', "select id from fts4_tag_titles_icu where tag match '(a* a*)'"]
    for source in sources:
        for literal in re.findall(r'"(?:select |SELECT |ANALYZE|WITH |with )(?:[^"\\\r\n]|\\.)*"', source.read_text()):
            value = ast.literal_eval(literal)
            if value.lower().startswith(('select ', 'analyze', 'with ')):
                foreign.append(value)
    check(len(foreign) > 80, 'existing matcher corpus found')
    for sql in foreign:
        prepared(db, sql)
        check(miss() == b'' and applied() == 0, ('related matcher misses existing shape', sql))
    check(close_db(db) == 0, 'close'); d.close()

    for filename in ('other.db', 'prefixcom.plexapp.plugins.library.db'):
        check(open_db(os.fsencode(Path(tmp)/filename), C.byref(db)) == 0, 'non-target open')
        prepared(db, Q)
        check(miss() == b'' and not applied(), 'target gate')
        check(close_db(db) == 0, 'non-target close')

# Enable all other private/public matchers in each fresh gate process: none
# may shadow this statement, even with the new gate disabled.
for value in (None, '', '1', '00', ' 0', 'false', '0'):
    env = os.environ.copy()
    if value is None: env.pop(GATE, None)
    else: env[GATE] = value
    for name in ('FTS', 'GUID_LIKE', 'TAGGINGS', 'ONDECK', 'HUB_TYPE_OR', 'FUZZY_MATCH', 'APP_ANALYZE'):
        env['SQLITE3_DISABLE_PLEX_'+name+'_REWRITE'] = '0'
    for name in ('FTS_JOIN', 'TITLE_SCOPE', 'COLLECTION_TAG', 'RECENT_EPISODE'):
        env['SQLITE3_ENABLE_PLEX_'+name+'_REWRITE'] = '1'
    env.update(SQLITE3_DISABLE_AUTOPRAGMA='1', SQLITE3_DISABLE_RUNTIME_OPTIMIZE='1')
    subprocess.run([sys.executable, __file__, SO, '--gate'], env=env, check=True)
env = os.environ.copy()
for name in ('FTS', 'GUID_LIKE', 'TAGGINGS', 'ONDECK', 'HUB_TYPE_OR', 'FUZZY_MATCH', 'APP_ANALYZE'):
    env['SQLITE3_DISABLE_PLEX_'+name+'_REWRITE'] = '1'
for name in ('FTS_JOIN', 'TITLE_SCOPE', 'COLLECTION_TAG', 'RECENT_EPISODE'):
    env['SQLITE3_ENABLE_PLEX_'+name+'_REWRITE'] = '0'
env[GATE] = '0'
subprocess.run([sys.executable, __file__, SO, '--gate'], env=env, check=True)
print(json.dumps({'status': 'PASS', 'related_assertions': h.checks,
                  'positive_variants': len(cases)*4*3*3, 'negative_shapes': len(bad),
                  'existing_matcher_heads': len(foreign)}))
