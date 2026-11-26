/* Copyright (c) 2024 The Brave Authors. All rights reserved.
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at https://mozilla.org/MPL/2.0/. */

import 'chrome://resources/cr_elements/cr_button/cr_button.js'
import 'chrome://resources/cr_elements/icons.html.js'

import { sendWithPromise } from 'chrome://resources/js/cr.js'
import { I18nMixin } from 'chrome://resources/cr_elements/i18n_mixin.js'
import { loadTimeData } from 'chrome://resources/js/load_time_data.js'
import { PolymerElement } from 'chrome://resources/polymer/v3_0/polymer/polymer_bundled.min.js'
import * as mojom from './brave_leo_assistant_browser_proxy.js'
import { BaseMixin } from '../base_mixin.js'
import { getTemplate } from './model_config_ui.html.js'

// Leo control events forward their detail object as the event payload directly.
type LeoInputEvent = { value: string; valueAsNumber?: number }
type LeoToggleEvent = { checked: boolean }

// K-O provider presets: they only fill in the form. Requests go straight to
// the provider and the key is stored like any custom model's (encrypted).
type ProviderPreset = 'custom' | 'cloudflare' | 'nanogpt'
const NANOGPT_ENDPOINT = 'https://nano-gpt.com/api/v1/chat/completions'
const CLOUDFLARE_ACCOUNT_ID = /^[0-9a-f]{32}$/i
const CLOUDFLARE_GATEWAY_ID = /^[A-Za-z0-9_-]{1,64}$/

function getCloudflareEndpoint(accountId: string, gatewayId: string) {
  return `https://gateway.ai.cloudflare.com/v1/${accountId}/${gatewayId}`
    + '/compat/chat/completions'
}

const isKOBuild = loadTimeData.getBoolean('isKOBuild')

const ModelConfigUIBase = I18nMixin(BaseMixin(PolymerElement))

export class ModelConfigUI extends ModelConfigUIBase {
  static get is() {
    return 'model-config-ui'
  }

  static get template() {
    return getTemplate()
  }

  static get properties() {
    return {
      label: {
        type: String
      },
      modelRequestName: {
        type: String
      },
      contextSize: {
        type: Number
      },
      modelSystemPrompt: {
        type: String,
        value: ''
      },
      promptTokensEstimate: {
        type: Number,
        value: 0
      },
      promptTokensEstimateString: {
        type: String,
        value: ''
      },
      endpointUrl: {
        type: String,
        value: ''
      },
      apiKey: {
        type: String,
        value: ''
      },
      isUrlInvalid: {
        type: Boolean,
        value: false,
      },
      shouldShowUnsafeEndpointModal: {
        type: Boolean,
        value: false,
      },
      shouldShowUnsafeEndpointLabel: {
        type: Boolean,
        value: false,
      },
      invalidUrlErrorMessage: {
        type: String,
        value: ''
      },
      modelItem: {
        type: Object,
        value: null,
        notify: true,
        observer: 'onModelItemChange_'
      },
      isEditing_: {
        type: Boolean,
        value: false,
        readOnly: true,
        computed: 'computeIsEditing_(modelItem)'
      },
      buttonLabel_: {
        type: String,
        computed: 'computeButtonLabel_(isEditing_)'
      },
      hasVisionSupport : {
        type: Boolean
      },
      supportsTools: {
        type: Boolean
      },
      isKOBuild_: {
        type: Boolean,
        value: isKOBuild,
      },
      showProviderPresets_: {
        type: Boolean,
        computed: 'computeShowProviderPresets_(isEditing_)'
      },
      providerPreset_: {
        type: String,
        value: 'custom',
      },
      cloudflareAccountId_: {
        type: String,
        value: '',
      },
      cloudflareGatewayId_: {
        type: String,
        value: '',
      },
      isCloudflareIdsInvalid_: {
        type: Boolean,
        value: false,
      },
      apiKeyInputType_: {
        type: String,
        // K-O never shows keys, even the one being typed.
        value: isKOBuild ? 'password' : 'text',
      },
    }
  }

  declare label: string
  declare modelRequestName: string
  declare contextSize: number
  declare modelSystemPrompt: string | null
  declare promptTokensEstimate: number
  declare promptTokensEstimateString: string
  declare endpointUrl: string
  declare apiKey: string
  declare modelItem: mojom.Model | null
  declare isEditing_: boolean
  declare buttonLabel_: string
  declare isUrlInvalid: boolean
  declare shouldShowUnsafeEndpointLabel: boolean
  isValidAsPrivateEndpoint: boolean
  declare shouldShowUnsafeEndpointModal: boolean
  declare invalidUrlErrorMessage: string
  declare hasVisionSupport: boolean
  declare supportsTools: boolean
  declare isKOBuild_: boolean
  declare showProviderPresets_: boolean
  declare providerPreset_: ProviderPreset
  declare cloudflareAccountId_: string
  declare cloudflareGatewayId_: string
  declare isCloudflareIdsInvalid_: boolean
  declare apiKeyInputType_: string

  override ready() {
    super.ready()
    // If a user previously had --brave-ai-chat-allow-private-ips enabled, but
    // now has it disabled, they should be notified of invalid endpoints
    // immediately upon opening the config view of an impacted model. We should
    // not wait for the user to make a change to the endpoint before informing
    // them that the endpoint is no longer valid.
    this.checkEndpointValidity_()
  }

  handleClick_() {
    // If the user is attempting to use a private endpoint, we should show a
    // modal warning instructing them to enable the optional feature in order
    // to proceed
    this.shouldShowUnsafeEndpointModal =
      this.isUrlInvalid && this.isValidAsPrivateEndpoint

    if (!this.saveEnabled_()) {
      return
    }

    // Send empty string if adding new model
    const modelKey = this.isEditing_ ? this.modelItem?.key : ''

    if (modelKey === undefined) {
      console.error('Model key is undefined')
      return
    }

    const modelConfig: mojom.Model = {
      options: {
        customModelOptions: {
          modelRequestName: this.modelRequestName,
          contextSize: this.contextSize,
          // Determined at runtime based on contextSize
          maxAssociatedContentLength: -1,
          // Determined at runtime based on contextSize
          longConversationWarningCharacterLimit: -1,
          modelSystemPrompt: this.modelSystemPrompt,
          endpoint: this.endpointUrl,
          apiKey: this.apiKey
        }
      },
      key: modelKey,
      displayName: this.label,
      visionSupport: this.hasVisionSupport,
      audioSupport: false,
      videoSupport: false,
      supportsTools: this.supportsTools,
      supportedCapabilities: [],
      isSuggestedModel: false,
      isNearModel: false,
      supportsPrivateInference: false,
    };

    this.fire('save', {modelConfig})
  }

  handleCloseClick_() {
    this.fire('close')
  }

  onModelLabelChange_(e: LeoInputEvent) {
    this.label = e.value
  }

  onModelRequestNameChange_(e: LeoInputEvent) {
    this.modelRequestName = e.value
  }

  onModelSystemPromptChange_(e: LeoInputEvent) {
    this.modelSystemPrompt = e.value;
  }

  onContextSizeChange_(e: LeoInputEvent) {
    this.contextSize = e.valueAsNumber!
  }

  onModelServerEndpointChange_(e: LeoInputEvent) {
    this.endpointUrl = e.value
    this.checkEndpointValidity_()
  }

  onModelApiKeyChange_(e: LeoInputEvent) {
    this.apiKey = e.value
  }

  constructTokenEstimateString_(e?: LeoInputEvent) {
    let charsLength = 0
    let tokensLength = 0

    if (e?.value) {
      charsLength = e.value.length
    } else if (this.modelSystemPrompt) {
      charsLength = this.modelSystemPrompt.length
    }

    tokensLength = charsLength > 0 ? Math.ceil(charsLength/4) : 0;

    this.promptTokensEstimate = tokensLength
    this.promptTokensEstimateString = this.i18n(
      'braveLeoAssistantTokensCount',
      this.promptTokensEstimate
    )
  }

  onVisionSupportChanged_(e: LeoToggleEvent) {
    this.hasVisionSupport = e.checked
  }

  onSupportsToolsChanged_(e: LeoToggleEvent) {
    this.supportsTools = e.checked
  }

  onProviderPresetChange_(e: LeoInputEvent) {
    this.providerPreset_ = e.value as ProviderPreset
    if (this.providerPreset_ === 'nanogpt') {
      this.label = this.label || this.i18n('koAssistantProviderPresetNanoGpt')
      this.endpointUrl = NANOGPT_ENDPOINT
      this.supportsTools = true
      this.checkEndpointValidity_()
    } else if (this.providerPreset_ === 'cloudflare') {
      this.label =
        this.label || this.i18n('koAssistantProviderPresetCloudflare')
      this.supportsTools = true
      this.updateCloudflareEndpoint_()
    }
  }

  onCloudflareAccountIdChange_(e: LeoInputEvent) {
    this.cloudflareAccountId_ = e.value.trim()
    this.updateCloudflareEndpoint_()
  }

  onCloudflareGatewayIdChange_(e: LeoInputEvent) {
    this.cloudflareGatewayId_ = e.value.trim()
    this.updateCloudflareEndpoint_()
  }

  private updateCloudflareEndpoint_() {
    const accountId = this.cloudflareAccountId_
    const gatewayId = this.cloudflareGatewayId_
    const isValid = CLOUDFLARE_ACCOUNT_ID.test(accountId)
      && CLOUDFLARE_GATEWAY_ID.test(gatewayId)
    this.isCloudflareIdsInvalid_ =
      !isValid && (accountId !== '' || gatewayId !== '')
    // An empty endpoint keeps Save disabled until both IDs are valid.
    this.endpointUrl = isValid ? getCloudflareEndpoint(accountId, gatewayId) : ''
    this.checkEndpointValidity_()
  }

  // Template-only helpers are protected so noUnusedLocals doesn't flag them.
  protected computeShowProviderPresets_(isEditing: boolean) {
    return this.isKOBuild_ && !isEditing
  }

  protected isPreset_(preset: ProviderPreset, value: ProviderPreset) {
    return preset === value
  }

  protected computeShowApiKeyKeepNote_(isEditing: boolean) {
    return this.isKOBuild_ && isEditing
  }

  private saveEnabled_() {
    // Make sure all required fields are filled
    return !!this.label?.trim()
        && !!this.modelRequestName?.trim()
        && !!this.endpointUrl?.trim()
        && !this.isUrlInvalid
  }

  private checkEndpointValidity_() {
    const url = this.endpointUrl.trim()
    if (url !== '') {
      sendWithPromise<{
        isValid: boolean
        isValidAsPrivateEndpoint: boolean
        isValidDueToPrivateIPsFeature: boolean
      }>('validateModelEndpoint', { url })
        .then((response) => {
          this.isUrlInvalid = !response.isValid
          this.isValidAsPrivateEndpoint = response.isValidAsPrivateEndpoint
          this.shouldShowUnsafeEndpointLabel =
            response.isValidDueToPrivateIPsFeature
          this.invalidUrlErrorMessage =
            this.i18n('braveLeoAssistantEndpointError')
        })
    }
  }

  private onModelItemChange_(newValue: mojom.Model | null) {
    if (newValue?.options.customModelOptions) {
      this.label = newValue.displayName;
      this.modelRequestName =
        newValue.options.customModelOptions.modelRequestName;
      this.contextSize =
        newValue.options.customModelOptions.contextSize;
      this.endpointUrl = newValue.options.customModelOptions.endpoint;
      this.apiKey = newValue.options.customModelOptions.apiKey;
      this.modelSystemPrompt =
        newValue.options.customModelOptions.modelSystemPrompt;
      this.hasVisionSupport = newValue.visionSupport;
      this.supportsTools = newValue.supportsTools
    }
    this.constructTokenEstimateString_()
  }

  private computeIsEditing_(modelItem: mojom.Model | null) {
    return !!modelItem
  }

  private computeButtonLabel_(isEditing: boolean) {
    return isEditing
      ? this.i18n('braveLeoAssistantSaveModelButtonLabel')
      : this.i18n('braveLeoAssistantAddModelButtonLabel')
  }
}

customElements.define(ModelConfigUI.is, ModelConfigUI)
