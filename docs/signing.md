# Code signing with SignPath Foundation

Release builds are signed for free through [SignPath Foundation](https://signpath.org), which signs open-source projects. Signed builds stop SmartScreen warnings over time and are not blocked by Smart App Control.

The workflow in [`.github/workflows/build.yml`](../.github/workflows/build.yml) already contains the signing steps. They stay off until the repository variable and secret below exist. Without them, every build, including tag releases, is published unsigned.

## How a signed release works

1. You push a tag such as `v0.2.0`.
2. GitHub Actions builds `bloatfclips.exe` and sends it to SignPath (artifact configuration `exe`).
3. You approve the request in SignPath. Every release needs this manual approval.
4. The signed exe is packed into the installer, which is sent to SignPath too (artifact configuration `installer`). You approve that request as well.
5. The signed installer is attached to the GitHub Release.

## One-time setup

### 1. Before you apply

- Turn on two-factor authentication for your GitHub account. SignPath requires MFA for every team member.
- Check that the README's [Code signing policy](../README.md#code-signing-policy) and [Privacy](../README.md#privacy) sections are on `master`. SignPath checks them.

### 2. Apply

Apply at <https://signpath.org/apply> with:

- Repository: `https://github.com/sacredcodr/bloatfclips`
- License: MIT
- Download page: `https://github.com/sacredcodr/bloatfclips/releases`

SignPath reviews each project by hand and can decline.

### 3. After approval, in the SignPath web app

- **Trusted build system:** add the predefined **GitHub.com** build system and link it to the project.
- **Project slug:** `bloatfclips`. If you use a different slug, set the repository variable `SIGNPATH_PROJECT_SLUG`.
- **Signing policy slug:** `release-signing`, with manual approval.
- **Artifact configurations:** create two, with the slugs below. GitHub artifacts arrive as ZIP files, so each one starts with `<zip-file>`.

  `exe`:

  ```xml
  <?xml version="1.0" encoding="utf-8"?>
  <artifact-configuration xmlns="http://signpath.io/artifact-configuration/v1">
    <zip-file>
      <pe-file path="bloatfclips.exe" product-name="bloatfclips">
        <authenticode-sign />
      </pe-file>
    </zip-file>
  </artifact-configuration>
  ```

  `installer`:

  ```xml
  <?xml version="1.0" encoding="utf-8"?>
  <artifact-configuration xmlns="http://signpath.io/artifact-configuration/v1">
    <zip-file>
      <pe-file path="bloatfclips-setup-*.exe" product-name="bloatfclips">
        <authenticode-sign />
      </pe-file>
    </zip-file>
  </artifact-configuration>
  ```

  SignPath requires the product name and version to be checked. Both are set in [`src/app.rc`](../src/app.rc) and the Inno Setup script. Compare the XML against SignPath's artifact configuration docs when you create it.
- **API token:** create one for a user with *submitter* permission on the project.

### 4. In GitHub (repo **Settings → Secrets and variables → Actions**)

| Kind | Name | Value |
| --- | --- | --- |
| Variable | `SIGNPATH_ORGANIZATION_ID` | Your SignPath organization ID |
| Variable | `SIGNPATH_PROJECT_SLUG` | Only if it isn't `bloatfclips` |
| Secret | `SIGNPATH_API_TOKEN` | The API token from SignPath |

The next tag you push builds a signed release.

## Known limitation

Inno Setup creates the uninstaller (`unins000.exe`) at compile time, and it is not covered by signing the installer afterwards. Signing it too needs Inno Setup's `SignTool` hook during compilation, which the SignPath GitHub integration doesn't support directly. Until that is solved, Smart App Control may block the uninstaller.
