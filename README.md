graph TD
    ESP32[ESP32 Controller]

    %% I2C Bus
    ESP32 -- GPIO 21/22 --> OLED[OLED Display]
    ESP32 -- GPIO 21/22 --> MPR[MPR121 Touch]
    ESP32 -- GPIO 21/22 --> MPU[MPU6050 Gyro]

    %% I2S Audio
    ESP32 -- GPIO 25/26/27 --> DAC[PCM5102 DAC]
    DAC --> Spk[Speaker/Jack]

    %% Encoder
    ESP32 -- GPIO 32/33 --> Enc[Rotary Encoder A/B]
    ESP32 -- GPIO 19 --> Sw[Encoder Switch]
    Enc -- GND --> Ground
    Sw -- GND --> Ground
