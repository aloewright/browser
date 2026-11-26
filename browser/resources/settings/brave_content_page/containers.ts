// Copyright (c) 2025 The Brave Authors. All rights reserved.
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this file,
// You can obtain one at https://mozilla.org/MPL/2.0/.

import '//resources/cr_elements/cr_icon_button/cr_icon_button.js'
import '//resources/mojo/skia/public/mojom/skcolor.mojom-webui.js'
import '//resources/brave/leo.bundle.js'
import '../controls/settings_toggle_button.js'

import { I18nMixinLit } from '//resources/cr_elements/i18n_mixin_lit.js'
import { assert } from '//resources/js/assert.js'
import { loadTimeData } from '//resources/js/load_time_data.js'
import { CrInputElement } from 'chrome://resources/cr_elements/cr_input/cr_input.js'
import {
  CrLitElement,
  PropertyValues,
} from 'chrome://resources/lit/v3_0/lit.rollup.js'
import { PrefServiceObserverMixinLit } from '/shared/settings/prefs2/pref_service_observer_mixin_lit.js'

import { ContainersStrings } from '../brave_generated_resources_webui_strings.js'
import {
  Container,
  ContainerOperationError,
  ContainerProxy,
  Icon,
} from '../containers.mojom-webui.js'

import backgroundColors from './background_colors.js'
import { getCss } from './containers.css.js'
import { getHtml } from './containers.html.js'
import type { ColorSelectedEvent } from './containers_background_chip.js'
import { ContainersSettingsHandlerBrowserProxy } from './containers_browser_proxy.js'
import type { IconSelectedEvent } from './containers_icon.js'
import { DragReorderMixin, getDragReorderCss } from './drag_reorder_mixin.js'

const SettingsBraveContentContainersElementBase = DragReorderMixin(
  PrefServiceObserverMixinLit(I18nMixinLit(CrLitElement)),
)

/**
 * 'settings-brave-content-containers' is the settings page containing settings for Containers
 */
export class SettingsBraveContentContainersElement extends SettingsBraveContentContainersElementBase {
  static get is() {
    return 'settings-brave-content-containers'
  }

  static override get styles() {
    return [getCss(), getDragReorderCss()]
  }

  override render() {
    return getHtml.bind(this)()
  }

  static override get properties() {
    return {
      containersEnabledPref_: {
        type: Object,
      },
      containersList_: {
        type: Array,
      },
      editingContainer_: {
        type: Object,
      },
      deletingContainer_: {
        type: Object,
      },
      isEditDialogNameInvalid_: {
        type: Boolean,
      },
      editDialogError_: {
        type: String,
      },
      deleteDialogError_: {
        type: String,
      },
      editingProxyUrl_: {
        type: String,
      },
      editingProxyUsername_: {
        type: String,
      },
      editingProxyPassword_: {
        type: String,
      },
      keepProxyCredentials_: {
        type: Boolean,
      },
    }
  }

  private browserProxy = ContainersSettingsHandlerBrowserProxy.getInstance()
  accessor containersEnabledPref_:
    | chrome.settingsPrivate.PrefObject<boolean>
    | undefined
  accessor containersList_: Container[] = []
  accessor editingContainer_: Container | undefined
  accessor deletingContainer_: Container | undefined
  accessor isEditDialogNameInvalid_ = false
  accessor editDialogError_: string | undefined
  accessor deleteDialogError_: string | undefined
  // K-O: per-container proxy. The browser never sends credentials back; the
  // password only lives here until the dialog closes.
  readonly isKOBuild_ = loadTimeData.getBoolean('isKOBuild')
  accessor editingProxyUrl_ = ''
  accessor editingProxyUsername_ = ''
  accessor editingProxyPassword_ = ''
  accessor keepProxyCredentials_ = false

  override connectedCallback() {
    super.connectedCallback()
    this.browserProxy.handler.getContainers().then(({ containers }) => {
      this.onContainersListUpdated_(containers)
    })
    this.mirrorPref('brave.containers.enabled', 'containersEnabledPref_')
    this.browserProxy.callbackRouter.onContainersChanged.addListener(
      this.onContainersListUpdated_.bind(this),
    )
  }

  override updated(changedProperties: PropertyValues<this>) {
    super.updated(changedProperties)
    if (
      changedProperties.has('containersEnabledPref_')
      && !this.containersEnabledPref_?.value
    ) {
      this.editingContainer_ = undefined
      this.deletingContainer_ = undefined
    }
  }

  onContainersListUpdated_(containers: Container[]) {
    this.containersList_ = containers
    this.onReorderableItemsUpdated_()
  }

  override getReorderableIds_(): string[] {
    return this.containersList_.map((c) => c.id)
  }

  override onItemsReordered_(orderedIds: string[]) {
    this.browserProxy.handler
      .reorderContainers(orderedIds)
      .then(({ error }) => {
        if (!error) {
          this.onReorderableItemsUpdated_()
        }
      })
      .catch(() => this.onReorderableItemsUpdated_())
  }

  onAddContainerClick_() {
    this.editingContainer_ = {
      id: '',
      name: '',
      icon: Icon.kDefault,
      backgroundColor: backgroundColors[0],
      proxy: null,
    }
    this.resetProxyFields_(null)
    this.editDialogError_ = undefined
  }

  onEditContainerClick_(e: Event) {
    const id = (e.currentTarget as HTMLElement).dataset['id']
    assert(id)
    this.editingContainer_ = this.containersList_.find((c) => c.id === id)
    this.resetProxyFields_(this.editingContainer_?.proxy ?? null)
    this.editDialogError_ = undefined
  }

  onDeleteContainerClick_(e: Event) {
    const id = (e.currentTarget as HTMLElement).dataset['id']
    assert(id)
    this.deletingContainer_ = this.containersList_.find((c) => c.id === id)
    this.deleteDialogError_ = undefined
  }

  onDialogClose_() {
    this.editingContainer_ = undefined
    this.deletingContainer_ = undefined
    this.resetProxyFields_(null)
  }

  onCancelDialogClick_() {
    this.onDialogClose_()
  }

  onContainerNameInput_(e: InputEvent) {
    assert(this.editingContainer_)
    const input = e.target as CrInputElement
    this.editingContainer_ = {
      ...this.editingContainer_,
      name: input.value,
    }
    this.isEditDialogNameInvalid_ = input.invalid
  }

  onContainersIconSelected_(event: IconSelectedEvent) {
    assert(this.editingContainer_)
    this.editingContainer_ = {
      ...this.editingContainer_,
      icon: event.detail.icon,
    }
  }

  onContainersBackgroundSelected_(event: ColorSelectedEvent) {
    assert(this.editingContainer_)
    this.editingContainer_ = {
      ...this.editingContainer_,
      backgroundColor: event.detail.color,
    }
  }

  onProxyUrlInput_(e: InputEvent) {
    this.editingProxyUrl_ = (e.target as CrInputElement).value
  }

  onProxyUsernameInput_(e: InputEvent) {
    this.editingProxyUsername_ = (e.target as CrInputElement).value
  }

  onProxyPasswordInput_(e: InputEvent) {
    this.editingProxyPassword_ = (e.target as CrInputElement).value
  }

  onClearProxyCredentialsClick_() {
    this.keepProxyCredentials_ = false
  }

  private resetProxyFields_(proxy: ContainerProxy | null) {
    this.editingProxyUrl_ = proxy?.url ?? ''
    this.editingProxyUsername_ = ''
    this.editingProxyPassword_ = ''
    this.keepProxyCredentials_ = proxy?.hasCredentials ?? false
  }

  // Credentials go to the browser embedded in the URL, percent-encoded.
  private buildProxy_(): ContainerProxy | null {
    if (!this.isKOBuild_) {
      return null
    }
    const url = this.editingProxyUrl_.trim()
    if (!url) {
      return null
    }
    const separator = url.indexOf('://')
    const username = this.editingProxyUsername_
    if (!username || separator < 0 || url.includes('@')) {
      return { url, hasCredentials: !username && this.keepProxyCredentials_ }
    }
    const password = this.editingProxyPassword_
    const userinfo =
      encodeURIComponent(username)
      + (password ? ':' + encodeURIComponent(password) : '')
    return {
      url: url.slice(0, separator + 3) + userinfo + '@'
        + url.slice(separator + 3),
      hasCredentials: false,
    }
  }

  async onSaveContainerFromDialogClick_() {
    assert(this.editingContainer_)
    this.editingContainer_ = {
      ...this.editingContainer_,
      proxy: this.buildProxy_(),
    }
    if (!this.editingContainer_.id) {
      const { error } = await this.browserProxy.handler.addContainer(
        this.editingContainer_,
      )
      if (!error) {
        this.onDialogClose_()
      } else {
        this.editDialogError_ = this.formatError_(error)
      }
    } else {
      const { error } = await this.browserProxy.handler.updateContainer(
        this.editingContainer_,
      )
      if (!error) {
        this.onDialogClose_()
      } else {
        this.editDialogError_ = this.formatError_(error)
      }
    }
  }

  async onDeleteContainerFromDialogClick_() {
    assert(this.deletingContainer_)
    const { error } = await this.browserProxy.handler.removeContainer(
      this.deletingContainer_.id,
    )
    if (!error) {
      this.deletingContainer_ = undefined
    } else {
      this.deleteDialogError_ = this.formatError_(error)
    }
  }

  getEditDialogTitle_(): string {
    return this.editingContainer_?.id
      ? this.i18n(ContainersStrings.SETTINGS_CONTAINERS_EDIT_CONTAINER_LABEL)
      : this.i18n(ContainersStrings.SETTINGS_CONTAINERS_ADD_CONTAINER_LABEL)
  }

  private formatError_(error: ContainerOperationError): string {
    let errorMessage = ''
    switch (error) {
      case ContainerOperationError.kNotFound:
        // This is the only error we expect in normal operation.
        errorMessage = this.i18n(
          ContainersStrings.SETTINGS_CONTAINERS_ERROR_NOT_FOUND,
        )
        break
      case ContainerOperationError.kInvalidProxy:
        errorMessage = this.i18n(
          ContainersStrings.SETTINGS_CONTAINERS_ERROR_INVALID_PROXY,
        )
        break
      case ContainerOperationError.kProxyCredentialsUnavailable:
        errorMessage = this.i18n(
          ContainersStrings.SETTINGS_CONTAINERS_ERROR_PROXY_CREDENTIALS_UNAVAILABLE,
        )
        break
      default:
        // These errors are not expected and should be fixed. Just display the
        // error code for debugging.
        errorMessage = ContainerOperationError[error]
        break
    }
    return this.i18n(
      ContainersStrings.SETTINGS_CONTAINERS_ERROR_TEMPLATE,
      errorMessage,
    )
  }
}

declare global {
  interface HTMLElementTagNameMap {
    'settings-brave-content-containers': SettingsBraveContentContainersElement
  }
}

customElements.define(
  SettingsBraveContentContainersElement.is,
  SettingsBraveContentContainersElement,
)
