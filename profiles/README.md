# Profiles

Example Coyote profiles plus tools to generate complete ones from a database.

## Files

- `profile_database.json` — source data: ~230 countries (name, ISO/dial codes,
  timezone, languages, carriers with MCC/MNC, and a `capital` with coordinates)
  and ~40 real device profiles (build fingerprint, SoC/GPU, memory, display,
  serial scheme, …).
- `generate_profiles.py` — builds full `test_profile`-shaped profiles (all
  sections Coyote consumes) from a country + carrier + device.
- `pixel7_us_tmobile.json` — a hand-written example profile.

## Generating profiles

```bash
# one random profile to stdout
python generate_profiles.py

# a specific country / carrier / device, to a file
python generate_profiles.py --country US --carrier 260 --device google-pixel-8-pro -o pixel_tmobile.json

# reproducible output
python generate_profiles.py --country AD --seed 42 -o andorra.json

# the whole matrix: one profile per carrier for every country
#   -> collection/<ISO>/<mcc><mnc>_<carrier>.json   (~1300 files)
python generate_profiles.py --full-collection
python generate_profiles.py --full-collection my_dir   # custom output dir
```

`--country`, `--carrier`, and `--device` are each optional and random when
omitted (`--carrier` matches an MNC or a name substring; `--device` an id or a
marketing-name substring).

Each profile is populated the way `coyote.py` does: synthesized Wi-Fi/account
identity, Play-Store attribution, a generated serial, and a **location
randomized within ~10 km of the country's capital**. Generated profiles pass
`coyote.py`'s validator and can be used directly:

```bash
python ../coyote.py -p andorra.json -t com.example.app
```

Generated `collection/` output is git-ignored.
