#!/usr/bin/env python3
"""Passes the official air alerts (api.ukrainealarm.com) to the M5Stick as a small status file.

The API allows about one request a minute, too slow to warn alone, so its signed webhook brings each change at once.
The poll every POLL_SEC stays as the truth: it repairs a lost webhook, and it shows on the stick as "bot 0" when it fails.
"""
import base64
import hashlib
import json
import logging
import os
import threading
import time
import urllib.error
import urllib.request
from datetime import datetime
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from zoneinfo import ZoneInfo

API = "https://api.ukrainealarm.com/api/v3"
API_KEY = os.environ.get("UA_API_KEY", "")
WEBHOOK_URL = os.environ.get("WEBHOOK_URL", "https://62.238.111.55.nip.io/peace-stick/webhook")
WEBHOOK_PORT = int(os.environ.get("WEBHOOK_PORT", "8787"))
PUBLIC_KEY = os.environ.get("WEBHOOK_PUBLIC_KEY", "/etc/peace-stick/webhook-public.pem")
OUT = os.environ.get("OUT", "/var/lib/peace-stick/alert.txt")
HISTORY_OUT = os.environ.get("HISTORY_OUT", "/var/lib/peace-stick/history.txt")
PRIVATE = os.environ.get("PRIVATE_DIR", "/var/lib/peace-stick/private")
HEARTBEAT_SEC = 5
HISTORY_EVERY = 12       # heartbeats between history files: its minutes grow during an alert
POLL_SEC = 65            # just over the API's limit
API_SILENT_SEC = 300     # a few failed polls: one slow answer should not ring
SETUP_TRIES = 3          # then only poll, so a failing setup call cannot use up the budget for alerts
WEBHOOK_MAX_AGE_SEC = 300
WEBHOOK_MAX_BODY = 65536
# The poll must not undo a recent webhook: its list can lag, and a wrong all-clear is the worst mistake.
# Two polls long, so a region whose webhooks stopped coming is still corrected soon.
FRESH_EVENT_SEC = 130
HISTORY_PER_ZONE = 20
KEEP_SEC = 8 * 86400     # enough for the week in the statistics
KYIV = ZoneInfo("Europe/Kyiv")

log = logging.getLogger("peace-bridge")

OUT_DIR = os.environ.get("OUT_DIR", "/var/lib/peace-stick/v2")  # one file per oblast, and the list of them
BUNDLED_REGIONS = os.path.join(os.path.dirname(os.path.abspath(__file__)), "regions.json")
COMPAT_GROUP = "14"      # alert.txt and history.txt: Kyiv, as the first firmware reads them
CITY_OBLAST = {"31": "14", "564": "12", "1293": "22"}  # cities the API lists apart from the oblast around them
SKIP_STATES = {"0"}      # the API's test region
RANK = {"alert": 1, "yellow": 2, "red": 3, "unknown": 4}
ORDER = ["BALLISTIC", "CRUISE", "MISSILES", "AIRCRAFT", "DRONES"]  # most dangerous first
# What the source writes is free text: first match wins, so a ballistic missile is not just "missile".
REASON_WORDS = [
    ("BALLISTIC", ("ballist", "балист", "баліст")),
    ("CRUISE", ("cruise", "крылат", "крилат")),
    ("AIRCRAFT", ("aircraft", "aviation", "авиац", "авіац", "міг-31", "миг-31")),
    ("MISSILES", ("missile", "rocket", "ракет")),
    ("DRONES", ("uav", "drone", "shahed", "бпла", "дрон", "шахед")),
]
OTHER_TYPES = {"ARTILLERY", "URBAN_FIGHTS", "CHEMICAL", "NUCLEAR"}  # dangerous without any level
IGNORED_TYPES = {"INFO"}


def threat_of(reason: str | None) -> str | None:
    low = (reason or "").lower()
    for threat, words in REASON_WORDS:
        if any(word in low for word in words):
            return threat
    return None


def level_of(name: str | None) -> str:
    """An unknown word means there is something we do not understand: loud, never quiet."""
    if not name:
        return "alert"
    return {"red": "red", "yellow": "yellow"}.get(name.lower(), "unknown")


def parse_time(text: str | None, default: float) -> float:
    try:
        return datetime.fromisoformat((text or "").replace("Z", "+00:00")).timestamp()
    except ValueError:
        return default


_LATIN = dict(zip("абвгґдезиіклмнопрстуфхцчшщ", "a b v h g d e z y i k l m n o p r s t u f kh ts ch sh shch".split()))
_LATIN.update({"ж": "zh", "ь": "", "’": "", "'": "", "ʼ": ""})
_WORD_START = {"є": "ye", "ї": "yi", "й": "y", "ю": "yu", "я": "ya"}
_INSIDE = {"є": "ie", "ї": "i", "й": "i", "ю": "iu", "я": "ia"}


def latin(text: str) -> str:
    """Ukrainian in the official transliteration: the stick's fonts have no Cyrillic."""
    out, start = [], True
    for i, ch in enumerate(text):
        low = ch.lower()
        if low == "г" and i and text[i - 1].lower() == "з":
            r = "gh"
        elif low in _WORD_START:
            r = (_WORD_START if start else _INSIDE)[low]
        else:
            r = _LATIN.get(low, ch)
        if ch != low and r:
            r = r[0].upper() + r[1:]
        out.append(r)
        start = not low.isalpha() and low not in "’'ʼ"
    return "".join(out)


def zone_name(region: dict) -> str:
    name = region.get("regionName") or str(region.get("regionId"))
    if name.startswith("м. "):  # "м. Харків та Харківська територіальна громада"
        return latin(name[3:].split(" та ")[0])
    if name == "Автономна Республіка Крим":
        return "Krym"
    return latin(name.removesuffix(" район").replace("область", "oblast"))


def zone_label(name: str) -> str:
    """Short, for a map tile: Obukhivskyi is OBUKHIV, Donetskyi is DONETS."""
    if name.endswith(" oblast"):
        return "OBLAST"
    stem = name.removesuffix("skyi")
    if stem != name and stem.endswith(("et", "iu")):
        stem += "s"
    if len(stem) < 4:  # Lutskyi, not LUT
        stem = name
    return stem.split("-")[0][:9].upper()


class Regions:
    """The API's tree as the stick sees it: an oblast (group) holds its raions, itself, and a city listed apart.
    A community's alert shows on its raion. Zones are keyed by region id: raion names repeat across oblasts."""

    def __init__(self, states: list[dict]) -> None:
        self.zone_of: dict[str, str] = {}     # any region id -> its zone
        self.zones: dict[str, dict] = {}      # zone -> group, name, label, kind (c city, r raion, o oblast)
        self.groups: dict[str, list[str]] = {}

        def walk(region: dict, group: str, zone: str) -> None:
            rid = str(region.get("regionId"))
            if region.get("regionType") == "District":
                zone = self._add(rid, region, group, "r")
            self.zone_of[rid] = zone
            for child in region.get("regionChildIds") or []:
                walk(child, group, zone)

        for state in states:
            sid = str(state.get("regionId"))
            if sid in SKIP_STATES:
                continue
            group = CITY_OBLAST.get(sid, sid)
            self._add(sid, state, group, "c" if sid in CITY_OBLAST else "o")
            for child in state.get("regionChildIds") or []:
                walk(child, group, sid)
        first = {"c": 0, "r": 1, "o": 2}  # the city first and the oblast last, as the first firmware had them
        for zones in self.groups.values():
            zones.sort(key=lambda z: (first[self.zones[z]["kind"]], self.zones[z]["name"]))
            labels = [self.zones[z]["label"] for z in zones]
            for z in zones:  # Kharkiv the city and Kharkivskyi the raion
                if self.zones[z]["kind"] == "r" and labels.count(self.zones[z]["label"]) > 1:
                    self.zones[z]["label"] = self.zones[z]["label"][:7] + " R"

    def _add(self, rid: str, region: dict, group: str, kind: str) -> str:
        name = zone_name(region)
        self.zones[rid] = {"group": group, "name": name, "label": zone_label(name), "kind": kind}
        self.zone_of[rid] = rid
        self.groups.setdefault(group, []).append(rid)
        return rid

    def title(self, group: str) -> str:
        z = self.zones.get(group)
        return z["name"].removesuffix(" oblast") if z else group

    def listing(self) -> str:
        return "".join(f"r {g}|{self.title(g)}\n" for g in sorted(self.groups, key=self.title))


def load_regions(path: str) -> Regions:
    with open(path) as f:
        return Regions(json.load(f))


def alert_entry(alarm_type: str, levels: list[dict], default_since: float) -> dict:
    """One alarm type of one region, in the form the zones are made of."""
    threats = {threat_of(l.get("reason")) for l in levels} - {None}
    names = {level_of(l.get("alertLevel")) for l in levels}
    if alarm_type in OTHER_TYPES:
        threats.add(alarm_type)
        names.add("red")
    if not names:
        names = {"alert"}
    if alarm_type not in OTHER_TYPES and alarm_type not in ("AIR", "UNKNOWN", "CUSTOM"):
        names.add("unknown")  # a type this code has never heard of
    since = min((parse_time(l.get("createdAt"), default_since) for l in levels), default=default_since)
    return {"level": max(names, key=RANK.__getitem__), "threats": sorted(threats), "since": int(since)}


def zone_escalated(was: dict | None, s: dict) -> bool:
    return not was or RANK.get(s["level"], 4) > RANK.get(was["level"], 0) or bool(set(s["threats"]) - set(was["threats"]))


class State:
    def __init__(self, path: str | None = None, regions: Regions | None = None) -> None:
        self.path = path
        self.regions = regions or load_regions(BUNDLED_REGIONS)
        self.alerts: dict[str, dict[str, dict]] = {}  # region -> alarm type -> entry
        self.event_at: dict[str, float] = {}           # region -> when its last webhook came
        self.zones: dict[str, dict] = {}
        self.seq = 0  # grows with each escalation; the stick rings when a zone it watches gets a newer one
        self.events: list[dict] = []    # what happened, for the stick's history
        self.sessions: list[dict] = []  # {"zone", "start", "end"}, for its statistics
        self.last_ok = time.time()
        self.last_message: dict[str, float] = {}  # oblast -> when its newest escalation was sent, for the stick's delay
        if path and os.path.exists(path):
            with open(path) as f:
                saved = json.load(f)
            self.seq = saved["seq"]
            self.zones = {self._key(z): s for z, s in saved["zones"].items() if self._key(z)}
            self.events = [{**e, "zone": self._key(e["zone"])} for e in saved.get("events", []) if self._key(e["zone"])]
            self.sessions = [{**s, "zone": self._key(s["zone"])} for s in saved.get("sessions", []) if self._key(s["zone"])]

    def _key(self, zone: str) -> str | None:
        """Files before all of Ukraine named Kyiv's zones instead of keeping their ids."""
        if zone in self.regions.zones:
            return zone
        return next((z for z in self.regions.groups.get(COMPAT_GROUP, []) if self.regions.zones[z]["name"] == zone), None)

    def apply_event(self, message: dict, now: float) -> bool:
        """A webhook. The snapshot says what is left: the event's own status may end one reason of several."""
        region = str(message.get("regionId"))
        if region not in self.regions.zone_of:
            return False
        alarm_type = message.get("alarmType") or "UNKNOWN"
        if alarm_type in IGNORED_TYPES:
            return False
        levels = message.get("activeAlertLevels") or []
        before = self.seq
        mine = self.alerts.setdefault(region, {})
        if levels:
            mine[alarm_type] = alert_entry(alarm_type, levels, now)
        else:
            mine.pop(alarm_type, None)
        if not mine:
            self.alerts.pop(region, None)
        self.event_at[region] = now
        self._rebuild(now)
        if self.seq != before:
            group = self.regions.zones[self.regions.zone_of[region]]["group"]
            self.last_message[group] = parse_time(message.get("createdAt"), now)
        return True

    def apply_poll(self, regions: list[dict], now: float) -> None:
        """The whole list: a region missing from it has no alert, and an active one with no levels still counts."""
        fresh: dict[str, dict[str, dict]] = {}
        for item in regions:
            for alert in item.get("activeAlerts") or []:
                region = str(alert.get("regionId") or item.get("regionId"))
                alarm_type = alert.get("type") or "UNKNOWN"
                if region in self.regions.zone_of and alarm_type not in IGNORED_TYPES:
                    since = parse_time(alert.get("lastUpdate"), now)
                    fresh.setdefault(region, {})[alarm_type] = alert_entry(alarm_type, alert.get("activeAlertLevels") or [], since)
        for region, at in self.event_at.items():  # this answer may be older than that webhook
            if now - at < FRESH_EVENT_SEC:
                if region in self.alerts:
                    fresh[region] = self.alerts[region]
                else:
                    fresh.pop(region, None)
        self.alerts = fresh
        self.last_ok = now
        self._rebuild(now)

    def _rebuild(self, now: float) -> None:
        zones: dict[str, dict] = {}
        for region, types in self.alerts.items():
            zone = self.regions.zone_of[region]
            for entry in types.values():
                z = zones.setdefault(zone, {"level": "alert", "threats": [], "since": entry["since"]})
                if RANK[entry["level"]] > RANK[z["level"]]:
                    z["level"] = entry["level"]
                z["threats"] = sorted(set(z["threats"]) | set(entry["threats"]),
                                      key=lambda t: (ORDER.index(t) if t in ORDER else len(ORDER), t))
                z["since"] = min(z["since"], entry["since"])
        self._set(zones, now)

    def _set(self, zones: dict, now: float) -> None:
        escalated = {z for z, s in zones.items() if zone_escalated(self.zones.get(z), s)}
        if escalated:
            self.seq += 1
        for zone, s in zones.items():
            was = self.zones.get(zone) or {}
            s["seq"] = self.seq if zone in escalated else was.get("seq", self.seq)
        changed = zones != self.zones
        self._record(zones, now)
        self.zones = zones
        if self.path and changed:
            _atomic_write(self.path, json.dumps({"zones": zones, "seq": self.seq, "events": self.events,
                                                 "sessions": self.sessions}), 0o600)

    def _record(self, zones: dict, now: float) -> None:
        for zone, s in zones.items():
            was = self.zones.get(zone)
            if not was:
                self.sessions.append({"zone": zone, "start": s["since"], "end": None})
            if not was or was["level"] != s["level"]:
                self.events.append({"ts": int(now), "zone": zone, "level": s["level"], "threats": s["threats"]})
        for zone in set(self.zones) - set(zones):
            self.events.append({"ts": int(now), "zone": zone, "level": "none", "threats": []})
            for session in self.sessions:
                if session["zone"] == zone and session["end"] is None:
                    session["end"] = int(now)
        self.events = [e for e in self.events if now - e["ts"] < KEEP_SEC]
        self.sessions = [s for s in self.sessions if s["end"] is None or now - s["end"] < KEEP_SEC]

    def _name(self, zone: str) -> str:
        return self.regions.zones[zone]["name"]

    def _in(self, group: str, zone: str) -> bool:
        return self.regions.zones.get(zone, {}).get("group") == group

    def _zone_list(self, group: str) -> list[str]:
        """The oblast's zones in a fixed order: the stick keeps its choice as their positions."""
        return [f"obl {group}"] + ["n " + "|".join((z["name"], z["label"], z["kind"]))
                                   for z in (self.regions.zones[i] for i in self.regions.groups.get(group, []))]

    def render(self, group: str = COMPAT_GROUP, now: float | None = None) -> str:
        now = now or time.time()
        mine = {z: s for z, s in self.zones.items() if self._in(group, z)}
        active = sorted(mine, key=lambda z: -RANK.get(mine[z]["level"], 0))
        level = mine[active[0]]["level"] if active else "none"
        zone = self._name(active[0]) + (f" +{len(active) - 1}" if len(active) > 1 else "") if active else ""
        found = {t for s in mine.values() for t in s["threats"]}
        threats = sorted(found, key=lambda t: ORDER.index(t) if t in ORDER else len(ORDER))
        lines = [f"ts {int(now)}", f"seq {self.seq}", f"msg {int(self.last_message.get(group, 0))}", f"level {level}",
                 f"bot {int(now - self.last_ok < API_SILENT_SEC)}",
                 f"zone {zone[:31]}", f"threat {', '.join(threats)[:47]}", *self._zone_list(group)]
        for z in active:  # one line per zone: the stick itself chooses which zones count
            s = mine[z]
            lines.append("z " + "|".join(str(v) for v in (self._name(z), s["level"], s.get("seq", self.seq),
                                                          s.get("since") or int(now), 0, ",".join(s["threats"]), "", 0)))
        return "\n".join(lines) + "\n"

    def render_history(self, group: str = COMPAT_GROUP, now: float | None = None) -> str:
        now = now or time.time()
        lines = [f"ts {int(now)}", *self._zone_list(group)]
        seen: dict[str, int] = {}
        for e in reversed(self.events):  # newest first, and a busy zone must not push out a quiet one
            if not self._in(group, e["zone"]):
                continue
            seen[e["zone"]] = seen.get(e["zone"], 0) + 1
            if seen[e["zone"]] <= HISTORY_PER_ZONE:
                lines.append("h " + "|".join(str(v) for v in (e["ts"], self._name(e["zone"]), e["level"], ",".join(e["threats"]))))
        day = datetime.fromtimestamp(now, KYIV).replace(hour=0, minute=0, second=0, microsecond=0).timestamp()
        week = now - 7 * 86400
        for zone in sorted({s["zone"] for s in self.sessions if self._in(group, s["zone"])}, key=self._name):
            mine = [s for s in self.sessions if s["zone"] == zone]
            lines.append("s " + "|".join(str(v) for v in (self._name(zone), *_stats(mine, day, now), *_stats(mine, week, now))))
        return "\n".join(lines) + "\n"


def _stats(sessions: list[dict], start: float, now: float) -> tuple[int, int]:
    """Alerts that began after `start`, and minutes under alert after it. Sessions that overlap count as one:
    a zone can return with the same start, and a day must not hold over 24 hours."""
    merged: list[list[float]] = []
    for begin, end in sorted((s["start"], s["end"] or now) for s in sessions):
        if merged and begin <= merged[-1][1]:
            merged[-1][1] = max(merged[-1][1], end)
        else:
            merged.append([begin, end])
    seconds = sum(max(0, end - max(begin, start)) for begin, end in merged)
    return sum(begin >= start for begin, _ in merged), int(seconds // 60)


def _atomic_write(path: str, text: str, mode: int) -> None:
    tmp = f"{path}.tmp"
    with open(tmp, "w") as f:
        f.write(text)
    os.chmod(tmp, mode)
    os.replace(tmp, path)  # the web server never serves a half-written file


# ---------- webhook ----------

class Verifier:
    """The webhook address is public: without this anyone could ring the siren or send a false all-clear."""

    def __init__(self, pem: bytes) -> None:
        from cryptography import x509
        from cryptography.hazmat.primitives.serialization import load_pem_public_key
        self.key = x509.load_pem_x509_certificate(pem).public_key() if b"CERTIFICATE" in pem else load_pem_public_key(pem)
        self.seen: dict[str, float] = {}
        self.lock = threading.Lock()

    def check(self, signature_b64: str | None, timestamp: str | None, algorithm: str | None, body: bytes, now: float) -> bool:
        from cryptography.exceptions import InvalidSignature
        from cryptography.hazmat.primitives import hashes
        from cryptography.hazmat.primitives.asymmetric import padding
        if not signature_b64 or not timestamp or (algorithm and algorithm.lower() != "rsa-sha256"):
            return False
        try:
            if abs(now - int(timestamp)) > WEBHOOK_MAX_AGE_SEC:
                return False
            signature = base64.b64decode(signature_b64, validate=True)
            self.key.verify(signature, timestamp.encode() + b"." + body, padding.PKCS1v15(), hashes.SHA256())
        except (ValueError, InvalidSignature):
            return False
        with self.lock:  # a signed message replayed within its window
            digest = hashlib.sha256(signature).hexdigest()
            self.seen = {k: v for k, v in self.seen.items() if now - v <= 2 * WEBHOOK_MAX_AGE_SEC}
            if digest in self.seen:
                return False
            self.seen[digest] = now
        return True


def make_handler(state: State, lock: threading.Lock, verifier: Verifier | None, publish):
    class Webhook(BaseHTTPRequestHandler):
        def do_POST(self):
            try:
                size = int(self.headers.get("Content-Length", ""))
            except ValueError:
                return self._answer(411)
            if self.path != "/peace-stick/webhook" or not 0 < size <= WEBHOOK_MAX_BODY:
                return self._answer(404)
            body = self.rfile.read(size)
            now = time.time()
            if not verifier or not verifier.check(self.headers.get("X-Webhook-Signature"), self.headers.get("X-Webhook-Timestamp"),
                                                  self.headers.get("X-Webhook-Signature-Alg"), body, now):
                log.warning("Webhook rejected: bad signature, time or repeat")
                return self._answer(401)
            try:
                message = json.loads(body)
            except ValueError:
                return self._answer(400)
            with lock:
                if isinstance(message, dict) and state.apply_event(message, now):
                    log.info("webhook region=%s %s %s %s reason=%r active=%d seq=%s zones=%s", message.get("regionId"),
                             message.get("status"), message.get("alarmType"), message.get("alertLevel"),
                             str(message.get("reason"))[:40], len(message.get("activeAlertLevels") or []),
                             state.seq, sorted(state.zones))
                    publish()
            self._answer(200)

        def _answer(self, code: int) -> None:
            self.send_response(code)
            self.send_header("Content-Length", "0")
            self.end_headers()

        def log_message(self, *args):  # the journal must not fill up with strangers probing a public address
            pass

    return Webhook


# ---------- the API ----------

def api_request(path: str, method: str = "GET", payload: dict | None = None):
    data = json.dumps(payload).encode() if payload is not None else None
    request = urllib.request.Request(API + path, data=data, method=method, headers={
        "Authorization": API_KEY, "Accept": "application/json", "User-Agent": "peace-stick-bridge/1.0",
        **({"Content-Type": "application/json"} if data else {})})
    with urllib.request.urlopen(request, timeout=20) as response:
        text = response.read()
    return json.loads(text) if text else None


class Setup:
    """One-time calls that share the single request a minute with the poll."""

    def __init__(self, state: State) -> None:
        self.state = state
        self.regions_path = os.path.join(PRIVATE, "regions.json")
        self.marker = os.path.join(PRIVATE, "webhook-registered")
        self.tries = {"regions": 0, "webhook": 0}

    def step(self) -> bool:
        """Does one pending call; False if nothing is left and this minute's request can poll."""
        if not os.path.exists(self.regions_path) and self.tries["regions"] < SETUP_TRIES:
            self.tries["regions"] += 1
            states = api_request("/regions")["states"]
            _atomic_write(self.regions_path, json.dumps(states), 0o600)
            self.state.regions = Regions(states)
            log.info("Loaded %d oblasts", len(self.state.regions.groups))
            return True
        if not os.path.exists(self.marker) and self.tries["webhook"] < SETUP_TRIES:
            self.tries["webhook"] += 1
            # A second attempt updates a subscription that an earlier run may have made.
            api_request("/webhook", "POST" if self.tries["webhook"] == 1 else "PATCH", {"webHookUrl": WEBHOOK_URL})
            _atomic_write(self.marker, WEBHOOK_URL, 0o600)
            log.info("Webhook registered")
            return True
        return False


def poller(state: State, lock: threading.Lock, publish) -> None:
    setup = Setup(state)
    while True:
        try:
            if not setup.step():
                regions = api_request("/alerts")
                with lock:
                    before = sorted(state.zones)
                    state.apply_poll(regions, time.time())
                    if sorted(state.zones) != before:
                        log.info("poll changed zones: %s -> %s", before, sorted(state.zones))
                    publish()
        except (urllib.error.URLError, OSError, ValueError, KeyError, TypeError) as e:  # the stick shows it through "bot 0"
            log.warning("API call failed: %s", e)
        time.sleep(POLL_SEC)


def main() -> None:
    logging.basicConfig(level=logging.INFO, format="%(asctime)s %(levelname)s %(message)s")
    if not API_KEY:
        raise SystemExit("UA_API_KEY is not set")
    fetched = os.path.join(PRIVATE, "regions.json")  # newer than the bundled copy, once the API answered
    state = State(os.path.join(PRIVATE, "state.json"), load_regions(fetched if os.path.exists(fetched) else BUNDLED_REGIONS))
    lock = threading.Lock()
    os.makedirs(OUT_DIR, exist_ok=True)

    def publish(history: bool = True) -> None:
        now = time.time()
        _atomic_write(os.path.join(OUT_DIR, "regions.txt"), state.regions.listing(), 0o644)
        _atomic_write(OUT, state.render(COMPAT_GROUP, now), 0o644)
        if history:
            _atomic_write(HISTORY_OUT, state.render_history(COMPAT_GROUP, now), 0o644)
        for group in state.regions.groups:
            _atomic_write(os.path.join(OUT_DIR, f"{group}.txt"), state.render(group, now), 0o644)
            if history:
                _atomic_write(os.path.join(OUT_DIR, f"{group}-history.txt"), state.render_history(group, now), 0o644)

    verifier = None
    try:
        with open(PUBLIC_KEY, "rb") as f:
            verifier = Verifier(f.read())
    except (OSError, ValueError) as e:  # the poll still works, only slower
        log.error("No webhook key, webhooks will be rejected: %s", e)

    server = ThreadingHTTPServer(("127.0.0.1", WEBHOOK_PORT), make_handler(state, lock, verifier, publish))
    threading.Thread(target=server.serve_forever, daemon=True).start()
    threading.Thread(target=poller, args=(state, lock, publish), daemon=True).start()
    log.info("Listening for webhooks, polling every %s s", POLL_SEC)

    beats = 0
    while True:
        with lock:
            publish(beats % HISTORY_EVERY == 0)
        beats += 1
        time.sleep(HEARTBEAT_SEC)


if __name__ == "__main__":
    main()
