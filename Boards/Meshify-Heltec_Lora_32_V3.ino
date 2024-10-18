// THIS VERSION NOW HAS CRC CHECKING. ALL NODES MUST BE THIS SAME VERSION OF CODE.
#define HELTEC_POWER_BUTTON // Use the power button feature of Heltec
#include <heltec_unofficial.h> // Heltec library for OLED and LoRa
#include <painlessMesh.h>
#include <WiFi.h>
#include <ESPAsyncWebServer.h>
#include <DNSServer.h>
#include <Wire.h>
#include <esp_task_wdt.h> // Watchdog timer library
#include <vector>         // For handling list of messages
#include <map>            // For unified retransmission tracking

// Unified Retransmission Tracking Structure
struct TransmissionStatus {
  bool transmittedViaWiFi = false;
  bool transmittedViaLoRa = false;
  bool addedToMessages = false;  // Flag to track if the message has been added to messages vector
};

// Map to track retransmissions
std::map<String, TransmissionStatus> messageTransmissions;

// LoRa Parameters
#include <RadioLib.h>
#define PAUSE 5400000  // Pause time to respect duty cycle: 54 minutes (60 mins - 6 mins)
#define FREQUENCY 869.4000 // Changed to start of EU868 10% band
#define BANDWIDTH 250.0 // Using the full band
#define SPREADING_FACTOR 11 // We can go to 12 but 11 seems fine
#define TRANSMIT_POWER 22 // Now using full power
#define CODING_RATE 8  // Coding rate 4/5
String rxdata;
volatile bool rxFlag = false;
long counter = 0;
uint64_t tx_time;
uint64_t last_tx = 0;
uint64_t minimum_pause = 0;
unsigned long lastTransmitTime = 0;  // Timing variable for managing sequential transmissions
String fullMessage;                  // Global variable to hold the message for sequential transmission

// Function to handle LoRa received packets
void rx() {
  rxFlag = true;
}

// Define the maximum allowed duty cycle (10%)
#define DUTY_CYCLE_LIMIT_MS 360000  // 6 minutes (360,000 ms) maximum in a 60-minute window
#define DUTY_CYCLE_WINDOW 3600000   // 60 minutes in milliseconds

// Duty Cycle Variables
uint64_t cumulativeTxTime = 0;      // Total transmit time accumulated
uint64_t dutyCycleStartTime = 0;    // When the duty cycle window started

void resetDutyCycle() {
    cumulativeTxTime = 0;
    dutyCycleStartTime = millis();
    Serial.println("[Duty Cycle] Reset duty cycle counter.");
}

// Function to calculate the required pause based on the duty cycle
void calculateDutyCyclePause(uint64_t tx_time) {
    cumulativeTxTime += tx_time;
    if (millis() - dutyCycleStartTime >= DUTY_CYCLE_WINDOW) {
        resetDutyCycle();
    }

    if (cumulativeTxTime >= DUTY_CYCLE_LIMIT_MS) {
        minimum_pause = DUTY_CYCLE_WINDOW - (millis() - dutyCycleStartTime);
        if (minimum_pause < 0) minimum_pause = 0;
        Serial.printf("[Duty Cycle] Duty cycle limit reached, waiting for %llu ms.\n", minimum_pause);
    } else {
        minimum_pause = 0;
        Serial.printf("[Duty Cycle] Duty cycle time used: %llu ms.\n", cumulativeTxTime);
    }
}

void setupLora() {
  heltec_setup();  // Initialize Heltec board, display, and other components if display is enabled
  Serial.println("Initializing LoRa radio...");

  RADIOLIB_OR_HALT(radio.begin());
  radio.setDio1Action(rx);

  RADIOLIB_OR_HALT(radio.setFrequency(FREQUENCY));
  RADIOLIB_OR_HALT(radio.setBandwidth(BANDWIDTH));
  RADIOLIB_OR_HALT(radio.setSpreadingFactor(SPREADING_FACTOR));
  RADIOLIB_OR_HALT(radio.setCodingRate(CODING_RATE));
  RADIOLIB_OR_HALT(radio.setOutputPower(TRANSMIT_POWER));

  radio.startReceive(RADIOLIB_SX126X_RX_TIMEOUT_INF);
}

// Meshify Parameters
#define MESH_SSID "Meshify 1.0"
#define MESH_PASSWORD ""  
#define MESH_PORT 5555
const int maxMessages = 50;

// Duty Cycle Variables For Bypass (Testing Only Please Use Low Power if True)
bool bypassDutyCycle = false;     // Set to true to bypass duty cycle check (please use low power)
bool dutyCycleActive = false;     // Tracks if duty cycle limit is reached
bool lastDutyCycleActive = false; // Tracks the last known duty cycle state

// Mesh and Web Server Setup
AsyncWebServer server(80);
DNSServer dnsServer;
painlessMesh mesh;

// Message structure for Meshify
struct Message {
  String nodeId;     // Originator ID
  String sender;
  String content;
  String source;     // Indicates message source (WiFi or LoRa)
  String messageID;  // Unique message ID
  String relayID;    // Node that last relayed the message
  int rssi;          // RSSI value, applicable for LoRa
  float snr;         // SNR value, applicable for LoRa
};

// Rolling list for messages
std::vector<Message> messages;  // Dynamic vector to store messages

// Centralized mesh data
int totalNodeCount = 0;
uint32_t currentNodeId = 0;

// Global variable to manage LoRa delay after WiFi transmission
unsigned long loRaTransmitDelay = 0;  // This stores the time after which LoRa can transmit

// Global message counter for generating unique message IDs
unsigned long messageCounter = 0;

// Function to generate a unique message ID
String generateMessageID(const String& nodeId) {
  messageCounter++;
  return nodeId + ":" + String(messageCounter);
}

// Function to compute CRC-16-CCITT
uint16_t crc16_ccitt(const uint8_t *buf, size_t len) {
  uint16_t crc = 0xFFFF;
  for (size_t i = 0; i < len; i++) {
    crc ^= (uint16_t)buf[i] << 8;
    for (uint8_t j = 0; j < 8; j++) {
      if (crc & 0x8000)
        crc = (crc << 1) ^ 0x1021;
      else
        crc <<= 1;
    }
  }
  return crc;
}

// Function to construct a unified message with originatorID, relayID, and CRC
String constructMessage(const String& messageID, const String& originatorID, const String& sender, const String& content, const String& relayID) {
  String messageWithoutCRC = messageID + "|" + originatorID + "|" + sender + "|" + content + "|" + relayID;
  // Compute CRC over messageWithoutCRC
  uint16_t crc = crc16_ccitt((const uint8_t *)messageWithoutCRC.c_str(), messageWithoutCRC.length());
  char crcStr[5]; // 4 hex digits plus null terminator
  sprintf(crcStr, "%04X", crc);
  String fullMessage = messageWithoutCRC + "|" + String(crcStr);
  return fullMessage;
}

// **New Data Structure for LoRa Nodes**
struct LoRaNode {
  String nodeId;
  int lastRSSI;
  float lastSNR;
  uint64_t lastSeen;
};

// Container to hold all LoRa nodes
std::map<String, LoRaNode> loraNodes;

unsigned long lastCleanupTime = 0;
const unsigned long cleanupInterval = 60000; // 1 minute in milliseconds

void cleanupLoRaNodes() {
  uint64_t currentTime = millis();
  const uint64_t timeout = 960000; // 16 minutes in milliseconds

  for (auto it = loraNodes.begin(); it != loraNodes.end(); ) {
    if (currentTime - it->second.lastSeen > timeout) {
      Serial.printf("[LoRa Nodes] Removing inactive LoRa node: %s\n", it->first.c_str());
      it = loraNodes.erase(it);
    } else {
      ++it;
    }
  }
}

// Function to add a message with a unique ID and size limit
void addMessage(const String& nodeId, const String& messageID, const String& sender, String content, const String& source, const String& relayID, int rssi = 0, float snr = 0.0) {
  const int maxMessageLength = 100;

  if (content.length() > maxMessageLength) {
    Serial.println("Message is too long, truncating...");
    content = content.substring(0, maxMessageLength);
  }

  auto& status = messageTransmissions[messageID];

  if (status.addedToMessages) {
    Serial.println("Message already exists in view, skipping addition...");
    return;
  }

  String finalSource = "";
  if (nodeId != String(getNodeId())) {
    finalSource = source;
  }

  Message newMessage = {nodeId, sender, content, finalSource, messageID, relayID, rssi, snr};

  messages.insert(messages.begin(), newMessage);
  status.addedToMessages = true;

  if (messages.size() > maxMessages) {
    messages.pop_back();
  }

  Serial.printf("Message added: NodeID: %s, Sender: %s, Content: %s, Source: %s, MessageID: %s, RelayID: %s, RSSI: %d, SNR: %.2f\n",
                nodeId.c_str(), sender.c_str(), content.c_str(), finalSource.c_str(), messageID.c_str(), relayID.c_str(), rssi, snr);
}

// **New Function: Schedule LoRa Transmission**
void scheduleLoRaTransmission(String message) {
    // Extract and update relayID
    int lastSeparator = message.lastIndexOf('|');
    if (lastSeparator == -1) {
        Serial.println("[LoRa Schedule] Invalid message format (no CRC).");
        return;
    }

    String crcStr = message.substring(lastSeparator + 1);
    String messageWithoutCRC = message.substring(0, lastSeparator);

    // Compute CRC over messageWithoutCRC
    uint16_t receivedCRC = (uint16_t)strtol(crcStr.c_str(), NULL, 16);
    uint16_t computedCRC = crc16_ccitt((const uint8_t *)messageWithoutCRC.c_str(), messageWithoutCRC.length());

    if (receivedCRC != computedCRC) {
        Serial.printf("[LoRa Schedule] CRC mismatch. Received: %04X, Computed: %04X\n", receivedCRC, computedCRC);
        return;
    }

    int firstSeparator = messageWithoutCRC.indexOf('|');
    int secondSeparator = messageWithoutCRC.indexOf('|', firstSeparator + 1);
    int thirdSeparator = messageWithoutCRC.indexOf('|', secondSeparator + 1);
    int fourthSeparator = messageWithoutCRC.indexOf('|', thirdSeparator + 1);

    if (firstSeparator == -1 || secondSeparator == -1 || thirdSeparator == -1 || fourthSeparator == -1) {
        Serial.println("[LoRa Schedule] Invalid message format.");
        return;
    }

    String messageID = messageWithoutCRC.substring(0, firstSeparator);
    String originatorID = messageWithoutCRC.substring(firstSeparator + 1, secondSeparator);
    String senderID = messageWithoutCRC.substring(secondSeparator + 1, thirdSeparator);
    String messageContent = messageWithoutCRC.substring(thirdSeparator + 1, fourthSeparator);
    // We don't need the old relayID here

    // Update relayID
    String newRelayID = String(getNodeId());
    String updatedMessage = constructMessage(messageID, originatorID, senderID, messageContent, newRelayID);

    fullMessage = updatedMessage;
    loRaTransmitDelay = millis() + random(2201, 5000); // Set delay between 2201ms and 5000ms
    Serial.printf("[LoRa Schedule] Message scheduled for LoRa transmission after %lu ms: %s\n", 
                loRaTransmitDelay - millis(), updatedMessage.c_str());
}

// **Modified Function: Transmit via WiFi**
void transmitViaWiFi(const String& message) {
  Serial.printf("[WiFi Tx] Preparing to transmit: %s\n", message.c_str());

  int separatorIndex = message.indexOf('|');
  if (separatorIndex == -1) {
    Serial.println("[WiFi Tx] Invalid message format.");
    return;
  }
  String messageID = message.substring(0, separatorIndex);

  auto& status = messageTransmissions[messageID];
  if (status.transmittedViaWiFi) {
    Serial.println("[WiFi Tx] Skipping retransmission via WiFi.");
    return;
  }

  mesh.sendBroadcast(message);
  status.transmittedViaWiFi = true;
  Serial.printf("[WiFi Tx] Message transmitted via WiFi: %s\n", message.c_str());
}

// Function to check and enforce duty cycle (for LoRa only)
bool isDutyCycleAllowed() {
  if (bypassDutyCycle) {
    dutyCycleActive = false;
    return true;
  }

  if (millis() > last_tx + minimum_pause) {
    dutyCycleActive = false;
  } else {
    dutyCycleActive = true;
  }

  return !dutyCycleActive;
}

long lastTxTimeMillis = -1;  // Initialized to -1, meaning no transmission yet

// Function to update the display with current status (if OLED is enabled)
void updateDisplay(long txTimeMillis = -1) {
  display.clear();
  display.setFont(ArialMT_Plain_10);  // Set to a slightly larger but still readable font
  int16_t titleWidth = display.getStringWidth("Meshify 1.0");
  display.drawString((128 - titleWidth) / 2, 0, "Meshify 1.0");
  display.drawString(0, 13, "Node ID: " + String(getNodeId()));
  display.drawString(0, 27, "Mesh Nodes: " + String(getNodeCount()));

  // Show whether LoRa transmission is allowed based on duty cycle
  if (dutyCycleActive) {
    display.drawString(0, 40, "Duty Cycle Limit Reached!");
  } else {
    display.drawString(0, 40, "LoRa Tx Allowed");
  }

  // If a new valid transmission time is provided, store it
  if (txTimeMillis >= 0) {
    lastTxTimeMillis = txTimeMillis;
  }

  // If we have a valid lastTxTimeMillis, display it
  if (lastTxTimeMillis >= 0) {
    String txMessage = "TxOK (" + String(lastTxTimeMillis) + " ms)";
    int16_t txMessageWidth = display.getStringWidth(txMessage);
    display.drawString((128 - txMessageWidth) / 2, 54, txMessage);  // Bottom middle
  }

  display.display();
}

void transmitWithDutyCycle(const String& message) {
  if (millis() < loRaTransmitDelay) {
    Serial.println("[LoRa Tx] LoRa delay not expired, waiting...");
    return;
  }

  int separatorIndex = message.indexOf('|');
  if (separatorIndex == -1) {
    Serial.println("[LoRa Tx] Invalid message format.");
    return;
  }
  String messageID = message.substring(0, separatorIndex);

  auto& status = messageTransmissions[messageID];
  if (status.transmittedViaLoRa) {
    Serial.println("[LoRa Tx] Skipping retransmission via LoRa.");
    return;
  }

  if (isDutyCycleAllowed()) {
    tx_time = millis();

    Serial.printf("[LoRa Tx] Preparing to transmit: %s\n", message.c_str());

    heltec_led(50);
    int transmitStatus = radio.transmit(message.c_str());
    tx_time = millis() - tx_time;
    heltec_led(0);

    if (transmitStatus == RADIOLIB_ERR_NONE) {
      Serial.printf("[LoRa Tx] Message transmitted successfully via LoRa (%i ms)\n", (int)tx_time);
      status.transmittedViaLoRa = true;

      calculateDutyCyclePause(tx_time);
      last_tx = millis();

      updateDisplay(tx_time);

      delay(200);
      radio.startReceive();

      transmitViaWiFi(message);
    } else {
      Serial.printf("[LoRa Tx] Transmission via LoRa failed (%i)\n", transmitStatus);
    }
  } else {
    Serial.printf("[LoRa Tx] Duty cycle limit reached, please wait %i sec.\n",
                  (int)((minimum_pause - (millis() - last_tx)) / 1000) + 1);
  }
}

// **Heartbeat Variables**
unsigned long lastHeartbeatTime = 0;
const unsigned long heartbeatInterval = 900000; // 15 minutes in milliseconds

// Function to send heartbeat via LoRa
void sendHeartbeat() {
  // Construct a heartbeat message
  String heartbeatWithoutCRC = "HEARTBEAT|" + String(getNodeId());
  // Compute CRC
  uint16_t crc = crc16_ccitt((const uint8_t *)heartbeatWithoutCRC.c_str(), heartbeatWithoutCRC.length());
  char crcStr[5];
  sprintf(crcStr, "%04X", crc);
  String heartbeatMessage = heartbeatWithoutCRC + "|" + String(crcStr);
  
  // Transmit the heartbeat message via LoRa
  if (isDutyCycleAllowed()) {
    tx_time = millis();
    Serial.printf("[Heartbeat Tx] Sending heartbeat: %s\n", heartbeatMessage.c_str());
    heltec_led(50);
    int transmitStatus = radio.transmit(heartbeatMessage.c_str());
    tx_time = millis() - tx_time;
    heltec_led(0);

    if (transmitStatus == RADIOLIB_ERR_NONE) {
      Serial.printf("[Heartbeat Tx] Heartbeat transmitted successfully via LoRa (%i ms)\n", (int)tx_time);
      calculateDutyCyclePause(tx_time);
      last_tx = millis();
      updateDisplay(tx_time);
      delay(200);
      radio.startReceive();
    } else {
      Serial.printf("[Heartbeat Tx] Transmission via LoRa failed (%i)\n", transmitStatus);
    }
  } else {
    Serial.printf("[Heartbeat Tx] Duty cycle limit reached, cannot send heartbeat now.\n");
  }
}

void setup() {
  Serial.begin(115200);

  heltec_setup();
  heltec_led(0);

  setupLora();

  WiFi.softAP(MESH_SSID, MESH_PASSWORD);
  WiFi.setTxPower(WIFI_POWER_19_5dBm);
  WiFi.setSleep(false);
  initMesh();
  setupServerRoutes();
  server.begin();
  dnsServer.start(53, "*", WiFi.softAPIP());

  display.init();
  display.flipScreenVertically();
  display.clear();
  display.setFont(ArialMT_Plain_10);
  updateDisplay();

  esp_task_wdt_init(30, true);
  esp_task_wdt_add(NULL);

  randomSeed(analogRead(0));  // Initialize random seed
}

void loop() {
  esp_task_wdt_reset();

  heltec_loop();

  mesh.update();

  if (rxFlag) {
    rxFlag = false;
    String message;
    int state = radio.readData(message);
    if (state == RADIOLIB_ERR_NONE) {
      Serial.printf("[LoRa Rx] Received message: %s\n", message.c_str());

      // Extract the CRC from the message
      int lastSeparatorIndex = message.lastIndexOf('|');
      if (lastSeparatorIndex == -1) {
        Serial.println("[LoRa Rx] Invalid message format (no CRC).");
      } else {
        String crcStr = message.substring(lastSeparatorIndex + 1);
        String messageWithoutCRC = message.substring(0, lastSeparatorIndex);

        // Compute CRC over messageWithoutCRC
        uint16_t receivedCRC = (uint16_t)strtol(crcStr.c_str(), NULL, 16);
        uint16_t computedCRC = crc16_ccitt((const uint8_t *)messageWithoutCRC.c_str(), messageWithoutCRC.length());

        if (receivedCRC != computedCRC) {
          Serial.printf("[LoRa Rx] CRC mismatch. Received: %04X, Computed: %04X\n", receivedCRC, computedCRC);
        } else {
          Serial.println("[LoRa Rx] CRC valid.");

          if (messageWithoutCRC.startsWith("HEARTBEAT|")) {
            // Process heartbeat message
            String senderNodeId = messageWithoutCRC.substring(strlen("HEARTBEAT|"));
            Serial.printf("[LoRa Rx] Received heartbeat from node: %s\n", senderNodeId.c_str());
            // Get RSSI and SNR
            int rssi = radio.getRSSI();
            float snr = radio.getSNR();
            // Update the lastSeen time and RSSI/SNR for the node
            uint64_t currentTime = millis();

            // **Avoid updating own node's entry**
            if (senderNodeId != String(getNodeId())) {
              if (loraNodes.find(senderNodeId) == loraNodes.end()) {
                LoRaNode newNode = {senderNodeId, rssi, snr, currentTime};
                loraNodes[senderNodeId] = newNode;
                Serial.printf("[LoRa Nodes] New LoRa node added: %s (RSSI: %d, SNR: %.2f)\n", senderNodeId.c_str(), rssi, snr);
              } else {
                loraNodes[senderNodeId].lastRSSI = rssi;
                loraNodes[senderNodeId].lastSNR = snr;
                loraNodes[senderNodeId].lastSeen = currentTime;
                Serial.printf("[LoRa Nodes] Updated LoRa node: %s (Heartbeat, RSSI: %d, SNR: %.2f)\n", senderNodeId.c_str(), rssi, snr);
              }
            } else {
              Serial.println("[LoRa Rx] Received own heartbeat, ignoring...");
            }
            // Do not relay heartbeat
          } else {
            // Process received message
            int firstSeparator = messageWithoutCRC.indexOf('|');
            int secondSeparator = messageWithoutCRC.indexOf('|', firstSeparator + 1);
            int thirdSeparator = messageWithoutCRC.indexOf('|', secondSeparator + 1);
            int fourthSeparator = messageWithoutCRC.indexOf('|', thirdSeparator + 1);

            if (firstSeparator == -1 || secondSeparator == -1 || thirdSeparator == -1 || fourthSeparator == -1) {
              Serial.println("[LoRa Rx] Invalid message format.");
            } else {
              String messageID = messageWithoutCRC.substring(0, firstSeparator);
              String originatorID = messageWithoutCRC.substring(firstSeparator + 1, secondSeparator);
              String senderID = messageWithoutCRC.substring(secondSeparator + 1, thirdSeparator);
              String messageContent = messageWithoutCRC.substring(thirdSeparator + 1, fourthSeparator);
              String relayID = messageWithoutCRC.substring(fourthSeparator + 1);

              int rssi = radio.getRSSI();
              float snr = radio.getSNR();

              Serial.printf("[LoRa Rx] RSSI: %d dBm, SNR: %.2f dB\n", rssi, snr);

              // **Check if originatorID is own node ID**
              if (originatorID == String(getNodeId())) {
                Serial.println("[LoRa Rx] Received own message, ignoring...");
              } else {
                auto& status = messageTransmissions[messageID];
                if (status.transmittedViaWiFi && status.transmittedViaLoRa) {
                  Serial.println("[LoRa Rx] Message already retransmitted via both WiFi and LoRa, ignoring...");
                } else {
                  addMessage(originatorID, messageID, senderID, messageContent, "[LoRa]", relayID, rssi, snr);

                  if (!status.transmittedViaLoRa) {
                    scheduleLoRaTransmission(message);
                  }

                  uint64_t currentTime = millis();

                  // **Avoid updating own node's entry**
                  if (relayID != String(getNodeId())) {
                    // Update loraNodes using relayID
                    if (loraNodes.find(relayID) == loraNodes.end()) {
                      LoRaNode newNode = {relayID, rssi, snr, currentTime};
                      loraNodes[relayID] = newNode;
                      Serial.printf("[LoRa Nodes] New node added: %s (RSSI: %d, SNR: %.2f)\n", relayID.c_str(), rssi, snr);
                    } else {
                      loraNodes[relayID].lastRSSI = rssi;
                      loraNodes[relayID].lastSNR = snr;
                      loraNodes[relayID].lastSeen = currentTime;
                      Serial.printf("[LoRa Nodes] Updated node: %s (RSSI: %d, SNR: %.2f)\n", relayID.c_str(), rssi, snr);
                    }
                  } else {
                    Serial.println("[LoRa Nodes] RelayID is own node ID, not updating loraNodes.");
                  }

                  // Ensure originatorID is in loraNodes, but do not update RSSI/SNR or lastSeen
                  if (originatorID != String(getNodeId())) {
                    if (loraNodes.find(originatorID) == loraNodes.end()) {
                      LoRaNode newOriginatorNode = {originatorID, 0, 0.0, 0};
                      loraNodes[originatorID] = newOriginatorNode;
                      Serial.printf("[LoRa Nodes] New originator node added: %s\n", originatorID.c_str());
                    }
                  } else {
                    Serial.println("[LoRa Nodes] OriginatorID is own node ID, not adding to loraNodes.");
                  }
                }
              }
            }
          }
        }
      }
      radio.startReceive();
    } else {
      Serial.printf("[LoRa Rx] Receive failed, code %d\n", state);
      radio.startReceive();
    }
  }

  if (!fullMessage.isEmpty() && millis() >= loRaTransmitDelay) {
    transmitWithDutyCycle(fullMessage);
    fullMessage = "";
  }

  updateMeshData();
  updateDisplay();
  dnsServer.processNextRequest();

    // **Add this block to periodically clean up old LoRa nodes**
  if (millis() - lastCleanupTime >= cleanupInterval) {
    cleanupLoRaNodes();
    lastCleanupTime = millis();
  }

  // Send heartbeat every 15 minutes
  if (millis() - lastHeartbeatTime >= heartbeatInterval) {
    sendHeartbeat();
    lastHeartbeatTime = millis();
  }
}

// Meshify Initialization Function
void initMesh() {
  mesh.setDebugMsgTypes(ERROR | STARTUP | CONNECTION);
  mesh.init(MESH_SSID, MESH_PASSWORD, MESH_PORT);
  mesh.onReceive(receivedCallback);

  mesh.onChangedConnections([]() {
    updateMeshData();
    updateDisplay();
  });

  mesh.setContainsRoot(false);
}

void receivedCallback(uint32_t from, String& message) {
  Serial.printf("[WiFi Rx] Received message from %u: %s\n", from, message.c_str());

  // Extract the CRC from the message
  int lastSeparatorIndex = message.lastIndexOf('|');
  if (lastSeparatorIndex == -1) {
    Serial.println("[WiFi Rx] Invalid message format (no CRC).");
    return;
  }
  String crcStr = message.substring(lastSeparatorIndex + 1);
  String messageWithoutCRC = message.substring(0, lastSeparatorIndex);

  // Compute CRC over messageWithoutCRC
  uint16_t receivedCRC = (uint16_t)strtol(crcStr.c_str(), NULL, 16);
  uint16_t computedCRC = crc16_ccitt((const uint8_t *)messageWithoutCRC.c_str(), messageWithoutCRC.length());

  if (receivedCRC != computedCRC) {
    Serial.printf("[WiFi Rx] CRC mismatch. Received: %04X, Computed: %04X\n", receivedCRC, computedCRC);
    return;
  } else {
    Serial.println("[WiFi Rx] CRC valid.");

    int firstSeparator = messageWithoutCRC.indexOf('|');
    int secondSeparator = messageWithoutCRC.indexOf('|', firstSeparator + 1);
    int thirdSeparator = messageWithoutCRC.indexOf('|', secondSeparator + 1);
    int fourthSeparator = messageWithoutCRC.indexOf('|', thirdSeparator + 1);

    if (firstSeparator == -1 || secondSeparator == -1 || thirdSeparator == -1 || fourthSeparator == -1) {
      Serial.println("[WiFi Rx] Invalid message format.");
      return;
    }

    String messageID = messageWithoutCRC.substring(0, firstSeparator);
    String originatorID = messageWithoutCRC.substring(firstSeparator + 1, secondSeparator);
    String senderID = messageWithoutCRC.substring(secondSeparator + 1, thirdSeparator);
    String messageContent = messageWithoutCRC.substring(thirdSeparator + 1, fourthSeparator);
    String relayID = messageWithoutCRC.substring(fourthSeparator + 1);

    if (originatorID == String(getNodeId())) {
      Serial.println("[WiFi Rx] Received own message, processing for node list but not retransmitting...");
      addMessage(originatorID, messageID, senderID, messageContent, "[WiFi]", relayID);
      return;
    }

    auto& status = messageTransmissions[messageID];
    if (status.transmittedViaWiFi && status.transmittedViaLoRa) {
      Serial.println("[WiFi Rx] Message already retransmitted via both WiFi and LoRa, ignoring...");
      return;
    }

    Serial.printf("[WiFi Rx] Adding message: %s\n", message.c_str());
    addMessage(originatorID, messageID, senderID, messageContent, "[WiFi]", relayID);

    if (!status.transmittedViaLoRa) {
      scheduleLoRaTransmission(message);
    }
  }
}

const char mainPageHtml[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="en">
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1.0">
  <title>Meshify Chat Room</title>
  <style>
    body {
      font-family: Arial, sans-serif;
      background-color: #f4f7f6;
      margin: 0;
      padding: 0;
      display: flex;
      flex-direction: column;
      align-items: center;
      justify-content: flex-start;
      height: 100vh;
      overflow: hidden;
    }
    .warning {
      color: red;
      font-weight: normal;
      font-size: 0.9em;
      padding: 10px;
      background-color: #fff3f3;
      border: 1px solid red;
      max-width: 100%;
      text-align: center;
      border-radius: 5px;
      margin: 0;
    }
    h2 {
      color: #333;
      margin: 10px 0 5px;
      font-size: 1.2em;
    }
    #chat-container {
      background-color: #fff;
      width: 100%;
      max-width: 600px;
      height: calc(100vh - 250px);
      margin-top: 10px;
      box-shadow: 0 0 10px rgba(0,0,0,0.1);
      overflow-y: auto;
      padding: 10px;
      border-radius: 10px;
      box-sizing: border-box;
    }
    #messageForm {
      width: 100%;
      max-width: 600px;
      display: flex;
      position: fixed;
      bottom: 0;
      background-color: #fff;
      padding: 10px;
      box-shadow: 0 -2px 10px rgba(0, 0, 0, 0.1);
      box-sizing: border-box;
    }
    #nameInput, #messageInput {
      width: 40%;
      padding: 10px;
      margin-right: 5px;
      border: 1px solid #ccc;
      border-radius: 5px;
      box-sizing: border-box;
    }
    #messageInput {
      width: 60%;
    }
    #messageForm input[type=submit] {
      background-color: #007bff;
      color: white;
      border: none;
      padding: 10px;
      cursor: pointer;
      border-radius: 5px;
    }
    #messageList {
      list-style-type: none;
      padding: 0;
      margin: 0;
      display: flex;
      flex-direction: column;
    }
    .message {
      display: flex;
      flex-direction: column;
      margin: 5px 0;
      padding: 8px;
      border-radius: 5px;
      width: 80%;
      box-sizing: border-box;
      border: 2px solid;
      word-wrap: break-word;
    }
    .message.sent {
      align-self: flex-end;
      border-color: green;
      background-color: #eaffea;
    }
    .message.received.wifi {
      align-self: flex-start;
      border-color: blue;
      background-color: #e7f0ff;
    }
    .message.received.lora {
      align-self: flex-start;
      border-color: orange;
      background-color: #fff4e0;
    }
    .message-nodeid {
      font-size: 0.7em; /* Make the Node ID smaller */
      color: #666;
    }
    .message-rssi-snr {
      font-size: 0.7em;
      color: #999;
      text-align: right;
      margin-top: 2px;
    }
    .message-content {
      font-size: 0.85em; /* Message content remains larger than Node ID */
      color: #333;
    }
    .message-time {
      font-size: 0.7em; /* Smaller font size for the time */
      color: #999;
      text-align: right;
      margin-top: 5px;
    }
    #deviceCount {
      margin: 5px 0;
      font-weight: normal;
      font-size: 0.9em;
    }
    #deviceCount a {
      color: #007bff;
      text-decoration: none;
    }
    #deviceCount a:hover {
      text-decoration: underline;
    }
  </style>

  <script>
    const messageTimestamps = {};  // Object to store timestamps for each message

    // Function to format the timestamp for display
    function formatTimestamp(timestamp) {
      const date = new Date(timestamp);
      return date.toLocaleTimeString();  // Adjust the format as needed
    }

    // Function to send a message via the form
    function sendMessage(event) {
      event.preventDefault();

      const nameInput = document.getElementById('nameInput');
      const messageInput = document.getElementById('messageInput');
      const sendButton = document.getElementById('sendButton'); // Get the send button

      const sender = nameInput.value;  // The sender (your name)
      const msg = messageInput.value;  // The message content

      if (!sender || !msg) {
        alert('Please enter both a name and a message.');
        return;
      }

      localStorage.setItem('username', sender);  // Save the username for future use

      const formData = new URLSearchParams();
      formData.append('sender', sender);  // Send the sender's name
      formData.append('msg', msg);        // Send the message content

      // Disable the send button for 15 seconds to prevent spam
      sendButton.disabled = true;
      sendButton.value = 'Wait 15s';

      fetch('/update', {
        method: 'POST',
        body: formData
      })
      .then(response => {
        if (!response.ok) throw new Error('Failed to send message');
        messageInput.value = '';  // Clear the input field after sending
        fetchData();  // Fetch new messages to update the UI

        // Re-enable the send button after 15 seconds
        setTimeout(() => {
          sendButton.disabled = false;
          sendButton.value = 'Send';
        }, 15000);
      })
      .catch(error => {
        console.error('Error sending message:', error);
        sendButton.disabled = false;
        sendButton.value = 'Send';
        alert('Failed to send message. Please try again.');
      });
    }

    // Function to fetch messages and update the UI
    function fetchData() {
      fetch('/messages')
        .then(response => response.json())
        .then(data => {
          const ul = document.getElementById('messageList');
          ul.innerHTML = '';  // Clear the list before appending new messages

          const currentNodeId = localStorage.getItem('nodeId');  // Get the current node ID

          data.messages.forEach(msg => {
            const li = document.createElement('li');
            li.classList.add('message');

            // Determine whether the message is sent by the current user (based on node ID)
            const isSentByCurrentNode = msg.nodeId === currentNodeId;
            if (isSentByCurrentNode) {
              li.classList.add('sent');
            } else {
              li.classList.add('received');
              if (msg.source === '[LoRa]') {
                li.classList.add('lora');
              } else {
                li.classList.add('wifi');
              }
            }

            // Store the timestamp when the message is received, if it doesn't already exist
            if (!messageTimestamps[msg.messageID]) {
              messageTimestamps[msg.messageID] = Date.now();  // Set the timestamp when the message is first received
            }

            // Format the timestamp for display
            const timestamp = formatTimestamp(messageTimestamps[msg.messageID]);

            // Display node ID and sender's name (for both sent and received messages)
            const nodeIdHtml = `Node Id: ${msg.nodeId}`;  // Always show the node ID
            const senderHtml = `<strong>${msg.sender || 'Unknown'}:</strong> `;  // Show sender name (or 'Unknown' if missing)

            // Display RSSI and SNR if available (for LoRa messages)
            let rssiSnrHtml = '';
            if (msg.source === '[LoRa]' && msg.rssi !== undefined && msg.snr !== undefined) {
              rssiSnrHtml = `<span class="message-rssi-snr">RSSI: ${msg.rssi} dBm, SNR: ${msg.snr} dB</span>`;
            }

            // Insert the message into the HTML
            li.innerHTML = `
              <span class="message-nodeid">${nodeIdHtml}</span>  <!-- Display node ID -->
              <div class="message-content">${senderHtml}${msg.content}</div>  <!-- Display sender name and message content -->
              <span class="message-time">${timestamp}</span>  <!-- Display message time -->
              ${rssiSnrHtml}  <!-- Display RSSI and SNR if available -->
            `;

            ul.appendChild(li);  // Append the message to the list
          });

          // Scroll to the bottom of the message list to show the most recent message
          ul.scrollTop = ul.scrollHeight;
        })
        .catch(error => console.error('Error fetching messages:', error));

      // Fetch the current device count and node ID
      fetch('/deviceCount')
        .then(response => response.json())
        .then(data => {
          localStorage.setItem('nodeId', data.nodeId);  // Store the node ID locally
          document.getElementById('deviceCount').innerHTML = 
            `Mesh Nodes: <a href="/nodes">${data.totalCount}</a>, Node ID: ${data.nodeId}`;
        })
        .catch(error => console.error('Error fetching device count:', error));
    }

    // Event listener for when the page is loaded
    window.onload = function() {
      const savedName = localStorage.getItem('username');
      if (savedName) {
        document.getElementById('nameInput').value = savedName;
      }

      fetchData();  // Fetch the initial set of messages
      setInterval(fetchData, 5000);  // Fetch messages every 5 seconds
      document.getElementById('messageForm').addEventListener('submit', sendMessage);
    };
  </script>
</head>
<body>
  <div class="warning">For your safety, do not share your location or any personal information!</div>
  
  <h2>Meshify Chat</h2>
  
  <div id="deviceCount">Mesh Nodes: 0</div>
  
  <div id="chat-container">
    <ul id="messageList"></ul>
  </div>
  
  <form id="messageForm">
    <input type="text" id="nameInput" name="sender" placeholder="Your name" maxlength="15" required>
    <input type="text" id="messageInput" name="msg" placeholder="Your message" maxlength="100" required>
    <input type="submit" id="sendButton" value="Send">
  </form>
</body>
</html>
)rawliteral";

const char nodesPageHtml[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="en">
<head>
  <meta name="viewport" content="width=device-width, initial-scale=1.0">
  <style>
    body { 
      font-family: Arial, sans-serif; 
      margin: 0; 
      padding: 20px; 
      text-align: center; 
      background-color: #f4f7f6;
    }
    h2 { 
      color: #333; 
      margin-top: 20px;
    }
    h3 {
      color: #333; 
      margin-top: 20px;
      text-align: left;
      max-width: 500px;
      margin-left: auto;
      margin-right: auto;
    }
    ul { 
      list-style-type: none; 
      padding: 0; 
      margin: 10px auto; 
      max-width: 500px; 
    }
    .node {
      display: flex;
      flex-direction: column;
      justify-content: center;
      align-items: flex-start;
      margin: 10px 0;
      padding: 15px;
      border-radius: 10px;
      width: 100%;
      max-width: 500px;
      box-sizing: border-box;
      border: 2px solid;
      text-align: left;
      font-size: 0.85em; /* Matches .message-content */
      color: #333;       /* Matches .message-content */
      font-weight: normal; /* Removes bold styling */
      background-color: #fff; /* Default background */
    }
    .node.wifi {
      background-color: #e7f0ff; /* Light blue background for Wi-Fi nodes */
      border-color: blue;        /* Blue border for Wi-Fi nodes */
    }
    .node.lora {
      background-color: #fff4e0; /* Light orange background for LoRa nodes */
      border-color: orange;      /* Orange border for LoRa nodes */
    }
    #nodeCount { 
      margin: 20px auto; 
      max-width: 500px; 
      font-weight: bold;
      text-align: left;
    }
    a {
      margin-top: 20px;
      display: inline-block;
      text-decoration: none;
      color: #007bff;
      font-weight: bold;
    }
    a:hover {
      text-decoration: underline;
    }
  </style>
  <script>
    function fetchNodes() {
      fetch('/nodesData')
        .then(response => response.json())
        .then(data => {
          // Update WiFi Nodes
          const wifiUl = document.getElementById('wifiNodeList');
          wifiUl.innerHTML = '';
          data.wifiNodes.forEach((node, index) => {
            const li = document.createElement('li');
            li.classList.add('node', 'wifi');
            li.textContent = 'Node ' + (index + 1) + ': ' + node;
            wifiUl.appendChild(li);
          });

          // Update LoRa Nodes with "Last Seen" information
          const loraUl = document.getElementById('loraNodeList');
          loraUl.innerHTML = '';
          data.loraNodes.forEach((node, index) => {
            const li = document.createElement('li');
            li.classList.add('node', 'lora');
            li.innerHTML = `
              <strong>Node ${index + 1}:</strong> ${node.nodeId}<br>
              RSSI: ${node.lastRSSI} dBm, SNR: ${node.lastSNR} dB<br>
              Last seen: ${node.lastSeen}
            `;
            loraUl.appendChild(li);
          });

          // Update node counts
          document.getElementById('nodeCount').textContent = 
            'WiFi Nodes Connected: ' + data.wifiNodes.length + 
            ', LoRa Nodes Detected (15 min): ' + data.loraNodes.length;
        })
        .catch(error => console.error('Error fetching nodes:', error));
    }

    window.onload = function() {
      fetchNodes();
      setInterval(fetchNodes, 5000); // Refresh node list every 5 seconds
    };
  </script>
</head>
<body>
  <h2>Meshify Nodes</h2>
  <div id="nodeCount">WiFi Nodes Connected: 0, LoRa Nodes Detected: 0</div>
  
  <h3>WiFi Nodes</h3>
  <ul id="wifiNodeList"></ul>
  
  <h3>LoRa Nodes</h3>
  <ul id="loraNodeList"></ul>
  
  <a href="/">Back to Main Page</a>
</body>
</html>
)rawliteral";

// Server Routes Setup
void setupServerRoutes() {
  server.on("/", HTTP_GET, [](AsyncWebServerRequest* request) {
    serveHtml(request, mainPageHtml);
  });

  server.on("/nodes", HTTP_GET, [](AsyncWebServerRequest* request) {
    serveHtml(request, nodesPageHtml);
  });

  server.on("/messages", HTTP_GET, [](AsyncWebServerRequest* request) {
    String json = "[";
    bool first = true;
    for (const auto& msg : messages) {
      if (!first) json += ",";
      // Add nodeId, messageID, and relayID to the JSON object
      json += "{\"nodeId\":\"" + msg.nodeId + "\",\"sender\":\"" + msg.sender + "\",\"content\":\"" + msg.content + "\",\"source\":\"" + msg.source + "\",\"messageID\":\"" + msg.messageID + "\",\"relayID\":\"" + msg.relayID + "\"";
      if (msg.source == "[LoRa]") {
        json += ",\"rssi\":" + String(msg.rssi) + ",\"snr\":" + String(msg.snr, 2);
      }
      json += "}";
      first = false;
    }
    json += "]";
    request->send(200, "application/json", "{\"messages\":" + json + "}");
  });

  server.on("/deviceCount", HTTP_GET, [](AsyncWebServerRequest* request) {
    updateMeshData();
    request->send(200, "application/json", "{\"totalCount\":" + String(getNodeCount()) + ", \"nodeId\":\"" + String(getNodeId()) + "\"}");
  });

  server.on("/nodesData", HTTP_GET, [](AsyncWebServerRequest* request) {
    updateMeshData();
    String json = "{\"wifiNodes\":[";

    // Update WiFi Node List
    auto wifiNodeList = mesh.getNodeList();
    bool firstWifi = true;
    for (auto node : wifiNodeList) {
      if (!firstWifi) json += ",";
      json += "\"" + String(node) + "\"";
      firstWifi = false;
    }
    json += "], \"loraNodes\":[";

    // Update LoRa Node List
    bool firstLora = true;
    uint64_t currentTime = millis();
    for (auto const& [nodeId, loraNode] : loraNodes) {
      if (!firstLora) json += ",";
      // Calculate the "last seen" time in seconds or minutes
      uint64_t lastSeenSeconds = (currentTime - loraNode.lastSeen) / 1000;
      String lastSeen = lastSeenSeconds < 60 
                          ? String(lastSeenSeconds) + " sec ago"
                          : String(lastSeenSeconds / 60) + " min ago";

      json += "{\"nodeId\":\"" + nodeId + "\",\"lastRSSI\":" + String(loraNode.lastRSSI) + ",\"lastSNR\":" + String(loraNode.lastSNR, 2) + ",\"lastSeen\":\"" + lastSeen + "\"}";
      firstLora = false;
    }
    json += "]}";

    request->send(200, "application/json", json);
  });

  server.on("/update", HTTP_POST, [](AsyncWebServerRequest* request) {
    String newMessage = "";
    String senderName = "";
    if (request->hasParam("msg", true)) {
      newMessage = request->getParam("msg", true)->value();
    }
    if (request->hasParam("sender", true)) {
      senderName = request->getParam("sender", true)->value();
    }

    newMessage.replace("<", "&lt;");
    newMessage.replace(">", "&gt;");
    senderName.replace("<", "&lt;");
    senderName.replace(">", "&gt;");

    // Generate a new message ID
    String messageID = generateMessageID(String(getNodeId()));

    // Set relayID to own node ID
    String relayID = String(getNodeId());

    // Construct the full message with the message ID, originatorID, and relayID
    String constructedMessage = constructMessage(messageID, String(getNodeId()), senderName, newMessage, relayID);

    // Add the message with source "[LoRa]"
    addMessage(String(getNodeId()), messageID, senderName, newMessage, "[LoRa]", relayID);
    Serial.printf("[LoRa Tx] Adding message: %s\n", constructedMessage.c_str());

    // Schedule LoRa transmission
    scheduleLoRaTransmission(constructedMessage);

    request->redirect("/");
  });
}

// HTML Serving Function
void serveHtml(AsyncWebServerRequest* request, const char* htmlContent) {
  request->send(200, "text/html", htmlContent);
}

// Centralized mesh data functions
int getNodeCount() {
  return totalNodeCount;
}

uint32_t getNodeId() {
  return currentNodeId;
}

// Function to update centralized mesh data
void updateMeshData() {
  mesh.update();
  totalNodeCount = mesh.getNodeList().size();
  currentNodeId = mesh.getNodeId();
}
