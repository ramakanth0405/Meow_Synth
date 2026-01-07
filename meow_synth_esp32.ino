#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <Adafruit_MPR121.h>
#include <Adafruit_MPU6050.h>
#include <Adafruit_Sensor.h>
#include <ESP32Encoder.h>
#include <driver/i2s.h>
#include <math.h>
#include <vector>

// --- NATIVE BLUETOOTH LIBRARIES ---
#include <BLEDevice.h>
#include <BLEUtils.h>
#include <BLEServer.h>
#include <BLE2902.h>

// --- HARDWARE CONFIG ---
#define I2S_DOUT      25
#define I2S_BCLK      27
#define I2S_LRC       26

// Encoder Pins (Direct Connection)
#define ENC_CLK       32
#define ENC_DT        33
#define ENC_SW        19

#define SDA_PIN       21
#define SCL_PIN       22
#define SCREEN_WIDTH  128
#define SCREEN_HEIGHT 32
#define OLED_RESET    -1

// --- AUDIO SETTINGS ---
#define SAMPLE_RATE   44100
// OPTIMIZATION: Reduced voices from 14 to 10 to prevent CPU overload/crackling
#define NUM_VOICES    10  
#define BASE_NOTE     48
#define PI_2          6.2831853f

// --- BLE MIDI CONSTANTS ---
#define SERVICE_UUID        "03b80e5a-ede8-4b33-a751-6ce34ec4c700"
#define CHARACTERISTIC_UUID "7772e5db-3868-4112-a1a9-f2669d106bf3"
BLECharacteristic *pCharacteristic;
bool deviceConnected = false;

// --- DATA STRUCTURES (SCALES & INSTRUMENTS) ---
struct Scale { const char* name; int offsets[12]; };
#define NUM_SCALES 6
Scale scales[NUM_SCALES] = {
  {"Kurd (D)",     {0, 2, 3, 5, 7, 8, 10, 12, 14, 15, 17, 19}},
  {"Celtic",       {0, 2, 3, 7, 9, 12, 14, 15, 19, 21, 24, 26}},
  {"Integral",     {0, 2, 3, 7, 10, 12, 14, 15, 19, 22, 24, 26}},
  {"Penta Maj",    {0, 2, 4, 7, 9, 12, 14, 16, 19, 21, 24, 26}},
  {"Hijaz",        {0, 1, 4, 5, 7, 8, 10, 12, 13, 16, 17, 19}},
  {"Chromatic",    {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11}} 
};

struct Instrument { 
  const char* name; 
  int engineType; // 0=Bell, 1=Bass, 2=Glock, 3=Piano, 4=DrumKit
  int octaveShift;
  float decay; 
};
#define NUM_INSTRUMENTS 5
Instrument instruments[NUM_INSTRUMENTS] = {
  {"Handpan",   0,  0, 0.9994f},
  {"Glock",     2, 24, 0.9996f},
  {"Bass Gtr",  1, -24, 0.9997f},
  {"E-Piano",   3,  0, 0.9990f},
  {"Drum Kit",  4,  0, 0.9900f}
};

// --- OBJECTS ---
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);
Adafruit_MPR121 touch = Adafruit_MPR121();
Adafruit_MPU6050 mpu;
ESP32Encoder encoder;

// --- AUDIO ENGINE STRUCTS ---
struct Envelope { float level; float decayMultiplier; };
struct Voice { 
  bool active;
  float frequency;    
  float currentFreq;  
  float phase; 
  float phaseMod; 
  Envelope env; 
  int noteIndex; 
  float prevSample;   
  int engineType;
};
Voice voices[NUM_VOICES];

// --- STATE VARIABLES ---
int currentScaleIndex = 0;
int currentInstIndex = 0;
float globalVolume = 1.0; 
float pitchBend = 1.0; 
bool padState[12] = {0};

// Menu System
int focus = 0; // 0=Vol, 1=Scale, 2=Inst, 3=Loop
unsigned long buttonDownTime = 0;
bool buttonHeld = false;

// Looper
struct LoopEvent { unsigned long timestamp; byte padIndex; bool isNoteOn; };
enum LooperState { LOOP_IDLE, LOOP_ARMED, LOOP_RECORDING, LOOP_PLAYING };
LooperState loopState = LOOP_IDLE;
std::vector<LoopEvent> loopSequence;
unsigned long loopStartTime = 0;
unsigned long loopLength = 0;
unsigned long loopPlaybackStart = 0;

// --- BLE CALLBACKS ---
class MyServerCallbacks: public BLEServerCallbacks {
    void onConnect(BLEServer* pServer) { deviceConnected = true; };
    void onDisconnect(BLEServer* pServer) { deviceConnected = false; pServer->getAdvertising()->start(); }
};

// --- OPTIMIZED AUDIO TASK ---
void audioTask(void *pvParameters) {
  // OPTIMIZATION: Increased buffer size to process more samples at once (efficiency)
  int32_t sampleBuffer[256]; 
  size_t bytes_written;

  while(1) {
    // Process 128 stereo frames (256 samples total) per loop
    for (int i = 0; i < 128; i++) { 
      float mixedSample = 0;
      int activeCount = 0;

      for (int v = 0; v < NUM_VOICES; v++) {
        if (!voices[v].active) continue; // OPTIMIZATION: Skip inactive voices immediately

        float sample = 0;
        int type = voices[v].engineType;

        // -- ENGINE 0: Handpan (FM) --
        if (type == 0) {
           float mod = sinf(voices[v].phaseMod * PI_2) * 2.5f;
           // OPTIMIZATION: Combine calculation steps
           sample = sinf((voices[v].phase * PI_2) + (mod * voices[v].env.level)) * voices[v].env.level;
           
           voices[v].phaseMod += ((voices[v].frequency * 2.0f) / SAMPLE_RATE) * pitchBend;
           if (voices[v].phaseMod >= 1.0f) voices[v].phaseMod -= 1.0f;
        }
        // -- ENGINE 1: Bass (Saw) --
        else if (type == 1) {
           float rawSaw = (voices[v].phase * 2.0f) - 1.0f;
           float filterCutoff = voices[v].env.level;
           voices[v].prevSample = (filterCutoff * rawSaw) + ((1.0f - filterCutoff) * voices[v].prevSample);
           
           // Fast constrain
           if(voices[v].prevSample > 1.2f) voices[v].prevSample = 1.2f;
           else if(voices[v].prevSample < -1.2f) voices[v].prevSample = -1.2f;
           
           sample = voices[v].prevSample;
        }
        // -- ENGINE 2: Glock (Sine) --
        else if (type == 2) {
           sample = sinf(voices[v].phase * PI_2) * voices[v].env.level;
        }
        // -- ENGINE 3: E-Piano (Soft Square) --
        else if (type == 3) {
           float rawSquare = (voices[v].phase < 0.5f) ? 0.8f : -0.8f;
           voices[v].prevSample = (0.2f * rawSquare) + (0.8f * voices[v].prevSample);
           sample = voices[v].prevSample * voices[v].env.level;
        }
        // -- ENGINE 4: Drum Kit --
        else if (type == 4) {
           int drum = voices[v].noteIndex / 4;
           if (drum == 0) { // Kick
              sample = sinf(voices[v].phase * PI_2) * voices[v].env.level;
              voices[v].currentFreq *= 0.9990f; 
              if(voices[v].currentFreq < 50) voices[v].currentFreq = 50;
              voices[v].phase += (voices[v].currentFreq / SAMPLE_RATE); // Independent phase add for kick
              // Skip the global phase add at bottom
              goto skipGlobalPhase; 
           } 
           else if (drum == 1) { // Snare
              float noise = ((float)rand() / RAND_MAX) * 2.0f - 1.0f;
              sample = noise * voices[v].env.level;
           } 
           else { // Hat
              float noise = ((float)rand() / RAND_MAX) * 2.0f - 1.0f;
              float hp = noise - voices[v].prevSample; 
              voices[v].prevSample = noise;
              sample = hp * voices[v].env.level;
           }
        }

        // Global Phase Increment for non-kick instruments
        voices[v].phase += (voices[v].frequency / SAMPLE_RATE) * pitchBend;

        skipGlobalPhase:
        if (voices[v].phase >= 1.0f) voices[v].phase -= 1.0f;
        
        // Decay
        voices[v].env.level *= voices[v].env.decayMultiplier;
        if (voices[v].env.level < 0.0001f) voices[v].active = false;

        mixedSample += sample;
        activeCount++;
      }

      // OPTIMIZATION: Replaced expensive tanhf() with Fast Soft Clipper
      if (activeCount > 0) {
        float gain = 1.0f / sqrt((float)activeCount + 1.0f); 
        mixedSample *= (gain * globalVolume);
        
        // Fast Limiter (prevents overflow without heavy math)
        if (mixedSample > 0.95f) mixedSample = 0.95f;
        else if (mixedSample < -0.95f) mixedSample = -0.95f;
      }

      // Convert to 32-bit (Shifted high for I2S volume)
      int32_t out = (int32_t)(mixedSample * 2147483647.0f); 
      
      // Interleaved Stereo (Left = Right)
      sampleBuffer[i*2] = out; 
      sampleBuffer[i*2+1] = out; 
    }
    i2s_write(I2S_NUM_0, sampleBuffer, sizeof(sampleBuffer), &bytes_written, portMAX_DELAY);
  }
}

// --- I2S SETUP ---
void setupI2S() {
  i2s_config_t i2s_config = {
    .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX),
    .sample_rate = SAMPLE_RATE,
    .bits_per_sample = I2S_BITS_PER_SAMPLE_32BIT, 
    .channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT,
    .communication_format = I2S_COMM_FORMAT_I2S,
    .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
    // OPTIMIZATION: Increased DMA buffers to prevent glitches
    .dma_buf_count = 8,   
    .dma_buf_len = 512,   
    .use_apll = true      
  };
  i2s_pin_config_t pin_config = {
    .bck_io_num = I2S_BCLK, .ws_io_num = I2S_LRC, .data_out_num = I2S_DOUT, .data_in_num = I2S_PIN_NO_CHANGE
  };
  i2s_driver_install(I2S_NUM_0, &i2s_config, 0, NULL);
  i2s_set_pin(I2S_NUM_0, &pin_config);
}

// --- SETUP ---
void setup() {
  Serial.begin(115200);
  delay(1000); 
  Wire.begin(SDA_PIN, SCL_PIN);
  
  display.begin(SSD1306_SWITCHCAPVCC, 0x3C);
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(0,0); display.println("System Boot..."); display.display();

  // BLE Setup
  BLEDevice::init("Dodepan Multi");
  BLEServer *pServer = BLEDevice::createServer();
  pServer->setCallbacks(new MyServerCallbacks());
  BLEService *pService = pServer->createService(BLEUUID(SERVICE_UUID));
  pCharacteristic = pService->createCharacteristic(
                      BLEUUID(CHARACTERISTIC_UUID),
                      BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_NOTIFY | BLECharacteristic::PROPERTY_WRITE_NR
                    );
  pCharacteristic->addDescriptor(new BLE2902());
  pService->start();
  pServer->getAdvertising()->addServiceUUID(SERVICE_UUID);
  pServer->getAdvertising()->start();

  // Touch Setup
  if (!touch.begin(0x5A)) { Serial.println("MPR121 Error"); }
  touch.writeRegister(0x7B, 0x00); 
  touch.writeRegister(0x5E, 0x00);
  touch.writeRegister(0x2B, 0x01); touch.writeRegister(0x2C, 0x01);
  touch.writeRegister(0x2D, 0x0E); touch.writeRegister(0x2E, 0x00);
  touch.setThresholds(6, 3);       
  touch.writeRegister(0x5E, 0x8F); 

  if (mpu.begin()) {
    mpu.setAccelerometerRange(MPU6050_RANGE_8_G);
    mpu.setFilterBandwidth(MPU6050_BAND_21_HZ);
  }

  // --- ENCODER SETUP (RAW WIRING SUPPORT) ---
  // Enable internal weak pull-up resistors for the rotation pins (A/B)
  ESP32Encoder::useInternalWeakPullResistors = puType::up;
  encoder.attachHalfQuad(ENC_DT, ENC_CLK);
  encoder.setCount(90); 
  // Enable internal pull-up for the switch pin (Connect other side to GND)
  pinMode(ENC_SW, INPUT_PULLUP);

  loopSequence.reserve(200); 
  setupI2S();
  
  // Start Audio Task on Core 1
  xTaskCreatePinnedToCore(audioTask, "AudioTask", 10000, NULL, 1, NULL, 0);
}

// --- HELPER FUNCTIONS ---
void noteOn(int padIndex, bool isLive) {
  if (isLive && loopState == LOOP_RECORDING) {
     LoopEvent e;
     e.timestamp = millis() - loopStartTime; e.padIndex = padIndex; e.isNoteOn = true; loopSequence.push_back(e);
  } else if (isLive && loopState == LOOP_ARMED) {
     loopState = LOOP_RECORDING; loopStartTime = millis(); loopSequence.clear();
     LoopEvent e; e.timestamp = 0; e.padIndex = padIndex; e.isNoteOn = true; loopSequence.push_back(e);
  }

  // Find free voice
  int v = 0;
  // Simple round-robin or first available
  for (int i = 0; i < NUM_VOICES; i++) { if (!voices[i].active) { v = i; break; } }
  
  int engine = instruments[currentInstIndex].engineType;
  int offset = scales[currentScaleIndex].offsets[padIndex];
  int octave = instruments[currentInstIndex].octaveShift;
  float decay = instruments[currentInstIndex].decay;
  
  float freq = 440.0f * pow(2.0f, (offset + BASE_NOTE + octave - 69) / 12.0f);

  voices[v].active = true;
  voices[v].noteIndex = padIndex;
  voices[v].frequency = freq;
  voices[v].phase = 0;
  voices[v].env.level = 1.0f;
  voices[v].env.decayMultiplier = decay;
  voices[v].engineType = engine;
  
  if (engine == 1) voices[v].prevSample = 0;
  if (engine == 4) { // Drums
     voices[v].currentFreq = 300.0f;
     if (padIndex >= 8) voices[v].env.decayMultiplier = 0.9800f; // Short hats
  }
}

void sendMidi(uint8_t status, uint8_t data1, uint8_t data2) {
  if (deviceConnected) {
    uint8_t packet[] = {0x80, 0x80, status, data1, data2};
    pCharacteristic->setValue(packet, 5);
    pCharacteristic->notify();
  }
}

// --- MAIN LOOP ---
void loop() {
  unsigned long currentMillis = millis();
  
  // Software Debounce for Button
  int btnReading = digitalRead(ENC_SW);
  static int lastBtnState = HIGH;
  static unsigned long lastDebounceTime = 0;
  
  // Check if button state changed
  if (btnReading != lastBtnState) {
    lastDebounceTime = currentMillis;
  }
  
  // Only act if the state has been stable for 50ms (Debouncing)
  if ((currentMillis - lastDebounceTime) > 50) {
      int btnState = btnReading;
      
      // Button Logic
      if (btnState == LOW && buttonDownTime == 0) { 
        buttonDownTime = currentMillis;
        buttonHeld = false; 
      }
    
      // Long press (Looper)
      if (btnState == LOW && (currentMillis - buttonDownTime > 800) && !buttonHeld) {
        buttonHeld = true;
        if (focus == 3) { 
          if (loopState == LOOP_IDLE) loopState = LOOP_ARMED;
          else if (loopState == LOOP_ARMED || loopState == LOOP_RECORDING) {
             loopState = LOOP_PLAYING;
             loopLength = millis() - loopStartTime; loopPlaybackStart = millis();
          }
          else if (loopState == LOOP_PLAYING) { loopState = LOOP_IDLE; loopSequence.clear(); }
        }
      }
    
      // Short press (Navigate)
      if (btnState == HIGH && buttonDownTime > 0) {
        if (!buttonHeld) {
           focus++;
           if (focus > 3) focus = 0;
           // Snap encoder
           if (focus == 0) encoder.setCount((int)(globalVolume * 100));
           if (focus == 1) encoder.setCount(currentScaleIndex * 4);
           if (focus == 2) encoder.setCount(currentInstIndex * 4);
        }
        buttonDownTime = 0; 
      }
  }
  lastBtnState = btnReading;

  // --- ENCODER LOGIC ---
  long encVal = encoder.getCount();
  if (focus == 0) { // VOL
    if (encVal < 0) encoder.setCount(0); if (encVal > 100) encoder.setCount(100);
    globalVolume = (float)encoder.getCount() / 100.0f;
  } 
  else if (focus == 1) { // SCALE
    if (encVal < 0) encoder.setCount(0);
    if (encVal > (NUM_SCALES * 4) - 1) encoder.setCount((NUM_SCALES * 4) - 1);
    currentScaleIndex = encoder.getCount() / 4;
  }
  else if (focus == 2) { // INSTRUMENT
    if (encVal < 0) encoder.setCount(0);
    if (encVal > (NUM_INSTRUMENTS * 4) - 1) encoder.setCount((NUM_INSTRUMENTS * 4) - 1);
    currentInstIndex = encoder.getCount() / 4;
  }

  // --- LOOPER PLAYBACK ---
  if (loopState == LOOP_PLAYING && !loopSequence.empty()) {
    unsigned long playTime = (currentMillis - loopPlaybackStart) % loopLength;
    static unsigned long lastPlayCheck = 0;
    
    // Handle wrap-around
    if (playTime < lastPlayCheck) lastPlayCheck = 0; 
    
    for (auto &e : loopSequence) {
      if (e.timestamp >= lastPlayCheck && e.timestamp < playTime) {
         noteOn(e.padIndex, false);
      }
    }
    lastPlayCheck = playTime;
  }

  // --- SENSORS ---
  sensors_event_t a, g, temp;
  if (mpu.getEvent(&a, &g, &temp)) {
     pitchBend = 1.0 + (a.acceleration.y / 15.0);
     int midiBend = 8192 + (int)(a.acceleration.y * 500); 
     if(midiBend<0) midiBend=0; if(midiBend>16383) midiBend=16383;
     uint8_t lsb = midiBend & 0x7F;
     uint8_t msb = (midiBend >> 7) & 0x7F;
     sendMidi(0xE0, lsb, msb);
  }

  uint16_t currTouched = touch.touched();
  for (int i = 0; i < 12; i++) {
    bool isTouched = (currTouched & _BV(i));
    int note = BASE_NOTE + scales[currentScaleIndex].offsets[i]; 
    if (isTouched && !padState[i]) { 
        padState[i] = true;
        noteOn(i, true); sendMidi(0x90, note, 127); 
    } 
    else if (!isTouched && padState[i]) { 
        padState[i] = false;
        sendMidi(0x80, note, 127); 
    }
  }

  // --- DISPLAY ---
  static unsigned long lastDraw = 0;
  if (millis() - lastDraw > 50) {
    display.clearDisplay();
    display.setCursor(0, 0);
    
    if(focus==0) display.print(">"); else display.print(" ");
    display.print("Vol"); display.print((int)(globalVolume*100));
    
    display.setCursor(64, 0);
    if(focus==1) display.print(">"); else display.print(" ");
    char sclName[10]; strncpy(sclName, scales[currentScaleIndex].name, 9); sclName[9]=0;
    display.print(sclName);

    display.setCursor(0, 11);
    if(focus==2) display.setTextColor(SSD1306_BLACK, SSD1306_WHITE);
    char insName[10]; strncpy(insName, instruments[currentInstIndex].name, 9); insName[9]=0;
    display.print(insName);
    display.setTextColor(SSD1306_WHITE);

    display.setCursor(64, 11);
    if(focus==3) display.print(">"); else display.print(" ");
    if(loopState == LOOP_IDLE) display.print("L:STOP");
    else if(loopState == LOOP_ARMED) display.print("L:RDY");
    else if(loopState == LOOP_RECORDING) display.print("L:REC");
    else display.print("L:PLAY");

    for(int i=0; i<12; i++) {
      int x = i * 10;
      if (padState[i]) display.fillRect(x, 22, 8, 10, SSD1306_WHITE); 
      else display.drawRect(x, 22, 8, 10, SSD1306_WHITE); 
    }
    display.display();
    lastDraw = millis();
  }
}