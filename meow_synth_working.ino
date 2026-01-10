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

// --- HARDWARE CONFIG ---
#define I2S_DOUT      25
#define I2S_BCLK      27
#define I2S_LRC       26
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
#define NUM_VOICES    14
#define BASE_NOTE     48
#define PI_2          6.2831853f

// --- DATA STRUCTURES ---

// 1. SCALES
struct Scale { const char* name; int offsets[12]; };
#define NUM_SCALES 4
Scale scales[NUM_SCALES] = {
  {"Kurd (D)",     {0, 2, 3, 5, 7, 8, 10, 12, 14, 15, 17, 19}},
  {"Celtic",       {0, 2, 3, 7, 9, 12, 14, 15, 19, 21, 24, 26}},
  {"Integral",     {0, 2, 3, 7, 10, 12, 14, 15, 19, 22, 24, 26}},
  {"Penta Maj",    {0, 2, 4, 7, 9, 12, 14, 16, 19, 21, 24, 26}}
};

// 2. INSTRUMENTS
struct Instrument { 
  const char* name; 
  int engineType;     // 0=Bell, 1=Bass, 2=Glock, 3=Lofi, 4=Band
  int octaveShift;    
  float decay;        
};
#define NUM_INSTRUMENTS 5
Instrument instruments[NUM_INSTRUMENTS] = {
  {"Lofi Keys", 3,  0, 0.9992f},
  {"Band Kit",  4,  0, 0.9900f},
  {"Handpan",   0,  0, 0.9994f},  
  {"Bass Gtr",  1, -24, 0.9997f}, 
  {"Glock",     2, 24, 0.9996f}   
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
  float lfoPhase; 
};
Voice voices[NUM_VOICES];

// --- STATE VARIABLES ---
int currentScaleIndex = 0;
int currentInstIndex = 0; 
float globalVolume = 1.0; 
float pitchBend = 1.0; 
bool padState[12] = {0};

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

// --- CAT ANIMATION DRAWING ---
void drawCatFrame(int x) {
  display.clearDisplay();
  // Body
  display.fillRect(x, 15, 20, 10, SSD1306_WHITE);
  // Head
  display.fillCircle(x+20, 12, 8, SSD1306_WHITE);
  // Ears
  display.fillTriangle(x+14, 6, x+18, 0, x+22, 6, SSD1306_WHITE);
  display.fillTriangle(x+26, 6, x+30, 0, x+24, 6, SSD1306_WHITE);
  // Tail
  display.drawLine(x, 15, x-5, 5, SSD1306_WHITE);
  // Legs
  if ((x / 4) % 2 == 0) { // Simple walking animation
     display.drawLine(x+2, 25, x+2, 30, SSD1306_WHITE);
     display.drawLine(x+18, 25, x+18, 30, SSD1306_WHITE);
  } else {
     display.drawLine(x+5, 25, x+5, 30, SSD1306_WHITE);
     display.drawLine(x+15, 25, x+15, 30, SSD1306_WHITE);
  }
  display.display();
}

// --- BOOT SOUND: MEOW ---
void playMeow() {
  // Use a free voice to synthesize a "Meow"
  // A Meow is a Sawtooth wave that slides pitch Up then Down
  int v = 0;
  voices[v].active = true;
  voices[v].frequency = 400.0f; // Start pitch
  voices[v].phase = 0;
  voices[v].env.level = 1.0f;
  voices[v].env.decayMultiplier = 0.9995f; // Long sustain
  voices[v].engineType = 3; // Use Lofi engine for wobbly texture
  
  // Slide pitch up quickly
  for(int i=0; i<50; i++) {
    voices[v].frequency += 10.0f; 
    delay(5);
  }
  // Slide pitch down slowly
  for(int i=0; i<100; i++) {
    voices[v].frequency -= 5.0f; 
    delay(5);
  }
  voices[v].active = false;
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
    .dma_buf_count = 4,   
    .dma_buf_len = 256,   
    .use_apll = true      
  };
  i2s_pin_config_t pin_config = {
    .bck_io_num = I2S_BCLK, .ws_io_num = I2S_LRC, .data_out_num = I2S_DOUT, .data_in_num = I2S_PIN_NO_CHANGE
  };
  i2s_driver_install(I2S_NUM_0, &i2s_config, 0, NULL);
  i2s_set_pin(I2S_NUM_0, &pin_config);
}

// --- AUDIO TASK ---
void audioTask(void *pvParameters) {
  int32_t sampleBuffer[128]; 
  size_t bytes_written;
  
  while(1) {
    for (int i = 0; i < 64; i++) { 
      float mixedSample = 0;
      int activeCount = 0;

      for (int v = 0; v < NUM_VOICES; v++) {
        if (voices[v].active) {
          float sample = 0;
          int type = voices[v].engineType;

          if (type == 3) { // Lofi / Meow
             float wobble = sinf(voices[v].lfoPhase) * 0.005f; voices[v].lfoPhase += 0.001f;
             float t = voices[v].phase * 2.0f; if (t > 1.0f) t = 2.0f - t;
             sample = ((t * 2.0f) - 1.0f) * voices[v].env.level;
             voices[v].phase += ((voices[v].frequency * (1.0f + wobble)) / SAMPLE_RATE) * pitchBend;
          }
          else if (type == 4) { // Band Kit
             int drum = voices[v].noteIndex % 4; 
             if (drum == 0) { // Kick
                sample = sinf(voices[v].phase * PI_2) * voices[v].env.level;
                voices[v].currentFreq *= 0.9985f; if(voices[v].currentFreq<40) voices[v].currentFreq=40;
                voices[v].phase += (voices[v].currentFreq / SAMPLE_RATE);
             } else { // Snare/Hat
                float noise = ((float)rand() / RAND_MAX) * 2.0f - 1.0f;
                sample = noise * voices[v].env.level;
             }
          }
          else if (type == 0 || type == 2) { // Bell/Glock
             sample = sinf(voices[v].phase * PI_2) * voices[v].env.level;
             voices[v].phase += (voices[v].frequency / SAMPLE_RATE) * pitchBend;
          }
          else if (type == 1) { // Bass
             float raw = (voices[v].phase * 2.0f) - 1.0f;
             voices[v].prevSample = (voices[v].env.level * raw) + ((1.0f-voices[v].env.level)*voices[v].prevSample);
             sample = voices[v].prevSample;
             voices[v].phase += (voices[v].frequency / SAMPLE_RATE) * pitchBend;
          }

          if (voices[v].phase >= 1.0f) voices[v].phase -= 1.0f;
          voices[v].env.level *= voices[v].env.decayMultiplier;
          if (voices[v].env.level < 0.0001f) voices[v].active = false;
          
          mixedSample += sample;
          activeCount++;
        }
      }

      if (activeCount > 0) {
        float gain = 1.3f / sqrt((float)activeCount + 1.0f);
        mixedSample = tanhf(mixedSample * gain * globalVolume);
      }
      int32_t out = (int32_t)(mixedSample * 2000000000.0f);
      sampleBuffer[i*2] = out; sampleBuffer[i*2+1] = out; 
    }
    i2s_write(I2S_NUM_0, sampleBuffer, sizeof(sampleBuffer), &bytes_written, portMAX_DELAY);
  }
}

void noteOn(int padIndex, bool isLive) {
  // LOOPER LOGIC
  if (isLive && loopState == LOOP_RECORDING) {
     LoopEvent e; e.timestamp = millis() - loopStartTime; e.padIndex = padIndex; e.isNoteOn = true; loopSequence.push_back(e);
  } else if (isLive && loopState == LOOP_ARMED) {
     loopState = LOOP_RECORDING; loopStartTime = millis(); loopSequence.clear();
     LoopEvent e; e.timestamp = 0; e.padIndex = padIndex; e.isNoteOn = true; loopSequence.push_back(e);
  }

  // AUDIO LOGIC
  int v = 0;
  for (int i = 0; i < NUM_VOICES; i++) { if (!voices[i].active) { v = i; break; } }
  
  // REMIX LOGIC: Use CURRENT instrument settings for ALL notes (even loops)
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
  voices[v].lfoPhase = 0;
  
  if (engine == 1) voices[v].prevSample = 0; 
  if (engine == 4) { // Band Kit
     int drum = padIndex % 4;
     if (drum == 0) { voices[v].currentFreq = 200.0f; voices[v].env.decayMultiplier = 0.9985f; } // Kick
     else if (drum == 1) { voices[v].env.decayMultiplier = 0.9970f; } // Snare
     else if (drum == 2) { voices[v].env.decayMultiplier = 0.9700f; } // Hat
     else { voices[v].currentFreq = 150.0f; voices[v].env.decayMultiplier = 0.9980f; } // Tom
  }
}

// --- SETUP ---
void setup() {
  Serial.begin(115200);
  delay(1000); 
  Wire.begin(SDA_PIN, SCL_PIN);
  
  // 1. OLED Init
  if(!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) { Serial.println("OLED Fail"); }
  
  // 2. BOOT ANIMATION: SLIDING CAT
  for(int x = -30; x < 128; x+=4) {
    drawCatFrame(x);
    // delay(10) is omitted to keep it smooth/fast, display i2c is the bottleneck anyway
  }
  
  // 3. TITLE SCREEN
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(25, 12);
  display.print("THE MEOW SYNTH");
  display.display();
  
  setupI2S();
  // We need to start the audio task BEFORE playing sound
  xTaskCreatePinnedToCore(audioTask, "AudioTask", 10000, NULL, 1, NULL, 0);
  
  delay(200);
  playMeow(); // Play boot sound
  delay(800);

  // 4. Hardware Init
  if (!touch.begin(0x5A)) { Serial.println("MPR121 Fail"); }
  touch.writeRegister(0x7B, 0x00); touch.writeRegister(0x5E, 0x00); 
  touch.setThresholds(6, 3);       touch.writeRegister(0x5E, 0x8F); 

  if (mpu.begin()) {
    mpu.setAccelerometerRange(MPU6050_RANGE_8_G);
    mpu.setFilterBandwidth(MPU6050_BAND_21_HZ);
  }

  ESP32Encoder::useInternalWeakPullResistors = puType::up;
  encoder.attachHalfQuad(ENC_DT, ENC_CLK);
  encoder.setCount(90); 
  pinMode(ENC_SW, INPUT_PULLUP);
  loopSequence.reserve(200); 
}

// --- MAIN LOOP ---
void loop() {
  unsigned long currentMillis = millis();
  int btnState = digitalRead(ENC_SW);

  if (btnState == LOW && buttonDownTime == 0) { buttonDownTime = currentMillis; buttonHeld = false; }
  
  // Long press for Looper
  if (btnState == LOW && (currentMillis - buttonDownTime > 800) && !buttonHeld) {
    buttonHeld = true; 
    if (focus == 3) { 
      if (loopState == LOOP_IDLE) loopState = LOOP_ARMED;
      else if (loopState == LOOP_ARMED || loopState == LOOP_RECORDING) {
         loopState = LOOP_PLAYING; loopLength = millis() - loopStartTime; loopPlaybackStart = millis();
      }
      else if (loopState == LOOP_PLAYING) { loopState = LOOP_IDLE; loopSequence.clear(); }
    }
  }

  // Short press to navigate
  if (btnState == HIGH && buttonDownTime > 0) {
    if (!buttonHeld && (currentMillis - buttonDownTime > 50)) {
       focus++; if (focus > 3) focus = 0;
       if (focus == 0) encoder.setCount((int)(globalVolume * 100));
       if (focus == 1) encoder.setCount(currentScaleIndex * 4);
       if (focus == 2) encoder.setCount(currentInstIndex * 4);
    }
    buttonDownTime = 0; 
  }

  long encVal = encoder.getCount();
  if (focus == 0) { 
    if (encVal < 0) encoder.setCount(0); if (encVal > 100) encoder.setCount(100);
    globalVolume = (float)encoder.getCount() / 100.0f;
  } 
  else if (focus == 1) { 
    if (encVal < 0) encoder.setCount(0); if (encVal > (NUM_SCALES * 4) - 1) encoder.setCount((NUM_SCALES * 4) - 1);
    currentScaleIndex = encoder.getCount() / 4;
  }
  else if (focus == 2) { 
    if (encVal < 0) encoder.setCount(0); if (encVal > (NUM_INSTRUMENTS * 4) - 1) encoder.setCount((NUM_INSTRUMENTS * 4) - 1);
    currentInstIndex = encoder.getCount() / 4;
  }

  // --- LOOPER PLAYBACK (WITH REMIX CAPABILITY) ---
  if (loopState == LOOP_PLAYING && !loopSequence.empty()) {
    unsigned long playTime = (currentMillis - loopPlaybackStart) % loopLength;
    static unsigned long lastPlayCheck = 0;
    for (auto &e : loopSequence) {
      if ((playTime >= lastPlayCheck && e.timestamp >= lastPlayCheck && e.timestamp < playTime) ||
          (playTime < lastPlayCheck && (e.timestamp >= lastPlayCheck || e.timestamp < playTime))) {
         // This triggers noteOn using the CURRENT selected instrument (currentInstIndex)
         // effectively "Remixing" the old sequence with the new sound.
         noteOn(e.padIndex, false); 
      }
    }
    lastPlayCheck = playTime;
  }

  sensors_event_t a, g, temp;
  if (mpu.getEvent(&a, &g, &temp)) { pitchBend = 1.0 + (a.acceleration.y / 15.0); }

  uint16_t currTouched = touch.touched();
  for (int i = 0; i < 12; i++) {
    bool isTouched = (currTouched & _BV(i));
    if (isTouched && !padState[i]) { padState[i] = true; noteOn(i, true); } 
    else if (!isTouched && padState[i]) { padState[i] = false; }
  }

  static unsigned long lastDraw = 0;
  if (millis() - lastDraw > 50) {
    display.clearDisplay();
    display.setCursor(0, 0);
    if(focus==0) display.print(">"); else display.print(" ");
    display.print("Vol"); display.print((int)(globalVolume*100));
    
    display.setCursor(64, 0);
    if(focus==1) display.print(">"); else display.print(" ");
    char sclName[6]; strncpy(sclName, scales[currentScaleIndex].name, 5); sclName[5]=0;
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
