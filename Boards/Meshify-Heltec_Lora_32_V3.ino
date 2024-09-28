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
};

// Map to track retransmissions
std::map<String, TransmissionStatus> messageTransmissions;

// LoRa Parameters
#include <RadioLib.h>
#define PAUSE 10000  // 10% duty cycle (10 seconds max transmission in 100 seconds)
#define FREQUENCY 869.525
#define BANDWIDTH 250.0
#define SPREADING_FACTOR 11
#define TRANSMIT_POWER 22
#define CODING_RATE 5  // Coding rate 4/5
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
#define DUTY_CYCLE_LIMIT_PERCENT 10
#define DUTY_CYCLE_WINDOW 100000  // 100 seconds in milliseconds

// Function to calculate the required pause based on the duty cycle
void calculateDutyCyclePause(uint64_t tx_time) {
  // Corrected the duty cycle calculation to ensure proper pause time
  // For a 10% duty cycle over 100 seconds:
  // Max transmit time = 10 seconds, hence pause = 90 seconds
  // Formula: minimum_pause = DUTY_CYCLE_WINDOW * (DUTY_CYCLE_LIMIT_PERCENT / 100.0) - tx_time
  minimum_pause = DUTY_CYCLE_WINDOW * (DUTY_CYCLE_LIMIT_PERCENT / 100.0) - tx_time;
  if (minimum_pause < 0) minimum_pause = 0; // Ensure non-negative pause
}

void setupLora() {
  heltec_setup();  // Initialize Heltec board, display, and other components if display is enabled
  Serial.println("Initializing LoRa radio...");

  // Initialize the LoRa radio with specified parameters
  RADIOLIB_OR_HALT(radio.begin());
  radio.setDio1Action(rx);

  RADIOLIB_OR_HALT(radio.setFrequency(FREQUENCY));
  RADIOLIB_OR_HALT(radio.setBandwidth(BANDWIDTH));
  RADIOLIB_OR_HALT(radio.setSpreadingFactor(SPREADING_FACTOR));
  RADIOLIB_OR_HALT(radio.setCodingRate(CODING_RATE));
  RADIOLIB_OR_HALT(radio.setOutputPower(TRANSMIT_POWER));

  // Start receiving
  RADIOLIB_OR_HALT(radio.startReceive(RADIOLIB_SX126X_RX_TIMEOUT_INF));
}

// Meshify Parameters
#define MESH_SSID "Meshify 1.0"
#define MESH_PASSWORD ""
#define MESH_PORT 5555
const int maxMessages = 10;

// Duty Cycle Variables
bool bypassDutyCycle = false;     // Set to true to bypass duty cycle check
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
  messageCounter++;  // Increment the counter
  return nodeId + ":" + String(messageCounter);
}

// Function to construct a message with the message ID included
String constructMessage(const String& messageID, const String& sender, const String& content) {
  // Format: messageID|sender|content
  return messageID + "|" + sender + "|" + content;
}

// Function to add a message with a unique ID and size limit
void addMessage(const String& nodeId, const String& messageID, const String& sender, String content, const String& source) {
  const int maxMessageLength = 100;

  // Truncate the message if it exceeds the maximum allowed length
  if (content.length() > maxMessageLength) {
    Serial.println("Message is too long, truncating...");
    content = content.substring(0, maxMessageLength);
  }

  // Check if the message already exists, if so, do not add
  auto& status = messageTransmissions[messageID];
  if (status.transmittedViaWiFi || status.transmittedViaLoRa) {
    Serial.println("Message already exists, skipping...");
    return;  // Message already exists
  }

  // If the message is from our own node, do not include the source tag
  String finalSource = "";
  if (nodeId != String(getNodeId())) {
    finalSource = source;  // Only show source if it's from another node
  }

  // Create the new message
  Message newMessage = {nodeId, sender, content, finalSource, messageID};

  // Insert the new message at the beginning of the list
  messages.insert(messages.begin(), newMessage);

  // Ensure the list doesn't exceed maxMessages
  if (messages.size() > maxMessages) {
    messages.pop_back();  // Remove the oldest message
  }

  // Log the message
  Serial.printf("Message added: NodeID: %s, Sender: %s, Content: %s, Source: %s, MessageID: %s\n",
                nodeId.c_str(), sender.c_str(), content.c_str(), finalSource.c_str(), messageID.c_str());
}

// **New Function: Schedule LoRa Transmission**
// This function schedules LoRa transmission by setting the message and the delay
void scheduleLoRaTransmission(const String& message) {
  fullMessage = message;
  loRaTransmitDelay = millis() + random(3000, 5001); // Set delay between 3000ms and 5000ms
  Serial.printf("[LoRa Schedule] Message scheduled for LoRa transmission after %lu ms: %s\n", 
                loRaTransmitDelay - millis(), message.c_str());
}

// **Modified Function: Transmit via WiFi**
// Now, this function only handles WiFi transmission without scheduling LoRa
void transmitViaWiFi(const String& message) {
  Serial.printf("[WiFi Tx] Preparing to transmit: %s\n", message.c_str());

  // Extract the message ID from the message
  int separatorIndex = message.indexOf('|');
  if (separatorIndex == -1) {
    Serial.println("[WiFi Tx] Invalid message format.");
    return;
  }
  String messageID = message.substring(0, separatorIndex);

  auto& status = messageTransmissions[messageID];
  // Only retransmit if it hasn't been retransmitted via WiFi already
  if (status.transmittedViaWiFi) {
    Serial.println("[WiFi Tx] Skipping retransmission via WiFi.");
    return;
  }

  mesh.sendBroadcast(message);
  status.transmittedViaWiFi = true;  // Mark as retransmitted via WiFi
  Serial.printf("[WiFi Tx] Message transmitted via WiFi: %s\n", message.c_str());
}

// Function to check and enforce duty cycle (for LoRa only)
bool isDutyCycleAllowed() {
  if (bypassDutyCycle) {
    dutyCycleActive = false;
    return true;
  }

  if (millis() > last_tx + minimum_pause) {
    dutyCycleActive = false;  // Duty cycle is over, we can transmit
  } else {
    dutyCycleActive = true;  // Duty cycle is still active
  }

  return !dutyCycleActive;
}

// Declare a global variable to store the last valid transmission time
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

// **Modified Function: Transmit with Duty Cycle**
// Now, after a successful LoRa transmission, it will trigger WiFi retransmission
void transmitWithDutyCycle(const String& message) {
  // Check if the LoRa delay has passed
  if (millis() < loRaTransmitDelay) {
    Serial.println("[LoRa Tx] LoRa delay not expired, waiting...");
    return;  // Exit the function and wait for the delay to expire
  }

  // Extract the message ID from the message
  int separatorIndex = message.indexOf('|');
  if (separatorIndex == -1) {
    Serial.println("[LoRa Tx] Invalid message format.");
    return;
  }
  String messageID = message.substring(0, separatorIndex);

  auto& status = messageTransmissions[messageID];
  if (status.transmittedViaLoRa) {
    Serial.println("[LoRa Tx] Skipping retransmission via LoRa.");
    return;  // Message already retransmitted via LoRa, skip it
  }

  if (isDutyCycleAllowed()) {
    tx_time = millis();

    Serial.printf("[LoRa Tx] Preparing to transmit: %s\n", message.c_str()); // Added log

    heltec_led(50);  // Turn LED on (this will light up during LoRa transmission)
    // Transmit the message
    int transmitStatus = radio.transmit(message.c_str());
    tx_time = millis() - tx_time;
    heltec_led(0);  // Turn off LED after transmission is done

    if (transmitStatus == RADIOLIB_ERR_NONE) {
      Serial.printf("[LoRa Tx] Message transmitted successfully via LoRa (%i ms)\n", (int)tx_time);
      Serial.printf("[LoRa Tx] Message transmitted via LoRa: %s\n", message.c_str());
      status.transmittedViaLoRa = true;  // Mark as retransmitted via LoRa

      // Calculate the required pause to respect the 10% duty cycle
      calculateDutyCyclePause(tx_time);
      last_tx = millis();  // Record the time of the last transmission

      updateDisplay(tx_time);  // Update display with transmission time

      // **Trigger WiFi retransmission after successful LoRa transmission**
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
  heltec_led(0);  // Ensure the LED is off by default at the start

  setupLora();

  WiFi.mode(WIFI_AP);
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

  esp_task_wdt_init(10, true);
  esp_task_wdt_add(NULL);

  // Initialize random seed
  randomSeed(analogRead(0));  // If using ESP32, you can use analogRead on an unconnected pin
}

void loop() {
  esp_task_wdt_reset();

  heltec_loop();

  // No need to call isDutyCycleAllowed() here separately

  // Process LoRa reception
  if (rxFlag) {
    rxFlag = false;  // Reset flag
    String message;
    int state = radio.readData(message);
    if (state == RADIOLIB_ERR_NONE) {
      Serial.printf("[LoRa Rx] Received message: %s\n", message.c_str());
      // Process the received message

      // Extract the message ID, sender, and content from the message
      int firstSeparator = message.indexOf('|');
      int secondSeparator = message.indexOf('|', firstSeparator + 1);

      if (firstSeparator == -1 || secondSeparator == -1) {
        Serial.println("[LoRa Rx] Invalid message format.");
      } else {
        String messageID = message.substring(0, firstSeparator);
        String sender = message.substring(firstSeparator + 1, secondSeparator);
        String messageContent = message.substring(secondSeparator + 1);

        // **Extract nodeId from messageID**
        int colonIndex = messageID.indexOf(':');
        String nodeId = messageID.substring(0, colonIndex);

        // Avoid processing and retransmitting messages from your own node
        if (sender == String(getNodeId())) {
          Serial.println("[LoRa Rx] Received own message, ignoring...");
        } else {
          auto& status = messageTransmissions[messageID];
          // Check if the message has already been retransmitted over WiFi or LoRa
          if (status.transmittedViaWiFi && status.transmittedViaLoRa) {
            Serial.println("[LoRa Rx] Message already retransmitted via both WiFi and LoRa, ignoring...");
          } else {
            // Add the message to the message list (tracking both WiFi and LoRa)
            Serial.printf("[LoRa Rx] Adding message: %s\n", message.c_str()); // Added log
            addMessage(nodeId, messageID, sender, messageContent, "[LoRa]");  // **Use the extracted nodeId**

            // **Schedule LoRa transmission first**
            if (!status.transmittedViaLoRa) {
              scheduleLoRaTransmission(message);  // Schedule LoRa retransmission
            }

            // **WiFi transmission will be handled after LoRa transmission**
            // No immediate action needed here
          }
        }
      }
    } else {
      Serial.printf("[LoRa Rx] Receive failed, code %d\n", state);
    }
    // Restart receiving
    radio.startReceive();
  }

  // **Handle Scheduled LoRa Transmission**
  if (!fullMessage.isEmpty() && millis() >= loRaTransmitDelay) {
    transmitWithDutyCycle(fullMessage);  // Use message with message ID for LoRa transmission
    fullMessage = "";                    // Clear the fullMessage after transmission to avoid retransmission
  }

  updateMeshData();

  updateDisplay();  // Refresh the display with current mesh data

  dnsServer.processNextRequest();
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

  // Extract the message ID, sender, and content from the message
  int firstSeparator = message.indexOf('|');
  int secondSeparator = message.indexOf('|', firstSeparator + 1);

  if (firstSeparator == -1 || secondSeparator == -1) {
    Serial.println("[WiFi Rx] Invalid message format.");
    return;
  }

  String messageID = message.substring(0, firstSeparator);
  String sender = message.substring(firstSeparator + 1, secondSeparator);
  String messageContent = message.substring(secondSeparator + 1);

  // **Extract nodeId from messageID**
  int colonIndex = messageID.indexOf(':');
  String nodeId = messageID.substring(0, colonIndex);

  // Avoid processing and retransmitting messages from your own node
  if (sender == String(getNodeId())) {
    Serial.println("[WiFi Rx] Received own message, ignoring...");
    return;  // Skip the message if it's from yourself
  }

  auto& status = messageTransmissions[messageID];
  // Check if the message has already been retransmitted over WiFi or LoRa
  if (status.transmittedViaWiFi && status.transmittedViaLoRa) {
    Serial.println("[WiFi Rx] Message already retransmitted via both WiFi and LoRa, ignoring...");
    return;  // Don't retransmit if it's already been retransmitted on both networks
  }

  // Add the message to the message list (tracking both WiFi and LoRa)
  Serial.printf("[WiFi Rx] Adding message: %s\n", message.c_str()); // Added log
  addMessage(nodeId, messageID, sender, messageContent, "[WiFi]");  // **Use the extracted nodeId**

  // **Schedule LoRa transmission first**
  if (!status.transmittedViaLoRa) {
    scheduleLoRaTransmission(message);  // Schedule LoRa retransmission
  }

  // **WiFi transmission will be handled after LoRa transmission**
  // No immediate action needed here
}

// Main HTML Page Content
const char mainPageHtml[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
<meta name="viewport" content="width=device-width, initial-scale=1.0">
<style>
  body { font-family: Arial, sans-serif; margin: 0; padding: 0; text-align: center; }
  h1, h2 { color: #333; }
  form { margin: 20px auto; max-width: 500px; }
  input[type=text], input[type=submit] { width: calc(100% - 22px); padding: 10px; margin: 10px 0; box-sizing: border-box; }
  input[type=submit] { background-color: #007BFF; color: white; border: none; border-radius: 5px; cursor: pointer; }
  input[type=submit]:hover { background-color: #0056b3; }
  ul { list-style-type: none; padding: 0; margin: 20px auto; max-width: 500px; }
  li { background-color: #f9f9f9; margin: 5px 0; padding: 10px; border-radius: 5px; word-wrap: break-word; overflow-wrap: break-word; white-space: pre-wrap; }
  #deviceCount { margin: 20px auto; max-width: 500px; }
  .warning { color: red; margin-bottom: 20px; }
  .wifi { color: blue; }
  .lora { color: orange; }
</style>
<script>
// Function to send a message without refreshing the page
function sendMessage(event) {
  event.preventDefault(); // Prevent form submission from reloading the page

  const nameInput = document.getElementById('nameInput');
  const messageInput = document.getElementById('messageInput');
  
  const sender = nameInput.value;
  const msg = messageInput.value;

  // Ensure both fields are filled
  if (!sender || !msg) {
    alert('Please enter both a name and a message.');
    return;
  }

  // Save the name locally so it's preserved
  localStorage.setItem('username', sender);

  // Create the form data
  const formData = new URLSearchParams();
  formData.append('sender', sender);
  formData.append('msg', msg);

  // Send the form data using fetch (AJAX)
  fetch('/update', {
    method: 'POST',
    body: formData
  }).then(response => {
    if (!response.ok) {
      throw new Error('Failed to send message');
    }
    // Clear the message input after successful submission
    messageInput.value = '';
    // Update the message list by calling fetchData
    fetchData();
  }).catch(error => {
    console.error('Error sending message:', error);
  });
}

function fetchData() {
  fetch('/messages')
    .then(response => {
      if (!response.ok) throw new Error('Failed to fetch messages');
      return response.json();
    })
    .then(data => {
      const ul = document.getElementById('messageList');
      ul.innerHTML = ''; // Clear the current list
      const currentNodeId = localStorage.getItem('nodeId');  // Get current node ID

      data.messages.forEach(msg => {
        const li = document.createElement('li');
        
        // Check if the message came from our own node
        if (msg.nodeId === currentNodeId) {
          li.innerHTML = `${msg.sender}: ${msg.message}`;  // Only show the message and sender
        } else {
          // Show node ID and source tag if it's from another node
          const tagClass = msg.source === '[LoRa]' ? 'lora' : 'wifi';
          li.innerHTML = `<span class="${tagClass}">${msg.source}</span> ${msg.nodeId}: ${msg.sender}: ${msg.message}`;
        }
        ul.appendChild(li);
      });
    })
    .catch(error => {
      console.error('Error fetching messages:', error);
    });

  fetch('/deviceCount')
    .then(response => {
      if (!response.ok) throw new Error('Failed to fetch device count');
      return response.json();
    })
    .then(data => {
      localStorage.setItem('nodeId', data.nodeId);
      document.getElementById('deviceCount').textContent =
        'Mesh Nodes: ' + data.totalCount + ', Node ID: ' + data.nodeId;
    })
    .catch(error => {
      console.error('Error fetching device count:', error);
    });
}

// On window load, set up the event listeners and start fetching data
window.onload = function() {
  // Load the saved name from local storage, if available
  const savedName = localStorage.getItem('username');
  if (savedName) {
    document.getElementById('nameInput').value = savedName;
  }

  // Fetch messages every 5 seconds
  fetchData();
  setInterval(fetchData, 5000); // Fetch data every 5 seconds

  // Attach the sendMessage function to the form's submit event
  document.getElementById('messageForm').addEventListener('submit', sendMessage);
};
</script>
</head>
<body>
<h2>Meshify 1.0</h2>
<div class='warning'>For your safety, do not share your location or any personal information!</div>
<form id="messageForm">
  <input type="text" id="nameInput" name="sender" placeholder="Enter your name" required maxlength="15" />
  <input type="text" id="messageInput" name="msg" placeholder="Enter your message" required maxlength="100" />
  <input type="submit" value="Send" />
</form>
<div id='deviceCount'>Mesh Nodes: 0</div>
<a href="/nodes">View Mesh Nodes List</a><br>
<ul id='messageList'></ul>
<p>github.com/djcasper1975</p>
</body>
</html>
)rawliteral";

// Nodes List HTML Page Content
const char nodesPageHtml[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
<meta name="viewport" content="width=device-width, initial-scale=1.0">
<style>
  body { font-family: Arial, sans-serif; margin: 0; padding: 0; text-align: center; }
  h1, h2 { color: #333; }
  ul { list-style-type: none; padding: 0; margin: 20px auto; max-width: 500px; }
  li { background-color: #f9f9f9; margin: 5px 0; padding: 10px; border-radius: 5px; word-wrap: break-word; overflow-wrap: break-word; white-space: pre-wrap; }
  #nodeCount { margin: 20px auto; max-width: 500px; }
</style>
<script>
function fetchNodes() {
  fetch('/nodesData').then(response => response.json()).then(data => {
    const ul = document.getElementById('nodeList');
    ul.innerHTML = data.nodes.map((node, index) => `<li>Node ${index + 1}: ${node}</li>`).join('');
    document.getElementById('nodeCount').textContent = 'Mesh Nodes Connected: ' + data.nodes.length;
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
<h2>Mesh Nodes Connected</h2>
<div id='nodeCount'>Mesh Nodes Connected: 0</div>
<ul id='nodeList'></ul>
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
      json += "{\"nodeId\":\"" + msg.nodeId + "\",\"sender\":\"" + msg.sender + "\",\"message\":\"" + msg.content + "\",\"source\":\"" + msg.source + "\",\"messageID\":\"" + msg.messageID + "\"}";
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
    String json = "[";
    auto nodeList = mesh.getNodeList();
    bool first = true;
    for (auto node : nodeList) {
      if (!first) json += ",";
      json += "\"" + String(node) + "\"";
      first = false;
    }
    json += "]";
    request->send(200, "application/json", "{\"nodes\":" + json + "}");
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

    // Construct the full message with the message ID
    String constructedMessage = constructMessage(messageID, senderName, newMessage);

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
