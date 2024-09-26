#define COMPILE_HELTEC // Use this line if compiling for Heltec LoRa 32 V3

// Feature Toggles
#ifdef COMPILE_HELTEC
    #define ENABLE_LORA // Enable LoRa functionality
    #define ENABLE_DISPLAY // Enable OLED display functionality
#endif

// Includes and Definitions
#ifdef ENABLE_DISPLAY
    #define HELTEC_POWER_BUTTON // Use the power button feature of Heltec
    #include <heltec_unofficial.h> // Heltec library for OLED and LoRa
#endif

#include <painlessMesh.h>
#include <WiFi.h>
#include <ESPAsyncWebServer.h>
#include <DNSServer.h>
#include <Wire.h>
#include <esp_task_wdt.h> // Watchdog timer library
#include <vector> // For handling list of message IDs
#include <map> // For tracking retransmissions

// LoRa Parameters
#ifdef ENABLE_LORA
    #include <RadioLib.h>
    #define PAUSE 10000  // 10% duty cycle (10 seconds max transmission in 100 seconds)
    #define FREQUENCY 869.525
    #define BANDWIDTH 250.0
    #define SPREADING_FACTOR 11
    #define TRANSMIT_POWER 22
    #define CODING_RATE 5 // Coding rate 4/5 
    String rxdata;
    volatile bool rxFlag = false;
    long counter = 0;
    uint64_t tx_time;
    uint64_t last_tx = 0;
    uint64_t minimum_pause = 0;
    unsigned long lastTransmitTime = 0; // Timing variable for managing sequential transmissions
    String fullMessage; // Global variable to hold the message for sequential transmission

    // Function to handle LoRa received packets
    void rx() {
      rxFlag = true;
    }

// Define the maximum allowed duty cycle (10%)
#define DUTY_CYCLE_LIMIT_PERCENT 10
#define DUTY_CYCLE_WINDOW 100000  // 100 seconds in milliseconds

// Function to calculate the required pause based on the duty cycle
void calculateDutyCyclePause(uint64_t tx_time) {
  // tx_time is the transmission time in milliseconds
  // Calculate the minimum pause time to ensure compliance with the 10% duty cycle
  minimum_pause = (tx_time * (10 / DUTY_CYCLE_LIMIT_PERCENT)) - tx_time;
}

    void setupLora() {
      heltec_setup(); // Initialize Heltec board, display, and other components if display is enabled
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
#endif

// Meshify Parameters
#define MESH_SSID "Meshify 1.0"
#define MESH_PASSWORD ""
#define MESH_PORT 5555
const int maxMessages = 10;

// Duty Cycle Variables
bool bypassDutyCycle = false; // Set to true to bypass duty cycle check
bool dutyCycleActive = false; // Tracks if duty cycle limit is reached
bool lastDutyCycleActive = false; // Tracks the last known duty cycle state

// Mesh and Web Server Setup
AsyncWebServer server(80);
DNSServer dnsServer;
painlessMesh mesh;

// Message structure for Meshify
struct Message {
  String nodeId; // Node ID of the message sender
  String sender;
  String content;
  String source; // Indicates message source (WiFi or LoRa)
};

// Rolling list for messages
std::vector<Message> messages; // Dynamic vector to store messages

// Track retransmissions
std::map<String, bool> loraRetransmitted; // Tracks if a message has been retransmitted via LoRa
std::map<String, bool> wifiRetransmitted; // Tracks if a message has been retransmitted via WiFi

// Centralized mesh data
int totalNodeCount = 0;
uint32_t currentNodeId = 0;

// Global variable to manage LoRa delay after WiFi transmission
unsigned long loRaTransmitDelay = 0;  // This stores the time after which LoRa can transmit

// Function to generate a unique message ID (now with timestamp)
String generateMessageID(const String& nodeId, const String& sender, const String& content) {
    return nodeId + ":" + sender + ":" + content + ":" + String(millis());  // Appending timestamp to ensure uniqueness
}

// Function to add a message with a unique ID and size limit
void addMessage(const String& nodeId, const String& sender, String content, const String& source) {
    const int maxMessageLength = 100;

    // Truncate the message if it exceeds the maximum allowed length
    if (content.length() > maxMessageLength) {
        Serial.println("Message is too long, truncating...");
        content = content.substring(0, maxMessageLength);
    }

    // Generate the message ID using the new function
    String messageID = generateMessageID(nodeId, sender, content);

    // Check if the message already exists, if so, do not add
    if (loraRetransmitted[messageID] || wifiRetransmitted[messageID]) {
        Serial.println("Message already exists, skipping...");
        return; // Message already exists
    }

    // Create the new message
    Message newMessage = {nodeId, sender, content, source};

    // Insert the new message at the beginning of the list
    messages.insert(messages.begin(), newMessage);

    // Ensure the list doesn't exceed maxMessages
    if (messages.size() > maxMessages) {
        messages.pop_back(); // Remove the oldest message
    }

    // Mark the message as not retransmitted yet
    loraRetransmitted[messageID] = false;
    wifiRetransmitted[messageID] = false;

    // Log the message
    Serial.printf("Message added: NodeID: %s, Sender: %s, Content: %s, Source: %s\n",
                  nodeId.c_str(), sender.c_str(), content.c_str(), source.c_str());
}

// Function to check if there are active mesh nodes
bool areMeshNodesAvailable() {
    return mesh.getNodeList().size() > 0;
}

void transmitViaWiFi(const String& message) {
    String fullMessageID = message;
    Serial.printf("[WiFi Tx] Preparing to transmit: %s\n", message.c_str());

    // Check if there are active mesh nodes
    if (!areMeshNodesAvailable()) {
        Serial.println("[WiFi Tx] No mesh nodes available, skipping WiFi transmission.");
        // If no mesh nodes, set LoRa to transmit immediately
        fullMessage = message;
        loRaTransmitDelay = millis(); // No delay for LoRa if no mesh nodes available
        return;
    }

    if (wifiRetransmitted[fullMessageID]) {
        Serial.println("[WiFi Tx] Skipping retransmission via WiFi.");
        return; // Message already retransmitted via WiFi, skip it
    }

    mesh.sendBroadcast(message);
    wifiRetransmitted[fullMessageID] = true; // Mark as retransmitted via WiFi
    Serial.printf("[WiFi Tx] Message transmitted via WiFi: %s\n", message.c_str());

    // Set the LoRa transmission delay after WiFi transmission
    loRaTransmitDelay = millis() + random(3000, 5001);  // Set delay between 3000ms and 5001ms
    lastTransmitTime = millis(); // Record the time of WiFi transmission
}


// Function to check and enforce duty cycle (for LoRa only)
bool isDutyCycleAllowed() {
  if (bypassDutyCycle) {
    dutyCycleActive = false;
    return true;
  }

  if (millis() > last_tx + minimum_pause) {
    dutyCycleActive = false; // Duty cycle is over, we can transmit
  } else {
    dutyCycleActive = true; // Duty cycle is still active
  }

  return !dutyCycleActive;
}

// Function to update the display with current status (if OLED is enabled)
// Declare a global variable to store the last valid transmission time
long lastTxTimeMillis = -1; // Initialized to -1, meaning no transmission yet

#ifdef ENABLE_DISPLAY
void updateDisplay(long txTimeMillis = -1) {
  display.clear();
  display.setFont(ArialMT_Plain_10); // Set to a slightly larger but still readable font
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
    display.drawString((128 - txMessageWidth) / 2, 54, txMessage); // Bottom middle
  }

  display.display();
}
#endif


// Function to handle LoRa transmissions and apply the delay
void transmitWithDutyCycle(const String& message) {
    // Check if the LoRa delay has passed
    if (millis() < loRaTransmitDelay) {
        Serial.println("[LoRa Tx] LoRa delay not expired, waiting...");
        return;  // Exit the function and wait for the delay to expire
    }

    String fullMessageID = message;
    if (loraRetransmitted[fullMessageID]) {
        Serial.println("[LoRa Tx] Skipping retransmission via LoRa.");
        return; // Message already retransmitted via LoRa, skip it
    }

    if (isDutyCycleAllowed()) {
        tx_time = millis();

        // Transmit the message WITHOUT node ID for retransmission
        int status = radio.transmit(message.c_str());  // Only the message is sent without node ID
        tx_time = millis() - tx_time;

        if (status == RADIOLIB_ERR_NONE) {
            Serial.printf("[LoRa Tx] Message transmitted successfully via LoRa (%i ms)\n", (int)tx_time);
            loraRetransmitted[fullMessageID] = true; // Mark as retransmitted via LoRa

            // Calculate the required pause to respect the 10% duty cycle
            calculateDutyCyclePause(tx_time);
            last_tx = millis(); // Record the time of the last transmission

            #ifdef ENABLE_DISPLAY
            updateDisplay(tx_time);  // Update display with transmission time
            #endif
        } else {
            Serial.printf("[LoRa Tx] Transmission via LoRa failed (%i)\n", status);
        }
    } else {
        Serial.printf("[LoRa Tx] Duty cycle limit reached, please wait %i sec.\n", 
                      (int)((minimum_pause - (millis() - last_tx)) / 1000) + 1);
    }
}

void setup() {
    Serial.begin(115200);

    #ifdef ENABLE_DISPLAY
    heltec_setup();
    #endif

    #ifdef ENABLE_LORA
    setupLora();
    #endif

    WiFi.mode(WIFI_AP);
    WiFi.setTxPower(WIFI_POWER_19_5dBm);
    WiFi.setSleep(false);
    initMesh();
    setupServerRoutes();
    server.begin();
    dnsServer.start(53, "*", WiFi.softAPIP());

    #ifdef ENABLE_DISPLAY
    display.init();
    display.flipScreenVertically();
    display.clear();
    display.setFont(ArialMT_Plain_10);
    updateDisplay();
    #endif

    esp_task_wdt_init(10, true);
    esp_task_wdt_add(NULL);

    // Initialize random seed
    randomSeed(analogRead(0)); // If using ESP32, you can use analogRead on an unconnected pin
}

void loop() {
    esp_task_wdt_reset();

    #ifdef ENABLE_DISPLAY
    heltec_loop();
    #endif

    // Check the duty cycle and update the display if necessary
    isDutyCycleAllowed();

    // Ensure we only transmit when we have a valid message
    if (!fullMessage.isEmpty() && millis() >= loRaTransmitDelay) {
        transmitWithDutyCycle(fullMessage);  // Use message without Node ID for LoRa transmission
        fullMessage = "";  // Clear the fullMessage after transmission to avoid retransmission
    }

    updateMeshData();

    #ifdef ENABLE_DISPLAY
    updateDisplay(); // Refresh the display with current mesh data
    #endif

    dnsServer.processNextRequest();
}

// Meshify Initialization Function
void initMesh() {
    mesh.setDebugMsgTypes(ERROR | STARTUP | CONNECTION);
    mesh.init(MESH_SSID, MESH_PASSWORD, MESH_PORT);
    mesh.onReceive(receivedCallback);

    mesh.onChangedConnections([]() {
        updateMeshData();
        #ifdef ENABLE_DISPLAY
        updateDisplay();
        #endif
    });

    mesh.setContainsRoot(false);
}

void receivedCallback(uint32_t from, String &message) {
    Serial.printf("Received message from %u: %s\n", from, message.c_str());

    int firstColonIndex = message.indexOf(':');
    if (firstColonIndex > 0) {
        String sender = message.substring(0, firstColonIndex);
        String messageContent = message.substring(firstColonIndex + 1);

        String fullMessageID = sender + ":" + messageContent;

        // Avoid processing and retransmitting messages from your own node
        if (sender == String(getNodeId())) {
            Serial.println("Received own WiFi message, ignoring...");
            return; // Skip the message if it's from yourself
        }

        // Check if message ID already exists
        if (wifiRetransmitted[fullMessageID]) {
            Serial.println("Message already retransmitted, ignoring...");
            return; // Don't retransmit if we've already processed this message
        }

        // Add the WiFi message to the list
        addMessage(String(from), sender, messageContent, "[WiFi]");
        wifiRetransmitted[fullMessageID] = true; // Mark as retransmitted

        Serial.printf("WiFi message added: Node ID: %s, Sender: %s, Message: %s\n", 
                      String(getNodeId()).c_str(), sender.c_str(), messageContent.c_str());

        // Retransmit the message via WiFi first, then LoRa with delay
        fullMessage = sender + ":" + messageContent;  // Only set `fullMessage` when needed

        // Set the LoRa transmission delay after WiFi retransmission
        loRaTransmitDelay = millis() + random(3000, 5001);  // Random delay for LoRa retransmission
    } else {
        Serial.println("Invalid message format.");
    }
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

// Function to fetch messages and update the list
function fetchData() {
  fetch('/messages')
    .then(response => {
      if (!response.ok) throw new Error('Failed to fetch messages');
      return response.json();
    })
    .then(data => {
      const ul = document.getElementById('messageList');
      ul.innerHTML = ''; // Clear the current list
      data.messages.forEach(msg => {
        const li = document.createElement('li');
        const tagClass = msg.source === '[LoRa]' ? 'lora' : 'wifi';
        li.innerHTML = `${msg.nodeId}: ${msg.sender}: ${msg.message}`;
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
  server.on("/", HTTP_GET, [](AsyncWebServerRequest *request) {
    serveHtml(request, mainPageHtml);
  });

  server.on("/nodes", HTTP_GET, [](AsyncWebServerRequest *request) {
    serveHtml(request, nodesPageHtml);
  });

  server.on("/messages", HTTP_GET, [](AsyncWebServerRequest *request) {
    String json = "[";
    bool first = true;
    for (const auto& msg : messages) {
      if (!first) json += ",";
      // Add nodeId to the JSON object
      json += "{\"nodeId\":\"" + msg.nodeId + "\",\"sender\":\"" + msg.sender + "\",\"message\":\"" + msg.content + "\",\"source\":\"" + msg.source + "\"}";
      first = false;
    }
    json += "]";
    request->send(200, "application/json", "{\"messages\":" + json + "}");
  });

  server.on("/deviceCount", HTTP_GET, [](AsyncWebServerRequest *request) {
    updateMeshData();
    request->send(200, "application/json", "{\"totalCount\":" + String(getNodeCount()) + ", \"nodeId\":\"" + String(getNodeId()) + "\"}");
  });

  server.on("/nodesData", HTTP_GET, [](AsyncWebServerRequest *request) {
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

  server.on("/update", HTTP_POST, [](AsyncWebServerRequest *request) {
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

    fullMessage = senderName + ":" + newMessage;

    addMessage(String(getNodeId()), senderName, newMessage, "[WiFi]");
    transmitViaWiFi(fullMessage);
    lastTransmitTime = millis(); // Start delay before sending via LoRa
    request->redirect("/");
  });
}

// HTML Serving Function
void serveHtml(AsyncWebServerRequest *request, const char* htmlContent) {
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
