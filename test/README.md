# Border Patrol Test Apk

Border Patrol is a verification app used to test whether Coyote and Puppy successfully mocked device settings.

It loads `test_profile.json` from its assets and verifies that the emulator's hardware specs and system properties match the profile across Reflection, Shell, and `android.os.Build` inspection vectors.

## Build & Install

Run the following commands from inside this `test/` directory:

```bash
# Build debug APK
./gradlew assembleDebug

# Install on target emulator
adb install build/outputs/apk/debug/borderpatrol-debug.apk
```

## Usage

From the project root directory, run `coyote.py` to apply a profile and pass Border Patrol as the target check app:

```bash
python3 coyote.py -p test/src/main/assets/test_profile.json -t com.border.patrol
```