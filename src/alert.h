// Pure logic with no Arduino dependency, so test/test_alert.cpp can run it on the PC.
#pragma once
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>

const unsigned long LINK_GRACE_MS = 60000;  // a short network blip should not ring
const long long STALE_AFTER_S = 60;
const long NEVER_ACKED = -1;
const int MAX_ZONES = 12;
const int MAX_EVENTS = 20;
const int ZONE_NAME = 24;
const int USB_ON_MV = 4120, USB_OFF_MV = 4050;  // this board cannot see USB, only the battery's voltage
const unsigned long REMIND_EVERY_MS = 15UL * 60 * 1000, NIGHT_LINK_EVERY_MS = 10UL * 60 * 1000;

// In order of danger. L_LOST: an alert that vanished with no all-clear, so it may still be on.
enum Level { L_NONE, L_LOST, L_ALERT, L_YELLOW, L_RED, L_UNKNOWN };
enum Link { LINK_WAIT, LINK_OK, LINK_NOWIFI, LINK_NOANSWER, LINK_STALE, LINK_BOT };
enum Sound { S_NONE, S_SIREN, S_BALLISTIC, S_BEEPS, S_LINK, S_CHIME, S_SOFT };

const int REGION_ID = 8;
const unsigned DEFAULT_ZONES = ~0u;  // a new oblast: all of it, until the user narrows it down

// The oblast's zones, as the bridge lists them in a fixed order; the user's choice is a bit per position.
struct ZoneInfo {
  char name[ZONE_NAME];  // Latin: what alert lines and history refer to
  char label[10];        // short, for the map
  char kind;             // 'c' a city listed apart, 'r' a raion, 'o' the whole oblast
  char ua[56], uaLabel[20];  // the same in Ukrainian (UTF-8, two bytes a letter), when the bridge sends them
};
struct ZoneList {
  int count;
  ZoneInfo zones[MAX_ZONES];
};

inline int zoneIndex(const ZoneList& l, const char* name) {
  for (int i = 0; i < l.count; i++)
    if (!strcmp(l.zones[i].name, name)) return i;
  return -1;
}

// A zone we do not know could be anything, so it counts. An alert for the whole oblast concerns
// whoever watches one of its raions; a city listed apart is not one of them.
inline bool zoneWanted(const ZoneList& l, int index, unsigned mask) {
  if (index < 0) return true;
  if (mask & (1u << index)) return true;
  if (l.zones[index].kind != 'o') return false;
  for (int i = 0; i < l.count; i++)
    if (l.zones[i].kind == 'r' && (mask & (1u << i))) return true;
  return false;
}

inline int watchedCount(const ZoneList& l, unsigned mask) {
  int n = 0;
  for (int i = 0; i < l.count; i++) n += (mask >> i) & 1;
  return n;
}

struct ZoneAlert {
  char name[ZONE_NAME];
  Level level;
  long seq;
  long long since;
  int conf;
  char threats[32];
  char direction[24];
  long dirSeq;  // when the direction last changed
};

struct Feed {
  char obl[REGION_ID];  // whose file this is: right after a change of oblast the old one may still arrive
  ZoneList list;
  long long ts;
  long long msgTs;  // when the bot sent the newest escalation
  long seq;
  bool botOk;
  int count;
  ZoneAlert zones[MAX_ZONES];
};

// An unknown word means the bridge knows something we do not: treat it as loud, not as calm.
inline Level parseLevel(const char* s, size_t n) {
  if (n == 4 && !strncmp(s, "none", 4)) return L_NONE;
  if (n == 4 && !strncmp(s, "lost", 4)) return L_LOST;
  if (n == 5 && !strncmp(s, "alert", 5)) return L_ALERT;
  if (n == 6 && !strncmp(s, "yellow", 6)) return L_YELLOW;
  if (n == 3 && !strncmp(s, "red", 3)) return L_RED;
  return L_UNKNOWN;
}

inline void copyValue(char* to, size_t size, const char* from, size_t n) {
  if (n >= size) n = size - 1;
  memcpy(to, from, n);
  to[n] = 0;
}

// Takes the next '|' separated field of [s, end); leaves s after it. Empty once the line is used up.
inline void nextField(const char*& s, const char* end, const char*& field, size_t& len) {
  field = s;
  const char* bar = (const char*)memchr(s, '|', end - s);
  len = (bar ? bar : end) - s;
  s = bar ? bar + 1 : end;
}

// Format of a zone list line: name|label|kind|Ukrainian name|Ukrainian label (the last two optional)
inline void parseZoneInfo(const char* s, const char* end, ZoneList& l) {
  if (l.count >= MAX_ZONES) return;
  ZoneInfo& z = l.zones[l.count++];
  const char* f;
  size_t n;
  nextField(s, end, f, n);
  copyValue(z.name, sizeof z.name, f, n);
  nextField(s, end, f, n);
  copyValue(z.label, sizeof z.label, f, n);
  nextField(s, end, f, n);
  z.kind = n ? *f : 'r';
  nextField(s, end, f, n);
  copyValue(z.ua, sizeof z.ua, f, n);
  nextField(s, end, f, n);
  copyValue(z.uaLabel, sizeof z.uaLabel, f, n);
}

inline void parseZoneLine(const char* s, const char* end, ZoneAlert& z) {
  const char* f;
  size_t n;
  nextField(s, end, f, n);
  copyValue(z.name, sizeof z.name, f, n);
  nextField(s, end, f, n);
  z.level = parseLevel(f, n);
  nextField(s, end, f, n);
  z.seq = strtol(f, nullptr, 10);
  nextField(s, end, f, n);
  z.since = strtoll(f, nullptr, 10);
  nextField(s, end, f, n);
  z.conf = atoi(f);
  nextField(s, end, f, n);
  copyValue(z.threats, sizeof z.threats, f, n);
  nextField(s, end, f, n);
  copyValue(z.direction, sizeof z.direction, f, n);
  nextField(s, end, f, n);
  z.dirSeq = n ? strtol(f, nullptr, 10) : 0;
}

// The bridge ends each alert file with "sig <base64>", a signature over everything before that line.
// Finds it; false if it is missing or anything follows it.
inline bool splitSigned(const char* s, size_t& signedLen, const char*& sig, size_t& sigLen) {
  const char* at = nullptr;
  for (const char* p = strstr(s, "\nsig "); p; p = strstr(p + 1, "\nsig ")) at = p;
  if (!at) return false;
  signedLen = at - s + 1;
  sig = at + 5;
  const char* end = strchr(sig, '\n');
  sigLen = end ? (size_t)(end - sig) : strlen(sig);
  return sigLen > 0 && (!end || end[1] == 0);
}

// "v1.4.0" as 1004000, so versions compare as numbers; anything else (a "dev" build) is 0, older than every release.
inline long versionNumber(const char* v) {
  int major = 0, minor = 0, patch = 0;
  if (!v || sscanf(v, "v%d.%d.%d", &major, &minor, &patch) != 3 || major < 0 || minor < 0 || patch < 0) return 0;
  return major * 1000000L + minor * 1000L + patch;
}

// The signed release manifest (sign-release.sh): "version vX.Y.Z", then "<sha256 hex>  <file>" lines.
// Takes the version and the firmware's hash; false unless both are there and well formed.
inline bool parseManifest(const char* s, char* version, size_t versionSize, char* firmwareHash /* 65 bytes */) {
  version[0] = firmwareHash[0] = 0;
  while (s && *s) {
    const char* end = strchr(s, '\n');
    size_t len = end ? (size_t)(end - s) : strlen(s);
    if (len > 8 && !strncmp(s, "version ", 8)) copyValue(version, versionSize, s + 8, len - 8);
    if (len == 64 + 2 + 12 && !strncmp(s + 64, "  firmware.bin", 14)) {
      bool hex = true;
      for (int i = 0; i < 64 && hex; i++) hex = isxdigit((unsigned char)s[i]) && !isupper((unsigned char)s[i]);
      if (hex) copyValue(firmwareHash, 65, s, 64);
    }
    s = end ? end + 1 : nullptr;
  }
  return versionNumber(version) > 0 && strlen(firmwareHash) == 64;
}

// Format: one "key value" per line; see vps/peace_bridge.py. False if this is not our file.
inline bool parseFeed(const char* s, Feed& out) {
  out = Feed{};
  bool hasLevel = false;
  Level summary = L_NONE;
  char zone[ZONE_NAME] = "", threat[32] = "";
  while (s && *s) {
    const char* end = strchr(s, '\n');
    size_t len = end ? (size_t)(end - s) : strlen(s);
    if (len && s[len - 1] == '\r') len--;
    const char* space = (const char*)memchr(s, ' ', len);
    if (space) {
      size_t keyLen = space - s, valLen = len - keyLen - 1;
      const char* val = space + 1;
      if (keyLen == 1 && *s == 'z') {
        if (out.count < MAX_ZONES) parseZoneLine(val, val + valLen, out.zones[out.count++]);
        // Zones beyond the limit must not hide an alert: the bridge sorts the worst first.
      } else if (keyLen == 1 && *s == 'n') parseZoneInfo(val, val + valLen, out.list);
      else if (keyLen == 3 && !strncmp(s, "obl", 3)) copyValue(out.obl, sizeof out.obl, val, valLen);
      else if (keyLen == 2 && !strncmp(s, "ts", 2)) out.ts = strtoll(val, nullptr, 10);
      else if (keyLen == 3 && !strncmp(s, "seq", 3)) out.seq = strtol(val, nullptr, 10);
      else if (keyLen == 3 && !strncmp(s, "msg", 3)) out.msgTs = strtoll(val, nullptr, 10);
      else if (keyLen == 3 && !strncmp(s, "bot", 3)) out.botOk = *val == '1';
      else if (keyLen == 5 && !strncmp(s, "level", 5)) { summary = parseLevel(val, valLen); hasLevel = true; }
      else if (keyLen == 4 && !strncmp(s, "zone", 4)) copyValue(zone, sizeof zone, val, valLen);
      else if (keyLen == 6 && !strncmp(s, "threat", 6)) copyValue(threat, sizeof threat, val, valLen);
    }
    s = end ? end + 1 : nullptr;
  }
  // A summary that reports danger with no zone lines (an older bridge) still has to ring.
  if (!out.count && summary != L_NONE) {
    ZoneAlert& z = out.zones[out.count++];
    strcpy(z.name, zone);
    strcpy(z.threats, threat);
    z.level = summary;
    z.seq = out.seq;
  }
  return out.ts > 0 && hasLevel;
}

// True if the direction names a zone the user watches, as a whole word: "Kyiv" is not in "Kyivska oblast".
inline bool headsToWatched(const char* direction, const ZoneList& l, unsigned mask) {
  for (int i = 0; i < l.count; i++) {
    if (!(mask & (1u << i))) continue;
    const char* name = l.zones[i].name;
    size_t n = strlen(name);
    for (const char* at = strstr(direction, name); at; at = strstr(at + 1, name)) {
      bool startOk = at == direction || !isalnum((unsigned char)at[-1]);
      if (startOk && !isalnum((unsigned char)at[n])) return true;
    }
  }
  return false;
}

// Watched, or heading for what is watched.
inline bool zoneMatters(const ZoneAlert& z, const ZoneList& l, unsigned mask) {
  return zoneWanted(l, zoneIndex(l, z.name), mask) || headsToWatched(z.direction, l, mask);
}

// What the zones the user watches add up to.
struct View {
  Level level;
  bool ballistic;
  long seq;
  long long since;
  int count;
  char zone[44];
  char threats[48];
};

inline void addThreats(char* to, size_t size, const char* list) {
  while (*list) {
    const char* comma = strchr(list, ',');
    size_t n = comma ? (size_t)(comma - list) : strlen(list);
    char one[32];
    copyValue(one, sizeof one, list, n);
    if (!strstr(to, one) && strlen(to) + n + 3 < size) {
      if (*to) strcat(to, ", ");
      strcat(to, one);
    }
    list += n + (comma ? 1 : 0);
  }
}

inline View computeView(const Feed& f, unsigned mask) {
  View v{};
  const ZoneAlert* top = nullptr;
  for (int i = 0; i < f.count; i++) {
    const ZoneAlert& z = f.zones[i];
    if (z.level == L_NONE || !zoneMatters(z, f.list, mask)) continue;
    bool watched = zoneWanted(f.list, zoneIndex(f.list, z.name), mask);
    v.count++;
    if (!top || z.level > top->level) top = &z;
    if (z.level > v.level) v.level = z.level;
    if (strstr(z.threats, "BALLISTIC")) v.ballistic = true;
    if (z.level != L_LOST) {  // something that may be over must not start a new ring
      long seq = !watched && z.dirSeq > z.seq ? z.dirSeq : z.seq;
      if (seq > v.seq) v.seq = seq;
    }
    if (z.since && (!v.since || z.since < v.since)) v.since = z.since;
    addThreats(v.threats, sizeof v.threats, z.threats);
  }
  if (top) {
    if (!zoneWanted(f.list, zoneIndex(f.list, top->name), mask)) snprintf(v.zone, sizeof v.zone, "%.15s to %.20s", top->name, top->direction);
    else if (v.count > 1) snprintf(v.zone, sizeof v.zone, "%.24s +%d", top->name, v.count - 1);
    else snprintf(v.zone, sizeof v.zone, "%.31s", top->name);
  }
  return v;
}

// Says what is known: the API names the threat only when its source gave a reason.
inline const char* viewTitle(const View& v) {
  if (v.level == L_LOST) return "UNCONFIRMED";
  if (v.level == L_UNKNOWN) return "CHECK ALERTS";
  if (v.ballistic) return "BALLISTIC";
  if (strstr(v.threats, "MISSILES") || strstr(v.threats, "CRUISE")) return "MISSILE ALERT";
  if (strstr(v.threats, "AIRCRAFT")) return "AIRCRAFT ALERT";
  if (v.level == L_RED) return "RED ALERT";
  if (strstr(v.threats, "DRONES")) return "DRONE ALERT";
  return v.level == L_YELLOW ? "YELLOW ALERT" : "AIR ALERT";
}

// ---------- history and statistics ----------

struct HistoryEvent {
  long long ts;
  char zone[ZONE_NAME];
  Level level;
  char threats[24];
};

struct ZoneStats {
  char zone[ZONE_NAME];
  int todayCount, todayMin, weekCount, weekMin;
};

struct History {
  char obl[REGION_ID];
  int eventCount, statCount;
  HistoryEvent events[MAX_EVENTS];
  ZoneStats stats[MAX_ZONES];
};

// Keeps only the zones the user watches, so a busy neighbour does not use up the screen.
// The file lists its zones first, so the choice is read against the same list.
inline bool parseHistory(const char* s, unsigned mask, History& out) {
  out = History{};
  ZoneList list{};
  bool ours = false;
  while (s && *s) {
    const char* end = strchr(s, '\n');
    const char* stop = end ? end : s + strlen(s);
    const char* val = s + 2;
    const char* f;
    size_t n;
    if (!strncmp(s, "ts ", 3)) {
      ours = true;
    } else if (!strncmp(s, "n ", 2) && val < stop) {
      parseZoneInfo(val, stop, list);
    } else if (!strncmp(s, "obl ", 4) && s + 4 < stop) {
      copyValue(out.obl, sizeof out.obl, s + 4, stop - s - 4);
    } else if (!strncmp(s, "h ", 2) && val < stop) {
      HistoryEvent e{};
      nextField(val, stop, f, n);
      e.ts = strtoll(f, nullptr, 10);
      nextField(val, stop, f, n);
      copyValue(e.zone, sizeof e.zone, f, n);
      nextField(val, stop, f, n);
      e.level = parseLevel(f, n);
      nextField(val, stop, f, n);
      copyValue(e.threats, sizeof e.threats, f, n);
      if (zoneWanted(list, zoneIndex(list, e.zone), mask) && out.eventCount < MAX_EVENTS) out.events[out.eventCount++] = e;
    } else if (!strncmp(s, "s ", 2) && val < stop) {
      ZoneStats z{};
      nextField(val, stop, f, n);
      copyValue(z.zone, sizeof z.zone, f, n);
      nextField(val, stop, f, n);
      z.todayCount = atoi(f);
      nextField(val, stop, f, n);
      z.todayMin = atoi(f);
      nextField(val, stop, f, n);
      z.weekCount = atoi(f);
      nextField(val, stop, f, n);
      z.weekMin = atoi(f);
      if (zoneWanted(list, zoneIndex(list, z.zone), mask) && out.statCount < MAX_ZONES) out.stats[out.statCount++] = z;
    }
    s = end ? end + 1 : nullptr;
  }
  return ours;
}

// ---------- oblasts ----------

const int MAX_REGIONS = 32;
struct Region {
  char id[REGION_ID];
  char name[ZONE_NAME];
  char ua[40];  // Ukrainian, when the bridge sends it
};
struct RegionList {
  int count;
  Region regions[MAX_REGIONS];
};

// Format: "r id|name|Ukrainian name" per line, in the order to show them.
inline bool parseRegions(const char* s, RegionList& out) {
  out = RegionList{};
  while (s && *s) {
    const char* end = strchr(s, '\n');
    const char* stop = end ? end : s + strlen(s);
    if (!strncmp(s, "r ", 2) && out.count < MAX_REGIONS) {
      const char* val = s + 2;
      const char* f;
      size_t n;
      Region& r = out.regions[out.count++];
      nextField(val, stop, f, n);
      copyValue(r.id, sizeof r.id, f, n);
      nextField(val, stop, f, n);
      copyValue(r.name, sizeof r.name, f, n);
      nextField(val, stop, f, n);
      copyValue(r.ua, sizeof r.ua, f, n);
    }
    s = end ? end + 1 : nullptr;
  }
  return out.count > 0;
}

// ---------- link and sound ----------

inline Link classifyLink(bool wifi, bool timeSynced, unsigned long msSinceGood, bool hasData, const Feed& f, long long now) {
  if (msSinceGood > LINK_GRACE_MS) return wifi ? LINK_NOANSWER : LINK_NOWIFI;
  if (!hasData) return LINK_WAIT;
  if (timeSynced && now - f.ts > STALE_AFTER_S) return LINK_STALE;
  return f.botOk ? LINK_OK : LINK_BOT;
}

inline bool linkIsBad(Link l) { return l != LINK_OK && l != LINK_WAIT; }

struct Ringer {
  long ackedSeq;     // the newest escalation the user silenced
  bool linkAcked;    // the outage the user silenced
};

// At night missiles still ring in full; drones and a lost link get one short beep.
inline Sound wantedSound(const Ringer& r, bool hasData, const View& v, Link link, bool night) {
  if (hasData && v.level != L_NONE && v.level != L_LOST && v.seq > r.ackedSeq) {
    if (v.ballistic) return S_BALLISTIC;
    if (v.level == L_RED || v.level == L_UNKNOWN) return S_SIREN;
    return night ? S_SOFT : S_BEEPS;
  }
  if (linkIsBad(link) && !r.linkAcked) return night ? S_SOFT : S_LINK;
  return S_NONE;
}

inline void ackAlert(Ringer& r, const View& v) {
  if (v.seq > r.ackedSeq) r.ackedSeq = v.seq;
}

inline void ackAll(Ringer& r, const View& v) {
  ackAlert(r, v);
  r.linkAcked = true;
}

// A new outage rings again once the link came back. A bridge that lost its state starts its seq over.
inline void updateRinger(Ringer& r, Link link, long feedSeq) {
  if (!linkIsBad(link)) r.linkAcked = false;
  if (feedSeq < r.ackedSeq) r.ackedSeq = NEVER_ACKED;
}

// L_LOST does not count: nobody confirmed that it ended, so no all-clear.
inline bool becameClear(Level before, Level now) { return before != L_NONE && before != L_LOST && now == L_NONE; }

// A short beep now and then, so a silenced alert is not forgotten. Not at night: silencing it there means sleep.
inline bool remindDue(Level level, bool night, unsigned long sinceRing) {
  return !night && level != L_NONE && level != L_LOST && sinceRing >= REMIND_EVERY_MS;
}

// Screen flash and LED during an alarm, from the same clock so they stay in step.
inline bool flashPhase(unsigned long ms, unsigned long periodMs) { return ms % (2 * periodMs) < periodMs; }

// ---------- knocks ----------
// From the accelerometer's total force in g: a free fall reads near 0, a hit or a sharp swing well over 1.

enum Knock { K_NONE, K_SWING, K_STRIKE, K_DROP };  // in order of how hard
struct KnockSense {
  unsigned long fallSince, fellAt, lastAt;
  bool fell;
};
const float FREEFALL_G = 0.35f, LANDING_G = 2.0f, SWING_G = 2.2f, STRIKE_LOW_G = 4.0f, STRIKE_HIGH_G = 3.5f;
const unsigned long FREEFALL_MS = 80, LAND_WITHIN_MS = 1000, KNOCK_GAP_MS = 300;

// level: 0 off, 1 only hits and drops, 2 also sharp swings.
inline Knock senseKnock(KnockSense& s, float g, unsigned long t, int level) {
  if (!level) return K_NONE;
  if (g < FREEFALL_G) {
    if (!s.fallSince) s.fallSince = t;
    if (t - s.fallSince >= FREEFALL_MS) s.fell = true, s.fellAt = t;
    return K_NONE;
  }
  s.fallSince = 0;
  if (s.fell && t - s.fellAt > LAND_WITHIN_MS) s.fell = false;  // caught in the air: no landing
  Knock k = K_NONE;
  if (s.fell && g >= LANDING_G) k = K_DROP;
  else if (g >= (level == 2 ? STRIKE_HIGH_G : STRIKE_LOW_G)) k = K_STRIKE;
  else if (level == 2 && g >= SWING_G) k = K_SWING;
  if (k == K_NONE) return K_NONE;
  if (k != K_DROP && s.lastAt && t - s.lastAt < KNOCK_GAP_MS) return K_NONE;  // one knock lasts several samples
  s.fell = false;
  s.lastAt = t;
  return k;
}

// The picture hangs on a damped spring: a push moves it, then it swings back and settles.
struct Spring {
  float x, v;  // offset in pixels, and its speed in pixels per second
  void step(float dt, float limit) {
    const float STIFF = 360, DAMP = 9;  // about 3 swings a second, quiet within a second or so
    v += (-STIFF * x - DAMP * v) * dt;
    x += v * dt;
    if (x > limit) x = limit, v = 0;
    if (x < -limit) x = -limit, v = 0;
  }
  bool moving() const { return x > 0.5f || x < -0.5f || v > 5 || v < -5; }
};

enum Side { SIDE_LEFT, SIDE_RIGHT, SIDE_TOP, SIDE_BOTTOM };
// A hit from the left pushes the stick to the right: the side it came from is opposite the push.
inline Side hitSide(float right, float down) {
  if ((right < 0 ? -right : right) >= (down < 0 ? -down : down)) return right > 0 ? SIDE_LEFT : SIDE_RIGHT;
  return down > 0 ? SIDE_TOP : SIDE_BOTTOM;
}

// ---------- small helpers ----------

inline bool isNightHour(int hour, int start, int end) {
  if (start == end) return false;
  return start > end ? hour >= start || hour < end : hour >= start && hour < end;
}

// A full battery reads the same as USB for a few minutes after unplugging, which costs nothing but some saver time.
inline bool updateUsb(bool wasUsb, int batteryMv) {
  return wasUsb ? batteryMv >= USB_OFF_MV : batteryMv >= USB_ON_MV;
}

inline void formatTimer(long long seconds, char* out, size_t size) {
  if (seconds < 0) seconds = 0;
  snprintf(out, size, "%02lld:%02lld:%02lld", seconds / 3600, seconds / 60 % 60, seconds % 60);
}

// Back and forth through a list, wrapping around.
inline int stepIndex(int index, int count, int step) { return count ? (index + step + count) % count : 0; }
