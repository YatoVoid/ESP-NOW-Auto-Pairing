#include "esp_wifi.h"
#include <esp_now.h>
#include <WiFi.h>
#include <EEPROM.h>

#define EEPROM_SIZE 256

// Structure used to send and receive ESP-NOW messages
struct Message {
  char message[32];
  uint8_t myMAC[6];
};

// Global variables for message handling and peer tracking
Message myData;
Message recvData;
uint8_t peerAddress[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};  // Default: broadcast
uint8_t myEEPROM[6];  // Stores last paired MAC address
bool macSent = false;
int failedDeliveryCount = 0;
const int MAX_DELIVERY_FAILURES = 6;
esp_now_peer_info_t peerInfo;

// ---------- Utility Functions ----------

// Read saved peer MAC address from EEPROM
void readFromEEPROM() {
  for (int i = 0; i < 6; i++) {
    myEEPROM[i] = EEPROM.read(i);
  }
  Serial.println("Read MAC Address from EEPROM.");
}

// Save new peer MAC address to EEPROM
void writeToEEPROM(uint8_t* mac) {
  for (int i = 0; i < 6; i++) {
    EEPROM.write(i, mac[i]);
  }
  EEPROM.commit();  // Commit changes to EEPROM
  Serial.println("MAC Address saved to EEPROM.");
}

// Clear the EEPROM to reset pairing
void clearEEPROM() {
  for (int i = 0; i < 6; i++) {
    EEPROM.write(i, 0x00);
    myEEPROM[i] = 0x00;
  }
  EEPROM.commit();
  Serial.println("EEPROM cleared and MAC reset.");
}

// Convert MAC address to human-readable string
String macToString(const uint8_t* mac) {
  char macStr[18];
  snprintf(macStr, sizeof(macStr), "%02X:%02X:%02X:%02X:%02X:%02X",
           mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  return String(macStr);
}

// Convert a string MAC ("AA:BB:CC:DD:EE:FF") to byte array
void stringToMAC(const String& macStr, uint8_t* macOut) {
  int idx = 0;
  for (int i = 0; i < 17; i += 3) {
    macOut[idx++] = strtoul(macStr.substring(i, i + 2).c_str(), nullptr, 16);
  }
}

// Set up a new ESP-NOW peer
void setupPeer(uint8_t* address) {
  esp_now_del_peer(address);  // Remove if already exists
  memset(&peerInfo, 0, sizeof(peerInfo));
  memcpy(peerInfo.peer_addr, address, 6);
  peerInfo.channel = 0;  // Same channel
  peerInfo.encrypt = false;

  if (esp_now_add_peer(&peerInfo) != ESP_OK) {
    Serial.println("Error: Failed to add ESP-NOW peer.");
  }
}

// ---------- ESP-NOW Callbacks ----------

// Called during pairing mode when message is received
void onDataRecvPairing(const esp_now_recv_info_t* info, const uint8_t* data, int len) {
  memcpy(&recvData, data, sizeof(recvData));

  Serial.println("Incoming MAC Address during pairing:");
  Serial.println(macToString(info->src_addr));
  Serial.print("Message: ");
  Serial.println(recvData.message);
  Serial.print("Sender's MAC: ");
  Serial.println(macToString(recvData.myMAC));
  Serial.println("----------------------------------");

  // Save peer MAC to EEPROM for future reconnection
  writeToEEPROM(recvData.myMAC);

  // Send own MAC as confirmation and restart to switch to connected mode
  if (!macSent) {
    Serial.println("Responding with our MAC and restarting...");
    strcpy(myData.message, "Yes Lets Talk");

    esp_err_t result = esp_now_send(peerAddress, (uint8_t*)&myData, sizeof(myData));
    if (result == ESP_OK) {
      Serial.println("Sent our MAC in response.");
      macSent = true;
      delay(500);
      ESP.restart();  // Restart to enter connected mode
    } else {
      Serial.println("Error sending MAC response.");
    }
  }
}

// Called during connected mode when message is received from paired peer
void onDataRecvConnected(const esp_now_recv_info_t* info, const uint8_t* data, int len) {
  memcpy(&recvData, data, sizeof(recvData));

  Serial.println("Message received from connected peer:");
  Serial.println("----------------------------------");
  Serial.print("From MAC: ");
  Serial.println(macToString(info->src_addr));
  Serial.print("Sender's MAC: ");
  Serial.println(macToString(recvData.myMAC));
  Serial.println("----------------------------------");
}

// Called after message send completes (success or fail)
void onDataSent(const uint8_t* mac_addr, esp_now_send_status_t status) {
  Serial.print("\r\nLast Packet Send Status:\t");
  if (status == ESP_NOW_SEND_SUCCESS) {
    Serial.println("Delivery Success");
    failedDeliveryCount = 0;  // Reset failure counter
  } else {
    Serial.println("Delivery Failed");
    failedDeliveryCount++;
    Serial.printf("Failed Attempts: %d/%d\n", failedDeliveryCount, MAX_DELIVERY_FAILURES);

    // Restart and clear pairing if message fails too many times
    if (failedDeliveryCount >= MAX_DELIVERY_FAILURES) {
      Serial.println("Too many delivery failures. Clearing EEPROM and restarting...");
      clearEEPROM();
      delay(1000);
      ESP.restart();
    }
  }
}


void setup() {
  Serial.begin(115200);
  EEPROM.begin(EEPROM_SIZE);
  readFromEEPROM();

  WiFi.mode(WIFI_STA);  // Set ESP to station mode
  esp_wifi_set_channel(1, WIFI_SECOND_CHAN_NONE);  // Ensure both devices are on same channel
  delay(100);

  Serial.print("WiFi Channel: ");
  Serial.println(WiFi.channel());

  // Fill local MAC address into outgoing message structure
  String myMACstr = WiFi.macAddress();
  stringToMAC(myMACstr, myData.myMAC);

  // Initialize ESP-NOW
  if (esp_now_init() != ESP_OK) {
    Serial.println("Error: ESP-NOW initialization failed.");
    return;
  }

  // Add self as a peer (not required but can help)
  setupPeer(myData.myMAC);

  // Determine whether to pair or reconnect
  if (myEEPROM[0] == 0x00) {
    // EEPROM is empty: begin pairing mode
    Serial.println("\nEEPROM is empty. Starting fresh pairing.");
    Serial.println("\nAttempting Dynamic Connection...");


    esp_now_register_recv_cb(onDataRecvPairing);
    esp_now_register_send_cb(onDataSent);

    setupPeer(peerAddress);  // Use broadcast address for initial pairing

    strcpy(myData.message, "Hello Lets Talk");
    delay(2000);  // Give peer time to boot and register callback
    esp_now_send(peerAddress, (uint8_t*)&myData, sizeof(myData));

  } else {
    // EEPROM has peer MAC: reconnect to known peer
    Serial.println("\nEEPROM contains previous pairing info.");
    Serial.print("Loaded MAC from EEPROM: ");
    for (int i = 0; i < 6; i++) {
      peerAddress[i] = myEEPROM[i];
      Serial.printf("%02X", myEEPROM[i]);
      if (i < 5) Serial.print(":");
    }
    Serial.println();

    esp_now_register_recv_cb(onDataRecvConnected);
    esp_now_register_send_cb(onDataSent);

    setupPeer(peerAddress);

    strcpy(myData.message, "Hello Lets Talk");
    delay(2000);
    esp_now_send(peerAddress, (uint8_t*)&myData, sizeof(myData));
    Serial.println("\nDevices Already Paired. Resuming Communication...");
  }
}


void loop() {
  // Send message to paired peer
  esp_err_t result = esp_now_send(peerAddress, (uint8_t*)&myData, sizeof(myData));
  if (result == ESP_OK) {
    Serial.print("Message sent successfully to -> ");
    Serial.println(macToString(peerAddress));
    Serial.print("Message sent successfully from -> ");
    Serial.println(WiFi.macAddress());
    Serial.println("");
  } else {
    Serial.println("Error sending the message.");
  }

  delay(500);  // Wait before sending the next message
}
