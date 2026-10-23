# Profiles

Shipped Coyote data: the source database the `coyote-profiles` command draws
from, plus a hand-written example profile.

## Files

- `profile_database.json` — source data: ~230 countries (name, ISO/dial codes,
  timezone, languages, carriers with MCC/MNC, and a `capital` with coordinates)
  and ~40 real device profiles (build fingerprint, SoC/GPU, memory, display,
  serial scheme, …).
- `pixel7_us_tmobile.json` — a hand-written example profile.

The profile generator itself lives one level up in the package
(`src/coyote/coyote_profiles.py`) and is exposed as the `coyote-profiles`
command once Coyote is installed.

## Generating profiles

```bash
# one random profile to stdout
coyote-profiles

# a specific country / carrier / device, to a file
coyote-profiles --country US --carrier 260 --device google-pixel-8-pro -o pixel_tmobile.json

# reproducible output
coyote-profiles --country AD --seed 42 -o andorra.json

# the whole matrix: one profile per carrier for every country
#   -> collection/<ISO>/<mcc><mnc>_<carrier>.json   (~1300 files)
coyote-profiles --full-collection
coyote-profiles --full-collection my_dir   # custom output dir
```

`--country`, `--carrier`, and `--device` are each optional and random when
omitted (`--carrier` matches an MNC or a name substring; `--device` an id or a
marketing-name substring).

Each profile is populated the way `coyote` does: synthesized Wi-Fi/account
identity, Play-Store attribution, a generated serial, and a **location
randomized within ~10 km of the country's capital**. Generated profiles pass
`coyote`'s validator and can be used directly:

```bash
coyote -p andorra.json -t com.example.app
```

Generated `collection/` output is git-ignored.
