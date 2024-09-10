#include <painlessMesh.h>
#include <WiFi.h>
#include <ESPAsyncWebServer.h>
#include <DNSServer.h>

// Constants and Variables
#define MESH_SSID "Meshify 1.0"
#define MESH_PORT 5555

const int maxMessages = 10;
struct Message {
  String sender;
  String content;
};
Message messages[maxMessages];
int messageIndex = 0;

AsyncWebServer server(80);
DNSServer dnsServer;
painlessMesh mesh;

// Centralized storage for node data
int totalNodeCount = 0;
uint32_t currentNodeId = 0;

// HTML Page Content
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
</style>
<script>
function fetchData() {
  fetch('/messages')
    .then(response => response.json())
    .then(data => {
      const ul = document.getElementById('messageList');
      ul.innerHTML = data.messages.reverse().map(msg => `<li>${msg.sender}: ${msg.message}</li>`).join('');
    })
    .catch(error => console.error('Error fetching messages:', error));

  fetch('/deviceCount')
    .then(response => response.json())
    .then(data => {
      document.getElementById('deviceCount').textContent =
        'Mesh Nodes: ' + data.totalCount + ', Node ID: ' + data.nodeId;
    })
    .catch(error => console.error('Error fetching device count:', error));
}

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

window.onload = function() {
  loadName();
  fetchData();
  setInterval(fetchData, 5000);
};
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
  fetch('/nodesData')
    .then(response => response.json())
    .then(data => {
      const ul = document.getElementById('nodeList');
      ul.innerHTML = data.nodes.map((node, index) => `<li>Node${index + 1}: ${node}</li>`).join('');
      document.getElementById('nodeCount').textContent = 'Mesh Nodes Connected: ' + data.nodes.length;
    })
    .catch(error => console.error('Error fetching nodes:', error));
}

window.onload = function() {
  fetchNodes();
  setInterval(fetchNodes, 10000);
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

// Function to update centralized mesh data
void updateMeshData() {
  mesh.update(); // Ensure the mesh is updated
  totalNodeCount = mesh.getNodeList().size();
  currentNodeId = mesh.getNodeId();

  // Debugging outputs to check values
  Serial.print("Updated Node Count: ");
  Serial.println(totalNodeCount);
  Serial.print("Updated Node ID: ");
  Serial.println(currentNodeId);
}

// Getter functions for centralized data
int getNodeCount() {
  return totalNodeCount;
}

uint32_t getNodeId() {
  return currentNodeId;
}

// Function to update node count and display immediately
void updateNodeDisplay() {
  updateMeshData(); // Ensure data is up-to-date before displaying
  Serial.println("Updating display with current mesh data...");
}

// Callback function for incoming mesh messages
void receivedCallback(uint32_t from, String &message) {
  Serial.printf("Received message from %u: %s\n", from, message.c_str());

  // Check if the message starts with "USER:"
  if (message.startsWith("USER:")) {
    String userMessage = message.substring(5);
    messages[messageIndex].content = userMessage;
    messages[messageIndex].sender = String(from);
    messageIndex = (messageIndex + 1) % maxMessages;
  }
}

// Mesh initialization and setup
void initMesh() {
  mesh.setDebugMsgTypes(ERROR | STARTUP | CONNECTION);
  mesh.init(MESH_SSID, "", MESH_PORT);
  mesh.onReceive(receivedCallback);

  // Callback for when connections change
  mesh.onChangedConnections([]() {
    Serial.println("Mesh connection changed.");
    updateNodeDisplay(); // Update display when connections change
  });

  // Automatically attempt reconnections
  mesh.setContainsRoot(false);
}

// Function to serve HTML pages
void serveHtml(AsyncWebServerRequest *request, const char* htmlContent) {
  request->send(200, "text/html", htmlContent);
}

// Function to set up server routes
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

  server.on("/deviceCount", HTTP_GET, [](AsyncWebServerRequest *request) {
    updateMeshData(); // Centralized data update
    request->send(200, "application/json", "{\"totalCount\":" + String(getNodeCount()) + ", \"nodeId\":\"" + String(getNodeId()) + "\"}");
  });

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

    messages[messageIndex].content = newMessage;
    messages[messageIndex].sender = senderName;
    messageIndex = (messageIndex + 1) % maxMessages;

    String message = "USER:" + senderName + ": " + newMessage;
    mesh.sendBroadcast(message);

    request->redirect("/");
  });
}

void setup() {
  Serial.begin(115200);

  // Initialize WiFi and Mesh network
  WiFi.mode(WIFI_AP);
  WiFi.setTxPower(WIFI_POWER_19_5dBm);

  initMesh();
  setupServerRoutes(); // Set up all server routes in one call

  dnsServer.start(53, "*", WiFi.softAPIP());
  server.begin();
}

void loop() {
  mesh.update();
  dnsServer.processNextRequest();
}