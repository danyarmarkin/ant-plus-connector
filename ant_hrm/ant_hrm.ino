/*
 * ANT+ Heart Rate Monitor receiver for nice!nano (nRF52840) with SoftDevice S340.
 *
 * Board: "nice!nano (S340 ANT+BLE)" (see arduino_s340/install.sh)
 * Channel 0 tracks the selected HRM strap (or the first one heard when "any" is selected),
 * channel 1 is a background scanning channel that lists every HRM strap nearby.
 * Prints the state to Serial (USB CDC) on every received data page (~4 Hz).
 *
 * Serial commands (newline terminated):
 *   pair <device number>   track only this strap (saved to flash)
 *   pair any               track the first strap heard (saved to flash)
 *   sensors                print the list of straps nearby now
 *
 * ANT+ HRM channel: network key = ANT+ key, RF 2457 MHz, device type 120, period 8070 (4.06 Hz).
 */
#include <bluefruit.h>
#include "ant_interface.h"
#include "ant_parameters.h"
#include <InternalFileSystem.h>

using namespace Adafruit_LittleFS_Namespace;

// Set to 1 to also dump every received data page as hex.
#define PRINT_RAW_PAGES   0

#define HRM_CHANNEL       0
#define ANTPLUS_NETWORK   0
#define HRM_DEVICE_TYPE   0x78    // 120 = Heart Rate
#define HRM_RF_FREQ       57      // 2457 MHz
#define HRM_PERIOD        8070    // 32768/8070 = 4.06 Hz
#define HRM_TRANS_TYPE    0       // 0 = wildcard
#define SCAN_CHANNEL      1       // background scanning channel
#define MAX_SENSORS       8
#define SENSOR_EXPIRE_MS  10000
#define SENSORS_PRINT_MS  2000
#define PAIR_FILE         "/hrm_pair.txt"

// ANT+ network key: not distributed with this repo (ANT+ Adopter agreement).
// Copy ant_plus_key.h.example to ant_plus_key.h and put the key there, see docs/03-firmware.md.
#if __has_include("ant_plus_key.h")
#include "ant_plus_key.h"
#else
#error "ant_plus_key.h not found: copy ant_plus_key.h.example to ant_plus_key.h and fill in the ANT+ network key"
#endif

static uint8_t antStackMem[ANT_ENABLE_GET_REQUIRED_SPACE(2, 0, MIN_ANT_TX_BURST_QUEUE_SIZE, 0)] __attribute__((aligned(4)));
static SemaphoreHandle_t antEventSem;

enum ChannelState { CH_SEARCHING, CH_TRACKING, CH_CLOSED };

struct HrmState {
  ChannelState state = CH_CLOSED;

  // Channel id of the paired strap (from extended data)
  bool     haveId = false;
  uint16_t deviceNumber = 0;
  uint8_t  deviceType = 0;
  uint8_t  transType = 0;
  int8_t   rssi = 0;
  bool     haveRssi = false;

  // Page 0/4 common fields
  bool     haveData = false;
  uint8_t  heartRate = 0;        // bpm
  uint8_t  beatCount = 0;        // rolls over at 256
  uint16_t beatEventTime = 0;    // 1/1024 s
  uint32_t lastRrMs = 0;
  uint32_t lastNewBeatMs = 0;    // millis() of last beat count change

  // Background pages
  bool     haveOpTime = false;   uint32_t opTimeSec = 0;
  bool     haveMfr = false;      uint8_t mfrId = 0;  uint16_t serialHi = 0;
  bool     haveProduct = false;  uint8_t hwVer = 0, swVer = 0, model = 0;
  bool     haveBattery = false;  uint8_t battPct = 0xFF; float battVolts = -1; uint8_t battStatus = 0;

  // Statistics
  uint32_t rxMessages = 0;
  uint32_t rxFails = 0;
  uint32_t lastRxMs = 0;
};

static HrmState hrm;
static uint16_t targetDeviceNumber = 0;  // 0 = wildcard, pair with any strap

// Straps heard by the background scanning channel
struct SensorEntry {
  uint16_t deviceNumber;
  uint8_t  transType;
  int8_t   rssi;
  uint8_t  heartRate;
  uint32_t lastSeenMs;
};
static SensorEntry sensors[MAX_SENSORS];
static uint8_t sensorCount = 0;
static uint32_t msgsThisSecond = 0;
static uint32_t msgRate = 0;           // messages received during the last full second
static bool newDataReceived = false;   // set on every data page, triggers a status line

static const char* batteryStatusStr(uint8_t s) {
  switch (s) {
    case 1: return "new";
    case 2: return "good";
    case 3: return "ok";
    case 4: return "low";
    case 5: return "critical";
    case 7: return "invalid";
    default: return "?";
  }
}

static bool check(uint32_t err, const char* what) {
  if (err == NRF_SUCCESS) return true;
  Serial.printf("ERROR: %s failed, err=0x%04lX\n", what, (unsigned long) err);
  return false;
}

static bool antHrmChannelOpen() {
  bool ok = true;
  ok &= check(sd_ant_channel_assign(HRM_CHANNEL, CHANNEL_TYPE_SLAVE, ANTPLUS_NETWORK, 0), "channel_assign");
  ok &= check(sd_ant_channel_id_set(HRM_CHANNEL, targetDeviceNumber, HRM_DEVICE_TYPE, HRM_TRANS_TYPE), "channel_id_set");
  ok &= check(sd_ant_channel_radio_freq_set(HRM_CHANNEL, HRM_RF_FREQ), "radio_freq_set");
  ok &= check(sd_ant_channel_period_set(HRM_CHANNEL, HRM_PERIOD), "period_set");
  ok &= check(sd_ant_channel_rx_search_timeout_set(HRM_CHANNEL, 0xFF), "search_timeout_set");  // 0xFF = search forever
  ok &= check(sd_ant_channel_open(HRM_CHANNEL), "channel_open");
  if (ok) hrm.state = CH_SEARCHING;
  return ok;
}

// Background scanning: receives pages from every HRM nearby without syncing to any of them.
static bool antScanChannelOpen() {
  bool ok = true;
  ok &= check(sd_ant_channel_assign(SCAN_CHANNEL, CHANNEL_TYPE_SLAVE, ANTPLUS_NETWORK, EXT_PARAM_ALWAYS_SEARCH), "scan_assign");
  ok &= check(sd_ant_channel_id_set(SCAN_CHANNEL, 0, HRM_DEVICE_TYPE, 0), "scan_id_set");
  ok &= check(sd_ant_channel_radio_freq_set(SCAN_CHANNEL, HRM_RF_FREQ), "scan_freq_set");
  ok &= check(sd_ant_channel_period_set(SCAN_CHANNEL, HRM_PERIOD), "scan_period_set");
  ok &= check(sd_ant_channel_rx_search_timeout_set(SCAN_CHANNEL, 0), "scan_hp_timeout_set");             // no high priority search
  ok &= check(sd_ant_channel_low_priority_rx_search_timeout_set(SCAN_CHANNEL, 0xFF), "scan_lp_timeout_set");  // search forever
  ok &= check(sd_ant_channel_open(SCAN_CHANNEL), "scan_open");
  return ok;
}

static void updateSensor(uint16_t devNum, uint8_t transType, int8_t rssi, uint8_t hr) {
  SensorEntry* e = nullptr;
  for (uint8_t i = 0; i < sensorCount; i++) {
    if (sensors[i].deviceNumber == devNum) e = &sensors[i];
  }
  if (!e) {
    if (sensorCount < MAX_SENSORS) {
      e = &sensors[sensorCount++];
    } else {  // replace the oldest entry
      e = &sensors[0];
      for (uint8_t i = 1; i < sensorCount; i++) {
        if (sensors[i].lastSeenMs < e->lastSeenMs) e = &sensors[i];
      }
    }
    e->deviceNumber = devNum;
  }
  e->transType = transType;
  e->rssi = rssi;
  e->heartRate = hr;
  e->lastSeenMs = millis();
}

static void expireSensors() {
  for (uint8_t i = 0; i < sensorCount;) {
    if (millis() - sensors[i].lastSeenMs > SENSOR_EXPIRE_MS) {
      sensors[i] = sensors[--sensorCount];
    } else {
      i++;
    }
  }
}

// One machine readable line: SENSORS target=<n|any> | <dev> rssi <dBm> hr <bpm> age <s>[ *] | ...
static void printSensors() {
  expireSensors();
  if (targetDeviceNumber) Serial.printf("SENSORS target=%u", targetDeviceNumber);
  else Serial.print("SENSORS target=any");
  for (uint8_t i = 0; i < sensorCount; i++) {
    const SensorEntry& e = sensors[i];
    bool paired = hrm.state == CH_TRACKING && hrm.haveId && hrm.deviceNumber == e.deviceNumber;
    Serial.printf(" | %u rssi %d hr %u age %.1f%s", e.deviceNumber, e.rssi, e.heartRate,
                  (millis() - e.lastSeenMs) / 1000.0f, paired ? " *" : "");
  }
  Serial.println();
}

static void savePairing() {
  InternalFS.remove(PAIR_FILE);
  File f(InternalFS);
  if (f.open(PAIR_FILE, FILE_O_WRITE)) {
    f.printf("%u\n", targetDeviceNumber);
    f.close();
  }
}

static void loadPairing() {
  File f(InternalFS);
  if (!f.open(PAIR_FILE, FILE_O_READ)) return;
  char buf[8] = {};
  f.read(buf, sizeof(buf) - 1);
  f.close();
  targetDeviceNumber = (uint16_t) atoi(buf);
}

static void resetHrmData() {
  ChannelState st = hrm.state;
  hrm = HrmState();
  hrm.state = st;
}

static uint8_t channelState(uint8_t ch) {
  uint8_t status = 0;
  sd_ant_channel_status_get(ch, &status);
  return status & STATUS_CHANNEL_STATE_MASK;
}

static void waitChannelClosed(uint8_t ch) {
  for (int i = 0; i < 200 && channelState(ch) >= STATUS_SEARCHING_CHANNEL; i++) delay(5);
}

// Re-pair channel 0. Both channels are closed while writing flash: the SoftDevice only writes
// flash in radio idle time, and a searching/scanning channel never leaves any.
// Channel 0 is reopened with the new id by the EVENT_CHANNEL_CLOSED handler.
static void selectSensor(uint16_t devNum) {
  targetDeviceNumber = devNum;
  resetHrmData();
  if (devNum) Serial.printf(">>> Pairing with strap #%u\n", devNum);
  else Serial.println(">>> Pairing with any strap");

  bool hrmWasOpen = channelState(HRM_CHANNEL) >= STATUS_SEARCHING_CHANNEL;
  if (hrmWasOpen) sd_ant_channel_close(HRM_CHANNEL);
  sd_ant_channel_close(SCAN_CHANNEL);
  waitChannelClosed(HRM_CHANNEL);
  waitChannelClosed(SCAN_CHANNEL);

  savePairing();

  check(sd_ant_channel_open(SCAN_CHANNEL), "scan_reopen");
  if (!hrmWasOpen) {
    if (channelState(HRM_CHANNEL) == STATUS_ASSIGNED_CHANNEL) sd_ant_channel_unassign(HRM_CHANNEL);
    antHrmChannelOpen();
  }
}

static void handleCommand(char* line) {
  char* cmd = strtok(line, " \t\r");
  if (!cmd) return;
  if (!strcmp(cmd, "pair")) {
    char* arg = strtok(nullptr, " \t\r");
    if (!arg || !strcmp(arg, "any") || !strcmp(arg, "0")) {
      selectSensor(0);
    } else {
      long n = atol(arg);
      if (n > 0 && n <= 65535) selectSensor((uint16_t) n);
      else Serial.println("usage: pair <device number 1..65535> | pair any");
    }
  } else if (!strcmp(cmd, "sensors")) {
    printSensors();
  } else {
    Serial.println("commands: pair <device number> | pair any | sensors");
  }
}

static void readSerialCommands() {
  static char buf[32];
  static uint8_t len = 0;
  while (Serial.available()) {
    char c = Serial.read();
    if (c == '\n' || c == '\r') {
      if (len) {
        buf[len] = 0;
        handleCommand(buf);
        len = 0;
      }
    } else if (len < sizeof(buf) - 1) {
      buf[len++] = c;
    }
  }
}

static void decodeHrmPage(const uint8_t* p) {
  uint8_t page = p[0] & 0x7F;  // bit 7 is the page change toggle

  // Bytes 4..7 are the same on every HRM page
  uint16_t evtTime = (uint16_t) (p[4] | (p[5] << 8));
  uint8_t  count = p[6];
  uint8_t  hr = p[7];

  if (hrm.haveData && count != hrm.beatCount) {
    // RR from consecutive beats (only exact when exactly one beat happened in between)
    if ((uint8_t) (count - hrm.beatCount) == 1) {
      hrm.lastRrMs = ((uint16_t) (evtTime - hrm.beatEventTime)) * 1000UL / 1024UL;
    }
    hrm.lastNewBeatMs = millis();
  }
  if (!hrm.haveData) hrm.lastNewBeatMs = millis();

  hrm.beatEventTime = evtTime;
  hrm.beatCount = count;
  hrm.heartRate = hr;
  hrm.haveData = true;

  switch (page) {
    case 1:  // Cumulative operating time, 2 s resolution
      hrm.opTimeSec = (uint32_t) (p[1] | (p[2] << 8) | ((uint32_t) p[3] << 16)) * 2;
      hrm.haveOpTime = true;
      break;
    case 2:  // Manufacturer information
      hrm.mfrId = p[1];
      hrm.serialHi = (uint16_t) (p[2] | (p[3] << 8));
      hrm.haveMfr = true;
      break;
    case 3:  // Product information
      hrm.hwVer = p[1];
      hrm.swVer = p[2];
      hrm.model = p[3];
      hrm.haveProduct = true;
      break;
    case 4: {  // Previous heart beat: exact RR interval
      uint16_t prevEvt = (uint16_t) (p[2] | (p[3] << 8));
      hrm.lastRrMs = ((uint16_t) (evtTime - prevEvt)) * 1000UL / 1024UL;
      break;
    }
    case 7:  // Battery status
      hrm.battPct = p[1];
      if ((p[3] & 0x0F) != 0x0F) hrm.battVolts = (p[3] & 0x0F) + p[2] / 256.0f;
      hrm.battStatus = (p[3] >> 4) & 0x07;
      hrm.haveBattery = true;
      break;
    default:
      break;
  }

#if PRINT_RAW_PAGES
  Serial.printf("  raw page %u: %02X %02X %02X %02X %02X %02X %02X %02X\n", page,
                p[0], p[1], p[2], p[3], p[4], p[5], p[6], p[7]);
#endif
}

static void decodeExtData(ANT_MESSAGE& m) {
  uint8_t flags = m.ANT_MESSAGE_ucExtMesgBF;
  const uint8_t* ext = m.ANT_MESSAGE_aucExtData;

  if (flags & ANT_EXT_MESG_BITFIELD_DEVICE_ID) {
    uint16_t devNum = (uint16_t) (ext[0] | (ext[1] << 8));
    if (!hrm.haveId || devNum != hrm.deviceNumber) {
      Serial.printf(">>> Paired with ANT+ HRM: device #%u, type %u, transmission type 0x%02X\n",
                    devNum, ext[2], ext[3]);
    }
    hrm.deviceNumber = devNum;
    hrm.deviceType = ext[2];
    hrm.transType = ext[3];
    hrm.haveId = true;
    ext += ANT_EXT_MESG_DEVICE_ID_FIELD_SIZE;
  }
  if (flags & ANT_EXT_MESG_BITFIELD_RSSI) {
    // ext[0] = measurement type, ext[1] = RSSI in dBm (signed), ext[2] = threshold
    hrm.rssi = (int8_t) ext[1];
    hrm.haveRssi = true;
  }
}

// Note: ANT does not deliver a strap tracked by channel 0 to the background scanning channel,
// so the paired strap is added to the sensor list from channel 0 data (see handleAntEvent).
static void handleScanEvent(uint8_t event, ANT_MESSAGE& m) {
  if (event != EVENT_RX) return;
  uint8_t flags = m.ANT_MESSAGE_ucExtMesgBF;
  if (!(flags & ANT_EXT_MESG_BITFIELD_DEVICE_ID)) return;
  const uint8_t* ext = m.ANT_MESSAGE_aucExtData;
  uint16_t devNum = (uint16_t) (ext[0] | (ext[1] << 8));
  uint8_t transType = ext[3];
  int8_t rssi = 0;
  if (flags & ANT_EXT_MESG_BITFIELD_RSSI) rssi = (int8_t) ext[ANT_EXT_MESG_DEVICE_ID_FIELD_SIZE + 1];
  updateSensor(devNum, transType, rssi, m.ANT_MESSAGE_aucPayload[7]);
}

static void handleAntEvent(uint8_t channel, uint8_t event, ANT_MESSAGE& m) {
  if (channel == SCAN_CHANNEL) {
    handleScanEvent(event, m);
    return;
  }
  if (channel != HRM_CHANNEL) return;

  switch (event) {
    case EVENT_RX:
      if (m.ANT_MESSAGE_ucMesgID == MESG_BROADCAST_DATA_ID ||
          m.ANT_MESSAGE_ucMesgID == MESG_ACKNOWLEDGED_DATA_ID ||
          m.ANT_MESSAGE_ucMesgID == MESG_BURST_DATA_ID) {
        if (hrm.state != CH_TRACKING) Serial.println(">>> Sensor found, tracking");
        hrm.state = CH_TRACKING;
        hrm.rxMessages++;
        hrm.lastRxMs = millis();
        msgsThisSecond++;
        newDataReceived = true;
        decodeExtData(m);
        decodeHrmPage(m.ANT_MESSAGE_aucPayload);
        if (hrm.haveId) updateSensor(hrm.deviceNumber, hrm.transType, hrm.rssi, hrm.heartRate);
      }
      break;

    case EVENT_RX_FAIL:
      hrm.rxFails++;
      break;

    case EVENT_RX_FAIL_GO_TO_SEARCH:
      Serial.println(">>> Signal lost, searching again...");
      hrm.state = CH_SEARCHING;
      break;

    case EVENT_RX_SEARCH_TIMEOUT:
      Serial.println(">>> Search timeout");
      break;

    case EVENT_CHANNEL_CLOSED:
      Serial.println(">>> Channel closed, reopening");
      hrm.state = CH_CLOSED;
      sd_ant_channel_unassign(HRM_CHANNEL);
      antHrmChannelOpen();
      break;

    case EVENT_QUE_OVERFLOW:
      Serial.println(">>> ANT event queue overflow");
      break;

    default:
      break;
  }
}

static void processAntEvents() {
  uint8_t channel, event;
  ANT_MESSAGE m;
  while (sd_ant_event_get(&channel, &event, m.ANT_MESSAGE_aucMessage) == NRF_SUCCESS) {
    handleAntEvent(channel, event, m);
  }
}

// Blinks the LED once per heart beat at the current heart rate; LED stays off without a valid HR.
static void heartbeatLedTask(void* arg) {
  (void) arg;
  const uint32_t FLASH_MS = 80;
  pinMode(LED_BUILTIN, OUTPUT);
  digitalWrite(LED_BUILTIN, !LED_STATE_ON);

  while (1) {
    uint8_t hr = hrm.heartRate;
    if (hrm.state != CH_TRACKING || hr == 0) {
      digitalWrite(LED_BUILTIN, !LED_STATE_ON);
      delay(100);
      continue;
    }
    uint32_t periodMs = 60000UL / hr;
    digitalWrite(LED_BUILTIN, LED_STATE_ON);
    delay(FLASH_MS);
    digitalWrite(LED_BUILTIN, !LED_STATE_ON);
    delay(periodMs - FLASH_MS);
  }
}

static void printStatus() {
  float t = millis() / 1000.0f;

  if (hrm.state != CH_TRACKING) {
    Serial.printf("[%7.1fs] %s for ANT+ heart rate strap", t, hrm.state == CH_SEARCHING ? "SEARCHING" : "CLOSED");
    if (targetDeviceNumber) Serial.printf(" #%u", targetDeviceNumber);
    Serial.println("... (put it on / moisten the electrodes)");
    return;
  }

  if (hrm.heartRate == 0) {
    Serial.printf("[%7.1fs] HR --- (no skin contact)", t);  // 0 = invalid computed heart rate
  } else {
    Serial.printf("[%7.1fs] HR %3u bpm", t, hrm.heartRate);
  }
  Serial.printf(" | beats %3u | RR %4lu ms", hrm.beatCount, (unsigned long) hrm.lastRrMs);
  if (hrm.haveId)   Serial.printf(" | ID %u", hrm.deviceNumber);
  if (hrm.haveRssi) Serial.printf(" | RSSI %d dBm", hrm.rssi);
  if (hrm.haveBattery) {
    if (hrm.battPct <= 100) Serial.printf(" | batt %u%%", hrm.battPct);
    if (hrm.battVolts >= 0) Serial.printf(" %.2fV", hrm.battVolts);
    Serial.printf(" (%s)", batteryStatusStr(hrm.battStatus));
  }
  Serial.printf(" | %lu msg/s, %lu missed", (unsigned long) msgRate, (unsigned long) hrm.rxFails);
  if (hrm.heartRate != 0 && millis() - hrm.lastNewBeatMs > 5000) Serial.print(" | NO NEW BEATS >5s");
  Serial.println();

  static uint32_t lastInfoMs = 0;
  if ((hrm.haveMfr || hrm.haveProduct || hrm.haveOpTime) && (lastInfoMs == 0 || millis() - lastInfoMs > 30000)) {
    lastInfoMs = millis();
    Serial.print("           info:");
    if (hrm.haveMfr)     Serial.printf(" manufacturer %u, serial %u", hrm.mfrId, ((uint32_t) hrm.serialHi << 16) | hrm.deviceNumber);
    if (hrm.haveProduct) Serial.printf(" | model %u, hw %u, sw %u", hrm.model, hrm.hwVer, hrm.swVer);
    if (hrm.haveOpTime)  Serial.printf(" | operating time %lu h %lu min", (unsigned long) (hrm.opTimeSec / 3600), (unsigned long) (hrm.opTimeSec / 60 % 60));
    Serial.println();
  }
}

void setup() {
  Serial.begin(115200);
  uint32_t start = millis();
  while (!Serial && millis() - start < 5000) delay(10);  // wait up to 5 s for the serial monitor

  Serial.println();
  Serial.println("=== ANT+ Heart Rate receiver (nice!nano, SoftDevice S340) ===");

  // Bluefruit enables the S340 SoftDevice (with ANT_LICENSE_KEY) and keeps USB working under it.
  Bluefruit.autoConnLed(false);
  if (!Bluefruit.begin()) {
    Serial.println("ERROR: SoftDevice enable failed");
    while (1) delay(1000);
  }

  // SD event IRQ is owned by Bluefruit; it signals this semaphore for ANT events.
  antEventSem = xSemaphoreCreateBinary();
  Bluefruit.setMultiprotocolSemaphore(antEventSem);

  ANT_ENABLE antCfg = {};
  antCfg.ucTotalNumberOfChannels = 2;
  antCfg.ucNumberOfEncryptedChannels = 0;
  antCfg.usNumberOfEvents = 0;  // use internal event queue
  antCfg.pucMemoryBlockStartLocation = antStackMem;
  antCfg.usMemoryBlockByteSize = sizeof(antStackMem);
  if (!check(sd_ant_enable(&antCfg), "sd_ant_enable")) while (1) delay(1000);

  check(sd_ant_network_address_set(ANTPLUS_NETWORK, ANT_PLUS_NETWORK_KEY), "network_key_set");
  // Append device id and RSSI to every received message
  check(sd_ant_lib_config_set(ANT_LIB_CONFIG_MESG_OUT_INC_DEVICE_ID | ANT_LIB_CONFIG_MESG_OUT_INC_RSSI), "lib_config_set");

  InternalFS.begin();
  loadPairing();
  if (targetDeviceNumber) Serial.printf("Saved pairing: strap #%u (type \"pair any\" to reset)\n", targetDeviceNumber);

  if (antHrmChannelOpen()) Serial.println("ANT+ HRM channel open, searching...");
  if (antScanChannelOpen()) Serial.println("ANT+ background scanning started");

  xTaskCreate(heartbeatLedTask, "hb_led", 256, NULL, TASK_PRIO_LOW, NULL);
}

void loop() {
  // Wake up on SoftDevice events, at least every 100 ms
  xSemaphoreTake(antEventSem, pdMS_TO_TICKS(100));
  processAntEvents();
  readSerialCommands();

  static uint32_t lastSensorsMs = 0;
  if (millis() - lastSensorsMs >= SENSORS_PRINT_MS) {
    lastSensorsMs = millis();
    printSensors();
  }

  static uint32_t lastRateMs = 0;
  if (millis() - lastRateMs >= 1000) {
    lastRateMs = millis();
    msgRate = msgsThisSecond;
    msgsThisSecond = 0;
  }

  // Print on every received data page (~4 Hz, the strap's transmit rate);
  // while searching / without data -- once per second.
  static uint32_t lastPrintMs = 0;
  if (newDataReceived || millis() - lastPrintMs >= 1000) {
    newDataReceived = false;
    lastPrintMs = millis();
    printStatus();
  }
}
