#!/usr/bin/env python3
# Copyright (c) 2026 The Brave Authors. All rights reserved.
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this file,
# You can obtain one at https://mozilla.org/MPL/2.0/.
"""Tests for ko/shields/update_lists.py and publish_lists.py.

Run: python3 ko/shields/test/test_shields_scripts.py
"""

import json
import os
import sys
import tempfile
import unittest
from unittest import mock

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

import publish_lists  # pylint: disable=wrong-import-position
import update_lists  # pylint: disable=wrong-import-position

# RFC 8032, section 7.1, TEST 1.
RFC_SEED = bytes.fromhex(
    '9d61b19deffd5a60ba844af492ec2cc44449c5697b326919703bac031cae7f60')
RFC_PUBKEY = 'd75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a'


class NormalizeTest(unittest.TestCase):

  def test_strips_comments_and_keeps_directives(self):
    raw = (b'\xef\xbb\xbf[Adblock Plus 2.0]\r\n! Title: x\r\n\r\n'
           b'||ads.example.com^  \r\n!#if env_mobile\n##.banner\n!#endif\n')
    data, rules = update_lists.normalize_filter_list(raw)
    self.assertEqual(
        data, b'||ads.example.com^\n!#if env_mobile\n##.banner\n!#endif\n')
    self.assertEqual(rules, 2)

  def test_rejects_binary(self):
    with self.assertRaises(update_lists.UpdateError):
      update_lists.normalize_filter_list(b'||a.example^\0')
    with self.assertRaises(update_lists.UpdateError):
      update_lists.normalize_filter_list(b'\xff\xfe')


class UnsafeExceptionTest(unittest.TestCase):

  def test_flags_global_allow_rules(self):
    for rule in ('@@*', '@@||*^', '@@|https://', '@@^$document', '@@*$ghide',
                 '@@*$domain=~example.com'):
      self.assertEqual(update_lists.unsafe_exceptions(rule.encode()), [rule],
                       rule)

  def test_allows_scoped_rules(self):
    text = (b'@@*$ghide,domain=example.com\n@@||example.com^\n'
            b'@@/cdn-cgi/access/\n@@*$to=example.org\n##.ad\n')
    self.assertEqual(update_lists.unsafe_exceptions(text), [])


class IdTest(unittest.TestCase):

  def test_ids_are_stable_and_unique(self):
    taken = set()
    f = update_lists.list_id_for_url
    self.assertEqual(
        f('https://raw.githubusercontent.com/uBlockOrigin/uAssets/master/'
          'filters/filters.txt', taken), 'ubo-filters')
    self.assertEqual(f('https://easylist.to/easylist/easylist.txt', taken),
                     'easylist')
    self.assertEqual(
        f('https://raw.githubusercontent.com/brave/adblock-lists/master/'
          'brave-unbreak.txt', taken), 'brave-unbreak')
    self.assertEqual(
        f('https://raw.githubusercontent.com/brave/adblock-lists/master/'
          'brave-lists/brave-unbreak.txt', taken), 'brave-lists-brave-unbreak')

  def test_resource_name(self):
    self.assertEqual(update_lists.resource_name('ubo-filters-2020'),
                     'IDR_KO_SHIELDS_UBO_FILTERS_2020')


class FetchTest(unittest.TestCase):

  def test_refuses_plain_http(self):
    with self.assertRaises(update_lists.UpdateError):
      update_lists.fetch('http://easylist.to/easylist/easylist.txt')


class SnapshotTest(unittest.TestCase):

  def test_committed_snapshot_is_consistent(self):
    update_lists.check_snapshot()

  def test_locked_groups_are_covered(self):
    entries = update_lists.load_manifest()['lists']
    with self.assertRaises(update_lists.UpdateError):
      update_lists.validate_entries(
          [e for e in entries if e.get('replaces') != 'default'])

  def test_rejects_oversized_list(self):
    entries = update_lists.load_manifest()['lists']
    entries[0] = dict(entries[0], size=update_lists.MAX_LIST_BYTES + 1)
    with self.assertRaises(update_lists.UpdateError):
      update_lists.validate_entries(entries)

  def test_manifest_fields(self):
    manifest = update_lists.load_manifest()
    for e in manifest['lists']:
      self.assertIn(e['kind'], ('filters', 'resources', 'catalog'))
      self.assertTrue(e['source_url'].startswith(('https://', 'local/')))
      if e['kind'] == 'filters':
        self.assertIn(e['engine'], ('default', 'additional', 'both'))
        self.assertIn(e['permission_mask'], (0, 1))


class PublishTest(unittest.TestCase):

  def test_rfc8032_vector(self):
    self.assertEqual(publish_lists.public_key_hex(RFC_SEED), RFC_PUBKEY)

  def test_sign_and_verify(self):
    data = b'{"schema":1}\n'
    sig = publish_lists.sign_manifest(data, RFC_SEED)
    self.assertTrue(publish_lists.verify_manifest(data, sig, RFC_PUBKEY))
    self.assertFalse(publish_lists.verify_manifest(data + b' ', sig,
                                                   RFC_PUBKEY))
    # The prefix binds signatures to this channel.
    raw = publish_lists._ed25519()[0].Ed25519PrivateKey.from_private_bytes(
        RFC_SEED).sign(data).hex()
    self.assertFalse(publish_lists.verify_manifest(data, raw, RFC_PUBKEY))

  def test_stage_writes_signed_channel(self):
    with tempfile.TemporaryDirectory() as out:
      files = publish_lists.stage(out, RFC_SEED, RFC_PUBKEY)
      self.assertEqual(files[-1], 'manifest.json')
      with open(os.path.join(out, 'manifest.json'), 'rb') as f:
        manifest_bytes = f.read()
      with open(os.path.join(out, 'manifest.json.sig')) as f:
        sig = f.read().strip()
      self.assertTrue(
          publish_lists.verify_manifest(manifest_bytes, sig, RFC_PUBKEY))
      manifest = json.loads(manifest_bytes)
      self.assertFalse(
          [e for e in manifest['lists'] if e['kind'] == 'catalog'])
      for e in manifest['lists']:
        self.assertTrue(os.path.exists(os.path.join(out, e['file'])))

  def test_seed_from_env(self):
    with mock.patch.dict(os.environ, {publish_lists.KEY_ENV: 'zz'}):
      with self.assertRaises(publish_lists.PublishError):
        publish_lists.read_seed()
    with mock.patch.dict(os.environ, {publish_lists.KEY_ENV: RFC_SEED.hex()}):
      self.assertEqual(publish_lists.read_seed(), RFC_SEED)


if __name__ == '__main__':
  unittest.main()
