#!/usr/bin/env python3
# Copyright (c) 2026 The Brave Authors. All rights reserved.
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this file,
# You can obtain one at https://mozilla.org/MPL/2.0/.
"""K-O product-name rewrite for grit message text.

grit applies rebrand_message() to every <message> it outputs when the build
uses the "ko" branding (see //brave/chromium_src/tools/grit/grit/tool/build.py),
so translations keep matching and no .grd is edited by hand.

  ko_branding.py --audit FILE.grd[p] ...   # list the English strings changed
"""

import argparse
import re
import sys
import xml.etree.ElementTree as ElementTree

PRODUCT_NAME = 'K-O'

# Names of Brave services and credits that K-O still uses or must not claim.
_KEEP = (
    'Brave Account',
    'Brave Accounts',
    'Brave Ads',
    'Brave Authors',
    'Brave Firewall',
    'Brave Leo',
    'Brave News',
    'Brave Origin',
    'Brave Playlist',
    'Brave Private Ads',
    'Brave Rewards',
    'Brave Root',
    'Brave Search',
    'Brave Software',
    'Brave Sync',
    'Brave Talk',
    'Brave Terms',
    'Brave Today',
    'Brave Translate',
    'Brave Updater',
    'Brave VPN',
    'Brave Wallet',
    'Brave icon',  # The Shields button still shows Brave's lion.
    'Brave sync',
    'Brave-hosted',
    "Brave's mission",
    "Brave's proxy",
    "Brave's updater",
    'Brave\u2019s mission',
    'Brave\u2019s proxy',
    'Brave\u2019s updater',
)

# Applied before the generic Brave -> K-O replacement. Shields keeps its name.
_REWRITE = (
    ('Brave Shields', 'Shields'),
    ('Brave Browser', PRODUCT_NAME),
    ('Brave browser', PRODUCT_NAME),
)

# Messages owned by other K-O areas, rebranded there if at all.
_SKIPPED_NAME_PARTS = ('AI_CHAT', '_LEO', 'IDS_CHAT_UI_', 'IDS_AI_',
                       'IDS_CUSTOM_MODEL')

# The license line credits the upstream project in its link text:
# "K-O is made possible by the <a ...>Brave</a> open source project".
_UPSTREAM_CREDIT_MESSAGES = ('IDS_VERSION_UI_LICENSE',
                             'IDS_VERSION_UI_LICENSE_CHROMIUM')
_LINK_TEXT_BRAVE = re.compile(r'(<a\b[^>]*>)Brave(</a>)')

_WORD = re.compile(r'\bBrave\b')
_PROTECT = re.compile('|'.join(
    re.escape(p) for p in sorted(_KEEP, key=len, reverse=True)))
_MARK = '\x00{}\x00'


def _is_skipped(name):
    return any(part in name for part in _SKIPPED_NAME_PARTS)


def rebrand_text(text):
    """Rewrites the product name in `text`, leaving kept phrases alone."""
    if 'Brave' not in text:
        return text
    kept = []

    def stash(match):
        kept.append(match.group(0))
        return _MARK.format(len(kept) - 1)

    text = _PROTECT.sub(stash, text)
    for old, new in _REWRITE:
        text = re.sub(r'\b%s\b' % re.escape(old), new, text)
    text = _WORD.sub(PRODUCT_NAME, text)
    return re.sub('\x00(\\d+)\x00', lambda m: kept[int(m.group(1))], text)


def rebrand_message(name, text):
    """grit hook: `name` is the message's IDS_ name, `text` its output."""
    if not text or _is_skipped(name):
        return text
    if name in _UPSTREAM_CREDIT_MESSAGES:
        held = _LINK_TEXT_BRAVE.sub('\\1\x01\\2', text)
        return rebrand_text(held).replace('\x01', 'Brave')
    return rebrand_text(text)


def _message_text(element):
    text = element.text or ''
    for child in element:
        if child.tag == 'ph':
            text += child.text or ''
        text += child.tail or ''
    return ' '.join(text.split())


def audit(paths):
    changed = 0
    for path in paths:
        for message in ElementTree.parse(path).iter('message'):
            name = message.get('name', '')
            before = _message_text(message)
            after = rebrand_message(name, before)
            if after != before:
                changed += 1
                print('%s\n  - %s\n  + %s' % (name, before, after))
    print('%d message(s) changed' % changed, file=sys.stderr)


def main():
    parser = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    parser.add_argument('--audit', nargs='+', metavar='FILE', required=True)
    args = parser.parse_args()
    audit(args.audit)
    return 0


if __name__ == '__main__':
    sys.exit(main())
