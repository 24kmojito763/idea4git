# GitHub Releases

`.github/workflows/release.yml` builds on Linux, Windows, and macOS whenever a `v*` tag is pushed. All three platform jobs build and run the test suite. After they pass, the workflow creates or updates a GitHub Release and uploads the artifacts using the repository's built-in `GITHUB_TOKEN`; no personal access token is needed.

## Publish a version

1. Push the project to a GitHub repository.
2. Keep the checked-in `LICENSE` file with the release. This project is licensed under GPL-3.0-only, and the workflow refuses to attach binaries if it cannot find `LICENSE`, `LICENSE.txt`, or `COPYING`.
3. From a clean, tested commit, create and push a version tag:

   ```bash
   git tag v1.0.1
   git push origin v1.0.1
   ```

4. Watch the Actions run. Once every platform build and test passes, the workflow publishes the release at [idea4git Releases](https://github.com/24kmojito763/idea4git/releases); the latest version is also available at `/releases/latest`.

The tag must use `vMAJOR.MINOR.PATCH` so the application and package version can be derived from it. Re-pushing a tag is not a normal update mechanism: prefer a new version tag for each release.

## Release assets

- `idea4git-vX.Y.Z-linux-amd64.deb` — Ubuntu/Debian x86_64 package. It relies on the distribution package manager for Qt/QScintilla runtime dependencies; it is not a universal Linux AppImage.
- `idea4git-vX.Y.Z-windows-x86_64.zip` — Windows x64 application with Qt runtime files deployed. Install Git for Windows separately and make `git` available to the application.
- `idea4git-vX.Y.Z-macos-<architecture>.zip` — macOS app bundle; the architecture suffix is read from the built executable. Install Git separately. The archive is not Developer ID signed or notarized, so Gatekeeper may require the user to explicitly allow opening it.
- `QScintilla_src-2.14.1.tar.gz` — the exact QScintilla source archive used by the Windows/macOS builds, checksum-verified against the upstream download.

Windows/macOS use Qt 6.8.3 and build QScintilla 2.14.1 as a static library against that Qt toolchain. Linux uses Ubuntu 24.04's Qt 6 and QScintilla development packages. These choices are build inputs, not a claim of completed manual QA on every supported operating-system release or CPU architecture. The first successful Actions run is required before treating Windows/macOS CI builds as verified.

The project is licensed under GPL-3.0-only. The workflow checks that a license file is present and attaches the QScintilla source archive used by the Windows/macOS builds; maintainers should keep required license notices and corresponding source materials with each distribution. The workflow cannot assess legal completeness. Windows and macOS artifacts are unsigned; signing/notarization can be added later using maintainer-managed certificates and repository secrets.
