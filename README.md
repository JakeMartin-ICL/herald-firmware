# Herald Firmware

Firmware for Herald LED game boxes (ESP32). Boxes self-elect a hub via mDNS and communicate with the Herald web app over WebSockets.

---

## Releasing a new version

### 1. Update the version number

Edit `platformio.ini` and bump the version in the `esp32dev-release` env:

```ini
build_flags =
    -DFIRMWARE_VERSION='"1.2.0"'
```

### 2. Update the changelog

Add an entry to `CHANGELOG.md` describing what changed. The GitHub release body is populated from this file.

### 3. Commit and push

```bash
git add platformio.ini CHANGELOG.md
git commit -m "Release v1.2.0"
git push
```

### 4. Tag the commit

```bash
git tag v1.2.0
git push origin v1.2.0
```

Pushing the tag triggers the GitHub Actions workflow (`.github/workflows/release.yml`), which:

1. Builds the firmware using PlatformIO (`esp32dev-release` env)
2. Copies the output `.bin` to `releases/herald-firmware.bin`
3. Creates a GitHub release named `Herald Firmware v1.2.0` with the `.bin` attached

### 5. Verify the release

Go to the repository's **Releases** page and confirm the release was created with `herald-firmware.bin` attached. The Herald web app fetches this via the GitHub API and will show an update prompt on any connected box running an older version.

---

## Local build

To build the release binary locally without publishing:

```bash
pio run -e esp32dev-release
```

The binary will be at `.pio/build/esp32dev-release/firmware.bin` and copied to `releases/herald-firmware.bin`.

## Development builds

| Environment | Description |
|---|---|
| `esp32dev` | Auto-elects hub or client at boot |
| `esp32dev-hub` | Force hub role (`FORCE_HUB`) |
| `esp32dev-client` | Force client role (`FORCE_CLIENT`) |
| `esp32dev-release` | Production build with versioned firmware |

```bash
pio run -e esp32dev-hub -t upload    # flash as hub
pio run -e esp32dev-client -t upload # flash as client
```
