#include <painlessMesh.h>
#include <WiFi.h>
#include <ESPAsyncWebServer.h>
#include <DNSServer.h>
#include <Wire.h>
#include <esp_task_wdt.h>
#include <vector>
#include <map>

// Meshify Parameters
#define MESH_SSID "Meshify 1.0"
#define MESH_PASSWORD ""
#define MESH_PORT 5555
const int maxMessages = 10;

// Mesh and Web Server Setup
AsyncWebServer server(80);
DNSServer dnsServer;
painlessMesh mesh;

// Message structure for Meshify
struct Message {
  String nodeId;    // Node ID of the message sender
  String sender;
  String content;
  String source;    // Indicates message source (WiFi or LoRa)
  String messageID; // Unique message ID
};

// Rolling list for messages
std::vector<Message> messages;

// Track retransmissions
struct TransmissionStatus {
  bool transmittedViaWiFi = false;
  bool transmittedViaLoRa = false;
  bool addedToMessages = false;
};
std::map<String, TransmissionStatus> messageTransmissions;

// Centralized mesh data
int totalNodeCount = 0;
uint32_t currentNodeId = 0;

// Global message counter for generating unique message IDs (in-memory)
unsigned long messageCounter = 0;

// Function to generate a unique message ID
String generateMessageID() {
  messageCounter++; // Increment the counter
  return String(getNodeId()) + ":" + String(messageCounter); // Format: nodeId:counter
}

// Function to construct a message with the message ID included
String constructMessage(const String& messageID, const String& sender, const String& content) {
  return messageID + "|" + sender + "|" + content; // New format
}

// Function to add a message with a unique ID and size limit
void addMessage(const String& nodeId, const String& messageID, const String& sender, String content, const String& source) {
  const int maxMessageLength = 100;

  // Truncate the message if it exceeds the maximum allowed length
  if (content.length() > maxMessageLength) {
    Serial.println("Message is too long, truncating...");
    content = content.substring(0, maxMessageLength);
  }

  // Retrieve the transmission status for this messageID
  auto& status = messageTransmissions[messageID];
  
  // Check if the message has already been added to the messages vector
  if (status.addedToMessages) {
    Serial.println("Message already exists in view, skipping addition...");
    return; // Message has already been added, skip
  }

  // If the message is from our own node, do not include the source tag
  String finalSource = "";
  if (nodeId != String(getNodeId())) {
    finalSource = source; // Only show source if it's from another node
  }

  // Create the new message
  Message newMessage = {nodeId, sender, content, finalSource, messageID};

  // Insert the new message at the beginning of the list
  messages.insert(messages.begin(), newMessage);

  // Mark the message as added to prevent future duplicates
  status.addedToMessages = true;

  // Ensure the list doesn't exceed maxMessages
  if (messages.size() > maxMessages) {
    messages.pop_back(); // Remove the oldest message
  }

  // Log the message
  Serial.printf("Message added: NodeID: %s, Sender: %s, Content: %s, Source: %s, MessageID: %s\n",
                nodeId.c_str(), sender.c_str(), content.c_str(), finalSource.c_str(), messageID.c_str());
}

// Function to send message via WiFi (Meshify)
void transmitViaWiFi(const String& message) {
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
  status.transmittedViaWiFi = true; // Mark as retransmitted via WiFi
  Serial.printf("[WiFi Tx] Message transmitted via WiFi: %s\n", message.c_str());
}

// Function to handle incoming messages
void receivedCallback(uint32_t from, String &message) {
  Serial.printf("Received message from %u: %s\n", from, message.c_str());

  // Parse the message based on the new format: messageID|sender|content
  int firstSeparator = message.indexOf('|');
  int secondSeparator = message.indexOf('|', firstSeparator + 1);

  if (firstSeparator == -1 || secondSeparator == -1) {
    Serial.println("[WiFi Rx] Invalid message format.");
    return;
  }

  String messageID = message.substring(0, firstSeparator);
  String sender = message.substring(firstSeparator + 1, secondSeparator);
  String messageContent = message.substring(secondSeparator + 1);

  // Validate messageID format
  int colonIndex = messageID.indexOf(':');
  if (colonIndex == -1) {
    Serial.println("[WiFi Rx] Invalid messageID format.");
    return;
  }
  String nodeId = messageID.substring(0, colonIndex);
  String counterStr = messageID.substring(colonIndex + 1);
  bool validCounter = true;
  for (unsigned int i = 0; i < counterStr.length(); i++) {
    if (!isDigit(counterStr[i])) {
      validCounter = false;
      break;
    }
  }
  if (!validCounter || nodeId.length() == 0) {
    Serial.println("[WiFi Rx] Invalid messageID content.");
    return;
  }

  // Avoid processing and retransmitting messages from your own node
  if (sender == String(getNodeId())) {
    Serial.println("[WiFi Rx] Received own message, ignoring...");
    return; // Skip the message if it's from yourself
  }

  // Add the message to the message list
  addMessage(nodeId, messageID, sender, messageContent, "[WiFi]");

  // Retransmit the message via WiFi if not already done
  transmitViaWiFi(message);
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
// Function to send a message without refreshing the page
function sendMessage(event) {
  event.preventDefault(); // Prevent form submission from reloading the page

  const nameInput = document.getElementById('nameInput');
  const messageInput = document.querySelector('input[name="msg"]');
  const sender = nameInput.value;
  const message = messageInput.value;

  // Send the message using fetch() and prevent full page reload
  fetch('/update', {
    method: 'POST',
    headers: {
      'Content-Type': 'application/x-www-form-urlencoded',
    },
    body: `sender=${encodeURIComponent(sender)}&msg=${encodeURIComponent(message)}`
  })
  .then(response => {
    if (response.ok) {
      // Clear the message input after successful submission
      messageInput.value = '';
      // Fetch the updated messages without reloading the page
      fetchData();
    }
  })
  .catch(error => {
    console.error('Error sending message:', error);
  });
}

function fetchData() {
  let currentNodeId = null;

  // Fetch current node information first
  fetch('/deviceCount')
    .then(response => response.json())
    .then(data => {
      currentNodeId = data.nodeId;
      document.getElementById('deviceCount').textContent =
        'Mesh Nodes: ' + data.totalCount + ', Node ID: ' + currentNodeId;
      
      // Fetch messages once we have the current node ID
      return fetch('/messages');
    })
    .then(response => response.json())
    .then(data => {
      const ul = document.getElementById('messageList');
      ul.innerHTML = '';
      data.messages.forEach(msg => {
        const li = document.createElement('li');
        const tagClass = msg.source === '[WiFi]' ? 'wifi' : 'lora';

        // Only include the node ID if it differs from the current node
        if (msg.nodeId !== currentNodeId) {
          li.innerHTML = `<span class="${tagClass}">${msg.source}</span> Node ${msg.nodeId}: ${msg.sender}: ${msg.message}`;
        } else {
          li.innerHTML = `<span class="${tagClass}">${msg.source}</span> ${msg.sender}: ${msg.message}`;
        }

        ul.appendChild(li);
      });
    })
    .catch(error => {
      console.error('Error fetching messages or node data:', error);
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
<!-- Attach the sendMessage function to the form onsubmit event -->
<form onsubmit="sendMessage(event)">
  <input type="text" id="nameInput" name="sender" placeholder="Enter your name" required maxlength="20" />
  <input type="text" name="msg" placeholder="Enter your message" required maxlength="180" />
  <input type="submit" value="Send" />
</form>
<div id='deviceCount'>Mesh Nodes: 0, Node ID: 0</div>
<a href="/nodes">View Mesh Nodes List</a><br>
<ul id='messageList'></ul>
<p>github.com/djcasper1975</p>
</body>
</html>
)rawliteral"; // Ensure this is properly closed

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
  // Fetch the node data and update the nodes list every 5 seconds
  fetch('/nodesData')
    .then(response => response.json())
    .then(data => {
      const ul = document.getElementById('nodeList');
      ul.innerHTML = data.nodes.map((node, index) => `<li>Node ${index + 1}: ${node}</li>`).join('');
      document.getElementById('nodeCount').textContent = 'Mesh Nodes Connected: ' + data.nodes.length;
    })
    .catch(error => {
      console.error('Error fetching nodes:', error);
      setTimeout(() => location.reload(), 5000); // Fallback in case of error
    });
}

// Run on window load
window.onload = function() {
  fetchNodes();
  setInterval(fetchNodes, 5000); // Fetch nodes data every 5 seconds
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
)rawliteral"; // Ensure this is properly closed

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
  mesh.onReceive(receivedCallback); // Ensure only one definition

  mesh.onChangedConnections([]() {
    updateMeshData();
  });

  mesh.setContainsRoot(false);
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
      // Include all necessary fields in the JSON object
      json += "{\"nodeId\":\"" + msg.nodeId + "\",\"sender\":\"" + msg.sender + "\",\"message\":\"" + msg.content + "\",\"source\":\"" + msg.source + "\",\"messageID\":\"" + msg.messageID + "\"}";
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

    // Sanitize inputs to prevent HTML injection
    newMessage.replace("<", "&lt;");
    newMessage.replace(">", "&gt;");
    senderName.replace("<", "&lt;");
    senderName.replace(">", "&gt;");

    // Generate a new message ID without passing parameters
    String messageID = generateMessageID();

    // Construct the full message with the message ID
    String constructedMessage = constructMessage(messageID, senderName, newMessage);

    // Add the message with source "[WiFi]" since WiFi is being used
    addMessage(String(getNodeId()), messageID, senderName, newMessage, "[WiFi]");
    transmitViaWiFi(constructedMessage); // Transmit via WiFi

    request->send(200); // Respond to indicate message was processed
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
