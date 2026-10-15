#!/usr/bin/env python3
"""Generate complete Coyote device profiles from profile_database.json.

Each profile has the full schema Coyote consumes (telecom, bypass, device_profile,
specs, network, attribution, identity, location, system_prop) — the same shape as
test/src/main/assets/test_profile.json — built from a country + carrier + a real
device profile. Location is randomized within ~10 km of the country's capital, and
identity/attribution/serial/etc. are synthesized the way coyote.py does.

Examples:
  # one random profile
  python generate_profiles.py -o out.json

  # a specific country/carrier/device
  python generate_profiles.py --country US --carrier 260 --device google-pixel-8-pro -o pixel_tmobile.json

  # the whole matrix: collection/<ISO>/<mcc><mnc>_<carrier>.json for every carrier
  python generate_profiles.py --full-collection collection
"""
import argparse
import json
import math
import random
import re
import sys
from datetime import datetime, timedelta, timezone
from pathlib import Path

DB_PATH = Path(__file__).resolve().parent / "profile_database.json"

SIM_STATES = {"READY": 5}
NETWORK_TYPES = ["LTE", "5G"]


# --------------------------------------------------------------------------- #
# helpers
# --------------------------------------------------------------------------- #
def slugify(s: str) -> str:
    return re.sub(r"[^a-z0-9]+", "-", (s or "").lower()).strip("-") or "x"


def rand_from(alphabet: str, n: int) -> str:
    return "".join(random.choice(alphabet) for _ in range(n))


def rand_mac() -> str:
    first = random.choice([0x02, 0x06, 0x0A, 0x0E])  # locally administered, unicast
    return ":".join(f"{b:02x}" for b in [first] + [random.randint(0, 255) for _ in range(5)])


def gen_serial(scheme: dict) -> str:
    if not scheme:
        return rand_from("ABCDEFGHJKLMNPQRSTUVWXYZ0123456789", 11)
    prefix = scheme.get("prefix", "")
    length = scheme.get("length", 11)
    alphabet = scheme.get("alphabet", "ABCDEFGHJKLMNPQRSTUVWXYZ0123456789")
    return prefix + rand_from(alphabet, max(0, length - len(prefix)))


def jitter_location(lat: float, lon: float, radius_m: float = 10000.0) -> tuple[float, float]:
    """A uniformly random point within `radius_m` of (lat, lon)."""
    r = radius_m * math.sqrt(random.random())
    theta = random.uniform(0, 2 * math.pi)
    dnorth = r * math.cos(theta)
    deast = r * math.sin(theta)
    dlat = dnorth / 111320.0
    dlon = deast / (111320.0 * math.cos(math.radians(lat)) or 1e-9)
    return round(lat + dlat, 6), round(lon + dlon, 6)


def gen_build_date(security_patch: str) -> tuple[str, str]:
    """A plausible build date near the security patch, as (formatted, epoch)."""
    try:
        base = datetime.strptime(security_patch, "%Y-%m-%d").replace(tzinfo=timezone.utc)
    except Exception:
        base = datetime.now(timezone.utc) - timedelta(days=random.randint(60, 400))
    dt = base + timedelta(days=random.randint(0, 25), hours=random.randint(0, 23),
                          minutes=random.randint(0, 59), seconds=random.randint(0, 59))
    return dt.strftime("%a %b %d %H:%M:%S UTC %Y"), str(int(dt.timestamp()))


def gen_cpuinfo(soc: dict, arch: str) -> str:
    cores = (soc or {}).get("cpu_cores", 8)
    hw = (soc or {}).get("model", "Generic")
    feats = ("fp asimd evtstrm aes pmull sha1 sha2 crc32 atomics fphp asimdhp cpuid "
             "asimdrdm jscvt fcma lrcpc dcpop sha3 asimddp sha512")
    blocks = []
    for i in range(cores):
        blocks.append(
            f"processor\t: {i}\n"
            f"BogoMIPS\t: 38.40\n"
            f"Features\t: {feats}\n"
            f"CPU implementer\t: 0x41\n"
            f"CPU architecture: 8\n"
            f"CPU variant\t: 0x1\n"
            f"CPU part\t: 0xd0b\n"
            f"CPU revision\t: 0\n"
        )
    header = "Processor\t: AArch64 Processor rev 0 (aarch64)\n\n"
    return header + "\n".join(blocks) + f"\nHardware\t: {hw}\n"


# --------------------------------------------------------------------------- #
# section builders
# --------------------------------------------------------------------------- #
def build_telecom(country: dict, carrier: dict) -> dict:
    iso = (country.get("isoCodes") or ["zz"])[0].lower()
    langs = country.get("languages") or []
    lang = langs[0].split("_")[0] if langs else "en"
    dial = (country.get("dialCodes") or [""])[0]
    return {
        "mcc": carrier["mcc"],
        "mnc": carrier["mnc"],
        "mcc_country_name": country.get("name", ""),
        "imei_prefix": "35",
        "network_type": random.choice(NETWORK_TYPES),
        "timezone_id": country.get("timezone", "UTC"),
        "country_iso": iso,
        "operator_name": carrier.get("network", ""),
        "language": lang,
        "phone_code": dial,
        "sim_state": "READY",
        "sim_state_num": 5,
    }


def build_device_profile(dev: dict, serial: str, build_date: str, build_date_utc: str) -> dict:
    gpu = dev.get("gpu", {})
    soc = dev.get("soc", {})
    build_id = dev.get("build_id", "")
    incr = dev.get("incremental", "")
    display = f"{build_id}.{incr}" if build_id and incr else build_id
    return {
        "profile_id": dev.get("id", ""),
        "marketing_name": dev.get("marketing_name", ""),
        "model": dev.get("model", ""),
        "device": dev.get("device", ""),
        "board": dev.get("board", ""),
        "product": dev.get("product", ""),
        "hardware": dev.get("hardware", ""),
        "fingerprint": dev.get("fingerprint", ""),
        "manufacturer": dev.get("manufacturer", ""),
        "bootloader": incr,
        "brand": dev.get("brand", ""),
        "host": dev.get("build_host", ""),
        "display": display,
        "display_id": build_id,
        "tags": "release-keys",
        "serial": serial,
        "type": "user",
        "user": dev.get("build_user", ""),
        "unknown": "unknown",
        "arch": dev.get("arch", "arm64-v8a"),
        "baseband": incr,
        "characteristics": "phone",
        "flavor": f"{dev.get('product','')}-user",
        "incremental": incr,
        "build_date": build_date,
        "build_date_utc": build_date_utc,
        "security_patch": dev.get("security_patch", ""),
        "soc_model": soc.get("model", ""),
        "slpi_firmware": "",
        "gl_vendor": gpu.get("vendor", ""),
        "gl_renderer": gpu.get("renderer", ""),
        "gl_version": gpu.get("version", ""),
        "keyboard": dev.get("keyboard", ""),
        "cpu_info": gen_cpuinfo(soc, dev.get("arch", "")),
    }


def build_specs(dev: dict) -> dict:
    disp = dev.get("display", {})
    mem = dev.get("memory", {})
    ram_gb = random.choice(mem.get("ram_gb", [8]) or [8])
    storage_gb = random.choice(mem.get("storage_gb", [128]) or [128])
    swap_gb = mem.get("swap_gb", 0)

    total_ram = int(ram_gb * (1024 ** 3) * random.uniform(0.90, 0.94))  # usable < nominal
    avail_ram = int(total_ram * random.uniform(0.30, 0.50))
    total_space = int(storage_gb * (1024 ** 3) * random.uniform(0.90, 0.95))
    free_space = int(total_space * random.uniform(0.35, 0.75))
    dpi = disp.get("density_dpi", 420)
    return {
        "TOTAL_SPACE": total_space,
        "FREE_SPACE": free_space,
        "WIDTH": disp.get("width", 1080),
        "HEIGHT": disp.get("height", 2400),
        "DENSITY": f"{dpi / 160.0}f",
        "DENSITY_DPI": dpi,
        "XDPI": f"{float(disp.get('xdpi', dpi))}f",
        "YDPI": f"{float(disp.get('ydpi', dpi))}f",
        "TOTAL_RAM": total_ram,
        "AVAIL_RAM": avail_ram,
        "RAM_THRESHOLD": int(total_ram * 0.05),
        "SWAP_TOTAL_KB": int(swap_gb * 1024 * 1024),
        "BATTERY_LEVEL": random.randint(35, 95),
    }


def build_network() -> dict:
    wifi = random.random() < 0.5
    net = {
        "net_type": 1 if wifi else 0,
        "mobile_data_local_ip": f"10.{random.randint(0,255)}.{random.randint(0,255)}.{random.randint(2,254)}",
        "wifi_local_ip": f"192.168.{random.randint(0,5)}.{random.randint(2,254)}",
    }
    return net


def build_identity() -> dict:
    ssid = random.choice(["HomeWiFi", "NETGEAR", "TP-Link_", "Linksys", "ATT", "Xfinity"]) + str(random.randint(10, 99))
    first = random.choice(["alex", "sam", "jordan", "taylor", "casey", "jane", "john", "maria", "liam"])
    last = random.choice(["lee", "khan", "silva", "jones", "garcia", "smith", "mueller", "rossi"])
    return {
        "wifi_ssid": ssid,
        "wifi_bssid": rand_mac(),
        "wifi_mac": rand_mac(),
        "account_email": f"{first}.{last}{random.randint(1,99)}@gmail.com",
        "account_type": "com.google",
    }


def build_attribution() -> dict:
    return {
        "facebook_app_id": "",
        "af_status": random.choice(["Organic", "non-Organic"]),
        "campaign": "",
        "install_referrer": "utm_source=google-play&utm_medium=organic",
        "install_package": "com.android.vending",
    }


def build_location(country: dict) -> dict:
    cap = country.get("capital")
    if not cap:
        return {}
    lat, lon = jitter_location(cap["latitude"], cap["longitude"], 10000.0)
    return {
        "latitude": lat,
        "longitude": lon,
        "accuracy": round(random.uniform(5.0, 25.0), 1),
        "altitude": round(random.uniform(5.0, 400.0), 1),
    }


def build_system_prop(dev: dict, tel: dict, serial: str, build_date: str, build_date_utc: str) -> dict:
    soc = dev.get("soc", {})
    density_dpi = dev.get("display", {}).get("density_dpi", 420)
    model = dev.get("model", "")
    brand = dev.get("brand", "")
    manuf = dev.get("manufacturer", "")
    device = dev.get("device", "")
    name = dev.get("product", "")
    board = dev.get("board", "")
    fp = dev.get("fingerprint", "")
    build_id = dev.get("build_id", "")
    incr = dev.get("incremental", "")
    arch = dev.get("arch", "arm64-v8a")
    numeric = tel["mcc"] + tel["mnc"]

    props: dict[str, str] = {}

    # product identity across partitions
    partitions = ["", "bootimage", "product", "system", "system_ext", "odm", "vendor", "vendor_dlkm"]
    for part in partitions:
        p = f"{part}." if part else ""
        props[f"ro.product.{p}model"] = model
        props[f"ro.product.{p}brand"] = brand
        props[f"ro.product.{p}manufacturer"] = manuf
        props[f"ro.product.{p}device"] = device
        props[f"ro.product.{p}name"] = name
    props["ro.product.cpu.abilist"] = arch
    props["ro.product.board"] = board
    props["ro.product.locale"] = f"{tel['language']}-{tel['country_iso'].upper()}"

    # build identity
    props.update({
        "ro.build.fingerprint": fp,
        "ro.build.id": build_id,
        "ro.build.display.id": f"{build_id}.{incr}" if build_id and incr else build_id,
        "ro.build.version.incremental": incr,
        "ro.build.version.security_patch": dev.get("security_patch", ""),
        "ro.build.type": "user",
        "ro.build.tags": "release-keys",
        "ro.build.flavor": f"{name}-user",
        "ro.build.characteristics": "phone",
        "ro.build.host": dev.get("build_host", ""),
        "ro.build.user": dev.get("build_user", ""),
        "ro.build.product": device,
        "ro.build.description": f"{name}-user {build_id} {incr} release-keys",
        "ro.build.date": build_date,
        "ro.build.date.utc": build_date_utc,
    })
    for part in ["", "vendor", "odm", "system", "system_ext", "product", "vendor_dlkm"]:
        p = f"{part}." if part else ""
        props[f"ro.{p}build.fingerprint"] = fp
        props[f"ro.{p}build.version.incremental"] = incr
        props[f"ro.{p}build.id"] = build_id
        props[f"ro.{p}build.type"] = "user"

    # hardware / soc
    props.update({
        "ro.hardware": dev.get("hardware", ""),
        "ro.board.platform": board,
        "ro.boot.hardware": dev.get("hardware", ""),
        "ro.boot.hardware.platform": soc.get("model", ""),
        "ro.soc.model": soc.get("model", ""),
        "ro.soc.manufacturer": soc.get("manufacturer", ""),
        "ro.serialno": serial,
        "ro.boot.serialno": serial,
        "ro.bootloader": incr,
        "ro.boot.bootloader": incr,
    })

    # anti-emulator tells (clean values)
    props.update({
        "ro.kernel.qemu": "",
        "ro.boot.qemu": "",
        "ro.boot.qemu.gltransport": "",
        "ro.boot.qemu.gltransport.name": "",
        "qemu.sf.lcd_density": "",
        "qemu.hw.mainkeys": "",
        "ro.hardware.egl": soc.get("manufacturer", "").lower() or "",
    })

    # security / verified boot
    props.update({
        "ro.secure": "1",
        "ro.debuggable": "0",
        "ro.force.debuggable": "0",
        "ro.adb.secure": "1",
        "ro.build.selinux": "1",
        "ro.boot.verifiedbootstate": "green",
        "ro.boot.flash.locked": "1",
        "ro.boot.vbmeta.device_state": "locked",
        "ro.warranty_bit": "0",
        "ro.boot.warranty_bit": "0",
        "ro.crypto.state": "encrypted",
        "sys.oem_unlock_allowed": "0",
    })

    # telephony / locale
    props.update({
        "gsm.operator.numeric": numeric,
        "gsm.operator.alpha": tel["operator_name"],
        "gsm.operator.iso-country": tel["country_iso"],
        "gsm.sim.operator.numeric": numeric,
        "gsm.sim.state": "READY",
        "gsm.version.baseband": incr,
        "persist.sys.timezone": tel["timezone_id"],
        "persist.sys.locale": f"{tel['language']}-{tel['country_iso'].upper()}",
        "persist.sys.language": tel["language"],
    })

    # USB / ADB / debug service state (clean, non-rooted, USB not connected)
    props.update({
        "init.svc.adbd": "stopped",
        "init.svc.adb_root": "",
        "init.svc_debug_pid.adb_root": "",
        "init.svc_debug_pid.adbd": "",
        "ro.boottime.adb_root": "",
        "ro.boottime.adbd": "",
        "service.adb.root": "0",
        "service.adb.tcp.port": "",
        "persist.adb.tls_server.enable": "",
        "persist.adb.wifi.guid": "",
        "persist.vendor.radio.adb_log_on": "",
        "persist.sys.disable_rescue": "",
        "persist.sys.usb.config": "",
        "sys.usb.config": "",
        "sys.usb.state": "",
        "sys.usb.ffs.ready": "0",
        "http.proxyHost": "",
    })

    # Custom-ROM tells left empty so the device reads as stock.
    props.update({
        "ro.lineage.version": "",
        "ro.lineage.device": "",
        "ro.lineage.releasetype": "",
        "ro.lineage.build.version": "",
        "ro.lineage.build.version.plat.sdk": "",
        "ro.lineage.build.version.plat.rev": "",
        "ro.lineagelegal.url": "",
        "ro.modversion": "",
    })

    # Misc
    props.update({
        "ro.sf.lcd_density": str(density_dpi),
        "ro.boot.hardware.sku": "",
        "ro.boot.selinux": "",
        "ro.boot.qemu.gltransport.drawFlushInterval": "",
        "vendor.sys.slpi.firmware.version": "",
    })

    return props


# --------------------------------------------------------------------------- #
# profile assembly
# --------------------------------------------------------------------------- #
def build_profile(country: dict, carrier: dict, dev: dict) -> dict:
    serial = gen_serial(dev.get("serial", {}))
    build_date, build_date_utc = gen_build_date(dev.get("security_patch", ""))
    tel = build_telecom(country, carrier)
    profile = {
        "telecom": tel,
        "bypass": {"flag_secure": 1},
        "device_profile": build_device_profile(dev, serial, build_date, build_date_utc),
        "specs": build_specs(dev),
        "network": build_network(),
        "attribution": build_attribution(),
        "identity": build_identity(),
        "location": build_location(country),
        "system_prop": build_system_prop(dev, tel, serial, build_date, build_date_utc),
    }
    return {k: v for k, v in profile.items() if v}


# --------------------------------------------------------------------------- #
# DB access
# --------------------------------------------------------------------------- #
def load_db() -> dict:
    if not DB_PATH.is_file():
        print(f"[!] profile_database.json not found at {DB_PATH}")
        sys.exit(1)
    return json.loads(DB_PATH.read_text())


def find_country(db: dict, iso: str) -> dict:
    iso = iso.upper()
    for c in db["countries"]:
        if (c.get("isoCodes") or [None])[0] == iso:
            return c
    print(f"[!] No country with ISO '{iso}'.")
    sys.exit(1)


def pick_carrier(country: dict, selector: str | None) -> dict:
    carriers = country.get("carriers") or []
    if not carriers:
        print(f"[!] {country.get('name')} has no carriers.")
        sys.exit(1)
    if selector is None:
        return random.choice(carriers)
    sel = selector.lower()
    for c in carriers:
        if c["mnc"] == selector or sel in c.get("network", "").lower():
            return c
    print(f"[!] No carrier matching '{selector}' in {country.get('name')}.")
    sys.exit(1)


def pick_device(db: dict, selector: str | None) -> dict:
    devices = db.get("deviceProfiles") or []
    if not devices:
        print("[!] profile_database.json has no deviceProfiles.")
        sys.exit(1)
    if selector is None:
        return random.choice(devices)
    for d in devices:
        if d.get("id") == selector or selector.lower() in d.get("marketing_name", "").lower():
            return d
    print(f"[!] No device matching '{selector}'.")
    sys.exit(1)


def write_profile(profile: dict, path: Path):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(profile, indent=2, ensure_ascii=False))


# --------------------------------------------------------------------------- #
# CLI
# --------------------------------------------------------------------------- #
def main():
    ap = argparse.ArgumentParser(description="Generate complete Coyote device profiles.")
    ap.add_argument("--country", help="ISO code of the country (e.g. US). Random if omitted.")
    ap.add_argument("--carrier", help="Carrier MNC or name substring. Random if omitted.")
    ap.add_argument("--device", help="Device profile id or marketing-name substring. Random if omitted.")
    ap.add_argument("-o", "--output", help="Output file (single profile). Default: stdout.")
    ap.add_argument("--full-collection", metavar="DIR", nargs="?", const="collection",
                    help="Generate one profile per carrier for every country into "
                         "DIR/<ISO>/<mcc><mnc>_<carrier>.json (default DIR: collection).")
    ap.add_argument("--seed", type=int, help="Random seed for reproducible output.")
    args = ap.parse_args()

    if args.seed is not None:
        random.seed(args.seed)

    db = load_db()

    if args.full_collection is not None:
        out_root = Path(args.full_collection)
        total = 0
        for country in db["countries"]:
            isos = country.get("isoCodes") or []
            carriers = country.get("carriers") or []
            if not isos or not carriers:
                continue
            iso = isos[0]
            cdir = out_root / iso
            for carrier in carriers:
                dev = pick_device(db, args.device)
                profile = build_profile(country, carrier, dev)
                fname = f"{carrier['mcc']}{carrier['mnc']}_{slugify(carrier.get('network',''))}.json"
                write_profile(profile, cdir / fname)
                total += 1
        print(f"[+] Wrote {total} profiles under {out_root}/")
        return

    # single profile
    if args.country:
        country = find_country(db, args.country)
    else:
        country = random.choice([c for c in db["countries"] if c.get("carriers") and c.get("isoCodes")])
    carrier = pick_carrier(country, args.carrier)
    dev = pick_device(db, args.device)
    profile = build_profile(country, carrier, dev)

    print(f"[+] {country.get('name')} / {carrier.get('network')} ({carrier['mcc']}{carrier['mnc']}) "
          f"on {dev.get('marketing_name')}", file=sys.stderr)

    if args.output:
        write_profile(profile, Path(args.output))
        print(f"[+] Wrote {args.output}", file=sys.stderr)
    else:
        print(json.dumps(profile, indent=2, ensure_ascii=False))


if __name__ == "__main__":
    main()
