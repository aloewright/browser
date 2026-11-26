# Machines: remote desktops in a tab

`chrome://ko-machines` lists your machines. **Open desktop** opens
`chrome://ko-machines/view?id=<id>` in a new tab, which shows the machine's
screen with [noVNC](https://github.com/novnc/noVNC) 1.7.0 (vendored in
`third_party/novnc/`). It works with macOS Screen Sharing and with any VNC
server.

There is no websockify or other relay. The browser opens the TCP connection
itself, and only to addresses inside your allowed networks.

> **Not compiled.** Like the rest of K-O, this was written without a Chromium
> checkout. Expect to fix build errors on the first build.

## How it works

```
chrome://ko-machines/view          browser process             network service
  noVNC RFB                         KOMachineBridge
    | MojoSocketChannel  --Connect(machine id)-->  look up host:port in prefs
    |   (WebSocket-like)                         resolve, keep allowlisted IPs
    |                                            CreateTCPConnectedSocket(IPs) -->  TCP
    |  <==== data pipes (raw RFB bytes) ===============================>
```

1. The page asks `KOMachineBridge.Connect(machine_id)`. It never passes an
   address: the browser reads the host and port from the `ko.machines.list`
   pref.
2. An IP literal is checked directly. A hostname is resolved through the network
   service with the system resolver (so NetBird DNS names work) and with secure
   DNS disabled (so machine names are not sent to a public DoH resolver).
3. Only resolved addresses inside the allowlist are kept. The browser connects
   to exactly those addresses, so a later DNS answer cannot redirect the
   connection. If none is left, the connection is refused.
4. After connecting, the peer address is checked against the allowlist again.
5. The socket's two data pipes go to the page. `MojoSocketChannel` wraps them in
   the WebSocket-like object noVNC's `RFB` accepts. The browser keeps the socket
   and closes it when the page drops its `MachineConnection` handle, reloads or
   closes the tab.

Code:

| Path                                | What                                                           |
| ----------------------------------- | -------------------------------------------------------------- |
| `components/ko/machines/core/`      | Machine model, prefs, CIDR allowlist, `machines.json` reader   |
| `components/ko/machines/browser/`   | `KOMachineBridge`, address resolver, handler, credential store |
| `components/ko/machines/common/`    | `ko_machines.mojom`                                            |
| `components/ko/machines/resources/` | React UI (Leo), `MojoSocketChannel`                            |
| `browser/ui/webui/ko_machines/`     | WebUI controller and config                                    |
| `third_party/novnc/`                | noVNC core and pako, unmodified, plus `core/rfb.d.ts`          |

## Security model

- **Only `chrome://ko-machines` can open sockets.** `MachinesHandler` and
  `KOMachineBridge` are registered on that WebUI's own interface broker
  (`registry.ForWebUI<KOMachinesUI>()` in `brave_content_browser_client.cc`). No
  other page, extension or renderer can bind them. The page is disabled in
  off-the-record profiles.
- **Allowlist, fail closed.** The bridge connects only to addresses inside
  `ko.machines.allowed_cidrs` (default `100.64.0.0/10`, the NetBird / CGNAT
  range). Every entry must lie entirely inside private space: `10.0.0.0/8`,
  `100.64.0.0/10`, `127.0.0.0/8`, `172.16.0.0/12`, `192.168.0.0/16`, `::1/128`
  or `fc00::/7`. Wider or public entries (for example `0.0.0.0/0`) are ignored
  and shown as ignored in the UI. An empty allowlist blocks every connection.
  Loopback is not allowed by default, so a DNS name that resolves to `127.0.0.1`
  is refused.
- **Plaintext RFB stays inside NetBird.** RFB itself is not encrypted: VNC and
  Apple Remote Desktop authentication protect only the password. NetBird's
  WireGuard tunnel encrypts the traffic between peers. Because the bridge
  refuses every destination outside the allowlist, a misconfigured host name or
  DNS answer cannot send plaintext VNC across the public internet. If you add a
  LAN range such as `192.168.1.0/24`, traffic to it is plaintext on that LAN.
- **Credentials.** A password (and, for macOS, the account name) can be saved
  per machine. It is encrypted with OSCrypt (on macOS, with a key held in the
  Keychain) and stored base64-encoded in `ko.machines.credentials`; nothing is
  stored if encryption is unavailable. It is never written to plain prefs, the
  command line or logs. Changing a machine's host forgets its saved password;
  removing a machine deletes it. The viewer asks the browser for the password
  only when the server requests credentials, and noVNC uses it for the challenge
  (VNC DES or ARD Diffie-Hellman with AES). A password typed into the viewer is
  saved only if you turn on **Save the password**, and only after the server
  accepts it.
- **Clipboard sharing is off** at the start of every session. When you turn it
  on, text the remote copies is written to your clipboard, and **Send
  clipboard** sends your clipboard text to the remote.
- **View only** stops all keyboard and pointer input.
- noVNC's RSA-AES security type asks the user to verify the server key. That is
  not supported yet; the viewer disconnects instead of trusting the key.
- At most 4 connections per page.

## Prefs

| Pref                        | Type | Default             | Notes                                      |
| --------------------------- | ---- | ------------------- | ------------------------------------------ |
| `ko.machines.list`          | list | `[]`                | `{id, name, host, port, kind}` per machine |
| `ko.machines.allowed_cidrs` | list | `["100.64.0.0/10"]` | CIDR strings                               |
| `ko.machines.credentials`   | dict | `{}`                | machine id to OSCrypt ciphertext (base64)  |
| `ko.machines.seeded`        | bool | `false`             | `machines.json` was read on first use      |

`kind` is `macos-screen-sharing` or `vnc`; `port` defaults to 5900. The list is
stored at `ko.machines.list`, not `ko.machines`: prefs are a nested dictionary,
so `ko.machines` has to be a dictionary for `ko.machines.allowed_cidrs` to sit
next to it.

These are profile prefs and are not synced.

## machines.json

Optional. Put it in the K-O data dir:

- macOS: `~/Library/Application Support/K-O/machines.json`
- Linux: `$XDG_DATA_HOME/k-o/machines.json` (default
  `~/.local/share/k-o/machines.json`)

It is never committed. `ko/machines/machines.example.json` shows the format:

```json
{
  "allowed_cidrs": ["100.64.0.0/10"],
  "machines": [
    {
      "id": "studio",
      "name": "Studio Mac",
      "host": "100.64.0.10",
      "port": 5900,
      "kind": "macos-screen-sharing"
    }
  ]
}
```

Both keys are optional, but one must be present. `id` may contain letters,
digits, `-` and `_`; it is generated if missing. `host` is a host name or IP
address without scheme, port or path. Comments and trailing commas are allowed.
Invalid entries are skipped. At most 64 machines.

The file is read when `chrome://ko-machines` is first opened in a profile.
**Import machines.json** reads it again: it adds machines whose id and host:port
are new (it never removes or changes existing ones), and if the file has
`allowed_cidrs`, replaces the allowlist with it. The allowlist can only be
changed this way, not from the page.

Because the file can widen the allowlist, it is refused unless it is a regular
file owned by you and not writable by group or others:

```bash
chmod 600 ~/Library/Application\ Support/K-O/machines.json
```

## Set up each Mac

### 1. Turn on Screen Sharing

1. Open **System Settings > General > Sharing** and turn on **Screen Sharing**.
   (Turn off **Remote Management** if it is on; only one of the two can run.)
2. Click the info button next to Screen Sharing. Set **Allow access for** to
   **Only these users** and add your account.
3. Optional: under **Computer Settings**, turn on **VNC viewers may control
   screen with password** and set a password. K-O prefers Apple Remote Desktop
   authentication (security type 30) with your macOS account and password, which
   noVNC 1.7.0 supports. Use the VNC password only if you do not want to use an
   account password; VNC passwords are limited to 8 characters.

In K-O, add the Mac as **macOS Screen Sharing**. When you connect, enter the
macOS account name and password (or leave the name empty to use the VNC
password).

### 2. Join NetBird

Install NetBird on the Mac and on the computer running K-O, and join both to
your network. Note the Mac's NetBird address (`100.64.x.y`) or DNS name from
`netbird status` or the NetBird dashboard, and use it as the machine's host.

In NetBird, add an access policy that allows TCP port 5900 to the Mac only from
the peers that should reach it.

### 3. Limit Screen Sharing to the NetBird interface

Screen Sharing listens on every interface. The macOS Application Firewall works
per app, not per interface, so use `pf` to accept port 5900 only on the NetBird
interface:

1. Find the NetBird interface (usually `utun100`):

   ```bash
   ifconfig | grep -B4 'inet 100\.'
   ```

2. Create `/etc/pf.anchors/ko.screensharing` (replace `utun100` if yours
   differs):

   ```
   # Screen Sharing (5900) and Remote Management (3283) only via NetBird.
   pass in quick on utun100 inet proto tcp from 100.64.0.0/10 to any port 5900 keep state
   block in quick proto { tcp, udp } to any port { 5900, 3283 }
   ```

3. Add these two lines to the end of `/etc/pf.conf`:

   ```
   anchor "ko.screensharing"
   load anchor "ko.screensharing" from "/etc/pf.anchors/ko.screensharing"
   ```

4. Load and enable `pf`:

   ```bash
   sudo pfctl -nf /etc/pf.conf   # check syntax
   sudo pfctl -f /etc/pf.conf
   sudo pfctl -E
   ```

5. Check: from another machine on your LAN (not over NetBird),
   `nc -vz <mac-lan-ip> 5900` must fail; over NetBird, `nc -vz 100.64.x.y 5900`
   must succeed.

`pf` is off after a reboot unless something enables it, and macOS updates can
reset `/etc/pf.conf`. To enable it at boot, add a LaunchDaemon that runs
`/sbin/pfctl -E -f /etc/pf.conf`, and re-check the anchor lines after each macOS
update.

## Other VNC servers

Add them as **VNC**. Bind the server to the NetBird address so it is not
reachable elsewhere, for example `x11vnc -listen 100.64.0.20 -rfbport 5901` or
`wayvnc 100.64.0.20 5901`.

## Viewer controls

- **View only**: watch without sending input.
- **Scale to window**: fit the remote screen to the tab (on by default). Off
  shows it at full size with scroll bars.
- **Share clipboard** (off by default) and **Send clipboard**.
- **Full screen**: press Esc to leave.
- **Disconnect** / **Reconnect**.

## Troubleshooting

| Message                                                              | Cause and fix                                                                                                    |
| -------------------------------------------------------------------- | ---------------------------------------------------------------------------------------------------------------- |
| "...was blocked because the address is not in your allowed networks" | The host resolved outside the allowlist. Check the host, or add its private range to `allowed_cidrs` and import. |
| "...could not be found. Check that NetBird is connected."            | The name did not resolve. Check `netbird status` and the NetBird DNS settings.                                   |
| "Could not connect to..."                                            | Screen Sharing is off, the NetBird policy blocks 5900, or `pf` blocks the interface.                             |
| "Sign-in failed."                                                    | Wrong account or password. ARD needs a macOS account allowed in Screen Sharing.                                  |
| "machines.json was not imported because other users can change it."  | `chmod 600` the file.                                                                                            |

## Tests

`brave_unit_tests` (only when `is_ko`):

- `KOAddressAllowlistTest`: CIDR parsing, private-space enforcement, IPv4-mapped
  addresses, filtering resolved endpoints.
- `KOMachineAddressResolverTest`: IP literals, host names resolving to allowed,
  public, loopback and mixed answers, resolution failures, empty allowlist.
- `KOMachineBridgeTest`: unknown machines and public destinations never reach
  `CreateTCPConnectedSocket`; only allowlisted addresses are dialed; an
  unexpected peer is rejected; a dropped connect reply fails closed; connection
  limit; sockets close with the page.
- `KOMachinesHandlerTest`, `KOCredentialStoreTest`: validation, encryption at
  rest, forgetting credentials on host change, seeding and import.
- `KOMachineTest`, `KOMachinesConfigTest`, `KOMachinesPrefsTest`.

```bash
pnpm run test brave_unit_tests --filter="KO*"
```
