"""Spec sections 3-6: real prepare adapter, corpus goldens and fail-open checks."""
import os
import sys
gate_worker = '--gate' in sys.argv
if not gate_worker:
    os.environ['SQLITE3_DISABLE_PLEX_FUZZY_MATCH_REWRITE'] = '0'
os.environ['SQLITE3_DISABLE_PLEX_FTS_REWRITE'] = '1' if gate_worker else '0'
os.environ['SQLITE3_DISABLE_PLEX_TAGGINGS_REWRITE'] = '1'
os.environ['SQLITE3_ENABLE_PLEX_TITLE_SCOPE_REWRITE'] = '1'
import plex_prepare_test_support as h
from plex_prepare_test_support import *
h.build = signature('test_fuzzy_build', ptr, ptr, C.c_size_t, C.c_size_t,
                    C.POINTER(C.c_size_t), C.POINTER(C.c_size_t))
reason = signature('test_fuzzy_reason', C.c_char_p)
miss = signature('test_fuzzy_miss', C.c_char_p)
applied = signature('test_fuzzy_applied', C.c_int)
ROOT = Path(__file__).resolve().parents[1]
CORPUS = ROOT/'docs/specs/2026-09-12-plex-search-match-literals-corpus.tsv'
GOLDEN = ROOT/'tests/fixtures/plex-fuzzy-match/golden.tsv'

def sql(lit, table='fts4_tag_titles_icu', col='tag'):
    return f"select rowid from {table} where {table}.{col} match '{lit}'"

with tempfile.TemporaryDirectory() as tmp:
    path = Path(tmp)/'com.plexapp.plugins.library.db'
    d = seed(path)
    d.executescript('CREATE VIRTUAL TABLE fts4_metadata_titles_icu USING fts4(title,title_sort,original_title);')
    db = ptr(); check(open_db(os.fsencode(path), C.byref(db)) == 0, 'open')
    if gate_worker:
        q = sql('(fault* fault*)')
        r = run(db,q)
        enabled = os.environ.get('SQLITE3_DISABLE_PLEX_FUZZY_MATCH_REWRITE') == '0'
        check(r[1] == (sql('(fault*)') if enabled else q), 'literal gate')
        check(applied() == int(enabled) and miss() == b'', 'disabled silence')
        check(close_db(db)==0,'close'); d.close()
        print('PASS: fuzzy gate '+repr(os.environ.get('SQLITE3_DISABLE_PLEX_FUZZY_MATCH_REWRITE')))
        sys.exit(0)
    goldens = dict(line.split('\t',1) for line in GOLDEN.read_text().splitlines())
    accepted = rejected = 0
    for line in CORPUS.read_text().splitlines():
        _, lit = line.split('\t',1)
        q = sql(lit); out = raw_build(q)
        if lit in goldens:
            accepted += 1
            expected = sql(goldens[lit])
            check(out == expected.encode(), ('golden',lit,out))
            check(raw_build(expected) is None and reason()==b'noop','idempotence')
            for kind in (0,1,2):
                for n in (-1,len(q.encode()),len(q.encode())+1):
                    r=run(db,q,kind=kind,nbyte=n,flags=1 if kind==2 else 0)
                    check(r[1]==expected and r[4]==len(q.encode()),'API text/tail')
                    check(applied()==1, 'applied exactly once')
        else:
            rejected += 1
            check(out is None and reason() in (b'no_match',b'grammar'),'corpus rejected')
            r=run(db,q); check(r[1]==q,'rejected original prepared unchanged')
    adversarial = {
        '(NEAR a)': 'keyword', '(NOT a)': 'keyword', '(AND a)': 'keyword',
        '(NEAR/2 a)': 'keyword', '("phrase")':'grammar', '(title:term)':'grammar',
        '(^first)':'grammar', '((a))':'grammar', '(a':'grammar', '()':'empty_group',
        '(OR a)':'or_layout', '(a OR)':'or_layout', '(a OR OR b)':'or_layout',
        '(a b OR c)':'or_layout', '(a OR b c)':'or_layout', "(a'b)":'quote',
        'a* b*':'grammar', '(a  a)':'grammar', ' (a a)':'grammar', '(a a) ':'grammar',
        '(a\ta)':'grammar', '(a**)':'grammar', '(há OR ha*)':'noop',
        '(a* a)':'noop', '(A OR a*)':'noop', '(a* OR a*)':'changed',
    }
    for lit, why in adversarial.items():
        q=sql(lit); out=raw_build(q)
        if why=='changed': check(out==sql('(a*)').encode(),'OR dedupe'); continue
        check(out is None and reason().decode()==why,('adversarial reason',lit,reason()))
        r=run(db,q)
        if r[1]: check(r[1]==q,'adversarial original unchanged')
        check(miss().decode()==why and not applied(),('capture reason',lit,miss()))
    for q, why in [
        (sql('(a a)')+' and fts4_tag_titles_icu.tag match ?', 'multi_match'),
        (sql('(a a)').replace("'(a a)'",'?'), 'bind_operand'),
        (sql('(a a)')+' -- comment','comment'),
        (sql('(a a)')+' /* comment */','comment'),
        (sql('(a a)').replace('select rowid','select "rowid"'),'dqs'),
        ('select 1','no_match'),
        (sql('(a a)').replace('select rowid','select offsets(fts4_tag_titles_icu)'), 'grammar'),
        (sql('(a a)').replace('select rowid','select matchinfo(fts4_tag_titles_icu)'), 'grammar'),
        (sql('(a a)').replace('select rowid','select snippet(fts4_tag_titles_icu)'), 'grammar'),
        (sql('(a a)').replace('select rowid','select rank(fts4_tag_titles_icu)'), 'grammar'),
        (sql('(a a)').replace('fts4_tag_titles_icu.tag','other.tag'),'no_match'),
    ]:
        check(raw_build(q) is None and reason().decode()==why,(q,reason()))
        # Prepare only: a bound operand has not been supplied yet.
        buf=C.create_string_buffer(q.encode());st,tail=ptr(),ptr()
        prepare(db,C.addressof(buf),-1,0,C.byref(st),C.byref(tail),1)
        if st: check(sql_text(st).decode()==q,'SQL fail open'); finalize(st)
        check(miss().decode()==why and not applied(), 'SQL miss diagnostics')
    for col in ('title','title_sort','original_title'):
        q=sql('(a a)','fts4_metadata_titles_icu',col)
        check(run(db,q)[1]==q.replace('(a a)','(a)'), 'title columns')
    for lit, expected in [("(há há OR* OR*)","(há OR*)"),
                           ("(harry OR harry''s OR harry*)","(harry''s OR harry*)"),
                           ('(ab* abc* abcd*)','(abcd*)'),
                           ('(abcd* OR abc* OR ab*)','(ab*)'),
                           ('(a1 OR a12 OR a1*)','(a1*)'),
                           ('(AND* AND*) (NOT* NOT*) (NEAR* NEAR*)','(AND*) (NOT*) (NEAR*)'),
                           ('(OR* OR* ORbit*)*','(ORbit*)*')]:
        check(raw_build(sql(lit))==sql(expected).encode(),('byte/ASCII rules',lit))
    q=sql('(fault* fault*)')
    for fault in (1,2,3,4):
        set_fault(fault); r=run(db,q); set_fault(0)
        check(r[1]==q and not applied(),('verification fallback',fault,r))
        check(get_skip()=={1:b'rewritten_prepare_failed',2:b'tail_mismatch',
             3:b'column_count_mismatch',4:b'bind_count_mismatch'}[fault],'fallback reason')
    q=sql('(a a)').replace('select rowid','select rowid, ?8, :name, :name, ?, $x::y(z)')
    for kind in (0,1,2):
        r=run(db,q,(None,)*11,kind=kind,typed=True)
        check(r[1]==q.replace('(a a)','(a)') and len(r[3])==11,'binds and columns')
    for suffix in (b'\0junk',b'; select 1',b' '*65536):
        q=sql('(a a)').encode()+suffix
        check(run(db,q,nbyte=len(q))[1].encode()==q.split(b'\0')[0].split(b';')[0]+(b';' if b';' in suffix else b''),'bounded fail open')
    for suffix in (';', ';\n', '; \t\n'):
        q=sql('(a a)')+suffix
        r=run(db,q)
        check(r[1]==sql('(a)')+';' and r[4]==len(sql('(a a)'))+1,
              'single-statement semicolon and original tail mapping')
    # FTS join composes after canonicalization; index-missing continues into
    # legacy unlikely(tag_type=...). MATCH itself stays opaque to both.
    q=query(term="'(the OR theater OR the*)'")
    set_path(0); original=run(db,q)[2]; set_path(1)
    r=run(db,q)
    check('CROSS JOIN' in r[1] and "match '(the*)'" in r[1], 'join composition')
    check(set(original)==set(r[2]),'composed membership')
    # Movie title-scope composition, including opaque quoted identifiers in
    # other statement positions and native column-count verification.
    d.executescript("ALTER TABLE metadata_items ADD COLUMN title_sort TEXT;"
                   "ALTER TABLE metadata_items ADD COLUMN originally_available_at TEXT;"
                   "INSERT INTO fts4_metadata_titles_icu(rowid,title) SELECT id,'the office' FROM metadata_items;")
    coll=C.CFUNCTYPE(C.c_int,ptr,C.c_int,ptr,C.c_int,ptr)
    @coll
    def compare(_,na,a,nb,b):
        a,b=C.string_at(a,na),C.string_at(b,nb)
        return (a>b)-(a<b)
    register=signature('sqlite3_create_collation',C.c_int,ptr,C.c_char_p,C.c_int,ptr,coll)
    check(register(db,b'icu_root',1,None,compare)==0,'register fixture collation')
    title_q=("select metadata_items.id from metadata_items join fts4_metadata_titles_icu on metadata_items.id=fts4_metadata_titles_icu.rowid"
             " left join taggings on taggings.metadata_item_id=metadata_items.id left join tags on taggings.tag_id=tags.id"
             " where fts4_metadata_titles_icu.title match '(the OR the*)' and metadata_items.metadata_type=1 and tags.id=1"
             " order by metadata_items.title_sort collate icu_root asc,metadata_items.originally_available_at asc,metadata_items.id asc limit 101 offset 0")
    set_path(0);original=run(db,title_q)[2];set_path(1)
    title_r=run(db,title_q)
    check('CROSS JOIN' in title_r[1] and "match '(the*)'" in title_r[1], 'title-scope composition')
    check(original and set(original)==set(title_r[2]),'title-scope membership')
    for projection in ('`rowid`','[rowid]'):
        quoted=sql('(the the)').replace('select rowid','select '+projection)
        check(run(db,quoted)[1]==quoted.replace('(the the)','(the)'), 'opaque quoted identifiers')
    opaque=sql('(the the)')+" and 'fts4_tag_titles_icu.tag match ''(a a)''' IS NOT NULL"
    check(run(db,opaque)[1]==opaque.replace('(the the)','(the)'), 'MATCH inside SQL string is opaque')
    d.execute('DROP INDEX idx_dshadow_taggings_tag_id_metadata_item_id');d.commit()
    r=run(db,q)
    check('unlikely(tag_type=1)' in r[1] and "match '(the*)'" in r[1], 'fallback composition')
    check(close_db(db)==0,'close');d.close()
for value in (None,'','0','1','00','false',' 0'):
    env=os.environ.copy()
    if value is None: env.pop('SQLITE3_DISABLE_PLEX_FUZZY_MATCH_REWRITE',None)
    else: env['SQLITE3_DISABLE_PLEX_FUZZY_MATCH_REWRITE']=value
    env['SQLITE3_DISABLE_AUTOPRAGMA']='1'
    subprocess.run([sys.executable,__file__,SO,'--gate'],env=env,check=True)
print(json.dumps({'status':'PASS','accepted':accepted,'rejected':rejected,'assertions':h.checks}))
