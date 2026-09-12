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
bind_double = signature('sqlite3_bind_double', C.c_int, ptr, C.c_int, C.c_double)
bind_blob = signature('sqlite3_bind_blob', C.c_int, ptr, C.c_int, ptr, C.c_int, ptr)
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
            elif isinstance(value, float): rc = bind_double(stmt, i, value)
            elif isinstance(value, bytes):
                blob = C.create_string_buffer(value);rc = bind_blob(stmt, i, C.addressof(blob), len(value), ptr(-1))
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

