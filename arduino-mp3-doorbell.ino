#include <ezButton.h> // Include the ezButton library

const int buttonPin = 2; // Push button connected to D2
const int ledPin = 13;   // Onboard LED connected to D13

ezButton button(buttonPin); // Create ezButton object (uses internal pull-up by default)

#include <SoftwareSerial.h>
#include <MD_YX5300.h>

// Define SoftwareSerial pins for Arduino Nano (RX, TX)
#define RX_PIN 10
#define TX_PIN 11

SoftwareSerial mp3Serial(RX_PIN, TX_PIN);
MD_YX5300 mp3(mp3Serial);

bool trackStarted = false;


void setup() {
  Serial.begin(9600);         // Initialize serial communication at 9600 bps
  pinMode(ledPin, OUTPUT);    // Set LED pin as output
  
  button.setDebounceTime(50); // Set debounce time to 50 milliseconds

  mp3Serial.begin(9600);
  
  mp3.begin();
  mp3.setSynchronous(true); // Wait for command execution
  mp3.volume(20);           // Set volume (0 to 30)

  delay(500);
}

void loop() {
  button.loop(); // MUST be called first at the top of the loop to process the button state
  mp3.check(); // Must be called periodically to process serial responses


  // if (!trackStarted) {
  //   uint8_t trackNumber = 5; // Change to your desired track index (1-255)
    
  //   // Option A: Play track once
  //   // mp3.playTrack(trackNumber);

  //   // Option B: Play track in a continuous loop (repeat)
  //   mp3.playTrackRepeat(trackNumber); 
  //   // Alternatively: mp3.playTrack(trackNumber); followed by mp3.repeat(true);

  //   trackStarted = true; // Prevents re-triggering every loop() cycle
  // }


  // Check if the button was just released
  if (button.isReleased()) {
                        if(trackStarted == true){
    digitalWrite(ledPin, LOW);    // Turn LED off
    Serial.println("Just Released");   // Human-readable output
    Serial.write(0);              // Raw binary data
    mp3.playStop();
         trackStarted = false;
      mp3.check(); // Must be called periodically to process serial responses
          Serial.println("stopped");    // Human-readable output
                        }
  }else{
      if (button.getState()==1) {
                    if(trackStarted == true){
            digitalWrite(ledPin, LOW);    // Turn LED off
            Serial.println("Released");   // Human-readable output
    Serial.write(0);              // Raw binary data
        mp3.playStop();
             trackStarted = false;
          mp3.check(); // Must be called periodically to process serial responses
              Serial.println("stopped");    // Human-readable output
      }
      }
  }

    // Check if the button was just pressed
  if (button.isPressed()) {
            if(trackStarted == false){
    digitalWrite(ledPin, HIGH);   // Turn LED on
    Serial.println("Just Pressed");    // Human-readable output
    Serial.write(1);              // Raw binary data
     uint8_t trackNumber =1;
     mp3.playTrackRepeat(trackNumber);
     trackStarted = true;
       mp3.check(); // Must be called periodically to process serial responses
        Serial.println("loop 1");    // Human-readable output
     delay(4000);
        mp3.playStop();
             trackStarted = false;
          mp3.check(); // Must be called periodically to process serial responses
              Serial.println("stopped");    // Human-readable output
            }
  }else{
      if (button.getState()==0) {
        if(trackStarted == false){

            digitalWrite(ledPin, HIGH);   // Turn LED on
            Serial.println("Pressed");    // Human-readable output
    Serial.write(1);              // Raw binary data
        uint8_t trackNumber =2;
        mp3.playTrackRepeat(trackNumber);
        trackStarted = true;
          mp3.check(); // Must be called periodically to process serial responses
           Serial.println("loop 2");    // Human-readable output
        delay(4000);
                  
        }
      }
  }

   delay(100);

}
