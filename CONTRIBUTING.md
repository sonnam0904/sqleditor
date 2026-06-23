# Contributing to SQLEditor

Thank you for your interest in contributing to SQLEditor! This document explains how to get started and what we expect from contributions.

## Before you start

- Read the [README](README.md) for build instructions and project overview.
- Review the [Code of Conduct](CODE_OF_CONDUCT.md).
- Check [existing issues](https://github.com/sonnam0904/sqleditor/issues) to avoid duplicate work.
- This project is licensed under the [Functional Source License](LICENSE). By contributing, you agree that your contributions will be licensed under the same terms.

## Ways to contribute

- **Bug reports** — reproducible steps, logs, and environment details help a lot.
- **Feature requests** — describe the problem and your proposed solution.
- **Code** — bug fixes, improvements, and new database support.
- **Documentation** — README updates, troubleshooting notes, and usage examples.

## Development setup

```sh
git clone --recursive git@github.com:sonnam0904/sqleditor.git
cd sqleditor
./scripts/setup
./scripts/build
```

See the [README](README.md) for vcpkg setup, platform-specific builds, and release builds.

## Making changes

1. Fork the repository and create a branch from `main`.
2. Make focused changes — one logical change per pull request when possible.
3. Format code before committing:

   ```sh
   ./scripts/format
   ```

4. Run tests when your change touches database drivers or connection logic:

   ```sh
   ./scripts/build
   ./scripts/run-tests
   ```

   Integration tests require Docker.

5. Verify the app still builds and runs on your platform:

   ```sh
   ./build/SQLEditor          # Linux debug
   ./build_release/SQLEditor  # Linux release
   ```

## Pull request guidelines

- Use a clear title and description.
- Link related issues (e.g. `Fixes #123`).
- Include screenshots or screen recordings for UI changes.
- Note which platforms and databases you tested against.
- Keep PRs reasonably sized — split large features into smaller reviewable chunks.

## Code style

- Follow the existing style in the file you are editing.
- C++20 is required; prefer modern C++ idioms consistent with the codebase.
- Run `./scripts/format` — do not hand-format to a different style.
- Avoid unrelated refactors in the same PR as a bug fix or feature.

## Reporting bugs

When filing a bug report, please include:

- SQLEditor version or commit hash
- Operating system and version (e.g. Ubuntu 24.04, macOS 15, Windows 11)
- GPU / graphics stack if relevant (especially on Linux with NVIDIA)
- Database type and version
- Steps to reproduce
- Expected vs actual behavior
- Logs from running in a terminal (`2>&1 | tee sqleditor.log`)

## Security issues

Do **not** open a public issue for security vulnerabilities. See [SECURITY.md](.github/SECURITY.md) for how to report them responsibly.

## Questions

Open a [GitHub Discussion](https://github.com/sonnam0904/sqleditor/discussions) or an issue if you are unsure whether a change would be accepted before investing significant effort.

Thank you for helping improve SQLEditor!
