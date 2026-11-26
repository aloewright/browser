# K-O

A personal browser built on the debloated Brave shell (`is_brave_debloated`),
with BYO model providers, multi-agent workspaces, machine desktops in tabs and
self-hosted telemetry. Enabled by GN arg `is_ko` (default: follows
`is_brave_debloated`); C++ uses `BUILDFLAG(IS_KO)` from
`brave/components/ko/buildflags/buildflags.h`, upstream `chromium_src`
overrides use the global define `KO_BUILD`.

Per-area docs live in `ko/docs/`. No secrets, hostnames or account IDs are
committed: everything environment-specific comes from local GN args, a local
config file or the macOS Keychain.
