#include <Arduino.h>
#include "SmoothThermistor.h"
#include <PID_v1.h>
#include <EEPROM.h>
#include <ClickEncoder.h>
#include <TimerOne.h>
#include <U8glib.h>
#include "avr/wdt.h"
#include "bitmap_logo.h"

#define HEATER_PIN 3
#define SENSOR_PIN A0

#define aLenght(x)       (sizeof(x) / sizeof(x[0]))

enum VIEW { VIEW_LOGO, VIEW_MAIN, VIEW_SETTINGS } view;
enum MEM { MEM1, MEM2, MEM3 } mem;
const char *title[] =  {
  "EXIT", "STDBY TIME", "STDBY TEMP", "POWER OFF", "PID: P", "PID: I", "PID: D", "TEMP CORR", "MAX POWER", "SAVE ALL",
      "RESET ALL"
};
byte menuPosition;


byte memoryToStore;
ClickEncoder *encoder;
int16_t encLast, encValue;

double Setpoint, Input, Output, serialMillis, lcdMillis, logoMillis, blinkMillis, functionTimeout, standByMillis,
    tempBeforeEnteringStandby;
bool isDisplayingLogo, blink, isSavingMemory, isOnStandBy;

// measuring the temp variation per second
double tempDelta;
double oldTemp;
double tempMillis;

// settings menu vars;
bool isEditing;
const char *topText;
const char *bottomText;
bool isFastCount;
int i;
double d;

typedef struct EepromMap {
  byte firstBoot;     // check for a number 123
  double standbyTemp; // temperature on stand-by
  unsigned int standbyTime;
  double p;           // p
  double i;           // i
  double d;           // d
  double m1;          // memory 1
  double m2;          // memory 2
  double m3;          // memory 3
  double tCorrection; // thermistor reading temperature correction factor
  byte maxPower;
  unsigned int timeout; //  seconds to standby
  byte lastMem;         // lastMemory selected

} eeprom_map_t;

eeprom_map_t settings;

U8GLIB_PCD8544 u8g(10, 9, 8); // uses 13 ,11 as Hardware pins 10-CS 9-A0 8-RS

void setPwmFrequency(int, int); // sets pwm frequency divisor
double getTemp();               // read thermistor temp
void resetFailSafe();      // setMenuSel all eencLprom encV to default
void printTunnings();           // outputs de pid settings
void draw();                    // displays a view
void updateLCD();               // updateLCD
void viewLogo();                // logo view
void viewMain();                // main view
void viewSettings();            // settings view
void software_Reboot();         // reboots
void timerIsr();                // rotary switch interrupt
void rotaryMain();              // rotary Main routines
void cicleMem();                // cicle ...MEM1->MEM2->MEM3->MEM1...
void resetTimeouts();           // reset    all timouts running to millis()
void drawMemIcon(byte);         // draws the given memory icon
void resetStandby();
void rotarySettings();
void drawTitle(const char *);
void drawBoxedStr(u8g_uint_t, u8g_uint_t, const char *);
void drawSettingsView(const char *_title, const char *_value, const char * _unit, bool _blink);

SmoothThermistor therm(SENSOR_PIN,      // the analog pin to read from
                       ADC_SIZE_10_BIT, // the ADC size
                       100000,          // the nominal resistance
                       4700,            // the series resistance
                       3950,            // the beta coefficient of the thermistor
                       25,              // the temperature for nominal resistance
                       10);             // the number of samples to take for each measurement

PID myPID(&Input, &Output, &Setpoint, 0, 0, 0, DIRECT);


void setup() {
  Serial.begin(9600);
  Serial.println(F("* START *"));

  tempDelta = 0;
  oldTemp = getTemp();
  // input and output pins
  pinMode(SENSOR_PIN, INPUT);
  pinMode(HEATER_PIN, OUTPUT);

  // rotary encoder
  encoder = new ClickEncoder(A1, A2, A3); // A, B, BTN
  encoder->setAccelerationEnabled(true);
  Timer1.initialize(1000);
  Timer1.attachInterrupt(timerIsr);
  // encLast = -1;
  encLast = encValue = encoder->getValue();

  // LCD
  u8g.setColorIndex(1);
  view = VIEW_LOGO; // display logo view.
  updateLCD();
  isDisplayingLogo = true;

  // heater
  // setPwmFrequency(HEATER_PIN, 1);
  analogWrite(HEATER_PIN, 0); // setMenuSels heater to 0 on s

  lcdMillis = serialMillis = logoMillis = blinkMillis = standByMillis = millis(); // delay routines

  // Load EEPROM
  EEPROM.get(0, settings);
  if (settings.firstBoot != 123) { // if the check number is not present then setMenuSel settings
    resetFailSafe();
  }
  myPID.SetTunings(settings.p, settings.i, settings.d);
  printTunnings();

  Input = getTemp();
  //  choose last selected memory setpoint temperature
  switch (settings.lastMem) {
  case MEM1:
    Setpoint = settings.m1;
    break;
  case MEM2:
    Setpoint = settings.m2;
    break;
  case MEM3:
    Setpoint = settings.m3;
    break;
  default:
    Setpoint = 150;
  }

  myPID.SetMode(AUTOMATIC);                    // enable pid controller
  myPID.SetOutputLimits(0, settings.maxPower); // limits heater pwm duty cycle

  u8g.setColorIndex(0);
  u8g.drawBox(0, 0, 84, 48);
  u8g.setColorIndex(1);
  blink = false;
  isSavingMemory = false;
  memoryToStore = settings.lastMem;
  functionTimeout = 0;
  isOnStandBy = false;
  menuPosition = 0;
  isFastCount = false;
}

void loop() {
  // serial input control
  if (Serial.available() > 0) {
    String strInput = Serial.readString();
    String command = strInput.substring(0, 2);
    double value = strInput.substring(2, strInput.length()).toDouble();
    if (command == "p:") {
      Serial.print(F("changed P value to: "));
      myPID.SetTunings(value, myPID.GetKi(), myPID.GetKd());
      Serial.println(myPID.GetKp());
    } else if (command == "i:") {
      Serial.print(F("changed I value to: "));
      myPID.SetTunings(myPID.GetKp(), value, myPID.GetKd());
      Serial.println(myPID.GetKi());
    } else if (command == "d:") {
      Serial.print(F("changed D value to: "));
      myPID.SetTunings(myPID.GetKp(), myPID.GetKi(), value);
      Serial.println(myPID.GetKd());
    } else if (command == "t") {
      printTunnings();
    } else if (command == "t:") {
      Serial.print(F("Setpoint: "));
      Setpoint = value;
      Serial.println(Setpoint);
    } else if (command == "s") {
      // setMenuSel settings
      settings.p = myPID.GetKp();
      settings.i = myPID.GetKi();
      settings.d = myPID.GetKd();
      EEPROM.put(0, settings);
      Serial.println(F("Settings setMenuSeld!"));
    } else if (command == "r") {
      resetFailSafe();
    } else {
      Serial.println(F("Unknown command!"));
    }
  }

  // rotary
  if (view == VIEW_MAIN) {
    rotaryMain();
  } else if (view == VIEW_SETTINGS) {
    rotarySettings();
  }

  // logo delay
  if (isDisplayingLogo) {
    if (millis() - logoMillis > 2000) { // show logo for 4 seconds
      view = VIEW_MAIN;
      isDisplayingLogo = false;
      updateLCD();
      resetStandby();
    }
  }

  // standby time
  if (!isOnStandBy) {
    if (millis() - standByMillis > (1000 * settings.standbyTime)) {
      tempBeforeEnteringStandby = Setpoint;
      Setpoint = settings.standbyTemp;
      isOnStandBy = true;
    }
  }

  // Control temperature
  Input = getTemp();
  myPID.Compute();
  analogWrite(HEATER_PIN, Output);

  // LCD Update
  if (millis() - lcdMillis > 250) { // lcd update delay
    blink = !blink;
    updateLCD();
    lcdMillis = millis();
  }

  // function timeout
  if (millis() - functionTimeout > 20000) {
    // only permits 20 seconds without action outside main
    // functionTimeout needs to be reset to millis() in every rotary event
    // or it will call main screen
    isSavingMemory = false;
    view = VIEW_MAIN;
    
    updateLCD();
  }

  // temperature delta per second
  if (millis() - tempMillis > 1000){
    double newTemp = getTemp();
    tempDelta = newTemp - oldTemp;

    if (isOnStandBy){
      if (tempDelta < -20){
        resetStandby();
      }
    }
    tempMillis = millis();
  }

  // Plotter
  if (millis() - serialMillis > 5000) { // pace the serial output
    Serial.print(Setpoint);
    Serial.print(" ");
    Serial.print(Input);
    Serial.print(" ");
    Serial.print(Output / settings.maxPower * 100);
    Serial.print(" ");
    Serial.print(settings.p);
    Serial.print(" ");
    Serial.print(settings.i);
    Serial.print(" ");
    Serial.print(settings.d);
    Serial.print(" ");
    Serial.print(isOnStandBy);
    Serial.print(" ");
    Serial.print(settings.standbyTemp);
    Serial.print(" ");
    Serial.println(settings.standbyTime);

    serialMillis = millis();
  }
}

void resetFailSafe() {
  settings.firstBoot = 123;
  settings.standbyTemp = 150;
  settings.standbyTime = 60; // seconds
  settings.p = 4;
  settings.i = 0;
  settings.d = 1.8;
  settings.m1 = 300;
  settings.m2 = 260;
  settings.m3 = 350;
  settings.tCorrection = 0.85;
  settings.maxPower = 220;
  settings.timeout = 30;
  settings.lastMem = MEM1;
  myPID.SetTunings(settings.p, settings.i, settings.d);
  EEPROM.put(0, settings); // save values to eeprom
  Serial.println(F("Reseted!"));
  delay(500);
  software_Reboot();
}

void printTunnings() {
  Serial.print(F("The tunings  P: "));
  Serial.print(myPID.GetKp());
  Serial.print(F(", I: "));
  Serial.print(myPID.GetKi());
  Serial.print(F(", D: "));
  Serial.println(myPID.GetKd());
}

void setPwmFrequency(int pin, int divisor) {
  byte mode;
  if (pin == 5 || pin == 6 || pin == 9 || pin == 10) {
    switch (divisor) {
    case 1:
      mode = 0x01;
      break;
    case 8:
      mode = 0x02;
      break;
    case 64:
      mode = 0x03;
      break;
    case 256:
      mode = 0x04;
      break;
    case 1024:
      mode = 0x05;
      break;
    default:
      return;
    }
    if (pin == 5 || pin == 6) {
      TCCR0B = TCCR0B & 0b11111000 | mode;
    } else {
      TCCR1B = TCCR1B & 0b11111000 | mode;
    }
  } else if (pin == 3 || pin == 11) {
    switch (divisor) {
    case 1:
      mode = 0x01;
      break;
    case 8:
      mode = 0x02;
      break;
    case 32:
      mode = 0x03;
      break;
    case 64:
      mode = 0x04;
      break;
    case 128:
      mode = 0x05;
      break;
    case 256:
      mode = 0x06;
      break;
    case 1024:
      mode = 0x07;
      break;
    default:
      return;
    }
    TCCR2B = TCCR2B & 0b11111000 | mode;
  }
}

void software_Reboot() {
  wdt_enable(WDTO_15MS);
  while (1) {
  }
}

void timerIsr() { encoder->service(); }

void draw() {
  // graphic commands to redraw the complete screen should be placed here
  u8g.setContrast(110);

  switch (view) {
  case VIEW_LOGO:
    viewLogo();
    break;
  case VIEW_MAIN:
    viewMain();
    break;
  case VIEW_SETTINGS:
    viewSettings();
    break;
  default:
    break;
  }
}

void updateLCD() {
  u8g.firstPage();
  do {
    draw();
  } while (u8g.nextPage());
}

// views layout
void viewLogo() { u8g.drawXBMP(0, 0, 84, 48, bitmap_logo); }
void viewMain() {

  // common main view mode drawing
  // temperature
  u8g.setColorIndex(1);
  u8g.setFont(u8g_font_freedoomr25n);
  u8g.drawStr(0, 37, String((int)Setpoint).c_str());
  u8g.drawCircle(59, 14, 2);
  u8g.setFont(u8g_font_freedoomr10r);
  u8g.drawStr(61, 37, "C");
  if (!isSavingMemory) {
    // render main view - normal
    u8g.setColorIndex(1);
    u8g.setFont(u8g_font_freedoomr10r);
    u8g.drawStr(0, 12, String((int)(Input + 0.5)).c_str());

    // draw pwr-meter
    // unit bar height is 5px, 8 boxes separated by 2px
    byte unit = settings.maxPower / 8; // max power in settings divided 8 bars
    // draw power barstop to bottom
    if (Output > (settings.maxPower - unit)) {
      u8g.drawBox(68, 0, 16, 5);
    }
    if (Output > (settings.maxPower - (2 * unit))) {
      u8g.drawBox(69, 8, 15, 5);
    }
    if (Output > (settings.maxPower - (3 * unit))) {
      u8g.drawBox(70, 16, 14, 5);
    }
    if (Output > (settings.maxPower - (4 * unit))) {
      u8g.drawBox(71, 24, 13, 5);
    }
    if (Output > (settings.maxPower - (5 * unit))) {
      u8g.drawBox(72, 32, 12, 5);
    }
    if (Output > (settings.maxPower - (6 * unit))) {
      u8g.drawBox(73, 40, 11, 5);
    }
    if (Output > (settings.maxPower - (7 * unit))) {
      u8g.drawBox(74, 48, 10, 5);
    }

    // draw the memory icon
    if (Setpoint == settings.m1) {
      drawMemIcon(MEM1);
      settings.lastMem = MEM1;
    } else if (Setpoint == settings.m2) {
      drawMemIcon(MEM2);
      settings.lastMem = MEM2;
    } else if (Setpoint == settings.m3) {
      drawMemIcon(MEM3);
      settings.lastMem = MEM3;
    }
  } else {

    // render main view - store
    u8g.setColorIndex(1);
    u8g.setFont(u8g_font_freedoomr10r);
    u8g.drawStr(0, 12, "SELECT MEM");

    if (blink) { // blink the memory icon
      drawMemIcon(memoryToStore);
    }
  }
}
void viewSettings() {
  switch(menuPosition){
    char topBuf[8];
    case 0: // exit
      topText = "";
      bottomText = "REBOOT";
      break;
    case 1: //stand by time
      topText = dtostrf(settings.standbyTime+0.0f,0,0,topBuf);
      bottomText = "SECONDS";
      break;
    case 2: // stand by temperature
      topText = dtostrf(settings.standbyTemp+0.0f,0,0,topBuf);
      bottomText = "CELSIUS";
      break;
    case 3: // power off
      topText = dtostrf(settings.timeout+0.0f,0,0,topBuf);
      bottomText = "MINUTES";
      break;
    case 4: // P
      topText = dtostrf(settings.p,0,2,topBuf);
      bottomText = "P";
      break;
    case 5: // I
      topText = dtostrf(settings.i,0,2,topBuf);
      bottomText = "I";
      break;
    case 6: // D
      topText = dtostrf(settings.d,0,2,topBuf);
      bottomText = "D";
      break;
    case 7: // temp correction
      topText = dtostrf(settings.tCorrection,0,2,topBuf);
      bottomText = "FACTOR";
      break;
    case 8: // max power
      topText = dtostrf(map(settings.maxPower,0,254,0,100)+0.0f,0,0,topBuf);
      bottomText = "%";
      break;
    case 9: // save
      topText = "";
      bottomText = "EEPROM";
      break;
    case 10: // reset
      topText = "";
      bottomText = "DEFAULTS";
      break;
    default:
      topText = "";
      bottomText = "";
    break;

  }


  // render the view
  drawTitle(title[menuPosition]);
  u8g.setColorIndex(1);
  u8g.setFont(u8g_font_freedoomr25n);
  if(isEditing){
    if (blink){
      u8g.drawStr(42-(u8g.getStrWidth(topText)/2), 39, topText); // center
    }
  }else{
    u8g.drawStr(42-(u8g.getStrWidth(topText)/2), 39, topText);
  }
  u8g.setFont(u8g_font_freedoomr10r);
  u8g.drawStr(42-(u8g.getStrWidth(bottomText)/2), 50, bottomText);
  // if (isFastCount){
  //   u8g.drawStr(0,40,"*"); // draw fast count icon
  // }
 }

// rotary behaviour
void rotaryMain() {

  encValue += encoder->getValue();
  if (isOnStandBy) {
    encLast = encValue;
  }
  if (encValue != encLast) {
    resetStandby();
    if (encValue > encLast) {
      if (!isSavingMemory) {
        Setpoint = constrain(Setpoint + (5 * (encValue - encLast)), 100, 400);
      } else {
        memoryToStore = constrain(memoryToStore + 1, 0, 2);
      }
    }
    if (encValue < encLast) {
      if (!isSavingMemory) {
        Setpoint = constrain(Setpoint - (5 * (encLast - encValue)), 100, 400);
      } else {
        memoryToStore = constrain(memoryToStore - 1, 0, 2);
      }
    }

    encLast = encValue;
    updateLCD();
  }

  ClickEncoder::Button b = encoder->getButton();

  if (b == ClickEncoder::Clicked) {
    void resetTimeouts();
    if (isOnStandBy) {
      Setpoint = tempBeforeEnteringStandby;
      resetStandby();
      return;
    }
    if (!isSavingMemory) {
      cicleMem();
    } else {
      switch (memoryToStore) {
      case MEM1:
        settings.m1 = Setpoint;
        settings.lastMem = MEM1;
        break;
      case MEM2:
        settings.m2 = Setpoint;
        settings.lastMem = MEM2;
        break;
      case MEM3:
        settings.m3 = Setpoint;
        settings.lastMem = MEM3;
        break;
      default:
        break;
      }
      EEPROM.put(0, settings);
      Serial.println("Memory setMenuSeld!");
      isSavingMemory = false;
    }
  }
  if (b == ClickEncoder::Held) {
    if (!isSavingMemory) {
      resetTimeouts();
      isSavingMemory = true;
    }
  }
  if (b == ClickEncoder::DoubleClicked) {
    resetTimeouts();
    // Serial.println("double clicked");
    view = VIEW_SETTINGS;
  }
}

void rotarySettings() {
  ClickEncoder::Button b = encoder->getButton();
  encValue += encoder->getValue();

  if (encValue != encLast) {
    resetTimeouts();

    if (encValue > encLast) {
      if(!isEditing){// if is not editing increment menu
        menuPosition = constrain(menuPosition+1,0,10); 
      }else{


        // increment value
        switch(menuPosition){
          case 1:
            settings.standbyTime = constrain(settings.standbyTime+5,30,300);
          break;
          case 2:
            i = (isFastCount) ? 10 : 5;
            settings.standbyTemp = constrain(settings.standbyTemp + i, 0 , 250);
          break;
          case 3:
            settings.timeout = constrain(settings.timeout + 10,10,120);
          break;
          case 4:
            d = (isFastCount) ? 1 : 0.01;
            settings.p = constrain(settings.p + d,0,30);
          break;
          case 5:
            d = (isFastCount) ? 1 : 0.01;
            settings.i = constrain(settings.i + d,0,30);
          break;
          case 6:
            d = (isFastCount) ? 1 : 0.01;
            settings.d = constrain(settings.d + d,0,30);
          break;
          case 7:
            settings.tCorrection = constrain(settings.tCorrection+0.05,0.5,1.5);
          break;
          case 8:
            settings.maxPower = constrain(settings.maxPower+2,50,255);
          break;
          default:
          break;
        }

      }
    }
    if (encValue < encLast) {
      if(!isEditing){// if is not editing decrement menu
        menuPosition = constrain(menuPosition-1,0,10); 
      }else{
        // decrement value
        
        switch(menuPosition){
          case 1:
            settings.standbyTime = constrain(settings.standbyTime-5,30,300);
            break;
          case 2:
            i = (isFastCount) ? 10 : 5;
            settings.standbyTemp = constrain(settings.standbyTemp - i, 0 , 250);
          break;
          case 3:
            settings.timeout = constrain(settings.timeout - 10,10,120);
          break;
          case 4:
            d = (isFastCount) ? 1 : 0.01;
            settings.p = constrain(settings.p - d,0,30);
          break;
          case 5:
            d = (isFastCount) ? 1 : 0.01;
            settings.i = constrain(settings.i - d,0,30);
          break;
          case 6:
            d = (isFastCount) ? 1 : 0.01;
            settings.d = constrain(settings.d - d,0,30);
          break;
          case 7:
            settings.tCorrection = constrain(settings.tCorrection-0.05,0.5,1.5);
          break;
          case 8:
            settings.maxPower = constrain(settings.maxPower-2,50,255);
          break;
          default:
          break;
        }
        
      }
    }
    blink = true;
    updateLCD();
    blink = true;
  }
  encLast = encValue;
  // on click
  if (b == ClickEncoder::Clicked) {
    resetTimeouts();
    if(menuPosition == 0 && !isEditing){// exit
      software_Reboot();
    }else if (menuPosition == 10 && !isEditing ){ // reset
      resetFailSafe();
    }else if (menuPosition == 9 && !isEditing) {  //save
      EEPROM.put(0,settings);
      delay(100);
      software_Reboot();
    }

    isEditing = !isEditing;
    
  }
  if (b == ClickEncoder::Held){
    
      isFastCount = true;
    
  }else{
    isFastCount = false;
  }

  
}

void cicleMem() {
  switch (settings.lastMem) {
  case MEM1:
    settings.lastMem = MEM2;
    Setpoint = settings.m2;
    break;
  case MEM2:
    settings.lastMem = MEM3;
    Setpoint = settings.m3;
    break;
  case MEM3:
    settings.lastMem = MEM1;
    Setpoint = settings.m1;
  default:
    break;
  }

  // EEPROM.put(0,settings); //setMenuSel
}

void resetTimeouts() { functionTimeout = millis(); }

void drawMemIcon(byte memory) {
  switch (memory) {

  case MEM1:
    u8g.setFont(u8g_font_freedoomr10r);
    u8g.setColorIndex(1);
    u8g.drawBox(0, 36, 18, 12);
    u8g.setColorIndex(0);
    u8g.drawStr(2, 49, "M1");
    break;
  case MEM2:
    u8g.setFont(u8g_font_freedoomr10r);
    u8g.setColorIndex(1);
    u8g.drawBox(20, 36, 18, 12);
    u8g.setColorIndex(0);
    u8g.drawStr(22, 49, "M2");
    break;
  case MEM3:
    u8g.setFont(u8g_font_freedoomr10r);
    u8g.setColorIndex(1);
    u8g.drawBox(39, 36, 18, 12);
    u8g.setColorIndex(0);
    u8g.drawStr(41, 49, "M3");
    break;
  default:
    break;
  }
}

double getTemp() { return therm.temperature() * settings.tCorrection; }

void resetStandby() {
  isOnStandBy = false;
  standByMillis = millis();
}

// void drawTitle(const char *title) {
//   u8g.setColorIndex(1);
//   u8g.drawRBox(0, 0, 83, 10, 2);
//   u8g.setColorIndex(0);
//   //u8g.setFont(u8g_font_6x10r);
//   u8g.setFont(u8g_font_freedoomr10r);
//   u8g.drawStr(42 - (u8g.getStrWidth(title) / 2), 8, title);
// }
void drawTitle(const char *title) {
  u8g.setColorIndex(1);
  u8g.drawRBox(0, 0, 83, 12, 2);
  u8g.setColorIndex(0);
  //u8g.setFont(u8g_font_6x10r);
  u8g.setFont(u8g_font_freedoomr10r);
  u8g.drawStr(42 - (u8g.getStrWidth(title) / 2), 13, title);
}

void drawBoxedStr(u8g_uint_t x, u8g_uint_t y, const char *s) {
  uint8_t color = u8g.getColorIndex();
  u8g.setColorIndex(1);
  u8g.drawRBox(x - 2, y - u8g.getFontLineSpacing(), u8g.getStrWidth(s) + 3, u8g.getFontLineSpacing() + 2, 2);
  u8g.setColorIndex(0);
  u8g.drawStr(x, y, s);
  u8g.setColorIndex(color);
}

void drawSettingsView(const char *_title, const char *_value, const char * _unit, bool _blink){
  drawTitle(_title);
  u8g.setColorIndex(1);
  u8g.setFont(u8g_font_freedoomr10r);
  if(_blink){
    if (blink){
      u8g.drawStr(42-(u8g.getStrWidth(_value)/2), 40, _value); // center
    }
  }else{
    u8g.drawStr(42-(u8g.getStrWidth(_value)/2), 40, _value);
  }
  u8g.setFont(u8g_font_6x10r);
  u8g.drawStr(42-(u8g.getStrWidth(_unit)/2), 48, _unit);

}

