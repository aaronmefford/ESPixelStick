/*
  ESPixelStick.ino

  Project: ESPixelStick - An ESP8266 and E1.31 based pixel driver
  Copyright (c) 2016 Shelby Merrick
  http://www.forkineye.com

   This program is provided free for you to use in any way that you wish,
   subject to the laws and regulations where you are using it.  Due diligence
   is strongly suggested before using this code.  Please give credit where due.

   The Author makes no warranty of any kind, express or implied, with regard
   to this program or the documentation contained in this document.  The
   Author shall not be liable in any event for incidental or consequential
   damages in connection with, or arising out of, the furnishing, performance
   or use of these programs.

*/

/*****************************************/
/*        BEGIN - Configuration          */
/*****************************************/

/* Fallback configuration if config.json is empty or fails */
// Don't change the default, change ssid[]
const char default_ssid[]       = "ESPPixelNet";
const char default_passphrase[] = "--ESP4YourLife!--";
const char ssid[] = "mefford";
//const char* ssid = default_ssid;
const char passphrase[] = "iwillobeytherules";
//const char passphrase[] = "";

/*****************************************/
/*         END - Configuration           */
/*****************************************/

#include <ESP8266WiFi.h>
#include <Ticker.h>
#include <AsyncMqttClient.h>
#include <ESP8266mDNS.h>
#include <ESPAsyncTCP.h>
#include <ESPAsyncUDP.h>
#include <ESPAsyncWebServer.h>
#include <ESPAsyncE131.h>
#include <ArduinoJson.h>
#include <DNSServer.h>
#include <Hash.h>
#include <SPI.h>
#include "ESPixelStick.h"
#include "EFUpdate.h"
#include "wshandler.h"
#include "gamma.h"

extern "C" {
#include <user_interface.h>
}

// Debugging support
#if defined(DEBUG)
extern "C" void system_set_os_print(uint8 onoff);
extern "C" void ets_install_putc1(void* routine);

static void _u0_putc(char c) {
  while (((U0S >> USTXC) & 0x7F) == 0x7F);
  U0F = c;
}
#endif

/////////////////////////////////////////////////////////
//
//  Globals
//
/////////////////////////////////////////////////////////

// MQTT State
const char MQTT_SET_COMMAND_TOPIC[] = "/set";

// MQTT Payloads by default (on/off)
const char LIGHT_ON[] = "ON";
const char LIGHT_OFF[] = "OFF";

// MQTT json buffer size
const int JSON_BUFFER_SIZE = JSON_OBJECT_SIZE(10);

// Configuration file
const char CONFIG_FILE[] = "/config.json";

// DNS Server Default Port
const byte DNS_PORT = 53;
IPAddress ap_ip(172, 31, 255, 254);
IPAddress ap_subnet(255, 255, 255, 0);

ESPAsyncE131        e131(10);       // ESPAsyncE131 with X buffers
config_t            config;         // Current configuration
uint32_t            *seqError;      // Sequence error tracking for each universe
uint16_t            uniLast = 1;    // Last Universe to listen for
bool                reboot = false; // Reboot flag
AsyncWebServer      web(HTTP_PORT); // Web Server
AsyncWebSocket      ws("/ws");      // Web Socket Plugin
DNSServer           dnsServer;      // DNS Server for Captive Portal
uint8_t             *seqTracker;    // Current sequence numbers for each Universe */
uint32_t            lastUpdate;     // Update timeout tracker
uint32_t            lastWebRequest; // WebRequest timeout tracker
uint32_t            lastE131Request;// E131 timeout tracker

WiFiEventHandler    wifiConnectHandler;     // WiFi connect handler
WiFiEventHandler    wifiDisconnectHandler;  // WiFi disconnect handler
Ticker              wifiTicker; // Ticker to handle WiFi
AsyncMqttClient     mqtt;       // MQTT object
Ticker              mqttTicker; // Ticker to handle MQTT
bool                wifiStationConnected;
bool                asAP;
bool                firstMessage;
bool                asAPColorOn;
uint32_t            asAPLastColor;     // Update asAP color timeout tracker
EffectEngine        effects;    // Effects Engine

// Output Drivers
#if defined(ESPS_MODE_PIXEL)
PixelDriver     pixels;         // Pixel object
#elif defined(ESPS_MODE_SERIAL)
SerialDriver    serial;         // Serial object
#else
#error "No valid output mode defined."
#endif

/////////////////////////////////////////////////////////
//
//  Forward Declarations
//
/////////////////////////////////////////////////////////

void loadConfig();
void initStationWifi();
void initWeb();
void updateConfig();
bool connectConfigWifi();
bool connectDefaultWifi();
bool connectStationWifi();
void reconnectStationWifi();


/////////////////////////////////////////////////////////
//
//  Macros
//
/////////////////////////////////////////////////////////


#define setPixelValue(x,i,val) (x.setValue(i, val);)
#if defined(ESPS_MODE_PIXEL)
#define sceneShow() (pixels.show())
#define setPixelValue(i,val) (pixels.setValue(i, val))
#define refresh() ( pixels.canRefresh() && pixels.show() )
#elif defined(ESPS_MODE_SERIAL)
#define sceneShow() (serial.show())
#define setPixelValue(i,val) (serial.setValue(i, val))
#define refresh() ( serial.canRefresh() && serial.show())
#endif

/////////////////////////////////////////////////////////
//
//  Initialize and Setup
//
/////////////////////////////////////////////////////////

// Radio config
RF_PRE_INIT() {
  //wifi_set_phy_mode(PHY_MODE_11G);    // Force 802.11g mode
  system_phy_set_powerup_option(31);  // Do full RF calibration on power-up
  system_phy_set_max_tpw(82);         // Set max TX power
}
void setInitialPinStates() {
  // Initial pin states
  pinMode(DATA_PIN, OUTPUT);
  digitalWrite(DATA_PIN, LOW);
  // Configure the outputs

#if defined (ESPS_SUPPORT_PWM)
  setupPWM();
#endif

#if defined (ESPS_MODE_PIXEL)
  pixels.setPin(DATA_PIN);
  pixels.begin();
#elif defined(ESPS_MODE_SERIAL)
  //serial.begin(&SEROUT_PORT, config.serial_type, config.channel_count, config.baudrate);
#endif

}

void reportMacAddress() {
  LOG_PORT.print(F("\n####################################################\n#  MAC Address: "));
  LOG_PORT.print(WiFi.macAddress());
  LOG_PORT.print(F("\n####################################################"));
}

void initSerialLogPort() {
  // Setup serial log port
  LOG_PORT.begin(115200);
  delay(10);
}



void setup() {
  asAP = false;
  asAPColorOn = false;
  firstMessage = false;
  wifiStationConnected = false;

  initSerialLogPort();
  reportMacAddress();

  setInitialPinStates();
  loadConfig();
  updateConfig();

  pixels.setGamma(0);
  updateGammaTable(2.2, 1.0);

  LOG_PORT.println("Initialized calling show");
  sceneShow();
  LOG_PORT.println("Called show, calling setStatic");
  //setStatic(255,255,255,200);
  //setStatic(0,0,0,200);
  //setStatic(255,0,0,200);
  //setStatic(0,255,0,200);
  //setStatic(0,0,255,200);
  //setStatic(255,255,0,200);
  //setStatic(0,255,255,200);
  //setStatic(255,0,255,200);
  //setStatic(0,0,0,200);
  //setStatic(64,64,64,200);
  //setStatic(128,128,128,200);
  //setStatic(192,192,192,200);
  
  LOG_PORT.println("Called setStatic");

#if defined(DEBUG)
  ets_install_putc1((void *) &_u0_putc);
  system_set_os_print(1);
#endif

  // Enable SPIFFS
  SPIFFS.begin();

  // Load configuration from SPIFFS and set Hostname
  //loadConfig();
  reportVersion();
  initStationWifi();
  initMQTT();

    // Set default data source to E131
    config.ds = DataSource::E131;

  if ( connectStationWifi() ) {
    wifiStationConnected = true;
  } else {
    //LOG_PORT.println(F("**** FAILED TO ASSOCIATE WITH AP ****"));
    // If we fail, go SoftAP or reboot
    WiFi.mode(WIFI_AP_STA);

    startSoftAP();

    //doReboot();
  }

  // Configure and start the web server
  initWeb();
  initE131();
  updateConfig();
}

void doReboot() {

  LOG_PORT.println(F("**** REBOOTING ****"));
  //setStatic(255,0,0);
  delay(REBOOT_DELAY / 4);
  //setStatic(0,0,0);
  delay(REBOOT_DELAY / 4);
  //setStatic(255,0,0);
  delay(REBOOT_DELAY / 4);
  //setStatic(0,0,0);
  delay(REBOOT_DELAY / 4);
  //setStatic(255,0,0);
  ESP.restart();
}

void reportVersion() {
    LOG_PORT.println("");
    LOG_PORT.print(F("ESPixelStick v"));
    for (uint8_t i = 0; i < strlen_P(VERSION); i++)
        LOG_PORT.print((char)(pgm_read_byte(VERSION + i)));
    LOG_PORT.print(F(" ("));
    for (uint8_t i = 0; i < strlen_P(BUILD_DATE); i++)
        LOG_PORT.print((char)(pgm_read_byte(BUILD_DATE + i)));
    LOG_PORT.println(F(")"));
    //LOG_PORT.println(ESP.getFullVersion());
}

void initE131() {
  // Setup E1.31
    // Setup the sequence error tracker
  uint8_t uniTotal = (uniLast + 1) - config.universe;

  if (seqTracker) free(seqTracker);
  if ((seqTracker = static_cast<uint8_t *>(malloc(uniTotal))))
    memset(seqTracker, 0x00, uniTotal);

  if (seqError) free(seqError);
  if ((seqError = static_cast<uint32_t *>(malloc(uniTotal * 4))))
    memset(seqError, 0x00, uniTotal * 4);

  // Zero out packet stats
  e131.stats.num_packets = 0;
  if (config.multicast) {
    if (e131.begin(E131_MULTICAST, config.universe,
                   uniLast - config.universe + 1)) {
      LOG_PORT.println(F("- Multicast Enabled"));
    }  else {
      LOG_PORT.println(F("*** MULTICAST INIT FAILED ****"));
    }
  } else {
    if (e131.begin(E131_UNICAST)) {
      LOG_PORT.print(F("- Unicast port: "));
      LOG_PORT.println(E131_DEFAULT_PORT);
    } else {
      LOG_PORT.println(F("*** UNICAST INIT FAILED ****"));
    }
  }
}

/////////////////////////////////////////////////////////
//
//  WiFi Section
//
/////////////////////////////////////////////////////////

void initStationWifi() {
  // Configure SDK params
  wifi_set_sleep_type(NONE_SLEEP_T);
  //Don't wear out the flash writing unused wifi settings
  WiFi.persistent(false);

  if ( config.hostname.length() )
    WiFi.hostname(config.hostname);

  // Switch to station mode and disconnect just in case
  WiFi.mode(WIFI_STA);

  // Setup WiFi Handlers
  wifiConnectHandler = WiFi.onStationModeGotIP(onWifiConnect);

}

bool connectStationWifi() {
  //setStatic(0,0,0);
  delay(secureRandom(50, 250));

  //setStatic(128,255,0);
  if ( connectConfigWifi() ) {
    LOG_PORT.println(F("Successfully connected to the configured WiFi"));
  }  else   {
    //setStatic(255,128,0);
    // Fallback to default SSID and passphrase if we fail to connect
    // If the SSID is the same as the configured SSID then there is no point to retry
    // If the hardcoded SSID has not been set to something different than the default SSID there is no point
    if (config.ssid.length() && ! config.ssid.equals(ssid) )
      LOG_PORT.println(F("*** Timeout - Reverting to default SSID ***"));
    if ( ! connectDefaultWifi() )
      return false;
  }
  if ( ! getStationIP() ) {
    return false;
  }
  wifiDisconnectHandler = WiFi.onStationModeDisconnected(onWiFiDisconnect);

  // Setup IGMP subscriptions if multicast is enabled
  if (config.multicast)
    multiSub();
  return true;

}
void reconnectStationWifi() {
  connectStationWifi();
}

bool connectConfigWifi() {

  // Config is empty or default
  if ( !config.ssid.length() || config.ssid.equals(default_ssid) ) {
    LOG_PORT.print(F("** Skipping first connect, we are using the default SSID: "));

    //If we have a default config we will need the AP to set the config
    config.ap_fallback = true;
    return false;
  }
  return connectAttemptWifi(config.ssid.c_str(), config.hostname.c_str(), config.passphrase.c_str());
}

bool connectDefaultWifi() {
  return connectAttemptWifi(ssid, config.hostname.c_str(), passphrase);
}

bool connectAttemptWifi(const char * ssid, const char * hostname, const char * passphrase) {
  WiFi.disconnect();

  LOG_PORT.println("");
  LOG_PORT.print(F("Connecting to "));
  LOG_PORT.print(ssid);
  LOG_PORT.print(F(" as "));
  LOG_PORT.println(hostname);

  WiFiMode_t wMode = WiFi.getMode();
  if ( wMode != WIFI_STA && wMode != WIFI_AP_STA ) {
    LOG_PORT.println(F("Cannot connectWiFi, WiFi mode is incorrect"));
    return false;
  }

  WiFi.begin(ssid, passphrase);

  if (WiFi.status() != WL_CONNECTED ) {
    LOG_PORT.println(F("WiFi Connect Failed!"));
    return false;
  }

  return true;
}

bool getStationIP() {
  if (config.dhcp) {
    LOG_PORT.print(F("Connecting with DHCP"));
  } else {
    // We don't use DNS, so just set it to our gateway
    WiFi.config(IPAddress(config.ip[0], config.ip[1], config.ip[2], config.ip[3]),
                IPAddress(config.gateway[0], config.gateway[1], config.gateway[2], config.gateway[3]),
                IPAddress(config.netmask[0], config.netmask[1], config.netmask[2], config.netmask[3]),
                IPAddress(config.gateway[0], config.gateway[1], config.gateway[2], config.gateway[3])
               );
    LOG_PORT.print(F("Connecting with Static IP"));
  }
  return true;
}

void onWifiConnect(const WiFiEventStationModeGotIP &event) {
  LOG_PORT.println("");
  LOG_PORT.print(F("Connected with IP: "));
  LOG_PORT.println(WiFi.localIP());

  // Setup MQTT connection if enabled
  if (config.mqtt)
    connectToMqtt();

  // Setup mDNS / DNS-SD
  //TODO: Reboot or restart mdns when config.id is changed?
  char chipId[7] = { 0 };
  snprintf(chipId, sizeof(chipId), "%06x", ESP.getChipId());
  MDNS.setInstanceName(config.id + " (" + String(chipId) + ")");
  if (MDNS.begin(config.hostname.c_str())) {
    MDNS.addService("http", "tcp", HTTP_PORT);
    MDNS.addService("e131", "udp", E131_DEFAULT_PORT);
    MDNS.addServiceTxt("e131", "udp", "TxtVers", String(RDMNET_DNSSD_TXTVERS));
    MDNS.addServiceTxt("e131", "udp", "ConfScope", RDMNET_DEFAULT_SCOPE);
    MDNS.addServiceTxt("e131", "udp", "E133Vers", String(RDMNET_DNSSD_E133VERS));
    MDNS.addServiceTxt("e131", "udp", "CID", String(chipId));
    MDNS.addServiceTxt("e131", "udp", "Model", "ESPixelStick");
    MDNS.addServiceTxt("e131", "udp", "Manuf", "Forkineye");
  } else {
    LOG_PORT.println(F("*** Error setting up mDNS responder ***"));
  }
}

void onWiFiDisconnect(const WiFiEventStationModeDisconnected &event) {
  LOG_PORT.println(F("*** WiFi Disconnected ***"));

  // Pause MQTT reconnect while WiFi is reconnecting
  mqttTicker.detach();
  wifiTicker.once(2, reconnectStationWifi);
}

// Subscribe to "n" universes, starting at "universe"
void multiSub() {
  uint8_t count;
  ip_addr_t ifaddr;
  ip_addr_t multicast_addr;

  count = uniLast - config.universe + 1;
  ifaddr.addr = static_cast<uint32_t>(WiFi.localIP());
  for (uint8_t i = 0; i < count; i++) {
    multicast_addr.addr = static_cast<uint32_t>(IPAddress(239, 255,
                          (((config.universe + i) >> 8) & 0xff),
                          (((config.universe + i) >> 0) & 0xff)));
    igmp_joingroup(&ifaddr, &multicast_addr);
  }
}

bool startSoftAP() {
  LOG_PORT.println(F("**** FAILED TO ASSOCIATE WITH AP, GOING SOFTAP ****"));
  //setStatic(255,255,0);

  //if (config.ap_fallback) {
  asAP = true;

  WiFi.mode(WIFI_AP);
  WiFiMode_t wMode = WiFi.getMode();
  if ( wMode != WIFI_AP && wMode != WIFI_AP_STA ) {
    LOG_PORT.println(F("Cannot start SoftAP, WiFi mode is incorrect"));
    return false;
  }
  String ssid = WiFi.hostname();
  if ( config.hostname.length() )
    ssid = "ESPixelStick " + String(config.hostname);

  LOG_PORT.println(
    WiFi.softAPConfig(ap_ip, ap_ip, ap_subnet) &&
    WiFi.softAP(ssid.c_str()) ? F("SoftAP Ready") : F("SoftAP Failed!"));
  LOG_PORT.print(F("MAC address = "));
  LOG_PORT.println(WiFi.softAPmacAddress().c_str());

  // if DNSServer is started with "*" for domain name,
  // it will reply with the provided IP to all DNS requests
  dnsServer.setErrorReplyCode(DNSReplyCode::NoError);
  dnsServer.start(DNS_PORT, "*", WiFi.softAPIP());
  LOG_PORT.print(F("IP Address: "));
  LOG_PORT.println(WiFi.softAPIP());

}



/////////////////////////////////////////////////////////
//
//  MQTT Section
//
/////////////////////////////////////////////////////////


void initMQTT() {
  // Setup MQTT Handlers
  if (config.mqtt) {
    mqtt.onConnect(onMqttConnect);
    mqtt.onDisconnect(onMqttDisconnect);
    mqtt.onMessage(onMqttMessage);
    mqtt.setServer(config.mqtt_ip.c_str(), config.mqtt_port);
    if (config.mqtt_user.length() > 0)
      mqtt.setCredentials(config.mqtt_user.c_str(), config.mqtt_password.c_str());
  }
}
void connectToMqtt() {
  LOG_PORT.print(F("- Connecting to MQTT Broker "));
  LOG_PORT.println(config.mqtt_ip);
  mqtt.connect();
}

void onMqttConnect(bool sessionPresent) {
  LOG_PORT.println(F("- MQTT Connected"));

    // Get retained MQTT state
    mqtt.subscribe(config.mqtt_topic.c_str(), 0);
    mqtt.unsubscribe(config.mqtt_topic.c_str());

    // Setup subscriptions
    mqtt.subscribe(String(config.mqtt_topic + MQTT_SET_COMMAND_TOPIC).c_str(), 0);

    // Publish state
    publishState();
}

void onMqttDisconnect(AsyncMqttClientDisconnectReason reason) {
    LOG_PORT.println(F("- MQTT Disconnected"));
    if (WiFi.isConnected()) {
        mqttTicker.once(2, connectToMqtt);
    }
}

void onMqttMessage(char* topic, char* payload,
        AsyncMqttClientMessageProperties properties, size_t len, size_t index, size_t total) {
    StaticJsonBuffer<JSON_BUFFER_SIZE> jsonBuffer;
    JsonObject& root = jsonBuffer.parseObject(payload);
    bool stateOn = false;

    if (!root.success()) {
        LOG_PORT.println("MQTT: Parsing failed");
        return;
    }

    if (root.containsKey("state")) {
        if (strcmp(root["state"], LIGHT_ON) == 0) {
            stateOn = true;
        } else if (strcmp(root["state"], LIGHT_OFF) == 0) {
            stateOn = false;
        }
    }

    if (root.containsKey("brightness")) {
        effects.setBrightness(root["brightness"]);
    }

    if (root.containsKey("color")) {
        effects.setColor({
            root["color"]["r"],
            root["color"]["g"],
            root["color"]["b"]
        });
    }

    if (root.containsKey("effect")) {
        // Set the explict effect provided by the MQTT client
        effects.setEffect(root["effect"]);
    }

    if (root.containsKey("reverse")) {
        effects.setReverse(root["reverse"]);
    }

    if (root.containsKey("mirror")) {
        effects.setMirror(root["mirror"]);
    }

    // Set data source based on state - Fall back to E131 when off
    if (stateOn) {
        config.ds = DataSource::MQTT;
    } else {
        config.ds = DataSource::E131;
        effects.clearAll();
    }

    publishState();
}

void publishState() {
    StaticJsonBuffer<JSON_BUFFER_SIZE> jsonBuffer;
    JsonObject& root = jsonBuffer.createObject();
    root["state"] = (config.ds == DataSource::MQTT) ? LIGHT_ON : LIGHT_OFF;
    JsonObject& color = root.createNestedObject("color");
    color["r"] = effects.getColor().r;
    color["g"] = effects.getColor().g;
    color["b"] = effects.getColor().b;
    root["brightness"] = effects.getBrightness();
    if (effects.getEffect() != nullptr) {
        root["effect"] = effects.getEffect();
    }
    root["reverse"] = effects.getReverse();
    root["mirror"] = effects.getMirror();

    char buffer[root.measureLength() + 1];
    root.printTo(buffer, sizeof(buffer));
    mqtt.publish(config.mqtt_topic.c_str(), 0, true, buffer);
}

/////////////////////////////////////////////////////////
//
//  Web Section
//
/////////////////////////////////////////////////////////

// Configure and start the web server
void initWeb() {
  // Handle OTA update from asynchronous callbacks
  Update.runAsync(true);

  // Add header for SVG plot support?
  DefaultHeaders::Instance().addHeader("Access-Control-Allow-Origin", "*");

  // Setup WebSockets
  ws.onEvent(wsEvent);
  web.addHandler(&ws);

  // Heap status handler
  web.on("/heap", HTTP_GET, [](AsyncWebServerRequest * request) {
    LOG_PORT.println(F("GET /heap request"));
    request->send(200, "text/plain", String(ESP.getFreeHeap()));
  });

  // JSON Config Handler
  web.on("/conf", HTTP_GET, [](AsyncWebServerRequest * request) {
    LOG_PORT.println(F("GET /conf request"));
    String jsonString;
    serializeConfig(jsonString, true);
    request->send(200, "text/json", jsonString);
  });

  // gamma debugging Config Handler
  web.on("/gamma", HTTP_GET, [](AsyncWebServerRequest * request) {
    LOG_PORT.println(F("GET /gamma request"));
    AsyncResponseStream *response = request->beginResponseStream("text/plain");
    for (int i = 0; i < 256; i++) {
      response->printf ("%5d,", GAMMA_TABLE[i]);
      if (i % 16 == 15) {
        response->printf("\r\n");
      }
    }
    request->send(response);
  });

  // Firmware upload handler
  web.on("/updatefw", HTTP_POST, [](AsyncWebServerRequest * request) {
    LOG_PORT.println(F("POST /updatefw request"));
    ws.textAll("X6");
  }, handle_fw_upload);

  // Static Handler
  web.serveStatic("/", SPIFFS, "/www/").setDefaultFile("index.html");
  web.serveStatic("/config.json", SPIFFS, "/config.json");

  web.onNotFound([](AsyncWebServerRequest * request) {
    LOG_PORT.println(F("GET NOT_FOUND request"));
    request->send(404, "text/plain", "Page not found");
  });

  web.begin();

  LOG_PORT.print(F("- Web Server started on port "));
  LOG_PORT.println(HTTP_PORT);
}

/////////////////////////////////////////////////////////
//
//  JSON / Configuration Section
//
/////////////////////////////////////////////////////////

// Configuration Validations
void validateConfig() {
  // E1.31 Limits
  if (config.universe < 1)
    config.universe = 1;

  if (config.universe_limit > UNIVERSE_MAX || config.universe_limit < 1)
    config.universe_limit = UNIVERSE_MAX;

  if (config.channel_start < 1)
    config.channel_start = 1;
  else if (config.channel_start > config.universe_limit)
    config.channel_start = config.universe_limit;

  // Set default MQTT port if missing
  if (config.mqtt_port == 0)
    config.mqtt_port = MQTT_PORT;

  // Generate default MQTT topic if needed
  if (!config.mqtt_topic.length()) {
    char chipId[7] = { 0 };
    snprintf(chipId, sizeof(chipId), "%06x", ESP.getChipId());
    config.mqtt_topic = "diy/esps/" + String(chipId);
  }

#if defined(ESPS_MODE_PIXEL)
  // Set Mode
  //    config.devmode = DevMode::MPIXEL;
  config.devmode.MPIXEL = true;
  config.devmode.MSERIAL = false;

  // Generic channel limits for pixels
  if (config.channel_count % 3)
    config.channel_count = (config.channel_count / 3) * 3;

  if (config.channel_count > PIXEL_LIMIT * 3)
    config.channel_count = PIXEL_LIMIT * 3;
  else if (config.channel_count < 1)
    config.channel_count = 1;

  // GECE Limits
  if (config.pixel_type == PixelType::GECE) {
    config.pixel_color = PixelColor::RGB;
    if (config.channel_count > 63 * 3)
      config.channel_count = 63 * 3;
  }

  // Default gamma value
  if (config.gammaVal <= 0) {
    config.gammaVal = 2.2;
  }

  // Default brightness value
  if (config.briteVal <= 0) {
    config.briteVal = 1.0;
  }
#elif defined(ESPS_MODE_SERIAL)
  // Set Mode
  //    config.devmode = DevMode::MSERIAL;
  config.devmode.MPIXEL = false;
  config.devmode.MSERIAL = true;

  // Generic serial channel limits
  if (config.channel_count > RENARD_LIMIT)
    config.channel_count = RENARD_LIMIT;
  else if (config.channel_count < 1)
    config.channel_count = 1;

  if (config.serial_type == SerialType::DMX512 && config.channel_count > UNIVERSE_MAX)
    config.channel_count = UNIVERSE_MAX;

  // Baud rate check
  if (config.baudrate > BaudRate::BR_460800)
    config.baudrate = BaudRate::BR_460800;
  else if (config.baudrate < BaudRate::BR_38400)
    config.baudrate = BaudRate::BR_57600;
#endif
  
}
void initPixels();
void updateConfig() {
  // Validate first
  validateConfig();

  // Find the last universe we should listen for
  uint16_t span = config.channel_start + config.channel_count - 1;
  if (span % config.universe_limit)
    uniLast = config.universe + span / config.universe_limit;
  else
    uniLast = config.universe + span / config.universe_limit - 1;

  //if the universe changed sizes
  initE131();

  initPixels();

  
  LOG_PORT.print(F("- Listening for "));
  LOG_PORT.print(config.channel_count);
  LOG_PORT.print(F(" channels, from Universe "));
  LOG_PORT.print(config.universe);
  LOG_PORT.print(F(" to "));
  LOG_PORT.println(uniLast);

}

// Initialize for our pixel type
#if defined(ESPS_MODE_PIXEL)
void initPixels() {
  pixels.begin(config.pixel_type, config.pixel_color, config.channel_count / 3);
  LOG_PORT.printf("Set Gamma: %0.2f, GammaVal: %0.2f, Brightness: %0.2f\n",config.gamma,config.gammaVal,config.briteVal);
  pixels.setGamma(config.gamma);
  updateGammaTable(config.gammaVal, config.briteVal);
  effects.begin(&pixels, config.channel_count / 3);
}
#elif defined(ESPS_MODE_SERIAL)
void initPixels() {
  serial.begin(&SEROUT_PORT, config.serial_type, config.channel_count, config.baudrate);
}
#endif

// De-Serialize Network config
void dsNetworkConfig(JsonObject &json) {
  // Fallback to embedded ssid and passphrase if null in config
  config.ssid = json["network"]["ssid"].as<String>();
  if (!config.ssid.length())
    config.ssid = ssid;

  config.passphrase = json["network"]["passphrase"].as<String>();
  if (!config.passphrase.length())
    config.passphrase = passphrase;

  // Network
  for (int i = 0; i < 4; i++) {
    config.ip[i] = json["network"]["ip"][i];
    config.netmask[i] = json["network"]["netmask"][i];
    config.gateway[i] = json["network"]["gateway"][i];
  }
  config.dhcp = json["network"]["dhcp"];
  config.ap_fallback = json["network"]["ap_fallback"];

  // Generate default hostname if needed
  config.hostname = json["network"]["hostname"].as<String>();
  if (!config.hostname.length()) {
    char chipId[7] = { 0 };
    snprintf(chipId, sizeof(chipId), "%06x", ESP.getChipId());
    config.hostname = "esps-" + String(chipId);
  }
}

// De-serialize Device Config
void dsDeviceConfig(JsonObject &json) {
  // Device
  config.id = json["device"]["id"].as<String>();

  // E131
  config.universe = json["e131"]["universe"];
  config.universe_limit = json["e131"]["universe_limit"];
  config.channel_start = json["e131"]["channel_start"];
  config.channel_count = json["e131"]["channel_count"];
  config.multicast = json["e131"]["multicast"];

  // MQTT
  config.mqtt = json["mqtt"]["enabled"];
  config.mqtt_ip = json["mqtt"]["ip"].as<String>();
  config.mqtt_port = json["mqtt"]["port"];
  config.mqtt_user = json["mqtt"]["user"].as<String>();
  config.mqtt_password = json["mqtt"]["password"].as<String>();
  config.mqtt_topic = json["mqtt"]["topic"].as<String>();

#if defined(ESPS_MODE_PIXEL)
  // Pixel
  config.pixel_type = PixelType(static_cast<uint8_t>(json["pixel"]["type"]));
  config.pixel_color = PixelColor(static_cast<uint8_t>(json["pixel"]["color"]));
  config.gamma = json["pixel"]["gamma"];
  config.gammaVal = json["pixel"]["gammaVal"];
  config.briteVal = json["pixel"]["briteVal"];
#elif defined(ESPS_MODE_SERIAL)
  // Serial
  config.serial_type = SerialType(static_cast<uint8_t>(json["serial"]["type"]));
  config.baudrate = BaudRate(static_cast<uint32_t>(json["serial"]["baudrate"]));
#endif

}

// Load configugration JSON file
void loadConfig() {
  // Zeroize Config struct
  memset(&config, 0, sizeof(config));

  // Load CONFIG_FILE json. Create and init with defaults if not found
  File file = SPIFFS.open(CONFIG_FILE, "r");
  if (!file) {
    LOG_PORT.println(F("- No configuration file found."));
    config.ssid = ssid;
    config.passphrase = passphrase;
    saveConfig();
  } else {
    // Parse CONFIG_FILE json
    size_t size = file.size();
    if (size > CONFIG_MAX_SIZE) {
      LOG_PORT.println(F("*** Configuration File too large ***"));
      return;
    }

    std::unique_ptr<char[]> buf(new char[size]);
    file.readBytes(buf.get(), size);

    DynamicJsonBuffer jsonBuffer;
    JsonObject &json = jsonBuffer.parseObject(buf.get());
    if (!json.success()) {
      LOG_PORT.println(F("*** Configuration File Format Error ***"));
      return;
    }

    dsNetworkConfig(json);
    dsDeviceConfig(json);

    LOG_PORT.println(F("- Configuration loaded."));
  }

  // Validate it
  validateConfig();
}

// Serialize the current config into a JSON string
void serializeConfig(String &jsonString, bool pretty, bool creds) {
  // Create buffer and root object
  DynamicJsonBuffer jsonBuffer;
  JsonObject &json = jsonBuffer.createObject();

  // Device
  JsonObject &device = json.createNestedObject("device");
  device["id"] = config.id.c_str();
  device["mode"] = config.devmode.toInt();

  // Network
  JsonObject &network = json.createNestedObject("network");
  network["ssid"] = config.ssid.c_str();
  if (creds)
    network["passphrase"] = config.passphrase.c_str();
  network["hostname"] = config.hostname.c_str();
  JsonArray &ip = network.createNestedArray("ip");
  JsonArray &netmask = network.createNestedArray("netmask");
  JsonArray &gateway = network.createNestedArray("gateway");
  for (int i = 0; i < 4; i++) {
    ip.add(config.ip[i]);
    netmask.add(config.netmask[i]);
    gateway.add(config.gateway[i]);
  }
  network["dhcp"] = config.dhcp;
  network["ap_fallback"] = config.ap_fallback;

  // MQTT
  JsonObject &_mqtt = json.createNestedObject("mqtt");
  _mqtt["enabled"] = config.mqtt;
  _mqtt["ip"] = config.mqtt_ip.c_str();
  _mqtt["port"] = config.mqtt_port;
  _mqtt["user"] = config.mqtt_user.c_str();
  _mqtt["password"] = config.mqtt_password.c_str();
  _mqtt["topic"] = config.mqtt_topic.c_str();

  // E131
  JsonObject &e131 = json.createNestedObject("e131");
  e131["universe"] = config.universe;
  e131["universe_limit"] = config.universe_limit;
  e131["channel_start"] = config.channel_start;
  e131["channel_count"] = config.channel_count;
  e131["multicast"] = config.multicast;

#if defined(ESPS_MODE_PIXEL)
  // Pixel
  JsonObject &pixel = json.createNestedObject("pixel");
  pixel["type"] = static_cast<uint8_t>(config.pixel_type);
  pixel["color"] = static_cast<uint8_t>(config.pixel_color);
  pixel["gamma"] = config.gamma;
  pixel["gammaVal"] = config.gammaVal;
  pixel["briteVal"] = config.briteVal;

#elif defined(ESPS_MODE_SERIAL)
  // Serial
  JsonObject &serial = json.createNestedObject("serial");
  serial["type"] = static_cast<uint8_t>(config.serial_type);
  serial["baudrate"] = static_cast<uint32_t>(config.baudrate);
#endif

  if (pretty)
    json.prettyPrintTo(jsonString);
  else
    json.printTo(jsonString);
}

// Save configuration JSON file
void saveConfig() {
  // Update Config
  updateConfig();

  // Serialize Config
  String jsonString;
  serializeConfig(jsonString, true, true);

  // Save Config
  File file = SPIFFS.open(CONFIG_FILE, "w");
  if (!file) {
    LOG_PORT.println(F("*** Error creating configuration file ***"));
    return;
  } else {
    file.println(jsonString);
    LOG_PORT.println(F("* Configuration saved."));
  }
}

/////////////////////////////////////////////////////////
//
//  Set routines for Testing and MQTT
//
/////////////////////////////////////////////////////////

void setStatic(uint8_t r, uint8_t g, uint8_t b,uint8_t waitTime) {
  LOG_PORT.printf("setStatic(%d,%d,%d,%d)\n",r,g,b,waitTime);
  
  setStatic(r,g,b);
  effects.run();
  //sceneShow();
  delay(waitTime);
}

void setStatic(uint8_t r, uint8_t g, uint8_t b) {
  effects.setColor({r,g,b});
  effects.setEffect("Solid");
  LOG_PORT.printf("setStatic(%d,%d,%d)\n",r,g,b);
  /*uint16_t i = 0;
  while (i <= config.channel_count - 3) {
    pixels.setValue(i,r);
    setPixelValue(i++, r);
    setPixelValue(i++, g);
    setPixelValue(i++, b);
  }*/
}
/////////////////////////////////////////////////////////
//
//  Main Loop
//
/////////////////////////////////////////////////////////
void loop() {
  e131_packet_t packet;
  if (asAP && millis() - asAPLastColor > 500 ) {
    asAPLastColor = millis();
    if ( asAPColorOn ) {
      //setStatic(255, 255, 0);
    } else {
      //setStatic(0, 0, 0);
    }
    asAPColorOn = ! asAPColorOn;
  }

  // Reboot handler
  if (reboot) doReboot();

    // Render output for current data source
    switch (config.ds) {
        case DataSource::E131:
            // Parse a packet and update pixels
            if (!e131.isEmpty()) {
                e131.pull(&packet);
                uint16_t universe = htons(packet.universe);
                uint8_t *data = packet.property_values + 1;
                //LOG_PORT.print(universe);
                //LOG_PORT.println(packet.sequence_number);
                if ((universe >= config.universe) && (universe <= uniLast)) {
                    // Universe offset and sequence tracking
                    uint8_t uniOffset = (universe - config.universe);
                    if (packet.sequence_number != seqTracker[uniOffset]++) {
                        LOG_PORT.print(F("Sequence Error - expected: "));
                        LOG_PORT.print(seqTracker[uniOffset] - 1);
                        LOG_PORT.print(F(" actual: "));
                        LOG_PORT.print(packet.sequence_number);
                        LOG_PORT.print(F(" universe: "));
                        LOG_PORT.println(universe);
                        seqError[uniOffset]++;
                        seqTracker[uniOffset] = packet.sequence_number + 1;
                    }

                    // Offset the channels if required
                    uint16_t offset = 0;
                    offset = config.channel_start - 1;

                    // Find start of data based off the Universe
                    int16_t dataStart = uniOffset * config.universe_limit - offset;

                    // Calculate how much data we need for this buffer
                    uint16_t dataStop = config.channel_count;
                    uint16_t channels = htons(packet.property_value_count) - 1;
                    if (config.universe_limit < channels)
                        channels = config.universe_limit;
                    if ((dataStart + channels) < dataStop)
                        dataStop = dataStart + channels;

                    // Set the data
                    uint16_t buffloc = 0;

                    // ignore data from start of first Universe before channel_start
                    if (dataStart < 0) {
                        dataStart = 0;
                        buffloc = config.channel_start - 1;
                    }

                    for (int i = dataStart; i < dataStop; i++) {
    #if defined(ESPS_MODE_PIXEL)
                        pixels.setValue(i, data[buffloc]);
    #elif defined(ESPS_MODE_SERIAL)
                        serial.setValue(i, data[buffloc]);
    #endif
                        buffloc++;
                    }
                }
            }
            break;

        case DataSource::MQTT:
            effects.run();
            break;

        case DataSource::WEB:
            effects.run();
            break;
    }
}
// vim:sw=2:ts=2:et
