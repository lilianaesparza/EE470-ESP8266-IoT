
/*
 * Project: EE470 ESP8266 IoT System
 * File: main.cpp
 * Author: Liliana Esparza
 * Date: 11/11/2026
 * Version: 3.3
 *
 * Purpose:
 * Read temperature and light sensor measurements.
 * Send data to Hostinger when a switch activates.
 * Retrieve timestamps using timeapi.io.
 * Retry unsuccessful transmissions every 30 seconds.
 *
 * Inputs:
 * DS18B20 - D2
 * Photoresistor - A0
 * Pushbutton - D7
 * Tilt Switch - D6
 * Serial Monitor - Time zone selection
 *
 * Outputs:
 * Sensor readings, timestamps, WiFi information,
 * and HTTP server responses.
 *
 * Dependencies:
 * OneWire, DallasTemperature, ESP8266 Arduino.
 *
 * Usage Notes:
 * Default time zone is Pacific.
 * HTTPS certificate verification disabled for testing.
 */

#include <Arduino.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include <ESP8266WiFi.h>
#include <ESP8266HTTPClient.h>
#include <WiFiClientSecure.h>

// WiFi settings
const char* WIFI_SSID = "YOUR_WIFI";
const char* WIFI_PASSWORD = "YOUR_WIFI_PASSWORD";

// Hostinger database endpoint

const char* SERVER_URL =
    "https://snow-squirrel-383242.hostingersite.com"
    "/EE470_Database/sensor_insert.php";


// Hardware pins
const uint8_t TEMP_PIN = D2;
const uint8_t LIGHT_PIN = A0;
const uint8_t BUTTON_PIN = D7;
const uint8_t TILT_PIN = D6;

const unsigned long DEBOUNCE_MS = 50;
const unsigned long RETRY_MS = 30000;

// Temperature sensor
OneWire oneWire(TEMP_PIN);
DallasTemperature temperatureSensor(&oneWire);

// Switch state
struct SwitchState {
    bool lastReading;
    bool stableState;
    unsigned long lastChangeTime;
};

SwitchState buttonState = {HIGH, HIGH, 0};
SwitchState tiltState = {HIGH, HIGH, 0};

// Time zones
String selectedTimeZone = "America/Los_Angeles";

const char* timeZones[] = {
    "America/New_York",
    "America/Chicago",
    "America/Denver",
    "America/Los_Angeles",
    "America/Anchorage",
    "Pacific/Honolulu",
    "America/Puerto_Rico"
};

// Pending measurement
struct Measurement {
    String node;
    String timestamp;
    float temperature;
    int light;
    unsigned long lastAttempt;
    bool pending;
};

Measurement pending = {"", "", 0, 0, 0, false};

// Select time zone
void selectTimeZone() {
    Serial.println("\nSelect your time zone:");
    Serial.println("1 Eastern  2 Central  3 Mountain");
    Serial.println("4 Pacific  5 Alaska   6 Hawaii");
    Serial.println("7 Atlantic");
    Serial.println("Default: Pacific (4)");
    Serial.println("Enter 1-7 within 15 seconds.");

    unsigned long start = millis();

    while (millis() - start < 15000) {
        if (Serial.available()) {
            char choice = Serial.read();

            if (choice >= '1' && choice <= '7') {
                selectedTimeZone = timeZones[choice - '1'];
            }

            break;
        }
        delay(10);
    }

    Serial.print("Selected time zone: ");
    Serial.println(selectedTimeZone);
}

// Connect to WiFi with manually configured DNS
void connectToWiFi() {
    Serial.print("\nConnecting to WiFi: ");
    Serial.println(WIFI_SSID);

    WiFi.mode(WIFI_STA);
    WiFi.persistent(false);
    WiFi.setAutoReconnect(true);

    // Windows hotspot network
    IPAddress localIP(192, 168, 137, 97);
    IPAddress gateway(192, 168, 137, 1);
    IPAddress subnet(255, 255, 255, 0);

    // Public DNS servers
    IPAddress dns1(8, 8, 8, 8);
    IPAddress dns2(1, 1, 1, 1);

    if (!WiFi.config(localIP, gateway, subnet, dns1, dns2)) {
        Serial.println("WiFi configuration failed.");
        return;
    }

    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

    unsigned long start = millis();

    while (WiFi.status() != WL_CONNECTED &&
           millis() - start < 30000) {
        delay(500);
        Serial.print(".");
    }

    Serial.println();

    if (WiFi.status() == WL_CONNECTED) {
        Serial.println("WiFi Connected!");

        Serial.print("IP Address: ");
        Serial.println(WiFi.localIP());

        Serial.print("Gateway: ");
        Serial.println(WiFi.gatewayIP());

        Serial.print("DNS Server: ");
        Serial.println(WiFi.dnsIP());

        Serial.print("MAC Address: ");
        Serial.println(WiFi.macAddress());

        Serial.print("RSSI: ");
        Serial.print(WiFi.RSSI());
        Serial.println(" dBm");
    } else {
        Serial.println("WiFi connection failed.");
        Serial.print("Status: ");
        Serial.println(WiFi.status());
    }
}

// Detect one event per switch activation
bool wasActivated(uint8_t pin, SwitchState& state) {
    bool reading = digitalRead(pin);

    if (reading != state.lastReading) {
        state.lastReading = reading;
        state.lastChangeTime = millis();
    }

    if (millis() - state.lastChangeTime >= DEBOUNCE_MS &&
        reading != state.stableState) {
        state.stableState = reading;
        return state.stableState == LOW;
    }

    return false;
}

// Get timestamp from timeapi.io
String getCurrentTimestamp() {
    if (WiFi.status() != WL_CONNECTED) {
        Serial.println("WiFi disconnected.");
        return "";
    }

    IPAddress apiIP;

    if (!WiFi.hostByName("timeapi.io", apiIP)) {
        Serial.println("DNS lookup failed.");
        return "";
    }

    Serial.print("Time API IP: ");
    Serial.println(apiIP);

    WiFiClientSecure client;
    client.setInsecure();

    HTTPClient http;
    http.setTimeout(10000);

    String url =
        "https://timeapi.io/api/Time/current/zone?timeZone=" +
        selectedTimeZone;

    if (!http.begin(client, url)) {
        Serial.println("Time API setup failed.");
        return "";
    }

    int code = http.GET();

    if (code != HTTP_CODE_OK) {
        Serial.print("Time API HTTP Error: ");
        Serial.println(code);

        Serial.print("Error Details: ");
        Serial.println(http.errorToString(code));

        http.end();
        return "";
    }

    String body = http.getString();
    http.end();

    int key = body.indexOf("\"dateTime\"");
    int colon = body.indexOf(':', key);
    int firstQuote = body.indexOf('"', colon + 1);
    int lastQuote = body.indexOf('"', firstQuote + 1);

    if (key < 0 || colon < 0 ||
        firstQuote < 0 || lastQuote < 0) {
        Serial.println("Timestamp parsing failed.");
        return "";
    }

    String timestamp =
        body.substring(firstQuote + 1, lastQuote);

    timestamp.replace('T', ' ');

    if (timestamp.length() > 19) {
        timestamp = timestamp.substring(0, 19);
    }

    if (timestamp.length() != 19) {
        Serial.println("Invalid timestamp.");
        return "";
    }

    return timestamp;
}

// Read sensors when a switch activates
void captureMeasurement(const char* node) {
    Serial.println();
    Serial.print(node);
    Serial.println(" activated.");

    if (pending.pending) {
        Serial.println("Previous measurement pending.");
        return;
    }

    String timestamp = getCurrentTimestamp();

    if (timestamp.length() == 0) {
        Serial.println("No timestamp; cannot send.");
        return;
    }

    temperatureSensor.requestTemperatures();
    float temp = temperatureSensor.getTempCByIndex(0);
    int light = analogRead(LIGHT_PIN);

    if (temp == DEVICE_DISCONNECTED_C ||
        isnan(temp) || temp < -10 || temp > 100) {
        Serial.println("Temperature reading failed.");
        return;
    }

    pending.node = node;
    pending.timestamp = timestamp;
    pending.temperature = temp;
    pending.light = light;
    pending.pending = true;
    pending.lastAttempt = millis() - RETRY_MS;

    Serial.print("Timestamp: ");
    Serial.println(timestamp);

    Serial.print("Temperature: ");
    Serial.print(temp);
    Serial.println(" C");

    Serial.print("Light: ");
    Serial.println(light);
}

// Send measurement to Hostinger
void transmitData() {
    if (!pending.pending) return;

    pending.lastAttempt = millis();

    if (WiFi.status() != WL_CONNECTED) {
        Serial.println("WiFi disconnected. Retry later.");
        return;
    }

    WiFiClientSecure client;
    client.setInsecure();

    HTTPClient http;
    http.setTimeout(10000);

    String url = String(SERVER_URL) +
        "?nodeId=" + pending.node +
        "&nodeTemp=" + String(pending.temperature, 2) +
        "&nodeLight=" + String(pending.light) +
        "&timeReceived=" + pending.timestamp;

    url.replace(" ", "%20");

    if (!http.begin(client, url)) {
        Serial.println("Hostinger HTTP setup failed.");
        return;
    }

    int code = http.GET();
    String response = http.getString();

    if (code < 0) {
        Serial.print("Hostinger connection error: ");
        Serial.println(http.errorToString(code));
    }

    http.end();

    Serial.print("HTTP Code: ");
    Serial.println(code);

    Serial.print("Server: ");
    Serial.println(response);

    if (code == 200 &&
        (response.indexOf("SUCCESS: DATA_STORED") >= 0 ||
         response.indexOf("SUCCESS: ALREADY_STORED") >= 0)) {
        Serial.println("Delivery confirmed!");
        pending.pending = false;
    } else {
        Serial.println("Retry in 30 seconds.");
    }
}

// Setup
void setup() {
    Serial.begin(115200);

    pinMode(BUTTON_PIN, INPUT_PULLUP);
    pinMode(TILT_PIN, INPUT_PULLUP);
    temperatureSensor.begin();

    buttonState.lastReading = digitalRead(BUTTON_PIN);
    buttonState.stableState = buttonState.lastReading;

    tiltState.lastReading = digitalRead(TILT_PIN);
    tiltState.stableState = tiltState.lastReading;

    Serial.println("\nEE470 ESP8266 Sensor Project");

    selectTimeZone();
    connectToWiFi();

    Serial.println("\nSystem Ready!");
    Serial.println("Press button or activate tilt switch.");
}

// Main loop
void loop() {
    if (wasActivated(BUTTON_PIN, buttonState)) {
        captureMeasurement("node_1");
    }

    if (wasActivated(TILT_PIN, tiltState)) {
        captureMeasurement("node_2");
    }

    if (pending.pending &&
        millis() - pending.lastAttempt >= RETRY_MS) {
        transmitData();
    }

    if (WiFi.status() != WL_CONNECTED) {
        static unsigned long lastReconnect = 0;

        if (millis() - lastReconnect >= 10000) {
            lastReconnect = millis();
            WiFi.reconnect();
        }
    }

    delay(5);
}
