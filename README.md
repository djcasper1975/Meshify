<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title>Meshmingle - Homebrew Mesh Chat</title>
    <style>
        body {
            font-family: Arial, sans-serif;
            margin: 0;
            padding: 20px;
            background-color: #f4f4f4;
            text-align: center;
        }
        .container {
            max-width: 800px;
            margin: auto;
            background: white;
            padding: 20px;
            border-radius: 10px;
            box-shadow: 0 0 10px rgba(0, 0, 0, 0.1);
        }
        h1 {
            color: #333;
        }
        p {
            font-size: 1.2em;
            color: #555;
        }
        a {
            color: #007bff;
            text-decoration: none;
        }
        a:hover {
            text-decoration: underline;
        }
        .button {
            display: inline-block;
            padding: 10px 20px;
            margin: 10px;
            font-size: 1.2em;
            color: white;
            background-color: #007bff;
            border: none;
            border-radius: 5px;
            cursor: pointer;
            text-decoration: none;
        }
        .button:hover {
            background-color: #0056b3;
        }
    </style>
</head>
<body>
    <div class="container">
        <h1>Welcome to Meshmingle</h1>
        <p>Meshmingle is a free homebrew mesh chat network that works through WiFi and LoRa to extend range beyond your home.</p>
        <p>When connected to WiFi via a mobile device, open your browser and go to <a href="http://mesh.local">http://mesh.local</a> to start chatting.</p>
        <p>Setup is easy! Just power up your device after flashing, and you're good to go.</p>
        <p>It is recommended to use a Heltec V3 LoRa device and connect it to an external antenna for longer range, although an external antenna is not required if many users are nearby on the same network.</p>
        <p>Each device on the mesh acts as a relay, so any messages sent will relay through all connected devices.</p>
        <p>You can purchase a Heltec V3 LoRa (868) device from the link below:</p>
        <a href="https://www.aliexpress.com/item/1005008177147021.html" class="button" target="_blank">Buy Heltec V3 LoRa</a>
        <h2>Flash Your Device</h2>
        <p>You can install/flash directly to your device from the links below using your PC. No additional software required (some browsers may not work; Chrome is recommended). Ensure you have the necessary drivers installed.</p>
        <a href="/HeltecV3flash.html" class="button">Flash Heltec V3</a>
        <a href="/ESP32VroomFlash.html" class="button">Flash ESP32 Vroom</a>
    </div>
</body>
</html>
