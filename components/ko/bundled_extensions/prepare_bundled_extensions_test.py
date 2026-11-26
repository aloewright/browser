#!/usr/bin/env python3
# Copyright (c) 2026 The Brave Authors. All rights reserved.
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this file,
# You can obtain one at https://mozilla.org/MPL/2.0/.

import json
import os
import tempfile
import unittest

import prepare_bundled_extensions as prepare


def write(path, content):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, 'w', encoding='utf-8') as f:
        f.write(content)


class PrepareBundledExtensionsTest(unittest.TestCase):

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = self.tmp.name
        self.out = os.path.join(self.root, 'out')
        self.header = os.path.join(self.root, 'ko_bundled_extensions.h')

    def tearDown(self):
        self.tmp.cleanup()

    def make_extension(self, name, manifest):
        source = os.path.join(self.root, 'src', name)
        write(os.path.join(source, 'manifest.json'), json.dumps(manifest))
        write(os.path.join(source, 'background.js'), '// js')
        write(os.path.join(source, '.git', 'config'), 'secret-ish')
        return source

    def read_header(self):
        with open(self.header, encoding='utf-8') as f:
            return f.read()

    def test_injects_stable_key(self):
        source = self.make_extension('my-ext', {
            'name': 'Mine',
            'version': '1.0',
            'manifest_version': 3
        })
        self.assertEqual(
            prepare.main(['--out-dir', self.out, '--header', self.header,
                          source]), 0)
        with open(os.path.join(self.out, 'my-ext', 'manifest.json')) as f:
            staged = json.load(f)
        self.assertEqual(staged['key'], prepare.derive_key('my-ext'))
        self.assertTrue(
            os.path.isfile(os.path.join(self.out, 'my-ext', 'background.js')))
        self.assertFalse(os.path.exists(os.path.join(self.out, 'my-ext',
                                                     '.git')))
        ext_id = prepare.extension_id(staged['key'])
        self.assertEqual(len(ext_id), 32)
        self.assertTrue(set(ext_id) <= set('abcdefghijklmnop'))
        self.assertIn(ext_id, self.read_header())

        # Same name, same ID on every run.
        self.assertEqual(
            prepare.main(['--out-dir', self.out, '--header', self.header,
                          source]), 0)
        self.assertIn(ext_id, self.read_header())

    def test_keeps_existing_key(self):
        source = self.make_extension('keyed', {
            'name': 'Keyed',
            'version': '1',
            'manifest_version': 3,
            'key': 'QUJDRA=='
        })
        self.assertEqual(
            prepare.main(['--out-dir', self.out, '--header', self.header,
                          source]), 0)
        self.assertIn(prepare.extension_id('QUJDRA=='), self.read_header())

    def test_empty_list(self):
        self.assertEqual(
            prepare.main(['--out-dir', self.out, '--header', self.header]), 0)
        self.assertIn('std::array<BundledExtension, 0> kBundledExtensions = {};',
                      self.read_header())

    def test_rejects_bad_input(self):
        bad_name = self.make_extension('bad name', {
            'name': 'x',
            'version': '1',
            'manifest_version': 3
        })
        self.assertEqual(
            prepare.main(['--out-dir', self.out, '--header', self.header,
                          bad_name]), 1)
        no_version = self.make_extension('nover', {
            'name': 'x',
            'manifest_version': 3
        })
        self.assertEqual(
            prepare.main(['--out-dir', self.out, '--header', self.header,
                          no_version]), 1)
        missing = os.path.join(self.root, 'src', 'missing')
        os.makedirs(missing)
        self.assertEqual(
            prepare.main(['--out-dir', self.out, '--header', self.header,
                          missing]), 1)

    def test_skips_symlinks(self):
        source = self.make_extension('links', {
            'name': 'x',
            'version': '1',
            'manifest_version': 3
        })
        os.symlink('/etc/hosts', os.path.join(source, 'hosts'))
        self.assertEqual(
            prepare.main(['--out-dir', self.out, '--header', self.header,
                          source]), 0)
        self.assertFalse(os.path.lexists(os.path.join(self.out, 'links',
                                                      'hosts')))


if __name__ == '__main__':
    unittest.main()
