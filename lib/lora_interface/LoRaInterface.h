#pragma once

// SPDX-License-Identifier: Apache-2.0
// ---------------------------------------------------------------
// Vendored from attermann/microReticulum's
// examples/common/lora_interface/LoRaInterface.h (real, working
// reference source, pulled directly from the repo).
//
// CHANGED for this project: a new board branch was added in the .cpp
// (see BOARD_XIAO_S3_SX1262) for the Seeed XIAO ESP32S3 + Wio-SX1262
// kit, connected via its B2B connector.
//
// Upstream is licensed under Apache-2.0 (see ../../THIRD_PARTY_NOTICES.md).
// This file has been modified from the upstream original, as stated here.
//
// `frequency` is back to the vendored original's 915.0 MHz. It was
// temporarily 433.0 for the old hand-wired SX1276 rig; 915 is the
// correct US band (FCC Part 15.247) and the SX1262 covers it.
// ---------------------------------------------------------------

#include <microReticulum/Interface.h>
#include <microReticulum/Bytes.h>
#include <microReticulum/Type.h>

#ifdef ARDUINO
#include <SPI.h>
#include <RadioLib.h>
#endif

#include <stdint.h>

class LoRaInterface : public RNS::InterfaceImpl {

public:
	LoRaInterface(const char* name = "LoRaInterface");
	virtual ~LoRaInterface();

	virtual bool start();
	virtual void stop();
	virtual void loop();

private:
	virtual bool send_outgoing(const RNS::Bytes& data);
	void on_incoming(const RNS::Bytes& data);

public:
	// Split-packet protocol constants
	static constexpr uint8_t HEADER_SPLIT     = 0x08;  // bit 3: split-packet flag
	static constexpr uint8_t HEADER_SEQ_MASK  = 0x07;  // bits 2:0: sequence number
	static constexpr uint8_t SEQ_UNSET        = 0xFF;  // sentinel: no split in progress
	static constexpr int     LORA_MAX_PAYLOAD = 254;   // 255 - 1 header byte

private:
	const uint8_t message_count = 0;
	RNS::Bytes buffer;

	uint8_t _rx_seq     = SEQ_UNSET;
	uint8_t _tx_seq_ctr = 0;

	// Radio parameters (RadioLib units: MHz, kHz)
	const float frequency = 915.0;   // MHz -- US / FCC Part 15.247
	const float bandwidth = 125.0;   // kHz
	const int   spreading = 8;
	const int   coding    = 5;
	const int   power     = 17;      // dBm

#ifdef ARDUINO
	Module*        _module      = nullptr;
	PhysicalLayer* _radio       = nullptr;
	int            _pa_mode_pin = -1;    // unused on this board; kept for parity with vendored source
#endif

};
