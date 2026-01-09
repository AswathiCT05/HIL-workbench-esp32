#include <Arduino.h>
#include "driver/twai.h"
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <DHT.h>

/* ================== PIN CONFIG ================== */
#define CAN_TX GPIO_NUM_5
#define CAN_RX GPIO_NUM_4

#define DHTPIN 15
#define DHTTYPE DHT11

#define TRIG_PIN 18
#define ECHO_PIN 19  //Voltage divider needed if Echo pin voltage is 5V 

#define OLED_SDA 21
#define OLED_SCL 22

#define REVERSE_LED_PIN 25

/* ================== OBJECTS ================== */
DHT dht(DHTPIN, DHTTYPE);
Adafruit_SSD1306 display(128, 64, &Wire, -1);

/* ================== ECU STATE ================== */
bool reverseActive = false;   // controlled via CAN (HIL stimulus)
bool forcedFault   = false;

float temperature = 0;
float humidity    = 0;
uint16_t distanceCm = 0;
uint8_t faultFlags  = 0;

/* ================== FUNCTIONS ================== */
uint16_t readDistance() {
  digitalWrite(TRIG_PIN, LOW);
  delayMicroseconds(5);
  digitalWrite(TRIG_PIN, HIGH);
  delayMicroseconds(10);
  digitalWrite(TRIG_PIN, LOW);

  long duration = pulseIn(ECHO_PIN, HIGH, 40000); // ~4 m max
  if (duration == 0) return 0;

  return duration / 58; // cm
}

/* ================== SETUP ================== */
void setup() {
  Serial.begin(115200);
  delay(1000);

  pinMode(TRIG_PIN, OUTPUT);
  pinMode(ECHO_PIN, INPUT);
  pinMode(REVERSE_LED_PIN, OUTPUT);
  digitalWrite(REVERSE_LED_PIN, LOW);

  dht.begin();

  Wire.begin(OLED_SDA, OLED_SCL);
  display.begin(SSD1306_SWITCHCAPVCC, 0x3C);
  display.clearDisplay();
  display.display();

  twai_general_config_t g_config =
      TWAI_GENERAL_CONFIG_DEFAULT(CAN_TX, CAN_RX, TWAI_MODE_NORMAL);
  twai_timing_config_t t_config = TWAI_TIMING_CONFIG_500KBITS();
  twai_filter_config_t f_config = TWAI_FILTER_CONFIG_ACCEPT_ALL();

  twai_driver_install(&g_config, &t_config, &f_config);
  twai_start();

  Serial.println("ESP32 CAN ECU STARTED (HIL-style)");
}

/* ================== LOOP ================== */
void loop() {
  /* ---------- RECEIVE CAN COMMANDS (HIL INPUT) ---------- */
  twai_message_t rx;
  while (twai_receive(&rx, 0) == ESP_OK) {
    if (rx.identifier == 0x200 && rx.data_length_code >= 2) {
      // Command 0x01 = Reverse / Active mode
      if (rx.data[0] == 0x01) {
        reverseActive = rx.data[1];
      }
      // Command 0x02 = Forced fault
      if (rx.data[0] == 0x02) {
        forcedFault = rx.data[1];
      }
    }
  }

  /* ---------- LED INDICATION ---------- */
  digitalWrite(REVERSE_LED_PIN, reverseActive ? HIGH : LOW);

  /* ---------- READ SENSORS ---------- */
  if (reverseActive) {
    temperature = dht.readTemperature();
    humidity    = dht.readHumidity();
    distanceCm  = readDistance();
  }

  /* ---------- FAULT LOGIC ---------- */
  faultFlags = 0;
  if (reverseActive && isnan(temperature)) faultFlags |= (1 << 0);
  if (reverseActive && distanceCm == 0)     faultFlags |= (1 << 1);
  if (temperature >= 90)                    faultFlags |= (1 << 2);
  if (reverseActive && distanceCm <= 30)    faultFlags |= (1 << 3);
  if (forcedFault)                          faultFlags |= (1 << 7);

  /* ---------- SEND CAN STATUS (ECU → HIL) ---------- */
  twai_message_t tx{};
  tx.identifier = 0x100;
  tx.extd = 0;
  tx.rtr  = 0;
  tx.data_length_code = 8;

  tx.data[0] = 50;                         // demo speed
  tx.data[1] = (int8_t)temperature;
  tx.data[2] = (uint8_t)humidity;
  tx.data[3] = faultFlags;
  tx.data[4] = reverseActive;
  tx.data[5] = forcedFault;
  tx.data[6] = 0;
  tx.data[7] = 0;

  twai_transmit(&tx, pdMS_TO_TICKS(50));

  /* ---------- OLED HMI ---------- */
  display.clearDisplay();
  display.setTextColor(WHITE);

  if (!reverseActive) {
    // ===== NORMAL ECU MODE =====
    display.setTextSize(2);
    display.setCursor(0, 0);
    display.println("ECU STATUS");

    display.setTextSize(1);
    display.print("Temp: ");
    display.print(temperature);
    display.println(" C");

    display.print("Hum : ");
    display.print(humidity);
    display.println(" %");

    display.println("MODE: NORMAL");
  }
  else {
    // ===== REVERSE / PARKING ASSIST MODE =====
    const char* parkStatus = "SAFE";
    if (distanceCm > 0 && distanceCm <= 30) {
      parkStatus = "CAUTION";
    } else if (distanceCm <= 100) {
      parkStatus = "NEAR";
    }

    display.setTextSize(1);
    display.setCursor(0, 0);
    display.println("REVERSE ASSIST");

    display.setTextSize(2);
    display.print(distanceCm);
    display.println("cm");

    display.setTextSize(1);
    display.print("Status: ");
    display.println(parkStatus);

    if (strcmp(parkStatus, "CAUTION") == 0) {
      display.setCursor(0, 56);
      display.println("STOP!");
    }
  }

  display.display();
  delay(500);
}
