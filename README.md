# Acoustic Threat Localization Network (ATLN)

## Project Overview

**Project Name:** Acoustic Threat Localization Network (ATLN)

**Category:** Internet of Things (IoT), Wireless Sensor Networks, Acoustic Sensing, Edge Analytics

**Objective:**
Develop a distributed acoustic sensing network capable of detecting loud acoustic events, estimating the source location using multiple sensor nodes, and visualizing the event in real time through a dashboard with optional cloud connectivity.

---

# Problem Statement

In surveillance, defense, smart campus, industrial safety, and perimeter monitoring applications, identifying the location of sudden acoustic events is critical.

Traditional systems are:

* Expensive
* Proprietary
* Infrastructure dependent

This project demonstrates a low-cost proof-of-concept using ESP32 microcontrollers and microphones to detect and localize acoustic events.

---

# Final System Architecture

```text
Node A (ESP32 + MAX4466)
            \
Node B (ESP32 + MAX4466)
              \
               > ESP-NOW Mesh
              /
Node C (ESP32 + MAX4466)
            /

                ↓

      Node D (Master ESP32)
      + MAX4466
      + Localization Engine
      + Dashboard Gateway

                ↓

         USB Serial Link

                ↓

        Python Flask Server

          ├── Local Dashboard
          └── Firebase Cloud
```

---

# Hardware Used

## Sensor Nodes

### Quantity

* 4 × ESP32 Development Boards
* 4 × MAX4466 Microphone Modules

### Node Assignments

| Node | Function                                 |
| ---- | ---------------------------------------- |
| A    | Acoustic Sensor                          |
| B    | Acoustic Sensor                          |
| C    | Acoustic Sensor                          |
| D    | Acoustic Sensor + Gateway + Localization |

---

# Communication Method

## ESP-NOW

ESP-NOW is used for:

* Low latency communication
* Peer-to-peer networking
* No router dependency
* Real-time event transmission

### Data Packet

```cpp
typedef struct {
    char node;
    int peak;
    unsigned long timestamp;
} SensorData;
```

Fields:

* node → Sender ID
* peak → Detected sound intensity
* timestamp → Event timestamp

---

# Microphone Calibration

MAX4466 output varies with environment.

Measured values:

### Silent Room (Fan ON)

```text
1600 - 2100
```

### Far Clap

```text
3000+
```

### Near Clap

```text
4000+
```

Adaptive thresholding used:

```cpp
threshold = background + 800;
```

Background noise:

```cpp
background =
background*0.99 +
sample*0.01;
```

---

# Sensor Layout

Nodes are deployed in a square.

```text
A(0,0) -------- B(4,0)

  |              |

  |              |

C(0,4) -------- D(4,4)
```

Units:

Meters

---

# Localization Method

## Weighted Acoustic Centroid

The system estimates source location using relative acoustic intensities.

### X Coordinate

```cpp
x =
(
A*0 +
B*4 +
C*0 +
D*4
)
/
(A+B+C+D);
```

### Y Coordinate

```cpp
y =
(
A*0 +
B*0 +
C*4 +
D*4
)
/
(A+B+C+D);
```

This provides an approximate source location.

---

# Threat Classification

Threat level is determined using peak acoustic intensity.

```cpp
if(maxPeak > 3800)
    HIGH;

else if(maxPeak > 3000)
    MEDIUM;

else
    LOW;
```

### Threat Levels

| Peak Value | Threat |
| ---------- | ------ |
| >3800      | HIGH   |
| >3000      | MEDIUM |
| Otherwise  | LOW    |

---

# Master Node Functions

Node D performs:

### Acoustic Sensing

Detects local sound levels.

### ESP-NOW Receiver

Receives data from:

* Node A
* Node B
* Node C

### Localization

Computes:

* X coordinate
* Y coordinate

### Threat Analysis

Classifies:

* LOW
* MEDIUM
* HIGH

### Data Output

Produces JSON packets.

Example:

```json
{
  "A":4095,
  "B":3269,
  "C":4095,
  "D":4095,
  "x":1.89,
  "y":2.11,
  "threat":"HIGH"
}
```

---

# Software Stack

## Embedded

### Platform

ESP32 Arduino Framework

### Communication

ESP-NOW

### Language

C++

---

## Backend

### Language

Python

### Framework

Flask

Responsibilities:

* Read serial data
* Serve dashboard
* Upload to Firebase

---

## Frontend

### Technologies

* HTML
* CSS
* JavaScript

Dashboard Features:

* Live sensor values
* Threat level
* Source location
* Event history
* Real-time updates

---

# Dashboard Features

## Live Sensor Values

Displays:

```text
A : 4095
B : 3269
C : 4095
D : 4095
```

---

## Threat Indicator

Displays:

```text
Threat: HIGH
```

Color coded:

* Green → LOW
* Orange → MEDIUM
* Red → HIGH

---

## Localization Map

Displays node positions.

```text
A ●----------● B

      🔴

C ●----------● D
```

Red marker indicates estimated source.

---

## Event History

Stores recent detections.

Example:

```text
Time       Threat
-------------------
21:35:02   HIGH
21:36:10   MEDIUM
21:37:04   HIGH
```

---

# Cloud Connectivity

## Firebase Realtime Database

Project:

```text
acousticthreatnetwork
```

Database URL:

```text
https://acousticthreatnetwork-default-rtdb.firebaseio.com
```

Purpose:

* Remote monitoring
* Event logging
* Historical analysis
* Future mobile integration

---

# Firebase Structure

```json
{
  "acoustic": {
    "latest": {
      "A": 4095,
      "B": 3269,
      "C": 4095,
      "D": 4095,
      "x": 1.89,
      "y": 2.11,
      "threat": "HIGH"
    }
  }
}
```

---

# Future Improvements

## Phase 2

Replace MAX4466 with:

```text
INMP441
```

Advantages:

* Digital I2S output
* Better synchronization
* Higher accuracy

---

## True TDOA Localization

Implement:

* Time Difference of Arrival
* Hyperbolic localization
* GCC-PHAT

---

## Edge AI

Add sound classification:

* Gunshot
* Explosion
* Vehicle
* Human Voice
* Ambient Noise

---

## Mobile Application

Features:

* Real-time notifications
* Remote monitoring
* Event replay

---

## Heatmap Analytics

Generate:

* Frequent event zones
* Threat density maps
* Historical analysis

---

# Demonstration Workflow

1. Deploy four nodes.
2. Open dashboard.
3. Generate acoustic event.
4. Nodes detect event.
5. ESP-NOW transmits data.
6. Master computes location.
7. Dashboard updates in real time.
8. Threat level displayed.
9. Event stored locally/cloud.

---

# Key Technical Highlights

* 4-node wireless acoustic sensor network
* ESP-NOW mesh communication
* Adaptive acoustic thresholding
* Real-time localization
* Threat classification
* Local dashboard visualization
* Firebase cloud integration
* Low-cost implementation
* Expandable architecture

---

# Team Notes

This implementation is a proof-of-concept designed to demonstrate distributed acoustic localization principles using affordable IoT hardware. It is intended as a foundation for future work involving precise TDOA localization, edge AI sound classification, and cloud-scale monitoring systems.
