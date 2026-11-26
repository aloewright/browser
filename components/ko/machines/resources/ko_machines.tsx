// Copyright (c) 2026 The Brave Authors. All rights reserved.
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this file,
// You can obtain one at https://mozilla.org/MPL/2.0/.

import * as React from 'react'
import { createRoot } from 'react-dom/client'
import { setIconBasePath } from '@brave/leo/react/icon'
import { MachineList } from './components/machine_list'
import { Viewer } from './components/viewer'
import './strings'

setIconBasePath('chrome://resources/brave-icons')

// chrome://ko-machines/ lists machines; /view?id=<id> shows one desktop.
function Root() {
  if (window.location.pathname === '/view') {
    const id = new URLSearchParams(window.location.search).get('id') ?? ''
    return <Viewer machineId={id} />
  }
  return <MachineList />
}

createRoot(document.getElementById('root')!).render(<Root />)
