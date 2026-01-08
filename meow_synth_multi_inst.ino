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

// -------------------------------------------------------------------------
// 1. HARDWARE & PIN DEFINITIONS
// -------------------------------------------------------------------------
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
#define SCREEN_HEIGHT 64  // Updated to 64 to fit Animation + UI comfortably
#define OLED_RESET    -1

// Audio Settings
#define SAMPLE_RATE   44100
#define NUM_VOICES    10  // Optimized for stability
#define BASE_NOTE     48  // C3
#define PI_2          6.2831853f

// -------------------------------------------------------------------------
// 2. GRAPHICS (THE MEOW BITMAP) [cite: 3]
// -------------------------------------------------------------------------
const unsigned char PROGMEM catBitmap[] = {
  0x00, 0x00, 0x0C, 0x30, 0x1E, 0x78, 0x33, 0xCC, 
  0x3F, 0xFC, 0x7F, 0xFE, 0x7F, 0xFE, 0x7F, 0xFE, 
  0x7F, 0xFE, 0x3F, 0xFC, 0x3F, 0xFC, 0x13, 0xC8, 
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
};

// Animation State
int catX = -16; 
int catY = 0; // Draw cat at the top

// -------------------------------------------------------------------------
// 3. MUSIC THEORY (SCALES & INSTRUMENTS)
// -------------------------------------------------------------------------
struct Scale { const char* name; int offsets[12]; };

#define NUM_SCALES 7
Scale scales[NUM_SCALES] = {
  {"Celtic",     {0, 2, 3, 7, 9, 12, 14, 15, 19, 21, 24, 26}}, // Hexatonic Minor
  {"Kurd",       {0, 2, 3, 5, 7, 8, 10, 12, 14, 15, 17, 19}},  // Phrygian
  {"Hijaz",      {0, 1, 4, 5, 7, 8, 10, 12, 13, 16, 17, 19}},  // Phrygian Dominant
  {"Pygmy",      {0, 2, 3, 7, 10, 12, 14, 15, 19, 22, 24, 26}},// Dorian-esque
  {"Pentatonic", {0, 2, 4, 7, 9, 12, 14, 16, 19, 21, 24, 26}}, // Major Pentatonic
  {"Lydian",     {0, 2, 4, 6, 7, 9, 11, 12, 14, 16, 18, 19}},  // Major #4
  {"Mixolydian", {0, 2, 4, 5, 7, 9, 10, 12, 14, 16, 17, 19}}   // Major b7
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
  {"Glock",     2, 24, 0.9996f}, // High pitch, sine based
  {"Bass Gtr",  1, -24, 0.9997f}, // Saw based, filtered
  {"Drum Kit",  4,  0, 0.9900f}, // Percussive
  {"E-Piano",   3,  0, 0.9990f}  // Square based
};

// -------------------------------------------------------------------------
// 4. GLOBAL OBJECTS & STATE
// -------------------------------------------------------------------------
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);
Adafruit_MPR121 touch = Adafruit_MPR121();
Adafruit_MPU6050 mpu;
ESP32Encoder encoder;

// Audio Engine Vars
struct Envelope { float level; float decayMultiplier; };
struct Voice { 
  bool active;
  float frequency;    
  float currentFreq; // For drums (pitch slide)
  float phase; 
  float phaseMod; 
  Envelope env; 
  int noteIndex; 
  float prevSample;   
  int engineType;
};
Voice voices[NUM_VOICES];

// Control State
int currentScaleIndex = 0;
int currentInstIndex = 0;
float globalVolume = 1.0; 
float pitchBend = 1.0;
bool padState[12] = {0};

// Menu Focus: 0=Vol, 1=Scale, 2=Inst, 3=Loop
int focus = 0; 
unsigned long buttonDownTime = 0;
bool buttonHeld = false;

// Looper State
struct LoopEvent { unsigned long timestamp; byte padIndex; bool isNoteOn; };
enum LooperState { LOOP_IDLE, LOOP_ARMED, LOOP_RECORDING, LOOP_PLAYING };
LooperState loopState = LOOP_IDLE;
std::vector<LoopEvent> loopSequence;
unsigned long loopStartTime = 0;
unsigned long loopLength = 0;
unsigned long loopPlaybackStart = 0;

// -------------------------------------------------------------------------
// 5. AUDIO TASK (High Priority, Core 0)
// -------------------------------------------------------------------------
void audioTask(void *pvParameters) {
  // Large buffer for stability
  int32_t sampleBuffer[256];
  size_t bytes_written;

  while(1) {
    // Process 128 stereo frames (256 samples total) per loop
    for (int i = 0; i < 128; i++) { 
      float mixedSample = 0;
      int activeCount = 0;

      for (int v = 0; v < NUM_VOICES; v++) {
        if (!voices[v].active) continue;

        float sample = 0;
        int type = voices[v].engineType;

        // -- ENGINE 0: Handpan (FM Synthesis) --
        if (type == 0) {
           float mod = sinf(voices[v].phaseMod * PI_2) * 2.5f;
           sample = sinf((voices[v].phase * PI_2) + (mod * voices[v].env.level)) * voices[v].env.level;
           voices[v].phaseMod += ((voices[v].frequency * 2.0f) / SAMPLE_RATE) * pitchBend;
           if (voices[v].phaseMod >= 1.0f) voices[v].phaseMod -= 1.0f;
        }
        // -- ENGINE 1: Bass (Filtered Saw) --
        else if (type == 1) {
           float rawSaw = (voices[v].phase * 2.0f) - 1.0f;
           float filterCutoff = voices[v].env.level; 
           voices[v].prevSample = (filterCutoff * rawSaw) + ((1.0f - filterCutoff) * voices[v].prevSample);
           // Simple Soft Clip
           if(voices[v].prevSample > 1.2f) voices[v].prevSample = 1.2f;
           else if(voices[v].prevSample < -1.2f) voices[v].prevSample = -1.2f;
           sample = voices[v].prevSample;
        }
        // -- ENGINE 2: Glockenspiel (Pure Sine) --
        else if (type == 2) {
           sample = sinf(voices[v].phase * PI_2) * voices[v].env.level;
        }
        // -- ENGINE 3: E-Piano (Filtered Square) --
        else if (type == 3) {
           float rawSquare = (voices[v].phase < 0.5f) ? 0.8f : -0.8f;
           voices[v].prevSample = (0.2f * rawSquare) + (0.8f * voices[v].prevSample);
           sample = voices[v].prevSample * voices[v].env.level;
        }
        // -- ENGINE 4: Drum Kit --
        else if (type == 4) {
           int drum = voices[v].noteIndex % 4; // Map pads to different drum sounds
           
           if (drum == 0) { // Kick
              sample = sinf(voices[v].phase * PI_2) * voices[v].env.level;
              voices[v].currentFreq *= 0.9990f; 
              if(voices[v].currentFreq < 50) voices[v].currentFreq = 50;
              voices[v].phase += (voices[v].currentFreq / SAMPLE_RATE);
              goto skipGlobalPhase; // Kick has its own phase logic
           } 
           else if (drum == 1) { // Snare (Noise)
              float noise = ((float)rand() / RAND_MAX) * 2.0f - 1.0f;
              sample = noise * voices[v].env.level;
           } 
           else { // Hat (High Pass Noise)
              float noise = ((float)rand() / RAND_MAX) * 2.0f - 1.0f;
              float hp = noise - voices[v].prevSample; 
              voices[v].prevSample = noise;
              sample = hp * voices[v].env.level;
           }
        }

        // Global Phase Increment (Standard)
        voices[v].phase += (voices[v].frequency / SAMPLE_RATE) * pitchBend;
        
        skipGlobalPhase:
        if (voices[v].phase >= 1.0f) voices[v].phase -= 1.0f;
        
        // Envelope Decay
        voices[v].env.level *= voices[v].env.decayMultiplier;
        if (voices[v].env.level < 0.0001f) voices[v].active = false;

        mixedSample += sample;
        activeCount++;
      }

      // Mixing & Limiting (Fast Soft Clipper) [cite: 52]
      if (activeCount > 0) {
        float gain = 1.0f / sqrt((float)activeCount + 1.0f);
        mixedSample *= (gain * globalVolume);
        if (mixedSample > 0.95f) mixedSample = 0.95f;
        else if (mixedSample < -0.95f) mixedSample = -0.95f;
      }

      // Output to Stereo Buffer (32-bit)
      int32_t out = (int32_t)(mixedSample * 2147483647.0f);
      sampleBuffer[i*2] = out; 
      sampleBuffer[i*2+1] = out;
    }
    i2s_write(I2S_NUM_0, sampleBuffer, sizeof(sampleBuffer), &bytes_written, portMAX_DELAY);
  }
}

// -------------------------------------------------------------------------
// 6. SETUP I2S [cite: 57]
// -------------------------------------------------------------------------
void setupI2S() {
  i2s_config_t i2s_config = {
    .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX),
    .sample_rate = SAMPLE_RATE,
    .bits_per_sample = I2S_BITS_PER_SAMPLE_32BIT, 
    .channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT,
    .communication_format = I2S_COMM_FORMAT_I2S,
    .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
    .dma_buf_count = 8,   
    .dma_buf_len = 512,   
    .use_apll = true      // Critical for high quality audio
  };
  i2s_pin_config_t pin_config = {
    .bck_io_num = I2S_BCLK, .ws_io_num = I2S_LRC, .data_out_num = I2S_DOUT, .data_in_num = I2S_PIN_NO_CHANGE
  };
  i2s_driver_install(I2S_NUM_0, &i2s_config, 0, NULL);
  i2s_set_pin(I2S_NUM_0, &pin_config);
}

// -------------------------------------------------------------------------
// 7. NOTE LOGIC
// -------------------------------------------------------------------------
void noteOn(int padIndex, bool isLive) {
  // Looper Recording Logic [cite: 67]
  if (isLive && loopState == LOOP_RECORDING) {
     LoopEvent e;
     e.timestamp = millis() - loopStartTime; e.padIndex = padIndex; e.isNoteOn = true; loopSequence.push_back(e);
  } else if (isLive && loopState == LOOP_ARMED) {
     loopState = LOOP_RECORDING; loopStartTime = millis(); loopSequence.clear();
     LoopEvent e; e.timestamp = 0; e.padIndex = padIndex; e.isNoteOn = true; loopSequence.push_back(e);
  }

  // Find free voice
  int v = 0;
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
  
  if (engine == 1) voices[v].prevSample = 0; // Reset bass filter
  if (engine == 4) { // Drums
     voices[v].currentFreq = 300.0f; // Kick start freq
     if ((padIndex % 4) >= 2) voices[v].env.decayMultiplier = 0.9600f; // Short decay for hats
  }
}

// -------------------------------------------------------------------------
// 8. SETUP
// -------------------------------------------------------------------------
void setup() {
  Serial.begin(115200);
  delay(500);
  Wire.begin(SDA_PIN, SCL_PIN);
  
  // OLED Init
  if(!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
    Serial.println(F("SSD1306 allocation failed"));
  }
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  display.setTextSize(1);
  display.setCursor(20,20);
  display.println("MEOW SYNTH");
  display.display();
  delay(1000);

  // Touch Init
  if (!touch.begin(0x5A)) { Serial.println("MPR121 Error"); }
  touch.setThresholds(8, 4); // Slightly less sensitive to prevent ghosts

  // MPU6050 Init (Accelerometer)
  if (mpu.begin()) {
    mpu.setAccelerometerRange(MPU6050_RANGE_8_G);
    mpu.setFilterBandwidth(MPU6050_BAND_21_HZ);
  }

  // Encoder Init
  ESP32Encoder::useInternalWeakPullResistors = puType::up;
  encoder.attachHalfQuad(ENC_DT, ENC_CLK);
  encoder.setCount(100); 
  pinMode(ENC_SW, INPUT_PULLUP);

  loopSequence.reserve(200);
  setupI2S();
  
  // Start Audio Engine on Core 0
  xTaskCreatePinnedToCore(audioTask, "AudioTask", 10000, NULL, 1, NULL, 0);
}

// -------------------------------------------------------------------------
// 9. MAIN LOOP (UI & Logic)
// -------------------------------------------------------------------------
void loop() {
  unsigned long currentMillis = millis();
  
  // --- BUTTON LOGIC (Encoder Switch) ---
  int btnReading = digitalRead(ENC_SW);
  static int lastBtnState = HIGH;
  static unsigned long lastDebounceTime = 0;
  
  if (btnReading != lastBtnState) lastDebounceTime = currentMillis;
  
  if ((currentMillis - lastDebounceTime) > 50) {
      if (btnReading == LOW && buttonDownTime == 0) { 
        buttonDownTime = currentMillis;
        buttonHeld = false; 
      }
      
      // Long press (>800ms) -> Toggle Looper
      if (btnReading == LOW && (currentMillis - buttonDownTime > 800) && !buttonHeld) {
        buttonHeld = true;
        if (loopState == LOOP_IDLE) loopState = LOOP_ARMED;
        else if (loopState == LOOP_ARMED || loopState == LOOP_RECORDING) {
             loopState = LOOP_PLAYING;
             loopLength = millis() - loopStartTime; loopPlaybackStart = millis();
        }
        else if (loopState == LOOP_PLAYING) { 
             loopState = LOOP_IDLE;
             loopSequence.clear(); 
        }
      }
    
      // Short press -> Navigation
      if (btnReading == HIGH && buttonDownTime > 0) {
        if (!buttonHeld) {
           focus++;
           if (focus > 2) focus = 0; // 0=Vol, 1=Scale, 2=Inst
           // Snap encoder to current value
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
  if (focus == 0) { // VOLUME
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
    
    if (playTime < lastPlayCheck) lastPlayCheck = 0; // Wrap around
    for (auto &e : loopSequence) {
      if (e.timestamp >= lastPlayCheck && e.timestamp < playTime) {
         noteOn(e.padIndex, false);
      }
    }
    lastPlayCheck = playTime;
  }

  // --- SENSORS (Pitch Bend) ---
  sensors_event_t a, g, temp;
  if (mpu.getEvent(&a, &g, &temp)) {
     // Tilt Y-axis for slight pitch bending
     pitchBend = 1.0 + (a.acceleration.y / 20.0); 
  }

  // --- TOUCH INPUT ---
  uint16_t currTouched = touch.touched();
  for (int i = 0; i < 12; i++) {
    bool isTouched = (currTouched & _BV(i));
    if (isTouched && !padState[i]) { 
        padState[i] = true;
        noteOn(i, true); 
    } 
    else if (!isTouched && padState[i]) { 
        padState[i] = false;
    }
  }

  // --- DISPLAY & ANIMATION (Every 50ms) ---
  static unsigned long lastDraw = 0;
  if (millis() - lastDraw > 50) {
    display.clearDisplay();
    
    // 1. Draw UI Info (Bottom Half)
    display.setCursor(0, 20);
    if(focus==0) display.print(">"); else display.print(" ");
    display.print("Vol:"); display.print((int)(globalVolume*100));

    display.setCursor(64, 20);
    if(focus==1) display.print(">"); else display.print(" ");
    char sclName[12]; strncpy(sclName, scales[currentScaleIndex].name, 11); sclName[11]=0;
    display.print(sclName);

    display.setCursor(0, 32);
    if(focus==2) display.setTextColor(SSD1306_BLACK, SSD1306_WHITE); // Invert highlight
    else display.setTextColor(SSD1306_WHITE);
    char insName[12]; strncpy(insName, instruments[currentInstIndex].name, 11); insName[11]=0;
    display.print(insName);
    display.setTextColor(SSD1306_WHITE);

    display.setCursor(64, 32);
    if(loopState == LOOP_IDLE) display.print("L:STOP");
    else if(loopState == LOOP_ARMED) display.print("L:RDY");
    else if(loopState == LOOP_RECORDING) display.print("L:REC");
    else display.print("L:PLAY");

    // Draw Pad Indicators
    for(int i=0; i<12; i++) {
      int x = i * 10;
      int y = 50;
      if (padState[i]) display.fillRect(x, y, 8, 10, SSD1306_WHITE); 
      else display.drawRect(x, y, 8, 10, SSD1306_WHITE); 
    }

    // 2. Draw Animation (Top Half) [cite: 8, 9, 10]
    display.drawBitmap(catX, catY, catBitmap, 16, 16, SSD1306_WHITE);
    
    // Move Cat
    catX += 2;
    if (catX > SCREEN_WIDTH) catX = -16;

    display.display();
    lastDraw = millis();
  }
}