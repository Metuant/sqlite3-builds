import csv, pathlib, subprocess
root=pathlib.Path(__file__).parent
pins=dict(line.split('=',1) for line in (root/'pins/versions.env').read_text().splitlines() if '=' in line and not line.startswith('#'))
with (root/'pins/library-compat-groups.tsv').open() as f:
    rows=list(csv.DictReader(f,delimiter='\t'))
row=next(r for r in rows if 'icu69' in r.values())
args={k:pins[k] for k in ['SQLITE_AMALG_URL','SQLITE_AMALG_SHA3_256','SQLITE_SRC_URL','SQLITE_SRC_SHA3_256','SQLITE_SOURCE_ID','MIMALLOC_VERSION','MIMALLOC_URL','MIMALLOC_SHA512','BASEIMAGE_ALPINE']}
args.update(LIBRARY_VARIANT='plex',BASE_IMAGE=pins['BASEIMAGE_ALPINE'],MARCH='x86-64-v3',ICU_SOURCE_VERSION=row['icu_source_version'],ICU_SOURCE_SHA512=row['icu_source_sha512'])
command=['docker','build','--progress=plain','-t','plex-collection-tagids-build:local','-f','docker-library/Dockerfile']
for k,v in args.items(): command+=['--build-arg',k+'='+v]
command+=['.']
print('Pinned build:',args,flush=True)
raise SystemExit(subprocess.call(command,cwd=root))
