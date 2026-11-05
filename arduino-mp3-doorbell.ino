/*
  MP3 Doorbell - Arduino Nano + YX5300 (MD_YX5300) + ezButton
  Cooperative multitasking with Protothreads (pt.h)

  Library needed (Library Manager): "Protothreads" (provides <pt.h>)
  Also: ezButton, MD_YX5300

  Wiring:
    Button   -> D2 and GND (internal pull-up)
    YX5300   -> RX_PIN / TX_PIN below (SoftwareSerial)
*/

#include <ezButton.h>
#include <SoftwareSerial.h>
#include <MD_YX5300.h>
#include <pt.h>

// ===================== SETTINGS (edit these) =====================

// Track numbers (1-255, file order on the SD card)
const uint8_t TRACK_ONE_PRESS                   = 1;  // first press / pressed after a long quiet time
const uint8_t TRACK_PRESSED_FOR_5_SEC           = 2;  // button held down for 5 s
const uint8_t TRACK_PRESSED_AGAIN_WITHIN_10_SEC = 3;  // pressed again within 10 s of first press
const uint8_t TRACK_PRESSED_AGAIN_WITHIN_30_SEC = 4;  // pressed again within 30 s of first press
const uint8_t TRACK_PRESSED_AGAIN_WITHIN_60_SEC = 5;  // pressed again within 60 s of first press
const uint8_t TRACK_PRESSED_AGAIN_WITHIN_5_MIN  = 6;  // pressed again within 5 min of first press
const uint8_t TRACK_PRESSED_AGAIN_WITHIN_10_MIN = 7;  // pressed again within 10 min of first press

// Timing (milliseconds)
const unsigned long TRACK_DURATION_TO_STOP_AFTER_RELEASED   = 4000; // hold track (repeat): keep playing this long after release, then stop (0 = stop at once)
const unsigned long PING_DELAY_STOP_TRACK_WHEN_NOT_PLAYING  = 3000; // while idle, send "stop" this often

// Playback: normal tracks play once, all the way to the end (release never stops them).
// The 5 s HOLD track plays on repeat until the button is released
// (+ TRACK_DURATION_TO_STOP_AFTER_RELEASED), and a hold always starts it.
// What should a new normal press do while a track is still playing?
//   false = let the current track finish, ignore the new track request
//   true  = cut the current track and start the newly chosen one
const bool INTERRUPT_PLAYING_ON_NEW_PRESS = true;

// Other
const uint8_t       VOLUME            = 20;    // 0 - 30
const unsigned long HOLD_TIME_MS      = 5000;  // how long counts as "pressed for 5 sec"
const unsigned long WINDOW_10_SEC_MS  = 10000;
const unsigned long WINDOW_30_SEC_MS  = 30000;
const unsigned long WINDOW_60_SEC_MS  = 60000UL;
const unsigned long WINDOW_5_MIN_MS   = 300000UL;
const unsigned long WINDOW_10_MIN_MS  = 600000UL;
const unsigned long COUNTDOWN_PRINT_INTERVAL_MS = 10000UL; // print time left every 10 s after first press

// ===================== PINS / OBJECTS =====================

const int buttonPin = 2;   // Push button on D2
const int ledPin    = 13;  // Onboard LED

#define RX_PIN 10
#define TX_PIN 11

ezButton button(buttonPin);
SoftwareSerial mp3Serial(RX_PIN, TX_PIN);
MD_YX5300 mp3(mp3Serial);

// ===================== SHARED STATE =====================

static struct pt ptMp3, ptButton, ptPlayer, ptPing, ptCountdown;

bool          playing         = false;  // a track is currently playing
bool          playRequest     = false;  // button thread asks player thread to (re)start a track
uint8_t       requestedTrack  = 0;      // track to play for the request
bool          requestedRepeat = false;  // true = play the requested track on repeat (hold)
bool          repeating       = false;  // the track now playing is on repeat
bool          buttonHeld      = false;  // button currently down
unsigned long releasedAt      = 0;      // millis() when button was released
unsigned long holdStart       = 0;      // millis() when button was pressed
unsigned long firstPressAt    = 0;      // millis() of the FIRST press of the current sequence
bool          hasFirstPress   = false;  // a sequence has been started
unsigned long lastPingAt      = 0;
unsigned long cdStart         = 0;      // countdown thread: sequence start it is tracking
unsigned long cdTicks         = 0;      // countdown thread: number of prints done

// ===================== HELPERS =====================

// Print milliseconds as m:ss
void printMMSS(unsigned long ms) {
  unsigned long totalSec = ms / 1000UL;
  Serial.print(totalSec / 60UL);
  Serial.print(':');
  unsigned long sec = totalSec % 60UL;
  if (sec < 10) Serial.print('0');
  Serial.print(sec);
}

// Print " | held for m:ss | since first press m:ss"
void printTimeInfo(unsigned long heldMs) {
  Serial.print(F(" | held for "));
  printMMSS(heldMs);
  Serial.print(F(" | since first press "));
  printMMSS(millis() - firstPressAt);
}

// Start of every console line: "[left m:ss] " = time left until the 10 min reset
void printPrefix() {
  Serial.print(F("[left "));
  if (hasFirstPress) {
    unsigned long elapsed = millis() - firstPressAt;
    printMMSS(elapsed >= WINDOW_10_MIN_MS ? 0UL : WINDOW_10_MIN_MS - elapsed);
  } else {
    Serial.print(F("--:--"));   // no sequence running
  }
  Serial.print(F("] "));
}

// True when a new track may be started right now
bool canStartTrack() {
  return !playing || INTERRUPT_PLAYING_ON_NEW_PRESS;
}

// Called by MD_YX5300 (from mp3.check()) when the module reports something
void mp3Callback(const MD_YX5300::cbData *status) {
  if (status->code == MD_YX5300::STS_FILE_END && !repeating) {   // single track finished playing
    playing    = false;
    lastPingAt = millis();
    printPrefix();
    Serial.print(F("Track finished | since first press "));
    printMMSS(millis() - firstPressAt);
    Serial.println();
  }
}

// ===================== THREADS =====================
// Note: with Protothreads, don't use switch() inside a thread and
// don't rely on local variables surviving a PT_WAIT/PT_YIELD.

// Thread 1: keep the MP3 module's serial replies processed
static PT_THREAD(mp3Thread(struct pt *pt)) {
  PT_BEGIN(pt);
  while (true) {
    mp3.check();
    PT_YIELD(pt);
  }
  PT_END(pt);
}

// Thread 2: read the button, decide which track to play
static PT_THREAD(buttonThread(struct pt *pt)) {
  // Locals are declared here (outside the protothread switch) and only used
  // between two waits, so their values never need to survive a wait.
  unsigned long now, sinceFirst;
  uint8_t track;
  bool startIt;

  PT_BEGIN(pt);
  while (true) {
    // Wait for (debounced) press. ezButton: LOW = pressed with pull-up
    PT_WAIT_UNTIL(pt, button.getState() == LOW);

    now        = millis();
    sinceFirst = 0;

    // All windows are measured from the FIRST press of the sequence.
    // Once 10 min have passed since it, the next press starts a new sequence.
    if (!hasFirstPress || (now - firstPressAt) >= WINDOW_10_MIN_MS) {
      track         = TRACK_ONE_PRESS;
      firstPressAt  = now;       // start a new sequence
      hasFirstPress = true;
      sinceFirst    = 0;
    } else {
      sinceFirst = now - firstPressAt;
      if      (sinceFirst <= WINDOW_10_SEC_MS) track = TRACK_PRESSED_AGAIN_WITHIN_10_SEC;
      else if (sinceFirst <= WINDOW_30_SEC_MS) track = TRACK_PRESSED_AGAIN_WITHIN_30_SEC;
      else if (sinceFirst <= WINDOW_60_SEC_MS) track = TRACK_PRESSED_AGAIN_WITHIN_60_SEC;
      else if (sinceFirst <= WINDOW_5_MIN_MS)  track = TRACK_PRESSED_AGAIN_WITHIN_5_MIN;
      else                                     track = TRACK_PRESSED_AGAIN_WITHIN_10_MIN;
    }

    holdStart        = now;
    buttonHeld       = true;
    startIt          = canStartTrack();
    if (startIt) {
      requestedTrack  = track;
      requestedRepeat = false;   // normal press: play once
      playRequest     = true;
    }

    digitalWrite(ledPin, HIGH);
    printPrefix(); Serial.print(F("Pressed -> track "));
    Serial.print(track);
    Serial.print(F(" | time since first press: "));
    printMMSS(sinceFirst);
    Serial.println(sinceFirst == 0 ? F(" (first press)") : F(""));
    if (!startIt) {
      printPrefix();
      Serial.println(F("Track still playing - not interrupting"));
    }

    // Wait for release, or until the button has been held long enough
    PT_WAIT_UNTIL(pt, button.getState() == HIGH || (millis() - holdStart) >= HOLD_TIME_MS);

    if (button.getState() == LOW) {
      // Still held after HOLD_TIME_MS -> play the "held" track on repeat
      // (always starts, even if another track is playing)
      requestedTrack  = TRACK_PRESSED_FOR_5_SEC;
      requestedRepeat = true;
      playRequest     = true;
      printPrefix(); Serial.print(F("Held -> track "));
      Serial.print(TRACK_PRESSED_FOR_5_SEC);
      Serial.print(F(" (repeat)"));
      printTimeInfo(millis() - holdStart);
      Serial.println();

      PT_WAIT_UNTIL(pt, button.getState() == HIGH);
    }

    buttonHeld = false;
    releasedAt = millis();
    digitalWrite(ledPin, LOW);
    printPrefix(); Serial.print(F("Released"));
    printTimeInfo(releasedAt - holdStart);
    Serial.println();
  }
  PT_END(pt);
}

// Thread 3: start a requested track.
//  - normal track: plays once to the end (mp3Callback clears "playing" when it finishes)
//  - hold track: plays on repeat; stops TRACK_DURATION_TO_STOP_AFTER_RELEASED ms after release
static PT_THREAD(playerThread(struct pt *pt)) {
  PT_BEGIN(pt);
  while (true) {
    PT_WAIT_UNTIL(pt, playRequest);

    playRequest = false;
    repeating   = requestedRepeat;
    if (repeating) {
      mp3.playTrackRepeat(requestedTrack);   // loop until stopped
    } else {
      mp3.playTrack(requestedTrack);         // play once, no repeat
    }
    playing = true;

    if (!repeating) continue;   // single track: nothing more to do, wait for next request

    // Repeating: play until a new request arrives or the button was released long enough ago
    PT_WAIT_UNTIL(pt, playRequest ||
                      (!buttonHeld && (millis() - releasedAt) >= TRACK_DURATION_TO_STOP_AFTER_RELEASED));

    if (playRequest) continue;   // a new track was requested: loop back and start it

    mp3.playStop();
    repeating  = false;
    playing    = false;
    lastPingAt = millis();
    printPrefix();
    Serial.print(F("Stopped (repeat ended)"));
    printTimeInfo(releasedAt - holdStart);
    Serial.println();
  }
  PT_END(pt);
}

// Thread 4: while idle, keep sending "stop" to make sure the module stays silent
static PT_THREAD(pingThread(struct pt *pt)) {
  PT_BEGIN(pt);
  while (true) {
    PT_WAIT_UNTIL(pt, !playing && !playRequest &&
                      (millis() - lastPingAt) >= PING_DELAY_STOP_TRACK_WHEN_NOT_PLAYING);
    mp3.playStop();
    lastPingAt = millis();
  }
  PT_END(pt);
}

// Thread 5: every 10 s after the first press print the time left until the
// 10 minute window ends; print RESET when it ends
static PT_THREAD(countdownThread(struct pt *pt)) {
  unsigned long elapsed;   // only used right after the wait below

  PT_BEGIN(pt);
  while (true) {
    PT_WAIT_UNTIL(pt, hasFirstPress);
    cdStart = firstPressAt;
    cdTicks = 1;

    while (hasFirstPress && firstPressAt == cdStart) {
      PT_WAIT_UNTIL(pt, !hasFirstPress || firstPressAt != cdStart ||
                        (millis() - cdStart) >= cdTicks * COUNTDOWN_PRINT_INTERVAL_MS);

      if (!hasFirstPress || firstPressAt != cdStart) break;  // sequence was restarted

      elapsed = millis() - cdStart;
      if (elapsed >= WINDOW_10_MIN_MS) {
        printPrefix();   // still shows [left 0:00] because the sequence is not cleared yet
        Serial.println(F("RESET - 10 minutes passed, next press plays first-press track"));
        hasFirstPress = false;  // next press = TRACK_ONE_PRESS again
        break;
      }

      printPrefix();
      Serial.print(F("Countdown: elapsed "));
      printMMSS(elapsed);
      Serial.println();
      cdTicks++;
    }
  }
  PT_END(pt);
}

// ===================== SETUP / LOOP =====================

void setup() {
  Serial.begin(9600);
  pinMode(ledPin, OUTPUT);

  button.setDebounceTime(50);

  mp3Serial.begin(9600);
  mp3.setCallback(mp3Callback);
  mp3.begin();
  mp3.setSynchronous(false);  // non-blocking; replies handled by mp3Thread
  mp3.volume(VOLUME);

  delay(500);

  PT_INIT(&ptMp3);
  PT_INIT(&ptButton);
  PT_INIT(&ptPlayer);
  PT_INIT(&ptPing);
  PT_INIT(&ptCountdown);
}

void loop() {
  button.loop();  // must run every pass, before the threads read the button

  mp3Thread(&ptMp3);
  buttonThread(&ptButton);
  playerThread(&ptPlayer);
  pingThread(&ptPing);
  countdownThread(&ptCountdown);
}
