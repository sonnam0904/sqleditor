# Security Policy

## Supported versions

Security fixes are applied to the latest release and the `main` branch. Older releases may not receive patches.

| Version | Supported |
| ------- | --------- |
| Latest release | Yes |
| `main` branch | Yes |
| Older releases | No |

## Reporting a vulnerability

**Please do not report security vulnerabilities through public GitHub issues.**

If you discover a security issue in SQLEditor, report it privately by opening a
[GitHub Security Advisory](https://github.com/sonnam0904/sqleditor/security/advisories/new)
or contacting the repository owner through GitHub.

Include as much detail as possible:

- Description of the vulnerability and potential impact
- Steps to reproduce
- Affected versions or commits
- Proof of concept if available
- Suggested fix (optional)

## What to report

Examples of issues we care about:

- Remote code execution or arbitrary file access through connection handling
- Credential leakage (passwords, connection strings, SSH keys) in logs, crash dumps, or UI
- TLS/SSL verification bypass or man-in-the-middle weaknesses
- Memory safety issues that could lead to crashes or exploitation
- Insecure defaults in connection or SSH tunnel configuration

## What is out of scope

- Issues in third-party dependencies already fixed upstream (please report to the upstream project)
- Denial of service from intentionally malformed SQL sent to your own database server
- Social engineering or physical access attacks
- Vulnerabilities in database servers themselves (report to the database vendor)

## Response timeline

We aim to:

- Acknowledge your report within **5 business days**
- Provide an initial assessment within **10 business days**
- Keep you informed of progress until the issue is resolved

## Disclosure

We prefer coordinated disclosure. Please allow reasonable time for a fix before public disclosure. We will credit reporters in the advisory unless you prefer to remain anonymous.

## Safe harbor

We support good-faith security research. We will not pursue legal action against researchers who follow this policy and avoid privacy violations, data destruction, or service disruption.
