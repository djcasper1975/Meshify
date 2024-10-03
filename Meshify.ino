//Make sure you install the same version on all devices.
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
      left: 0;
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
    .message-nodeid {
      font-size: 0.7em; /* Make the Node ID smaller */
      color: #666;
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
    // Object to store timestamps for each message, keyed by messageID
    const messageTimestamps = {}; 

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

      fetch('/update', {
        method: 'POST',
        body: formData
      })
      .then(response => {
        if (!response.ok) throw new Error('Failed to send message');
        messageInput.value = '';
        fetchData();  // Fetch new messages
      })
      .catch(error => console.error('Error sending message:', error));
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
            if (msg.nodeId === currentNodeId) {
              li.classList.add('sent');
            } else {
              li.classList.add('received');
              li.classList.add('wifi');
            }

            // Check if the message already has a timestamp; if not, add it
            if (!messageTimestamps[msg.messageID]) {
              messageTimestamps[msg.messageID] = Date.now();  // Set the timestamp when the message is first received
            }

            // Format the timestamp for display
            const timestamp = formatTimestamp(messageTimestamps[msg.messageID]);

            // Optionally display the node ID if it's a received message
            const nodeIdHtml = (msg.nodeId !== currentNodeId) ? 
              `<span class="message-nodeid">Node: ${msg.nodeId}</span>` : '';

            // Insert the message and timestamp into the UI
            li.innerHTML = `
              ${nodeIdHtml}
              <div class="message-content">${msg.sender}: ${msg.message}</div>
              <span class="message-time">${timestamp}</span>
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
  <input type="submit" value="Send">
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
      padding: 0; 
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
      margin: 20px auto; 
      max-width: 500px; 
    }
    .node {
      display: flex;
      justify-content: center;
      align-items: center;
      margin: 10px 0;
      padding: 15px;
      border-radius: 10px;
      width: 100%;
      max-width: 500px;
      box-sizing: border-box;
      border: 2px solid;
      text-align: center;
      font-weight: bold;
      font-size: 1.1em;
    }
    .node.wifi {
      background-color: #e7f0ff; /* Light blue background for Wi-Fi nodes */
      border-color: blue; /* Blue border for Wi-Fi nodes */
    }
    .node.lora {
      background-color: #fff4e0; /* Light orange background for LoRa nodes */
      border-color: orange; /* Orange border for LoRa nodes */
    }
    #nodeCount { 
      margin: 20px auto; 
      max-width: 500px; 
      font-weight: bold;
    }
    a {
      margin-top: 20px;
      display: inline-block;
      text-decoration: none;
      color: #007bff;
      font-weight: bold;
    }
  </style>
  <script>
    function fetchNodes() {
      fetch('/nodesData')
        .then(response => response.json())
        .then(data => {
          const ul = document.getElementById('nodeList');
          ul.innerHTML = '';
          
          data.nodes.forEach((node, index) => {
            const li = document.createElement('li');
            const nodeType = node.startsWith('LoRa') ? 'lora' : 'wifi'; // Determine node type
            
            li.classList.add('node', nodeType);
            li.textContent = 'Node ' + (index + 1) + ': ' + node;
            
            ul.appendChild(li);
          });
          
          document.getElementById('nodeCount').textContent = 'Mesh Nodes Connected: ' + data.nodes.length;
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
  <h2>Wifi Nodes Connected</h2>
  <div id="nodeCount">Mesh Nodes Connected: 0</div>
  <ul id="nodeList"></ul>
  <a href="/">Back to Main Page</a>
</body>
</html>
)rawliteral";
 // Ensure this is properly closed

// Setup Function
void setup() {
  Serial.begin(115200);

  WiFi.softAP(MESH_SSID, MESH_PASSWORD);
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
