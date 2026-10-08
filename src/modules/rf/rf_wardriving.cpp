/**
 * @file rf_wardriving.cpp
 * @brief Sub-GHz (CC1101) Wardriving / War-Walk for NM-RF-HAT + CYD
 *
 * Receive-only. See rf_wardriving.h for the overview.
 *
 * Hardware note (NM-RF-HAT): set DIP switch position 1 (CC1101) ON and connect
 * the GPS module to the GNSS expansion interface. The CC1101 lives on the SPI
 * bus (GDO0=22, SS=27 on the CYD preset) while the GPS uses a separate UART
 * (RX=1 / TX=3 on this hat), so the two can run at the same time.
 */

#include "rf_wardriving.h"
#include "core/display.h"
#include "core/mykeyboard.h"      // check(), EscPress
#include "core/sd_functions.h"    // getFsStorage()
#include "core/type_convertion.h" // decimalToHexString()
#include "current_year.h"
#include "modules/rf/rf_utils.h" // initRfModule/deinitRfModule + ELECHOUSE_cc1101

#define RF_LISTEN_WINDOW_MS 1000 // refresh banner/GPS roughly once per second
#define RF_MIN_RAW_TRANSITIONS 20

RFWardriving::RFWardriving(float freq) {
    frequency = (freq > 0) ? freq : bruceConfigPins.rfFreq;
    if (frequency <= 0) frequency = 433.92f;
    setup();
}

RFWardriving::~RFWardriving() {
    if (gpsConnected) end();
    stop_rf();
    ioExpander.turnPinOnOff(IO_EXP_GPS, LOW);
#ifdef USE_BOOST /// ENABLE 5V OUTPUT
    PPM.disableOTG();
#endif
}

void RFWardriving::setup() {
    signalCount = 0;
    ioExpander.turnPinOnOff(IO_EXP_GPS, HIGH);
#ifdef USE_BOOST /// ENABLE 5V OUTPUT
    PPM.enableOTG();
#endif

    drawMainBorderWithTitle("RF Wardriving");
    padprintln("");
    padprintln("Freq: " + String(frequency, 2) + " MHz");
    padprintln("Initializing...");

    if (!begin_gps()) return;
    if (!start_rf()) return end();

    sessionStartMs = millis();
    vTaskDelay(500 / portTICK_PERIOD_MS);
    return loop();
}

// ---------------------------------------------------------------------------
// GPS
// ---------------------------------------------------------------------------
bool RFWardriving::begin_gps() {
    releasePins();
    pinMode(bruceConfigPins.gps_bus.rx, INPUT);
    GPSserial.begin(
        bruceConfigPins.gpsBaudrate, SERIAL_8N1, bruceConfigPins.gps_bus.rx, bruceConfigPins.gps_bus.tx
    );

    int count = 0;
    padprintln("Waiting for GPS data");
    while (GPSserial.available() <= 0) {
        if (check(EscPress)) {
            end();
            return false;
        }
        displayTextLine("Waiting GPS: " + String(count) + "s");
        count++;
        vTaskDelay(1000 / portTICK_PERIOD_MS);
    }

    gpsConnected = true;
    return true;
}

void RFWardriving::end() {
    stop_rf();
    GPSserial.end();
    restorePins();
    returnToMenu = true;
    gpsConnected = false;
}

void RFWardriving::feed_gps() {
    while (GPSserial.available() > 0) gps.encode(GPSserial.read());

    if (gps.location.isUpdated() && gps.location.isValid()) set_position();

    if (filename == "" && gps.date.isValid() && gps.date.year() >= CURRENT_YEAR &&
        gps.date.year() < CURRENT_YEAR + 5)
        create_filename();
}

void RFWardriving::set_position() {
    double lat = gps.location.lat();
    double lng = gps.location.lng();

    if (initial_position_set) distance += gps.distanceBetween(cur_lat, cur_lng, lat, lng);
    else initial_position_set = true;

    cur_lat = lat;
    cur_lng = lng;
}

void RFWardriving::create_filename() {
    char timestamp[20];
    sprintf(
        timestamp,
        "%02d%02d%02d_%02d%02d%02d",
        gps.date.year() % 100,
        gps.date.month() % 100,
        gps.date.day() % 100,
        gps.time.hour() % 100,
        gps.time.minute() % 100,
        gps.time.second() % 100
    );
    filename = String(timestamp) + "_rf_wardriving.csv";
}

// ---------------------------------------------------------------------------
// RF
// ---------------------------------------------------------------------------
bool RFWardriving::start_rf() {
    if (rfStarted) return true;
    if (!initRfModule("rx", frequency)) {
        displayError("CC1101 init failed", true);
        return false;
    }
    if (!rx.begin()) {
        deinitRfModule();
        displayError("RF RX begin failed", true);
        return false;
    }
    rfStarted = true;
    return true;
}

void RFWardriving::stop_rf() {
    if (!rfStarted) return;
    rx.end();
    deinitRfModule();
    rfStarted = false;
}

void RFWardriving::log_signal(RfCodes &code, int rssi, uint64_t dedupKey) {
    // De-dup: skip codes we have already logged this session.
    if (dedupKey != 0) {
        if (seenKeys.find(dedupKey) != seenKeys.end()) return;
        seenKeys.insert(dedupKey);
    }

    FS *fs;
    if (!getFsStorage(fs)) {
        displayError("Storage setup error", true);
        returnToMenu = true;
        return;
    }

    if (filename == "") create_filename();
    if (filename == "") filename = "nofix_rf_wardriving.csv"; // no GPS date yet

    if (!(*fs).exists("/BruceRFWardriving")) (*fs).mkdir("/BruceRFWardriving");

    String path = "/BruceRFWardriving/" + filename;
    bool is_new_file = !(*fs).exists(path);
    File file = (*fs).open(path, is_new_file ? FILE_WRITE : FILE_APPEND);
    if (!file) {
        displayError("Failed to open log file", true);
        returnToMenu = true;
        return;
    }

    if (is_new_file) {
        file.println(
            String("RFWardriving-1.0,appRelease=v") + String(BRUCE_VERSION) + ",device=NM-RF-HAT/CYD"
        );
        file.println("Frequency_MHz,Protocol,Preset,KeyHex,Bit,RSSI,FirstSeen,Latitude,Longitude,"
                     "Altitude_m,HDOP");
    }

    char keyHex[32] = {0};
    decimalToHexString(code.key, keyHex);

    char latStr[24] = "";
    char lngStr[24] = "";
    if (gps.location.isValid()) {
        snprintf(latStr, sizeof(latStr), "%.6f", gps.location.lat());
        snprintf(lngStr, sizeof(lngStr), "%.6f", gps.location.lng());
    }

    char buffer[320];
    snprintf(
        buffer,
        sizeof(buffer),
        "%.2f,%s,%s,%s,%d,%d,%04d-%02d-%02d %02d:%02d:%02d,%s,%s,%.1f,%.2f\n",
        frequency,
        (code.protocol == "" ? "RcSwitch" : code.protocol.c_str()),
        (code.preset == "" ? "Ook270Async" : code.preset.c_str()),
        keyHex,
        code.Bit,
        rssi,
        gps.date.year(),
        gps.date.month(),
        gps.date.day(),
        gps.time.hour(),
        gps.time.minute(),
        gps.time.second(),
        latStr,
        lngStr,
        gps.altitude.meters(),
        gps.hdop.hdop() * 1.0
    );
    file.print(buffer);
    file.close();

    signalCount++;
}

// ---------------------------------------------------------------------------
// Main loop
// ---------------------------------------------------------------------------
void RFWardriving::loop() {
    returnToMenu = false;

    while (1) {
        display_banner();

        unsigned long windowStart = millis();
        while (millis() - windowStart < RF_LISTEN_WINDOW_MS) {
            if (check(EscPress) || returnToMenu) return end();

            feed_gps();

            // Live RSSI meter
            lastRssi = ELECHOUSE_cc1101.getRssi();
            draw_rssi_bar(lastRssi);

            // Poll for a captured burst
            std::vector<int> durations;
            if (rx.poll(durations)) {
                RfCodes code;
                bool decoded = rf_decode_ook(durations, code);

                String rawData;
                bool hasCrc = false;
                uint64_t crc = 0;
                std::vector<int> indexed;
                int rawBits = 0, rawTe = 0;
                int transitions = rf_build_raw(durations, rawData, hasCrc, crc, indexed, rawBits, rawTe);

                if (decoded) {
                    code.frequency = (uint32_t)(frequency * 1000000);
                    code.data = rawData;
                    uint64_t dedup = code.key ? code.key : crc;
                    log_signal(code, lastRssi, dedup);
                    display_banner();
                } else if (transitions > RF_MIN_RAW_TRANSITIONS) {
                    code.frequency = (uint32_t)(frequency * 1000000);
                    code.protocol = "RAW";
                    code.preset = "Ook270Async";
                    code.te = rawTe;
                    code.data = rawData;
                    log_signal(code, lastRssi, crc);
                    display_banner();
                }
            }

            vTaskDelay(20 / portTICK_PERIOD_MS);
        }
    }
}

// ---------------------------------------------------------------------------
// Display
// ---------------------------------------------------------------------------
void RFWardriving::display_banner() {
    drawMainBorderWithTitle("RF Wardriving");

    padprintln("");
    padprintln("Freq: " + String(frequency, 2) + " MHz");
    if (filename != "") padprintln("File: " + filename.substring(0, filename.length() - 4));
    padprintln("Signals: " + String(signalCount) + "   Sat: " + String(gps.satellites.value()));

    uint32_t elapsedSeconds = (millis() - sessionStartMs) / 1000;
    uint32_t hours = elapsedSeconds / 3600;
    uint32_t minutes = (elapsedSeconds / 60) % 60;
    uint32_t seconds = elapsedSeconds % 60;
    padprintf("Distance: %.2fkm  ET: %02lu:%02lu:%02lu\n", distance / 1000, hours, minutes, seconds);
}

void RFWardriving::draw_rssi_bar(int rssi) {
    int barX = 16;
    int barW = tft.width() - 32;
    int barH = 16;
    int barY = tft.height() - 44;
    if (barW < 20) return;

    // Map RSSI (-110 dBm .. -20 dBm) to bar fill width.
    int r = rssi;
    if (r < -110) r = -110;
    if (r > -20) r = -20;
    int fill = (int)((float)(r + 110) / 90.0f * barW);
    if (fill < 0) fill = 0;
    if (fill > barW) fill = barW;

    tft.setTextSize(FP);
    tft.setTextColor(bruceConfig.priColor, bruceConfig.bgColor);
    tft.setCursor(barX, barY - 12);
    char lbl[24];
    snprintf(lbl, sizeof(lbl), "RSSI: %4d dBm ", rssi);
    tft.print(lbl);

    // Frame + fill
    tft.drawRect(barX, barY, barW, barH, bruceConfig.priColor);
    tft.fillRect(barX + 1, barY + 1, barW - 2, barH - 2, bruceConfig.bgColor);
    if (fill > 2) tft.fillRect(barX + 1, barY + 1, fill - 2, barH - 2, bruceConfig.secColor);
}

// ---------------------------------------------------------------------------
// GPS RX pin conflict handling (mirrors Wardriving)
// ---------------------------------------------------------------------------
void RFWardriving::releasePins() {
    rxPinReleased = false;
    if (bruceConfigPins.CC1101_bus.checkConflict(bruceConfigPins.gps_bus.rx) ||
        bruceConfigPins.NRF24_bus.checkConflict(bruceConfigPins.gps_bus.rx) ||
#if !defined(LITE_VERSION)
        bruceConfigPins.W5500_bus.checkConflict(bruceConfigPins.gps_bus.rx) ||
        bruceConfigPins.LoRa_bus.checkConflict(bruceConfigPins.gps_bus.rx) ||
#endif
        bruceConfigPins.SDCARD_bus.checkConflict(bruceConfigPins.gps_bus.rx)) {
        pinMode(bruceConfigPins.gps_bus.rx, INPUT);
        rxPinReleased = true;
    }
}

void RFWardriving::restorePins() {
    if (!rxPinReleased) return;
    if (bruceConfigPins.CC1101_bus.checkConflict(bruceConfigPins.gps_bus.rx) ||
        bruceConfigPins.NRF24_bus.checkConflict(bruceConfigPins.gps_bus.rx) ||
#if !defined(LITE_VERSION)
        bruceConfigPins.W5500_bus.checkConflict(bruceConfigPins.gps_bus.rx) ||
        bruceConfigPins.LoRa_bus.checkConflict(bruceConfigPins.gps_bus.rx) ||
#endif
        bruceConfigPins.SDCARD_bus.checkConflict(bruceConfigPins.gps_bus.rx)) {
        pinMode(bruceConfigPins.gps_bus.rx, OUTPUT);
        if (bruceConfigPins.gps_bus.rx == bruceConfigPins.CC1101_bus.cs ||
            bruceConfigPins.gps_bus.rx == bruceConfigPins.NRF24_bus.cs ||
#if !defined(LITE_VERSION)
            bruceConfigPins.gps_bus.rx == bruceConfigPins.W5500_bus.cs ||
#endif
            bruceConfigPins.gps_bus.rx == bruceConfigPins.SDCARD_bus.cs) {
            digitalWrite(bruceConfigPins.gps_bus.rx, HIGH);
        } else {
            digitalWrite(bruceConfigPins.gps_bus.rx, LOW);
        }
    }
    rxPinReleased = false;
}
