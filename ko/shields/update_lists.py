#!/usr/bin/env python3
# Copyright (c) 2026 The Brave Authors. All rights reserved.
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this file,
# You can obtain one at https://mozilla.org/MPL/2.0/.
"""Refreshes the K-O embedded filter list snapshot.

Downloads the lists behind Brave's core catalog entries plus K-O extras over
HTTPS, normalises and lints them, and writes:

  lists/<id>.txt|json        normalised list data
  lists/manifest.json        source URL, sha256, size and date of every list
  ko_shields_resources.grd   grit file compiled into brave_resources.pak
  ko_embedded_lists.inc      id -> IDR table for the C++ loader

Usage:
  update_lists.py              fetch everything and rewrite the snapshot
  update_lists.py --local-only refresh only local/ lists, keep downloads
  update_lists.py --check      verify the committed snapshot offline
"""

import argparse
import datetime
import hashlib
import json
import os
import re
import ssl
import sys
import tempfile
import urllib.parse
import urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
LISTS_DIR = os.path.join(HERE, 'lists')
LOCAL_DIR = os.path.join(HERE, 'local')
MANIFEST_PATH = os.path.join(LISTS_DIR, 'manifest.json')
GRD_PATH = os.path.join(HERE, 'ko_shields_resources.grd')
INC_PATH = os.path.join(HERE, 'ko_embedded_lists.inc')

SCHEMA = 1
CHANNEL = 'ko-filter-lists'
USER_AGENT = 'k-o-update-lists/1'

CATALOG_URL = ('https://raw.githubusercontent.com/brave/adblock-resources/'
               'master/filter_lists/list_catalog.json')
RESOURCES_URL = ('https://raw.githubusercontent.com/brave/adblock-resources/'
                 'master/dist/resources.json')

# Brave catalog entries whose sources are embedded. Locked entries cannot be
# disabled in K-O; the rest follow the user's toggle in the list settings.
CATALOG_GROUPS = [
    # (catalog uuid, locked)
    ('default', True),
    ('4D715457-307C-4383-8873-73A2FD263F71', True),  # Default privacy
    ('E99CBD02-FFD1-4651-9BDD-6A9ED7B87819', True),  # First-party
    ('AC023D22-AE88-4060-A978-4FEEEC4221693', True),  # Cookie notices
    ('2F3DCE16-A19A-493C-A88F-2E110FBD37D6', False),  # Mobile app promos
    ('67E792D4-AE03-4D1A-9EDE-80E01C81F9B8', False),  # Fanboy annoyances
]

# Lists K-O adds on top of Brave's catalog. Always enabled.
EXTRA_LISTS = [
    {
        'id': 'peter-lowe',
        'title': "Peter Lowe's Ad and tracking server list",
        'url': ('https://pgl.yoyo.org/adservers/serverlist.php?'
                'hostformat=adblockplus&showintro=0&mimetype=plaintext'),
        'engine': 'default',
        'permission_mask': 0,
        'min_rules': 1000,
    },
]

LOCAL_LISTS = [
    {
        'id': 'ko-unbreak',
        'title': 'K-O unbreak',
        'file': 'ko-unbreak.txt',
        'engine': 'both',
        'permission_mask': 0,
    },
]

MAX_LIST_BYTES = 32 * 1024 * 1024
MAX_TOTAL_BYTES = 128 * 1024 * 1024
DEFAULT_MIN_RULES = 1
ID_RE = re.compile(r'^[a-z0-9][a-z0-9-]{0,63}$')

# Exception rules that would disable blocking everywhere. Mirrored by
# KoListHasUnsafeException() in the browser.
UNIVERSAL_REGEX_PATTERNS = ('/.*/', '/^/')
UNIVERSAL_SCHEMES = ('', 'http', 'https', 'ws', 'wss')


class UpdateError(Exception):
  pass


def sha256_hex(data):
  return hashlib.sha256(data).hexdigest()


def resource_name(list_id):
  return 'IDR_KO_SHIELDS_' + re.sub(r'[^A-Z0-9]', '_', list_id.upper())


def slug(text):
  return re.sub(r'[^a-z0-9]+', '-', text.lower()).strip('-')


def list_id_for_url(url, taken):
  """Stable, readable id derived from a list URL."""
  parsed = urllib.parse.urlparse(url)
  parts = [p for p in parsed.path.split('/') if p]
  base = slug(os.path.splitext(parts[-1])[0]) if parts else slug(parsed.netloc)
  if 'uBlockOrigin/uAssets' in parsed.path and not base.startswith('ubo-'):
    base = 'ubo-' + base
  elif parsed.path.startswith('/easylist/easylist/') or (
      parsed.netloc == 'easylist-downloads.adblockplus.org' and
      not base.startswith('fanboy')):
    if not base.startswith('easylist'):
      base = 'easylist-' + base
  candidate = base
  if candidate in taken and len(parts) >= 2:
    candidate = slug(parts[-2]) + '-' + base
  n = 2
  while candidate in taken:
    candidate = '%s-%d' % (base, n)
    n += 1
  if not ID_RE.match(candidate):
    raise UpdateError('cannot derive id from %s' % url)
  taken.add(candidate)
  return candidate


def fetch(url, max_bytes=MAX_LIST_BYTES):
  """HTTPS GET with certificate verification and a size cap."""
  if urllib.parse.urlparse(url).scheme != 'https':
    raise UpdateError('refusing non-https source: %s' % url)

  class HttpsOnlyRedirect(urllib.request.HTTPRedirectHandler):

    def redirect_request(self, req, fp, code, msg, headers, newurl):
      if urllib.parse.urlparse(newurl).scheme != 'https':
        raise UpdateError('redirect to non-https url from %s' % url)
      return super().redirect_request(req, fp, code, msg, headers, newurl)

  opener = urllib.request.build_opener(
      urllib.request.HTTPSHandler(context=ssl.create_default_context()),
      HttpsOnlyRedirect())
  last_error = None
  for _ in range(3):
    try:
      req = urllib.request.Request(url, headers={'User-Agent': USER_AGENT})
      with opener.open(req, timeout=60) as resp:
        if resp.status != 200:
          raise UpdateError('HTTP %d for %s' % (resp.status, url))
        data = resp.read(max_bytes + 1)
        if len(data) > max_bytes:
          raise UpdateError('%s is larger than %d bytes' % (url, max_bytes))
        return data
    except UpdateError:
      raise
    except Exception as e:  # pylint: disable=broad-except
      last_error = e
  raise UpdateError('download failed for %s: %s' % (url, last_error))


def normalize_filter_list(raw):
  """Returns (normalised bytes, rule count).

  Drops comments and blank lines but keeps `!#` preprocessor directives, which
  adblock-rust evaluates.
  """
  if b'\0' in raw:
    raise UpdateError('list contains NUL bytes')
  try:
    text = raw.decode('utf-8')
  except UnicodeDecodeError as e:
    raise UpdateError('list is not UTF-8: %s' % e) from e
  text = text.lstrip('﻿')
  out = []
  rules = 0
  for line in text.replace('\r\n', '\n').replace('\r', '\n').split('\n'):
    line = line.strip()
    if not line:
      continue
    if line.startswith('!') and not line.startswith('!#'):
      continue
    if line.startswith('[') and line.endswith(']'):
      continue  # "[Adblock Plus 2.0]" style headers
    out.append(line)
    if not line.startswith('!#'):
      rules += 1
  return ('\n'.join(out) + '\n').encode('utf-8'), rules


def is_scoped(options):
  """True if filter options restrict the rule to specific sites."""
  for option in options.split(','):
    name, _, value = option.strip().partition('=')
    if name in ('domain', 'from', 'to') and any(
        d and not d.startswith('~') for d in value.split('|')):
      return True
  return False


def is_universal(pattern):
  """True if a network pattern matches every URL."""
  pattern = pattern.strip()
  if pattern in UNIVERSAL_REGEX_PATTERNS:
    return True
  return pattern.strip('|*^/:') in UNIVERSAL_SCHEMES


def unsafe_exceptions(text_bytes):
  """Unscoped exception rules that would allow everything."""
  bad = []
  for line in text_bytes.decode('utf-8').split('\n'):
    line = line.strip()
    if not line.startswith('@@'):
      continue
    pattern, _, options = line[2:].partition('$')
    if is_universal(pattern) and not is_scoped(options):
      bad.append(line)
  return bad


def validate_resources(raw):
  try:
    resources = json.loads(raw.decode('utf-8'))
  except (UnicodeDecodeError, ValueError) as e:
    raise UpdateError('resources.json is not valid JSON: %s' % e) from e
  if not isinstance(resources, list) or not resources:
    raise UpdateError('resources.json must be a non-empty list')
  for r in resources:
    if not isinstance(r, dict) or not {'name', 'kind', 'content'} <= set(r):
      raise UpdateError('malformed resource entry')
  return len(resources)


def validate_catalog(raw):
  try:
    catalog = json.loads(raw.decode('utf-8'))
  except (UnicodeDecodeError, ValueError) as e:
    raise UpdateError('list catalog is not valid JSON: %s' % e) from e
  if not isinstance(catalog, list) or not catalog:
    raise UpdateError('list catalog must be a non-empty list')
  return catalog


def make_entry(list_id, title, kind, file_name, data, **extra):
  entry = {
      'id': list_id,
      'title': title,
      'kind': kind,
      'file': file_name,
      'sha256': sha256_hex(data),
      'size': len(data),
  }
  entry.update(extra)
  return entry


def build_filter_entry(list_id, title, source_url, raw, replaces, engine,
                       permission_mask, min_rules):
  data, rules = normalize_filter_list(raw)
  if rules < min_rules:
    raise UpdateError('%s has only %d rules' % (source_url, rules))
  bad = unsafe_exceptions(data)
  if bad:
    raise UpdateError('%s has unsafe exceptions: %s' % (source_url, bad[:3]))
  entry = make_entry(list_id,
                     title,
                     'filters',
                     list_id + '.txt',
                     data,
                     source_url=source_url,
                     upstream_sha256=sha256_hex(raw),
                     rules=rules,
                     replaces=replaces,
                     engine=engine,
                     permission_mask=permission_mask)
  return entry, data


def local_entries():
  entries = []
  for spec in LOCAL_LISTS:
    with open(os.path.join(LOCAL_DIR, spec['file']), 'rb') as f:
      raw = f.read()
    entry, data = build_filter_entry(spec['id'], spec['title'],
                                     'local/' + spec['file'], raw, '',
                                     spec['engine'], spec['permission_mask'],
                                     min_rules=1)
    del entry['upstream_sha256']
    entries.append((entry, data))
  return entries


def fetch_all():
  """Returns [(entry, data)] for every list, catalog and resources."""
  results = []
  taken = set(s['id'] for s in EXTRA_LISTS) | set(s['id'] for s in LOCAL_LISTS)
  taken |= {'brave-list-catalog', 'brave-resources'}

  catalog_raw = fetch(CATALOG_URL)
  catalog = validate_catalog(catalog_raw)
  results.append((make_entry('brave-list-catalog',
                             'Brave filter list catalog',
                             'catalog',
                             'brave-list-catalog.json',
                             catalog_raw,
                             source_url=CATALOG_URL), catalog_raw))

  resources_raw = fetch(RESOURCES_URL)
  validate_resources(resources_raw)
  results.append((make_entry('brave-resources',
                             'Brave adblock resources',
                             'resources',
                             'brave-resources.json',
                             resources_raw,
                             source_url=RESOURCES_URL), resources_raw))

  by_uuid = {e.get('uuid'): e for e in catalog}
  seen_urls = set()
  for uuid, locked in CATALOG_GROUPS:
    entry = by_uuid.get(uuid)
    if not entry:
      raise UpdateError('catalog entry %s is missing' % uuid)
    # Lists with first-party protections run in the default engine, as in
    # AdBlockComponentServiceManager::StartRegionalServices().
    engine = ('default'
              if entry.get('first_party_protections') else 'additional')
    mask = int(entry.get('permission_mask', 0))
    for source in entry.get('sources', []):
      url = source['url']
      if url in seen_urls:
        continue  # same list shared by two catalog entries
      seen_urls.add(url)
      list_id = list_id_for_url(url, taken)
      title = source.get('title') or list_id
      raw = fetch(url)
      if normalize_filter_list(raw)[1] == 0:
        print('  %-40s skipped, no rules' % list_id)
        continue
      print('  %-40s %8d bytes' % (list_id, len(raw)))
      results.append(
          build_filter_entry(list_id,
                             title,
                             url,
                             raw,
                             uuid,
                             engine,
                             mask,
                             min_rules=DEFAULT_MIN_RULES))
    del locked  # Locking is enforced in the browser by uuid.

  for spec in EXTRA_LISTS:
    raw = fetch(spec['url'])
    print('  %-40s %8d bytes' % (spec['id'], len(raw)))
    results.append(
        build_filter_entry(spec['id'], spec['title'], spec['url'], raw, '',
                           spec['engine'], spec['permission_mask'],
                           spec['min_rules']))
  return results


def manifest_version(now):
  return int(now.strftime('%Y%m%d%H%M'))


def render_manifest(entries, now):
  return json.dumps(
      {
          'schema': SCHEMA,
          'channel': CHANNEL,
          'version': manifest_version(now),
          'generated': now.strftime('%Y-%m-%dT%H:%M:%SZ'),
          'lists': entries,
      },
      indent=2,
      sort_keys=False) + '\n'


GRD_HEADER = '''<?xml version="1.0" encoding="UTF-8"?>
<!-- Generated by ko/shields/update_lists.py. Do not edit. -->
<grit latest_public_release="0" current_release="1">
  <outputs>
    <output filename="grit/ko_shields_resources.h" type="rc_header">
      <emit emit_type='prepend'></emit>
    </output>
    <output filename="ko_shields_resources.pak" type="data_package" />
  </outputs>
  <release seq="1">
    <includes>
      <include name="IDR_KO_SHIELDS_MANIFEST" file="lists/manifest.json" type="BINDATA" compress="brotli" />
'''

GRD_FOOTER = '''    </includes>
  </release>
</grit>
'''

INC_HEADER = '''// Copyright (c) 2026 The Brave Authors. All rights reserved.
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this file,
// You can obtain one at https://mozilla.org/MPL/2.0/.

// Generated by ko/shields/update_lists.py. Do not edit.
// clang-format off
'''


def render_grd(entries):
  lines = [GRD_HEADER]
  for e in entries:
    lines.append('      <include name="%s" file="lists/%s" type="BINDATA" '
                 'compress="brotli" />\n' % (resource_name(e['id']), e['file']))
  lines.append(GRD_FOOTER)
  return ''.join(lines)


def render_inc(entries):
  lines = [INC_HEADER]
  for e in entries:
    lines.append('KO_EMBEDDED_LIST("%s", %s)\n' %
                 (e['id'], resource_name(e['id'])))
  return ''.join(lines)


def validate_entries(entries):
  ids = set()
  total = 0
  for e in entries:
    if not ID_RE.match(e['id']) or e['id'] in ids:
      raise UpdateError('bad or duplicate id: %s' % e['id'])
    ids.add(e['id'])
    total += e['size']
  if total > MAX_TOTAL_BYTES:
    raise UpdateError('snapshot is %d bytes, over the limit' % total)
  for uuid, locked in CATALOG_GROUPS:
    if locked and not any(e.get('replaces') == uuid for e in entries):
      raise UpdateError('no list covers locked catalog entry %s' % uuid)


def write_atomic(path, data):
  if isinstance(data, str):
    data = data.encode('utf-8')
  fd, tmp = tempfile.mkstemp(dir=os.path.dirname(path))
  with os.fdopen(fd, 'wb') as f:
    f.write(data)
  os.chmod(tmp, 0o644)
  os.replace(tmp, path)


def write_snapshot(results, now):
  entries = [e for e, _ in results]
  validate_entries(entries)
  os.makedirs(LISTS_DIR, exist_ok=True)
  keep = {e['file'] for e in entries} | {'manifest.json'}
  for name in os.listdir(LISTS_DIR):
    if name not in keep:
      os.remove(os.path.join(LISTS_DIR, name))
  for entry, data in results:
    write_atomic(os.path.join(LISTS_DIR, entry['file']), data)
  write_atomic(MANIFEST_PATH, render_manifest(entries, now))
  write_atomic(GRD_PATH, render_grd(entries))
  write_atomic(INC_PATH, render_inc(entries))
  total = sum(e['size'] for e in entries)
  print('wrote %d lists, %d bytes' % (len(entries), total))


def load_manifest():
  with open(MANIFEST_PATH, 'rb') as f:
    return json.loads(f.read().decode('utf-8'))


def check_snapshot():
  """Offline consistency check of the committed snapshot."""
  manifest = load_manifest()
  if manifest.get('schema') != SCHEMA or manifest.get('channel') != CHANNEL:
    raise UpdateError('unexpected manifest schema/channel')
  entries = manifest['lists']
  validate_entries(entries)
  for e in entries:
    with open(os.path.join(LISTS_DIR, e['file']), 'rb') as f:
      data = f.read()
    if len(data) != e['size'] or sha256_hex(data) != e['sha256']:
      raise UpdateError('%s does not match the manifest' % e['file'])
    if e['kind'] == 'filters' and unsafe_exceptions(data):
      raise UpdateError('%s has unsafe exceptions' % e['file'])
  with open(GRD_PATH, encoding='utf-8') as f:
    if f.read() != render_grd(entries):
      raise UpdateError('ko_shields_resources.grd is stale')
  with open(INC_PATH, encoding='utf-8') as f:
    if f.read() != render_inc(entries):
      raise UpdateError('ko_embedded_lists.inc is stale')
  print('snapshot OK: %d lists, version %d' %
        (len(entries), manifest['version']))


def refresh_local_only(now):
  manifest = load_manifest()
  local_ids = {s['id'] for s in LOCAL_LISTS}
  results = []
  for e in manifest['lists']:
    if e['id'] in local_ids:
      continue
    with open(os.path.join(LISTS_DIR, e['file']), 'rb') as f:
      results.append((e, f.read()))
  results.extend(local_entries())
  write_snapshot(results, now)


def main():
  parser = argparse.ArgumentParser(description=__doc__)
  group = parser.add_mutually_exclusive_group()
  group.add_argument('--check', action='store_true')
  group.add_argument('--local-only', action='store_true')
  args = parser.parse_args()
  now = datetime.datetime.now(datetime.timezone.utc)
  try:
    if args.check:
      check_snapshot()
    elif args.local_only:
      refresh_local_only(now)
    else:
      results = fetch_all()
      results.extend(local_entries())
      write_snapshot(results, now)
  except UpdateError as e:
    print('error: %s' % e, file=sys.stderr)
    return 1
  return 0


if __name__ == '__main__':
  sys.exit(main())
