#!/usr/bin/env python3
"""Fixture-only Plex collating parity/bench in fresh, network-disabled containers.

Usage: python3 tests/plex_fuzzy_match_native.py ARTIFACT WORK_DIRECTORY PLEX_IMAGE
The library is mounted read-only; WORK_DIRECTORY must be a fresh directory.
No production containers, databases or configuration are accessed.
"""
import json
import re
import statistics
import subprocess
import sys
from pathlib import Path

root = Path(__file__).resolve().parents[1]
artifact, work, image = Path(sys.argv[1]).resolve(), Path(sys.argv[2]).resolve(), sys.argv[3]
work.mkdir()  # Deliberately refuse to reuse a fixture directory.
# Plex SQLite is a launcher into PMS's embedded shell. That shell checks its
# baked source-id before registering the native collating tokenizer. Patch only
# a private executable copy, exactly as the sideload compatibility check does;
# never edit an image, running container, installed executable or version pin.
vanilla = subprocess.run(['docker','run','--rm','-i','--network','none','--entrypoint',
    '/usr/lib/plexmediaserver/Plex SQLite',image,':memory:'],
    input='select sqlite_source_id();\n',text=True,capture_output=True,check=True).stdout.strip()
pins=dict(line.split('=',1) for line in (root/'pins/versions.env').read_text().splitlines() if '=' in line)
source_id=pins['SQLITE_SOURCE_ID'].replace('%20',' ')
cid=subprocess.check_output(['docker','create','--entrypoint','/bin/true',image],text=True).strip()
try:
    subprocess.run(['docker','cp',cid+':/usr/lib/plexmediaserver/Plex Media Server',
                    str(work/'Plex Media Server')],check=True)
finally:
    subprocess.run(['docker','rm',cid],check=True,stdout=subprocess.DEVNULL)
pms=work/'Plex Media Server'
data=pms.read_bytes()
assert len(vanilla)==len(source_id)==84 and data.count(vanilla.encode())==1, 'source-id patch guard'
pms.write_bytes(data.replace(vanilla.encode(),source_id.encode()))
corpus = [line.split('\t', 1)[1] for line in
          (root/'docs/specs/2026-09-12-plex-search-match-literals-corpus.tsv').read_text().splitlines()]
goldens = dict(line.split('\t', 1) for line in
               (root/'tests/fixtures/plex-fuzzy-match/golden.tsv').read_text().splitlines())
base = ['docker', 'run', '--rm', '-i', '--network', 'none',
        '--entrypoint', '/usr/lib/plexmediaserver/Plex SQLite',
        '-v', f'{artifact}:/usr/lib/plexmediaserver/lib/libsqlite3.so:ro',
        '-v', f'{pms}:/usr/lib/plexmediaserver/Plex Media Server:ro',
        '-v', f'{work}:/fixture', '-w', '/fixture',
        '-e', 'SQLITE3_DISABLE_AUTOPRAGMA=1', '-e', 'SQLITE3_DISABLE_PLEX_FTS_REWRITE=1']

def shell(script, gate, filename, obs=False):
    cmd = base + ['-e', f'SQLITE3_DISABLE_PLEX_FUZZY_MATCH_REWRITE={gate}',
                  '-e', 'SQLITE3_DISABLE_OBSERVABILITY='+('0' if obs else '1'),
                  image, filename]
    result = subprocess.run(cmd, input='.bail on\n.headers off\n.mode list\n'+script,
                            text=True, capture_output=True)
    if result.returncode:
        raise AssertionError((cmd, result.stdout[-4000:], result.stderr[-4000:]))
    return result

def select(literal, table='fts4_tag_titles_icu', col='tag'):
    return f"SELECT rowid FROM {table} WHERE {table}.{col} match '{literal}'"

docs = []
for literal in goldens:
    tokens = re.sub(r'[()*]', '', literal).replace("''", "'").split()
    # Include OR even for OR-form seeds: star-form OR* is a real required term.
    docs.append(' '.join(dict.fromkeys(tokens)))
docs += ['the office', 'star wars', 'harry potter and', 'unrelated zebra']
all_words = ' '.join(dict.fromkeys(' '.join(docs).split()))
docs.insert(0, all_words)
records = []
parity_cases = 0
for prefix in (False, True):
    label = 'prefix23' if prefix else 'plain'
    folder = work/label; folder.mkdir()
    filename = label+'/com.plexapp.plugins.library.db'
    option = ", prefix='2,3'" if prefix else ''
    setup = "SELECT 'sqlite|'||sqlite_version();\n"
    setup += f"CREATE VIRTUAL TABLE fts4_tag_titles_icu USING fts4(tag, tokenize=collating 'root@colStrength=primary;colAlternate=shifted'{option});\n"
    setup += f"CREATE VIRTUAL TABLE fts4_metadata_titles_icu USING fts4(title,title_sort,original_title, tokenize=collating 'root@colStrength=primary;colAlternate=shifted'{option});\n"
    setup += 'BEGIN;\n'
    for i, doc in enumerate(docs, 1):
        escaped = doc.replace("'", "''")
        setup += f"INSERT INTO fts4_tag_titles_icu(rowid,tag) VALUES({i},'{escaped}');\n"
    # Broad doclists with selective intersections; wholly synthetic, reproducible.
    setup += "WITH RECURSIVE n(x) AS (VALUES(1) UNION ALL SELECT x+1 FROM n WHERE x<24000) INSERT INTO fts4_tag_titles_icu(rowid,tag) SELECT 1000+x,(SELECT tag FROM fts4_tag_titles_icu WHERE rowid=2+x%"+str(len(docs)-1)+")||' filler'||x FROM n;\n"
    setup += 'INSERT INTO fts4_metadata_titles_icu(rowid,title,title_sort,original_title) SELECT rowid,tag,tag,tag FROM fts4_tag_titles_icu;\nCOMMIT;\n'
    result=shell(setup,'1',filename)
    assert 'sqlite|3.53.3' in result.stdout, result.stdout
    for i, literal in enumerate(corpus, 1):
        if literal.startswith('(') and literal not in goldens:
            records.append(dict(corpus_row=i,table=label,status='truncated: grammar rejection'))
            continue
        rewritten=goldens.get(literal,literal)
        checks=''
        for table,col in [('fts4_tag_titles_icu','tag'),
                          *[('fts4_metadata_titles_icu',c) for c in ('title','title_sort','original_title')]]:
            a,b=select(literal,table,col),select(rewritten,table,col)
            checks += f"SELECT 'parity|'||(SELECT count(*) FROM ({a} EXCEPT {b}))||'|'||(SELECT count(*) FROM ({b} EXCEPT {a}))||'|'||(SELECT count(*) FROM ({a}));\n"
        result=shell(checks,'1',filename)
        counts=[]
        for line in result.stdout.splitlines():
            if line.startswith('parity|'):
                _,a,b,hits=line.split('|'); assert a==b=='0', (literal,label,line)
                counts.append(int(hits)); parity_cases+=1
        assert len(counts)==4
        if literal in goldens: assert min(counts)>0, (literal,'vacuous parity')
        # The actual public wrapper must execute the same corpus query enabled.
        a=select(literal)
        enabled=shell(f'SELECT count(*) FROM ({a});\n','0',filename,obs=True)
        assert int(enabled.stdout.strip())==counts[0]
        if literal in goldens:
            assert 'event=rewrite_applied target=plex mode=fuzzy+match_canon' in enabled.stderr, enabled.stderr
            assert rewritten in enabled.stderr, (literal, enabled.stderr)
        else: assert 'event=rewrite_applied target=plex mode=fuzzy+match_canon' not in enabled.stderr
        (folder/f'corpus-{i:02d}.log').write_text(enabled.stderr)
        timings=[]
        for text in (literal,rewritten):
            query=f'SELECT count(*) FROM ({select(text)});\n'
            result=shell(query+'.timer on\n'+query*9+'.timer off\n','1',filename)
            samples=[float(m.group(1))*1000 for m in re.finditer(r'Run Time: real ([0-9.]+)',result.stdout)]
            assert len(samples)==9, result.stdout
            timings.append(statistics.median(samples))
        records.append(dict(corpus_row=i,table=label,status='accepted' if literal in goldens else 'bare/noop',
                            hits=counts[0],original_ms=timings[0],rewritten_ms=timings[1]))
        print(json.dumps(records[-1]),flush=True)
(work/'results.json').write_text(json.dumps(dict(status='PASS',parity_cases=parity_cases,
    accepted_literals=len(goldens),records=records),indent=2)+'\n')
print(json.dumps(dict(status='PASS',parity_cases=parity_cases,accepted_literals=len(goldens),
    tokenizer="collating root@colStrength=primary;colAlternate=shifted",rows_per_table=24000+len(docs))))
