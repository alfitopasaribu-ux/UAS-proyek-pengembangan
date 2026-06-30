#include <WiFi.h>
#include <PubSubClient.h>
#include <ESP32Servo.h>
#include <HX711.h>
#include <ArduinoJson.h>

// ========== WiFi Configuration ==========
const char* ssid = "Yuistecu";
const char* password = "12345678";

// ========== MQTT Configuration ==========
const char* mqtt_server = "broker.emqx.io";
const int mqtt_port = 1883;

// IMPORTANT: Ganti dengan prefix unik Anda!
const char* UNIQUE_PREFIX = "Kelompok2";  // ⚠️ HARUS SAMA dengan web dashboard

// MQTT Topics
char topic_foodlevel[50];
char topic_feed[50];
char topic_portion[50];
char topic_bowlweight[50];      // Berat makanan di wadah (gram)
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
const int LOADCELL_DOUT_PIN = 4;
const int LOADCELL_SCK_PIN = 2;

// ========== Ultrasonic Distance Mapping ==========
// Jarak sensor di wadah makanan panjangnya 20cm
const float DISTANCE_FULL = 5.0;    // 5cm = wadah penuh (100%)
const float DISTANCE_EMPTY = 20.0;  // 20cm = wadah kosong (0%)

// ========== Variables ==========
Servo servoMotor;
HX711 scale;

int foodLevel = 100;              // Food level percentage
int feedCount = 0;                // Daily feed count
int portionSize = 2;              // Default portion (1-5)
float bowlWeight = 0.0;           // Berat makanan di wadah (gram)
float previousBowlWeight = 0.0;   // Berat sebelumnya untuk deteksi perubahan
float totalWeightToday = 0.0;     // Total berat yang dimakan hari ini

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
const long publishInterval = 3000;  // Publish every 3 seconds

unsigned long lastFoodCheck = 0;
const long foodCheckInterval = 3000; // Check food level every 3 seconds

unsigned long lastWeightCheck = 0;
const long weightCheckInterval = 2000; // Check bowl weight every 2 seconds

// ========== Objects ==========
WiFiClient espClient;
PubSubClient mqtt(espClient);

// ========== Function Declarations ==========
void testUltrasonicContinuous();

// ========== Setup ==========
void setup() {
  Serial.begin(115200);
  
  // IMPORTANT: Add delay to ensure Serial Monitor is ready
  delay(2000);
  
  Serial.println("\n\n");
  Serial.println("=================================");
  Serial.println("🐱 Smart Cat Feeder with MQTT");
  Serial.println("   Bowl Weight Monitor Edition");
  Serial.println("   Ultrasonic Range: 5-20cm");
  Serial.println("   🔧 ENHANCED DEBUG VERSION");
  Serial.println("=================================");
  Serial.printf("Prefix: %s\n", UNIQUE_PREFIX);
  Serial.println();
  
  // Setup pins
  pinMode(TRIG_PIN, OUTPUT);
  pinMode(ECHO_PIN, INPUT);
  Serial.println("✓ Pins configured");
  
  // Setup Servo
  servoMotor.attach(SERVO_PIN);
  servoMotor.write(0);  // Initial position (closed)
  Serial.println("✓ Servo motor initialized");
  
  // Setup Load Cell for Bowl Weight Monitoring
  Serial.println("Initializing Load Cell for Bowl Weight...");
  Serial.printf("  DOUT Pin: GPIO %d\n", LOADCELL_DOUT_PIN);
  Serial.printf("  SCK Pin: GPIO %d\n", LOADCELL_SCK_PIN);
  Serial.println("  Purpose: Monitor food weight in the bowl");
  
  scale.begin(LOADCELL_DOUT_PIN, LOADCELL_SCK_PIN);
  scale.set_scale(420.0983);  // Calibration factor (adjust as needed)
  
  Serial.println("⚠️ Please remove all weight from the scale...");
  delay(2000);
  scale.tare();  // Reset scale to 0
  Serial.println("✓ Load Cell initialized and calibrated");
  Serial.println("✓ You can now place the food bowl on the scale");
  
  // Generate topics
  sprintf(topic_foodlevel, "%s/foodlevel", UNIQUE_PREFIX);
  sprintf(topic_feed, "%s/feed", UNIQUE_PREFIX);
  sprintf(topic_portion, "%s/portion", UNIQUE_PREFIX);
  sprintf(topic_bowlweight, "%s/bowlweight", UNIQUE_PREFIX);  // Bowl weight topic
  sprintf(topic_count, "%s/count", UNIQUE_PREFIX);
  sprintf(topic_totalweight, "%s/totalweight", UNIQUE_PREFIX);
  sprintf(topic_schedule, "%s/schedule", UNIQUE_PREFIX);
  sprintf(topic_currenttime, "%s/currenttime", UNIQUE_PREFIX);
  
  // Generate client ID
  sprintf(client_id, "esp32_%s_%d", UNIQUE_PREFIX, random(1000, 9999));
  Serial.printf("Client ID: %s\n", client_id);
  Serial.println();
  
  // Connect to WiFi
  connectWiFi();
  
  // Setup MQTT
  mqtt.setServer(mqtt_server, mqtt_port);
  mqtt.setCallback(mqttCallback);
  
  // Connect to MQTT
  connectMQTT();
  
  Serial.println("\n✅ Setup complete! System ready.\n");
  
  // Display ultrasonic mapping info
  Serial.println("📏 Ultrasonic Distance Mapping:");
  Serial.printf("   %.1fcm = 100%% FULL\n", DISTANCE_FULL);
  Serial.printf("   %.1fcm = 0%% EMPTY\n", DISTANCE_EMPTY);
  Serial.println();
  
  // ========== ENHANCED TEST MODE ==========
  Serial.println("🧪 Testing Ultrasonic Sensor with DETAILED DEBUG...");
  Serial.println("   This will help identify why it's stuck at 100%");
  delay(1000);
  testUltrasonicContinuous();
  
  Serial.println("\n⚠️ IMPORTANT CHECKS:");
  Serial.println("1. Is ECHO pin using voltage divider? (MUST for 5V→3.3V)");
  Serial.println("2. Is sensor facing DOWN into container?");
  Serial.println("3. Is distance to food surface 5-20cm?");
  Serial.println("4. Is there anything blocking the sensor?");
  Serial.println();
  
  delay(2000);
}

// ========== Main Loop ==========
void loop() {
  // Check MQTT connection
  if (!mqtt.connected()) {
    Serial.println("⚠️ MQTT disconnected, reconnecting...");
    connectMQTT();
  }
  mqtt.loop();
  
  unsigned long currentMillis = millis();
  
  // Check food level periodically
  if (currentMillis - lastFoodCheck >= foodCheckInterval) {
    checkFoodLevel();
    lastFoodCheck = currentMillis;
  }
  
  // Check bowl weight periodically
  if (currentMillis - lastWeightCheck >= weightCheckInterval) {
    checkBowlWeight();
    lastWeightCheck = currentMillis;
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
  Serial.print("Connecting to WiFi: ");
  Serial.println(ssid);
  Serial.print("Password: ");
  Serial.println(password);
  
  WiFi.begin(ssid, password);
  
  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 30) {
    delay(500);
    Serial.print(".");
    attempts++;
    
    if (attempts % 10 == 0) {
      Serial.println();
      Serial.printf("  Attempt %d/30...\n", attempts);
    }
  }
  
  Serial.println();
  
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("✅ WiFi Connected!");
    Serial.print("   IP Address: ");
    Serial.println(WiFi.localIP());
    Serial.print("   Signal Strength: ");
    Serial.print(WiFi.RSSI());
    Serial.println(" dBm");
  } else {
    Serial.println("❌ WiFi Connection Failed!");
    Serial.println("   Possible issues:");
    Serial.println("   - Wrong password");
    Serial.println("   - WiFi not in range");
    Serial.println("   - WiFi is 5GHz (ESP32 only supports 2.4GHz)");
    Serial.println("\n   System will continue without WiFi...");
  }
  Serial.println();
}

// ========== MQTT Connection ==========
void connectMQTT() {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("⚠️ Cannot connect to MQTT: WiFi not connected");
    return;
  }
  
  int attempts = 0;
  while (!mqtt.connected() && attempts < 3) {
    Serial.print("Connecting to MQTT broker: ");
    Serial.print(mqtt_server);
    Serial.print(":");
    Serial.print(mqtt_port);
    Serial.print("...");
    
    if (mqtt.connect(client_id)) {
      Serial.println(" ✅ Connected!");
      
      // Subscribe to control topics
      mqtt.subscribe(topic_feed);
      mqtt.subscribe(topic_portion);
      mqtt.subscribe(topic_schedule);
      mqtt.subscribe(topic_currenttime);
      
      Serial.println("\n📡 Subscribed to topics:");
      Serial.printf("   - %s\n", topic_feed);
      Serial.printf("   - %s\n", topic_portion);
      Serial.printf("   - %s\n", topic_schedule);
      Serial.printf("   - %s\n", topic_currenttime);
      Serial.println();
      
    } else {
      Serial.print(" ❌ Failed, rc=");
      Serial.println(mqtt.state());
      Serial.println("   Retrying in 5 seconds...");
      attempts++;
      delay(5000);
    }
  }
  
  if (!mqtt.connected()) {
    Serial.println("⚠️ Could not connect to MQTT after 3 attempts");
    Serial.println("   System will work offline (no remote control)\n");
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

// ========== ENHANCED Check Food Level with DETAILED DEBUG ==========
void checkFoodLevel() {
  // Ensure trigger is LOW before starting
  digitalWrite(TRIG_PIN, LOW);
  delayMicroseconds(5);
  
  // Send 10us pulse to trigger
  digitalWrite(TRIG_PIN, HIGH);
  delayMicroseconds(10);
  digitalWrite(TRIG_PIN, LOW);
  
  // Read echo with timeout (15ms = 15000us, enough for ~25cm max range)
  long duration = pulseIn(ECHO_PIN, HIGH, 15000);
  
  // ========== DETAILED DEBUG OUTPUT ==========
  Serial.println("\n========== ULTRASONIC DEBUG ==========");
  Serial.printf("Raw Duration: %ld μs\n", duration);
  
  if (duration == 0) {
    // No echo received - sensor issue
    Serial.println("❌ ERROR: No echo received!");
    Serial.println("\n🔧 TROUBLESHOOTING CHECKLIST:");
    Serial.println("1. Check Wiring:");
    Serial.println("   VCC  → 5V (NOT 3.3V!)");
    Serial.println("   GND  → GND");
    Serial.println("   TRIG → GPIO 5");
    Serial.println("   ECHO → GPIO 19");
    Serial.println("\n2. ⚠️ CRITICAL: Voltage Divider for ECHO pin:");
    Serial.println("   HC-SR04 outputs 5V but ESP32 GPIO is 3.3V max!");
    Serial.println("   Without voltage divider, sensor may not work properly.");
    Serial.println("   ");
    Serial.println("   Wiring diagram:");
    Serial.println("   ECHO(5V) ──[1kΩ]──┬── ESP32 GPIO19");
    Serial.println("                      │");
    Serial.println("                   [2kΩ]");
    Serial.println("                      │");
    Serial.println("                     GND");
    Serial.println("\n3. Check sensor orientation:");
    Serial.println("   - Sensor facing DOWN into container");
    Serial.println("   - No obstacles blocking ultrasonic waves");
    Serial.println("   - Target distance should be 5-20cm");
    Serial.println("======================================\n");
    
    foodLevel = 0; // Set to 0 on error
    return;
  }
  
  // Calculate distance in cm
  float distance = duration * 0.034 / 2;
  
  Serial.printf("Calculated Distance: %.2f cm\n", distance);
  
  // Validate distance range (HC-SR04 reliable range: 2-400cm)
  if (distance < 2) {
    Serial.println("⚠️ WARNING: Distance TOO CLOSE (< 2cm)");
    Serial.println("   Possible causes:");
    Serial.println("   1. Container is OVERFILLED (food level above sensor range)");
    Serial.println("   2. Sensor mounted TOO LOW in container");
    Serial.println("   3. Object blocking sensor (lid, plastic, etc)");
    Serial.println("   ");
    Serial.println("   🔧 SOLUTION:");
    Serial.println("   - Remove some food to lower the level");
    Serial.println("   - OR mount sensor higher (min 5cm from max food level)");
    Serial.println("   - Check for obstacles in sensor path");
    
    foodLevel = 100; // Assume full if too close
    Serial.printf("Food Level: %d%% (MAXED OUT - TOO CLOSE)\n", foodLevel);
    Serial.println("======================================\n");
    return;
  }
  
  if (distance > 25) {
    Serial.println("⚠️ WARNING: Distance out of range (> 25cm)");
    Serial.println("   Possible causes:");
    Serial.println("   - Container is EMPTY or nearly empty");
    Serial.println("   - Sensor mounted too high");
    Serial.println("   - Poor echo reflection from food surface");
    
    foodLevel = 0;
    Serial.printf("Food Level: %d%% (EMPTY OR OUT OF RANGE)\n", foodLevel);
    Serial.println("======================================\n");
    return;
  }
  
  // Convert distance to percentage
  // Container mapping: 5cm = full (100%), 20cm = empty (0%)
  int previousLevel = foodLevel;
  
  Serial.printf("Mapping: %.1fcm (FULL) ← %.2fcm → %.1fcm (EMPTY)\n", 
                DISTANCE_FULL, distance, DISTANCE_EMPTY);
  
  if (distance <= DISTANCE_FULL) {
    foodLevel = 100;
    Serial.printf("✅ Status: FULL (distance %.2fcm ≤ %.1fcm)\n", distance, DISTANCE_FULL);
    Serial.println("   ⚠️ If this is wrong, the food level is too high!");
    Serial.println("   Consider mounting sensor higher or removing food.");
  } else if (distance >= DISTANCE_EMPTY) {
    foodLevel = 0;
    Serial.printf("⚠️ Status: EMPTY (distance %.2fcm ≥ %.1fcm)\n", distance, DISTANCE_EMPTY);
  } else {
    // Map distance 5-20cm to percentage 100-0%
    // Use float for more accurate mapping
    foodLevel = map((int)(distance * 10), 
                    (int)(DISTANCE_FULL * 10), 
                    (int)(DISTANCE_EMPTY * 10), 
                    100, 0);
    
    Serial.printf("📊 Status: %d%% ", foodLevel);
    
    // Show change indicator
    if (foodLevel < previousLevel) {
      Serial.printf("(↓ -%d%%) - Food decreasing\n", previousLevel - foodLevel);
    } else if (foodLevel > previousLevel) {
      Serial.printf("(↑ +%d%%) - Food increasing\n", foodLevel - previousLevel);
    } else {
      Serial.println("(━ no change)");
    }
  }
  
  Serial.printf("Final Food Level: %d%%\n", foodLevel);
  Serial.println("======================================\n");
}

// ========== NEW: Continuous Test Function ==========
void testUltrasonicContinuous() {
  Serial.println("\n🧪 CONTINUOUS ULTRASONIC TEST MODE");
  Serial.println("Reading 10 samples to identify the problem...\n");
  
  int stuckAt100Count = 0;
  int zeroEchoCount = 0;
  float totalDistance = 0;
  int validReadings = 0;
  
  for (int i = 0; i < 10; i++) {
    digitalWrite(TRIG_PIN, LOW);
    delayMicroseconds(5);
    digitalWrite(TRIG_PIN, HIGH);
    delayMicroseconds(10);
    digitalWrite(TRIG_PIN, LOW);
    
    long duration = pulseIn(ECHO_PIN, HIGH, 15000);
    float distance = duration * 0.034 / 2;
    
    Serial.printf("Sample %2d: Duration=%6ld μs | Distance=%6.2f cm", 
                  i + 1, duration, distance);
    
    if (duration == 0) {
      Serial.println(" ❌ NO ECHO - WIRING PROBLEM!");
      zeroEchoCount++;
    } else if (distance < 2) {
      Serial.println(" ⚠️ TOO CLOSE - THIS IS WHY IT'S STUCK AT 100%!");
      stuckAt100Count++;
    } else if (distance > 25) {
      Serial.println(" ⚠️ TOO FAR");
    } else {
      Serial.println(" ✓ VALID");
      totalDistance += distance;
      validReadings++;
    }
    
    delay(300);
  }
  
  Serial.println("\n========== TEST RESULTS ==========");
  
  if (zeroEchoCount > 5) {
    Serial.println("❌ PROBLEM IDENTIFIED: NO ECHO SIGNAL");
    Serial.println("   → Check wiring, especially ECHO pin!");
    Serial.println("   → Verify voltage divider is installed");
  } else if (stuckAt100Count > 5) {
    Serial.println("❌ PROBLEM IDENTIFIED: DISTANCE TOO CLOSE");
    Serial.println("   → Sensor reading < 2cm consistently");
    Serial.println("   → Food level is TOO HIGH or sensor TOO LOW");
    Serial.println("\n   🔧 SOLUTIONS:");
    Serial.println("   1. Remove some food from container");
    Serial.println("   2. Mount sensor at least 5cm above max food level");
    Serial.println("   3. Check for obstacles blocking sensor");
  } else if (validReadings > 0) {
    float avgDistance = totalDistance / validReadings;
    Serial.printf("✅ Sensor working! Average distance: %.2f cm\n", avgDistance);
    Serial.printf("   This equals approximately %d%% food level\n", 
                  map((int)(avgDistance * 10), 50, 200, 100, 0));
  }
  
  Serial.println("==================================\n");
}

// ========== Check Bowl Weight ==========
void checkBowlWeight() {
  if (scale.is_ready()) {
    // Read weight from load cell (average of 5 readings for stability)
    float weight = scale.get_units(5);
    
    // Convert to positive grams
    bowlWeight = abs(weight);
    
    // Detect if cat is eating (weight decreased significantly)
    float weightDifference = previousBowlWeight - bowlWeight;
    if (weightDifference > 5.0 && previousBowlWeight > 0) {
      // Cat ate some food
      totalWeightToday += weightDifference;
      Serial.printf("🐱 Cat is eating! Consumed: %.1fg\n", weightDifference);
    }
    
    previousBowlWeight = bowlWeight;
  } else {
    Serial.println("⚠️ Load cell not ready");
  }
}

// ========== Feed Cat Function ==========
void feedCat() {
  Serial.println("\n🍽️ ========== FEEDING CAT ==========");
  
  float weightBefore = bowlWeight;
  
  // Open dispenser based on portion size
  for (int i = 0; i < portionSize; i++) {
    Serial.printf("   Dispensing portion %d/%d\n", i + 1, portionSize);
    
    // Open
    servoMotor.write(45);
    delay(700);
    
    // Close
    servoMotor.write(0);
    delay(500);
  }
  
  // Update feed count
  feedCount++;
  
  Serial.printf("✅ Feeding complete! Total feedings today: %d\n", feedCount);
  
  // Wait for food to settle in bowl
  delay(2000);
  checkBowlWeight();
  
  float weightAdded = bowlWeight - weightBefore;
  Serial.printf("   ➕ Food added to bowl: %.1fg\n", weightAdded);
  Serial.printf("   🍽️ Current bowl weight: %.1fg\n", bowlWeight);
  Serial.println("=====================================\n");
  
  // Publish updated data immediately
  publishData();
}

// ========== Publish Data ==========
void publishData() {
  if (!mqtt.connected()) {
    return;
  }
  
  // Publish food level
  char foodStr[10];
  sprintf(foodStr, "%d", foodLevel);
  mqtt.publish(topic_foodlevel, foodStr);
  
  // Publish bowl weight (makanan di wadah)
  char bowlWeightStr[20];
  dtostrf(bowlWeight, 6, 1, bowlWeightStr);
  mqtt.publish(topic_bowlweight, bowlWeightStr);
  
  // Publish feed count
  char countStr[10];
  sprintf(countStr, "%d", feedCount);
  mqtt.publish(topic_count, countStr);
  
  // Publish total weight consumed today
  char totalStr[20];
  dtostrf(totalWeightToday, 6, 1, totalStr);
  mqtt.publish(topic_totalweight, totalStr);
  
  Serial.println("--- System Status ---");
  Serial.printf("Current Time: %s\n", currentTime.c_str());
  Serial.printf("Food Level (Container): %d%%\n", foodLevel);
  Serial.printf("Bowl Weight: %.1fg\n", bowlWeight);
  Serial.printf("Feed Count: %d\n", feedCount);
  Serial.printf("Total Consumed Today: %.1fg\n", totalWeightToday);
  Serial.printf("Portion Size: %d\n", portionSize);
  Serial.printf("Active Schedules: %d\n", scheduleCount);
  Serial.println("--------------------");
  Serial.println();
}
