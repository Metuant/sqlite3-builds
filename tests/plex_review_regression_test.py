"""Review findings 1, 2, 5, 6; real prepare/step on the production wrapper adapter."""
import os
for mode in ('FTS_JOIN', 'TITLE_SCOPE', 'COLLECTION_TAG', 'RECENT_EPISODE'):
    os.environ['SQLITE3_ENABLE_PLEX_' + mode + '_REWRITE'] = '1'
import plex_prepare_test_support as h
from plex_prepare_test_support import *

COLL = C.CFUNCTYPE(C.c_int, ptr, C.c_int, ptr, C.c_int, ptr)
@COLL
def compare(_, na, a, nb, b):
    a, b = C.string_at(a, na), C.string_at(b, nb)
    return (a > b) - (a < b)
coll = signature('sqlite3_create_collation', C.c_int, ptr, C.c_char_p, C.c_int, ptr, COLL)
exec_sql = signature('sqlite3_exec', C.c_int, ptr, C.c_char_p, ptr, ptr, ptr)
index = 'idx_dshadow_taggings_tag_id_metadata_item_id'
join = query()
scope = ("select metadata_items.id from metadata_items join fts4_metadata_titles_icu "
         "on metadata_items.id=fts4_metadata_titles_icu.rowid left join taggings "
         "on taggings.metadata_item_id=metadata_items.id left join tags on taggings.tag_id=tags.id "
         "where fts4_metadata_titles_icu.title match 'Th*' and metadata_items.metadata_type=1 "
         "and tags.id=1 order by metadata_items.title_sort collate icu_root asc,"
         "metadata_items.originally_available_at asc,metadata_items.id asc limit 100 offset 0")
collection = ('select tags.id,metadata_items.id,metadata_items.absolute_index from tags '
              'join taggings on taggings.tag_id=tags.id join metadata_items on '
              'metadata_items.id=taggings.metadata_item_id where library_section_id=5 and tag_type=2')
recent = ("select metadata_items.id from metadata_items join metadata_items as children on children.parent_id=metadata_items.id "
          "join metadata_items as grandchildren on grandchildren.parent_id=children.id where metadata_items.library_section_id in (5) "
          "and (metadata_items.title NOT LIKE 'Exclude' and metadata_items.id not in (select taggings.metadata_item_id from taggings "
          "join tags on tags.id=taggings.tag_id where tags.id=10) and grandchildren.added_at>0 and "
          "(metadata_items.metadata_type=2 or metadata_items.metadata_type=10)) order by metadata_items.title_sort collate icu_root limit 10 offset 0")
with tempfile.TemporaryDirectory() as tmp:
    path = Path(tmp) / 'com.plexapp.plugins.library.db'
    db = ptr()
    check(open_db(os.fsencode(path), C.byref(db)) == 0, 'open')
    check(coll(db, b'icu_root', 1, None, compare) == 0, 'collation')
    def ddl(s): check(exec_sql(db, s.encode(), None, None, None) == 0, s)
    ddl('''CREATE TABLE tags(id INTEGER PRIMARY KEY,tag_type INTEGER,tag TEXT);
      CREATE TABLE metadata_items(id INTEGER PRIMARY KEY,library_section_id INTEGER,
        metadata_type INTEGER,title TEXT,title_sort TEXT,originally_available_at,absolute_index,
        parent_id INTEGER,added_at INTEGER);
      CREATE TABLE taggings(tag_id INTEGER,metadata_item_id INTEGER);
      CREATE INDEX idx_dshadow_taggings_tag_id_metadata_item_id ON taggings(tag_id,metadata_item_id);
      CREATE VIRTUAL TABLE fts4_tag_titles_icu USING fts4(tag);
      CREATE VIRTUAL TABLE fts4_metadata_titles_icu USING fts4(title);
      INSERT INTO tags VALUES(1,1,'Theater');
      INSERT INTO metadata_items VALUES(1,1,1,'Th*','same',NULL,11,NULL,0);
      INSERT INTO taggings VALUES(1,1);
      INSERT INTO fts4_tag_titles_icu(rowid,tag) VALUES(1,'Theater');
      INSERT INTO fts4_metadata_titles_icu(rowid,title) VALUES(1,'Theater');''')
    # Finding 1: exact review query (whitespace is accepted by this matcher).
    dqs = join.replace("'Th*'", '"title"')
    set_path(0); original = run(db, dqs)
    set_path(1); candidate = run(db, dqs)
    check(original[0] == 101 and original[2] == [(1,)], 'DQS original returns row')
    check(raw_build(dqs) is None and candidate[1] == dqs and candidate[2] == original[2], 'DQS unchanged')
    # Reconstruct the old forced order to prove the positive-original/step-error premise.
    old = raw_build(join).decode().replace("'Th*'", '"title"')
    set_path(0); check(run(db, old)[0] == 1, 'old FTS-first DQS step error'); set_path(1)
    for scalar in [query(tag='tag_type="title"'), query(sections='"title"'),
                   query(kind='"title"'), query(limit='"title"')]:
        check(raw_build(scalar) is None, 'all FTS-join scalar slots reject DQS')

    # Finding 2: prepare BOTH idle statements, drop index on same connection, step.
    for text in (join, scope, collection):
        if text == collection:
            ddl('UPDATE tags SET tag_type=2; UPDATE metadata_items SET library_section_id=5')
        for kind in (1, 2):
            statements = []
            for enabled in (0, 1):
                set_path(enabled); buf = C.create_string_buffer(text.encode()); stmt, tail = ptr(), ptr()
                check(prepare(db, C.addressof(buf), -1, 0, C.byref(stmt), C.byref(tail), kind) == 0, 'prepare before drop')
                generated = sql_text(stmt).decode()
                check(('CROSS JOIN' in generated) == bool(enabled), 'rewrite hit before drop')
                check('INDEXED BY' not in generated, 'no forced index')
                statements.append(stmt)
            ddl('DROP INDEX ' + index)
            rows = []
            for stmt in statements:
                bag = []
                while True:
                    rc = step(stmt)
                    if rc != 100: break
                    bag.append(h.typed_row(stmt))
                check(rc == 101 and bag, 'step after schema invalidation')
                rows.append(bag); finalize(stmt)
            check(rows[0] == rows[1], 'post-drop complete rows')
            ddl('CREATE INDEX ' + index + ' ON taggings(tag_id,metadata_item_id)')

    # Finding 5: malformed numeric tokens must stay prepare errors, including UTF-8.
    build_recent = signature('test_recent_build', ptr, ptr, C.c_size_t, C.c_size_t, C.POINTER(C.c_size_t), C.POINTER(C.c_size_t))
    h.build = build_recent
    for bad in [recent.replace('>0 and', '>0AND'), recent.replace('=2 or', '=2OR'),
                recent.replace('>0 and', '>0é and'), recent.replace('>0 and', '>0$ and')]:
        check(raw_build(bad) is None, 'numeric-token miss')
        for enabled in (0, 1):
            set_path(enabled); check(run(db, bad)[0] == 1, 'original prepare error preserved')

    # Finding 6: left BINARY collation must not become tags.id's NOCASE.
    ddl('DROP TABLE taggings; DROP TABLE tags; CREATE TABLE tags(id TEXT COLLATE NOCASE);'
        'CREATE TABLE taggings(tag_id TEXT COLLATE BINARY,metadata_item_id INTEGER);'
        'CREATE INDEX ' + index + ' ON taggings(tag_id,metadata_item_id);'
        "INSERT INTO tags VALUES('a'); INSERT INTO taggings VALUES('A',1)")
    text = scope.replace('tags.id=1', "tags.id='a'")
    set_path(0); original = run(db, text)
    set_path(1); candidate = run(db, text)
    check(original[0] == candidate[0] == 101 and original[2] == candidate[2] == [], 'collation parity')
    check('ON taggings.tag_id=tags.id' in candidate[1], 'original equality direction')
    check(close_db(db) == 0, 'close')
print(json.dumps({'status': 'PASS', 'review_findings': [1, 2, 5, 6], 'assertions': h.checks}))
