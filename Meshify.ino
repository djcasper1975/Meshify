// Define flags to enable or disable features for use with (Heltec Lora 32 V3 ONLY)
//#define ENABLE_LORA // Comment this line to disable LoRa functionality
//#define ENABLE_DISPLAY // Comment this line to disable OLED display functionality

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

// LoRa Parameters
#ifdef ENABLE_LORA
#define PAUSE 300
#define FREQUENCY 866.3
#define BANDWIDTH 250.0
#define SPREADING_FACTOR 9
#define TRANSMIT_POWER 0
#endif

// Meshify Parameters
#define MESH_SSID "Meshify 1.0"
#define MESH_PASSWORD ""
#define MESH_PORT 5555
const int maxMessages = 10;

// Duty Cycle Variables
bool bypassDutyCycle = false; // Set to true to bypass duty cycle check
uint64_t last_tx = 0;
uint64_t minimum_pause = 0;
bool dutyCycleActive = false; // Tracks if duty cycle limit is reached

// Mesh and Web Server Setup
AsyncWebServer server(80);
DNSServer dnsServer;
painlessMesh mesh;

// Message structure for Meshify
struct Message {
  String id; // Unique ID for the message
  String sender;
  String content;
  String source; // Indicates message source (WiFi or LoRa)
};
Message messages[maxMessages];
int messageIndex = 0;

// List to store recent message IDs to prevent re-transmitting the same message
std::vector<String> forwardedMessageIDs;
const int maxStoredIDs = 50; // Maximum number of IDs to store for preventing loops

// Shared Variables
#ifdef ENABLE_LORA
String rxdata;
volatile bool rxFlag = false;
long counter = 0;
uint64_t tx_time;
#endif

// Centralized mesh data
int totalNodeCount = 0;
uint32_t currentNodeId = 0;

// Function to generate a unique message ID
String generateMessageID() {
  return String(getNodeId()) + "-" + String(millis()); // Combines Node ID and current time
}

// Function to check if a message ID has been forwarded
bool hasForwardedMessage(const String& id) {
  return std::find(forwardedMessageIDs.begin(), forwardedMessageIDs.end(), id) != forwardedMessageIDs.end();
}

// Function to remember a forwarded message ID
void rememberMessageID(const String& id) {
  forwardedMessageIDs.push_back(id);
  if (forwardedMessageIDs.size() > maxStoredIDs) {
    forwardedMessageIDs.erase(forwardedMessageIDs.begin()); // Keep the list within size limits
  }
}

// Function to handle incoming messages and update the message array
void handleIncomingMessage(const String& id, const String& sender, const String& content, const String& source) {
  // Create a new message structure
  Message newMessage = {id, sender, content, source};

  // Save the message in the circular buffer
  messages[messageIndex] = newMessage;
  messageIndex = (messageIndex + 1) % maxMessages; // Update the index cyclically
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
  .wifi { color: blue; } /* WiFi message tag color */
  .lora { color: orange; } /* LoRa message tag color */
</style>
<script>
function fetchData() {
  fetch('/messages')
    .then(response => response.json())
    .then(data => {
      const ul = document.getElementById('messageList');
      ul.innerHTML = ''; // Clear the list before updating
      data.messages.forEach(msg => {
        const li = document.createElement('li');
        // Set color based on source
        const tagClass = msg.source === '[WiFi]' ? 'wifi' : 'lora';
        li.innerHTML = `<span class="${tagClass}">${msg.source}</span> ${msg.sender}: ${msg.message}`;
        ul.prepend(li); // Add each message at the start of the list
      });
    });

  fetch('/deviceCount')
    .then(response => response.json())
    .then(data => {
      document.getElementById('deviceCount').textContent =
        'Mesh Nodes: ' + data.totalCount + ', Node ID: ' + data.nodeId;
    });
}

window.onload = function() {
  loadName();
  fetchData();
  setInterval(fetchData, 5000); // Fetch data every 5 seconds to ensure synchronized updates
};

function saveName() {
  const nameInput = document.getElementById('nameInput');
  localStorage.setItem('username', nameInput.value);
}

function loadName() {
  const savedName = localStorage.getItem('username');
  if (savedName) {
    document.getElementById('nameInput').value = savedName;
  }
}
</script>
</head>
<body>
<h2>Meshify 1.0</h2>
<div class='warning'>For your safety, do not share your location or any personal information!</div>
<form action="/update" method="POST" onsubmit="saveName()">
  <input type="text" id="nameInput" name="sender" placeholder="Enter your name" required maxlength="25" />
  <input type="text" name="msg" placeholder="Enter your message" required maxlength="256" />
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
  });
}
window.onload = function() {
  fetchNodes();
  setInterval(fetchNodes, 5000); // Unified refresh rate every 5 seconds for consistency
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

#ifdef ENABLE_DISPLAY
// Update the display with Meshify information and duty cycle status
void updateDisplay() {
  display.clear();
  display.setFont(ArialMT_Plain_10); // Set to a slightly larger but still readable font

  int16_t titleWidth = display.getStringWidth("Meshify 1.0");
  display.drawString((128 - titleWidth) / 2, 0, "Meshify 1.0");
  display.drawString(0, 16, "Node ID: " + String(getNodeId())); // Position adjusted for clarity
  display.drawString(0, 32, "Mesh Nodes: " + String(getNodeCount())); // Moved up slightly for better layout

  // Show duty cycle status with adjusted position
  if (dutyCycleActive) {
    display.drawString(0, 46, "Duty Cycle Limit Reached!"); // Positioned for better visibility
  } else {
    display.drawString(0, 46, "LoRa Tx Allowed"); // Updated text and adjusted position
  }
  display.display();
}
#endif

// Function to check and enforce duty cycle
bool isDutyCycleAllowed() {
  if (bypassDutyCycle) {
    return true; // Bypass duty cycle if override is set
  }
  if (millis() > last_tx + minimum_pause) {
    dutyCycleActive = false; // Reset duty cycle status when allowed
    return true;
  } else {
    dutyCycleActive = true; // Set duty cycle status when limit is reached
    return false;
  }
}

#ifdef ENABLE_LORA
// Function to handle transmissions and update duty cycle
void transmitWithDutyCycle(const String& message) {
  if (isDutyCycleAllowed()) {
    // Transmit message and record transmission time
    tx_time = millis();
    RADIOLIB(radio.transmit(message.c_str()));
    tx_time = millis() - tx_time;

    if (_radiolib_status == RADIOLIB_ERR_NONE) {
      both.printf("Message transmitted successfully (%i ms)\n", (int)tx_time);
    } else {
      both.printf("Transmission failed (%i)\n", _radiolib_status);
    }

    // Update duty cycle constraints
    minimum_pause = tx_time * 100;
    last_tx = millis();
  } else {
    // Provide feedback on duty cycle status
    both.printf("Duty cycle limit reached, please wait %i sec.\n", (int)((minimum_pause - (millis() - last_tx)) / 1000) + 1);
  }
  #ifdef ENABLE_DISPLAY
  updateDisplay(); // Update display to reflect current duty cycle status
  #endif
}
#endif

// Setup Function
void setup() {
  // Initialize Serial, Heltec board, and LoRa radio using your original setup
  Serial.begin(115200);

  #ifdef ENABLE_DISPLAY
  heltec_setup();
  #endif

  #ifdef ENABLE_LORA
  // LoRa Radio Setup remains unchanged
  RADIOLIB_OR_HALT(radio.begin());
  radio.setDio1Action(rx);
  RADIOLIB_OR_HALT(radio.setFrequency(FREQUENCY));
  RADIOLIB_OR_HALT(radio.setBandwidth(BANDWIDTH));
  RADIOLIB_OR_HALT(radio.setSpreadingFactor(SPREADING_FACTOR));
  RADIOLIB_OR_HALT(radio.setOutputPower(TRANSMIT_POWER));
  RADIOLIB_OR_HALT(radio.startReceive(RADIOLIB_SX126X_RX_TIMEOUT_INF));
  #endif

  // Meshify Initialization
  WiFi.mode(WIFI_AP);
  WiFi.setTxPower(WIFI_POWER_19_5dBm);
  WiFi.setSleep(false);
  initMesh();               // Initialize the mesh network
  setupServerRoutes();      // Set up web server routes
  server.begin();           // Start the web server
  dnsServer.start(53, "*", WiFi.softAPIP());  // Start DNS server for captive portal

  #ifdef ENABLE_DISPLAY
  // Initialize Display using your original setup
  heltec_display_power(true); // Turn on display power if needed
  display.init();
  display.flipScreenVertically(); // Rotate the display to correct orientation
  display.clear();
  display.setFont(ArialMT_Plain_10); // Set the font as per your original settings

  // Display Meshify title centered at the top
  updateDisplay();
  #endif

  // Watchdog Timer Setup remains as per your original code
  esp_task_wdt_init(10, true);
  esp_task_wdt_add(NULL);
}

// Main Loop Function
void loop() {
  esp_task_wdt_reset(); // Reset watchdog timer to prevent resets

  #ifdef ENABLE_DISPLAY
  // Maintain the original Heltec loop functionality
  heltec_loop();
  #endif

  #ifdef ENABLE_LORA
  // Check if a LoRa message has been received
  if (rxFlag) {
    rxFlag = false;
    radio.readData(rxdata);
    if (_radiolib_status == RADIOLIB_ERR_NONE) {
      // Extract message ID from LoRa data (expected format: "ID:sender:content")
      int firstColonIndex = rxdata.indexOf(':');
      int secondColonIndex = rxdata.indexOf(':', firstColonIndex + 1);
      if (firstColonIndex > 0 && secondColonIndex > 0) {
        String messageID = rxdata.substring(0, firstColonIndex);
        String sender = rxdata.substring(firstColonIndex + 1, secondColonIndex);
        String messageContent = rxdata.substring(secondColonIndex + 1);

        // Check if the message ID has already been forwarded
        if (!hasForwardedMessage(messageID)) {
          // Forward the message over the mesh network
          String forwardMessage = "LORA:" + messageID + ":" + sender + ":" + messageContent;
          mesh.sendBroadcast(forwardMessage);
          both.printf("RX [%s] -> Forwarded to Mesh\n", rxdata.c_str());

          // Save the message and mark it as forwarded
          rememberMessageID(messageID);
          handleIncomingMessage(messageID, sender, messageContent, "[LoRa]");
        }
      }
    }
    RADIOLIB_OR_HALT(radio.startReceive(RADIOLIB_SX126X_RX_TIMEOUT_INF));
  }
  #endif

  // Regularly update Mesh data
  updateMeshData();

  // Update the display regularly to reflect current mesh state
  #ifdef ENABLE_DISPLAY
  updateDisplay();
  #endif

  // Meshify Loop and handle web requests
  dnsServer.processNextRequest();
}

#ifdef ENABLE_LORA
// LoRa Packet Received Callback remains the same
void rx() {
  rxFlag = true;
}
#endif

// Meshify Initialization Function
void initMesh() {
  mesh.setDebugMsgTypes(ERROR | STARTUP | CONNECTION);
  mesh.init(MESH_SSID, MESH_PASSWORD, MESH_PORT);
  mesh.onReceive(receivedCallback);

  // Callback to update display when nodes change
  mesh.onChangedConnections([]() {
    updateMeshData();
    #ifdef ENABLE_DISPLAY
    updateDisplay();
    #endif
  });

  mesh.setContainsRoot(false);
}

// Meshify Received Callback with WiFi Tag
void receivedCallback(uint32_t from, String &message) {
  Serial.printf("Received message from %u: %s\n", from, message.c_str());

  // Extract message ID, sender, and content from received message
  int firstColonIndex = message.indexOf(':');
  int secondColonIndex = message.indexOf(':', firstColonIndex + 1);
  if (firstColonIndex > 0 && secondColonIndex > 0) {
    String messageID = message.substring(0, firstColonIndex);
    String sender = message.substring(firstColonIndex + 1, secondColonIndex);
    String messageContent = message.substring(secondColonIndex + 1);

    // Check if the message has already been forwarded
    if (!hasForwardedMessage(messageID)) {
      // Forward the message over LoRa
      #ifdef ENABLE_LORA
      transmitWithDutyCycle(message);
      both.printf("Mesh Message [%s] -> Sent via LoRa\n", message.c_str());
      #endif

      // Save and handle the message
      rememberMessageID(messageID);
      handleIncomingMessage(messageID, sender, messageContent, "[WiFi]");
    }
  }
}

// Server Routes Setup
void setupServerRoutes() {
  // Main page route
  server.on("/", HTTP_GET, [](AsyncWebServerRequest *request) {
    serveHtml(request, mainPageHtml);
  });

  // Nodes list page route
  server.on("/nodes", HTTP_GET, [](AsyncWebServerRequest *request) {
    serveHtml(request, nodesPageHtml);
  });

  // Fetch messages in JSON format
  server.on("/messages", HTTP_GET, [](AsyncWebServerRequest *request) {
    String json = "[";
    bool first = true;
    for (int i = maxMessages - 1; i >= 0; i--) {
      int index = (messageIndex - 1 - i + maxMessages) % maxMessages;
      if (messages[index].content != "") {
        if (!first) json += ",";
        json += "{\"sender\":\"" + messages[index].sender + "\",\"message\":\"" + messages[index].content + "\",\"source\":\"" + messages[index].source + "\"}";
        first = false;
      }
    }
    json += "]";
    request->send(200, "application/json", "{\"messages\":" + json + "}");
  });

  // Fetch device count information in JSON format
  server.on("/deviceCount", HTTP_GET, [](AsyncWebServerRequest *request) {
    updateMeshData(); // Centralized data update
    request->send(200, "application/json", "{\"totalCount\":" + String(getNodeCount()) + ", \"nodeId\":\"" + String(getNodeId()) + "\"}");
  });

  // Fetch nodes data in JSON format
  server.on("/nodesData", HTTP_GET, [](AsyncWebServerRequest *request) {
    updateMeshData(); // Centralized data update
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

  // Handle message updates from the form
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

    // Generate a unique ID for the new message
    String messageID = generateMessageID();
    String fullMessage = messageID + ":" + senderName + ":" + newMessage;

    // Save and broadcast the message
    handleIncomingMessage(messageID, senderName, newMessage, ""); // Empty source since it's a sent message
    rememberMessageID(messageID); // Mark the message as forwarded
    mesh.sendBroadcast(fullMessage); // Broadcast the message with ID

    request->redirect("/");
  });
}

// HTML Serving Function remains the same
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
  mesh.update(); // Ensure the mesh is updated
  totalNodeCount = mesh.getNodeList().size();
  currentNodeId = mesh.getNodeId();
}
