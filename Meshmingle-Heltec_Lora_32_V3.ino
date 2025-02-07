//Test v1.00.009
//08-02-2025
//MAKE SURE ALL NODES USE THE SAME VERSION OR EXPECT STRANGE THINGS HAPPENING.
//EU868 Band P (869.4 MHz - 869.65 MHz): 10%, 500 mW ERP (10% 24hr 8640 seconds = 6 mins per hour TX Time.)
//After Accounting for Heartbeats: 20 sec after boot then every 15 mins therafter.
//Per Hour: 136 Max Char messages within the 6-minute (360,000 ms) duty cycle
//Per Day: 3,296 Max Char messages within the 8,640,000 ms (10% duty cycle) allowance
//Indirect nodes show in nodelist and history.
//lots of other back end tweeks to improve mesh relaying.
//still struggling with noise on the frequency in my area causing loss of packets.
//nodes did not relay if on the same subnet (wifi) now regardless of on same subnet messages are relayed.
//still having issues at times with relay not happening and trying to work this out.
//metrics page has updates.
////////////////////////////////////////////////////////////////////////
// M    M  EEEEE  SSSSS  H   H  M    M  I  N   N  GGGGG  L      EEEEE //
// MM  MM  E      S      H   H  MM  MM  I  NN  N  G      L      E     //
// M MM M  EEEE   SSSSS  HHHHH  M MM M  I  N N N  G  GG  L      EEEE  //
// M    M  E          S  H   H  M    M  I  N  NN  G   G  L      E     //
// M    M  EEEEE  SSSSS  H   H  M    M  I  N   N   GGG   LLLLL  EEEEE //
////////////////////////////////////////////////////////////////////////

#define HELTEC_POWER_BUTTON // Use the power button feature of Heltec
#include <heltec_unofficial.h> // Heltec library for OLED and LoRa
#include <painlessMesh.h>
#include <WiFi.h>
#include <ESPAsyncWebServer.h>
#include <DNSServer.h>
#include <Wire.h>
#include <esp_task_wdt.h> // Watchdog timer library
#include <vector>         // For handling list of messages and our queue
#include <map>            // For unified retransmission tracking
#include <RadioLib.h>

// ===================
// TRANSMISSION TRACKING
// ===================
// Previously there was a single "relayed" flag; now we use separate flags for WiFi and LoRa.
struct TransmissionStatus {
  bool transmittedViaWiFi = false;
  bool transmittedViaLoRa = false;
  bool addedToMessages = false;  // Flag to track if the message has been added to messages vector
  bool relayedViaWiFi = false;     // NEW: whether the message has been relayed via WiFi
  bool relayedViaLoRa = false;     // NEW: whether the message has been relayed via LoRa
  uint64_t timestamp = millis();   // record when the entry was created/updated
};

// Map to track transmissions by message ID
std::map<String, TransmissionStatus> messageTransmissions;

// -----------------------
// (Other functions, e.g. cleanupMessageTransmissions(), LoRa setup, mesh setup, etc., remain unchanged.)
// -----------------------

// LoRa Parameters
#define PAUSE 5400000  
#define FREQUENCY 869.4000 
#define BANDWIDTH 250.0 
#define SPREADING_FACTOR 11 
#define TRANSMIT_POWER 22 
#define CODING_RATE 8  
String rxdata;
// Global RX flag
volatile bool rxFlag = false;
long counter = 0;
uint64_t tx_time;
uint64_t last_tx = 0;
uint64_t minimum_pause = 0;
unsigned long lastTransmitTime = 0;  

// Instead of a single message buffer, we now use a queue for outgoing LoRa messages.
std::vector<String> loraTransmissionQueue;

// Duty Cycle Definitions and Variables
#define DUTY_CYCLE_LIMIT_MS 360000   // 6 minutes in a 60-minute window
#define DUTY_CYCLE_WINDOW   3600000  // 60 minutes in milliseconds

uint64_t cumulativeTxTime = 0;
uint64_t dutyCycleStartTime = 0;

void resetDutyCycle() {
    cumulativeTxTime = 0;
    dutyCycleStartTime = millis();
    Serial.println("[Duty Cycle] Reset duty cycle counter.");
}

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

// Meshmingle Parameters
#define MESH_SSID "meshmingle.co.uk"
#define MESH_PASSWORD ""  
#define MESH_PORT 5555
const int maxMessages = 50;

// Duty Cycle Variables
bool bypassDutyCycle = false;     
bool dutyCycleActive = false;     
bool lastDutyCycleActive = false; 

AsyncWebServer server(80);
DNSServer dnsServer;
painlessMesh mesh;

// --- Updated Message structure with a new recipient field ---
struct Message {
  String nodeId;     // originator node ID
  String sender;
  String recipient;  // NEW: target node (or "ALL" for public messages)
  String content;
  String source;     // e.g., "[WiFi]" or "[LoRa]"
  String messageID;  
  String relayID;    
  int rssi;          
  float snr;         
  uint64_t timeReceived;
};

std::vector<Message> messages;  

int totalNodeCount = 0;
uint32_t currentNodeId = 0;

unsigned long loRaTransmitDelay = 0; 
unsigned long messageCounter = 0;

// Returns a custom formatted node id, for example: "!Mxxxxxx"
String getCustomNodeId(uint32_t nodeId) {
    String hexNodeId = String(nodeId, HEX);
    while (hexNodeId.length() < 6) {
        hexNodeId = "0" + hexNodeId;
    }
    if (hexNodeId.length() > 6) {
        hexNodeId = hexNodeId.substring(hexNodeId.length() - 6);
    }
    return "!M" + hexNodeId;
}

String generateMessageID(const String& nodeId) {
  messageCounter++;
  return nodeId + ":" + String(messageCounter);
}

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

// --- Updated constructMessage() to include the recipient field ---
// The message format is now: 
// messageID|originatorID|sender|recipient|content|relayID|CRC
String constructMessage(const String& messageID, const String& originatorID, const String& sender, const String& recipient, const String& content, const String& relayID) {
  String messageWithoutCRC = messageID + "|" + originatorID + "|" + sender + "|" + recipient + "|" + content + "|" + relayID;
  uint16_t crc = crc16_ccitt((const uint8_t *)messageWithoutCRC.c_str(), messageWithoutCRC.length());
  char crcStr[5];
  sprintf(crcStr, "%04X", crc);
  String fullMessage = messageWithoutCRC + "|" + String(crcStr);
  return fullMessage;
}

// --- METRICS HISTORY CHANGES ---
struct NodeMetricsSample {
  uint64_t timestamp;
  int rssi;
  float snr;
};

struct LoRaNode {
  String nodeId;
  int lastRSSI;
  float lastSNR;
  uint64_t lastSeen;
  std::vector<NodeMetricsSample> history; 
  String statusEmoji; // <-- NEW: Holds the emoji for this node
};

// Global container for direct LoRa nodes
std::map<String, LoRaNode> loraNodes;

// --- NEW: Structure for Indirect Nodes ---
// (Modified to include a history vector.)
struct IndirectNode {
  String originatorId;   // The node that originally sent the message (which we never hear directly)
  String relayId;        // The node that relayed the message to us
  int rssi;              // RSSI as measured from the relay’s transmission
  float snr;             // SNR as measured from the relay’s transmission
  uint64_t lastSeen;     // Timestamp when we last received a relayed message from this originator via the relay
  String statusEmoji;    // <-- NEW: Holds the emoji for this indirect node
  std::vector<NodeMetricsSample> history;  // NEW: History samples (up to 24 hours)
};

// Global container to hold indirect nodes keyed by a composite key (originatorID-relayID)
std::map<String, IndirectNode> indirectNodes;

unsigned long lastCleanupTime = 0;
const unsigned long cleanupInterval = 60000; // 1 minute

void cleanupLoRaNodes() {
  uint64_t currentTime = millis();
  const uint64_t timeout = 86400000; // 24 hours
  for (auto it = loraNodes.begin(); it != loraNodes.end();) {
    if (currentTime - it->second.lastSeen > timeout) {
      Serial.printf("[LoRa Nodes] Removing inactive LoRa node: %s\n", it->first.c_str());
      it = loraNodes.erase(it);
    } else {
      ++it;
    }
  }
}

// --- NEW: Cleanup function for Indirect Nodes ---
void cleanupIndirectNodes() {
  uint64_t currentTime = millis();
  const uint64_t timeout = 86400000; // 24 hours
  for (auto it = indirectNodes.begin(); it != indirectNodes.end();) {
    if (currentTime - it->second.lastSeen > timeout) {
      Serial.printf("[Indirect Nodes] Removing inactive indirect node: %s\n", it->first.c_str());
      it = indirectNodes.erase(it);
    } else {
      ++it;
    }
  }
}

// --- Updated addMessage() to accept the recipient and only add private messages if intended ---
void addMessage(const String& nodeId, const String& messageID, const String& sender, const String& recipient, String content, const String& source, const String& relayID, int rssi = 0, float snr = 0.0) {
  const int maxMessageLength = 150;
  if (content.length() > maxMessageLength) {
    Serial.println("Message too long, truncating...");
    content = content.substring(0, maxMessageLength);
  }

  auto& status = messageTransmissions[messageID];
  if (status.addedToMessages) {
    Serial.println("Message already exists, skipping...");
    return;
  }

  // For private messages (recipient != "ALL") only add if this node is either the originator or the designated recipient.
  String myId = getCustomNodeId(getNodeId());
  if (recipient != "ALL" && myId != nodeId && myId != recipient) {
    Serial.println("Private message not for me, skipping local addition.");
    return;
  }

  String finalSource = "";
  if (nodeId != getCustomNodeId(getNodeId())) {
    finalSource = source;
  }

  Message newMessage = {
    nodeId,
    sender,
    recipient, // new field
    content,
    finalSource,
    messageID,
    relayID,
    rssi,
    snr,
    millis()
  };

  messages.insert(messages.begin(), newMessage);
  status.addedToMessages = true;

  if (messages.size() > maxMessages) {
    messages.pop_back();
  }

  Serial.printf("Message added: NodeID: %s, Sender: %s, Recipient: %s, Content: %s, Source: %s, ID: %s, RelayID: %s\n",
                nodeId.c_str(), sender.c_str(), recipient.c_str(), content.c_str(), finalSource.c_str(), messageID.c_str(), relayID.c_str());
}

//
// --- scheduleLoRaTransmission() updated to parse 6 fields ---
// Expected format: messageID|originatorID|sender|recipient|content|relayID|CRC
void scheduleLoRaTransmission(String message) {
    int lastSeparator = message.lastIndexOf('|');
    if (lastSeparator == -1) {
        Serial.println("[LoRa Schedule] Invalid format (no CRC).");
        return;
    }

    String crcStr = message.substring(lastSeparator + 1);
    String messageWithoutCRC = message.substring(0, lastSeparator);

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
    int fifthSeparator = messageWithoutCRC.indexOf('|', fourthSeparator + 1);

    if (firstSeparator == -1 || secondSeparator == -1 || thirdSeparator == -1 ||
        fourthSeparator == -1 || fifthSeparator == -1) {
        Serial.println("[LoRa Schedule] Invalid message format.");
        return;
    }

    String messageID = messageWithoutCRC.substring(0, firstSeparator);
    String originatorID = messageWithoutCRC.substring(firstSeparator + 1, secondSeparator);
    String senderID = messageWithoutCRC.substring(secondSeparator + 1, thirdSeparator);
    String recipientID = messageWithoutCRC.substring(thirdSeparator + 1, fourthSeparator);
    String messageContent = messageWithoutCRC.substring(fourthSeparator + 1, fifthSeparator);
    String relayID = messageWithoutCRC.substring(fifthSeparator + 1);

    // Do not relay private messages that have reached their recipient.
    String myId = getCustomNodeId(getNodeId());
    if (recipientID != "ALL" && myId == recipientID) {
        Serial.println("[LoRa Schedule] Private message reached its recipient. Not scheduling retransmission.");
        return;
    }

    // Check if this node is the originator or a relay
    if (originatorID != myId) {
        // This node is relaying someone else's message.
        String newRelayID = myId;
        String updatedMessage = constructMessage(messageID, originatorID, senderID, recipientID, messageContent, newRelayID);
        // Instead of overwriting a single fullMessage, push it onto the queue.
        loraTransmissionQueue.push_back(updatedMessage);
        if (loraTransmissionQueue.size() == 1) {
          loRaTransmitDelay = millis() + random(1201, 5000);
        }
        Serial.printf("[LoRa Schedule] Scheduled relay from %s after %lu ms: %s\n",
                      newRelayID.c_str(), loRaTransmitDelay - millis(), updatedMessage.c_str());
    } else {
        // This is the originator. Schedule the original message for transmission.
        loraTransmissionQueue.push_back(message);
        if (loraTransmissionQueue.size() == 1) {
          loRaTransmitDelay = millis() + random(1201, 5000);
        }
        Serial.printf("[LoRa Schedule] Scheduled original message after %lu ms: %s\n",
                      loRaTransmitDelay - millis(), message.c_str());
    }
}

void transmitViaWiFi(const String& message) {
  Serial.printf("[WiFi Tx] Preparing to transmit: %s\n", message.c_str());
  if (message.startsWith("HEARTBEAT|")) {
    Serial.println("[WiFi Tx] Skipping heartbeat over WiFi.");
    return;
  }

  int separatorIndex = message.indexOf('|');
  if (separatorIndex == -1) {
    Serial.println("[WiFi Tx] Invalid format.");
    return;
  }
  String messageID = message.substring(0, separatorIndex);

  auto& status = messageTransmissions[messageID];
  if (status.transmittedViaWiFi) {
    Serial.println("[WiFi Tx] Already sent via WiFi, skipping...");
    return;
  }

  mesh.sendBroadcast(message);
  status.transmittedViaWiFi = true;
  Serial.printf("[WiFi Tx] Sent: %s\n", message.c_str());
}

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

// ----------------------------------------------------------------------------
// CAROUSEL CHANGES: Global variables for the carousel
// ----------------------------------------------------------------------------
unsigned long lastCarouselChange = 0;
const unsigned long carouselInterval = 3000; // 3 seconds each screen
int carouselIndex = 0;
long lastTxTimeMillis = -1; // store last LoRa Tx time for the display

void drawMonospacedLine(int16_t x, int16_t y, const String &line, int charWidth = 7) {
  for (uint16_t i = 0; i < line.length(); i++) {
    display.drawString(x + i * charWidth, y, String(line[i]));
  }
}

void showScrollingMonospacedAsciiArt() {
  display.clear();
  display.setFont(ArialMT_Plain_10);

  String lines[5];
  lines[0] = "M    M  EEEEE  SSSSS  H   H  M    M  I  N   N  GGGGG  L      EEEEE  ";
  lines[1] = "MM  MM  E      S      H   H  MM  MM  I  NN  N  G      L      E     ";
  lines[2] = "M MM M  EEEE   SSSSS  HHHHH  M MM M  I  N N N  G  GG  L      EEEE   ";
  lines[3] = "M    M  E          S  H   H  M    M  I  N  NN  G   G  L      E     ";
  lines[4] = "M    M  EEEEE  SSSSS  H   H  M    M  I  N   N   GGG   LLLLL  EEEEE  ";

  const int screenWidth  = 128;
  const int screenHeight = 64;
  const int lineHeight   = 10;
  const int totalBlockHeight = 5 * lineHeight;
  int verticalOffset = (screenHeight - totalBlockHeight) / 2;

  int charWidth = 7;
  uint16_t maxChars = 0;
  for (int i = 0; i < 5; i++) {
    if (lines[i].length() > maxChars) {
      maxChars = lines[i].length();
    }
  }
  int totalBlockWidth = maxChars * charWidth;

  if (totalBlockWidth <= screenWidth) {
    int offsetX = (screenWidth - totalBlockWidth) / 2;
    for (int i = 0; i < 5; i++) {
      drawMonospacedLine(offsetX, verticalOffset + i * lineHeight, lines[i], charWidth);
    }
    display.display();
    delay(3000);
    return;
  }

  const int blankStart = 20; 
  const int blankEnd   = 20; 

  for (int offset = -blankStart; offset <= (totalBlockWidth + screenWidth + blankEnd); offset += 2) {
    display.clear();
    for (int i = 0; i < 5; i++) {
      drawMonospacedLine(-offset, verticalOffset + i * lineHeight, lines[i], charWidth);
    }
    display.display();
    delay(30);
  }
}

void drawMainScreen(long txTimeMillis = -1) {
  if (txTimeMillis >= 0) {
    lastTxTimeMillis = txTimeMillis;
  }

  display.clear();
  display.setFont(ArialMT_Plain_10);
  int16_t titleWidth = display.getStringWidth("Meshmingle 1.0");
  display.drawString((128 - titleWidth) / 2, 0, "Meshmingle 1.0");
  display.drawString(0, 13, "Node ID: " + getCustomNodeId(getNodeId()));

  // Count active LoRa nodes
  uint64_t currentTime = millis();
  const uint64_t timeout = 900000; // 15 minutes
  int activeLoRaNodes = 0;

  for (const auto& node : loraNodes) {
    if (currentTime - node.second.lastSeen <= timeout) {
      activeLoRaNodes++;
    }
  }

  String combinedNodes = "WiFi Nodes: " + String(getNodeCount()) + "  LoRa: " + String(activeLoRaNodes);
  int16_t combinedWidth = display.getStringWidth(combinedNodes);
  if (combinedWidth > 128) {
    combinedNodes = "WiFi: " + String(getNodeCount()) + " LoRa: " + String(activeLoRaNodes);
  }
  display.drawString(0, 27, combinedNodes);

  if (dutyCycleActive) {
    display.drawString(0, 40, "Duty Cycle Limit Reached!");
  } else {
    display.drawString(0, 40, "LoRa Tx Allowed");
  }

  if (lastTxTimeMillis >= 0) {
    String txMessage = "TxOK (" + String(lastTxTimeMillis) + " ms)";
    int16_t txMessageWidth = display.getStringWidth(txMessage);
    display.drawString((128 - txMessageWidth) / 2, 54, txMessage);
  }

  display.display();
}

void displayCarousel() {
  unsigned long currentMillis = millis();
  if (currentMillis - lastCarouselChange >= carouselInterval) {
    lastCarouselChange = currentMillis;

    // Filter messages: Only show private messages for this node
    String myId = getCustomNodeId(getNodeId());
    std::vector<Message> filteredMessages;
    
    for (const auto& msg : messages) {
      if (msg.recipient != "ALL" && (msg.recipient == myId || msg.nodeId == myId)) {
        filteredMessages.push_back(msg);
      }
    }

    if (filteredMessages.empty()) {
      // No private messages for this node, reset carousel to main screen
      carouselIndex = 0;
    } else {
      // Loop through filtered messages
      carouselIndex++;
      if (carouselIndex > (int)filteredMessages.size()) {
        carouselIndex = 0;
      }
    }

    display.clear();
    display.setFont(ArialMT_Plain_10);

    if (carouselIndex == 0) {
      drawMainScreen(-1);
    } else {
      int msgIndex = carouselIndex - 1;
      if (msgIndex < (int)filteredMessages.size()) {
        String nodeLine = "Node: " + filteredMessages[msgIndex].nodeId;
        display.drawString(0, 0, nodeLine);

        String nameLine = "Name: " + filteredMessages[msgIndex].sender;
        display.drawString(0, 13, nameLine);

        display.drawString(0, 26, "[Private]");
        display.drawString(0, 36, filteredMessages[msgIndex].content);
      }
    }
    display.display();
  }
}


long lastTxTimeMillisVar = -1;

void transmitWithDutyCycle(const String& message) {
  // Ensure that the scheduled LoRa delay has expired.
  if (millis() < loRaTransmitDelay) {
    Serial.println("[LoRa Tx] LoRa delay not expired, waiting...");
    return;
  }

  // Check if the radio is busy with a timeout of 10 seconds.
  // If radio.available() remains true for 10 seconds, force TX.
  static uint64_t rxCheckStart = 0;
  if (radio.available()) {
    if (rxCheckStart == 0) {
      rxCheckStart = millis();  // Start timer when RX is first detected.
    }
    if (millis() - rxCheckStart >= 5000) {  // 5-second timeout reached.
      Serial.println("[LoRa Tx] RX appears stuck for 5 seconds. Forcing transmission...");
      // Since SX1262 doesn't have stopReceive(), we simply restart RX mode.
      radio.startReceive();  
      rxCheckStart = 0;  // Reset timer.
    } else {
      Serial.println("[LoRa Tx] Radio busy. Delaying transmission by 500ms.");
      loRaTransmitDelay = millis() + 500;
      return;
    }
  } else {
    // Reset the timer if the radio is not busy.
    rxCheckStart = 0;
  }

  // Extract the messageID from the message (assumes message format is valid).
  int separatorIndex = message.indexOf('|');
  if (separatorIndex == -1) {
    Serial.println("[LoRa Tx] Invalid message format.");
    return;
  }
  String messageID = message.substring(0, separatorIndex);
  auto& status = messageTransmissions[messageID];
  if (status.transmittedViaLoRa) {
    Serial.println("[LoRa Tx] Message already sent via LoRa, skipping...");
    if (!loraTransmissionQueue.empty()) {
      loraTransmissionQueue.erase(loraTransmissionQueue.begin());
      if (!loraTransmissionQueue.empty()) {
        loRaTransmitDelay = millis() + random(1201, 5000);
      }
    }
    return;
  }

  // Record the transmission start time and transmit the message.
  tx_time = millis();
  Serial.printf("[LoRa Tx] Transmitting: %s\n", message.c_str());
  heltec_led(50);
  int transmitStatus = radio.transmit(message.c_str());
  tx_time = millis() - tx_time;
  heltec_led(0);

  if (transmitStatus == RADIOLIB_ERR_NONE) {
    Serial.printf("[LoRa Tx] Sent successfully (%i ms)\n", (int)tx_time);
    status.transmittedViaLoRa = true;
    // --- NEW: Mark the message as relayed via LoRa (separate from any WiFi relay) ---
    messageTransmissions[messageID].relayedViaLoRa = true;
    calculateDutyCyclePause(tx_time);
    last_tx = millis();
    drawMainScreen(tx_time);
    radio.startReceive();

    // Forward the message via WiFi if it's not a heartbeat.
    if (!message.startsWith("HEARTBEAT|")) {
      transmitViaWiFi(message);
    }

    // After successful transmission, remove the message from the queue.
    if (!loraTransmissionQueue.empty()) {
      loraTransmissionQueue.erase(loraTransmissionQueue.begin());
    }
    // If more messages remain, schedule the next transmission.
    if (!loraTransmissionQueue.empty()) {
      loRaTransmitDelay = millis() + random(1201, 5000);
    }
  } else {
    Serial.printf("[LoRa Tx] Transmission failed with error code: %i\n", transmitStatus);
  }
}

unsigned long lastHeartbeatTime = 0;
const unsigned long firstHeartbeatDelay = 20000; // 20 seconds for first heartbeat
const unsigned long heartbeatInterval = 900000; // 15 minutes for subsequent heartbeats

void sendHeartbeat() {
  String heartbeatWithoutCRC = "HEARTBEAT|" + getCustomNodeId(getNodeId());
  uint16_t crc = crc16_ccitt((const uint8_t *)heartbeatWithoutCRC.c_str(), heartbeatWithoutCRC.length());
  char crcStr[5];
  sprintf(crcStr, "%04X", crc);
  String heartbeatMessage = heartbeatWithoutCRC + "|" + String(crcStr);

  // Check if duty cycle restrictions allow transmission.
  if (!isDutyCycleAllowed()) {
    Serial.println("[Heartbeat Tx] Duty cycle limit reached, skipping.");
    return;
  }

  // Check if the radio is busy (similar to queued messages)
  if (radio.available()) {
    Serial.println("[Heartbeat Tx] Radio is busy receiving a packet. Delaying heartbeat by 500ms.");
    return;
  }

  uint64_t txStart = millis();
  Serial.printf("[Heartbeat Tx] Sending: %s\n", heartbeatMessage.c_str());
  heltec_led(50);
  int transmitStatus = radio.transmit(heartbeatMessage.c_str());
  uint64_t txTime = millis() - txStart;
  heltec_led(0);

  if (transmitStatus == RADIOLIB_ERR_NONE) {
    Serial.printf("[Heartbeat Tx] Sent successfully (%llu ms)\n", txTime);
    calculateDutyCyclePause(txTime);
    last_tx = millis();
    drawMainScreen(txTime);
    radio.startReceive();
  } else {
    Serial.printf("[Heartbeat Tx] Failed with error code: %i\n", transmitStatus);
  }
}

// --------------------------------------------------------------------------
// IMPORTANT: Callback for radio RX events. (Renamed to avoid conflict.)
// --------------------------------------------------------------------------
void onRadioRx() {
  rxFlag = true;
}

void setup() {
  Serial.begin(115200);

  heltec_setup();
  Serial.println("Initializing LoRa radio...");
  heltec_led(0);

  display.init();
  display.flipScreenVertically();
  display.clear();
  display.setFont(ArialMT_Plain_10);
  drawMainScreen();

  showScrollingMonospacedAsciiArt(); 

  RADIOLIB_OR_HALT(radio.begin());
  // Set the radio DIO1 action callback to our renamed function.
  radio.setDio1Action(onRadioRx);

  RADIOLIB_OR_HALT(radio.setFrequency(FREQUENCY));
  RADIOLIB_OR_HALT(radio.setBandwidth(BANDWIDTH));
  RADIOLIB_OR_HALT(radio.setSpreadingFactor(SPREADING_FACTOR));
  RADIOLIB_OR_HALT(radio.setCodingRate(CODING_RATE));
  RADIOLIB_OR_HALT(radio.setOutputPower(TRANSMIT_POWER));

  // Disable hardware CRC so that only our manual CRC is used.
  //radio.setCRC(false);

  //  -- Set your unique sync word here --
  // radio.setSyncWord(0x12);  // e.g., 0x12

  radio.startReceive(RADIOLIB_SX126X_RX_TIMEOUT_INF);
  delay(2000);

  WiFi.softAP(MESH_SSID, MESH_PASSWORD);
  WiFi.setTxPower(WIFI_POWER_19_5dBm);
  WiFi.setSleep(false);
  initMesh();
  setupServerRoutes();
  server.begin();
  dnsServer.start(53, "*", WiFi.softAPIP());

  esp_task_wdt_init(30, true);
  esp_task_wdt_add(NULL);

  esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);
  esp_sleep_pd_config(ESP_PD_DOMAIN_RTC_PERIPH, ESP_PD_OPTION_ON);
  // Combine analog noise and a high-resolution timer:
  // -------------------
  // IMPROVED RANDOM SEED
  // -------------------
  uint32_t nodeId = getNodeId();
  uint32_t uniqueSeed = nodeId ^ analogRead(0) ^ micros();  // XOR for randomness
  randomSeed(uniqueSeed);
}

void loop() {
  esp_task_wdt_reset();
  heltec_loop();

  mesh.update();

  if (millis() - lastHeartbeatTime >= firstHeartbeatDelay && lastHeartbeatTime == 0) {
    sendHeartbeat();
    lastHeartbeatTime = millis();
  } else if (millis() - lastHeartbeatTime >= heartbeatInterval && lastHeartbeatTime > 0) {
    sendHeartbeat();
    lastHeartbeatTime = millis();
  }

  if (rxFlag) {
    rxFlag = false;
    String message;
    int state = radio.readData(message);
    if (state == RADIOLIB_ERR_NONE) {
      Serial.printf("[LoRa Rx] %s\n", message.c_str());

      int lastSeparatorIndex = message.lastIndexOf('|');
      if (lastSeparatorIndex == -1) {
        Serial.println("[LoRa Rx] Invalid format (no CRC).");
      } else {
        String crcStr = message.substring(lastSeparatorIndex + 1);
        String messageWithoutCRC = message.substring(0, lastSeparatorIndex);

        uint16_t receivedCRC = (uint16_t)strtol(crcStr.c_str(), NULL, 16);
        uint16_t computedCRC = crc16_ccitt((const uint8_t *)messageWithoutCRC.c_str(), messageWithoutCRC.length());

        if (receivedCRC != computedCRC) {
          Serial.printf("[LoRa Rx] CRC mismatch. Recv: %04X, Computed: %04X\n", receivedCRC, computedCRC);
        } else {
          Serial.println("[LoRa Rx] CRC valid.");

          // --- Only process messages from our system ---
          // For non-heartbeat messages, check that the first token (messageID) starts with our expected prefix.
          if (!messageWithoutCRC.startsWith("HEARTBEAT|")) {
            int firstSeparator = messageWithoutCRC.indexOf('|');
            if (firstSeparator == -1) {
              Serial.println("[LoRa Rx] Invalid message format.");
              radio.startReceive();
              return;
            }
            String messageID = messageWithoutCRC.substring(0, firstSeparator);
            if (!messageID.startsWith("!M")) {  // Our system-generated IDs start with "!M"
              Serial.println("[LoRa Rx] Foreign message detected, ignoring.");
              radio.startReceive();
              return;
            }
          }

          // --- Heartbeat Handling (Unchanged) ---
          if (messageWithoutCRC.startsWith("HEARTBEAT|")) {
            String senderNodeId = messageWithoutCRC.substring(strlen("HEARTBEAT|"));
            Serial.printf("[LoRa Rx] Heartbeat from %s\n", senderNodeId.c_str());
            int rssi = radio.getRSSI();
            float snr = radio.getSNR();
            uint64_t currentTime = millis();

            if (senderNodeId != getCustomNodeId(getNodeId())) {
              LoRaNode& node = loraNodes[senderNodeId];
              node.nodeId = senderNodeId;
              node.lastRSSI = rssi;
              node.lastSNR = snr;
              node.lastSeen = currentTime;

              NodeMetricsSample sample = { currentTime, rssi, snr };
              node.history.push_back(sample);
              if (node.history.size() > 60) {
                node.history.erase(node.history.begin());
              }
              node.statusEmoji = "❤️";
              Serial.printf("[LoRa Nodes] Updated/Added node: %s (Heartbeat)\n", senderNodeId.c_str());
            } else {
              Serial.println("[LoRa Rx] Own heartbeat, ignore.");
            }
          }
          // --- Non-Heartbeat (Message) Handling ---
          else {
            int firstSeparator = messageWithoutCRC.indexOf('|');
            int secondSeparator = messageWithoutCRC.indexOf('|', firstSeparator + 1);
            int thirdSeparator = messageWithoutCRC.indexOf('|', secondSeparator + 1);
            int fourthSeparator = messageWithoutCRC.indexOf('|', thirdSeparator + 1);
            int fifthSeparator = messageWithoutCRC.indexOf('|', fourthSeparator + 1);

            if (firstSeparator == -1 || secondSeparator == -1 || thirdSeparator == -1 ||
                fourthSeparator == -1 || fifthSeparator == -1) {
              Serial.println("[LoRa Rx] Invalid format.");
            } else {
              String messageID = messageWithoutCRC.substring(0, firstSeparator);
              String originatorID = messageWithoutCRC.substring(firstSeparator + 1, secondSeparator);
              String senderID = messageWithoutCRC.substring(secondSeparator + 1, thirdSeparator);
              String recipientID = messageWithoutCRC.substring(thirdSeparator + 1, fourthSeparator);
              String messageContent = messageWithoutCRC.substring(fourthSeparator + 1, fifthSeparator);
              String relayID = messageWithoutCRC.substring(fifthSeparator + 1);

              int rssi = radio.getRSSI();
              float snr = radio.getSNR();

              String myId = getCustomNodeId(getNodeId());

              // Ignore the message if it's our own original transmission.
              if (originatorID == myId && relayID == myId) {
                Serial.println("[LoRa Rx] Own original message, ignore.");
              } else {
                // Process and add the message if public or intended for us.
                if (recipientID == "ALL" || myId == originatorID || myId == recipientID) {
                  addMessage(originatorID, messageID, senderID, recipientID, messageContent, "[LoRa]", relayID, rssi, snr);
                } else {
                  Serial.println("[LoRa Rx] Private message not for me, ignoring display.");
                }
                // --- Use the new "relayedViaLoRa" flag to decide whether to schedule a relay ---
                if (!messageTransmissions[messageID].relayedViaLoRa) {
                  scheduleLoRaTransmission(message);
                }
                uint64_t currentTime = millis();

                // Update direct relay node information
                if (relayID != myId) {
                  LoRaNode& relayNode = loraNodes[relayID];
                  relayNode.nodeId = relayID;
                  relayNode.lastRSSI = rssi;
                  relayNode.lastSNR = snr;
                  relayNode.lastSeen = currentTime;

                  NodeMetricsSample sample = { currentTime, rssi, snr };
                  relayNode.history.push_back(sample);
                  if (relayNode.history.size() > 60) {
                    relayNode.history.erase(relayNode.history.begin());
                  }
                  if (relayID == originatorID) {
                    relayNode.statusEmoji = "⌨️";
                  } else {
                    relayNode.statusEmoji = "🛰️";
                  }
                  Serial.printf("[LoRa Nodes] Updated/Added node: %s\n", relayID.c_str());
                } else {
                  Serial.println("[LoRa Nodes] RelayID is own node, not updating.");
                }

                // Update indirect nodes if applicable (allow separate entries per relay)
                if (originatorID != myId && relayID != myId && relayID != originatorID) {
                  bool seenDirectly = false;
                  if (loraNodes.find(originatorID) != loraNodes.end()) {
                    const uint64_t THIRTY_SECONDS = 30000;
                    uint64_t lastSeenDirect = loraNodes[originatorID].lastSeen;
                    if (millis() - lastSeenDirect <= THIRTY_SECONDS) {
                      seenDirectly = true;
                    }
                  }
                  if (!seenDirectly) {
                    NodeMetricsSample sample = { currentTime, rssi, snr };
                    String key = originatorID + "-" + relayID; // Composite key for separate relay entries
                    auto it = indirectNodes.find(key);
                    if (it != indirectNodes.end()) {
                      // Update existing indirect node entry for this relay
                      it->second.lastSeen = currentTime;
                      it->second.rssi = rssi;
                      it->second.snr = snr;
                      it->second.history.push_back(sample);
                      if (it->second.history.size() > 60) {
                        it->second.history.erase(it->second.history.begin());
                      }
                    } else {
                      // Create a new indirect node entry for this relay
                      IndirectNode indNode;
                      indNode.originatorId = originatorID;
                      indNode.relayId = relayID;
                      indNode.rssi = rssi;
                      indNode.snr = snr;
                      indNode.lastSeen = currentTime;
                      indNode.statusEmoji = "🛰️";
                      indNode.history.push_back(sample);
                      indirectNodes[key] = indNode;
                    }
                    Serial.printf("[Indirect Nodes] Updated indirect node: Originator: %s, Relay: %s, RSSI: %d, SNR: %.2f\n",
                                  originatorID.c_str(), relayID.c_str(), rssi, snr);
                  } else {
                    Serial.printf("[Indirect Nodes] Skipped indirect update for %s because it is seen directly.\n", originatorID.c_str());
                  }
                }
              }
            }
          }
        }
        radio.startReceive();
      }
    } else {
      Serial.printf("[LoRa Rx] Receive failed, code %d\n", state);
      radio.startReceive();
    }
  }

  // Instead of checking a single fullMessage, check if the queue has messages
  if (!loraTransmissionQueue.empty() && millis() >= loRaTransmitDelay) {
    transmitWithDutyCycle(loraTransmissionQueue.front());
  }

  updateMeshData();
  displayCarousel();
  dnsServer.processNextRequest();

  // --- Cleanup: Remove old LoRa nodes, indirect nodes, and transmission statuses ---
  if (millis() - lastCleanupTime >= cleanupInterval) {
    cleanupLoRaNodes();          
    cleanupIndirectNodes();  // <-- NEW: Clean up indirect nodes every 24 hours
    // (Assume cleanup of messageTransmissions as needed)
    lastCleanupTime = millis();
  }

  if (millis() - lastHeartbeatTime >= heartbeatInterval) {
    sendHeartbeat();
    lastHeartbeatTime = millis();
  }
}

void initMesh() {
  mesh.setDebugMsgTypes(ERROR | STARTUP | CONNECTION);
  mesh.init(MESH_SSID, MESH_PASSWORD, MESH_PORT);
  mesh.onReceive(receivedCallback);

  mesh.onChangedConnections([]() {
    updateMeshData();
  });

  mesh.setContainsRoot(false);
}

void receivedCallback(uint32_t from, String& message) {
  Serial.printf("[WiFi Rx] From %u: %s\n", from, message.c_str());

  int lastSeparatorIndex = message.lastIndexOf('|');
  if (lastSeparatorIndex == -1) {
    Serial.println("[WiFi Rx] Invalid format (no CRC).");
    return;
  }
  String crcStr = message.substring(lastSeparatorIndex + 1);
  String messageWithoutCRC = message.substring(0, lastSeparatorIndex);

  uint16_t receivedCRC = (uint16_t)strtol(crcStr.c_str(), NULL, 16);
  uint16_t computedCRC = crc16_ccitt((const uint8_t *)messageWithoutCRC.c_str(), messageWithoutCRC.length());

  if (receivedCRC != computedCRC) {
    Serial.printf("[WiFi Rx] CRC mismatch. Recv: %04X, Computed: %04X\n", receivedCRC, computedCRC);
    return;
  } else {
    Serial.println("[WiFi Rx] CRC valid.");
  }

  // --- Only accept messages from our system ---
// For non-heartbeat messages, verify that the first token (messageID) starts with "!M"
  int firstSeparator = messageWithoutCRC.indexOf('|');
  if (firstSeparator == -1) {
    Serial.println("[WiFi Rx] Invalid message format.");
    return;
  }
  String messageID = messageWithoutCRC.substring(0, firstSeparator);
  if (!messageWithoutCRC.startsWith("HEARTBEAT|") && !messageID.startsWith("!M")) {
    Serial.println("[WiFi Rx] Foreign message detected, ignoring.");
    return;
  }

  int secondSeparator = messageWithoutCRC.indexOf('|', firstSeparator + 1);
  int thirdSeparator = messageWithoutCRC.indexOf('|', secondSeparator + 1);
  int fourthSeparator = messageWithoutCRC.indexOf('|', thirdSeparator + 1);
  int fifthSeparator = messageWithoutCRC.indexOf('|', fourthSeparator + 1);

  if (firstSeparator == -1 || secondSeparator == -1 || thirdSeparator == -1 ||
      fourthSeparator == -1 || fifthSeparator == -1) {
    Serial.println("[WiFi Rx] Invalid format (missing fields).");
    return;
  }

  messageID = messageWithoutCRC.substring(0, firstSeparator);
  String originatorID = messageWithoutCRC.substring(firstSeparator + 1, secondSeparator);
  String senderID = messageWithoutCRC.substring(secondSeparator + 1, thirdSeparator);
  String recipientID = messageWithoutCRC.substring(thirdSeparator + 1, fourthSeparator);
  String messageContent = messageWithoutCRC.substring(fourthSeparator + 1, fifthSeparator);
  String relayID = messageWithoutCRC.substring(fifthSeparator + 1);

  if (messageWithoutCRC.startsWith("HEARTBEAT|")) {
    Serial.println("[WiFi Rx] Skipping heartbeat relay over WiFi.");
    return;
  }

  String myId = getCustomNodeId(getNodeId());
  // Only add and display the message if it is public or intended for us.
  if (originatorID == myId) {
    addMessage(originatorID, messageID, senderID, recipientID, messageContent, "[WiFi]", relayID);
  } else if (recipientID == "ALL" || myId == recipientID) {
    addMessage(originatorID, messageID, senderID, recipientID, messageContent, "[WiFi]", relayID);
  } else {
    Serial.println("[WiFi Rx] Private message not for me, ignoring display.");
  }

  // --- Use the new "relayedViaLoRa" flag to schedule a relay if not already done ---
  String myIdForRelay = getCustomNodeId(getNodeId());
  auto& status = messageTransmissions[messageID];
if (!status.relayedViaLoRa) {
    scheduleLoRaTransmission(message);
}
}

const char mainPageHtml[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="en">
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1.0">
  <title>Meshmingle Chat Room</title>
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
      flex-wrap: wrap;
      gap: 5px;
    }
    #nameInput, #messageInput, #targetInput {
      padding: 10px;
      border: 1px solid #ccc;
      border-radius: 5px;
      box-sizing: border-box;
    }
    #nameInput {
      width: 30%;
    }
    #targetInput {
      width: 30%;
    }
    #messageInput {
      width: 30%;
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
      font-size: 0.7em;
      color: #666;
    }
    .message-relayid {
      font-size: 0.7em;
      color: #555;
      margin-bottom: 2px;
      display: block;
    }
    .message-rssi-snr {
      font-size: 0.7em;
      color: #666;
      text-align: right;
      margin-top: 2px;
    }
    .message-content {
      font-size: 0.85em;
      color: #333;
    }
    .message-time {
      font-size: 0.7em;
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
    let deviceCurrentTime = 0;
    function sendMessage(event) {
      event.preventDefault();

      const nameInput = document.getElementById('nameInput');
      const targetInput = document.getElementById('targetInput');
      const messageInput = document.getElementById('messageInput');
      const sendButton = document.getElementById('sendButton');

      const sender = nameInput.value;  
      const target = targetInput.value;  
      const msg = messageInput.value;  

      if (!sender || !msg) {
        alert('Please enter both a name and a message.');
        return;
      }

      localStorage.setItem('username', sender);

      const formData = new URLSearchParams();
      formData.append('sender', sender);
      formData.append('msg', msg);
      formData.append('target', target);

      sendButton.disabled = true;
      sendButton.value = 'Wait 15s';

      fetch('/update', {
        method: 'POST',
        body: formData
      })
      .then(response => {
        if (!response.ok) throw new Error('Failed to send message');
        messageInput.value = '';
        targetInput.value = '';
        fetchData();

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

    function fetchData() {
      fetch('/messages')
        .then(response => response.json())
        .then(data => {
          deviceCurrentTime = data.currentDeviceTime;

          const ul = document.getElementById('messageList');
          ul.innerHTML = '';

          const currentNodeId = localStorage.getItem('nodeId');

          data.messages.forEach(msg => {
            // Filter out private messages not meant for this node.
            if (msg.recipient && msg.recipient !== "ALL" &&
                msg.nodeId !== currentNodeId && msg.recipient !== currentNodeId) {
              return;
            }

            const li = document.createElement('li');
            li.classList.add('message');

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

            const messageAgeMillis = deviceCurrentTime - msg.timeReceived;
            const messageAgeSeconds = Math.floor(messageAgeMillis / 1000);
            let timestamp = '';
            if (messageAgeSeconds < 60) {
              timestamp = `${messageAgeSeconds} sec ago`;
            } else if (messageAgeSeconds < 3600) {
              const minutes = Math.floor(messageAgeSeconds / 60);
              timestamp = `${minutes} min ago`;
            } else if (messageAgeSeconds < 86400) {
              const hours = Math.floor(messageAgeSeconds / 3600);
              const minutes = Math.floor((messageAgeSeconds % 3600) / 60);
              timestamp = minutes > 0 ? `${hours} hr ${minutes} min ago` : `${hours} hr ago`;
            } else if (messageAgeSeconds < 604800) {
              const days = Math.floor(messageAgeSeconds / 86400);
              timestamp = `${days} day${days > 1 ? 's' : ''} ago`;
            } else if (messageAgeSeconds < 2592000) {
              const weeks = Math.floor(messageAgeSeconds / 604800);
              timestamp = `${weeks} week${weeks > 1 ? 's' : ''} ago`;
            } else if (messageAgeSeconds < 31536000) {
              const months = Math.floor(messageAgeSeconds / 2592000);
              timestamp = `${months} month${months > 1 ? 's' : ''} ago`;
            } else {
              const years = Math.floor(messageAgeSeconds / 31536000);
              timestamp = `${years} year${years > 1 ? 's' : ''} ago`;
            }

            const nodeIdHtml = `Node Id: ${msg.nodeId}`;
            const senderHtml = `<strong>${msg.sender || 'Unknown'}:</strong> `;
            let privateIndicator = "";
            if (msg.recipient && msg.recipient !== "ALL") {
              privateIndicator = `<span style="color:red;">[Private to ${msg.recipient}]</span> `;
            }
            
            // NEW: Build the RSSI/SNR line if this is a LoRa message
            let rssiSnrHtml = "";
            if (msg.source === "[LoRa]" && msg.rssi !== undefined && msg.snr !== undefined) {
              rssiSnrHtml = `<span class="message-rssi-snr">RSSI: ${msg.rssi} dBm, SNR: ${msg.snr} dB</span>`;
            }

            li.innerHTML = `
              <span class="message-nodeid">${nodeIdHtml}</span>
              <div class="message-content">${senderHtml}${privateIndicator}${msg.content}</div>
              <span class="message-time">${timestamp}</span>
              ${rssiSnrHtml}
            `;
            ul.appendChild(li);
          });

          ul.scrollTop = ul.scrollHeight;
        })
        .catch(error => console.error('Error fetching messages:', error));

      fetch('/deviceCount')
        .then(response => response.json())
        .then(data => {
          localStorage.setItem('nodeId', data.nodeId);
          document.getElementById('deviceCount').innerHTML = 
            `Mesh Nodes: <a href="/nodes">${data.totalCount}</a>, Node ID: ${data.nodeId}`;
        })
        .catch(error => console.error('Error fetching device count:', error));
    }

    window.onload = function() {
      const savedName = localStorage.getItem('username');
      if (savedName) {
        document.getElementById('nameInput').value = savedName;
      }
      fetchData();
      setInterval(fetchData, 5000);
      document.getElementById('messageForm').addEventListener('submit', sendMessage);
    };
  </script>
</head>
<body>
  <div class="warning">For your safety, do not share your location or any personal information!</div>
  
  <h2>Meshmingle Chat</h2>
  
  <div id="deviceCount">Mesh Nodes: 0</div>
  
  <div id="chat-container">
    <ul id="messageList"></ul>
  </div>
  
  <form id="messageForm" style="display: flex; align-items: center; gap: 5px;">
    <input type="text" id="nameInput" name="sender" placeholder="Your name" maxlength="15" required style="flex: 1;">
    <input type="text" id="messageInput" name="msg" placeholder="Your message" maxlength="150" required style="flex: 1;">
    <input type="text" id="targetInput" name="target" placeholder="Node" maxlength="8" style="width: 8ch;">
    <input type="submit" id="sendButton" value="Send" style="flex-shrink: 0;">
  </form>
  
</body>
</html>
)rawliteral";

const char nodesPageHtml[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="en">
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1.0">
  <title>Meshmingle Nodes</title>
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
    ul { 
      list-style-type: none; 
      padding: 0; 
      margin: 10px auto; 
      max-width: 600px; 
    }
    .node {
      background-color: #fff;
      border: 2px solid;
      border-radius: 10px;
      padding: 15px;
      margin: 10px 0;
      text-align: left;
      font-size: 0.85em;
      color: #333;
      box-shadow: 0 2px 5px rgba(0,0,0,0.1);
      position: relative;
    }
    .node.wifi {
      background-color: #e7f0ff;
      border-color: blue;
    }
    .node.lora {
      background-color: #fff4e0;
      border-color: orange;
    }
    .node-header {
      display: flex;
      align-items: center;
      gap: 10px;
      margin-bottom: 8px;
    }
    .node-header strong {
      font-size: 1em;
    }
    .node-header a {
      text-decoration: none;
      color: #007bff;
      font-weight: bold;
    }
    .node-header a:hover {
      text-decoration: underline;
    }
    .node-info {
      margin-left: 20px;
      font-size: 0.9em;
    }
    .node-emoji {
      position: absolute;
      bottom: 5px;
      right: 5px;
      font-size: 1.2em;
    }
    #nodeCount { 
      margin: 20px auto; 
      max-width: 600px; 
      font-weight: bold;
      text-align: left;
    }
    .node-section {
      margin-bottom: 30px;
    }
    @media (max-width: 600px) {
      .node-info {
        margin-left: 0;
      }
    }
    .nav-links {
      text-align: center;
      margin-bottom: 20px;
    }
    .nav-links a {
      margin: 0 15px;
      text-decoration: none;
      color: #007bff;
      font-weight: bold;
    }
    .nav-links a:hover {
      text-decoration: underline;
    }
  </style>
  <script>
    function fetchNodes() {
      fetch('/nodesData')
        .then(response => response.json())
        .then(data => {
          // --- WiFi Nodes Section (unchanged) ---
          const wifiUl = document.getElementById('wifiNodeList');
          wifiUl.innerHTML = '';
          const wifiCount = data.wifiNodes.length;
          document.getElementById('wifiCount').innerText = 'WiFi Nodes Connected: ' + wifiCount;
          data.wifiNodes.forEach((node, index) => {
            const li = document.createElement('li');
            li.classList.add('node', 'wifi');
            li.innerHTML = `
              <div class="node-header">
                <strong>Node ${index + 1}:</strong>
                <span>${node}</span>
              </div>
              <div class="node-info"></div>
            `;
            wifiUl.appendChild(li);
          });

          // --- Direct LoRa Nodes Section (unchanged) ---
          const loraUl = document.getElementById('loraNodeList');
          loraUl.innerHTML = '';
          const loraCount = data.loraNodes.length;
          document.getElementById('loraCount').innerText = 'Direct LoRa Nodes Active: ' + loraCount;
          data.loraNodes.forEach((node, index) => {
            const li = document.createElement('li');
            li.classList.add('node', 'lora');
            li.innerHTML = `
              <div class="node-header">
                <strong>Node ${index + 1}:</strong>
                <a href="/loraDetails?nodeId=${encodeURIComponent(node.nodeId)}">${node.nodeId}</a>
              </div>
              <div class="node-info">
                RSSI: ${node.lastRSSI} dBm, SNR: ${node.lastSNR} dB<br>
                Last seen: ${node.lastSeen}
              </div>
              <div class="node-emoji">${node.statusEmoji || ""}</div>
            `;
            loraUl.appendChild(li);
          });

          // --- Indirect (Relayed) Nodes Section ---
          // Instead of listing every relay entry, we group them by the originator
          // and only show the most recent relay for each originator.
          const indirectUl = document.getElementById('indirectNodeList');
          indirectUl.innerHTML = "";
          const groupedIndirect = {};
          data.indirectNodes.forEach((node) => {
            let originator = node.originatorId;
            // If this originator is not yet in the group or this node's lastSeen is more recent, update it.
            if (!groupedIndirect[originator] || node.lastSeen > groupedIndirect[originator].lastSeen) {
              groupedIndirect[originator] = node;
            }
          });
          const groupedIndirectArray = Object.values(groupedIndirect);
          document.getElementById('indirectCount').innerText = 'Indirect Nodes (Relayed) Active: ' + groupedIndirectArray.length;
          groupedIndirectArray.forEach((node, index) => {
            const li = document.createElement('li');
            li.classList.add('node');
            li.style.backgroundColor = '#f9f9f9';
            li.style.borderColor = '#999';
            li.innerHTML = `
              <div class="node-header">
                <strong>Originator:</strong>
                <span>${node.originatorId}</span>
              </div>
              <div class="node-info">
                Last Relay: <a href="/loraDetails?nodeId=${encodeURIComponent(node.relayId)}">${node.relayId}</a><br>
                RSSI: ${node.rssi} dBm, SNR: ${node.snr} dB<br>
                Last seen: ${node.lastSeen}
              </div>
              <div class="node-emoji">${node.statusEmoji || ""}</div>
            `;
            indirectUl.appendChild(li);
          });
        })
        .catch(error => console.error('Error fetching nodes:', error));
    }

    window.onload = function() {
      fetchNodes();
      setInterval(fetchNodes, 5000);
    };
  </script>
</head>
<body>
  <div class="nav-links">
    <a href="/">Back</a> | <a href="/metrics">History</a>
  </div>
  <h2>Meshmingle Nodes</h2>
  
  <div class="node-section">
    <span id="wifiCount">WiFi Nodes Connected: 0</span>
    <ul id="wifiNodeList"></ul>
  </div>

  <div class="node-section">
    <span id="loraCount">Direct LoRa Nodes Active: 0</span>
    <ul id="loraNodeList"></ul>
  </div>
  
  <div class="node-section">
    <span id="indirectCount">Indirect Nodes (Relayed) Active: 0</span>
    <ul id="indirectNodeList"></ul>
  </div>
</body>
</html>
)rawliteral";

// --- METRICS HISTORY PAGE (unchanged) ---
const char metricsPageHtml[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="en">
<head>
  <meta charset="UTF-8" />
  <meta name="viewport" content="width=device-width, initial-scale=1.0" />
  <title>LoRa Signal History</title>
  <style>
    body {
      font-family: Arial, sans-serif;
      background-color: #f4f7f6;
      margin: 0;
      padding: 20px;
    }
    h2 {
      text-align: center;
      color: #333;
    }
    .node-id {
      text-align: center;
      font-size: 1.2em;
      color: #555;
      margin-top: 10px;
    }
    /* Container for each node (direct or originator for indirect) */
    .node-block {
      background-color: #fff;
      border: 2px solid #ccc;
      border-radius: 8px;
      margin: 20px auto;
      max-width: 600px;
      padding: 10px;
      box-sizing: border-box;
    }
    /* Sub-container for relay entries within an originator */
    .sub-node-block {
      background-color: #f9f9f9;
      border: 1px solid #ccc;
      border-radius: 6px;
      margin: 10px 0;
      padding: 8px;
    }
    .node-title {
      font-weight: bold;
      margin-bottom: 8px;
    }
    table {
      border-collapse: collapse;
      width: 100%;
      margin-bottom: 10px;
    }
    th, td {
      border: 1px solid #ccc;
      padding: 5px;
      text-align: left;
      font-size: 0.9em;
    }
    th {
      background-color: #eee;
    }
    .nav-links {
      display: flex;
      justify-content: center;
      align-items: center;
      gap: 10px;
      margin-bottom: 20px;
    }
    .nav-links a {
      text-decoration: none;
      color: #007bff;
      font-weight: bold;
    }
    .nav-links a:hover {
      text-decoration: underline;
    }
  </style>
  <script>
    // On page load, fetch the device count and metrics history data.
    window.onload = function() {
      fetch("/deviceCount")
        .then(response => response.json())
        .then(data => {
          const nodeIdElement = document.getElementById("nodeIdDisplay");
          nodeIdElement.textContent = `Node ID: ${data.nodeId}`;
        })
        .catch(error => console.error("Error fetching Node ID:", error));

      fetchHistory();
      setInterval(fetchHistory, 5000);
    };

    function fetchHistory() {
      fetch("/metricsHistoryData")
        .then(response => response.json())
        .then(data => {
          const container = document.getElementById("historyContainer");
          container.innerHTML = "";

          // --- Direct LoRa Nodes History Section ---
          if (data.loraNodes && data.loraNodes.length > 0) {
            const directHeader = document.createElement("h3");
            directHeader.textContent = "Direct LoRa Nodes History";
            container.appendChild(directHeader);

            data.loraNodes.forEach(node => {
              const nodeBlock = document.createElement("div");
              nodeBlock.classList.add("node-block");

              // Header for direct node
              const nodeTitle = document.createElement("div");
              nodeTitle.classList.add("node-title");
              nodeTitle.textContent = `Node ID: ${node.nodeId}`;
              nodeBlock.appendChild(nodeTitle);

              // Summary table (best signal)
              const summaryTable = document.createElement("table");
              const summaryHeader = document.createElement("thead");
              summaryHeader.innerHTML = `
                <tr>
                  <th>Best Signal RSSI (dBm)</th>
                  <th>Best Signal SNR (dB)</th>
                </tr>`;
              summaryTable.appendChild(summaryHeader);
              const summaryBody = document.createElement("tbody");
              const summaryRow = document.createElement("tr");
              summaryRow.innerHTML = `<td>${node.bestRssi}</td><td>${node.bestSnr}</td>`;
              summaryBody.appendChild(summaryRow);
              summaryTable.appendChild(summaryBody);
              nodeBlock.appendChild(summaryTable);

              // History table for node
              const historyTable = document.createElement("table");
              const historyHeader = document.createElement("thead");
              historyHeader.innerHTML = `
                <tr>
                  <th>Time Ago</th>
                  <th>RSSI (dBm)</th>
                  <th>SNR (dB)</th>
                </tr>`;
              historyTable.appendChild(historyHeader);
              const historyBody = document.createElement("tbody");
              node.history.forEach(sample => {
                const row = document.createElement("tr");
                row.innerHTML = `<td>${sample.timestamp}</td><td>${sample.rssi}</td><td>${sample.snr}</td>`;
                historyBody.appendChild(row);
              });
              historyTable.appendChild(historyBody);
              nodeBlock.appendChild(historyTable);

              container.appendChild(nodeBlock);
            });
          }

          // --- Indirect Nodes History Section (Grouped by Originator) ---
          if (data.indirectNodes && data.indirectNodes.length > 0) {
            // Group indirect node records by originator (each record has a 'nodeId' representing the originator)
            const groupedIndirect = {};
            data.indirectNodes.forEach(indirect => {
              let originator = indirect.nodeId;
              if (!groupedIndirect[originator]) {
                groupedIndirect[originator] = [];
              }
              groupedIndirect[originator].push(indirect);
            });

            const indirectHeader = document.createElement("h3");
            indirectHeader.textContent = "Indirect Nodes History (Grouped by Originator)";
            container.appendChild(indirectHeader);

            // For each originator, create a container with a relays table
            Object.keys(groupedIndirect).forEach(originator => {
              const group = groupedIndirect[originator];
              const originatorBlock = document.createElement("div");
              originatorBlock.classList.add("node-block");

              // Originator header (like direct node header)
              const originatorTitle = document.createElement("div");
              originatorTitle.classList.add("node-title");
              originatorTitle.textContent = `Originator: ${originator}`;
              originatorBlock.appendChild(originatorTitle);

              // For each relay associated with this originator, render its own summary and history tables
              group.forEach(relay => {
                const relayBlock = document.createElement("div");
                relayBlock.classList.add("sub-node-block");

                const relayTitle = document.createElement("div");
                relayTitle.classList.add("node-title");
                relayTitle.textContent = `Relay ID: ${relay.relayId}`;
                relayBlock.appendChild(relayTitle);

                // Relay summary table
                const relaySummaryTable = document.createElement("table");
                const relaySummaryHeader = document.createElement("thead");
                relaySummaryHeader.innerHTML = `
                  <tr>
                    <th>Best Signal RSSI (dBm)</th>
                    <th>Best Signal SNR (dB)</th>
                  </tr>`;
                relaySummaryTable.appendChild(relaySummaryHeader);
                const relaySummaryBody = document.createElement("tbody");
                const relaySummaryRow = document.createElement("tr");
                relaySummaryRow.innerHTML = `<td>${relay.bestRssi}</td><td>${relay.bestSnr}</td>`;
                relaySummaryBody.appendChild(relaySummaryRow);
                relaySummaryTable.appendChild(relaySummaryBody);
                relayBlock.appendChild(relaySummaryTable);

                // Relay history table
                const relayHistoryTable = document.createElement("table");
                const relayHistoryHeader = document.createElement("thead");
                relayHistoryHeader.innerHTML = `
                  <tr>
                    <th>Time Ago</th>
                    <th>RSSI (dBm)</th>
                    <th>SNR (dB)</th>
                  </tr>`;
                relayHistoryTable.appendChild(relayHistoryHeader);
                const relayHistoryBody = document.createElement("tbody");
                relay.history.forEach(sample => {
                  const row = document.createElement("tr");
                  row.innerHTML = `<td>${sample.timestamp}</td><td>${sample.rssi}</td><td>${sample.snr}</td>`;
                  relayHistoryBody.appendChild(row);
                });
                relayHistoryTable.appendChild(relayHistoryBody);
                relayBlock.appendChild(relayHistoryTable);

                originatorBlock.appendChild(relayBlock);
              });

              container.appendChild(originatorBlock);
            });
          }
        })
        .catch(error => {
          console.error("Error fetching metrics history:", error);
        });
    }
  </script>
</head>
<body>
  <div class="nav-links">
    <a href="/nodes">NodeList</a> | <a href="/">Chat</a>
  </div>
  <h2>LoRa 24hr History</h2>
  <div class="node-id" id="nodeIdDisplay">Node ID: Loading...</div>
  <div id="historyContainer"></div>
</body>
</html>
)rawliteral";

// --- Helper function to format relative time ---
String formatRelativeTime(uint64_t ageMs) {
  uint64_t ageSec = ageMs / 1000;
  if (ageSec < 60) {
    return String(ageSec) + " sec ago";
  } else if (ageSec < 3600) {
    uint64_t minutes = ageSec / 60;
    uint64_t seconds = ageSec % 60;
    String result = String(minutes) + " min";
    if (seconds > 0) {
      result += " " + String(seconds) + " sec";
    }
    result += " ago";
    return result;
  } else {
    uint64_t hours = ageSec / 3600;
    uint64_t minutes = (ageSec % 3600) / 60;
    uint64_t seconds = ageSec % 60;
    String result = String(hours) + " hr";
    if (minutes > 0) {
      result += " " + String(minutes) + " min";
    }
    if (seconds > 0) {
      result += " " + String(seconds) + " sec";
    }
    result += " ago";
    return result;
  }
}

// --- Helper to filter messages for a given node ---
std::vector<Message> getNodeMessages(const String& nodeId) {
  std::vector<Message> result;
  for (auto &m : messages) {
    // Only include public messages (recipient == "ALL")
    if (m.recipient != "ALL") continue;
    if (m.nodeId == nodeId || m.relayID == nodeId) {
      result.push_back(m);
    }
  }
  return result;
}

void setupServerRoutes() {
  server.on("/", HTTP_GET, [](AsyncWebServerRequest* request) {
    request->send(200, "text/html", mainPageHtml);
  });

  server.on("/nodes", HTTP_GET, [](AsyncWebServerRequest* request) {
    request->send(200, "text/html", nodesPageHtml);
  });

  server.on("/messages", HTTP_GET, [](AsyncWebServerRequest* request) {
    String json = "{\"messages\":[";
    bool first = true;
    for (const auto& msg : messages) {
      if (!first) json += ",";
      json += "{\"nodeId\":\"" + msg.nodeId + "\",\"sender\":\"" + msg.sender + "\",\"recipient\":\"" + msg.recipient + "\",\"content\":\"" + msg.content + "\",\"source\":\"" + msg.source + "\",\"messageID\":\"" + msg.messageID + "\",\"relayID\":\"" + msg.relayID + "\",\"timeReceived\":" + String(msg.timeReceived);
      if (msg.source == "[LoRa]") {
        json += ",\"rssi\":" + String(msg.rssi) + ",\"snr\":" + String(msg.snr, 2);
      }
      json += "}";
      first = false;
    }
    json += "], \"currentDeviceTime\":" + String(millis()) + "}";
    request->send(200, "application/json", json);
  });

  server.on("/deviceCount", HTTP_GET, [](AsyncWebServerRequest* request) {
    updateMeshData();
    request->send(200, "application/json", "{\"totalCount\":" + String(getNodeCount()) + ", \"nodeId\":\"" + getCustomNodeId(getNodeId()) + "\"}");
  });

  // --- Updated /nodesData route for indirect nodes ---
  server.on("/nodesData", HTTP_GET, [](AsyncWebServerRequest* request) {
    updateMeshData();
    String json = "{\"wifiNodes\":[";
    auto wifiNodeList = mesh.getNodeList();
    bool firstWifi = true;
    for (auto node : wifiNodeList) {
      if (!firstWifi) json += ",";
      json += "\"" + getCustomNodeId(node) + "\"";
      firstWifi = false;
    }
    json += "], \"loraNodes\":[";
    bool firstLora = true;
    uint64_t currentTime = millis();
    const uint64_t FIFTEEN_MINUTES = 900000;
    for (auto const& [nodeId, loraNode] : loraNodes) {
      uint64_t lastSeenTime = loraNode.lastSeen;
      if (currentTime - lastSeenTime <= FIFTEEN_MINUTES) {
        if (!firstLora) json += ",";
        json += "{\"nodeId\":\"" + nodeId + "\",\"lastRSSI\":" + String(loraNode.lastRSSI)
             + ",\"lastSNR\":" + String(loraNode.lastSNR, 2)
             + ",\"lastSeen\":\"" + formatRelativeTime(currentTime - lastSeenTime) + "\""
             + ",\"statusEmoji\":\"" + loraNode.statusEmoji + "\"}";
        firstLora = false;
      }
    }
    json += "], \"indirectNodes\":[";
    bool firstIndirect = true;
    for (auto const& kv : indirectNodes) {
      const auto &node = kv.second;
      if (currentTime - node.lastSeen <= FIFTEEN_MINUTES) {
        if (!firstIndirect) json += ",";
        json += "{\"originatorId\":\"" + node.originatorId + "\",\"relayId\":\"" + node.relayId + "\",";
        json += "\"rssi\":" + String(node.rssi) + ",\"snr\":" + String(node.snr, 2) + ",\"lastSeen\":\"" + formatRelativeTime(currentTime - node.lastSeen) + "\","; 
        json += "\"statusEmoji\":\"" + node.statusEmoji + "\"}";
        firstIndirect = false;
      }
    }
    json += "]}";
    request->send(200, "application/json", json);
  });

  // --- Updated /update route to accept an optional "target" parameter ---
  server.on("/update", HTTP_POST, [](AsyncWebServerRequest* request) {
    String newMessage = "";
    String senderName = "";
    String target = "";
    if (request->hasParam("msg", true)) {
      newMessage = request->getParam("msg", true)->value();
    }
    if (request->hasParam("sender", true)) {
      senderName = request->getParam("sender", true)->value();
    }
    if (request->hasParam("target", true)) {
      target = request->getParam("target", true)->value();
    }
    newMessage.replace("<", "&lt;");
    newMessage.replace(">", "&gt;");
    senderName.replace("<", "&lt;");
    senderName.replace(">", "&gt;");
    target.replace("<", "&lt;");
    target.replace(">", "&gt;");

    if(target.length() == 0) {
         target = "ALL";
    }

    String messageID = generateMessageID(getCustomNodeId(getNodeId()));
    String relayID = getCustomNodeId(getNodeId());
    String constructedMessage = constructMessage(messageID, getCustomNodeId(getNodeId()), senderName, target, newMessage, relayID);

    addMessage(getCustomNodeId(getNodeId()), messageID, senderName, target, newMessage, "[LoRa]", relayID);
    Serial.printf("[LoRa Tx] Adding message: %s\n", constructedMessage.c_str());

    scheduleLoRaTransmission(constructedMessage);
    request->redirect("/");
  });

  server.on("/metrics", HTTP_GET, [](AsyncWebServerRequest* request) {
    request->send_P(200, "text/html", metricsPageHtml);
  });

server.on("/metricsHistoryData", HTTP_GET, [](AsyncWebServerRequest* request) {
  uint64_t now = millis();
  const uint64_t ONE_DAY = 86400000;
  String json = "{";
  // --- Direct (LoRa) Nodes History ---
  json += "\"loraNodes\":[";
  bool firstNode = true;
  for (auto const& kv : loraNodes) {
    if (!firstNode) json += ",";
    firstNode = false;
    const auto &node = kv.second;
    int bestRssi = node.history.empty() ? node.lastRSSI : node.history[0].rssi;
    float bestSnr = node.history.empty() ? node.lastSNR : node.history[0].snr;
    for (const auto &sample : node.history) {
      if (sample.rssi > bestRssi) {
        bestRssi = sample.rssi;
      }
      if (sample.snr > bestSnr) {
        bestSnr = sample.snr;
      }
    }
    json += "{\"nodeId\":\"" + node.nodeId + "\",\"bestRssi\":" + String(bestRssi)
         + ",\"bestSnr\":" + String(bestSnr, 2) + ",\"history\":[";
    bool firstSample = true;
    for (const auto &sample : node.history) {
      uint64_t ageMs = now - sample.timestamp;
      if (ageMs <= ONE_DAY && sample.rssi != 0) {
        if (!firstSample) json += ",";
        firstSample = false;
        String relativeTime = formatRelativeTime(ageMs);
        json += "{\"timestamp\":\"" + relativeTime
             + "\",\"rssi\":" + String(sample.rssi)
             + ",\"snr\":" + String(sample.snr, 2) + "}";
      }
    }
    json += "]}";
  }
  json += "],";

  // --- Indirect Nodes History ---
  json += "\"indirectNodes\":[";
  bool firstIndirect = true;
  for (auto const& kv : indirectNodes) {
    const auto &node = kv.second;
    if (!firstIndirect) json += ",";
    firstIndirect = false;
    int bestRssi = node.history.empty() ? node.rssi : node.history[0].rssi;
    float bestSnr = node.history.empty() ? node.snr : node.history[0].snr;
    for (const auto &sample : node.history) {
      if (sample.rssi > bestRssi) {
        bestRssi = sample.rssi;
      }
      if (sample.snr > bestSnr) {
        bestSnr = sample.snr;
      }
    }
    json += "{\"nodeId\":\"" + node.originatorId + "\",\"relayId\":\"" + node.relayId + "\",";
    json += "\"bestRssi\":" + String(bestRssi) + ",\"bestSnr\":" + String(bestSnr, 2) + ",\"history\":[";
    
    bool firstSample = true;
    for (const auto &sample : node.history) {
      uint64_t ageMs = now - sample.timestamp;
      if (ageMs <= ONE_DAY && sample.rssi != 0) {
        if (!firstSample) json += ",";
        firstSample = false;
        String relativeTime = formatRelativeTime(ageMs);
        json += "{\"timestamp\":\"" + relativeTime
             + "\",\"rssi\":" + String(sample.rssi)
             + ",\"snr\":" + String(sample.snr, 2) + "}";
      }
    }
    json += "],\"statusEmoji\":\"" + node.statusEmoji + "\"}";
  }
  json += "]";
  json += "}";
  request->send(200, "application/json", json);
});

  // --- New /loraDetails page remains unchanged ---
  server.on("/loraDetails", HTTP_GET, [](AsyncWebServerRequest *request){
    if(!request->hasParam("nodeId")) {
      String html = 
      "<!DOCTYPE html><html lang='en'><head>"
      "<meta charset='UTF-8'><meta name='viewport' content='width=device-width,initial-scale=1.0'>"
      "<title>LoRa Nodes List</title>"
      "<style>"
      "body { font-family: Arial, sans-serif; background-color: #f4f7f6; margin:0; padding:20px; }"
      ".nav-links { text-align:center; margin-bottom:20px; background-color:#eee; padding:10px; }"
      ".nav-links a { margin:0 15px; text-decoration:none; color:#007bff; font-weight:bold; }"
      ".nav-links a:hover { text-decoration:underline; }"
      "h2 { text-align:center; color:#333; }"
      ".node-list { max-width:600px; margin:10px auto; background:#fff; padding:10px; border-radius:8px; }"
      ".node-link { display:block; padding:8px; border-bottom:1px solid #ccc; color:#007bff; text-decoration:none; }"
      ".node-link:hover { background-color:#f0f0f0; }"
      "</style>"
      "</head><body>"
      "<div class='nav-links'>"
      "<a href='/'>Chat</a>"
      "<a href='/nodes'>All Nodes</a>"
      "<a href='/metrics'>History</a>"
      "</div>"
      "<h2>LoRa Node Details</h2>"
      "<p style='text-align:center;'>Select a LoRa node to see metrics and messages.</p>"
      "<div class='node-list'>";
      uint64_t currentTime = millis();
      const uint64_t FIFTEEN_MINUTES = 900000;
      bool anyLoRa = false;
      for(auto &kv : loraNodes) {
        if(currentTime - kv.second.lastSeen <= FIFTEEN_MINUTES) {
          anyLoRa = true;
          html += "<a class='node-link' href='/loraDetails?nodeId=" + kv.first + "'>"
                  + kv.first + "</a>";
        }
      }
      if(!anyLoRa) {
        html += "<p>No LoRa nodes seen in last 15 minutes.</p>";
      }
      html += "</div></body></html>";
      request->send(200, "text/html", html);
    } else {
      String nodeId = request->getParam("nodeId")->value();
      LoRaNode *found = nullptr;
      if(loraNodes.find(nodeId) != loraNodes.end()){
        found = &loraNodes[nodeId];
      }
      String html =
      "<!DOCTYPE html><html lang='en'><head>"
      "<meta charset='UTF-8'><meta name='viewport' content='width=device-width,initial-scale=1.0'>"
      "<title>LoRa Node " + nodeId + "</title>"
      "<style>"
      "body { font-family: Arial, sans-serif; background-color: #f4f7f6; margin:0; padding:20px; }"
      ".nav-links { text-align:center; margin-bottom:20px; background-color:#eee; padding:10px; }"
      ".nav-links a { margin:0 15px; text-decoration:none; color:#007bff; font-weight:bold; }"
      ".nav-links a:hover { text-decoration:underline; }"
      "h2 { text-align:center; color:#333; }"
      ".details { max-width:600px; margin:10px auto; background:#fff; padding:10px; border-radius:8px; }"
      "table { border-collapse: collapse; width: 100%; margin-bottom: 10px; }"
      "th, td { border: 1px solid #ccc; padding: 5px; text-align:left; font-size:0.9em; }"
      "th { background-color:#eee; }"
      ".section-title { font-weight:bold; font-size:1.1em; margin-top:20px; }"
      ".message-block { border:1px solid #ccc; margin:5px 0; padding:8px; border-radius:4px; }"
      ".message-block h4 { margin:0; font-size:0.9em; }"
      ".message-block p { margin:4px 0; font-size:0.85em; }"
      "</style></head><body>";
      html +=
      "<div class='nav-links'>"
      "<a href='/'>Chat</a>"
      "<a href='/nodes'>Node List</a>"
      "<a href='/metrics'>History</a>"
      "</div>";
      html += "<h2>LoRa Node: " + nodeId + "</h2>";
      if(!found) {
        html += "<div class='details'><p>No details available. This node hasn't been heard from or it's older than 24h.</p></div>";
        html += "</body></html>";
        request->send(200, "text/html", html);
        return;
      }
      uint64_t ageMs = millis() - found->lastSeen;
      String ageStr = formatRelativeTime(ageMs);
      html += "<div class='details'>";
      html += "<p><strong>Last RSSI:</strong> " + String(found->lastRSSI) + " dBm<br>";
      html += "<strong>Last SNR:</strong> " + String(found->lastSNR) + " dB<br>";
      html += "<strong>Last Seen:</strong> " + ageStr + "</p>";
      const uint64_t ONE_DAY = 86400000;
      bool anySample = false;
      html += "<div class='section-title'>RSSI/SNR History (24hr)</div>";
      html += "<table><thead><tr><th>Time Ago</th><th>RSSI (dBm)</th><th>SNR (dB)</th></tr></thead><tbody>";
      for(auto &sample : found->history) {
        uint64_t sampleAge = millis() - sample.timestamp;
        if(sampleAge <= ONE_DAY) {
          anySample = true;
          html += "<tr>";
          html += "<td>" + formatRelativeTime(sampleAge) + "</td>";
          html += "<td>" + String(sample.rssi) + "</td>";
          html += "<td>" + String(sample.snr,2) + "</td>";
          html += "</tr>";
        }
      }
      if(!anySample) {
        html += "<tr><td colspan='3'>No samples in last 24 hours.</td></tr>";
      }
      html += "</tbody></table>";
      auto nodeMsgs = getNodeMessages(nodeId);
      html += "<div class='section-title'>Messages Sent/Relayed</div>";
      if(nodeMsgs.empty()) {
        html += "<p>No messages from or through this node.</p>";
      } else {
        for(auto it = nodeMsgs.rbegin(); it != nodeMsgs.rend(); ++it) {
          uint64_t msgAgeMs = millis() - it->timeReceived;
          String msgAgeStr = formatRelativeTime(msgAgeMs);
          html += "<div class='message-block'>";
          html += "<h4>Sender: " + it->sender + " | NodeID: " + it->nodeId + "</h4>";
          html += "<p><strong>Content:</strong> " + it->content + "</p>";
          html += "<p><strong>Source:</strong> " + it->source + "</p>";
          if(!it->relayID.isEmpty()) {
            html += "<p><strong>RelayID:</strong> " + it->relayID + "</p>";
          }
          html += "<p><em>" + msgAgeStr + "</em></p>";
          html += "</div>";
        }
      }
      html += "</div></body></html>";
      request->send(200, "text/html", html);
    }
  });
}

void updateMeshData() {
  mesh.update();
  totalNodeCount = mesh.getNodeList().size();
  currentNodeId = mesh.getNodeId();
}

int getNodeCount() {
  return totalNodeCount;
}

uint32_t getNodeId() {
  return currentNodeId;
}

void initServer() {
  setupServerRoutes();
}
