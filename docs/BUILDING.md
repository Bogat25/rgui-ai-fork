# Build, test, package, and release

## Prerequisites

Use Windows x64 with Rtools45 and Python 3. The build wrapper locates Rtools or
accepts `-Rtools`. Choose an absolute writable BuildRoot outside the checkout,
without spaces. The source checkout itself can contain spaces: the wrapper
mirrors it before invoking R's makefiles. Allow substantial space for the source
mirror, compiled R, packages, runtime, models, and distribution staging.

```powershell
.\rgui.cmd doctor -BuildRoot C:\rgui-build
.\rgui.cmd fetch -NoModel -BuildRoot C:\rgui-build
.\rgui.cmd full -BuildRoot C:\rgui-build
```

`fetch` obtains verified Tcl/Tk, llama.cpp, and the per-user Inno Setup compiler.
Without `-NoModel`, it also downloads the base model and projector. The pins and
download behavior are defined in [rgui.ps1](../rgui.ps1) and
[Rai.conf](../src/gnuwin32/fixed/etc/Rai.conf).

## Development and checks

| Command | Purpose |
| --- | --- |
| `rgui.cmd dev` | Incremental native rebuild, then launch with a development home |
| `rgui.cmd quick` | Rebuild core libraries and front ends |
| `rgui.cmd full` | Build R plus base and recommended packages; use after broader changes |
| `rgui.cmd run` | Start built RGui with the assistant payload |
| `rgui.cmd test` | Compile checks, native unit/protocol/control regressions |
| `rgui.cmd test -Real` | Include actual model startup, generation, and picture inference |
| `rgui.cmd test -Gui` | Exercise built RGui menus, controls, attachments, and lifecycle |
| `rgui.cmd test -Installer` | Install, launch, upgrade, and uninstall the built installer |

Pass the same `-BuildRoot` and optional `-Rtools` to each command. Real and GUI
tests require their model/runtime/build prerequisites. GUI tests manipulate real
windows and the clipboard; use a disposable desktop with synthetic input and
leave the test windows alone. Installer tests change local registration: review
the harness and use a disposable profile without an existing RGui AI installation.
Unlike the other two forks, this installer harness uses the production AppId.

The [test harness guide](../src/gnuwin32/aitests/README.md) describes fake-server
fixtures, captured streaming output, and the standalone native executable.
Logs and generated fixtures belong under BuildRoot, not Git.

## Packages

```powershell
.\rgui.cmd package -BuildRoot C:\rgui-build
.\rgui.cmd installer -Version 0.1.0 -BuildRoot C:\rgui-build
```

`package` assembles the portable `dist` layout. `-NoModel` omits downloaded
weights. `installer` creates `installer/RGui-AI-<version>-setup.exe` and its
`.sha256`; it always excludes models. Build current sources before packaging:
the RGui installer command stages an existing build rather than compiling it.
`deploy -Drive E:` copies the distribution to removable storage; review its
destination and preservation behavior in the detailed setup guide before use.

Installers request no elevation. Upgrades preserve the shipped editable
configuration and prompt; work data remains separate from application payload.
The `.iss` file is the source of truth for cleanup rules.

## Version-tag workflow

Fork package versions are independent of the bundled R version. For example,
an installer version `0.1.0` does not mean R itself is version 0.1.0.

[release.yml](../.github/workflows/release.yml) handles version-tag pushes and
manual dispatch. A tag such as `v0.1.0` builds the tagged commit, fetches pinned
tools, builds R, runs default checks, packages the installer, and checks its
installation lifecycle. A separate job publishes the EXE and checksum as a
GitHub release. A suffix such as `-rc1` marks a prerelease. Manual dispatch builds
Actions artifacts without publishing.

Before the owner publishes a tag:

1. Commit the intended source and workflow changes.
2. Run default, real-model, GUI, and installer checks appropriate to the change.
3. Review licenses, release notes, checksums, and the repository privacy report.
4. Create and push a unique version tag on that reviewed commit.

Creating a local tag alone does not trigger GitHub. Retrying an old run uses the
old tagged source. Use a new tag for a corrected source revision; do not silently
move a published release tag. GUI/real-model checks require a local desktop and
are not included in the hosted default suite.

The optional [CI dry-run tool](../packaging/ci-dryrun.py) rehearses the build job
from a clean copy of the committed source. Publishing, signing, and hosted
success are separate from a successful local build.
