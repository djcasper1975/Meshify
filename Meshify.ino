#include <painlessMesh.h>
#include <WiFi.h>
#include <ESPAsyncWebServer.h>
#include <DNSServer.h>
#include <Wire.h>
#include <esp_task_wdt.h> // Watchdog timer library
#include <vector> // For handling list of message IDs
#include <map> // For tracking retransmissions

// Meshify Parameters
#define MESH_SSID "Meshify 1.0"
#define MESH_PASSWORD ""
#define MESH_PORT 5555
const int maxMessages = 10;

// Duty Cycle Variables
bool bypassDutyCycle = true; // Set to true to bypass duty cycle check
bool dutyCycleActive = false; // Tracks if duty cycle limit is reached

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
std::map<String, bool> wifiRetransmitted; // Tracks if a message has been retransmitted via WiFi

// Centralized mesh data
int totalNodeCount = 0;
uint32_t currentNodeId = 0;

// Function to generate a unique message ID
String generateMessageID() {
  return String(millis()); // Use current time as a unique message ID
}

// Function to add a message to the list and ensure new messages are at the top
void addMessage(const String& nodeId, const String& sender, const String& content, const String& source) {
  // Create the new message
  Message newMessage = {nodeId, sender, content, source};

  // Check if the message already exists, if so, do not add
  for (const auto& msg : messages) {
    if (msg.nodeId == nodeId && msg.sender == sender && msg.content == content) {
      return; // Message already exists
    }
  }

  // Insert new message at the beginning of the list
  messages.insert(messages.begin(), newMessage);

  // Ensure the list doesn't exceed maxMessages
  if (messages.size() > maxMessages) {
    messages.pop_back(); // Remove the oldest message
  }

  // Mark the message as not retransmitted yet
  String fullMessageID = nodeId + ":" + sender + ":" + content;
  wifiRetransmitted[fullMessageID] = false;
}

// Function to send message via WiFi (Meshify)
void transmitViaWiFi(const String& message) {
  String fullMessageID = message;
  if (wifiRetransmitted[fullMessageID]) {
    Serial.println("Skipping retransmission via WiFi.");
    return; // Message already retransmitted via WiFi, skip it
  }

  mesh.sendBroadcast(message);
  wifiRetransmitted[fullMessageID] = true; // Mark as retransmitted via WiFi
  Serial.printf("Message transmitted via WiFi: %s\n", message.c_str());
}

// Main HTML Page Content
const char mainPageHtml[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
<meta name="viewport" content="width=device-width, initial-scale=1.0">
<style>
  body { font-family: Arial, sans-serif; margin: 0; padding: 0; text-align: center; }
  h2 { color: #333; }
  form { margin: 20px auto; max-width: 500px; }
  input[type=text], input[type=submit] { width: calc(100% - 22px); padding: 10px; margin: 10px 0; box-sizing: border-box; }
  input[type=submit] { background-color: #007BFF; color: white; border: none; border-radius: 5px; cursor: pointer; }
  input[type=submit]:hover { background-color: #0056b3; }
  ul { list-style-type: none; padding: 0; margin: 20px auto; max-width: 500px; }
  li { background-color: #f9f9f9; margin: 5px 0; padding: 10px; border-radius: 5px; word-wrap: break-word; overflow-wrap: break-word; white-space: pre-wrap; }
  .warning { color: red; margin-bottom: 20px; }
  .wifi { color: blue; }
</style>
<script>
function fetchData() {
  fetch('/messages')
    .then(response => response.json())
    .then(data => {
      const ul = document.getElementById('messageList');
      ul.innerHTML = '';
      data.messages.forEach(msg => {
        const li = document.createElement('li');
        const tagClass = msg.source === '[WiFi]' ? 'wifi' : 'lora';
        li.innerHTML = `<span class="${tagClass}">${msg.source}</span> ${msg.nodeId}: ${msg.sender}: ${msg.message}`;
        ul.appendChild(li);
      });
    })
    .catch(error => {
      console.error('Error fetching messages:', error);
      setTimeout(() => location.reload(), 5000);
    });

  fetch('/deviceCount')
    .then(response => response.json())
    .then(data => {
      document.getElementById('deviceCount').textContent =
        'Mesh Nodes: ' + data.totalCount + ', Node ID: ' + data.nodeId;
    })
    .catch(error => {
      console.error('Error fetching device count:', error);
      setTimeout(() => location.reload(), 5000);
    });
}

window.onload = function() {
  loadName();
  fetchData();
  setInterval(fetchData, 5000); // Fetch data every 5 seconds
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
<div id='deviceCount'>Mesh Nodes: 0, Node ID: 0</div>
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
  h2 { color: #333; }
  ul { list-style-type: none; padding: 0; margin: 20px auto; max-width: 500px; }
  li { background-color: #f9f9f9; margin: 5px 0; padding: 10px; border-radius: 5px; word-wrap: break-word; overflow-wrap: break-word; white-space: pre-wrap; }
</style>
<script>
function fetchNodes() {
  fetch('/nodesData')
    .then(response => response.json())
    .then(data => {
      const ul = document.getElementById('nodeList');
      ul.innerHTML = data.nodes.map((node, index) => `<li>Node ${index + 1}: ${node}</li>`).join('');
      document.getElementById('nodeCount').textContent = 'Mesh Nodes Connected: ' + data.nodes.length;
    })
    .catch(error => {
      console.error('Error fetching nodes:', error);
    });
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

// Setup Function
void setup() {
  Serial.begin(115200);

  WiFi.mode(WIFI_AP);
  WiFi.setTxPower(WIFI_POWER_19_5dBm);
  WiFi.setSleep(false);
  initMesh();
  setupServerRoutes();
  server.begin();
  dnsServer.start(53, "*", WiFi.softAPIP());

  esp_task_wdt_init(10, true);
  esp_task_wdt_add(NULL);
}

// Main Loop Function
void loop() {
  esp_task_wdt_reset();

  updateMeshData();

  dnsServer.processNextRequest();
}

// Meshify Initialization Function
void initMesh() {
  mesh.setDebugMsgTypes(ERROR | STARTUP | CONNECTION);
  mesh.init(MESH_SSID, MESH_PASSWORD, MESH_PORT);
  mesh.onReceive(receivedCallback);

  mesh.onChangedConnections([]() {
    updateMeshData();
  });

  mesh.setContainsRoot(false);
}

// Meshify Received Callback
void receivedCallback(uint32_t from, String &message) {
  Serial.printf("Received message from %u: %s\n", from, message.c_str());

  int firstColonIndex = message.indexOf(':');
  if (firstColonIndex > 0) {
    String sender = message.substring(0, firstColonIndex);
    String messageContent = message.substring(firstColonIndex + 1);

    // Check if message was sent by this node or if it exists in the list
    for (const auto& msg : messages) {
      if (msg.sender == sender && msg.content == messageContent) {
        Serial.println("Ignoring message already in list.");
        return;
      }
    }

    addMessage(String(from), sender, messageContent, "[WiFi]");
    // Retransmit the message via WiFi
    String fullMessage = sender + ":" + messageContent;
    transmitViaWiFi(fullMessage);
    Serial.printf("Mesh Message [%s] -> Sent via WiFi\n", message.c_str());
  }
}

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

    String fullMessage = senderName + ":" + newMessage;

    addMessage(String(getNodeId()), senderName, newMessage, "[WiFi]");
    transmitViaWiFi(fullMessage);
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
