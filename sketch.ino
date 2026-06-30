/*
 * Smart Cat Feeder with MQTT & Scheduling
 * ESP32 + Servo Motor + Ultrasonic + Load Cell
 * 
 * Hardware:
 * - Servo Motor (Pin 18) - Untuk membuka dispenser makanan
 * - Ultrasonic HC-SR04 - Level makanan di container
 *   - Trig Pin: 5
 *   - Echo Pin: 19
 * - Load Cell HX711 - Mengukur berat makanan yang keluar
 *   - DOUT Pin: 16
 *   - SCK Pin: 17
 */

#include <WiFi.h>
#include <PubSubClient.h>
#include <ESP32Servo.h>
#include <HX711.h>
#include <ArduinoJson.h>

// ========== WiFi Configuration ==========
const char* ssid = "Wokwi-GUEST";
const char* password = "";

// ========== MQTT Configuration ==========
const char* mqtt_server = "broker.emqx.io";
const int mqtt_port = 1883;

// IMPORTANT: Ganti dengan prefix unik Anda!
const char* UNIQUE_PREFIX = "Kelompok2";  // ⚠️ HARUS SAMA dengan web dashboard

// MQTT Topics
char topic_foodlevel[50];
char topic_feed[50];
char topic_portion[50];
char topic_weight[50];
char topic_count[50];
char topic_totalweight[50];
char topic_schedule[50];
char topic_currenttime[50];

// Client ID
char client_id[50];

// ========== Hardware Pins ==========
const int SERVO_PIN = 18;
const int TRIG_PIN = 5;
const int ECHO_PIN = 19;
const int LOADCELL_DOUT_PIN = 16;
const int LOADCELL_SCK_PIN = 17;

// ========== Variables ==========
Servo servoMotor;
HX711 scale;

int foodLevel = 100;        // Food level percentage
int feedCount = 0;          // Daily feed count
int portionSize = 2;        // Default portion (1-5)
float lastFeedWeight = 0.0; // Weight of last feeding
float totalWeightToday = 0.0; // Total weight fed today

// Current time from web dashboard
String currentTime = "00:00";
String lastCheckedTime = "";

// Schedule storage
struct Schedule {
  String time;    // Format: "HH:MM"
  int portion;
};
Schedule schedules[10];  // Max 10 schedules
int scheduleCount = 0;

unsigned long lastPublish = 0;
const long publishInterval = 5000;  // Publish every 5 seconds

unsigned long lastFoodCheck = 0;
const long foodCheckInterval = 3000; // Check food level every 3 seconds

// Schedule check dilakukan oleh web dashboard, ESP32 hanya eksekusi

// ========== Objects ==========
WiFiClient espClient;
PubSubClient mqtt(espClient);

// ========== Setup ==========
void setup() {
  Serial.begin(115200);
  
  // Setup pins
  pinMode(TRIG_PIN, OUTPUT);
  pinMode(ECHO_PIN, INPUT);
  
  // Setup Servo
  servoMotor.attach(SERVO_PIN);
  servoMotor.write(0);  // Initial position (closed)
  
  // Setup Load Cell
  scale.begin(LOADCELL_DOUT_PIN, LOADCELL_SCK_PIN);
  scale.set_scale(420.0983);  // Calibration factor (adjust as needed)
  scale.tare();  // Reset scale to 0
  
  // Generate topics
  sprintf(topic_foodlevel, "%s/foodlevel", UNIQUE_PREFIX);
  sprintf(topic_feed, "%s/feed", UNIQUE_PREFIX);
  sprintf(topic_portion, "%s/portion", UNIQUE_PREFIX);
  sprintf(topic_weight, "%s/weight", UNIQUE_PREFIX);
  sprintf(topic_count, "%s/count", UNIQUE_PREFIX);
  sprintf(topic_totalweight, "%s/totalweight", UNIQUE_PREFIX);
  sprintf(topic_schedule, "%s/schedule", UNIQUE_PREFIX);
  sprintf(topic_currenttime, "%s/currenttime", UNIQUE_PREFIX);
  
  // Generate client ID
  sprintf(client_id, "esp32_%s_%d", UNIQUE_PREFIX, random(1000, 9999));
  
  Serial.println("=================================");
  Serial.println("🐱 Smart Cat Feeder with MQTT");
  Serial.println("=================================");
  Serial.printf("Prefix: %s\n", UNIQUE_PREFIX);
  Serial.printf("Client ID: %s\n", client_id);
  Serial.println();
  
  // Connect to WiFi
  connectWiFi();
  
  // Setup MQTT
  mqtt.setServer(mqtt_server, mqtt_port);
  mqtt.setCallback(mqttCallback);
  
  // Connect to MQTT
  connectMQTT();
  
  delay(1000);
}

// ========== Main Loop ==========
void loop() {
  // Check MQTT connection
  if (!mqtt.connected()) {
    connectMQTT();
  }
  mqtt.loop();
  
  unsigned long currentMillis = millis();
  
  // Check food level periodically
  if (currentMillis - lastFoodCheck >= foodCheckInterval) {
    checkFoodLevel();
    lastFoodCheck = currentMillis;
  }
  
  // Publish data periodically
  if (currentMillis - lastPublish >= publishInterval) {
    publishData();
    lastPublish = currentMillis;
  }
  
  delay(100);
}

// ========== WiFi Connection ==========
void connectWiFi() {
  Serial.print("Connecting to WiFi");
  WiFi.begin(ssid, password);
  
  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 20) {
    delay(500);
    Serial.print(".");
    attempts++;
  }
  
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println(" Connected!");
    Serial.print("IP Address: ");
    Serial.println(WiFi.localIP());
  } else {
    Serial.println(" Failed!");
  }
}

// ========== MQTT Connection ==========
void connectMQTT() {
  while (!mqtt.connected()) {
    Serial.print("Connecting to MQTT...");
    
    if (mqtt.connect(client_id)) {
      Serial.println(" Connected!");
      
      // Subscribe to control topics
      mqtt.subscribe(topic_feed);
      mqtt.subscribe(topic_portion);
      mqtt.subscribe(topic_schedule);
      mqtt.subscribe(topic_currenttime);
      
      Serial.printf("📡 Subscribed to: %s\n", topic_feed);
      Serial.printf("📡 Subscribed to: %s\n", topic_portion);
      Serial.printf("📡 Subscribed to: %s\n", topic_schedule);
      Serial.printf("📡 Subscribed to: %s\n", topic_currenttime);
      
    } else {
      Serial.print(" Failed, rc=");
      Serial.print(mqtt.state());
      Serial.println(" Retrying in 5 seconds...");
      delay(5000);
    }
  }
}

// ========== MQTT Callback ==========
void mqttCallback(char* topic, byte* payload, unsigned int length) {
  String message = "";
  for (int i = 0; i < length; i++) {
    message += (char)payload[i];
  }
  
  Serial.printf("📥 Received [%s]: %s\n", topic, message.c_str());
  
  // Handle feed command
  if (String(topic) == String(topic_feed)) {
    if (message == "1") {
      feedCat();
    }
  }
  
  // Handle portion size
  if (String(topic) == String(topic_portion)) {
    portionSize = message.toInt();
    if (portionSize < 1) portionSize = 1;
    if (portionSize > 5) portionSize = 5;
    Serial.printf("🍽️ Portion size set to: %d\n", portionSize);
  }

  // Handle schedule update
  if (String(topic) == String(topic_schedule)) {
    parseSchedules(message);
  }

  // Handle current time update from web dashboard
  if (String(topic) == String(topic_currenttime)) {
    currentTime = message;
    Serial.printf("🕐 Current time updated: %s\n", currentTime.c_str());
  }
}

// ========== Parse Schedules from JSON ==========
void parseSchedules(String jsonStr) {
  StaticJsonDocument<1024> doc;
  DeserializationError error = deserializeJson(doc, jsonStr);
  
  if (error) {
    Serial.print("❌ Failed to parse schedules: ");
    Serial.println(error.c_str());
    return;
  }
  
  scheduleCount = 0;
  JsonArray array = doc.as<JsonArray>();
  
  for (JsonObject obj : array) {
    if (scheduleCount >= 10) break;  // Max 10 schedules
    
    schedules[scheduleCount].time = obj["time"].as<String>();
    schedules[scheduleCount].portion = obj["portion"];
    scheduleCount++;
  }
  
  Serial.printf("📅 Loaded %d schedules\n", scheduleCount);
  printSchedules();
}

// ========== Print Schedules ==========
void printSchedules() {
  Serial.println("--- Feeding Schedules ---");
  for (int i = 0; i < scheduleCount; i++) {
    Serial.printf("  %d. %s - %d portions\n", i+1, schedules[i].time.c_str(), schedules[i].portion);
  }
  Serial.println("------------------------");
}

// ========== Check Food Level ==========
void checkFoodLevel() {
  // Trigger ultrasonic sensor
  digitalWrite(TRIG_PIN, LOW);
  delayMicroseconds(2);
  digitalWrite(TRIG_PIN, HIGH);
  delayMicroseconds(10);
  digitalWrite(TRIG_PIN, LOW);
  
  // Read echo
  long duration = pulseIn(ECHO_PIN, HIGH, 30000); // Timeout 30ms
  
  if (duration == 0) {
    // No echo received
    foodLevel = 0;
  } else {
    // Calculate distance in cm
    float distance = duration * 0.034 / 2;
    
    // Convert distance to percentage
    // Assuming: 5cm = full (100%), 30cm = empty (0%)
    if (distance <= 5) {
      foodLevel = 100;
    } else if (distance >= 30) {
      foodLevel = 0;
    } else {
      foodLevel = map((int)distance, 5, 30, 100, 0);
    }
  }
}

// ========== Feed Cat Function ==========
void feedCat() {
  Serial.println("🍽️ Feeding cat...");
  
  // Reset scale before feeding
  scale.tare();
  delay(500);
  
  // Open dispenser based on portion size
  for (int i = 0; i < portionSize; i++) {
    Serial.printf("   Portion %d/%d\n", i + 1, portionSize);
    
    // Open
    servoMotor.write(45);
    delay(1000);
    
    // Close
    servoMotor.write(0);
    delay(500);
  }
  
  // Wait for food to settle
  delay(1000);
  
  // Measure weight
  if (scale.is_ready()) {
    float weight = scale.get_units(10);  // Average of 10 readings
    
    // Convert to grams and ensure positive value
    lastFeedWeight = abs(weight);
    
    // Update total weight
    totalWeightToday += lastFeedWeight;
    
    Serial.printf("⚖️ Feed weight: %.1fg\n", lastFeedWeight);
    Serial.printf("🍽️ Total today: %.1fg\n", totalWeightToday);
  } else {
    Serial.println("⚠️ Load cell not ready");
    lastFeedWeight = 0;
  }
  
  // Update feed count
  feedCount++;
  
  Serial.printf("✅ Feeding complete! Total feedings today: %d\n", feedCount);
  
  // Publish updated data immediately
  publishData();
  
  // Reset scale for next feeding
  scale.tare();
}

// ========== Publish Data ==========
void publishData() {
  // Publish food level
  char foodStr[10];
  sprintf(foodStr, "%d", foodLevel);
  mqtt.publish(topic_foodlevel, foodStr);
  
  // Publish last feed weight
  char weightStr[20];
  dtostrf(lastFeedWeight, 6, 1, weightStr);
  mqtt.publish(topic_weight, weightStr);
  
  // Publish feed count
  char countStr[10];
  sprintf(countStr, "%d", feedCount);
  mqtt.publish(topic_count, countStr);
  
  // Publish total weight today
  char totalStr[20];
  dtostrf(totalWeightToday, 6, 1, totalStr);
  mqtt.publish(topic_totalweight, totalStr);
  
  Serial.println("--- System Status ---");
  Serial.printf("Current Time: %s\n", currentTime.c_str());
  Serial.printf("Food Level: %d%%\n", foodLevel);
  Serial.printf("Last Feed Weight: %.1fg\n", lastFeedWeight);
  Serial.printf("Feed Count: %d\n", feedCount);
  Serial.printf("Total Weight: %.1fg\n", totalWeightToday);
  Serial.printf("Portion Size: %d\n", portionSize);
  Serial.printf("Active Schedules: %d\n", scheduleCount);
  Serial.println("--------------------");
  Serial.println();
}
