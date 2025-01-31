//Test v1.00.002
//31-01-2025
//MAKE SURE ALL NODES USE THE SAME VERSION OR EXPECT STRANGE THINGS HAPPENING.
//EU868 Band P (869.4 MHz - 869.65 MHz): 10%, 500 mW ERP (10% 24hr 8640 seconds = 6 mins per hour TX Time.)
//After Accounting for Heartbeats: 20 sec after boot then every 15 mins therafter.
//Per Hour: 136 Max Char messages within the 6-minute (360,000 ms) duty cycle
//Per Day: 3,296 Max Char messages within the 8,640,000 ms (10% duty cycle) allowance
//changes network name to meshmingle.co.uk so people can find the site and see how it works.
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
#include <vector>         // For handling list of messages
#include <map>            // For unified retransmission tracking
#include <RadioLib.h>

// Unified Retransmission Tracking Structure
struct TransmissionStatus {
  bool transmittedViaWiFi = false;
  bool transmittedViaLoRa = false;
  bool addedToMessages = false;  // Flag to track if the message has been added to messages vector
};

// Map to track retransmissions
std::map<String, TransmissionStatus> messageTransmissions;

// LoRa Parameters
#define PAUSE 5400000  
#define FREQUENCY 869.4000 
#define BANDWIDTH 250.0 
#define SPREADING_FACTOR 11 
#define TRANSMIT_POWER 22 
#define CODING_RATE 8  
String rxdata;
volatile bool rxFlag = false;
long counter = 0;
uint64_t tx_time;
uint64_t last_tx = 0;
uint64_t minimum_pause = 0;
unsigned long lastTransmitTime = 0;  
String fullMessage;                  

// Function to handle LoRa received packets
void rx() {
  rxFlag = true;
}

// Define the maximum allowed duty cycle (10%)
#define DUTY_CYCLE_LIMIT_MS 360000   // 6 minutes in a 60-minute window
#define DUTY_CYCLE_WINDOW   3600000  // 60 minutes in milliseconds

// Duty Cycle Variables
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

struct Message {
  String nodeId;     
  String sender;
  String content;
  String source;     
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

String constructMessage(const String& messageID, const String& originatorID, const String& sender, const String& content, const String& relayID) {
  String messageWithoutCRC = messageID + "|" + originatorID + "|" + sender + "|" + content + "|" + relayID;
  uint16_t crc = crc16_ccitt((const uint8_t *)messageWithoutCRC.c_str(), messageWithoutCRC.length());
  char crcStr[5];
  sprintf(crcStr, "%04X", crc);
  String fullMessage = messageWithoutCRC + "|" + String(crcStr);
  return fullMessage;
}

// *** METRICS HISTORY CHANGES ***
// Store multiple samples for each node to track signal history (RSSI, SNR, timestamp).
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

  // Rolling history of signal metrics (RSSI, SNR, timestamp).
  std::vector<NodeMetricsSample> history; 
};

std::map<String, LoRaNode> loraNodes;
// *** END METRICS HISTORY CHANGES ***

unsigned long lastCleanupTime = 0;
const unsigned long cleanupInterval = 60000; // 1 minute

void cleanupLoRaNodes() {
  uint64_t currentTime = millis();
  const uint64_t timeout = 86400000; // 24 hours node is removed if not seen

  for (auto it = loraNodes.begin(); it != loraNodes.end();) {
    if (currentTime - it->second.lastSeen > timeout) {
      Serial.printf("[LoRa Nodes] Removing inactive LoRa node: %s\n", it->first.c_str());
      it = loraNodes.erase(it);
    } else {
      ++it;
    }
  }
}

void addMessage(const String& nodeId, const String& messageID, const String& sender, String content, const String& source, const String& relayID, int rssi = 0, float snr = 0.0) {
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

  String finalSource = "";
  if (nodeId != getCustomNodeId(getNodeId())) {
    finalSource = source;
  }

  Message newMessage = {
    nodeId, sender, content, finalSource, messageID, relayID, rssi, snr, millis()
  };

  messages.insert(messages.begin(), newMessage);
  status.addedToMessages = true;

  if (messages.size() > maxMessages) {
    messages.pop_back();
  }

  Serial.printf("Message added: NodeID: %s, Sender: %s, Content: %s, Source: %s, ID: %s, RelayID: %s\n",
                nodeId.c_str(), sender.c_str(), content.c_str(), finalSource.c_str(), messageID.c_str(), relayID.c_str());
}

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

    if (firstSeparator == -1 || secondSeparator == -1 || thirdSeparator == -1 || fourthSeparator == -1) {
        Serial.println("[LoRa Schedule] Invalid message format.");
        return;
    }

    String messageID = messageWithoutCRC.substring(0, firstSeparator);
    String originatorID = messageWithoutCRC.substring(firstSeparator + 1, secondSeparator);
    String senderID = messageWithoutCRC.substring(secondSeparator + 1, thirdSeparator);
    String messageContent = messageWithoutCRC.substring(thirdSeparator + 1, fourthSeparator);

    String newRelayID = getCustomNodeId(getNodeId());
    String updatedMessage = constructMessage(messageID, originatorID, senderID, messageContent, newRelayID);

    fullMessage = updatedMessage;
    loRaTransmitDelay = millis() + random(2201, 5000);
    Serial.printf("[LoRa Schedule] Scheduled after %lu ms: %s\n", loRaTransmitDelay - millis(), updatedMessage.c_str());
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
  lines[1] = "MM  MM  E      S      H   H  MM  MM  I  NN  N  G      L      E      ";
  lines[2] = "M MM M  EEEE   SSSSS  HHHHH  M MM M  I  N N N  G  GG  L      EEEE   ";
  lines[3] = "M    M  E          S  H   H  M    M  I  N  NN  G   G  L      E      ";
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
    carouselIndex++;

    if (messages.empty()) {
      carouselIndex = 0;
    } else {
      if (carouselIndex > (int)messages.size()) {
        carouselIndex = 0;
      }
    }
  }

  display.clear();
  display.setFont(ArialMT_Plain_10);

  if (carouselIndex == 0) {
    drawMainScreen(-1);
  } else {
    int msgIndex = carouselIndex - 1;
    if (msgIndex < (int)messages.size()) {
      String nodeLine = "Node: " + messages[msgIndex].nodeId;
      display.drawString(0, 0, nodeLine);

      String nameLine = "Name: " + messages[msgIndex].sender;
      display.drawString(0, 13, nameLine);

      display.drawStringMaxWidth(0, 26, 128, messages[msgIndex].content);
    }
  }

  display.display();
}

long lastTxTimeMillisVar = -1;

void transmitWithDutyCycle(const String& message) {
    // Check if the LoRa transmit delay has expired
    if (millis() < loRaTransmitDelay) {
        Serial.println("[LoRa Tx] LoRa delay not expired, waiting...");
        return;
    }

    // Extract message ID for tracking
    int separatorIndex = message.indexOf('|');
    if (separatorIndex == -1) {
        Serial.println("[LoRa Tx] Invalid message format.");
        return;
    }
    String messageID = message.substring(0, separatorIndex);

    // Check if the message has already been transmitted
    auto& status = messageTransmissions[messageID];
    if (status.transmittedViaLoRa) {
        Serial.println("[LoRa Tx] Message already sent via LoRa, skipping...");
        return;
    }

    // Check duty cycle compliance
    if (isDutyCycleAllowed()) {
        // Check if any incoming packets are being received
        if (radio.available()) {
            Serial.println("[LoRa Tx] Currently receiving a packet. Delaying transmission...");
            loRaTransmitDelay = millis() + 500; // Short delay to allow RX to complete
            return;
        }

        // Not receiving, proceed with transmission
        tx_time = millis(); // Start transmission timer
        Serial.printf("[LoRa Tx] Transmitting: %s\n", message.c_str());

        heltec_led(50); // Turn on LED to indicate transmission
        int transmitStatus = radio.transmit(message.c_str());
        tx_time = millis() - tx_time; // Calculate transmission time
        heltec_led(0); // Turn off LED

        if (transmitStatus == RADIOLIB_ERR_NONE) {
            // Transmission successful
            Serial.printf("[LoRa Tx] Sent successfully (%i ms)\n", (int)tx_time);
            status.transmittedViaLoRa = true;

            // Update duty cycle delay
            calculateDutyCyclePause(tx_time);
            last_tx = millis(); // Update last transmission time
            drawMainScreen(tx_time); // Refresh display

            // Start receiving mode again
            delay(200);
            radio.startReceive();

            // If the message is not a heartbeat, also send via WiFi
            if (!message.startsWith("HEARTBEAT|")) {
                transmitViaWiFi(message);
            }
        } else {
            // Transmission failed
            Serial.printf("[LoRa Tx] Transmission failed with error code: %i\n", transmitStatus);
        }
    } else {
        // If duty cycle limits are reached, delay the next attempt
        Serial.printf("[LoRa Tx] Duty cycle limit reached. Wait %i seconds.\n",
                      (int)((minimum_pause - (millis() - last_tx)) / 1000) + 1);
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

  if (isDutyCycleAllowed()) {
    tx_time = millis();
    Serial.printf("[Heartbeat Tx] Sending: %s\n", heartbeatMessage.c_str());
    heltec_led(50);
    int transmitStatus = radio.transmit(heartbeatMessage.c_str());
    tx_time = millis() - tx_time;
    heltec_led(0);

    if (transmitStatus == RADIOLIB_ERR_NONE) {
      Serial.printf("[Heartbeat Tx] OK (%i ms)\n", (int)tx_time);
      calculateDutyCyclePause(tx_time);
      last_tx = millis();
      drawMainScreen(tx_time);
      delay(200);
      radio.startReceive();
    } else {
      Serial.printf("[Heartbeat Tx] Failed (%i)\n", transmitStatus);
    }
  } else {
    Serial.println("[Heartbeat Tx] Duty cycle limit reached, skipping.");
  }
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
  radio.setDio1Action(rx);

  RADIOLIB_OR_HALT(radio.setFrequency(FREQUENCY));
  RADIOLIB_OR_HALT(radio.setBandwidth(BANDWIDTH));
  RADIOLIB_OR_HALT(radio.setSpreadingFactor(SPREADING_FACTOR));
  RADIOLIB_OR_HALT(radio.setCodingRate(CODING_RATE));
  RADIOLIB_OR_HALT(radio.setOutputPower(TRANSMIT_POWER));

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

  randomSeed(analogRead(0));
}

void loop() {
  esp_task_wdt_reset();
  heltec_loop();

  mesh.update();

  // Send the first heartbeat after 20 seconds, then subsequent heartbeats every 15 minutes
  if (millis() - lastHeartbeatTime >= firstHeartbeatDelay && lastHeartbeatTime == 0) {
    sendHeartbeat();
    lastHeartbeatTime = millis();  // After the first heartbeat, set the lastHeartbeatTime
  } else if (millis() - lastHeartbeatTime >= heartbeatInterval && lastHeartbeatTime > 0) {
    sendHeartbeat();  // Send subsequent heartbeats every 15 minutes
    lastHeartbeatTime = millis();  // Update the last heartbeat time
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

          if (messageWithoutCRC.startsWith("HEARTBEAT|")) {
            String senderNodeId = messageWithoutCRC.substring(strlen("HEARTBEAT|"));
            Serial.printf("[LoRa Rx] Heartbeat from %s\n", senderNodeId.c_str());
            int rssi = radio.getRSSI();
            float snr = radio.getSNR();
            uint64_t currentTime = millis();

            if (senderNodeId != getCustomNodeId(getNodeId())) {
              // *** METRICS HISTORY CHANGES ***
              LoRaNode& node = loraNodes[senderNodeId];
              node.nodeId = senderNodeId;
              node.lastRSSI = rssi;
              node.lastSNR = snr;
              node.lastSeen = currentTime;

              NodeMetricsSample sample = {currentTime, rssi, snr};
              node.history.push_back(sample);
              if (node.history.size() > 60) {
                node.history.erase(node.history.begin());
              }
              // *** END METRICS HISTORY CHANGES ***
              Serial.printf("[LoRa Nodes] Updated/Added node: %s (Heartbeat)\n", senderNodeId.c_str());
            } else {
              Serial.println("[LoRa Rx] Own heartbeat, ignore.");
            }
          } else {
            int firstSeparator = messageWithoutCRC.indexOf('|');
            int secondSeparator = messageWithoutCRC.indexOf('|', firstSeparator + 1);
            int thirdSeparator = messageWithoutCRC.indexOf('|', secondSeparator + 1);
            int fourthSeparator = messageWithoutCRC.indexOf('|', thirdSeparator + 1);

            if (firstSeparator == -1 || secondSeparator == -1 || thirdSeparator == -1 || fourthSeparator == -1) {
              Serial.println("[LoRa Rx] Invalid format.");
            } else {
              String messageID = messageWithoutCRC.substring(0, firstSeparator);
              String originatorID = messageWithoutCRC.substring(firstSeparator + 1, secondSeparator);
              String senderID = messageWithoutCRC.substring(secondSeparator + 1, thirdSeparator);
              String messageContent = messageWithoutCRC.substring(thirdSeparator + 1, fourthSeparator);
              String relayID = messageWithoutCRC.substring(fourthSeparator + 1);

              int rssi = radio.getRSSI();
              float snr = radio.getSNR();

              if (originatorID == getCustomNodeId(getNodeId())) {
                Serial.println("[LoRa Rx] Own message, ignore.");
              } else {
                auto& status = messageTransmissions[messageID];
                if (status.transmittedViaWiFi && status.transmittedViaLoRa) {
                  Serial.println("[LoRa Rx] Already relayed both WiFi/LoRa, ignore...");
                } else {
                  addMessage(originatorID, messageID, senderID, messageContent, "[LoRa]", relayID, rssi, snr);
                  if (!status.transmittedViaLoRa) {
                    scheduleLoRaTransmission(message);
                  }

                  uint64_t currentTime = millis();
                  // *** METRICS HISTORY CHANGES ***
                  // Update LoRaNode for 'relayID'
                  if (relayID != getCustomNodeId(getNodeId())) {
                    LoRaNode& relayNode = loraNodes[relayID];
                    relayNode.nodeId = relayID;
                    relayNode.lastRSSI = rssi;
                    relayNode.lastSNR = snr;
                    relayNode.lastSeen = currentTime;

                    NodeMetricsSample sample = {currentTime, rssi, snr};
                    relayNode.history.push_back(sample);
                    if (relayNode.history.size() > 60) {
                      relayNode.history.erase(relayNode.history.begin());
                    }
                    Serial.printf("[LoRa Nodes] Updated/Added node: %s\n", relayID.c_str());
                  } else {
                    Serial.println("[LoRa Nodes] RelayID is own node, not updating.");
                  }
                  // *** END METRICS HISTORY CHANGES ***
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
  displayCarousel();
  dnsServer.processNextRequest();

  if (millis() - lastCleanupTime >= cleanupInterval) {
    cleanupLoRaNodes();
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

  // Extract CRC from the message
  int lastSeparatorIndex = message.lastIndexOf('|');
  if (lastSeparatorIndex == -1) {
    Serial.println("[WiFi Rx] Invalid format (no CRC).");
    return;
  }
  String crcStr = message.substring(lastSeparatorIndex + 1);
  String messageWithoutCRC = message.substring(0, lastSeparatorIndex);

  // Compute and verify CRC
  uint16_t receivedCRC = (uint16_t)strtol(crcStr.c_str(), NULL, 16);
  uint16_t computedCRC = crc16_ccitt((const uint8_t *)messageWithoutCRC.c_str(), messageWithoutCRC.length());

  if (receivedCRC != computedCRC) {
    Serial.printf("[WiFi Rx] CRC mismatch. Recv: %04X, Computed: %04X\n", receivedCRC, computedCRC);
    return;
  } else {
    Serial.println("[WiFi Rx] CRC valid.");
  }

  // Parse message components
  int firstSeparator = messageWithoutCRC.indexOf('|');
  int secondSeparator = messageWithoutCRC.indexOf('|', firstSeparator + 1);
  int thirdSeparator = messageWithoutCRC.indexOf('|', secondSeparator + 1);
  int fourthSeparator = messageWithoutCRC.indexOf('|', thirdSeparator + 1);

  if (firstSeparator == -1 || secondSeparator == -1 || thirdSeparator == -1 || fourthSeparator == -1) {
    Serial.println("[WiFi Rx] Invalid format (missing separators).");
    return;
  }

  String messageID = messageWithoutCRC.substring(0, firstSeparator);
  String originatorID = messageWithoutCRC.substring(firstSeparator + 1, secondSeparator);
  String senderID = messageWithoutCRC.substring(secondSeparator + 1, thirdSeparator);
  String messageContent = messageWithoutCRC.substring(thirdSeparator + 1, fourthSeparator);
  String relayID = messageWithoutCRC.substring(fourthSeparator + 1);

  // **Convert originatorID to custom format if necessary**
  if (!originatorID.startsWith("!M")) {
    // Attempt to convert numeric ID to custom format
    uint32_t numericId = originatorID.toInt();
    originatorID = getCustomNodeId(numericId);
    Serial.printf("[WiFi Rx] Converted originatorID to custom format: %s\n", originatorID.c_str());
  }

  // **Convert relayID to custom format if necessary**
  if (!relayID.startsWith("!M")) {
    // Attempt to convert numericRelayId to custom format
    uint32_t numericRelayId = relayID.toInt();
    relayID = getCustomNodeId(numericRelayId);
    Serial.printf("[WiFi Rx] Converted relayID to custom format: %s\n", relayID.c_str());
  }

  // **Handle Heartbeat Messages**
  if (messageWithoutCRC.startsWith("HEARTBEAT|")) {
    Serial.println("[WiFi Rx] Skipping heartbeat relay over WiFi.");
    return;
  }

  // **Ignore Own Messages**
  if (originatorID == getCustomNodeId(getNodeId())) {
    Serial.println("[WiFi Rx] Own message, just adding locally...");
    addMessage(originatorID, messageID, senderID, messageContent, "[WiFi]", relayID);
    return;
  }

  // **Check if Message Already Relayed**
  auto& status = messageTransmissions[messageID];
  if (status.transmittedViaWiFi && status.transmittedViaLoRa) {
    Serial.println("[WiFi Rx] Already relayed both via WiFi and LoRa, ignoring...");
    return;
  }

  // **Add Message to Local Storage**
  Serial.printf("[WiFi Rx] Adding message: %s\n", message.c_str());
  addMessage(originatorID, messageID, senderID, messageContent, "[WiFi]", relayID);

  // **Schedule LoRa Transmission if Not Already Done**
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
    let deviceCurrentTime = 0;  // Global variable to store the device's current time

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

      // Disable the send button for 15 seconds
      sendButton.disabled = true;
      sendButton.value = 'Wait 15s';

      fetch('/update', {
        method: 'POST',
        body: formData
      })
      .then(response => {
        if (!response.ok) throw new Error('Failed to send message');
        messageInput.value = '';
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
              if (minutes > 0) {
                timestamp = `${hours} hr ${minutes} min ago`;
              } else {
                timestamp = `${hours} hr ago`;
              }
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
            let relayIdHtml = '';
            if (msg.relayID && msg.relayID !== currentNodeId) {
              relayIdHtml = `<span class="message-relayid">Relay ID: ${msg.relayID}</span>`;
            }

            let rssiSnrHtml = '';
            if (msg.source === '[LoRa]' && msg.rssi !== undefined && msg.snr !== undefined) {
              rssiSnrHtml = `<span class="message-rssi-snr">RSSI: ${msg.rssi} dBm, SNR: ${msg.snr} dB</span>`;
            }

            li.innerHTML = `
              <span class="message-nodeid">${nodeIdHtml}</span>
              <div class="message-content">${senderHtml}${msg.content}</div>
              ${relayIdHtml}
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
  
  <form id="messageForm">
    <input type="text" id="nameInput" name="sender" placeholder="Your name" maxlength="15" required>
    <input type="text" id="messageInput" name="msg" placeholder="Your message" maxlength="150" required>
    <input type="submit" id="sendButton" value="Send">
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
    #nodeCount { 
      margin: 20px auto; 
      max-width: 600px; 
      font-weight: bold;
      text-align: left;
    }
    .node-section {
      margin-bottom: 30px;
    }
    /* Responsive Design */
    @media (max-width: 600px) {
      .node-info {
        margin-left: 0;
      }
    }
    /* Navigation Links */
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
          const wifiUl = document.getElementById('wifiNodeList');
          const loraUl = document.getElementById('loraNodeList');

          // Clear previous lists
          wifiUl.innerHTML = '';
          loraUl.innerHTML = '';

          // Update WiFi Nodes list
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
              <div class="node-info">
                <!-- Add any additional WiFi node info here if needed -->
              </div>
            `;
            wifiUl.appendChild(li);
          });

          // Update LoRa Nodes list
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
            `;
            loraUl.appendChild(li);
          });
        })
        .catch(error => console.error('Error fetching nodes:', error));
    }

    window.onload = function() {
      fetchNodes();
      setInterval(fetchNodes, 5000); // Update node data every 5 seconds
    };
  </script>
</head>
<body>
  <div class="nav-links">
    <a href="/">Back</a> | <a href="/metrics">History</a>
  </div>
  <h2>Meshmingle Nodes</h2>
  
  <!-- Node count for WiFi -->
  <div class="node-section">
    <span id="wifiCount">WiFi Nodes Connected: 0</span>
    <ul id="wifiNodeList"></ul>
  </div>

  <!-- Node count for LoRa -->
  <div class="node-section">
    <span id="loraCount">Direct LoRa Nodes Active: 0</span>
    <ul id="loraNodeList"></ul>
  </div>
</body>
</html>
)rawliteral";

// *** METRICS HISTORY PAGE ***
const char metricsPageHtml[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="en">
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1.0">
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
    .node-block {
      background-color: #fff;
      border: 2px solid #ccc;
      border-radius: 8px;
      margin: 20px auto;
      max-width: 600px;
      padding: 10px;
      box-sizing: border-box;
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
    .timestamp {
      font-size: 0.85em;
      color: #666;
    }
    /* Flexbox styles for navigation links */
    .nav-links {
      display: flex;                 /* Enables Flexbox layout */
      justify-content: center;       /* Centers the links horizontally */
      align-items: center;           /* Aligns the links vertically */
      gap: 10px;                     /* Adds space between the links */
      margin-bottom: 20px;           /* Adds space below the links */
    }
    .nav-links a {
      text-decoration: none;         /* Removes underline from links */
      color: #007bff;                /* Sets link color */
      font-weight: bold;             /* Makes links bold */
    }
    .nav-links a:hover {
      text-decoration: underline;    /* Underlines links on hover */
    }
  </style>
  <script>
    // Fetch Node ID and set it on page load
    window.onload = function() {
      // Assuming the Node ID is being fetched from /deviceCount route
      fetch('/deviceCount')
        .then(response => response.json())
        .then(data => {
          const nodeIdElement = document.getElementById('nodeIdDisplay');
          nodeIdElement.textContent = `Node ID: ${data.nodeId}`;
        })
        .catch(error => console.error('Error fetching Node ID:', error));

      fetchHistory();
      setInterval(fetchHistory, 5000); 
    };

    function fetchHistory() {
      fetch('/metricsHistoryData')
        .then(response => response.json())
        .then(data => {
          const container = document.getElementById('historyContainer');
          container.innerHTML = '';

          // Display Node ID, Best RSSI, and Best SNR for each node at the top in a table
          data.loraNodes.forEach(node => {
            const nodeBlock = document.createElement('div');
            nodeBlock.classList.add('node-block');

            // Display Node ID above the table
            const nodeTitle = document.createElement('div');
            nodeTitle.classList.add('node-title');
            nodeTitle.textContent = `Node ID: ${node.nodeId}`;
            nodeBlock.appendChild(nodeTitle);

            // Create a table for Best RSSI and Best SNR
            const nodeTable = document.createElement('table');
            const nodeHeader = document.createElement('thead');
            nodeHeader.innerHTML = `
              <tr>
                <th>Best Signal RSSI (dBm)</th>
                <th>Best Signal SNR (dB)</th>
              </tr>`;
            nodeTable.appendChild(nodeHeader);

            const nodeBody = document.createElement('tbody');
            const nodeRow = document.createElement('tr');
            nodeRow.innerHTML = `
              <td>${node.bestRssi}</td>
              <td>${node.bestSnr}</td>`;
            nodeBody.appendChild(nodeRow);
            nodeTable.appendChild(nodeBody);

            // Append the node table to the node block
            nodeBlock.appendChild(nodeTable);

            // Create table for the history data
            const historyTable = document.createElement('table');
            const historyHeader = document.createElement('thead');
            historyHeader.innerHTML = `
              <tr>
                <th>Time Ago</th>
                <th>RSSI (dBm)</th>
                <th>SNR (dB)</th>
              </tr>`;
            historyTable.appendChild(historyHeader);

            const historyBody = document.createElement('tbody');
            node.history.forEach(sample => {
              const row = document.createElement('tr');

              const tsCell = document.createElement('td');
              tsCell.textContent = sample.timestamp; // now contains "X min ago" from server
              row.appendChild(tsCell);

              const rssiCell = document.createElement('td');
              rssiCell.textContent = sample.rssi;
              row.appendChild(rssiCell);

              const snrCell = document.createElement('td');
              snrCell.textContent = sample.snr;
              row.appendChild(snrCell);

              historyBody.appendChild(row);
            });
            historyTable.appendChild(historyBody);
            nodeBlock.appendChild(historyTable);

            container.appendChild(nodeBlock);
          });
        })
        .catch(error => {
          console.error('Error fetching metrics history:', error);
        });
    }
  </script>
</head>
<body>
  <div class="nav-links">
    <a href="/nodes">NodeList</a> | <a href="/">Chat</a>
  </div>
  <h2>LoRa 24hr History</h2>
  <!-- Node ID displayed below the title -->
  <div class="node-id" id="nodeIdDisplay">Node ID: Loading...</div>
  <div id="historyContainer"></div>
</body>
</html>
)rawliteral";

/**
 * Helper function to convert an elapsed time in milliseconds 
 * into a human-readable relative time string.
 * e.g., 30 sec => "30 sec ago", 5 min => "5 min ago", 2 hr 30 min 15 sec ago
 */
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

// ----------------------------------------------------------------------------
// NEW HELPER to filter messages for a given nodeId
// We consider the node the "origin" (msg.nodeId == nodeId) or the "relay" (msg.relayID == nodeId).
// ----------------------------------------------------------------------------
std::vector<Message> getNodeMessages(const String& nodeId) {
  std::vector<Message> result;
  for (auto &m : messages) {
    // If the node was either the origin or the relay
    if (m.nodeId == nodeId || m.relayID == nodeId) {
      result.push_back(m);
    }
  }
  return result;
}

void setupServerRoutes() {
  server.on("/", HTTP_GET, [](AsyncWebServerRequest* request) {
    serveHtml(request, mainPageHtml);
  });

  server.on("/nodes", HTTP_GET, [](AsyncWebServerRequest* request) {
    serveHtml(request, nodesPageHtml);
  });

  server.on("/messages", HTTP_GET, [](AsyncWebServerRequest* request) {
    String json = "{\"messages\":[";
    bool first = true;
    for (const auto& msg : messages) {
      if (!first) json += ",";
      json += "{\"nodeId\":\"" + msg.nodeId + "\",\"sender\":\"" + msg.sender + "\",\"content\":\"" + msg.content + "\",\"source\":\"" + msg.source + "\",\"messageID\":\"" + msg.messageID + "\",\"relayID\":\"" + msg.relayID + "\",\"timeReceived\":" + String(msg.timeReceived);
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
    const uint64_t FIFTEEN_MINUTES = 900000; // 15 minutes in milliseconds
    for (auto const& [nodeId, loraNode] : loraNodes) {
        uint64_t lastSeenTime = loraNode.lastSeen;
        if (currentTime - lastSeenTime <= FIFTEEN_MINUTES) {  // Only include nodes seen in the last 15 minutes
            if (!firstLora) json += ",";
            json += "{\"nodeId\":\"" + nodeId + "\",\"lastRSSI\":" + String(loraNode.lastRSSI)
                  + ",\"lastSNR\":" + String(loraNode.lastSNR, 2)
                  + ",\"lastSeen\":\"" + formatRelativeTime(currentTime - lastSeenTime) + "\"}";
            firstLora = false;
        }
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

    String messageID = generateMessageID(getCustomNodeId(getNodeId()));
    String relayID = getCustomNodeId(getNodeId());
    String constructedMessage = constructMessage(messageID, getCustomNodeId(getNodeId()), senderName, newMessage, relayID);

    addMessage(getCustomNodeId(getNodeId()), messageID, senderName, newMessage, "[LoRa]", relayID);
    Serial.printf("[LoRa Tx] Adding message: %s\n", constructedMessage.c_str());

    scheduleLoRaTransmission(constructedMessage);
    request->redirect("/");
  });

  // *** METRICS HISTORY CHANGES ***
  server.on("/metrics", HTTP_GET, [](AsyncWebServerRequest* request) {
    request->send_P(200, "text/html", metricsPageHtml);
  });

  // Return only the last hour of samples, converting timestamp to relative time
  server.on("/metricsHistoryData", HTTP_GET, [](AsyncWebServerRequest* request) {
    uint64_t now = millis();
    const uint64_t ONE_DAY = 86400000;
    String json = "{\"loraNodes\":[";

    bool firstNode = true;
    for (auto const& kv : loraNodes) {
      if (!firstNode) json += ",";
      firstNode = false;

      const auto &node = kv.second;

      // Find Best RSSI and Best SNR independently
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
    json += "]}";

    request->send(200, "application/json", json);
  });

  // ---------------------------------------------------------------
  // NEW PAGE: /loraDetails
  // Lists all LoRa node IDs if nodeId is not provided.
  // If ?nodeId= is provided, shows that node's metrics & messages.
  // ---------------------------------------------------------------
server.on("/loraDetails", HTTP_GET, [](AsyncWebServerRequest *request){
  // Check if we have a query parameter "nodeId"
  if(!request->hasParam("nodeId")) {
    // Show a list of LoRa nodes
    String html = 
    "<!DOCTYPE html><html lang='en'><head>"
    "<meta charset='UTF-8'><meta name='viewport' content='width=device-width,initial-scale=1.0'>"
    "<title>LoRa Nodes List</title>"
    "<style>"
    "body { font-family: Arial, sans-serif; background-color: #f4f7f6; margin:0; padding:20px; }"
    ".nav-links { text-align:center; margin-bottom:20px; background-color:#eee; padding:10px; }"
    ".nav-links a { margin:0 15px; text-decoration:none; color:#007bff; font-weight:bold; }"
    "h2 { text-align:center; color:#333; }"
    ".node-list { max-width:600px; margin:10px auto; background:#fff; padding:10px; border-radius:8px; }"
    ".node-link { display:block; padding:8px; border-bottom:1px solid #ccc; color:#007bff; text-decoration:none; }"
    ".node-link:hover { background-color:#f0f0f0; }"
    "</style>"
    "</head><body>"
    "<div class='nav-links'>"
    "<a href='/'>Chat</a>"
    "<a href='/nodes'>All Nodes</a>"
    "<a href='/metrics'>Signal History</a>"
    "</div>"
    "<h2>LoRa Node Details</h2>"
    "<p style='text-align:center;'>Select a LoRa node to see metrics and messages.</p>"
    "<div class='node-list'>";

    // Gather list of LoRa nodes (active in last 15 mins)
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
  } 
  else {
    // We have nodeId param, let's show that node's details
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
    "h2 { text-align:center; color:#333; }"
    ".details { max-width:600px; margin:10px auto; background:#fff; padding:10px; border-radius:8px; }"
    "table { border-collapse: collapse; width: 100%; margin-bottom: 10px; }"
    "th, td { border: 1px solid #ccc; padding: 5px; text-align:left; font-size: 0.9em; }"
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
    //"<a href='/loraDetails'>LoRa List</a>"
    "</div>";

    html += "<h2>LoRa Node: " + nodeId + "</h2>";

    if(!found) {
      // Node not found at all
      html += "<div class='details'><p>No details available. This node hasn't been heard from or it's older than 24h.</p></div>";
      html += "</body></html>";
      request->send(200, "text/html", html);
      return;
    }

    // If found, show current metrics
    uint64_t ageMs = millis() - found->lastSeen;
    String ageStr = formatRelativeTime(ageMs);

    html += "<div class='details'>";
    html += "<p><strong>Last RSSI:</strong> " + String(found->lastRSSI) + " dBm<br>";
    html += "<strong>Last SNR:</strong> " + String(found->lastSNR) + " dB<br>";
    html += "<strong>Last Seen:</strong> " + ageStr + "</p>";

    // Rolling history table
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

    // Show messages from this node
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
        if(it->source == "[LoRa]") {
          html += "<p><strong>RSSI:</strong> " + String(it->rssi) + " dBm, "
                  "<strong>SNR:</strong> " + String(it->snr,2) + " dB</p>";
        }
        html += "<p><em>" + msgAgeStr + "</em></p>";
        html += "</div>";
      }
    }

    html += "</div></body></html>";
    request->send(200, "text/html", html);
  }
});
  // ---------------------------------------------------------------

}

// Serve static HTML from PROGMEM
void serveHtml(AsyncWebServerRequest* request, const char* htmlContent) {
  request->send(200, "text/html", htmlContent);
}

int getNodeCount() {
  return totalNodeCount;
}

uint32_t getNodeId() {
  return currentNodeId;
}

void updateMeshData() {
  mesh.update();
  totalNodeCount = mesh.getNodeList().size();
  currentNodeId = mesh.getNodeId();
}
