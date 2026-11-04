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
const uint8_t TRACK_PRESSED_AGAIN_WITHIN_10_SEC = 3;  // pressed again within 10 s of last press
const uint8_t TRACK_PRESSED_AGAIN_WITHIN_30_SEC = 4;  // pressed again within 30 s of last press
const uint8_t TRACK_PRESSED_AGAIN_WITHIN_60_SEC = 5;  // pressed again within 60 s of last press


// Timing (milliseconds)
const unsigned long TRACK_DURATION_TO_STOP_AFTER_RELEASED   = 4000; // keep playing this long after release, then stop
const unsigned long PING_DELAY_STOP_TRACK_WHEN_NOT_PLAYING  = 3000; // while idle, send "stop" this often

// Other
const uint8_t       VOLUME            = 20;    // 0 - 30
const unsigned long HOLD_TIME_MS      = 5000;  // how long counts as "pressed for 5 sec"
const unsigned long WINDOW_10_SEC_MS  = 10000;
const unsigned long WINDOW_30_SEC_MS  = 30000;
const unsigned long WINDOW_60_SEC_MS  = 60000;

// ===================== PINS / OBJECTS =====================

const int buttonPin = 2;   // Push button on D2
const int ledPin    = 13;  // Onboard LED

#define RX_PIN 10
#define TX_PIN 11

ezButton button(buttonPin);
SoftwareSerial mp3Serial(RX_PIN, TX_PIN);
MD_YX5300 mp3(mp3Serial);

// ===================== SHARED STATE =====================

static struct pt ptMp3, ptButton, ptPlayer, ptPing;

bool          playing         = false;  // a track is currently playing
bool          playRequest     = false;  // button thread asks player thread to (re)start a track
uint8_t       requestedTrack  = 0;      // track to play for the request
bool          buttonHeld      = false;  // button currently down
unsigned long releasedAt      = 0;      // millis() when button was released
unsigned long holdStart       = 0;      // millis() when button was pressed
unsigned long lastPressAt     = 0;      // millis() of previous press
bool          hasPreviousPress = false;
unsigned long lastPingAt      = 0;

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
  PT_BEGIN(pt);
  while (true) {
    // Wait for (debounced) press. ezButton: LOW = pressed with pull-up
    PT_WAIT_UNTIL(pt, button.getState() == LOW);

    unsigned long now = millis();
    uint8_t track;

    if (!hasPreviousPress) {
      track = TRACK_ONE_PRESS;
    } else {
      unsigned long gap = now - lastPressAt;
      if      (gap <= WINDOW_10_SEC_MS) track = TRACK_PRESSED_AGAIN_WITHIN_10_SEC;
      else if (gap <= WINDOW_30_SEC_MS) track = TRACK_PRESSED_AGAIN_WITHIN_30_SEC;
      else if (gap <= WINDOW_60_SEC_MS) track = TRACK_PRESSED_AGAIN_WITHIN_60_SEC;
      else                              track = TRACK_ONE_PRESS;
    }

    lastPressAt      = now;
    hasPreviousPress = true;
    holdStart        = now;
    buttonHeld       = true;
    requestedTrack   = track;
    playRequest      = true;

    digitalWrite(ledPin, HIGH);
    Serial.print("Pressed -> track ");
    Serial.println(track);

    // Wait for release, or until the button has been held long enough
    PT_WAIT_UNTIL(pt, button.getState() == HIGH || (millis() - holdStart) >= HOLD_TIME_MS);

    if (button.getState() == LOW) {
      // Still held after HOLD_TIME_MS -> switch to the "held" track
      requestedTrack = TRACK_PRESSED_FOR_5_SEC;
      playRequest    = true;
      Serial.print("Held -> track ");
      Serial.println(TRACK_PRESSED_FOR_5_SEC);

      PT_WAIT_UNTIL(pt, button.getState() == HIGH);
    }

    buttonHeld = false;
    releasedAt = millis();
    digitalWrite(ledPin, LOW);
    Serial.println("Released");
  }
  PT_END(pt);
}

// Thread 3: start tracks, stop them after release + TRACK_DURATION_TO_STOP_AFTER_RELEASED
static PT_THREAD(playerThread(struct pt *pt)) {
  PT_BEGIN(pt);
  while (true) {
    PT_WAIT_UNTIL(pt, playRequest);

    playRequest = false;
    mp3.playTrackRepeat(requestedTrack);
    playing = true;

    // Play until a new request arrives (restart) or the button was
    // released long enough ago
    PT_WAIT_UNTIL(pt, playRequest ||
                      (!buttonHeld && (millis() - releasedAt) >= TRACK_DURATION_TO_STOP_AFTER_RELEASED));

    if (playRequest) {
      continue;  // new press / 5 s hold: loop back and start the new track
    }

    mp3.playStop();
    playing    = false;
    lastPingAt = millis();
    Serial.println("Stopped");
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

// ===================== SETUP / LOOP =====================

void setup() {
  Serial.begin(9600);
  pinMode(ledPin, OUTPUT);

  button.setDebounceTime(50);

  mp3Serial.begin(9600);
  mp3.begin();
  mp3.setSynchronous(false);  // non-blocking; replies handled by mp3Thread
  mp3.volume(VOLUME);

  delay(500);

  PT_INIT(&ptMp3);
  PT_INIT(&ptButton);
  PT_INIT(&ptPlayer);
  PT_INIT(&ptPing);
}

void loop() {
  button.loop();  // must run every pass, before the threads read the button

  mp3Thread(&ptMp3);
  buttonThread(&ptButton);
  playerThread(&ptPlayer);
  pingThread(&ptPing);
}
