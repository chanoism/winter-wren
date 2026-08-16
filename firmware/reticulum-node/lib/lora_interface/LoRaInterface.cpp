#include "LoRaInterface.h"

#include <microReticulum/Log.h>
#include <microReticulum/Utilities/OS.h>

#include <memory>

// ---------------------------------------------------------------------------
// Board-specific pin definitions
//
// Vendored from attermann/microReticulum's examples/common/lora_interface/
// LoRaInterface.cpp (real reference source). The BOARD_TBEAM,
// BOARD_LORA32_V21, BOARD_RAK4631, BOARD_HELTEC_V3, and BOARD_HELTEC_V4
// branches below are unmodified from the original -- kept for reference in
// case you switch boards later (e.g. onto the Wio-SX1262 kit, which is an
// SX1262 like the RAK4631/Heltec branches, though its own pins would still
// need to be added as another branch).
//
// ADDED for this project: BOARD_CUSTOM_WROOM_SX1276, matching your wiring
// diagram exactly (NSS5/SCK18/MOSI23/MISO19/RST14/DIO0 26, DIO1 not wired --
// fine, since loop() below polls the IRQ status register over SPI rather
// than needing a hardware interrupt pin).
// ---------------------------------------------------------------------------

#if defined(BOARD_TBEAM) || defined(BOARD_LORA32_V21)
// LILYGO T-Beam V1.X / LoRa32 V2.1 — SX1276
#define RADIO_SCLK_PIN               5
#define RADIO_MISO_PIN              19
#define RADIO_MOSI_PIN              27
#define RADIO_CS_PIN                18
#define RADIO_DIO0_PIN              26
#define RADIO_RST_PIN               23
#define RADIO_DIO1_PIN              33

#elif defined(BOARD_CUSTOM_WROOM_SX1276)
// Plain ESP32 WROOM-32 + SX1276 module, hand-wired per your diagram.
// DIO1 intentionally not wired -- GPIO27 is used for the buzzer instead
// in this project, and the polling-based checkIrq() approach below
// doesn't require a hardware interrupt line.
#define RADIO_SCLK_PIN              18
#define RADIO_MISO_PIN              19
#define RADIO_MOSI_PIN              23
#define RADIO_CS_PIN                 5
#define RADIO_DIO0_PIN              26
#define RADIO_RST_PIN               14

#elif defined(BOARD_RAK4631)
// RAK4631 (WisCore RAK4630) — SX1262
#define RADIO_SCLK_PIN              43
#define RADIO_MISO_PIN              45
#define RADIO_MOSI_PIN              44
#define RADIO_CS_PIN                42
#define RADIO_DIO1_PIN              47
#define RADIO_RST_PIN               38
#define RADIO_BUSY_PIN              46

#elif defined(BOARD_HELTEC_V3)
// Heltec WiFi LoRa 32 V3 — ESP32-S3 + SX1262
#define RADIO_SCLK_PIN               9
#define RADIO_MISO_PIN              11
#define RADIO_MOSI_PIN              10
#define RADIO_CS_PIN                 8
#define RADIO_DIO1_PIN              14
#define RADIO_RST_PIN               12
#define RADIO_BUSY_PIN              13

#elif defined(BOARD_HELTEC_V4)
// Heltec WiFi LoRa 32 V4 — ESP32-S3R2 + SX1262 + external FEM
#define RADIO_SCLK_PIN               9
#define RADIO_MISO_PIN              11
#define RADIO_MOSI_PIN              10
#define RADIO_CS_PIN                 8
#define RADIO_DIO1_PIN              14
#define RADIO_RST_PIN               12
#define RADIO_BUSY_PIN              13
#define RADIO_VFEM_EN               7
#define RADIO_FEM_CE                2
#define RADIO_PA_MODE              46

#endif

using namespace RNS;

static inline bool    isSplitPacket(uint8_t h)  { return (h & LoRaInterface::HEADER_SPLIT)   != 0; }
static inline uint8_t packetSequence(uint8_t h) { return  h & LoRaInterface::HEADER_SEQ_MASK;      }

LoRaInterface::LoRaInterface(const char* name /*= "LoRaInterface"*/) : RNS::InterfaceImpl(name) {
	_IN = true;
	_OUT = true;
	_bitrate = (double)spreading * ( (4.0/coding) / (pow(2, spreading)/bandwidth) ) * 1000.0;
	_HW_MTU = 508;
}

/*virtual*/ LoRaInterface::~LoRaInterface() {
	stop();
}

bool LoRaInterface::start() {
	_online = false;
	INFO("LoRa initializing...");

#ifdef ARDUINO

#if defined(BOARD_TBEAM) || defined(BOARD_LORA32_V21)
	SPI.begin(RADIO_SCLK_PIN, RADIO_MISO_PIN, RADIO_MOSI_PIN);
	_module = new Module(RADIO_CS_PIN, RADIO_DIO0_PIN, RADIO_RST_PIN, RADIO_DIO1_PIN, SPI);
	SX1276* chip = new SX1276(_module);
	_radio = chip;
	int state = chip->begin(frequency, bandwidth, spreading, coding,
	                        RADIOLIB_SX127X_SYNC_WORD, power, 20, 0);
	if (state == RADIOLIB_ERR_NONE) state = chip->setCRC(true);

#elif defined(BOARD_CUSTOM_WROOM_SX1276)
	// Bare WROOM-32 + SX1276, per your wiring diagram. DIO1 passed as
	// RADIOLIB_NC since it isn't wired -- loop() below polls the IRQ
	// status register over SPI instead of needing a hardware interrupt.
	SPI.begin(RADIO_SCLK_PIN, RADIO_MISO_PIN, RADIO_MOSI_PIN);
	_module = new Module(RADIO_CS_PIN, RADIO_DIO0_PIN, RADIO_RST_PIN, RADIOLIB_NC, SPI);
	SX1276* chip = new SX1276(_module);
	_radio = chip;
	int state = chip->begin(frequency, bandwidth, spreading, coding,
	                        RADIOLIB_SX127X_SYNC_WORD, power, 20, 0);
	// Same CRC fix as the T-Beam/LoRa32 branch above: real RNodes enable
	// CRC unconditionally, and RadioLib's SX127x::begin() doesn't touch
	// it by default. Enable explicitly so this interoperates.
	if (state == RADIOLIB_ERR_NONE) state = chip->setCRC(true);

#elif defined(BOARD_RAK4631)
	SPI.setPins(RADIO_MISO_PIN, RADIO_SCLK_PIN, RADIO_MOSI_PIN);
	SPI.begin();
	_module = new Module(RADIO_CS_PIN, RADIO_DIO1_PIN, RADIO_RST_PIN, RADIO_BUSY_PIN, SPI);
	SX1262* chip = new SX1262(_module);
	_radio = chip;
	chip->setDio2AsRfSwitch(true);
	int state = chip->begin(frequency, bandwidth, spreading, coding,
	                        RADIOLIB_SX126X_SYNC_WORD_PRIVATE, power, 20, 1.6, false);

#elif defined(BOARD_HELTEC_V3)
	SPI.begin(RADIO_SCLK_PIN, RADIO_MISO_PIN, RADIO_MOSI_PIN);
	_module = new Module(RADIO_CS_PIN, RADIO_DIO1_PIN, RADIO_RST_PIN, RADIO_BUSY_PIN, SPI);
	SX1262* chip = new SX1262(_module);
	_radio = chip;
	chip->setDio2AsRfSwitch(true);
	int state = chip->begin(frequency, bandwidth, spreading, coding,
	                        RADIOLIB_SX126X_SYNC_WORD_PRIVATE, power, 20, 1.8, false);

#elif defined(BOARD_HELTEC_V4)
	SPI.begin(RADIO_SCLK_PIN, RADIO_MISO_PIN, RADIO_MOSI_PIN);
	pinMode(RADIO_VFEM_EN, OUTPUT);
	pinMode(RADIO_FEM_CE, OUTPUT);
	pinMode(RADIO_PA_MODE, OUTPUT);
	digitalWrite(RADIO_VFEM_EN, HIGH);
	digitalWrite(RADIO_FEM_CE, HIGH);
	digitalWrite(RADIO_PA_MODE, LOW);
	_pa_mode_pin = RADIO_PA_MODE;
	_module = new Module(RADIO_CS_PIN, RADIO_DIO1_PIN, RADIO_RST_PIN, RADIO_BUSY_PIN, SPI);
	SX1262* chip = new SX1262(_module);
	_radio = chip;
	chip->setDio2AsRfSwitch(true);
	int state = chip->begin(frequency, bandwidth, spreading, coding,
	                        RADIOLIB_SX126X_SYNC_WORD_PRIVATE, power, 20, 1.8, false);

#else
	#error "Unsupported board: define BOARD_CUSTOM_WROOM_SX1276, BOARD_TBEAM, BOARD_LORA32_V21, BOARD_RAK4631, BOARD_HELTEC_V3, or BOARD_HELTEC_V4"
	int state = RADIOLIB_ERR_UNKNOWN;
#endif

	if (state != RADIOLIB_ERR_NONE) {
		ERRORF("LoRa init failed, code %d. Check wiring/board define.", state);
		return false;
	}

	_radio->startReceive();

	INFO("LoRa init succeeded.");
	TRACEF("LoRa bandwidth is %.2f Kbps", Utilities::OS::round(_bitrate/1000.0, 2));
#endif

	_online = true;
	return true;
}

void LoRaInterface::stop() {
#ifdef ARDUINO
	if (_radio) {
		_radio->standby();
	}
#endif
	_online = false;
}

void LoRaInterface::loop() {
	if (_online) {
#ifdef ARDUINO
		if (_radio->checkIrq(RADIOLIB_IRQ_RX_DONE)) {
			int len = _radio->getPacketLength();

			uint8_t rxBuf[255];
			int state = _radio->readData(rxBuf, len);

			if (state == RADIOLIB_ERR_NONE && len > 1) {
				Serial.println("RSSI: " + String(_radio->getRSSI()));
				Serial.println("Snr: "  + String(_radio->getSNR()));

				uint8_t hdr = rxBuf[0];
				uint8_t seq = packetSequence(hdr);

				if (isSplitPacket(hdr)) {
					if (_rx_seq == SEQ_UNSET || _rx_seq != seq) {
						_rx_seq = seq;
						buffer.clear();
						buffer.append(rxBuf + 1, len - 1);
					} else {
						buffer.append(rxBuf + 1, len - 1);
						_rx_seq = SEQ_UNSET;
						on_incoming(buffer);
					}
				} else {
					if (_rx_seq != SEQ_UNSET) {
						buffer.clear();
						_rx_seq = SEQ_UNSET;
					}
					buffer.clear();
					buffer.append(rxBuf + 1, len - 1);
					on_incoming(buffer);
				}
			} else if (state != RADIOLIB_ERR_NONE) {
				DEBUGF("LoRaInterface: readData failed, code %d", state);
			}

			_radio->startReceive();
		}
#endif
	}
}

/*virtual*/ bool LoRaInterface::send_outgoing(const Bytes& data) {
	DEBUGF("%s.on_outgoing: data: %s", toString().c_str(), data.toHex().c_str());
	bool success = true;
	try {
		if (_online) {
			TRACEF("LoRaInterface: sending %lu bytes...", data.size());
#ifdef ARDUINO
			uint8_t txBuf[255];
			uint8_t rand_nibble = (uint8_t)(Cryptography::randomnum(256)) & 0xF0;

			if ((int)data.size() <= LORA_MAX_PAYLOAD) {
				txBuf[0] = rand_nibble;
				memcpy(txBuf + 1, data.data(), data.size());

				if (_pa_mode_pin >= 0) { digitalWrite(_pa_mode_pin, HIGH); }
				int state = _radio->transmit(txBuf, 1 + data.size());
				if (_pa_mode_pin >= 0) { digitalWrite(_pa_mode_pin, LOW); }
				if (state != RADIOLIB_ERR_NONE) {
					ERRORF("LoRaInterface: transmit failed, code %d", state);
					success = false;
				}
			} else {
				uint8_t seq       = (_tx_seq_ctr++) & HEADER_SEQ_MASK;
				uint8_t split_hdr = rand_nibble | HEADER_SPLIT | seq;

				txBuf[0] = split_hdr;
				memcpy(txBuf + 1, data.data(), LORA_MAX_PAYLOAD);

				if (_pa_mode_pin >= 0) { digitalWrite(_pa_mode_pin, HIGH); }
				int state = _radio->transmit(txBuf, 1 + LORA_MAX_PAYLOAD);
				if (_pa_mode_pin >= 0) { digitalWrite(_pa_mode_pin, LOW); }
				if (state != RADIOLIB_ERR_NONE) {
					ERRORF("LoRaInterface: transmit part 1 failed, code %d", state);
					success = false;
				}

				size_t remainder = data.size() - LORA_MAX_PAYLOAD;
				txBuf[0] = split_hdr;
				memcpy(txBuf + 1, data.data() + LORA_MAX_PAYLOAD, remainder);

				if (_pa_mode_pin >= 0) { digitalWrite(_pa_mode_pin, HIGH); }
				state = _radio->transmit(txBuf, 1 + remainder);
				if (_pa_mode_pin >= 0) { digitalWrite(_pa_mode_pin, LOW); }
				if (state != RADIOLIB_ERR_NONE) {
					ERRORF("LoRaInterface: transmit part 2 failed, code %d", state);
					success = false;
				}
			}

			_radio->startReceive();
#endif
			TRACE("LoRaInterface: sent bytes");
		}

		InterfaceImpl::handle_outgoing(data);
	}
	catch (const std::exception& e) {
		ERRORF("Could not transmit on %s. The contained exception was: %s", toString().c_str(), e.what());
		success = false;
	}
	return success;
}

/*virtual*/ void LoRaInterface::on_incoming(const Bytes& data) {
	DEBUGF("%s.on_incoming: data: %s", toString().c_str(), data.toHex().c_str());
	InterfaceImpl::handle_incoming(data);
}
