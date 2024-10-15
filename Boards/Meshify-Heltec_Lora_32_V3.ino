//Make sure all nodes use this same updated version to avoid problems.
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
#define FREQUENCY 869.4000 //Changed to start of Eu868 10% band 
#define BANDWIDTH 250.0 //using the full band
#define SPREADING_FACTOR 11 //we can goto 12 but 11 seems fine.
#define TRANSMIT_POWER 22 //now using full power
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
  String nodeId;     // Node ID of the message sender
  String sender;
  String content;
  String source;     // Indicates message source (WiFi or LoRa)
  String messageID;  // Unique message ID
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

// Function to construct a unified message with originatorID
String constructMessage(const String& messageID, const String& originatorID, const String& sender, const String& content) {
  return messageID + "|" + originatorID + "|" + sender + "|" + content;
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

// **Function to clean up old LoRa nodes**
void cleanupLoRaNodes() {
  uint64_t currentTime = millis();
  const uint64_t timeout = 3.6e+6; // 1hr minutes in milliseconds

  for(auto it = loraNodes.begin(); it != loraNodes.end(); ) {
    if(currentTime - it->second.lastSeen > timeout) {
      Serial.printf("[LoRa Nodes] Removing inactive LoRa node: %s\n", it->first.c_str());
      it = loraNodes.erase(it);
    } else {
      ++it;
    }
  }
}

// Function to add a message with a unique ID and size limit
void addMessage(const String& nodeId, const String& messageID, const String& sender, String content, const String& source, int rssi = 0, float snr = 0.0) {
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

  Message newMessage = {nodeId, sender, content, finalSource, messageID, rssi, snr};

  messages.insert(messages.begin(), newMessage);
  status.addedToMessages = true;

  if (messages.size() > maxMessages) {
    messages.pop_back();
  }

  Serial.printf("Message added: NodeID: %s, Sender: %s, Content: %s, Source: %s, MessageID: %s, RSSI: %d, SNR: %.2f\n",
                nodeId.c_str(), sender.c_str(), content.c_str(), finalSource.c_str(), messageID.c_str(), rssi, snr);
}

// **New Function: Schedule LoRa Transmission**
void scheduleLoRaTransmission(String message) {
    int firstSeparator = message.indexOf('|');
    int secondSeparator = message.indexOf('|', firstSeparator + 1);
    int thirdSeparator = message.indexOf('|', secondSeparator + 1);

    if (firstSeparator == -1 || secondSeparator == -1 || thirdSeparator == -1) {
        Serial.println("[LoRa Tx] Invalid message format.");
        return;
    }

    String messageID = message.substring(0, firstSeparator);
    String originatorID = message.substring(firstSeparator + 1, secondSeparator);
    String currentSenderID = message.substring(secondSeparator + 1, thirdSeparator);
    String messageContent = message.substring(thirdSeparator + 1);

    String newSenderID = String(getNodeId());
    String updatedMessage = messageID + "|" + originatorID + "|" + newSenderID + "|" + messageContent;

    fullMessage = updatedMessage;
    loRaTransmitDelay = millis() + random(3000, 10001); // Set delay between 3000ms and 10000ms
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

  // Display last seen times for nodes
  uint64_t currentTime = millis();
  for (const auto& node : loraNodes) {
    uint64_t lastSeenSeconds = (currentTime - node.second.lastSeen) / 1000;
    String lastSeenMessage;
    if (lastSeenSeconds < 60) {
      lastSeenMessage = "Last seen: " + String(lastSeenSeconds) + " seconds ago";
    } else {
      lastSeenMessage = "Last seen: " + String(lastSeenSeconds / 60) + " minutes ago";
    }
    display.drawString(0, 54 + (node.second.nodeId.length() * 12), node.second.nodeId + " " + lastSeenMessage);
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

      int firstSeparator = message.indexOf('|');
      int secondSeparator = message.indexOf('|', firstSeparator + 1);
      int thirdSeparator = message.indexOf('|', secondSeparator + 1);

      if (firstSeparator == -1 || secondSeparator == -1 || thirdSeparator == -1) {
        Serial.println("[LoRa Rx] Invalid message format.");
      } else {
        String messageID = message.substring(0, firstSeparator);
        String originatorID = message.substring(firstSeparator + 1, secondSeparator);
        String senderID = message.substring(secondSeparator + 1, thirdSeparator);
        String messageContent = message.substring(thirdSeparator + 1);

        int rssi = radio.getRSSI();
        float snr = radio.getSNR();

        Serial.printf("[LoRa Rx] RSSI: %d dBm, SNR: %.2f dB\n", rssi, snr);

        if (senderID == String(getNodeId())) {
          Serial.println("[LoRa Rx] Received own message, ignoring...");
        } else {
          auto& status = messageTransmissions[messageID];
          if (status.transmittedViaWiFi && status.transmittedViaLoRa) {
            Serial.println("[LoRa Rx] Message already retransmitted via both WiFi and LoRa, ignoring...");
          } else {
            addMessage(originatorID, messageID, senderID, messageContent, "[LoRa]", rssi, snr);

            if (!status.transmittedViaLoRa) {
              scheduleLoRaTransmission(message);
            }

            if (loraNodes.find(originatorID) == loraNodes.end()) {
              LoRaNode newNode = {originatorID, rssi, snr, millis()};
              loraNodes[originatorID] = newNode;
              Serial.printf("[LoRa Nodes] New LoRa node added: %s\n", originatorID.c_str());
            } else {
              loraNodes[originatorID].lastRSSI = rssi;
              loraNodes[originatorID].lastSNR = snr;
              loraNodes[originatorID].lastSeen = millis();
              Serial.printf("[LoRa Nodes] Updated LoRa node: %s (RSSI: %d, SNR: %.2f)\n", originatorID.c_str(), rssi, snr);
            }

            if (loraNodes.find(senderID) == loraNodes.end()) {
              LoRaNode newRelayNode = {senderID, rssi, snr, millis()};
              loraNodes[senderID] = newRelayNode;
              Serial.printf("[LoRa Nodes] New relay node added: %s\n", senderID.c_str());
            } else {
              loraNodes[senderID].lastRSSI = rssi;
              loraNodes[senderID].lastSNR = snr;
              loraNodes[senderID].lastSeen = millis();
              Serial.printf("[LoRa Nodes] Updated relay node: %s (RSSI: %d, SNR: %.2f)\n", senderID.c_str(), rssi, snr);
            }
          }
        }
      }
    } else {
      Serial.printf("[LoRa Rx] Receive failed, code %d\n", state);
    }
    radio.startReceive();
  }

  if (!fullMessage.isEmpty() && millis() >= loRaTransmitDelay) {
    transmitWithDutyCycle(fullMessage);
    fullMessage = "";
  }

  updateMeshData();
  updateDisplay();
  dnsServer.processNextRequest();

  static unsigned long lastCleanup = 0;
  if (millis() - lastCleanup > 3.6e+6) {
    cleanupLoRaNodes();
    lastCleanup = millis();
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

void processReceivedMessageLoRa(String receivedMessage) {
    // Split the message into its components using the separator
    int firstSeparator = receivedMessage.indexOf('|');
    int secondSeparator = receivedMessage.indexOf('|', firstSeparator + 1);
    int thirdSeparator = receivedMessage.indexOf('|', secondSeparator + 1);

    if (firstSeparator == -1 || secondSeparator == -1 || thirdSeparator == -1) {
        Serial.println("[LoRa Rx] Invalid message format.");
        return;
    }

    // Extract message components (messageID|originatorID|senderID|content)
    String messageID = receivedMessage.substring(0, firstSeparator);
    String originatorID = receivedMessage.substring(firstSeparator + 1, secondSeparator);
    String senderID = receivedMessage.substring(secondSeparator + 1, thirdSeparator);
    String messageContent = receivedMessage.substring(thirdSeparator + 1);

    // Log the parsed message details
    Serial.printf("[LoRa Rx] Message ID: %s, Originator ID: %s, Sender ID: %s, Content: %s\n",
                  messageID.c_str(), originatorID.c_str(), senderID.c_str(), messageContent.c_str());

    // Get RSSI and SNR for LoRa messages (applies to the relay node only)
    int rssi = radio.getRSSI();
    float snr = radio.getSNR();

    // Avoid retransmitting messages from our own node but still process them for node listing
    if (senderID == String(getNodeId())) {
        Serial.println("[LoRa Rx] Received own message, not retransmitting...");
    } else {
        // Handle retransmission only if it's not from the current node
        auto& status = messageTransmissions[messageID];
        if (!status.transmittedViaLoRa) {
            scheduleLoRaTransmission(receivedMessage);
        }
    }

    // Add the message to the list regardless of the sender for proper display
    addMessage(originatorID, messageID, senderID, messageContent, "[LoRa]", rssi, snr);

    // **Update LoRa Node List**

    // 1. Add or update the originator node (DO NOT update RSSI, SNR, or last seen)
    if (loraNodes.find(originatorID) == loraNodes.end()) {
        // Add the originator node without modifying its RSSI, SNR, or last seen time
        LoRaNode newNode = {originatorID, 0, 0.0, 0};  // New node without RSSI and SNR
        loraNodes[originatorID] = newNode;
        Serial.printf("[LoRa Nodes] New originator node added: %s\n", originatorID.c_str());
    }

    // 2. Add or update the relay node (UPDATE RSSI, SNR, and last seen)
    if (loraNodes.find(senderID) == loraNodes.end()) {
        // Add the relay node and update with the current transmission values
        LoRaNode newRelayNode = {senderID, rssi, snr, millis()};  // New node with RSSI and SNR
        loraNodes[senderID] = newRelayNode;
        Serial.printf("[LoRa Nodes] New relay node added: %s (RSSI: %d, SNR: %.2f)\n", senderID.c_str(), rssi, snr);
    } else {
        // Update the relay node's values
        loraNodes[senderID].lastRSSI = rssi;
        loraNodes[senderID].lastSNR = snr;
        loraNodes[senderID].lastSeen = millis();
        Serial.printf("[LoRa Nodes] Updated relay node: %s (RSSI: %d, SNR: %.2f)\n", senderID.c_str(), rssi, snr);
    }

    // **Log the current LoRa Node list**
    Serial.println("[LoRa Nodes] Current node list:");
    for (const auto& node : loraNodes) {
        Serial.printf("Node ID: %s, RSSI: %d, SNR: %.2f, Last Seen: %llu ms\n",
                      node.first.c_str(), node.second.lastRSSI, node.second.lastSNR, node.second.lastSeen);
    }
}



void receivedCallback(uint32_t from, String& message) {
  Serial.printf("[WiFi Rx] Received message from %u: %s\n", from, message.c_str());

  int firstSeparator = message.indexOf('|');
  int secondSeparator = message.indexOf('|', firstSeparator + 1);
  int thirdSeparator = message.indexOf('|', secondSeparator + 1);

  if (firstSeparator == -1 || secondSeparator == -1 || thirdSeparator == -1) {
    Serial.println("[WiFi Rx] Invalid message format.");
    return;
  }

  String messageID = message.substring(0, firstSeparator);
  String originatorID = message.substring(firstSeparator + 1, secondSeparator);
  String senderID = message.substring(secondSeparator + 1, thirdSeparator);
  String messageContent = message.substring(thirdSeparator + 1);

  if (senderID == String(getNodeId())) {
    Serial.println("[WiFi Rx] Received own message, processing for node list but not retransmitting...");
    addMessage(originatorID, messageID, senderID, messageContent, "[WiFi]");
    return;
  }

  auto& status = messageTransmissions[messageID];
  if (status.transmittedViaWiFi && status.transmittedViaLoRa) {
    Serial.println("[WiFi Rx] Message already retransmitted via both WiFi and LoRa, ignoring...");
    return;
  }

  Serial.printf("[WiFi Rx] Adding message: %s\n", message.c_str());
  addMessage(originatorID, messageID, senderID, messageContent, "[WiFi]");

  if (!status.transmittedViaLoRa) {
    scheduleLoRaTransmission(message);
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
    return date.toLocaleTimeString();  // Adjust the format as needed (for example, add date if necessary)
  }

  // Function to send a message via the form
  function sendMessage(event) {
    event.preventDefault();

    const nameInput = document.getElementById('nameInput');
    const messageInput = document.getElementById('messageInput');
    const sendButton = document.getElementById('sendButton'); // Get the send button

    const sender = nameInput.value;
    const msg = messageInput.value;

    if (!sender || !msg) {
      alert('Please enter both a name and a message.');
      return;
    }

    localStorage.setItem('username', sender);

    const formData = new URLSearchParams();
    formData.append('sender', sender);
    formData.append('msg', msg);

    // Disable the send button and provide user feedback
    sendButton.disabled = true;
    sendButton.value = 'Wait 15s';

    fetch('/update', {
      method: 'POST',
      body: formData
    })
    .then(response => {
      if (!response.ok) throw new Error('Failed to send message');
      messageInput.value = '';
      fetchData();  // Fetch new messages

      // Re-enable the send button after 15 seconds
      setTimeout(() => {
        sendButton.disabled = false;
        sendButton.value = 'Send';
      }, 15000);
    })
    .catch(error => {
      console.error('Error sending message:', error);

      // Re-enable the send button even if there's an error
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

        const currentNodeId = localStorage.getItem('nodeId');

data.messages.forEach(msg => {
  const li = document.createElement('li');
  li.classList.add('message');

  // Determine whether the message is sent or received
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

  // **Update nodeId display logic: Always display "Node: " followed by node ID**
  let nodeIdHtml = `Node Id: ${msg.nodeId}`;  // Always show "Node: " before the node ID

  // Display RSSI and SNR if available and source is LoRa
  let rssiSnrHtml = '';
  if (msg.source === '[LoRa]' && typeof msg.rssi !== 'undefined' && typeof msg.snr !== 'undefined') {
    rssiSnrHtml = `<span class="message-rssi-snr">RSSI: ${msg.rssi} dBm, SNR: ${msg.snr} dB</span>`;
  }

  // Insert the message, displaying NodeId for both sent and received messages
  li.innerHTML = `
    <span class="message-nodeid">${nodeIdHtml}</span>  <!-- Always display NodeId -->
    <div class="message-content">${msg.content}</div>  <!-- Show message content -->
    <span class="message-time">${timestamp}</span>  <!-- Show time of the message -->
    ${rssiSnrHtml}  <!-- Show RSSI and SNR if available -->
  `;
  ul.appendChild(li);
});


        // Scroll to the bottom of the message list to show the most recent message
        ul.scrollTop = ul.scrollHeight;
      })
      .catch(error => console.error('Error fetching messages:', error));

    fetch('/deviceCount')
      .then(response => response.json())
      .then(data => {
        localStorage.setItem('nodeId', data.nodeId);
        document.getElementById('deviceCount').innerHTML = 
          'Mesh Nodes: <a href="/nodes">' + data.totalCount + '</a>, Node ID: ' + data.nodeId;
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
  <!-- Warning message at the top -->
  <div class="warning">For your safety, do not share your location or any personal information!</div>
  
  <!-- Title below warning -->
  <h2>Meshify Chat</h2>
  
  <div id="deviceCount">Mesh Nodes: 0</div>
  
  <div id="chat-container">
    <ul id="messageList"></ul>
  </div>
  
<form id="messageForm">
  <input type="text" id="nameInput" name="sender" placeholder="Your name" maxlength="15" required>
  <input type="text" id="messageInput" name="msg" placeholder="Your message" maxlength="100" required>
  <input type="submit" id="sendButton" value="Send"> <!-- Added id="sendButton" -->
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
            ', LoRa Nodes Detected (Last Hour): ' + data.loraNodes.length;
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
      // Add nodeId and messageID to the JSON object
      json += "{\"nodeId\":\"" + msg.nodeId + "\",\"sender\":\"" + msg.sender + "\",\"content\":\"" + msg.content + "\",\"source\":\"" + msg.source + "\",\"messageID\":\"" + msg.messageID + "\"";
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

    // Construct the full message with the message ID and originatorID
    String constructedMessage = constructMessage(messageID, String(getNodeId()), senderName, newMessage);

    // Add the message with source "[LoRa]" since LoRa will transmit first
    addMessage(String(getNodeId()), messageID, senderName, newMessage, "[LoRa]");
    Serial.printf("[LoRa Tx] Adding message: %s\n", constructedMessage.c_str()); // Added log

    // **Schedule LoRa transmission first**
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
