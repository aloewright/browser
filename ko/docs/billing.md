# K-O: no Brave billing

K-O has no Brave subscription or purchase flow. Under `BUILDFLAG(IS_KO)`:

- **SKUs** (`components/skus`, `browser/skus`): `kSkusFeature` is off by
  default and `SkusServiceFactory` never builds the service, even if the
  feature is forced on. The `skus::mojom::SkusService` frame binder stays
  registered but drops the receiver, so a stray request disconnects instead
  of killing the renderer. No `window.chrome.braveSkus` on account.brave.com
  pages, no `brave://skus-internals`. Nothing calls the SKU backend.
- **Brave Origin** (`components/brave_origin`): `IsBraveOriginFeatureEnabled()`
  is always false, so there is no purchase check, settings section, upsell or
  checkout link. The service no longer watches SKU credentials and ignores the
  Linux free tier. The Origin startup dialog only exists in Origin-branded
  builds, and `is_ko` with `is_brave_origin_branded` fails at `gn gen`. The
  service itself stays, because Brave's policy providers wait on its policy
  manager.
- **Brave Account**: already off in the debloat
  (`IsBraveAccountEnabled()` is false); every entry point checks it.
- **VPN, Talk, Rewards, Wallet, Leo**: compiled out by the debloat, including
  their purchase state, menus and NTP widgets. Leo premium code belongs to the
  ai-providers area.

Tests that only cover the removed purchase flows are skipped when `is_ko`.
`brave_origin_ko_unittest.cc` and `skus_ko_browsertest.cc` check that the
SKU and Origin purchase paths stay inert in K-O. Not covered: iOS
(`ios/browser/skus/skus_service_factory.mm` only checks `kSkusFeature`) and
Android's Origin in-app subscription binding still follow the feature flags.
