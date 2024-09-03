MeshChat 1.0

MeshChat 1.0 is a lightweight web-based chat application designed to run on ESP32 microcontrollers using a mesh network. This project leverages the painlessMesh library to create a self-healing, self-configuring WiFi mesh network, allowing nodes to communicate without relying on a traditional WiFi router. Users can send and receive messages over the mesh network, view connected nodes, and interact with other participants through a simple web interface hosted on the ESP32.

Features
Mesh Networking: Uses painlessMesh to create a dynamic, decentralized WiFi mesh network.
Web-based Chat Interface: Users can send messages and view chat history directly through a web page hosted by the ESP32.
Real-time Node Information: Displays the number of connected nodes and the node's unique ID.
Secure Messaging: Sanitizes inputs to prevent script injection and unauthorized content sharing.
Self-contained: Does not require an internet connection; communication is strictly within the mesh.

Setup Instructions:
Install Required Libraries:

Ensure you have the latest ESP32 board package installed in the Arduino IDE.

Required libraries:
painlessMesh: Install via Arduino Library Manager by searching for painlessMesh.
ESPAsyncWebServer: Install from the Arduino Library Manager by searching for ESPAsyncWebServer.
AsyncTCP: Required dependency for ESPAsyncWebServer, also available via the Arduino Library Manager.
DNSServer: Install from the Arduino Library Manager.
Flash the Code to the ESP32:

Copy and paste the provided code into your Arduino IDE.
Select the correct board (e.g., "ESP32 Dev Module") and the correct COM port.
Click "Upload" to flash the code to the ESP32.
Connect to the WiFi Mesh Network:

After uploading, the ESP32 will create a mesh network named MeshChat 1.0.
Connect any WiFi-enabled device (e.g., smartphone, laptop) to the mesh network.
Access the Chat Interface:

Open a web browser on your connected device.
Enter the ESP32's IP address, typically 192.168.4.1, in the browser's address bar. This will bring up the chat interface.
Using the Chat Interface:

Sending Messages: Enter your name and message in the form and click "Send." Messages will appear in the chat window.
Viewing Mesh Nodes: Click on the "View Mesh Nodes List" link to see all connected nodes in the mesh network.
Node Information: The page displays the number of connected nodes and the current node's ID.
Security Warning:

Avoid sharing personal or sensitive information. This is a local mesh network, and communication is not encrypted.
This project is an excellent introduction to mesh networking using ESP32, allowing for decentralized communication between devices without relying on existing WiFi infrastructure.

I hope it's useful to some extent. Thanks for testing!

Mark Coultous (djcasper1975)
GitHub: djcasper1975
