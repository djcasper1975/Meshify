//Test v1.00.002
//01-02-2025
//MAKE SURE ALL NODES USE THE SAME VERSION OR EXPECT STRANGE THINGS HAPPENING.
//Changed network name.
//added private messages to other nodes.
////////////////////////////////////////////////////////////////////////
// M    M  EEEEE  SSSSS  H   H  M    M  I  N   N  GGGGG  L      EEEEE //
// MM  MM  E      S      H   H  MM  MM  I  NN  N  G      L      E     //
// M MM M  EEEE   SSSSS  HHHHH  M MM M  I  N N N  G  GG  L      EEEE  //
// M    M  E          S  H   H  M    M  I  N  NN  G   G  L      E     //
// M    M  EEEEE  SSSSS  H   H  M    M  I  N   N   GGG   LLLLL  EEEEE //
////////////////////////////////////////////////////////////////////////
#include <painlessMesh.h>
#include <WiFi.h>
#include <ESPAsyncWebServer.h>
#include <DNSServer.h>
#include <Wire.h>
#include <esp_task_wdt.h> // Watchdog timer library
#include <vector>         // For handling list of messages
#include <map>            // For unified retransmission tracking

// Function Prototypes
uint32_t getNodeId();
void setupServerRoutes();
void updateMeshData();
String formatRelativeTime(uint64_t ageMs);
void serveHtml(AsyncWebServerRequest* request, const char* htmlContent);
void addMessage(const String& nodeId, const String& messageID, const String& sender, const String& recipient, String content, const String& source, const String& relayID, int rssi = 0, float snr = 0.0);
void sendHeartbeat();
void cleanupLoRaNodes();
void transmitViaWiFi(const String& message);
void transmitWithDutyCycle(const String& message);

// Unified Retransmission Tracking Structure
struct TransmissionStatus {
  bool transmittedViaWiFi = false;
  bool transmittedViaLoRa = false;
  bool addedToMessages = false;  // Flag to track if the message has been added to messages vector
};

// Map to track retransmissions
std::map<String, TransmissionStatus> messageTransmissions;

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
        Serial.printf("[Duty Cycle] Duty cycle limit reached, waiting...\n");
    } else {
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

// Updated Message structure (added recipient)
struct Message {
  String nodeId;     
  String sender;
  String recipient;  // NEW field for private messages (or "ALL" for public)
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

// Returns a custom formatted node id, e.g., "!Mxxxxxx"
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

// -----------------------------------------------------------------------------
// Updated constructMessage() with recipient field.
// Format: messageID|originatorID|sender|recipient|content|relayID|CRC
// -----------------------------------------------------------------------------
String constructMessage(const String& messageID, const String& originatorID, const String& sender, const String& recipient, const String& content, const String& relayID) {
  String messageWithoutCRC = messageID + "|" + originatorID + "|" + sender + "|" + recipient + "|" + content + "|" + relayID;
  uint16_t crc = crc16_ccitt((const uint8_t *)messageWithoutCRC.c_str(), messageWithoutCRC.length());
  char crcStr[5];
  sprintf(crcStr, "%04X", crc);
  String fullMessage = messageWithoutCRC + "|" + String(crcStr);
  return fullMessage;
}

// *** METRICS HISTORY CHANGES ***
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
};

std::map<String, LoRaNode> loraNodes;
// *** END METRICS HISTORY CHANGES ***

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

// -----------------------------------------------------------------------------
// Updated addMessage() to include recipient and only add private messages if intended.
// -----------------------------------------------------------------------------
void addMessage(const String& nodeId, const String& messageID, const String& sender, const String& recipient, String content, const String& source, const String& relayID, int rssi, float snr) {
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
  dutyCycleActive = false; // Simplified for WiFi-only
  return true;
}

void drawMainScreen(long txTimeMillis = -1) {
  // Since there is no display on the ESP32 version, output via Serial.
  if (txTimeMillis >= 0) {
    Serial.print("TxOK (");
    Serial.print(txTimeMillis);
    Serial.println(" ms)");
  }
}

void displayCarousel() {
  // No display code on ESP32 – no operation.
}

void transmitWithDutyCycle(const String& message) {
  if (isDutyCycleAllowed()) {
    uint64_t tx_time = millis();
    Serial.printf("[WiFi Tx] Transmitting: %s\n", message.c_str());

    transmitViaWiFi(message);

    tx_time = millis() - tx_time;
    Serial.printf("[WiFi Tx] Sent OK (%i ms)\n", (int)tx_time);

    calculateDutyCyclePause(tx_time);
    drawMainScreen(tx_time);
  } else {
    Serial.println("[WiFi Tx] Duty cycle limit reached, waiting...");
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
    uint64_t tx_time = millis();
    Serial.printf("[Heartbeat Tx] Sending: %s\n", heartbeatMessage.c_str());

    transmitViaWiFi(heartbeatMessage);

    tx_time = millis() - tx_time;
    Serial.printf("[Heartbeat Tx] OK (%i ms)\n", (int)tx_time);

    calculateDutyCyclePause(tx_time);
    drawMainScreen(tx_time);
  } else {
    Serial.println("[Heartbeat Tx] Duty cycle limit reached, skipping.");
  }
}

void setup() {
  Serial.begin(115200);

  // Initialize WiFi Access Point
  WiFi.softAP(MESH_SSID, MESH_PASSWORD);
  WiFi.setTxPower(WIFI_POWER_19_5dBm);
  WiFi.setSleep(false);
  
  // Initialize Mesh Network
  initMesh();
  setupServerRoutes();
  server.begin();
  dnsServer.start(53, "*", WiFi.softAPIP());

  // Initialize Watchdog Timer
  esp_task_wdt_init(30, true);
  esp_task_wdt_add(NULL);

  esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);
  esp_sleep_pd_config(ESP_PD_DOMAIN_RTC_PERIPH, ESP_PD_OPTION_ON);

  randomSeed(analogRead(0));
}

void loop() {
  esp_task_wdt_reset();
  
  mesh.update();

  // Send heartbeat: first after 20 sec then every 15 minutes.
  if (millis() - lastHeartbeatTime >= firstHeartbeatDelay && lastHeartbeatTime == 0) {
    sendHeartbeat();
    lastHeartbeatTime = millis();
  } else if (millis() - lastHeartbeatTime >= heartbeatInterval && lastHeartbeatTime > 0) {
    sendHeartbeat();
    lastHeartbeatTime = millis();
  }

  updateMeshData();
  displayCarousel();
  dnsServer.processNextRequest();

  if (millis() - lastCleanupTime >= cleanupInterval) {
    cleanupLoRaNodes();
    lastCleanupTime = millis();
  }
}

// -----------------------------------------------------------------------------
// Mesh and WiFi receive functions
// -----------------------------------------------------------------------------
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
    Serial.println("[WiFi Rx] Invalid format.");
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

    if (messageWithoutCRC.startsWith("HEARTBEAT|")) {
      String senderNodeId = messageWithoutCRC.substring(strlen("HEARTBEAT|"));
      Serial.printf("[WiFi Rx] Heartbeat from %s\n", senderNodeId.c_str());
      // Optionally handle heartbeat.
    } else {
      // Expecting the new format: messageID|originatorID|sender|recipient|content|relayID
      int sep1 = messageWithoutCRC.indexOf('|');
      int sep2 = messageWithoutCRC.indexOf('|', sep1 + 1);
      int sep3 = messageWithoutCRC.indexOf('|', sep2 + 1);
      int sep4 = messageWithoutCRC.indexOf('|', sep3 + 1);
      int sep5 = messageWithoutCRC.indexOf('|', sep4 + 1);

      if (sep1 == -1 || sep2 == -1 || sep3 == -1 || sep4 == -1 || sep5 == -1) {
        Serial.println("[WiFi Rx] Invalid message format.");
        return;
      }

      String messageID = messageWithoutCRC.substring(0, sep1);
      String originatorID = messageWithoutCRC.substring(sep1 + 1, sep2);
      String senderID = messageWithoutCRC.substring(sep2 + 1, sep3);
      String recipientID = messageWithoutCRC.substring(sep3 + 1, sep4);
      String messageContent = messageWithoutCRC.substring(sep4 + 1, sep5);
      String relayID = messageWithoutCRC.substring(sep5 + 1);

      String myId = getCustomNodeId(getNodeId());
      // Only add the message if it is public or if this node is a participant.
      if (originatorID == myId) {
        Serial.println("[WiFi Rx] Own message, adding locally...");
        addMessage(originatorID, messageID, senderID, recipientID, messageContent, "[WiFi]", relayID, 0, 0.0);
      } else if (recipientID == "ALL" || myId == recipientID) {
        addMessage(originatorID, messageID, senderID, recipientID, messageContent, "[WiFi]", relayID, 0, 0.0);
      } else {
        Serial.println("[WiFi Rx] Private message not for me, ignoring display.");
      }
    }
  }
}

// -----------------------------------------------------------------------------
// Web Server Routes and HTML
// -----------------------------------------------------------------------------
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
            // For private messages, only show if recipient is "ALL" or matches current node.
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
              <div class="message-content">${senderHtml}${privateIndicator}${msg.content}</div>
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
      font-size: 0.85em;
      color: #333;
      background-color: #fff;
    }
    .node.wifi {
      background-color: #e7f0ff;
      border-color: blue;
    }
    .node.lora {
      background-color: #fff4e0;
      border-color: orange;
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
    .node-section {
      margin-bottom: 30px;
    }
  </style>
  <script>
    function fetchNodes() {
      fetch('/nodesData')
        .then(response => response.json())
        .then(data => {
          const wifiUl = document.getElementById('wifiNodeList');
          const loraUl = document.getElementById('loraNodeList');

          wifiUl.innerHTML = '';
          loraUl.innerHTML = '';

          const wifiCount = data.wifiNodes.length;
          document.getElementById('wifiCount').innerText = 'WiFi Nodes Connected: ' + wifiCount;
          data.wifiNodes.forEach((node, index) => {
            const li = document.createElement('li');
            li.classList.add('node', 'wifi');
            li.textContent = 'Node ' + (index + 1) + ': ' + node;
            wifiUl.appendChild(li);
          });

          const loraCount = data.loraNodes.length;
          document.getElementById('loraCount').innerText = 'LoRa Nodes Active: ' + loraCount;
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
  <h2>Meshmingle Nodes</h2>
  <div class="node-section">
    <span id="wifiCount">WiFi Nodes Connected: 0</span>
    <ul id="wifiNodeList"></ul>
  </div>
  <h2>Lora Coming Soon</h2>
  <div class="node-section">
    <span id="loraCount">LoRa Nodes Active: 0</span>
    <ul id="loraNodeList"></ul>
  </div>
  <a href="/">Back</a><br>
  <a href="/metrics">Node History</a>
</body>
</html>
)rawliteral";

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
    a {
      display: block;
      text-align: center;
      margin-top: 20px;
      font-weight: bold;
      color: #007bff;
      text-decoration: none;
    }
    a:hover {
      text-decoration: underline;
    }
  </style>
  <script>
    window.onload = function() {
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
          data.loraNodes.forEach(node => {
            const nodeBlock = document.createElement('div');
            nodeBlock.classList.add('node-block');
            const nodeTitle = document.createElement('div');
            nodeTitle.classList.add('node-title');
            nodeTitle.textContent = `Node ID: ${node.nodeId}`;
            nodeBlock.appendChild(nodeTitle);
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
            nodeBlock.appendChild(nodeTable);
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
              tsCell.textContent = sample.timestamp;
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
  <h2>LoRa 24hr History</h2>
  <center><h3>coming soon</h3></center>
  <div class="node-id" id="nodeIdDisplay">Node ID: Loading...</div>
  <div id="historyContainer"></div>
  <a href="/nodes">Back to Nodes</a>
  <a href="/">Back to Chat</a>
</body>
</html>
)rawliteral";

/**
 * Helper function to convert an elapsed time in milliseconds into a human-readable relative time string.
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
                  + ",\"lastSeen\":\"" + formatRelativeTime(currentTime - lastSeenTime) + "\"}";
            firstLora = false;
        }
    }
    json += "]}";
    request->send(200, "application/json", json);
  });

  // Updated /update route to accept optional "target" parameter for private messages.
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
    addMessage(getCustomNodeId(getNodeId()), messageID, senderName, target, newMessage, "[WiFi]", relayID, 0, 0.0);
    Serial.printf("[WiFi Tx] Adding message: %s\n", constructedMessage.c_str());
    transmitWithDutyCycle(constructedMessage);
    request->redirect("/");
  });

  // Metrics History Routes
  server.on("/metrics", HTTP_GET, [](AsyncWebServerRequest* request) {
    request->send_P(200, "text/html", metricsPageHtml);
  });

  server.on("/metricsHistoryData", HTTP_GET, [](AsyncWebServerRequest* request) {
      uint64_t now = millis();
      const uint64_t ONE_DAY = 86400000;
      String json = "{\"loraNodes\":[";
      bool firstNode = true;
      for (auto const& kv : loraNodes) {
        if (!firstNode) json += ",";
        firstNode = false;
        const auto &node = kv.second;
        int bestRssi = node.history[0].rssi;
        float bestSnr = node.history[0].snr;
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
}

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
