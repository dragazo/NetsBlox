# About

This project modifies source from [Protocentral's HealthyPi v4](https://github.com/Protocentral/protocentral_healthypi_v4) to function as networked sensors for Roboscape.

## Building

This project is meant to run on an Arduino ESP32 board, and is specifically extending pre-existing behavior of HealthPi v4.
You'll need the Arduino IDE installed, as well as some special options and libraries.
Here is a detailed [walkthrough](https://healthypi.protocentral.com/Programming_HealthyPiv4.html) for getting it set up.

On top of this, you'll need to make a file named `network.h` in the project directory and supply it with your WiFi SSID (network name) and password, as well as connection info for the local port and the external NetsBlox/RoboScape server.
If your network is public (no password), leave the password empty.
Here's an example file:

```cpp
// network login info
const char *const NET_SSID   = "NetworkName";
const char *const NET_PASSWD = "PasswordOrEmpty";

// local port to listen on (UDP)
constexpr u16 LISTEN_PORT = 8888;

// NetsBlox HealthyPi server ip and port
const IPAddress SERVER_IP(12, 34, 56, 78);
constexpr unsigned int SERVER_PORT = 1974; // default healthypi server port
```

From there, simply open the `.ino` file in the Arduino IDE and compile/upload the program to the board.