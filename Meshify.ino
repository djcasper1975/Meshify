#include <painlessMesh.h>
#include <WiFi.h>
#include <ESPAsyncWebServer.h>
#include <DNSServer.h>
#include <Wire.h>
#include <esp_task_wdt.h> // Watchdog timer library

// Uncomment the following line if using Heltec WiFi LoRa 32 v3 with OLED screen to test Meshify.
//#define USE_DISPLAY

#ifdef USE_DISPLAY
#include <U8g2lib.h> // Include the U8g2 library for the OLED display

// Display setup
#define RESET_OLED RST_OLED
#define I2C_SDA SDA_OLED
#define I2C_SCL SCL_OLED
#define VEXT_ENABLE Vext

U8G2_SSD1306_128X64_NONAME_F_HW_I2C u8g2(U8G2_R0, RESET_OLED, I2C_SCL, I2C_SDA);
#endif

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

unsigned long previousMillis = 0;
unsigned long animationPreviousMillis = 0;
const long interval = 5000;          // Unified update interval (5 seconds)
const long animationInterval = 90000; // Animation interval (1 minute 30 seconds)
const long animationDuration = 30000;  // Animation duration (30 seconds)

// Ball properties structure
struct Ball {
  int x;          // X position
  int y;          // Y position
  float speedX;   // Speed in the X direction
  float speedY;   // Speed in the Y direction
  int size;       // Ball size
};

// Array to hold multiple balls
const int numBalls = 3; // Number of balls
Ball balls[numBalls];   // Array of balls
bool isAnimating = false; // Flag to control when to animate

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
      ul.innerHTML = ''; // Clear the list before updating
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

#ifdef USE_DISPLAY
// Function to update node count and display immediately
void updateNodeDisplay() {
  updateMeshData(); // Ensure data is up-to-date before displaying
  Serial.println("Updating display with current mesh data...");
  updateDisplay("Meshify 1.0", ("Node ID: " + String(getNodeId())).c_str(), ("Mesh Nodes: " + String(getNodeCount())).c_str());
}
#endif

// Callback function for incoming mesh messages
void receivedCallback(uint32_t from, String &message) {
  Serial.printf("Received message from %u: %s\n", from, message.c_str());

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
    #ifdef USE_DISPLAY
    updateNodeDisplay(); // Update display when connections change
    #endif
  });

  mesh.setContainsRoot(false);
}

#ifdef USE_DISPLAY
// Function to initialize balls with random positions and speeds
void initializeBalls() {
  for (int i = 0; i < numBalls; i++) {
    balls[i].size = 4; // Set the size of each ball
    balls[i].x = random(balls[i].size + 1, u8g2.getWidth() - balls[i].size - 1);
    balls[i].y = random(balls[i].size + 1, u8g2.getHeight() - balls[i].size - 1);
    balls[i].speedX = random(1, 3) * (random(0, 2) == 0 ? 1 : -1);
    balls[i].speedY = random(1, 3) * (random(0, 2) == 0 ? 1 : -1);
  }
}

// Function to handle bouncing balls and collisions
void displayBouncingBalls() {
  u8g2.clearBuffer(); // Clear buffer before drawing new positions

  for (int i = 0; i < numBalls; i++) {
    // Update ball position
    balls[i].x += balls[i].speedX;
    balls[i].y += balls[i].speedY;

    // Check for collisions with the display boundaries
    if (balls[i].x - balls[i].size < 0) {
      balls[i].x = balls[i].size;  // Correct position if ball goes out of bounds
      balls[i].speedX = -balls[i].speedX;  // Reverse direction
    } else if (balls[i].x + balls[i].size > u8g2.getWidth()) {
      balls[i].x = u8g2.getWidth() - balls[i].size;  // Correct position if ball goes out of bounds
      balls[i].speedX = -balls[i].speedX;  // Reverse direction
    }

    if (balls[i].y - balls[i].size < 0) {
      balls[i].y = balls[i].size;  // Correct position if ball goes out of bounds
      balls[i].speedY = -balls[i].speedY;  // Reverse direction
    } else if (balls[i].y + balls[i].size > u8g2.getHeight()) {
      balls[i].y = u8g2.getHeight() - balls[i].size;  // Correct position if ball goes out of bounds
      balls[i].speedY = -balls[i].speedY;  // Reverse direction
    }

    // Check for collisions with other balls
    for (int j = 0; j < numBalls; j++) {
      if (i != j) {
        int dx = balls[i].x - balls[j].x;
        int dy = balls[i].y - balls[j].y;
        int distanceSquared = dx * dx + dy * dy;
        int collisionDistance = (balls[i].size + balls[j].size) * (balls[i].size + balls[j].size);

        if (distanceSquared < collisionDistance) {
          // Swap velocities upon collision
          float tempSpeedX = balls[i].speedX;
          float tempSpeedY = balls[i].speedY;
          balls[i].speedX = balls[j].speedX;
          balls[i].speedY = balls[j].speedY;
          balls[j].speedX = tempSpeedX;
          balls[j].speedY = tempSpeedY;
        }
      }
    }

    // Draw the ball at its new position
    u8g2.drawDisc(balls[i].x, balls[i].y, balls[i].size);
  }

  u8g2.sendBuffer(); // Send the buffer to the display once all balls are drawn
}

// Function to update the OLED display
void updateDisplay(const char* title, const char* nodeId, const char* nodeCount) {
  if (!isAnimating) {
    u8g2.clearBuffer();
    int titleWidth = u8g2.getStrWidth(title);
    u8g2.drawStr((128 - titleWidth) / 2, 10, title);
    u8g2.drawStr(0, 30, nodeId);
    u8g2.drawStr(0, 50, nodeCount);
    u8g2.sendBuffer();
  }
}
#endif

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

  WiFi.mode(WIFI_AP);
  WiFi.setTxPower(WIFI_POWER_19_5dBm);

  #ifdef USE_DISPLAY
  pinMode(VEXT_ENABLE, OUTPUT);
  digitalWrite(VEXT_ENABLE, LOW);
  u8g2.begin();
  u8g2.setFont(u8g2_font_ncenB08_tr);
  updateDisplay("Meshify 1.0", "By Mark Coultous", "Loading");
  #endif

  initMesh();  // Ensure initMesh() is called here
  #ifdef USE_DISPLAY
  initializeBalls();
  #endif

  esp_task_wdt_init(10, true);
  esp_task_wdt_add(NULL);

  dnsServer.start(53, "*", WiFi.softAPIP());
  setupServerRoutes();

  server.begin();
}

void loop() {
  esp_task_wdt_reset();

  mesh.update();
  dnsServer.processNextRequest();

  unsigned long currentMillis = millis();

  // Regular update to display the latest node data
  #ifdef USE_DISPLAY
  if (!isAnimating && (currentMillis - previousMillis >= interval)) {
    previousMillis = currentMillis;
    updateNodeDisplay(); // Ensure display is updated regularly
  }

  // Animation logic for bouncing balls
  if (currentMillis - animationPreviousMillis >= animationInterval && !isAnimating) {
    isAnimating = true;
    animationPreviousMillis = currentMillis;
  }

  if (isAnimating && (currentMillis - animationPreviousMillis < animationDuration)) {
    esp_task_wdt_reset();
    displayBouncingBalls();
  } else if (isAnimating && (currentMillis - animationPreviousMillis >= animationDuration)) {
    isAnimating = false;
    animationPreviousMillis = currentMillis;
    updateNodeDisplay(); // Update the display after animation ends
  }
  #endif
}
