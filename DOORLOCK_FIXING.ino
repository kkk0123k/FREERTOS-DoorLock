#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <MFRC522.h>
#include <Keypad.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include <Arduino.h>
#include <WiFi.h>
#include <Firebase_ESP_Client.h>

//Provide the token generation process info.
#include "addons/TokenHelper.h"
//Provide the RTDB payload printing info and other helper functions.
#include "addons/RTDBHelper.h"

// Insert your network credentials
#define WIFI_SSID "Redmi Note 8 Pro"
#define WIFI_PASSWORD "1234567890ab"

// Insert Firebase project API Key
#define API_KEY "AIzaSyCAzsvANrl4PNEyCUJ7chHf6aE-FpW9uPY"

// Insert RTDB URL
#define DATABASE_URL "https://esp32-firebase-4f7d3-default-rtdb.asia-southeast1.firebasedatabase.app"

// Definitions for pins to connect to RFID
#define SS_PIN 5
#define RST_PIN 4

// Define Firebase Data object for interfacing with the Firebase Realtime Database
FirebaseData fbdo;
// Define Firebase Authentication object for handling authentication
FirebaseAuth auth;
// Define Firebase Configuration object for setting up Firebase configuration details
FirebaseConfig config;
// Define the path for the password node in the Firebase Realtime Database
const char* passwordPath = "password";
// Define the path for the validCard node in the Firebase Realtime Database
const char* uidPath = "validCard";
// Variable to store the previous time data was sent, used for timing intervals
unsigned long sendDataPrevMillis = 0;
// Boolean flag to indicate if the Firebase signup was successful
bool signupOK = false;

// Connect to RFID and LCD
MFRC522 mfrc522(SS_PIN, RST_PIN);
LiquidCrystal_I2C lcd(0x27, 16, 2);

// Define task handles for FreeRTOS tasks. These handles will be used to manage and control the tasks.
// Handle for the task that updates the LCD display
TaskHandle_t lcdTaskHandle = NULL;
// Handle for the task that manages the RFID reader
TaskHandle_t rfidTaskHandle = NULL;
// Handle for the task that handles keypad input
TaskHandle_t keyPadTaskHandle = NULL;
// Define a semaphore handle for the mutex. This mutex will be used to synchronize access to the LCD display,
// ensuring that only one task can update the display at a time.
SemaphoreHandle_t mutexLCD;


const byte ROWS = 4;  // Four rows
const byte COLS = 4;  // Four columns

// Define the keymap to keypad
char keys[ROWS][COLS] = {
  { '1', '2', '3', 'A' },
  { '4', '5', '6', 'B' },
  { '7', '8', '9', 'C' },
  { '*', '0', '#', 'D' }
};

int pos = 0;  // Global variable to keep track of the next empty position in the validUID array
// Valid UID array, used for saved valid card
byte validUID[20][4] = {
  // Add more valid UIDs here
};
// Connect keypad ROW0, ROW1, ROW2, ROW3 to these pins
byte rowPins[ROWS] = { 13, 12, 14, 27 };
// Connect keypad COL0, COL1, COL2, COL3 to these pins
byte colPins[COLS] = { 26, 25, 33, 32 };

// Create the Keypad
Keypad keypad = Keypad(makeKeymap(keys), rowPins, colPins, ROWS, COLS);

// variable to saved door password
char password[16] = "";  // Initial password

void lcdTask(void* parameter) {
  (void)parameter;

  Serial.println("LCD Task started");

  while (1) {
    if (xSemaphoreTake(mutexLCD, portMAX_DELAY) == pdTRUE) {
      // Update the LCD
      lcd.clear();
      lcd.setCursor(0, 0);
      lcd.print("Door Lock");

      xSemaphoreGive(mutexLCD);
    }

    // Check for updates in the valid UID list
    readUIDFromFirebase();
    // Check for updates in the door password
    readPasswordFromFirebase();

    // Print updates for debugging
    Serial.println("LCD Task: Updated valid UIDs and password from Firebase.");
    Serial.print("Current password: ");
    Serial.println(password);
    Serial.println("Current valid UIDs:");
    for (int i = 0; i < pos; i++) {
      Serial.print("UID ");
      Serial.print(i);
      Serial.print(": ");
      for (int j = 0; j < 4; j++) {
        Serial.print(validUID[i][j], HEX);
        Serial.print(" ");
      }
      Serial.println();
    }

    // Yield to the watchdog
    yield();

    // Delay before the next check (e.g., 1 second)
    vTaskDelay(2000 / portTICK_PERIOD_MS);
  }
}

void rfidTask(void* parameter) {
  (void)parameter;

  Serial.println("RFID Task started");

  SPI.begin();
  mfrc522.PCD_Init();

  Serial.println("RFID initialized!");

  while (1) {
    if (!mfrc522.PICC_IsNewCardPresent()) {
      vTaskDelay(50 / portTICK_PERIOD_MS);
      continue;
    }

    if (!mfrc522.PICC_ReadCardSerial()) {
      vTaskDelay(50 / portTICK_PERIOD_MS);
      continue;
    }

    Serial.print("UID: ");
    for (byte i = 0; i < mfrc522.uid.size; i++) {
      Serial.print(mfrc522.uid.uidByte[i] < 0x10 ? "0" : "");
      Serial.print(mfrc522.uid.uidByte[i], HEX);
    }
    Serial.println();

    // Debug: Print the current validUID array
    Serial.println("Current validUID array:");
    for (size_t i = 0; i < pos; i++) {
      Serial.print("UID ");
      Serial.print(i + 1);
      Serial.print(": ");
      for (byte j = 0; j < 4; j++) {
        Serial.print(validUID[i][j] < 0x10 ? "0" : "");
        Serial.print(validUID[i][j], HEX);
      }
      Serial.println();
    }

    bool isValidCard = false;
    for (size_t i = 0; i < pos; i++) {
      if (memcmp(mfrc522.uid.uidByte, validUID[i], 4) == 0) {
        isValidCard = true;
        break;
      }
    }

    if (xSemaphoreTake(mutexLCD, portMAX_DELAY) == pdTRUE) {
      lcd.clear();
      lcd.setCursor(0, 0);

      if (isValidCard) {
        lcd.print("Door Unlocked");
      } else {
        lcd.print("Wrong Card");
      }

      vTaskDelay(3000 / portTICK_PERIOD_MS);
      lcd.clear();
      xSemaphoreGive(mutexLCD);
    }
  }
}

void keyPadTask(void* parameter) {
  (void)parameter;

  Serial.println("Keypad Task started");

  while (1) {
    char key = keypad.getKey();
    if (key) {
      Serial.print("Key pressed: ");
      Serial.println(key);

      if (xSemaphoreTake(mutexLCD, portMAX_DELAY) == pdTRUE) {
        Serial.println("LCD mutex taken");
        vTaskSuspend(rfidTaskHandle);  // Suspend RFID task
        Serial.println("RFID Task suspended");
        vTaskSuspend(lcdTaskHandle);  // Suspend LCD task
        Serial.println("LCD Task suspended");

        if (key == 'A') {
          lcd.clear();
          lcd.setCursor(0, 0);
          lcd.print("Scan Card");
          Serial.println("Displayed 'Scan Card'");

          while (1) {
            if (mfrc522.PICC_IsNewCardPresent() && mfrc522.PICC_ReadCardSerial()) {
              Serial.print("Scanned UID: ");
              for (byte i = 0; i < mfrc522.uid.size; i++) {
                Serial.print(mfrc522.uid.uidByte[i] < 0x10 ? "0" : "");
                Serial.print(mfrc522.uid.uidByte[i], HEX);
              }
              Serial.println();

              bool isDuplicate = false;
              for (size_t i = 0; i < pos; i++) {
                if (memcmp(mfrc522.uid.uidByte, validUID[i], 4) == 0) {
                  isDuplicate = true;
                  break;
                }
              }

              if (!isDuplicate) {
                lcd.clear();
                lcd.setCursor(0, 0);
                lcd.print("Card Invalid");
                Serial.println("Card Invalid");
                vTaskDelay(1500 / portTICK_PERIOD_MS);  // Display duplicate message for 1 second
                lcd.clear();
                lcd.print("Scan Card");
              } else {
                lcd.clear();
                lcd.setCursor(0, 0);
                lcd.print("New Password");
                lcd.setCursor(0, 1);
                Serial.println("Displayed 'New Password'");

                char newPassword[16] = "";
                int index = 0;
                unsigned long startTime = millis();

                // Loop until user presses '#' or 'B', or timeout after 60 seconds
                while (1) {
                  char keyInput = keypad.getKey();
                  if (keyInput) {
                    Serial.print("New Password Key input: ");
                    Serial.println(keyInput);
                    if (keyInput == '#') {
                      newPassword[index] = '\0';
                      strcpy(password, newPassword);

                      // Update the new password to Firebase
                      writeNewPasswordToFirebase(newPassword);

                      lcd.clear();
                      lcd.setCursor(0, 0);
                      lcd.print("Password Update");
                      Serial.println("Password updated");
                      vTaskDelay(3000 / portTICK_PERIOD_MS);  // Display update message for 3 seconds
                      break;
                    } else if (keyInput == 'B') {
                      lcd.clear();
                      Serial.println("New password entry canceled");
                      break;
                    } else {
                      newPassword[index++] = keyInput;
                      lcd.print(keyInput);
                    }
                  }
                  if (millis() - startTime > 60000) {  // Check if 60 seconds have passed
                    Serial.println("New password entry timed out");
                    lcd.clear();
                    break;
                  }
                  vTaskDelay(50 / portTICK_PERIOD_MS);  // Yield control to prevent WDT
                }

                Serial.println("Updated Password");
                vTaskDelay(3000 / portTICK_PERIOD_MS);  // Display add success message for 3 seconds
                break;
              }
            }

            if (keypad.getKey() == 'B') {
              lcd.clear();
              Serial.println("Card scan entry canceled");
              break;
            }
            vTaskDelay(50 / portTICK_PERIOD_MS);  // Yield control to prevent WDT
          }
        } else if (key == '*') {
          lcd.clear();
          lcd.setCursor(0, 0);
          lcd.print("Enter Password");
          lcd.setCursor(0, 1);
          Serial.println("Displayed 'Enter Password'");

          char input[16] = "";
          int index = 0;
          unsigned long startTime = millis();

          // Loop until user presses '#' or 'B', or timeout after 60 seconds
          while (1) {
            char keyInput = keypad.getKey();
            if (keyInput) {
              Serial.print("Enter Password Key input: ");
              Serial.println(keyInput);
              if (keyInput == '#') {
                input[index] = '\0';
                if (strcmp(input, password) == 0) {
                  lcd.clear();
                  lcd.print("Door Unlocked");
                  Serial.println("Password correct, door unlocked");
                  vTaskDelay(3000 / portTICK_PERIOD_MS);  // Display unlock message for 3 seconds
                  break;
                } else {
                  lcd.clear();
                  lcd.print("Wrong Pass");
                  Serial.println("Password incorrect");
                  vTaskDelay(1500 / portTICK_PERIOD_MS);  // Display wrong password message for 1.5 seconds
                  lcd.clear();
                  lcd.print("Enter Password");
                  lcd.setCursor(0, 1);
                  index = 0;
                }
              } else if (keyInput == 'B') {
                lcd.clear();
                Serial.println("Password entry canceled");
                break;
              } else {
                input[index++] = keyInput;
                lcd.print(keyInput);
              }
            }
            if (millis() - startTime > 60000) {  // Check if 60 seconds have passed
              Serial.println("Password entry timed out");
              lcd.clear();
              break;
            }
            vTaskDelay(50 / portTICK_PERIOD_MS);  // Yield control to prevent WDT
          }
        } else if (key == 'C') {
          lcd.clear();
          lcd.setCursor(0, 0);
          lcd.print("Scan Card");
          Serial.println("Displayed 'Scan Card'");

          while (1) {
            if (mfrc522.PICC_IsNewCardPresent() && mfrc522.PICC_ReadCardSerial()) {
              Serial.print("Scanned UID: ");
              for (byte i = 0; i < mfrc522.uid.size; i++) {
                Serial.print(mfrc522.uid.uidByte[i] < 0x10 ? "0" : "");
                Serial.print(mfrc522.uid.uidByte[i], HEX);
              }
              Serial.println();

              bool isDuplicate = false;
              for (size_t i = 0; i < pos; i++) {
                if (memcmp(mfrc522.uid.uidByte, validUID[i], 4) == 0) {
                  isDuplicate = true;
                  break;
                }
              }

              if (isDuplicate) {
                lcd.clear();
                lcd.setCursor(0, 0);
                lcd.print("Duplicate Card");
                Serial.println("Duplicate Card");
                vTaskDelay(1500 / portTICK_PERIOD_MS);  // Display duplicate message for 1 second
                lcd.clear();
                lcd.print("Scan Card");
              } else {
                lcd.print("Add Success");
                memcpy(validUID[pos], mfrc522.uid.uidByte, 4);

                // Write the newly added UID to Firebase
                writeValidUIDToFirebase(pos);
                pos++;

                lcd.clear();
                lcd.print("Add Success");
                Serial.println("Add Success");
                Serial.println("Updated validUID array:");
                for (size_t i = 0; i < pos; i++) {
                  Serial.print("UID ");
                  Serial.print(i);
                  Serial.print(": ");
                  for (byte j = 0; j < 4; j++) {
                    Serial.print(validUID[i][j] < 0x10 ? "0" : "");
                    Serial.print(validUID[i][j], HEX);
                    if (j < 3) Serial.print(":");
                  }
                  Serial.println();
                }
                vTaskDelay(3000 / portTICK_PERIOD_MS);  // Display add success message for 3 seconds
                break;
              }
            }

            if (keypad.getKey() == 'B') {
              lcd.clear();
              Serial.println("Card scan entry canceled");
              break;
            }
            vTaskDelay(50 / portTICK_PERIOD_MS);  // Yield control to prevent WDT
          }
        }

        xSemaphoreGive(mutexLCD);
        Serial.println("LCD mutex given");
        vTaskResume(lcdTaskHandle);
        Serial.println("LCD Task resumed");
        vTaskResume(rfidTaskHandle);
        Serial.println("RFID Task resumed");
      }
    }
    vTaskDelay(50 / portTICK_PERIOD_MS);
  }
}


void setup() {

  Serial.begin(115200);
  while (!Serial)
    ;
  connectToWiFi();
  firebaseSetup();
  lcd.init();
  lcd.backlight();

  mutexLCD = xSemaphoreCreateMutex();
  if (mutexLCD == NULL) {
    Serial.println("Failed to create LCD mutex");
    while (1)
      ;
  }

// Create the LCD task and pin it to core 0
xTaskCreatePinnedToCore(
  lcdTask,           // Function that implements the task
  "lcdTask",         // Name of the task
  10000,             // Stack size allocated for the task in bytes
  NULL,              // Parameter passed to the task (not used here)
  1,                 // Priority of the task
  &lcdTaskHandle,    // Task handle used to manage the task
  0);                // Core on which the task is pinned (core 0)

// Create the RFID task and pin it to core 0
xTaskCreatePinnedToCore(
  rfidTask,          // Function that implements the task
  "rfidTask",        // Name of the task
  4096,              // Stack size allocated for the task in bytes
  NULL,              // Parameter passed to the task (not used here)
  2,                 // Priority of the task
  &rfidTaskHandle,   // Task handle used to manage the task
  0);                // Core on which the task is pinned (core 0)

// Create the keypad task and pin it to core 0
xTaskCreatePinnedToCore(
  keyPadTask,        // Function that implements the task
  "keyPadTask",      // Name of the task
  4096,              // Stack size allocated for the task in bytes
  NULL,              // Parameter passed to the task (not used here)
  3,                 // Priority of the task
  &keyPadTaskHandle, // Task handle used to manage the task
  0);                // Core on which the task is pinned (core 0)


  byte emptyUID[4] = { 0, 0, 0, 0 };  // Temporary array for comparison

  // Find the first empty slot in the validUID array and update pos
  for (size_t i = 0; i < sizeof(validUID) / sizeof(validUID[0]); i++) {
    if (memcmp(validUID[i], emptyUID, 4) == 0) {
      pos = i;
      break;
    }
  }


  Serial.println("Setup completed.");
}

void loop() {
  // Nothing to do here as we're using FreeRTOS tasks
}

void connectToWiFi() {
  Serial.begin(115200);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.print("Connecting to Wi-Fi");
  while (WiFi.status() != WL_CONNECTED) {
    Serial.print(".");
    delay(300);
  }
  Serial.println();
  Serial.print("Connected with IP: ");
  Serial.println(WiFi.localIP());
  Serial.println();
}

void firebaseSetup() {
  /* Assign the API key (required for authenticating requests to Firebase services) */
  config.api_key = API_KEY;

  /* Assign the Realtime Database URL (required to identify the database instance to connect to) */
  config.database_url = DATABASE_URL;

  /* Attempt to sign up for a new Firebase user account (using anonymous authentication) */
  if (Firebase.signUp(&config, &auth, "", "")) {
    /* If sign-up is successful, print "ok" to the serial monitor and set the signupOK flag to true */
    Serial.println("ok");
    signupOK = true;
  } else {
    /* If sign-up fails, print the error message from the signup process to the serial monitor */
    Serial.printf("%s\n", config.signer.signupError.message.c_str());
  }

  /* Assign the callback function for handling token generation status updates
     (TokenHelper.h defines the actual implementation of tokenStatusCallback)
     This function will be called to provide updates on the token generation process. */
  config.token_status_callback = tokenStatusCallback;  // see addons/TokenHelper.h

  /* Initialize the Firebase library with the provided configuration and authentication objects
     (config contains settings such as API key and database URL, and auth holds authentication info) */
  Firebase.begin(&config, &auth);

  /* Enable automatic WiFi reconnection
     (if the WiFi connection drops, the library will attempt to reconnect automatically) */
  Firebase.reconnectWiFi(true);
}



void writeValidUIDToFirebase(int index) {
  // Convert the valid UID at the specified index to a hex string
  char uidStr[9];  // 4 bytes UID as hex string (8 characters + null terminator)
  snprintf(uidStr, sizeof(uidStr), "%02X%02X%02X%02X", validUID[index][0], validUID[index][1], validUID[index][2], validUID[index][3]);

  // Create the Firebase path for the specified index
  String path = String(uidPath) + "/" + String(index);
  if (Firebase.RTDB.setString(&fbdo, path.c_str(), uidStr)) {
    // If writing to Firebase is successful, print a success message
    Serial.print("UID ");
    Serial.print(index);
    Serial.println(" written to Firebase.");
  } else {
    // If writing to Firebase fails, print the error reason
    Serial.print("Failed to write UID ");
    Serial.print(index);
    Serial.print(": ");
    Serial.println(fbdo.errorReason());
  }
}

void writeNewPasswordToFirebase(const char* newPassword) {
  // Copy the new password to the password variable
  strncpy(password, newPassword, sizeof(password) - 1);
  password[sizeof(password) - 1] = '\0';  // Ensure null-termination

  // Write the new password to Firebase
  if (Firebase.RTDB.setString(&fbdo, passwordPath, password)) {
    Serial.println("PASSED");
    Serial.println("PATH: " + fbdo.dataPath());
    Serial.println("TYPE: " + fbdo.dataType());
  } else {
    Serial.println("FAILED");
    Serial.println("REASON: " + fbdo.errorReason());
  }
}

void readUIDFromFirebase() {
  // Read the list of valid UIDs from Firebase
  if (Firebase.RTDB.getArray(&fbdo, "/" + String(uidPath))) {
    Serial.println("PASSED");
    Serial.println("PATH: " + fbdo.dataPath());
    Serial.println("TYPE: " + fbdo.dataType());

    FirebaseJsonArray jsonArray = fbdo.jsonArray();
    FirebaseJsonData jsonData;

    // Print out the entire JSON array data
    String jsonStr;
    jsonArray.toString(jsonStr, true);  // true for pretty print

    for (size_t i = 0; i < jsonArray.size(); i++) {
      jsonArray.get(jsonData, i);
      if (jsonData.typeNum == FirebaseJson::JSON_STRING) {
        String uidString = jsonData.stringValue;

        byte uid[4];
        sscanf(uidString.c_str(), "%2hhx%2hhx%2hhx%2hhx", &uid[0], &uid[1], &uid[2], &uid[3]);
        memcpy(validUID[i], uid, 4);
      }
      yield();  // Yield to prevent WDT trigger
    }
    pos = jsonArray.size();
  } else {
    Serial.println(fbdo.errorReason());
  }
}

void readPasswordFromFirebase() {
  // Read the door password from Firebase
  if (Firebase.RTDB.getString(&fbdo, "/" + String(passwordPath))) {
    if (fbdo.dataType() == "string") {
      fbdo.stringData().toCharArray(password, sizeof(password));  // Ensure password fits in the defined char array
      Serial.print("Password read from Firebase: ");
      Serial.println(password);
    }
  } else {
    Serial.println(fbdo.errorReason());
  }
}

