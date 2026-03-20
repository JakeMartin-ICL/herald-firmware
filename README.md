# Herald Firmware

Firmware for Herald LED game boxes (ESP32). Boxes self-elect a hub via mDNS and communicate with the Herald web app over WebSockets.

---

## Releasing a new version

### Using the release script

`release.sh` automates all steps below. It reads the current version from `platformio.ini`, bumps it, and handles the commit, push, tag, and tag push.

```bash
./release.sh           # patch bump: 0.1.3 → 0.1.4
./release.sh --minor   # minor bump: 0.1.3 → 0.2.0
./release.sh --major   # major bump: 0.1.3 → 1.0.0
```

You will be asked to confirm before anything is committed.

### Manual steps

#### 1. Update the version number

Edit `platformio.ini` and bump the version in the `esp32dev-release` env:

```ini
build_flags =
    -DFIRMWARE_VERSION='"1.2.0"'
```

#### 2. Commit and push

```bash
git add platformio.ini
git commit -m "Release v1.2.0"
git push
```

#### 3. Tag the commit

```bash
git tag v1.2.0
git push origin v1.2.0
```

Pushing the tag triggers the GitHub Actions workflow (`.github/workflows/release.yml`), which:

1. Builds the firmware using PlatformIO (`esp32dev-release` env)
2. Copies the output `.bin` to `releases/herald-firmware.bin`
3. Creates a GitHub release named `Herald Firmware v1.2.0` with the `.bin` attached

Release notes are generated automatically from commit messages since the previous tag.

#### 4. Verify the release

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
