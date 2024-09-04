MeshChat 1.0 is a lightweight web-based chat application designed to run on ESP32 microcontrollers using a mesh network. This project leverages the painlessMesh library to create a self-healing, self-configuring WiFi mesh network, allowing nodes to communicate without relying on a traditional WiFi router. Users can send and receive messages over the mesh network, view connected nodes, and interact with other participants through a simple web interface hosted on the ESP32.

Features
Mesh Networking: Uses painlessMesh to create a dynamic, decentralized WiFi mesh network.
Web-based Chat Interface: Users can send messages and view chat history directly through a web page hosted by the ESP32.
Real-time Node Information: Displays the number of connected nodes and the node's unique ID.
Secure Messaging: Sanitizes inputs to prevent script injection and unauthorized content sharing.
Self-contained: Does not require an internet connection; communication is strictly within the mesh.

How Messages Are Delivered in Mesh Networks
Multi-Hop Communication: In a mesh network, messages can be relayed through intermediary nodes. This means that even if a node is not directly connected to your node, the message can still be delivered if there is a path through other nodes. The network dynamically routes messages through the available nodes to reach the destination.

Broadcasting Messages: When a message is broadcast using mesh.sendBroadcast(), it is sent to all connected nodes. These nodes then forward the message to other nodes, effectively propagating the message throughout the entire mesh network. This ensures that every node, even those not directly connected to your node, receives the message as long as the mesh is connected as a whole.

Node Visibility: If a node is visible in the list of connected nodes on your device, it means it is part of the mesh, either directly or indirectly. However, even if a node does not appear directly in your node's list, it can still be part of the network as an intermediary node, forwarding messages between other nodes.

Mesh Node List and Connection Changes: The list of nodes you see from your node reflects the nodes directly visible to it (nodes it has direct connections with). Nodes that are connected indirectly (via other nodes) will not appear directly in this list, but they still participate in the communication network.

Ensuring Message Delivery: The painlessMesh library handles the message propagation across the network, ensuring messages are delivered as long as there is a path between nodes. If the message fails to reach a node because the network is partitioned or there are no available routes, this would be an exception rather than the norm.

Practical Example
Scenario 1: Your node (Node A) is connected to Node B. Node B is connected to Node C, but Node C is not directly connected to Node A. If you send a message from Node A, Node B will relay it to Node C automatically.

Scenario 2: If Node C is out of range of both Node A and Node B and not connected through any other intermediary nodes, it will not receive the message, and it will not appear in the list of nodes visible to Node A or B.

Conclusion
Messages will still get to nodes that are not directly connected to your node as long as they are part of the mesh through any path. The mesh network is designed to route messages dynamically, making it robust and efficient in communication across multiple nodes.

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
