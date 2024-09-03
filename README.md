MeshChat 1.0

MeshChat 1.0 is a lightweight web-based chat application designed to run on ESP32 microcontrollers using a mesh network. This project leverages the painlessMesh library to create a self-healing, self-configuring WiFi mesh network, allowing nodes to communicate without relying on a traditional WiFi router. Users can send and receive messages over the mesh network, view connected nodes, and interact with other participants through a simple web interface hosted on the ESP32.

Features
Mesh Networking: Uses painlessMesh to create a dynamic, decentralized WiFi mesh network.
Web-based Chat Interface: Users can send messages and view chat history directly through a web page hosted by the ESP32.
Real-time Node Information: Displays the number of connected nodes and the node's unique ID.
Secure Messaging: Sanitizes inputs to prevent script injection and unauthorized content sharing.
Self-contained: Does not require an internet connection; communication is strictly within the mesh.

Required Libraries

To make this project work, the following libraries are required:

painlessMesh - Library to create and manage a WiFi mesh network.
Install via Arduino Library Manager: Search for painlessMesh
WiFi - Built-in ESP32 library for WiFi functionality.
Comes pre-installed with the ESP32 board support package.
ESPAsyncWebServer - Asynchronous web server for ESP32.
Install via Arduino Library Manager: Search for ESPAsyncWebServer
Requires the AsyncTCP library as a dependency.
DNSServer - Library to handle DNS requests, used for captive portal-like behavior.
Install via Arduino Library Manager: Search for DNSServer

Setup Instructions
Install the required libraries as listed above.
Flash the code to your ESP32 using the Arduino IDE.
Once powered, the ESP32 will create a mesh network named MeshChat 1.0.
Connect to the mesh network using any WiFi-enabled device and access the chat interface at the ESP32's IP address.

Usage
Connect to wifi ssid MeshChat 1.0 and set to always connect.
Point your browser to mesh.local
Enter your name and message in the provided form to send messages.
View connected mesh nodes and the current node's ID through the interface.

Security Warning
Avoid sharing sensitive or personal information as this mesh chat is public within the connected network.
This project is ideal for experimenting with mesh networking on ESP32, creating chat applications without relying on the internet, and exploring IoT communication models.

I hope it's useful to some extent. Thanks for testing!

Mark Coultous (djcasper1975)
GitHub: djcasper1975
