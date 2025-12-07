# **🤖 Escher AI Doorbell**

**Serverless AI Smart Doorbell & Voice Assistant | ESP32 Platform | Pure Arduino**

## **📝 Project Introduction**

Escher AI Doorbell is a library and example implementation designed to build Serverless AI Voice Assistants for Doorbell implementation on the ESP32 platform. This project is a fork of the original DAZI-AI repository. While the core library supports various voice applications, this project focuses on the AI Smart Doorbell—a fully autonomous, intelligent intercom system that can converse with guests, take messages, and integrate with your smart home via MQTT, all without requiring an external server or complex backend.

## **🌟 Featured: AI Smart Doorbell**

The **Smart Doorbell** turns an ESP32-S3 into a conversational agent that acts as your home's receptionist.

### **Why is this special?**

Unlike traditional doorbells that just ring, this AI:

1. **Talks to visitors** using OpenAI's ChatGPT.  
2. **Identifies intent** (delivery, guest, security check) based on your custom system prompt.  
3. **Does not need hardcoding**: You configure everything via a phone-friendly Web Dashboard.

### **✨ Doorbell Features**

* 📱 Web Configuration Dashboard:  
  No need to recompile code to change WiFi password or API Keys. The device hosts its own website for easy setup.  
* 🔗 MQTT Integration:  
  Connects seamlessly to Home Assistant, Node-RED, or OpenHAB.  
  * **Publishes**: Full chat history (JSON), Status updates, Button press events.  
  * **Subscribes**: Remote trigger commands (open door, trigger talk, etc.).  
* **🎨 Visual Feedback (RGB LED)**:  
  * 🟢 **Green**: AI Thinking / Speaking.  
  * 🔵 **Blue**: Listening (Guest should speak now).  
  * 🔴 **Red**: Error (Check Microphone/WiFi).  
  * ⚫ **Off**: Idle / Sleep.  
* **🗣️ Intelligent Conversation**:  
  * **Intro Mode**: The AI introduces itself when the button is first pressed.  
  * **Continuous Loop**: Automatically listens for a reply after speaking, creating a natural conversation flow.  
  * **Context Memory**: Remembers previous turns to hold a coherent conversation.

## **🚀 How to Use the Doorbell**

### **1\. Hardware Setup**

**Required:** ESP32-S3, INMP441 Microphone, MAX98357A Amp, Speaker, Button.

| Peripheral | Pin Name | ESP32-S3 GPIO |
| :---- | :---- | :---- |
| **Microphone** | SCK / WS / SD | 5 / 4 / 6 |
| **Speaker** | BCLK / LRC / DIN | 46 / 45 / 47 |
| **Button** | Signal | 0 (Boot) |

### **2\. Flash the Code**

1. Open EscherDoorbell.ino.  
2. Select Board: **ESP32S3 Dev Module**.  
3. Partition: **8M Flash (3MB APP/1.5MB SPIFFS)**.  
4. Upload.

### **3\. First Setup (AP Mode)**

1. If the device cannot connect to WiFi, it creates a Hotspot.  
2. Connect to **SSID**: Escher-Doorbell-Setup (**Password**: 12345678).  
3. Visit http://192.168.4.1 in your browser.  
4. Enter your **WiFi credentials**, **OpenAI/ByteDance keys**, and **System Prompt**.  
5. Click "Save & Restart".

### **4\. Operation**

* **Press the Button**: The AI starts the intro.  
* **Talk**: Speak when the LED turns **Blue**.  
* **Listen**: The AI responds when the LED turns **Green**.

## **📡 MQTT Topics**

| Topic | Direction | Description |
| :---- | :---- | :---- |
| escher/doorbell/history | Publish | Full chat history array (JSON format) |
| escher/doorbell/status | Publish | Current status (Listening, Speaking, Idle) |
| escher/doorbell/button | Publish | Sends "pressed" or "released" |
| escher/doorbell/in | Subscribe | Send "trigger" to this topic to remotely start the doorbell |

## **🚀 Key Features (Library)**

✅ **Serverless Design**:

* More flexible secondary development  
* Higher degree of freedom (customize prompts or models)  
* Simpler deployment (no additional server required)

✅ **Complete Voice Interaction**:

* Voice input via INMP441 microphone  
* Real-time speech recognition using ByteDance ASR API  
* AI processing through OpenAI API  
* Voice output via MAX98357A I2S audio amplifier

✅ **Continuous Conversation Mode**:

* Automatic speech recognition with VAD (Voice Activity Detection)  
* Seamless ASR → LLM → TTS conversation loop  
* Configurable conversation memory to maintain context

## **🔧 System Architecture**

The system uses a modular design with the following key components:

* **Voice Input**: INMP441 microphone with I2S interface  
* **Speech Recognition**: ByteDance ASR API for real-time transcription  
* **AI Processing**: OpenAI ChatGPT API for conversation with memory support  
* **Voice Output**: MAX98357A I2S audio amplifier for TTS playback  
* **Connectivity**: WiFi for API communication & Web Server

## **💻 Code Description**

### **Code Structure**

EscherDoorbell/  
├── EscherDoorbell.ino      \# Main State Machine & Logic  
├── src/  
│   ├── config.h            \# Pin definitions and constants  
│   ├── utils.h             \# Helper function declarations  
│   ├── utils.cpp           \# Helper function implementations  
│   └── web\_index.h         \# HTML/CSS/JS for the Web Interface  
├── library.properties      \# Arduino Library definition  
└── README.md               \# This file  
