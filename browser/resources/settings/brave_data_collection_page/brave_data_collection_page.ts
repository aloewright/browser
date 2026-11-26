/* Copyright (c) 2024 The Brave Authors. All rights reserved.
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at https://mozilla.org/MPL/2.0/. */

import {WebUiListenerMixin} from '/shared/settings/web_ui_listener_mixin.js'
import { assert } from 'chrome://resources/js/assert.js'
import {SettingsToggleButtonElement} from '../controls/settings_toggle_button.js'
import {PolymerElement} from 'chrome://resources/polymer/v3_0/polymer/polymer_bundled.min.js'
import { SettingsViewMixin } from '../settings_page/settings_view_mixin.js'

import {BraveDataCollectionBrowserProxyImpl} from './brave_data_collection_browser_proxy.js'
import {getTemplate} from './brave_data_collection_page.html.js'

import {MetricsReporting} from '/shared/settings/privacy_page/privacy_page_browser_proxy.js'

import {loadTimeData} from "../i18n_setup.js"

const SettingBraveDataCollectionPageElementBase =
  WebUiListenerMixin(SettingsViewMixin(PolymerElement))

// K-O: self-hosted telemetry; keys only exist in Brave settings WebUI.
function getOptionalBoolean(key: string): boolean {
  return loadTimeData.valueExists(key) && loadTimeData.getBoolean(key)
}

/**
 * 'settings-brave-data-collection-page' is the settings page containing
 * Brave's data collection settings.
 */
class SettingsBraveDataCollectionPageElement
extends SettingBraveDataCollectionPageElementBase
{
  static get is() {
    return 'settings-brave-data-collection-subpage'
  }

  static get template() {
    return getTemplate()
  }

  static get properties() {
    return {
      p3aEnabled_: Boolean,
      statsUsagePingEnabled_: Boolean,
      metricsReportingEnabled_: Boolean,
      isMetricsReportingManaged_: Boolean,
      showRestartForMetricsReporting_: Boolean,
      showSponsoredAdsEnabledToggle_: Boolean,
      isStatsReportingEnabledManaged_: Boolean,
      isP3AEnabledManaged_: Boolean,
      showMetricsReporting_: Boolean,
      showKOAnalytics_: Boolean,
      metricsReportingLabel_: String,
      metricsReportingSubLabel_: String,
    }
  }

  private declare p3aEnabled_: boolean
  private declare statsUsagePingEnabled_: boolean
  private declare metricsReportingEnabled_: boolean
  private declare isMetricsReportingManaged_: boolean
  private hasMetricsReporting_ = false
  private declare showRestartForMetricsReporting_: boolean
  private declare showSponsoredAdsEnabledToggle_: boolean
  private declare isStatsReportingEnabledManaged_: boolean
  private declare isP3AEnabledManaged_: boolean
  private declare showMetricsReporting_: boolean
  private declare showKOAnalytics_: boolean
  private declare metricsReportingLabel_: string
  private declare metricsReportingSubLabel_: string

  browserProxy_ = BraveDataCollectionBrowserProxyImpl.getInstance()

  override ready() {
    super.ready()

    const setP3AEnabled = (userEnabled: boolean, isManaged: boolean) =>
      this.setP3AEnabled_(userEnabled, isManaged)

    this.isStatsReportingEnabledManaged_ = loadTimeData.getBoolean('isStatsReportingEnabledManaged')
    this.isP3AEnabledManaged_ = loadTimeData.getBoolean('isP3AEnabledManaged')

    // Debloated builds have no crash endpoint unless K-O configures one.
    const koCrashReporting = getOptionalBoolean('koCrashReportingAvailable')
    this.showMetricsReporting_ =
      !loadTimeData.getBoolean('isBraveDebloatedBuild') || koCrashReporting
    this.showKOAnalytics_ = getOptionalBoolean('koAnalyticsAvailable')
    this.metricsReportingLabel_ = loadTimeData.getString(
      koCrashReporting ? 'koCrashReportsTitle' : 'enablePersonalizationLogging')
    this.metricsReportingSubLabel_ = loadTimeData.getString(
      koCrashReporting ? 'koCrashReportsDesc'
                       : 'enablePersonalizationLoggingDesc')

    this.addWebUiListener('p3a-enabled-changed', setP3AEnabled)
    this.browserProxy_.getP3AEnabled().then(
      (enabled: boolean) => setP3AEnabled(enabled, this.isP3AEnabledManaged_))

    const setMetricsReporting = (metricsReporting: MetricsReporting) =>
        this.setMetricsReporting_(metricsReporting)
    this.addWebUiListener('metrics-reporting-change', setMetricsReporting)
    this.browserProxy_.getMetricsReporting().then(setMetricsReporting)

    const setStatsUsagePingEnabled = (userEnabled: boolean, isManaged: boolean) =>
      this.setStatsUsagePingEnabled_(userEnabled, isManaged)
    this.addWebUiListener(
      'stats-usage-ping-enabled-changed', setStatsUsagePingEnabled)
    this.browserProxy_.getStatsUsagePingEnabled().then(
      (enabled: boolean) => setStatsUsagePingEnabled(enabled, this.isStatsReportingEnabledManaged_))

    this.showSponsoredAdsEnabledToggle_ = loadTimeData.getBoolean('isSponsoredAdsAllowed')
  }

  setP3AEnabled_(userEnabled: boolean, isManaged: boolean) {
    this.p3aEnabled_ = userEnabled
    this.isP3AEnabledManaged_ = isManaged
  }

  onP3AEnabledChange_(event: Event) {
    const target = event.target
    assert(target instanceof SettingsToggleButtonElement)
    this.browserProxy_.setP3AEnabled(target.checked)
  }

  setStatsUsagePingEnabled_(userEnabled: boolean, isManaged: boolean) {
    this.statsUsagePingEnabled_ = userEnabled
    this.isStatsReportingEnabledManaged_ = isManaged
  }

  onStatsUsagePingEnabledChange_(event: Event) {
    const target = event.target
    assert(target instanceof SettingsToggleButtonElement)
    this.browserProxy_.
      setStatsUsagePingEnabled(target.checked)
  }

  // Metrics related code is copied from
  // chrome/browser/resources/settings/privacy_page/personalization_options.ts
  // as we hide that upstream option and put it in this page.
  // The toggle sits in a dom-if, so read it from the event, not this.$.
  onMetricsReportingChange_(event: Event) {
    const target = event.target
    assert(target instanceof SettingsToggleButtonElement)
    this.browserProxy_.setMetricsReportingEnabled(target.checked)
  }

  setMetricsReporting_(metricsReporting: MetricsReporting) {
    const hadPreviousValue = this.hasMetricsReporting_
    this.hasMetricsReporting_ = true
    this.metricsReportingEnabled_ = metricsReporting.enabled
    this.isMetricsReportingManaged_ = metricsReporting.managed

    // TODO(dbeam): remember whether metrics reporting was enabled when Chrome
    // started.
    if (metricsReporting.managed) {
      this.showRestartForMetricsReporting_ = false
    } else if (hadPreviousValue) {
      this.showRestartForMetricsReporting_ = true
    }
  }

  restartBrowser_(e: Event) {
    e.stopPropagation()
    window.open("chrome://restart", "_self")
  }
}

customElements.define(
  SettingsBraveDataCollectionPageElement.is,
  SettingsBraveDataCollectionPageElement)
