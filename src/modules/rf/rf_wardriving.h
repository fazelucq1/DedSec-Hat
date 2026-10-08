/**
 * @file rf_wardriving.h
 * @brief Sub-GHz (CC1101) Wardriving / War-Walk for NM-RF-HAT + CYD
 *
 * Captures OOK/ASK sub-GHz signals with the CC1101 while moving and geo-tags
 * each capture (frequency, protocol, key, RSSI) with the current GPS position,
 * logging everything to a CSV on the TF card. Receive-only: it never transmits.
 *
 * This complements Bruce's existing WiFi/BLE wardriving (which has no sub-GHz
 * awareness) by reusing the RF RX/decoder stack and the GPS stack together.
 */

#ifndef __RF_WARDRIVING_H__
#define __RF_WARDRIVING_H__

#include "modules/rf/protocols/rf_decoder.h" // RfRxSession, rf_decode_ook, rf_build_raw
#include "modules/rf/structs.h"              // RfCodes
#include <TinyGPS++.h>
#include <globals.h>
#include <set>

class RFWardriving {
public:
    // frequency in MHz; 0 -> use the configured default (bruceConfigPins.rfFreq)
    RFWardriving(float frequency = 0);
    ~RFWardriving();

    void setup();
    void loop();

private:
    // ---- GPS state (mirrors Wardriving) ----
    bool date_time_updated = false;
    bool initial_position_set = false;
    double cur_lat = 0;
    double cur_lng = 0;
    double distance = 0;
    uint32_t sessionStartMs = 0;
    String filename = "";
    TinyGPSPlus gps;
    HardwareSerial GPSserial = HardwareSerial(2); // UART2, like Wardriving
    bool rxPinReleased = false;

    // ---- RF state ----
    float frequency = 433.92f;
    bool rfStarted = false;
    RfRxSession rx;
    std::set<uint64_t> seenKeys; // de-dup already logged codes
    int signalCount = 0;
    int lastRssi = -127;

    // ---- GPS helpers (mirror Wardriving) ----
    bool begin_gps(void);
    void end(void);
    void releasePins(void);
    void restorePins(void);
    void set_position(void);
    void feed_gps(void);
    void create_filename(void);

    // ---- RF helpers ----
    bool start_rf(void);
    void stop_rf(void);
    void log_signal(RfCodes &code, int rssi, uint64_t dedupKey);

    // ---- Display ----
    void display_banner(void);
    void draw_rssi_bar(int rssi);
};

#endif // __RF_WARDRIVING_H__
