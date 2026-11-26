#!/usr/bin/env python3
# Copyright (c) 2026 The Brave Authors. All rights reserved.
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this file,
# You can obtain one at https://mozilla.org/MPL/2.0/.

import unittest

from ko_branding import rebrand_message, rebrand_text


class RebrandTextTest(unittest.TestCase):

    def test_product_name(self):
        self.assertEqual(rebrand_text('About Brave'), 'About K-O')
        self.assertEqual(rebrand_text("Brave's settings"), "K-O's settings")
        self.assertEqual(rebrand_text('$1 - Brave'), '$1 - K-O')
        self.assertEqual(rebrand_text('Make Brave your default browser'),
                         'Make K-O your default browser')

    def test_full_name_and_shields(self):
        self.assertEqual(rebrand_text('Brave Browser'), 'K-O')
        self.assertEqual(rebrand_text('Brave Shields are UP for $1'),
                         'Shields are UP for $1')

    def test_kept_service_names(self):
        for text in ('Brave Search', 'Brave Sync', 'Turn on Brave sync',
                     'Brave Software Inc', 'Brave Rewards', 'Brave-hosted',
                     "Brave's updater", 'Brave\u2019s proxy',
                     'Click the Brave icon'):
            self.assertEqual(rebrand_text(text), text)
        self.assertEqual(rebrand_text('Brave uses Brave Search'),
                         'K-O uses Brave Search')

    def test_keeps_brave_as_data_recipient(self):
        for before, after in (
            ('Brave sends part of the URL to Brave through a privacy server',
             'K-O sends part of the URL to Brave through a privacy server'),
            ('Help make Brave better by sending crash reports to Brave',
             'Help make K-O better by sending crash reports to Brave'),
            ('The passphrase is not sent to or stored by Brave.',
             'The passphrase is not sent to or stored by Brave.'),
            ('Remove your encrypted data from Brave servers',
             'Remove your encrypted data from Brave servers'),
            ('Help by sending <a href="#">URLs you visit</a> to Brave.',
             'Help by sending <a href="#">URLs you visit</a> to Brave.'),
            ('Info is sent to Brave about your Brave usage',
             'Info is sent to Brave about your K-O usage'),
            ('See the Brave Privacy Policy', 'See the Brave Privacy Policy'),
            ('$1 may be dangerous. Send to Brave for scanning?',
             '$1 may be dangerous. Send to Brave for scanning?'),
        ):
            self.assertEqual(rebrand_text(before), after)
        self.assertEqual(rebrand_text('Sign in to Brave'), 'Sign in to K-O')

    def test_leaves_other_words(self):
        for text in ('brave://settings', 'https://brave.com', 'BraveSoftware',
                     'Bravery', 'Be brave', 'Braves'):
            self.assertEqual(rebrand_text(text), text)

    def test_translations(self):
        self.assertEqual(rebrand_text('Über Brave'), 'Über K-O')
        self.assertEqual(rebrand_text('Brave est à jour'), 'K-O est à jour')


class RebrandMessageTest(unittest.TestCase):

    def test_skips_ai_chat_area(self):
        for name in ('IDS_CHAT_UI_TITLE', 'IDS_AI_CHAT_NAME',
                     'IDS_OPEN_BRAVE_AI_CHAT_SIDE_PANEL', 'IDS_SETTINGS_LEO'):
            self.assertEqual(rebrand_message(name, 'Brave'), 'Brave')
        self.assertEqual(rebrand_message('IDS_ABOUT', 'About Brave'),
                         'About K-O')

    def test_upstream_credit_link_text(self):
        text = ('Brave is made possible by the <a target="_blank" href="$1">'
                'Brave</a> open source project.')
        self.assertEqual(
            rebrand_message('IDS_VERSION_UI_LICENSE', text),
            'K-O is made possible by the <a target="_blank" href="$1">'
            'Brave</a> open source project.')
        self.assertEqual(
            rebrand_message('IDS_OTHER', '<a href="$1">Brave</a>'),
            '<a href="$1">K-O</a>')

    def test_keeps_brave_search_names(self):
        for name in ('IDS_SEARCH_ENGINES_STARTER_PACK_ASK_BRAVE_SEARCH_NAME',
                     'IDS_NTP_DEFAULT_SEARCH_TITLE'):
            self.assertEqual(rebrand_message(name, 'Brave'), 'Brave')
        self.assertEqual(rebrand_message('IDS_PRODUCT_NAME', 'Brave'), 'K-O')

    def test_empty(self):
        self.assertEqual(rebrand_message('IDS_X', ''), '')


if __name__ == '__main__':
    unittest.main()
