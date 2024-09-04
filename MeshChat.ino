#include <painlessMesh.h>
#include <WiFi.h>
#include <ESPAsyncWebServer.h>
#include <DNSServer.h>

#define MESH_SSID "MeshChat 1.0"
#define MESH_PASSWORD ""
#define MESH_PORT 5555
#define MAX_MESSAGE_LENGTH 256 // Define maximum length for a message
#define MAX_SENDER_NAME_LENGTH 25 // Define maximum length for the sender's name

const int maxMessages = 5;
struct Message {
  String sender;
  String content;
};
Message messages[maxMessages];
int messageIndex = 0;

AsyncWebServer server(80);
DNSServer dnsServer;

painlessMesh mesh;

const char mainPageHtml[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
<meta name="viewport" content="width=device-width, initial-scale=1.0">
<style>
  body { font-family: Arial, sans-serif; margin: 0; padding: 0; text-align: center; }
  h1, h2 { color: #333; }
  form { margin: 20px auto; max-width: 500px; }
  input[type=text] { width: calc(100% - 22px); padding: 10px; margin: 10px 0; box-sizing: border-box; }
  input[type=submit] { padding: 10px 20px; background-color: #007BFF; color: white; border: none; border-radius: 5px; cursor: pointer; }
  input[type=submit]:hover { background-color: #0056b3; }
  ul { list-style-type: none; padding: 0; margin: 20px auto; max-width: 500px; }
  li { background-color: #f9f9f9; margin: 5px 0; padding: 10px; border-radius: 5px; word-wrap: break-word; overflow-wrap: break-word; white-space: pre-wrap; }
  #deviceCount { margin: 20px auto; max-width: 500px; }
  .warning { color: red; margin-bottom: 20px; }
</style>
<script>
function fetchData() {
  fetch('/messages')
    .then(response => response.json())
    .then(data => {
      const ul = document.getElementById('messageList');
      ul.innerHTML = ''; // Clear the list before updating
      // Prepend each message to the list
      data.messages.forEach(msg => {
        const li = document.createElement('li');
        li.innerText = `${msg.sender}: ${msg.message}`;
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
  setInterval(fetchData, 10000);
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
<h2>MeshChat 1.0</h2>
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
    ul.innerHTML = data.nodes.map((node, index) => `<li>Node${index + 1}: ${node}</li>`).join('');
    document.getElementById('nodeCount').textContent = 'Mesh Nodes Connected: ' + data.nodes.length;
  });
}
window.onload = function() {
  fetchNodes();
  setInterval(fetchNodes, 10000); // Refresh every 10 seconds
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

// Callback function for incoming mesh messages
void receivedCallback(uint32_t from, String &message) {
  // Check if the message length exceeds the maximum allowed length
  if (message.length() > MAX_MESSAGE_LENGTH) {
    // Truncate the message to the maximum allowed length
    message = message.substring(0, MAX_MESSAGE_LENGTH);
    Serial.printf("Message truncated from %u to prevent overflow.\n", from);
  }

  Serial.printf("Received message from %u: %s\n", from, message.c_str());

  // Check if the message starts with "USER:"
  if (message.startsWith("USER:")) {
    String userMessage = message.substring(5);
    if (userMessage.length() > MAX_SENDER_NAME_LENGTH) {
      userMessage = userMessage.substring(0, MAX_SENDER_NAME_LENGTH);
      Serial.printf("Sender name truncated to prevent overflow.\n");
    }
    messages[messageIndex].content = userMessage;
    messages[messageIndex].sender = String(from);
    messageIndex = (messageIndex + 1) % maxMessages;
  }
}

// Mesh initialization and setup
void initMesh() {
  mesh.setDebugMsgTypes(ERROR | STARTUP | CONNECTION);
  mesh.init(MESH_SSID, MESH_PASSWORD, MESH_PORT);
  mesh.onReceive(receivedCallback);

  // Callback for when connections change
  mesh.onChangedConnections([]() {
    Serial.println("Mesh connection changed. Attempting to reconnect if possible.");
    // Custom reconnection handling can be placed here
  });

  // Automatically attempt reconnections
  mesh.setContainsRoot(false);
}

void setup() {
  Serial.begin(115200);

  // Initialize WiFi and Mesh network
  WiFi.mode(WIFI_AP);
  WiFi.setTxPower(WIFI_POWER_19_5dBm);

  initMesh();

  // Set up DNS server to redirect all requests to the captive portal
  dnsServer.start(53, "*", WiFi.softAPIP());

  // Serve the main page
  server.on("/", HTTP_GET, [](AsyncWebServerRequest *request){
    request->send(200, "text/html", mainPageHtml);
  });

  // Serve the nodes page
  server.on("/nodes", HTTP_GET, [](AsyncWebServerRequest *request){
    request->send(200, "text/html", nodesPageHtml);
  });

  // Serve messages as JSON
  server.on("/messages", HTTP_GET, [](AsyncWebServerRequest *request){
    String json = "[";
    bool first = true;
    for (int i = maxMessages - 1; i >= 0; i--) {
      int index = (messageIndex - 1 - i + maxMessages) % maxMessages;
      if (messages[index].content != "") {
        if (!first) json += ",";
        json += "{\"sender\":\"" + messages[index].sender + "\",\"message\":\"" + messages[index].content + "\"}";
        first = false;
      }
    }
    json += "]";
    request->send(200, "application/json", "{\"messages\":" + json + "}");
  });

  // Serve connected device count and node ID as JSON
  server.on("/deviceCount", HTTP_GET, [](AsyncWebServerRequest *request){
    mesh.update();
    int totalCount = mesh.getNodeList().size();
    uint32_t nodeId = mesh.getNodeId(); // Get the current node's ID
    request->send(200, "application/json", "{\"totalCount\":" + String(totalCount) + ", \"nodeId\":\"" + String(nodeId) + "\"}");
  });

  // Serve the mesh nodes list as JSON
  server.on("/nodesData", HTTP_GET, [](AsyncWebServerRequest *request){
    mesh.update();
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

  // Handle message update
  server.on("/update", HTTP_POST, [](AsyncWebServerRequest *request){
    String newMessage = "";
    String senderName = "";
    if (request->hasParam("msg", true)) {
      newMessage = request->getParam("msg", true)->value();
      if (newMessage.length() > MAX_MESSAGE_LENGTH) {
        newMessage = newMessage.substring(0, MAX_MESSAGE_LENGTH); // Truncate message if too long
      }
    }
    if (request->hasParam("sender", true)) {
      senderName = request->getParam("sender", true)->value();
      if (senderName.length() > MAX_SENDER_NAME_LENGTH) {
        senderName = senderName.substring(0, MAX_SENDER_NAME_LENGTH); // Truncate sender name if too long
      }
    }

    newMessage.replace("<", "&lt;");
    newMessage.replace(">", "&gt;");
    senderName.replace("<", "&lt;");
    senderName.replace(">", "&gt;");

    messages[messageIndex].content = newMessage;
    messages[messageIndex].sender = senderName;
    messageIndex = (messageIndex + 1) % maxMessages;

    String message = "USER:" + senderName + ": " + newMessage;
    mesh.sendBroadcast(message);

    request->redirect("/");
  });

  server.begin();
}

void loop() {
  mesh.update();
  dnsServer.processNextRequest();
}
