// Run: g++ -std=c++17 -I src test/test_alert.cpp -o /tmp/t && /tmp/t
#include <cassert>
#include <cstdio>
#include <string>

#include "alert.h"

const Ringer FRESH = {NEVER_ACKED, false};
const char* SAMPLE =
    "ts 2000000000\nseq 7\nlevel red\nbot 1\nzone Kyiv +2\nthreat BALLISTIC, DRONES\n"
    "z Kyiv|red|7|1999990000|90|BALLISTIC,DRONES|Kyiv\n"
    "z Buchanskyi|yellow|5|1999980000|45|DRONES|\n"
    "z Fastivskyi|alert|3|1999995000|50||\n";

// Kyiv's zones as the bridge lists them, in its order.
const std::string KYIV_LIST =
    "obl 14\nn Kyiv|KYIV|c\nn Bilotserkivskyi|BILOTSERK|r\nn Boryspilskyi|BORYSPIL|r\nn Brovarskyi|BROVAR|r\n"
    "n Buchanskyi|BUCHAN|r\nn Fastivskyi|FASTIV|r\nn Obukhivskyi|OBUKHIV|r\nn Vyshhorodskyi|VYSHHOROD|r\n"
    "n Kyivska oblast|OBLAST|o\n";
const int ZONE_KYIV = 0, ZONE_OBLAST = 8;
const unsigned KYIV_ONLY = 1u << ZONE_KYIV;
static ZoneList K;

bool parse(const char* body, Feed& f) { return parseFeed((KYIV_LIST + body).c_str(), f); }
bool parseHist(const char* body, unsigned mask, History& h) { return parseHistory((KYIV_LIST + body).c_str(), mask, h); }

View viewOf(Level level, long seq) {
  View v{};
  v.level = level;
  v.seq = seq;
  return v;
}

int main() {
  static Feed f;
  assert(parse("ts 1\nlevel none\n", f) && f.list.count == 9 && !strcmp(f.obl, "14"));
  K = f.list;
  assert(!strcmp(K.zones[ZONE_OBLAST].name, "Kyivska oblast") && K.zones[ZONE_OBLAST].kind == 'o' && !strcmp(K.zones[4].label, "BUCHAN"));
  assert(watchedCount(K, KYIV_ONLY | (1u << 4)) == 2 && watchedCount(K, DEFAULT_ZONES) == 9);
  assert(parse(SAMPLE, f));
  assert(f.ts == 2000000000LL && f.seq == 7 && f.botOk && f.count == 3);
  const ZoneAlert& k = f.zones[0];
  assert(!strcmp(k.name, "Kyiv") && k.level == L_RED && k.seq == 7 && k.since == 1999990000LL && k.conf == 90);
  assert(!strcmp(k.threats, "BALLISTIC,DRONES") && !strcmp(k.direction, "Kyiv"));
  assert(f.zones[2].level == L_ALERT && f.zones[2].threats[0] == 0 && f.zones[2].direction[0] == 0);

  // Quiet state, as the bridge writes it
  assert(parse("ts 5\nseq 0\nlevel none\nbot 1\nzone \nthreat \n", f));
  assert(f.count == 0);

  // Not our file (an HTML error page, an empty answer, no level)
  assert(!parse("<html>level red</html>", f));
  assert(!parse("", f));
  assert(!parse("ts 5\nseq 1\n", f));

  // An unknown level is loud, and a long value is cut, never overflows
  assert(parse("ts 1\nlevel none\nz a-name-much-longer-than-the-twenty-four-byte-field|purple|1|1|1|x|y\n", f));
  assert(f.zones[0].level == L_UNKNOWN && strlen(f.zones[0].name) == ZONE_NAME - 1);
  assert(parse("ts 1\r\nlevel none\r\nz Kyiv|yellow|1|2|3|DRONES|Kyiv\r\n", f));
  assert(f.zones[0].level == L_YELLOW && !strcmp(f.zones[0].direction, "Kyiv"));

  // More zones than fit: no overflow, and the worst stay because the bridge sorts them first
  std::string many = "ts 1\nlevel red\n";
  for (int i = 0; i < MAX_ZONES + 5; i++) many += "z Z" + std::to_string(i) + "|red|1|1|1||\n";
  assert(parse(many.c_str(), f) && f.count == MAX_ZONES);

  // An older bridge with no zone lines still rings, whatever is selected
  assert(parse("ts 1\nseq 4\nlevel red\nzone Kyiv +1\nthreat DRONES\n", f));
  View old = computeView(f, KYIV_ONLY);
  assert(f.count == 1 && old.level == L_RED && old.seq == 4 && !strcmp(old.threats, "DRONES"));

  // A file with no zone list: nothing is known, so everything counts
  assert(parseFeed("ts 1\nlevel red\nz Buchanskyi|red|1|1|1||\n", f) && f.list.count == 0 && computeView(f, KYIV_ONLY).level == L_RED);

  // Choosing zones
  assert(zoneIndex(K, "Kyiv") == ZONE_KYIV && zoneIndex(K, "Kyivska oblast") == ZONE_OBLAST && zoneIndex(K, "Mars") == -1);
  assert(zoneWanted(K, ZONE_KYIV, KYIV_ONLY) && !zoneWanted(K, zoneIndex(K, "Buchanskyi"), KYIV_ONLY));
  assert(zoneWanted(K, -1, 0));                                   // unknown zones always count
  assert(!zoneWanted(K, ZONE_OBLAST, KYIV_ONLY));             // Kyiv is not part of the oblast's raions
  unsigned bucha = 1u << zoneIndex(K, "Buchanskyi");
  assert(zoneWanted(K, ZONE_OBLAST, bucha) && !zoneWanted(K, ZONE_KYIV, bucha));
  assert(zoneWanted(K, ZONE_OBLAST, 1u << ZONE_OBLAST) && !zoneWanted(K, zoneIndex(K, "Buchanskyi"), 1u << ZONE_OBLAST));

  // The view only adds up what is watched
  parse(SAMPLE, f);
  View v = computeView(f, KYIV_ONLY);
  assert(v.level == L_RED && v.seq == 7 && v.count == 1 && !strcmp(v.zone, "Kyiv") && v.since == 1999990000LL);
  assert(!strcmp(v.threats, "BALLISTIC, DRONES"));
  v = computeView(f, bucha);
  assert(v.level == L_YELLOW && v.seq == 5 && !strcmp(v.zone, "Buchanskyi") && !strcmp(v.threats, "DRONES"));
  v = computeView(f, KYIV_ONLY | bucha);
  assert(v.level == L_RED && v.count == 2 && !strcmp(v.zone, "Kyiv +1") && v.since == 1999980000LL);
  v = computeView(f, 1u << zoneIndex(K, "Brovarskyi"));
  assert(v.level == L_NONE && v.count == 0 && v.zone[0] == 0);

  // Heading for a watched zone counts, with its direction's seq; the zone's own watchers go by its own seq
  assert(parse("ts 1\nlevel yellow\nz Buchanskyi|yellow|5|100|45|DRONES|Kyiv|9\n", f));
  v = computeView(f, KYIV_ONLY);
  assert(v.level == L_YELLOW && v.seq == 9 && !strcmp(v.zone, "Buchanskyi to Kyiv"));
  v = computeView(f, bucha);
  assert(v.seq == 5 && !strcmp(v.zone, "Buchanskyi"));
  v = computeView(f, 1u << zoneIndex(K, "Brovarskyi"));
  assert(v.level == L_NONE);
  assert(headsToWatched("Kyiv, Buchanskyi r-n", K, bucha) && headsToWatched("Kyiv", K, KYIV_ONLY));
  assert(!headsToWatched("Kyivska oblast", K, KYIV_ONLY) && !headsToWatched("", K, KYIV_ONLY));
  assert(headsToWatched("Kyivska oblast", K, 1u << ZONE_OBLAST));
  assert(parse("ts 1\nlevel yellow\nz Buchanskyi|yellow|5|100|45|DRONES|Kyiv\n", f));  // no direction seq yet
  assert(computeView(f, KYIV_ONLY).seq == 5);

  // Ballistic has its own alarm, whichever zone brings it, also at night
  assert(parse("ts 1\nlevel red\nz Kyiv|red|4|1|90|CRUISE,BALLISTIC|\n", f));
  v = computeView(f, KYIV_ONLY);
  assert(v.ballistic && wantedSound(FRESH, true, v, LINK_OK, false) == S_BALLISTIC);
  assert(wantedSound(FRESH, true, v, LINK_OK, true) == S_BALLISTIC);
  assert(parse("ts 1\nlevel red\nz Kyiv|red|4|1|90|CRUISE|\n", f) && !computeView(f, KYIV_ONLY).ballistic);

  // An alert that may be over: shown, silent, no new ring, and no all-clear when it ends
  assert(parse("ts 1\nlevel lost\nz Kyiv|lost|8|1|90|DRONES|\n", f));
  v = computeView(f, KYIV_ONLY);
  assert(v.level == L_LOST && v.seq == 0 && !strcmp(v.zone, "Kyiv"));
  assert(wantedSound(FRESH, true, v, LINK_OK, false) == S_NONE);
  assert(!becameClear(L_LOST, L_NONE) && !remindDue(L_LOST, false, REMIND_EVERY_MS));
  // ...but it does not hide a real alert next to it
  assert(parse("ts 1\nlevel red\nz Kyiv|red|3|1|90|DRONES|\nz Fastivskyi|lost|8|1|90||\n", f));
  v = computeView(f, KYIV_ONLY | (1u << zoneIndex(K, "Fastivskyi")));
  assert(v.level == L_RED && v.seq == 3);

  // Reminders of a silenced alert, not at night
  assert(remindDue(L_YELLOW, false, REMIND_EVERY_MS) && !remindDue(L_YELLOW, false, REMIND_EVERY_MS - 1));
  assert(!remindDue(L_RED, true, REMIND_EVERY_MS) && !remindDue(L_NONE, false, REMIND_EVERY_MS));

  // The delay is measured from the time the bridge reports
  assert(parse("ts 1\nseq 2\nmsg 1999\nlevel none\n", f) && f.msgTs == 1999);

  // The title says only what is known
  View t{};
  t.level = L_RED;
  assert(!strcmp(viewTitle(t), "RED ALERT"));
  t.level = L_YELLOW;
  assert(!strcmp(viewTitle(t), "YELLOW ALERT"));
  t.level = L_ALERT;
  assert(!strcmp(viewTitle(t), "AIR ALERT"));
  t.level = L_UNKNOWN;
  assert(!strcmp(viewTitle(t), "CHECK ALERTS"));
  t.level = L_YELLOW;
  strcpy(t.threats, "DRONES");
  assert(!strcmp(viewTitle(t), "DRONE ALERT"));
  t.level = L_RED;
  assert(!strcmp(viewTitle(t), "RED ALERT"));              // red with only drones named: do not claim missiles
  strcpy(t.threats, "MISSILES, DRONES");
  assert(!strcmp(viewTitle(t), "MISSILE ALERT"));
  t.ballistic = true;
  assert(!strcmp(viewTitle(t), "BALLISTIC"));

  // Threats merge without repeats, and never overflow
  char merged[16] = "";
  addThreats(merged, sizeof merged, "DRONES,CRUISE");
  addThreats(merged, sizeof merged, "DRONES,BALLISTIC");
  assert(!strcmp(merged, "DRONES, CRUISE"));

  // History and statistics keep only the watched zones
  static History h;
  const char* HIST =
      "ts 100\nh 90|Kyiv|red|BALLISTIC,DRONES\nh 80|Buchanskyi|yellow|DRONES\nh 70|Kyiv|none|\n"
      "s Buchanskyi|1|5|2|50\ns Kyiv|2|95|9|600\n";
  assert(parseHist(HIST, KYIV_ONLY, h));
  assert(h.eventCount == 2 && h.events[0].ts == 90 && h.events[0].level == L_RED && h.events[1].level == L_NONE);
  assert(h.statCount == 1 && !strcmp(h.stats[0].zone, "Kyiv") && h.stats[0].todayCount == 2);
  assert(h.stats[0].todayMin == 95 && h.stats[0].weekCount == 9 && h.stats[0].weekMin == 600);
  assert(parseHist(HIST, KYIV_ONLY | bucha, h) && h.eventCount == 3 && h.statCount == 2);
  assert(!parseHist("<html>", KYIV_ONLY, h));
  std::string longHist = "ts 1\n";
  for (int i = 0; i < MAX_EVENTS + 9; i++) longHist += "h 5|Kyiv|red|\n";
  assert(parseHist(longHist.c_str(), KYIV_ONLY, h) && h.eventCount == MAX_EVENTS);

  // The list of oblasts
  static RegionList rl;
  assert(parseRegions("r 24|Cherkaska\nr 14|Kyivska\nr 9999|Krym\n", rl) && rl.count == 3);
  assert(!strcmp(rl.regions[1].id, "14") && !strcmp(rl.regions[1].name, "Kyivska"));
  assert(!parseRegions("<html>", rl) && !parseRegions("", rl));
  assert(parseHist("ts 1\n", KYIV_ONLY, h) && !strcmp(h.obl, "14"));

  // Link states
  static Feed good;
  good = Feed{};
  good.ts = 1000;
  good.botOk = true;
  assert(classifyLink(false, false, 1000, false, good, 0) == LINK_WAIT);       // just booted
  assert(classifyLink(false, false, LINK_GRACE_MS + 1, false, good, 0) == LINK_NOWIFI);
  assert(classifyLink(true, true, LINK_GRACE_MS + 1, true, good, 1000) == LINK_NOANSWER);
  assert(classifyLink(false, true, 5000, true, good, 1000) == LINK_OK);        // a blip stays calm
  assert(classifyLink(true, true, 1000, true, good, 1000 + STALE_AFTER_S) == LINK_OK);
  assert(classifyLink(true, true, 1000, true, good, 1001 + STALE_AFTER_S) == LINK_STALE);
  assert(classifyLink(true, false, 1000, true, good, 5) == LINK_OK);           // no clock yet: cannot call it stale
  good.botOk = false;
  assert(classifyLink(true, true, 1000, true, good, 1000) == LINK_BOT);

  // Each level gets its sound; silence lasts until the next escalation
  Ringer r = FRESH;
  assert(wantedSound(r, false, viewOf(L_RED, 3), LINK_WAIT, false) == S_NONE);  // nothing known yet
  assert(wantedSound(r, true, viewOf(L_RED, 3), LINK_OK, false) == S_SIREN);
  ackAll(r, viewOf(L_RED, 3));
  assert(wantedSound(r, true, viewOf(L_RED, 3), LINK_OK, false) == S_NONE);
  assert(wantedSound(r, true, viewOf(L_RED, 4), LINK_OK, false) == S_SIREN);
  assert(wantedSound(r, true, viewOf(L_YELLOW, 4), LINK_OK, false) == S_BEEPS);
  assert(wantedSound(r, true, viewOf(L_ALERT, 4), LINK_OK, false) == S_BEEPS);
  assert(wantedSound(r, true, viewOf(L_UNKNOWN, 4), LINK_OK, false) == S_SIREN);
  assert(wantedSound(r, true, viewOf(L_NONE, 4), LINK_OK, false) == S_NONE);

  // A cleared zone lowers the watched seq: the remaining zone, already silenced, must not ring again
  assert(wantedSound(r, true, viewOf(L_YELLOW, 2), LINK_OK, false) == S_NONE);

  // Night: missiles ring in full, drones and a lost link beep once
  r = FRESH;
  assert(wantedSound(r, true, viewOf(L_RED, 1), LINK_OK, true) == S_SIREN);
  assert(wantedSound(r, true, viewOf(L_UNKNOWN, 1), LINK_OK, true) == S_SIREN);
  assert(wantedSound(r, true, viewOf(L_YELLOW, 1), LINK_OK, true) == S_SOFT);
  assert(wantedSound(r, true, viewOf(L_ALERT, 1), LINK_OK, true) == S_SOFT);
  assert(wantedSound(r, true, viewOf(L_NONE, 0), LINK_NOWIFI, true) == S_SOFT);

  // An outage rings once until silenced, ringing again only after the link came back; an alert goes first
  r = FRESH;
  assert(wantedSound(r, true, viewOf(L_NONE, 0), LINK_NOANSWER, false) == S_LINK);
  assert(wantedSound(r, true, viewOf(L_RED, 9), LINK_NOANSWER, false) == S_SIREN);
  ackAll(r, viewOf(L_RED, 9));
  assert(wantedSound(r, true, viewOf(L_RED, 9), LINK_NOANSWER, false) == S_NONE);
  updateRinger(r, LINK_STALE, 9);
  assert(wantedSound(r, true, viewOf(L_RED, 9), LINK_STALE, false) == S_NONE);
  updateRinger(r, LINK_OK, 9);
  assert(wantedSound(r, true, viewOf(L_RED, 9), LINK_STALE, false) == S_LINK);

  // Auto-ending the beeps must not silence a later outage
  r = FRESH;
  ackAlert(r, viewOf(L_YELLOW, 2));
  assert(wantedSound(r, true, viewOf(L_YELLOW, 2), LINK_NOWIFI, false) == S_LINK);

  // A bridge that lost its state counts from zero again, and its alerts still ring
  r = FRESH;
  ackAll(r, viewOf(L_RED, 50));
  updateRinger(r, LINK_OK, 1);
  assert(wantedSound(r, true, viewOf(L_RED, 1), LINK_OK, false) == S_SIREN);

  assert(becameClear(L_RED, L_NONE) && becameClear(L_UNKNOWN, L_NONE));
  assert(!becameClear(L_NONE, L_NONE) && !becameClear(L_RED, L_YELLOW));
  assert(flashPhase(0, 150) && flashPhase(149, 150) && !flashPhase(150, 150) && flashPhase(300, 150));

  // Night hours
  assert(isNightHour(23, 23, 7) && isNightHour(0, 23, 7) && isNightHour(6, 23, 7));
  assert(!isNightHour(7, 23, 7) && !isNightHour(22, 23, 7));
  assert(isNightHour(2, 1, 6) && !isNightHour(6, 1, 6) && !isNightHour(0, 1, 6));
  assert(!isNightHour(3, 5, 5));

  // USB from voltage, with a gap so it does not flicker
  assert(!updateUsb(false, 4000) && !updateUsb(false, USB_ON_MV - 1) && updateUsb(false, USB_ON_MV));
  assert(updateUsb(true, USB_OFF_MV) && !updateUsb(true, USB_OFF_MV - 1));

  // Formats
  char text[16];
  formatTimer(3725, text, sizeof text);
  assert(!strcmp(text, "01:02:05"));
  formatTimer(-5, text, sizeof text);
  assert(!strcmp(text, "00:00:00"));
  formatTimer(360000, text, sizeof text);
  assert(!strcmp(text, "100:00:00"));
  formatMinutes(45, text, sizeof text);
  assert(!strcmp(text, "45m"));
  formatMinutes(95, text, sizeof text);
  assert(!strcmp(text, "1h 35m"));

  assert(stepIndex(0, 6, -1) == 5 && stepIndex(5, 6, 1) == 0 && stepIndex(2, 6, 1) == 3 && stepIndex(0, 0, 1) == 0);

  puts("alert tests passed");
}
