#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_RESET -1
#define SCREEN_ADDRESS 0x3C

#define TRIG_PIN 5
#define ECHO_PIN 18
#define MAX_DISTANCE_CM 450.0
#define LOOP_DELAY_MS 500

Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);

void setup() {
  Serial.begin(115200);
  pinMode(TRIG_PIN, OUTPUT);
  pinMode(ECHO_PIN, INPUT);

  if (!display.begin(SSD1306_SWITCHCAPVCC, SCREEN_ADDRESS)) {
    Serial.println("{\"readingState\":\"invalid\",\"message\":\"display_init_failed\"}");
    for (;;) {
    }
  }

  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  display.setTextSize(1);
  display.setCursor(0, 0);
  display.println("Water level ready");
  display.display();
}

void loop() {
  long duration = readDurationUs();
  float distanceCm = duration <= 0 ? -1.0 : (duration * 0.034f) / 2.0f;
  const char* readingState = "ok";

  if (duration <= 0 || distanceCm > MAX_DISTANCE_CM) {
    readingState = "out_of_range";
  } else if (distanceCm < 0) {
    readingState = "invalid";
  }

  writeSerialJson(distanceCm, duration, readingState);
  drawDisplay(distanceCm, readingState);
  delay(LOOP_DELAY_MS);
}

long readDurationUs() {
  digitalWrite(TRIG_PIN, LOW);
  delayMicroseconds(2);
  digitalWrite(TRIG_PIN, HIGH);
  delayMicroseconds(10);
  digitalWrite(TRIG_PIN, LOW);
  return pulseIn(ECHO_PIN, HIGH, 30000);
}

void writeSerialJson(float distanceCm, long durationUs, const char* readingState) {
  Serial.print("{\"distanceCm\":");
  if (distanceCm < 0) {
    Serial.print("null");
  } else {
    Serial.print(distanceCm, 1);
  }
  Serial.print(",\"rawDurationUs\":");
  Serial.print(durationUs);
  Serial.print(",\"readingState\":\"");
  Serial.print(readingState);
  Serial.println("\"}");
}

void drawDisplay(float distanceCm, const char* readingState) {
  display.clearDisplay();
  display.setTextSize(1);
  display.setCursor(0, 0);
  display.println("Water Level Sensor");
  display.drawLine(0, 10, 128, 10, SSD1306_WHITE);
  display.setTextSize(2);
  display.setCursor(0, 20);

  if (distanceCm < 0 || distanceCm > MAX_DISTANCE_CM) {
    display.println("No Read");
  } else {
    display.print(distanceCm, 1);
    display.println(" cm");
  }

  display.setTextSize(1);
  display.setCursor(0, 52);
  display.print("State: ");
  display.println(readingState);
  display.display();
}
