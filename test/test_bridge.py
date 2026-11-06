# Run: python3 test/test_bridge.py   Messages follow the examples in the API's webhook documentation.
import base64
import json
import sys
import threading
from datetime import datetime
from http.client import HTTPConnection
from http.server import ThreadingHTTPServer
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent.parent / "vps"))
from peace_bridge import (BUNDLED_REGIONS, KYIV, RejectLog, Signer, State, Verifier, alert_entry, latin, level_of,  # noqa: E402
                          load_regions, make_handler, parse_time, threat_of, zone_escalated)

NOW = datetime(2026, 9, 7, 15, 40, tzinfo=KYIV).timestamp()


def fields(state, now=NOW, group="14"):
    return dict(line.split(" ", 1) for line in state.render(group, now).splitlines() if " " in line and line[:2] not in ("z ", "n "))


def zone_lines(state, now=NOW, group="14"):
    return {l[2:].split("|")[0]: l[2:].split("|") for l in state.render(group, now).splitlines() if l.startswith("z ")}


def event(region, levels, status="Activate", alarm="AIR", created="2026-09-07T12:05:00Z"):
    return {"status": status, "regionId": region, "alarmType": alarm, "createdAt": created,
            "activeAlertLevels": [{"alertLevel": l, "reason": r, "createdAt": "2026-09-07T11:42:00Z"} for l, r in levels]}


# Reasons are free text: the most specific word wins
assert threat_of("Ballistic missile threat") == "BALLISTIC" and threat_of("Missile threat") == "MISSILES"
assert threat_of("UAV activity") == "DRONES" and threat_of("Загроза балістики") == "BALLISTIC" and threat_of("БпЛА") == "DRONES"
assert threat_of("") is None and threat_of(None) is None and threat_of("something else") is None

# Levels: an unknown word is loud
assert level_of("Red") == "red" and level_of("Yellow") == "yellow" and level_of("") == "alert" and level_of("Purple") == "unknown"
assert parse_time("2026-09-07T12:05:00Z", 0) == datetime(2026, 9, 7, 15, 5, tzinfo=KYIV).timestamp()
assert parse_time("garbage", 7.0) == 7.0 and parse_time(None, 7.0) == 7.0

# Entries of one region
e = alert_entry("AIR", [{"alertLevel": "Yellow", "reason": "UAV activity"}, {"alertLevel": "Red", "reason": "Missile threat"}], NOW)
assert e["level"] == "red" and set(e["threats"]) == {"DRONES", "MISSILES"}
assert alert_entry("AIR", [], NOW)["level"] == "alert"                   # active, with no level given
assert alert_entry("CHEMICAL", [], NOW) == {"level": "red", "threats": ["CHEMICAL"], "since": int(NOW)}
assert alert_entry("MARTIANS", [{"alertLevel": "Yellow"}], NOW)["level"] == "unknown"

# Refused webhooks: the first is reported, then at most one summary a minute
rl = RejectLog()
assert rl.add(1000) == 1 and all(rl.add(1000 + i) == 0 for i in range(1, 59))
assert rl.add(1061) == 59 and rl.add(1062) == 0

# The real tree: a community belongs to its raion, a city listed apart to the oblast around it
assert latin("Київська область") == "Kyivska oblast" and latin("Кам’янець-Подільський") == "Kamianets-Podilskyi"
assert latin("Ярмолинці") == "Yarmolyntsi" and latin("Згурівка") == "Zghurivka" and latin("Їжак") == "Yizhak"
R = load_regions(BUNDLED_REGIONS)
assert R.zone_of["701"] == "75" and R.zone_of["1303"] == "126" and R.zones["126"]["group"] == "22"
assert [R.zones[z]["name"] for z in R.groups["14"]] == ["Kyiv", "Bilotserkivskyi", "Boryspilskyi", "Brovarskyi", "Buchanskyi",
                                                         "Fastivskyi", "Obukhivskyi", "Vyshhorodskyi", "Kyivska oblast"]
assert R.zones["1293"]["group"] == "22" and R.zones["564"]["group"] == "12" and "0" not in R.zones   # no test region
assert len(R.groups) == 25 and all(len(z) <= 12 for z in R.groups.values())      # the stick holds 12 zones
assert all(len(z["name"]) < 24 and len(z["label"]) <= 9 for z in R.zones.values())  # its name and tile sizes
assert R.zones["126"]["label"] == "BOHODUKHI" and "r 14|Kyivska" in R.listing()
assert [R.zones[z]["label"] for z in R.groups["22"]].count("KHARKIV") == 1    # the city, not also its raion

# The three examples from the documentation, in order
s = State(regions=R)
assert s.apply_event(event(14, [("Yellow", "UAV activity"), ("Red", "Missile threat"), ("Red", "Ballistic missile threat")]), NOW)
z = zone_lines(s)["Kyivska oblast"]
assert z[1] == "red" and z[2] == "1" and z[5] == "BALLISTIC,MISSILES,DRONES" and z[6] == "" and z[7] == "0"
assert int(z[3]) == int(datetime(2026, 9, 7, 14, 42, tzinfo=KYIV).timestamp())  # the oldest active level
assert fields(s)["level"] == "red" and int(fields(s)["msg"]) == int(datetime(2026, 9, 7, 15, 5, tzinfo=KYIV).timestamp())

# One reason ends: "DEACTIVATE" with a snapshot still full, so the alert stays and nothing rings
s.apply_event(event(14, [("Yellow", "UAV activity"), ("Red", "Ballistic missile threat")], "DEACTIVATE"), NOW + 60)
assert fields(s)["level"] == "red" and s.seq == 1 and zone_lines(s)["Kyivska oblast"][5] == "BALLISTIC,DRONES"

# The last one ends: the empty snapshot is the all-clear, whatever the status says
s.apply_event(event(14, [], "DEACTIVATE"), NOW + 120)
assert fields(s)["level"] == "none" and not s.zones and fields(s)["zone"] == ""

# An all-clear with status Activate still clears: only the snapshot counts
s.apply_event(event(14, [("Red", "")]), NOW + 130)
s.apply_event(event(14, [], "Activate"), NOW + 140)
assert not s.zones

# Rings: each escalation, not a repeat
s = State(regions=R)
s.apply_event(event(31, [("Yellow", "UAV activity")]), NOW)
assert s.seq == 1
s.apply_event(event(31, [("Yellow", "UAV activity")]), NOW + 10)
assert s.seq == 1
s.apply_event(event(31, [("Red", "UAV activity")]), NOW + 20)
assert s.seq == 2
s.apply_event(event(31, [("Red", "UAV activity"), ("Red", "Missile threat")]), NOW + 30)   # new threat, same level
assert s.seq == 3
s.apply_event(event(31, [("Yellow", "UAV activity")]), NOW + 40)                           # going down does not ring
assert s.seq == 3 and fields(s)["level"] == "yellow"
assert not zone_escalated({"level": "red", "threats": ["DRONES"]}, {"level": "yellow", "threats": ["DRONES"]})

# Each zone has its own seq, so a stick watching Kyiv is not rung by Bucha
s = State(regions=R)
s.apply_event(event(31, [("Red", "")]), NOW)
s.apply_event(event(75, [("Yellow", "")]), NOW + 10)
z = zone_lines(s)
assert z["Kyiv"][2] == "1" and z["Buchanskyi"][2] == "2"

# A community's alert shows on its raion, and the worst one of the raion wins; each ends on its own
s = State(regions=R)
s.apply_event(event(701, [("Yellow", "")]), NOW)
s.apply_event(event(75, [("Red", "")]), NOW + 10)
assert zone_lines(s)["Buchanskyi"][1] == "red"
s.apply_event(event(75, [], "DEACTIVATE"), NOW + 20)
assert zone_lines(s)["Buchanskyi"][1] == "yellow"
s.apply_event(event(701, [], "DEACTIVATE"), NOW + 30)
assert not s.zones

# Several alarm types live side by side in one region
s = State(regions=R)
s.apply_event(event(31, [("Yellow", "")]), NOW)
s.apply_event(event(31, [], alarm="ARTILLERY"), NOW + 1)
assert zone_lines(s)["Kyiv"][1] == "yellow"
s.apply_event(event(31, [("Red", "x")], alarm="ARTILLERY"), NOW + 2)
assert zone_lines(s)["Kyiv"][1] == "red" and "ARTILLERY" in zone_lines(s)["Kyiv"][5]
s.apply_event(event(31, [], alarm="AIR"), NOW + 3)
assert zone_lines(s)["Kyiv"][1] == "red"

# Unknown, or only information: ignored
s = State(regions=R)
assert not s.apply_event(event(424242, [("Red", "")]), NOW) and not s.zones
assert not s.apply_event(event(31, [("Red", "")], alarm="INFO"), NOW) and not s.zones

# The poll is the truth: it brings what a webhook lost, and ends what a webhook did not
POLL = [{"regionId": "31", "activeAlerts": [{"regionId": "31", "type": "AIR", "lastUpdate": "2026-09-07T12:00:00Z",
                                             "activeAlertLevels": [{"alertLevel": "Red", "reason": "", "createdAt": "2026-09-07T12:00:00Z"}]}]},
        {"regionId": "22", "activeAlerts": [{"regionId": "22", "type": "AIR", "activeAlertLevels": []}]}]
s = State(regions=R)
s.apply_poll(POLL, NOW)
assert fields(s)["level"] == "red" and s.seq == 1 and set(s.zones) == {"31", "22"}
assert set(zone_lines(s)) == {"Kyiv"} and set(zone_lines(s, group="22")) == {"Kharkivska oblast"}   # each oblast its own file
s.apply_poll(POLL, NOW + 65)
assert s.seq == 1
s.apply_poll([], NOW + 130)
assert not s.zones
# An active alert the poll lists with no levels counts, as the plain "air alert"
s.apply_poll([{"regionId": "31", "activeAlerts": [{"type": "AIR", "activeAlertLevels": []}]}], NOW + 140)
assert zone_lines(s, NOW + 140)["Kyiv"][1] == "alert"

# A poll answer older than a webhook that just came does not undo it, in either direction
s = State(regions=R)
s.apply_event(event(31, [("Red", "")]), NOW)
s.apply_poll([], NOW + 65)
assert s.seq == 1 and "31" in s.zones
s.apply_poll([], NOW + 131)     # two polls agree and no webhook came since: now it counts
assert not s.zones
s.apply_event(event(31, [("Red", "")]), NOW + 200)
s.apply_event(event(31, [], "DEACTIVATE"), NOW + 210)
s.apply_poll(POLL[:1], NOW + 212)   # a lagging list does not bring a cleared alert back, which would ring again
assert not s.zones and s.seq == 2

# A failing API shows on the stick only after a while
s = State(regions=R)
s.apply_poll([], NOW)
assert fields(s, NOW + 299)["bot"] == "1" and fields(s, NOW + 301)["bot"] == "0"

# History and statistics
NOON = datetime(2026, 11, 5, 12, 0, tzinfo=KYIV).timestamp()
s = State(regions=R)
s.apply_event({"regionId": 31, "alarmType": "AIR", "activeAlertLevels": [{"alertLevel": "Red", "reason": "Ballistic", "createdAt": datetime.fromtimestamp(NOON - 600, KYIV).isoformat()}]}, NOON)
s.apply_event(event(31, []), NOON + 600)
s.apply_event({"regionId": 31, "alarmType": "AIR", "activeAlertLevels": [{"alertLevel": "Yellow", "reason": "", "createdAt": datetime.fromtimestamp(NOON + 1200, KYIV).isoformat()}]}, NOON + 1200)
text = s.render_history(now=NOON + 1800)
assert "n Kyiv|KYIV|c" in text and "obl 14" in text and not s.render_history("22", NOON + 1800).count("h ")
events = [l for l in text.splitlines() if l.startswith("h ")]
assert [e.split("|")[2] for e in events] == ["yellow", "none", "red"]   # newest first
assert events[2].endswith("|BALLISTIC")
stat = next(l for l in text.splitlines() if l.startswith("s Kyiv")).split("|")
assert stat[1:3] == ["2", "30"] and stat[3:5] == ["2", "30"]   # 11:50-12:10 and 12:20-12:30
stat = next(l for l in s.render_history(now=NOON + 86400).splitlines() if l.startswith("s Kyiv")).split("|")
assert stat[1] == "0" and stat[3] == "2"                       # today starts at midnight, the week keeps it

# Overlapping sessions are one alert: a day never holds over 24 hours
s = State(regions=R)
back = datetime.fromtimestamp(NOON - 3600, KYIV).isoformat()
for minute in range(0, 60, 10):
    s.apply_event({"regionId": 14, "alarmType": "AIR", "activeAlertLevels": [{"alertLevel": "Yellow", "reason": "", "createdAt": back}]}, NOON + minute * 60)
    s.apply_event(event(14, []), NOON + (minute + 5) * 60)
stat = next(l for l in s.render_history(now=NOON + 3600).splitlines() if l.startswith("s Kyivska")).split("|")
assert stat[1] == "1" and stat[2] == "115", stat

# State survives a restart, including a file written by the Telegram version
old = Path(__file__).parent / "_old_state.json"
old.write_text('{"zones": {"Kyiv": {"level": "lost", "threats": ["DRONES"], "seq": 7, "lost_since": 5}}, "seq": 7}')
try:
    s = State(str(old), R)
    assert s.seq == 7 and "31" in s.zones      # the old file named Kyiv's zones
    s.apply_poll([], NOW)       # the first poll replaces what the old version knew
    assert not s.zones and s.seq == 7
    assert json.loads(old.read_text())["seq"] == 7
    s = State(str(old), R)
    s.apply_poll(POLL, NOW + 100)
    assert s.seq == 8
    assert State(str(old), R).seq == 8
finally:
    old.unlink(missing_ok=True)

try:
    from cryptography.hazmat.primitives import hashes, serialization
    from cryptography.hazmat.primitives.asymmetric import padding, rsa
except ImportError:
    print("bridge tests passed (signature tests skipped: no cryptography here)")
    raise SystemExit(0)

# ---------- signatures, end to end over HTTP ----------
private = rsa.generate_private_key(public_exponent=65537, key_size=2048)
PEM = private.public_key().public_bytes(serialization.Encoding.PEM, serialization.PublicFormat.SubjectPublicKeyInfo)
other = rsa.generate_private_key(public_exponent=65537, key_size=2048)


def sign(body, ts, key=private):
    return base64.b64encode(key.sign(ts.encode() + b"." + body, padding.PKCS1v15(), hashes.SHA256())).decode()


v = Verifier(PEM)
body = b'{"regionId":31}'
ts = str(int(NOW))
assert v.check(sign(body, ts), ts, "rsa-sha256", body, NOW)
assert not v.check(sign(body, ts), ts, "rsa-sha256", body, NOW)                # the same message again
assert v.check(sign(b'{"regionId":14}', ts), ts, "", b'{"regionId":14}', NOW)  # the algorithm header is optional
assert not v.check(sign(body, ts, other), ts, "rsa-sha256", body, NOW)         # someone else's key
assert not v.check(sign(body, ts), ts, "rsa-sha256", body + b" ", NOW)         # changed in transit
assert not v.check(sign(body, ts), ts, "sha1", body, NOW)
assert not v.check(sign(body, ts), str(int(NOW) + 1), "rsa-sha256", body, NOW)  # the time is part of what is signed
old_ts = str(int(NOW) - 301)
assert not v.check(sign(body, old_ts), old_ts, "rsa-sha256", body, NOW)        # too old
future = str(int(NOW) + 301)
assert not v.check(sign(body, future), future, "rsa-sha256", body, NOW)
assert not v.check(None, ts, "rsa-sha256", body, NOW) and not v.check("x", None, "", body, NOW)
assert not v.check("not base64!!", ts, "", body, NOW) and not v.check(sign(body, "abc"), "abc", "", body, NOW)

# Alert files are signed as the stick checks them: everything before the last "sig " line
from cryptography.exceptions import InvalidSignature  # noqa: E402
from cryptography.hazmat.primitives.asymmetric import ec  # noqa: E402
feed_key = ec.generate_private_key(ec.SECP256R1())
signer = Signer(feed_key.private_bytes(serialization.Encoding.PEM, serialization.PrivateFormat.PKCS8, serialization.NoEncryption()))
plain = State(regions=R).render("14", NOW)
signed_text = signer.sign(plain)
cut = signed_text.rindex("\nsig ") + 1
assert signed_text[:cut] == plain and signed_text.endswith("\n") and signed_text.count("\nsig ") == 1
feed_key.public_key().verify(base64.b64decode(signed_text[cut + 4:].strip()), signed_text[:cut].encode(), ec.ECDSA(hashes.SHA256()))
try:
    feed_key.public_key().verify(base64.b64decode(signed_text[cut + 4:].strip()), signed_text[:cut].replace("none", "red").encode(),
                                 ec.ECDSA(hashes.SHA256()))
    raise AssertionError("a changed file must not verify")
except InvalidSignature:
    pass

published = []
state = State(regions=R)
lock = threading.Lock()
server = ThreadingHTTPServer(("127.0.0.1", 0), make_handler(state, lock, Verifier(PEM), lambda: published.append(1)))
threading.Thread(target=server.serve_forever, daemon=True).start()


def post(payload, headers=None, path="/peace-stick/webhook"):
    c = HTTPConnection("127.0.0.1", server.server_address[1], timeout=5)
    c.request("POST", path, payload, headers or {})
    code = c.getresponse().status
    c.close()
    return code


real_ts = str(int(datetime.now().timestamp()))
signed = json.dumps(event(31, [("Red", "Ballistic missile threat")])).encode()
good = {"X-Webhook-Signature": sign(signed, real_ts), "X-Webhook-Timestamp": real_ts, "X-Webhook-Signature-Alg": "rsa-sha256"}
assert post(signed) == 401 and not state.zones                                 # no signature: a false alarm is refused
assert post(signed, {**good, "X-Webhook-Signature": sign(signed, real_ts, other)}) == 401 and not state.zones
assert post(signed, good, "/other") == 404 and not state.zones
assert post(signed, good) == 200 and state.zones["31"]["level"] == "red" and published
assert post(signed, good) == 401                                               # replayed
junk = b"not json"
assert post(junk, {"X-Webhook-Signature": sign(junk, real_ts), "X-Webhook-Timestamp": real_ts}) == 400
huge = b"x" * 70000
assert post(huge, {"X-Webhook-Signature": sign(huge, real_ts), "X-Webhook-Timestamp": real_ts}) == 404
server.shutdown()

print("bridge tests passed")
