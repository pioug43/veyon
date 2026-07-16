# ExamMode in an Omnissa-style VDI

ExamMode applies a leased exam profile to an endpoint. The broker or portal must
renew `StartExam` before `leaseSeconds` expires and send `StopExam` when the exam
ends. If the portal, connection server, or desktop agent disappears, the endpoint
restores its previous system state automatically when the lease expires.

## Profile arguments

| Argument | Type | Meaning |
|---|---|---|
| `blockedApps` | string list | Applications blocked on every platform |
| `blockedAppsWindows` | string list | Additional Windows image names |
| `blockedAppsLinux` | string list | Additional Linux process names |
| `blockedAppsMacos` | string list | Additional macOS process names |
| `sites` | string list | Host names; URLs are accepted only without path, query, credentials, or port |
| `sitesMode` | `block` or `allow` | Network policy mode |
| `leaseSeconds` | integer | Renewal lease, clamped to 60–3600 seconds; default 300 |
| `processRules` | object list | Typed `block`/`allow` rules by OS, with independent terminate/prevent-launch controls |
| `urlRules` | object list | Ordered allow/block rules; glob/domain expressions or regular expressions |
| `urlDefaultAction` | `allow` or `block` | Result when no ordered URL rule matches |
| `profileId` | string | Stable identity of the managed profile; default `legacy` |
| `profileRevision` | non-negative integer | Monotonic revision within a profile identity |
| `profileDigest` | SHA-256 hex string | Optional expected digest of the normalized effective profile |
| `strict` | boolean | Opt-in: also block shells, interpreters, and system tools (see below); default `false` |
| `networkBackend` | `hosts` or `firewall` | Network enforcement backend (Linux `hosts`/nftables; Windows `firewall` is explicitly refused until WFP support); default `hosts` |
| `allowedNetworks` | string list | CIDR/IP allowed for egress (firewall backend) |
| `dnsServers` | string list | DNS resolver IPs allowed through the firewall |
| `supervisionNetworks` | string list | CIDR of the portal/master always allowed through the firewall |
| `sessionId` / `sequence` | string / positive integer | Session identity and strictly increasing anti-replay counter |
| `issuedAt` / `expiresAt` | epoch milliseconds | Signed freshness window (maximum two hours) |
| `highSecurity` | boolean | Requires a valid RSA-SHA512 profile signature and strong network enforcement |
| `signingKeyId` / `profileSignature` | string / base64 | Veyon public-key name and signature of the canonical session/profile payload |
| `requiredCapabilities` | string list | Endpoint or externally attested controls which must be available |
| `externalCapabilities` | object | Broker controls attested as `ACTIVE`; covered by the signature |

Application, site, and network lists are normalized, deduplicated, and sorted
before they are applied. Invalid values are rejected and logged. Rule counts are
capped (512 process/URL/network rules, 4096 domains) to reject degenerate profiles.

## Strict mode (opt-in)

When `strict` is set, a platform-specific set of shells, script interpreters,
terminal emulators, process-management tools, and policy-editing tools is merged
into the block list (terminate **and** prevent-launch). This is defence in depth
against arbitrary code execution and against a student stopping ExamMode
(`taskmgr`, `regedit`, `sc`, terminals, `python`, `bash`, `powershell`, …). It is
opt-in because it breaks exams that legitimately need a terminal or interpreter.

## Linux firewall backend (experimental, opt-in)

`networkBackend: firewall` replaces the `hosts` block list with an **nftables
egress allow-list** (`policy drop`): only loopback, DNS to the listed
`dnsServers`, and the `allowedNetworks` /
`supervisionNetworks` CIDRs may leave the machine. Unlike the browser PAC, this
cannot be bypassed by a direct IP connection, DoH, an alternative resolver, or a
VPN to an arbitrary endpoint — enforcement is in the kernel and applies to every
process, not just policy-aware browsers. Existing connections receive no blanket
exception: a connection opened before activation is dropped unless its destination
is still allowed by the active profile.

It is **disabled by default and experimental**: validate it in a lab on your exact
image before production. Safeguards:

* **Fail-closed application**: if `nft` fails, no partial ruleset is left and the
  machine stays *open* (loudly logged) rather than half-blocked.
* **Dead-man timer**: applying the ruleset arms a fixed-name transient systemd
  timer (`veyon-exammode-failsafe`) that deletes the nftables table at
  `leaseSeconds + 60`. Each renewal pushes it back; if Veyon dies entirely the
  timer still restores connectivity, so a crash never isolates the VM for good.
* **Startup cleanup**: a residual table left by a crash is removed when the
  service restarts.

Requires root (the endpoint Service/Server component) and `nftables` +
`systemd-run` on the image. `allowedNetworks` drive an IP allow-list, so provide
the exam-server CIDRs (domain-only allow-listing is not possible at the firewall
layer — that is what the Windows PAC backend is for).

## Windows firewall backend

`networkBackend: firewall` is currently **refused on Windows** with the capability
`UNSUPPORTED_WFP_REQUIRED`. The former global-policy command implementation was
removed: it could not restore a non-default policy exactly and its recovery task
was locale-dependent. A production implementation must use a dedicated Windows
Filtering Platform provider/sublayer with dynamic, transactionally committed
filters and explicit VDI-control exceptions. Refusal is returned as `REJECTED`;
the endpoint is never reported as isolated when no safe backend exists.

## Platform capabilities

| Capability | Windows | Linux | macOS |
|---|---:|---:|---:|
| Kill already-running applications | Yes | Yes | Yes |
| Prevent application launch | Yes, IFEO | Yes, fanotify | No |
| Site block list | PAC policies | `hosts` / firewall | `hosts` |
| Site allow list | PAC policies | firewall (CIDR) | Refused |
| Egress allow-list (CIDR) | Refused pending WFP | firewall (nftables) | No |
| Crash-safe restoration | Exact Registry/PAC backup + active-state journal | Marked `hosts` / nftables dead-man + active-state journal | Marked `hosts` section |

## Linux launch prevention (fanotify)

On Linux, prevent-launch applications are enforced in the kernel via
`fanotify` with `FAN_OPEN_EXEC_PERM`: every `execve` on executable mounts is
submitted to a userspace permission decision, and a blocked binary is denied
*before* it runs. Unlike a `PATH` shim or an `LD_PRELOAD` shim, this covers all
launch paths — interactive shell, GUI launcher, absolute path, or a portable copy
carrying the same file name — and cannot be bypassed by ignoring `PATH`.

* **Privilege / kernel**: requires root (`CAP_SYS_ADMIN`) and kernel ≥ 5.0. If
  unavailable, a requested prevent-launch rule fails the transaction. The
  periodic kill loop remains defence in depth but is not reported as equivalent.
* **Fail-safe**: matching is a deny-list with default-allow, so a path that cannot
  be resolved is allowed (the OS is never broken by unknown execs). If the Veyon
  process dies, the kernel closes the fanotify descriptor and pending execs are
  allowed — the machine is never left unable to start programs.
* **Matching**: by file basename (case-insensitive, `.exe` suffix tolerated).
  Renaming a binary defeats name matching (documented limitation; a content-hash
  mode is the natural future enhancement). The periodic kill loop remains as a
  complementary layer for already-running and renamed processes.
* **Scope note**: with `strict`, interactive shells and interpreters are on the
  block list, so during an exam new `bash`/`python`/… launches are denied
  system-wide. `sh`/`dash` are deliberately excluded to avoid breaking session and
  system scripts. ExamMode never invokes a shell for its own operations, so strict
  mode does not disable its firewall/DNS/registry helpers.

The non-Windows allow-list mode is refused explicitly because a `hosts` file
cannot implement a safe allow list. The same applies to structured `urlRules`
and to `urlDefaultAction: block` (allow-list semantics through the default
action): on Linux/macOS the network part of such profiles is refused and logged
rather than silently degraded to a weaker policy. A future Linux/macOS backend
should use a managed local proxy or broker-controlled network policy, not
synthetic `hosts` entries.

Regular-expression URL rules are evaluated by the browser's PAC engine with
JavaScript `RegExp` semantics. A pattern that is valid PCRE but invalid
JavaScript never matches (the PAC wraps evaluation in `try/catch`), so prefer
glob/domain expressions unless JavaScript compatibility has been checked.

## Structured profile example

```json
{
  "profileId": "math-final-2026",
  "profileRevision": 7,
  "leaseSeconds": 300,
  "processRules": [
    { "active": true, "os": "windows", "executable": "teams.exe", "action": "block", "strongKill": true },
    { "active": true, "os": "windows", "executable": "calculator.exe", "action": "allow" },
    { "active": true, "os": "linux", "executable": "telegram-desktop", "action": "block",
      "terminateRunning": true, "preventLaunch": false }
  ],
  "urlRules": [
    { "active": true, "expression": "*://*.exam.example/*", "action": "allow" },
    { "active": true, "expression": "^https://[^/]+/chat", "regex": true, "action": "block" }
  ],
  "urlDefaultAction": "block"
}
```

`allow` process rules override matching `block` rules case-insensitively. URL
rules use first-match semantics. Invalid, inactive, or wrong-platform rules are
ignored and logged. A versioned profile is rejected before endpoint state changes
when its revision is stale, its content changes without a revision increment, or
its supplied digest does not match.

The digest remains an integrity checksum. In `highSecurity` mode, authenticity is
additionally enforced by RSA-SHA512 over a canonical payload containing the
digest, operation, session identity, sequence, freshness window, required
capabilities, and broker attestations. Public keys are loaded from Veyon's managed
public-key directory. The last accepted sequence and the active normalized policy
are atomically journaled with owner-only permissions, so a crash can resume an
unexpired session without accepting a replay.

Every start/stop produces a per-endpoint status reply: `PENDING`, `APPLIED`,
`REJECTED`, `DEGRADED`, `STOPPED`, or `EXPIRED`, plus an error code, effective
digest, capability map, backend results, session/sequence, and timestamp. The Web
API feature-status route merges this detail with the ordinary `active` flag.

## VDI client full-screen check (Windows + Linux)

While an exam is active, the 10-second drift monitor also verifies that the
Omnissa Horizon (formerly VMware Horizon) VDI client occupies an entire screen. A
windowed or merely *maximized* client leaves the physical host reachable behind
it, so the endpoint reports `DEGRADED` with error code `VDI_CLIENT_NOT_FULLSCREEN`.
The client process is matched by executable (`vmware-view` / `omnissa` /
`horizon`+`client`), robust to the VMware→Omnissa rename. Cross-platform, in
`ExamModeVdiClient`:

- **Windows** — `EnumWindows` + compare the window rectangle to the monitor's full
  bounds (`rcMonitor`, taskbar included), so a maximized-to-work-area window is
  correctly flagged as *not* full screen.
- **Linux (X11/EWMH)** — enumerate `_NET_CLIENT_LIST`, resolve the owning PID
  (`_NET_WM_PID` → `/proc/<pid>/comm`+`cmdline`), and treat a window as full screen
  if it carries `_NET_WM_STATE_FULLSCREEN` or geometrically covers the screen.
  Requires X11 (built when `find_package(X11)` succeeds; Wayland-native windows
  are not enumerable — falls back to *inconclusive*).

When no client window is visible (client absent, or the check runs without access
to the graphical session — e.g. as SYSTEM/root without a display) the result is
*inconclusive* and raises no violation, avoiding false positives. The alert is
self-resolving: once the student returns the client to full screen the status
clears back to `APPLIED`.

**At activation**, if the client is not already full screen, the endpoint first
tries to **force** it — Windows: bring the main window to the foreground and
extend it to cover the monitor (`SetWindowPos`, topmost); Linux: request
`_NET_WM_STATE_FULLSCREEN` from the window manager (EWMH). If forcing does not
achieve full screen, it shows a **non-blocking message in the student's session**
— Windows `WTSSendMessage` (works even from SYSTEM), Linux `notify-send`
(best-effort, needs the user session bus) — telling them they have **1 minute** to
go full screen, and grants a matching 60-second grace window during which the
`VDI_CLIENT_NOT_FULLSCREEN` violation is suppressed. After the grace elapses the
drift monitor reports the violation normally.

IFEO and fanotify rules in this plugin match executable basenames and are exposed
as `process.preventLaunch.basename`, not as strong application allow-listing. A
`highSecurity` profile requesting process prevention must therefore include a
signed `externalCapabilities.applicationControl: "ACTIVE"` attestation from a
broker/image policy such as WDAC/AppLocker or Linux IMA. This prevents a renamed
binary from being mistaken for a fully enforced application policy.

## Safe Exam Browser design influence

The model follows Safe Exam Browser's useful invariants: typed permitted and
prohibited process entries, ordered URL-filter rules, a configuration identity,
and backup-before-change with ownership-aware restoration. It deliberately does
not copy machine policies that are unsafe inside an Omnissa desktop. In
particular, ExamMode never disables remote connections, user switching, power
controls, or VDI transport components. Clipboard, screen capture, session
integrity, and VM-attestation controls remain outside this plugin until a
broker-aware implementation can preserve the remote exam session.

## Windows state ownership

Before changing browser policies or an IFEO `Debugger`, ExamMode persists every
previous value under `C:\ProgramData\Veyon`. Stop, lease expiry, plugin shutdown,
and the next service start restore those values. State files are retained when a
restoration fails so that the operation can be retried and diagnosed. If a GPO or
another administrator changes a managed value while an exam is active, ExamMode
detects that it no longer owns the value and does not overwrite the newer policy.

For pooled non-persistent desktops, keep the lease renewal interval below half of
`leaseSeconds`. For persistent desktops, monitor warnings about failed registry or
state-file restoration before returning a machine to the pool.
