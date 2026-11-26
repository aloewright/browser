#!/usr/bin/env python3
# Copyright (c) 2026 The Brave Authors. All rights reserved.
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this file,
# You can obtain one at https://mozilla.org/MPL/2.0/.
"""Signs the K-O filter list snapshot for the optional update channel.

The signing key is an ed25519 seed (64 hex chars) read from the environment
variable KO_FILTER_LISTS_SIGNING_KEY, e.g. injected by Doppler:

  doppler run -- python3 ko/shields/publish_lists.py --out /tmp/ko-lists

Upload the output directory to the HTTPS location set in the GN arg
ko_filter_lists_url (lists first, manifest.json last). With --r2-bucket the
script uploads through `wrangler r2 object put`.

  publish_lists.py --generate-key FILE
      writes a new seed to FILE (mode 0600) and prints its public key; load it
      with `doppler secrets set KO_FILTER_LISTS_SIGNING_KEY < FILE`, then
      delete FILE.
"""

import argparse
import json
import os
import shutil
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
LISTS_DIR = os.path.join(HERE, 'lists')
KEY_ENV = 'KO_FILTER_LISTS_SIGNING_KEY'

# Domain separation; must match kKoManifestSignaturePrefix in the browser.
SIGNATURE_PREFIX = b'ko-filter-lists-manifest-v1\n'
CHANNEL_KINDS = ('filters', 'resources')


class PublishError(Exception):
  pass


def _ed25519():
  try:
    from cryptography.hazmat.primitives.asymmetric import ed25519  # pylint: disable=import-outside-toplevel
    from cryptography.hazmat.primitives import serialization  # pylint: disable=import-outside-toplevel
  except ImportError as e:
    raise PublishError('install the "cryptography" package') from e
  return ed25519, serialization


def public_key_hex(seed):
  ed25519, serialization = _ed25519()
  key = ed25519.Ed25519PrivateKey.from_private_bytes(seed)
  return key.public_key().public_bytes(
      serialization.Encoding.Raw, serialization.PublicFormat.Raw).hex()


def sign_manifest(manifest_bytes, seed):
  ed25519, _ = _ed25519()
  key = ed25519.Ed25519PrivateKey.from_private_bytes(seed)
  return key.sign(SIGNATURE_PREFIX + manifest_bytes).hex()


def verify_manifest(manifest_bytes, signature_hex, pubkey_hex):
  ed25519, _ = _ed25519()
  from cryptography.exceptions import InvalidSignature  # pylint: disable=import-outside-toplevel
  key = ed25519.Ed25519PublicKey.from_public_bytes(bytes.fromhex(pubkey_hex))
  try:
    key.verify(bytes.fromhex(signature_hex), SIGNATURE_PREFIX + manifest_bytes)
  except InvalidSignature:
    return False
  return True


def read_seed():
  value = os.environ.get(KEY_ENV, '').strip()
  if len(value) != 64:
    raise PublishError('%s must hold a 32-byte hex seed' % KEY_ENV)
  try:
    return bytes.fromhex(value)
  except ValueError as e:
    raise PublishError('%s is not hex' % KEY_ENV) from e


def build_channel_manifest(manifest):
  """Channel manifest: the snapshot without browser-only entries."""
  lists = [e for e in manifest['lists'] if e['kind'] in CHANNEL_KINDS]
  out = dict(manifest)
  out['lists'] = lists
  return (json.dumps(out, indent=2) + '\n').encode('utf-8')


def stage(out_dir, seed, pubkey_hex=None):
  with open(os.path.join(LISTS_DIR, 'manifest.json'), 'rb') as f:
    manifest = json.loads(f.read().decode('utf-8'))
  channel_bytes = build_channel_manifest(manifest)
  signature = sign_manifest(channel_bytes, seed)
  if pubkey_hex and not verify_manifest(channel_bytes, signature, pubkey_hex):
    raise PublishError('signature does not verify with --pubkey')

  os.makedirs(out_dir, exist_ok=True)
  files = []
  for entry in json.loads(channel_bytes)['lists']:
    shutil.copyfile(os.path.join(LISTS_DIR, entry['file']),
                    os.path.join(out_dir, entry['file']))
    files.append(entry['file'])
  with open(os.path.join(out_dir, 'manifest.json.sig'), 'w') as f:
    f.write(signature + '\n')
  with open(os.path.join(out_dir, 'manifest.json'), 'wb') as f:
    f.write(channel_bytes)
  # Upload order: lists, signature, then the manifest that references them.
  return files + ['manifest.json.sig', 'manifest.json']


def content_type(name):
  if name.endswith('.json'):
    return 'application/json'
  return 'text/plain; charset=utf-8'


def upload_r2(out_dir, files, bucket, prefix):
  for name in files:
    key = '/'.join(p for p in (bucket, prefix.strip('/'), name) if p)
    subprocess.run([
        'wrangler', 'r2', 'object', 'put', key, '--remote', '--file',
        os.path.join(out_dir, name), '--content-type',
        content_type(name), '--cache-control', 'no-cache'
    ],
                   check=True)


def main():
  parser = argparse.ArgumentParser(description=__doc__)
  parser.add_argument('--generate-key', metavar='FILE')
  parser.add_argument('--out', help='staging directory')
  parser.add_argument('--pubkey', help='expected public key (hex)')
  parser.add_argument('--r2-bucket', help='upload with wrangler to this bucket')
  parser.add_argument('--r2-prefix', default='', help='object key prefix')
  args = parser.parse_args()
  try:
    if args.generate_key:
      seed = os.urandom(32)
      fd = os.open(args.generate_key, os.O_WRONLY | os.O_CREAT | os.O_EXCL,
                   0o600)
      with os.fdopen(fd, 'w') as f:
        f.write(seed.hex())
      print('seed written to %s; store it as %s, never commit it' %
            (args.generate_key, KEY_ENV))
      print('pubkey: %s  (GN arg ko_filter_lists_pubkey)' %
            public_key_hex(seed))
      return 0
    if not args.out:
      parser.error('--out is required')
    files = stage(args.out, read_seed(), args.pubkey)
    print('staged %d files in %s' % (len(files), args.out))
    if args.r2_bucket:
      upload_r2(args.out, files, args.r2_bucket, args.r2_prefix)
  except PublishError as e:
    print('error: %s' % e, file=sys.stderr)
    return 1
  return 0


if __name__ == '__main__':
  sys.exit(main())
