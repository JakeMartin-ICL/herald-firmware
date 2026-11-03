# Herald Firmware

Firmware for Herald LED game boxes (ESP32). Boxes self-elect a hub via mDNS and communicate with the Herald web app over WebSockets.

---

## Checks before release

Pull requests, branch pushes, and manual runs of **Check Firmware** run the LED
sector tests and compile `esp32dev-release` without publishing a release. The tag
release workflow runs the same tests before building and publishing its binary.

The sector tests also run locally with Python 3 and a desktop C++ compiler; no
PlatformIO installation or connected box is needed:

```bash
python3 scripts/test_led_sectors.py
```

These tests exercise the production sector-expansion helper. A complete firmware
build additionally checks its ArduinoJson/FastLED integration. Neither replaces
testing the firmware on physical boxes.

## Repeating LED sectors

The next patch release, **3.5.1**, adds optional `repeat: true` to `led_sectors`:

```json
{
  "type": "led_sectors",
  "hwid": "AA:BB:CC:DD:EE:FF",
  "repeat": true,
  "sectors": [
    { "color": "#ffffff", "count": 1 },
    { "color": "#4444ff", "count": 3 }
  ]
}
```

This repeats one white LED and three blue LEDs around the ring. Any sequence of
colours and positive counts can repeat; the final sector is clipped at the end
of the ring. Nonpositive counts are skipped. Empty patterns, or patterns with no
positive counts, leave the ring off. Omitting `repeat` keeps the existing single
pass and leaves any unused LEDs off.

The example is 135 bytes when compactly serialized, below the 250-byte ESP-NOW
limit. Explicitly listing all six copies would be 421 bytes.

Older firmware ignores `repeat`, so the web app uses compact alternating pairs
for speaker patterns on versions below 3.5.1 or when the version is unknown.
Virtual boxes preview the full repeating pattern. The version in
`platformio.ini` is bumped by the release process; this change does not create
or publish a release.

---

## Releasing a new version

### Using the release script

`release.ps1` (Windows) and `release.sh` (Linux/macOS) automate all steps below. They read the current version from `platformio.ini`, bump it, and handle the commit, push, tag, and tag push.

```powershell
# Windows
.\release.ps1           # patch bump: 0.1.3 → 0.1.4
.\release.ps1 -Minor    # minor bump: 0.1.3 → 0.2.0
.\release.ps1 -Major    # major bump: 0.1.3 → 1.0.0
.\release.ps1 -Retag    # re-point existing tag to HEAD and re-trigger pipeline
```

```bash
# Linux/macOS
./release.sh            # patch bump: 0.1.3 → 0.1.4
./release.sh --minor    # minor bump: 0.1.3 → 0.2.0
./release.sh --major    # major bump: 0.1.3 → 1.0.0
./release.sh --retag    # re-point existing tag to HEAD and re-trigger pipeline
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
