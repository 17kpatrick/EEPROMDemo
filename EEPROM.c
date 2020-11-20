#include <Arduino.h>
#include <EEPROM.h>
#include <ESP8266WiFi.h>
#include "Adafruit_MQTT.h"
#include "Adafruit_MQTT_Client.h"
#include <Adafruit_Sensor.h>
#include <DHT.h>
#include <DHT_U.h>
#include <Wire.h>
#include <BH1750.h>

/***************************
 * WIFI Settings
 **************************/
WiFiClient client;
const char *WLAN_SSID = "************"; //Enter Wi-Fi SSID
const char *WLAN_PASS = "************"; //Enter Wi-Fi Password
const int MAX_WIFI_CONNECTION_TIME = 10;
bool isInternetConnected = false;

/************************* Adafruit.io Setup *********************************/

#define AIO_SERVER      "io.adafruit.com"
#define AIO_SERVERPORT  1883                   // use 8883 for SSL
#define AIO_USERNAME    "*****" //Enter Adafruit Username
#define AIO_KEY         "*****" //Enter Adafruit Key
#define FEED_PATH AIO_USERNAME "/*****" //Enter Adafruit Feed
#define MQTT_RETRIES 3

Adafruit_MQTT_Client mqtt(&client, AIO_SERVER, AIO_SERVERPORT, AIO_USERNAME, AIO_KEY);
Adafruit_MQTT_Publish feed = Adafruit_MQTT_Publish(&mqtt, FEED_PATH);
Adafruit_MQTT_Subscribe subscribe = Adafruit_MQTT_Subscribe(&mqtt, FEED_PATH);

/************************* Local Storage Setup *********************************/

int currentAddress;
const bool ERASE_DATA_ON_RESTART = false;
const int STORAGE_SIZE = 512;
const int MAX_NUMBER_OF_SAVED_WEATHER_DATA_STRINGS = 10;
int currentlySavedWeatherDataStrings[MAX_NUMBER_OF_SAVED_WEATHER_DATA_STRINGS]; //Saves the byte location of each string.
int currentlySavedStringsCount;

/************************* Weather Station Setup *********************************/
#define TEMP_AND_HUMIDITY_PIN 2  //D4
#define DHTTYPE DHT11
DHT_Unified dht(TEMP_AND_HUMIDITY_PIN, DHTTYPE);
BH1750 lightMeter;
#define LIGHT_SENSOR_PIN_SDA D3
#define LIGHT_SENSOR_PIN_SCL D2


void setup() {
    Serial.begin(115200);
    dht.begin(); //Start DHT11 Temperature & Humidity Sensors
    Wire.begin(LIGHT_SENSOR_PIN_SDA, LIGHT_SENSOR_PIN_SCL);
    lightMeter.begin(); //Start GY-30 Light Intensity Sensor
    isInternetConnected = connectToWifi();
    initializeStorage(false);
}

void loop() {
    delay(10000);
    readAllDataInLocalStorage();
    String weatherDataJson = "{" + getTemperatureAndHumidityReadings() + "," + getLightReading() + "}";
    isInternetConnected = connectToWifi();

    if (!isInternetConnected) {
        Serial.println(" ");
        Serial.println("Internet connection unavailable. Saving latest weather data to local storage.");
        writeWeatherDataToLocal(weatherDataJson);
    } else {
        connectToMqtt();
        readAndUploadWeatherDataFromLocal();
        uploadWeatherData(weatherDataJson);
    }
}

//Returns false if connection fails and true if connection is a success.
bool connectToWifi() {
    int currentConnectionAttempt = 0;

    Serial.print("Connecting to ");
    Serial.println(WLAN_SSID);

    WiFi.begin(WLAN_SSID, WLAN_PASS);
    while (WiFi.status() != WL_CONNECTED && currentConnectionAttempt < MAX_WIFI_CONNECTION_TIME) {
        delay(1000);
        Serial.print(".");
        currentConnectionAttempt++;
    }

    return WiFi.status() != WL_CONNECTED ? false : true;
}

/*
 * Byte #1: Current Address
 * Byte #2: Number of currently saved strings
 * Byte #3-MAX_NUMBER_OF_SAVED_WEATHER_DATA_STRINGS: Each byte contains a location of a saved string
 * Byte #MAX_NUMBER_OF_SAVED_WEATHER_DATA_STRINGS-STORAGE_SIZE: Each byte contains the saved strings themselves
*/
void initializeStorage(bool eraseOverride) {
    EEPROM.begin(STORAGE_SIZE);
    if (ERASE_DATA_ON_RESTART == true || eraseOverride == true) {
        eraseDataFromLocalStorage();
    }

    currentAddress = EEPROM.read(1) == 0 ? MAX_NUMBER_OF_SAVED_WEATHER_DATA_STRINGS + 3 : EEPROM.read(1);
    Serial.println(" ");
    Serial.println("CURRENT ADDRESS: ");
    Serial.println(currentAddress);

    currentlySavedStringsCount = EEPROM.read(2);
    Serial.println("CURRENTLY SAVED STRINGS COUNT:");
    Serial.println(currentlySavedStringsCount);

    if (currentlySavedStringsCount != 0) {
        for (int i = 0; i < currentlySavedStringsCount; i++) {
            currentlySavedWeatherDataStrings[i] = (int) EEPROM.read(i + 3);
        }
    }


}

void writeDataToEEPROM(int address, int value) {
    EEPROM.write(address, value);

    //Writes to flash
    EEPROM.commit();
}

/*
 * We write a string in the following format:
 * Byte #currentAddress: Length of String
 * Byte #currentAddress + 1 -> Length of String: The String
 * Example:
 * "Hello"
 * String: 5 | H | e | l | l | o
 * Bytes:  0 | 1 | 2 | 3 | 4 | 5
*/
int writeDataToEEPROM(String &value) {
    byte lengthOfValue = value.length();
    int locationOfWrite = currentAddress;

    //First we write the length of the upcoming string.
    EEPROM.write(currentAddress, lengthOfValue);

    currentAddress++;

    for (int i = 0; i < lengthOfValue; i++) {
        EEPROM.write(currentAddress, value.charAt(i));
        currentAddress++;
    }

    currentlySavedStringsCount++;

    EEPROM.write(1, currentAddress); //Save current address to byte 1
    EEPROM.write(2, currentlySavedStringsCount); //Save currently saved strings count to byte 2
    EEPROM.write(currentlySavedStringsCount + 2,
                 locationOfWrite); //Save the location of the latest string to one of the string pointer slots in memory


    //Writes to flash
    EEPROM.commit();

    return locationOfWrite;
}

int readDataFromEEPROM(int address) {
    return EEPROM.read(address);
}

String readStringFromEEPROM(int location) {
    //We stored the length of the string at the address location.
    byte stringLength = EEPROM.read(location);

    char returnStringBuffer[stringLength + 1];

    for (int i = 0; i < stringLength; i++) {
        returnStringBuffer[i] = EEPROM.read(location + 1 + i);
    }

    returnStringBuffer[stringLength] = '\0';

    return (String) returnStringBuffer;
}

void eraseDataFromLocalStorage() {
    // By writing 0 to all available bytes, we clear the existing storage.
    for (int i = 0; i < STORAGE_SIZE; i++) {
        EEPROM.write(i, 0);
    }
}

void connectToMqtt() {
    int8_t ret;

    // Stop if already connected.
    if (mqtt.connected()) {
        return;
    }

    Serial.print("Connecting to MQTT... ");

    uint8_t retries = MQTT_RETRIES;
    while ((ret = mqtt.connect()) != 0) { // connect will return 0 for connected
        Serial.println(mqtt.connectErrorString(ret));
        Serial.println("Retrying MQTT connection in 5 seconds...");
        mqtt.disconnect();
        delay(5000);  // wait 5 seconds
        retries--;
        if (retries == 0) {
            // basically die and wait for WDT to reset me
            return;
        } else {
            Serial.println("MQTT Connected!");
        }
    }
}

void uploadWeatherData(const String &data) {
    char buf[data.length()];
    data.toCharArray(buf, data.length() + 1);

    if (!feed.publish(buf)) {
        Serial.println("Failed");
    } else {
        Serial.println("OK!");
    }
}

void writeWeatherDataToLocal(String weatherData) {
    int locationOfWrite = writeDataToEEPROM(weatherData);

    currentlySavedWeatherDataStrings[currentlySavedStringsCount - 1] = locationOfWrite;
    Serial.println("Data successfully written to local storage.");
}

void readAndUploadWeatherDataFromLocal() {
    if (currentlySavedWeatherDataStrings[0] == 0) {
        Serial.println("No data was found in local storage that needs to be uploaded.");
        return;
    } else {
        int iterator = 0;
        while (currentlySavedWeatherDataStrings[iterator] != 0) {
            uploadWeatherData(readStringFromEEPROM(currentlySavedWeatherDataStrings[iterator]));
            currentlySavedWeatherDataStrings[iterator] = 0; //Empty out the reference
            iterator++;
        }
        Serial.println("Data was found in local storage and successfully uploaded to the cloud.");
        initializeStorage(true);
    }
}


String getTemperatureAndHumidityReadings() {
    String returnValue;
    sensors_event_t event;
    dht.temperature().getEvent(&event);
    returnValue = "t:" + (String) event.temperature;
    dht.humidity().getEvent(&event);
    returnValue = returnValue + ",h:" + (String) event.relative_humidity;
    return returnValue;
}

String getLightReading(){
  float lux = lightMeter.readLightLevel();
  return "l:" + (String)lux;
}

//Used for purposes of Demo
void readAllDataInLocalStorage() {
    if (currentlySavedWeatherDataStrings[0] == 0) {
        Serial.println(" ");
        Serial.println("No data was found saved in the local storage.");
        return;
    } else {
        int iterator = 0;
        while (currentlySavedWeatherDataStrings[iterator] != 0) {
            Serial.println("A string was found at address: " + currentlySavedWeatherDataStrings[iterator]);
            Serial.println("The value of the string is: ");
            Serial.print(readStringFromEEPROM(currentlySavedWeatherDataStrings[iterator]));
            Serial.println("");
            iterator++;
        }
    }
}