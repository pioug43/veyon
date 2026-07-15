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
| `networkBackend` | `hosts` or `firewall` | Linux network enforcement backend; default `hosts` |
| `allowedNetworks` | string list | CIDR/IP allowed for egress (firewall backend) |
| `dnsServers` | string list | DNS resolver IPs allowed through the firewall |
| `supervisionNetworks` | string list | CIDR of the portal/master always allowed through the firewall |

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
egress allow-list** (`policy drop`): only loopback, established/related
connections, DNS to the listed `dnsServers`, and the `allowedNetworks` /
`supervisionNetworks` CIDRs may leave the machine. Unlike the browser PAC, this
cannot be bypassed by a direct IP connection, DoH, an alternative resolver, or a
VPN to an arbitrary endpoint — enforcement is in the kernel and applies to every
process, not just policy-aware browsers.

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

## Platform capabilities

| Capability | Windows | Linux | macOS |
|---|---:|---:|---:|
| Kill already-running applications | Yes | Yes | Yes |
| Prevent application launch | Yes, IFEO | No | No |
| Site block list | PAC policies | `hosts` | `hosts` |
| Site allow list | PAC policies | Refused | Refused |
| Crash-safe restoration | Registry/PAC backup | Marked `hosts` section | Marked `hosts` section |

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

The digest is an integrity checksum, not a digital signature. Authenticity still
depends on Veyon's authenticated and encrypted control channel.

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
