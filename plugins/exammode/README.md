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

Application and site lists are normalized, deduplicated, and sorted before they
are applied. Invalid site values are rejected and logged.

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
