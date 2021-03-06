/*
 This project is free software: you can redistribute it and/or modify
 it under the terms of the GNU General Public License as published by
 the Free Software Foundation, either version 3 of the License, or
 (at your option) any later version.

 Multiprotocol is distributed in the hope that it will be useful,
 but WITHOUT ANY WARRANTY; without even the implied warranty of
 MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 GNU General Public License for more details.

 You should have received a copy of the GNU General Public License
 along with Multiprotocol.  If not, see <http://www.gnu.org/licenses/>.
 */
// compatible with WLToys V2x2, JXD JD38x, JD39x, JJRC H6C, Yizhan Tarantula X6 ...
// Last sync with hexfet new_protocols/v202_nrf24l01.c dated 2015-03-15

#if defined(V2X2_NRF24L01_INO)


#include "iface_nrf24l01.h"
#define V2X2_MR101_FORCE_ID

#define V2X2_BIND_COUNT 1000
// Timeout for callback in uSec, 4ms=4000us for V202
#define V2X2_PACKET_PERIOD 4000
//
// Time to wait for packet to be sent (no ACK, so very short)
#define V2X2_PACKET_CHKTIME  100
#define V2X2_PAYLOADSIZE 16

// 
enum {
	V2X2_FLAG_CAMERA = 0x01, // also automatic Missile Launcher and Hoist in one direction
	V2X2_FLAG_VIDEO  = 0x02, // also Sprayer, Bubbler, Missile Launcher(1), and Hoist in the other dir.
	V2X2_FLAG_FLIP   = 0x04,
	V2X2_FLAG_UNK9   = 0x08,
	V2X2_FLAG_LIGHT  = 0x10,
	V2X2_FLAG_UNK10  = 0x20,
	V2X2_FLAG_BIND   = 0xC0,
	// flags going to byte 10
	V2X2_FLAG_HEADLESS  = 0x02,
	V2X2_FLAG_MAG_CAL_X = 0x08,
	V2X2_FLAG_MAG_CAL_Y = 0x20,
    V2X2_FLAG_EMERGENCY = 0x80,	// JXD-506
    // flags going to byte 11 (JXD-506)
    V2X2_FLAG_START_STOP = 0x40,
    V2X2_FLAG_CAMERA_UP  = 0x01,   
    V2X2_FLAG_CAMERA_DN  = 0x02,
};

//

// This is frequency hopping table for V202 protocol
// The table is the first 4 rows of 32 frequency hopping
// patterns, all other rows are derived from the first 4.
// For some reason the protocol avoids channels, dividing
// by 16 and replaces them by subtracting 3 from the channel
// number in this case.
// The pattern is defined by 5 least significant bits of
// sum of 3 bytes comprising TX id
const uint8_t PROGMEM freq_hopping[][16] = {
	{ 0x27, 0x1B, 0x39, 0x28, 0x24, 0x22, 0x2E, 0x36,
		0x19, 0x21, 0x29, 0x14, 0x1E, 0x12, 0x2D, 0x18 }, //  00
	{ 0x2E, 0x33, 0x25, 0x38, 0x19, 0x12, 0x18, 0x16,
		0x2A, 0x1C, 0x1F, 0x37, 0x2F, 0x23, 0x34, 0x10 }, //  01
	{ 0x11, 0x1A, 0x35, 0x24, 0x28, 0x18, 0x25, 0x2A,
		0x32, 0x2C, 0x14, 0x27, 0x36, 0x34, 0x1C, 0x17 }, //  02
	{ 0x22, 0x27, 0x17, 0x39, 0x34, 0x28, 0x2B, 0x1D,
		0x18, 0x2A, 0x21, 0x38, 0x10, 0x26, 0x20, 0x1F }  //  03
};

static void __attribute__((unused)) V2X2_RF_init()
{
	NRF24L01_Initialize();

	NRF24L01_WriteReg(NRF24L01_02_EN_RXADDR, 0x3F);  // Enable all data pipes
	if(sub_protocol==V2X2_MR101)
		NRF24L01_SetBitrate(NRF24L01_BR_250K);
	NRF24L01_WriteRegisterMulti(NRF24L01_0A_RX_ADDR_P0, (uint8_t *)"\x66\x88\x68\x68\x68", 5);
	NRF24L01_WriteRegisterMulti(NRF24L01_10_TX_ADDR,    (uint8_t *)"\x66\x88\x68\x68\x68", 5);
}

static void __attribute__((unused)) V2X2_set_tx_id(void)
{
	uint8_t sum;
	sum = rx_tx_addr[1] + rx_tx_addr[2] + rx_tx_addr[3];
	// Higher 3 bits define increment to corresponding row
	uint8_t increment = (sum & 0x1e) >> 2;
	// Base row is defined by lowest 2 bits
	sum &=0x03;
	for (uint8_t i = 0; i < 16; ++i) {
		uint8_t val = pgm_read_byte_near(&freq_hopping[sum][i]) + increment;
		// Strange avoidance of channels divisible by 16
		hopping_frequency[i] = (val & 0x0f) ? val : val - 3;
	}
	#ifdef V2X2_MR101_FORCE_ID
	if(sub_protocol==V2X2_MR101)
	{
		rx_tx_addr[1]=0x83;
		rx_tx_addr[2]=0x03;
		rx_tx_addr[3]=0xAE;
		memcpy(hopping_frequency,"\x05\x12\x08\x0C\x04\x0E\x10",7);
	}
	#endif
}

static void __attribute__((unused)) V2X2_send_packet()
{
	uint8_t rf_ch = hopping_frequency[hopping_frequency_no >> 1];
	hopping_frequency_no = (hopping_frequency_no + 1) & 0x1F;
	if(sub_protocol==V2X2_MR101 && hopping_frequency_no>13)
			hopping_frequency_no=0;
	NRF24L01_WriteReg(NRF24L01_05_RF_CH, rf_ch);

	uint8_t flags2=0;
	if (IS_BIND_IN_PROGRESS)
	{
		flags     = V2X2_FLAG_BIND;
		packet[0] = 0;
		packet[1] = 0;
		packet[2] = 0;
		packet[3] = 0;
		packet[4] = 0;
		packet[5] = 0;
		packet[6] = 0;
	}
	else
	{
		packet[0] = convert_channel_8b(THROTTLE);
		packet[1] = convert_channel_s8b(RUDDER);
		packet[2] = convert_channel_s8b(ELEVATOR);
		packet[3] = convert_channel_s8b(AILERON);
		// Trims, middle is 0x40
		packet[4] = 0x40; // yaw
		packet[5] = 0x40; // pitch
		packet[6] = 0x40; // roll

		//Flags
		flags=0;
		// Channel 5
		if (CH5_SW)	flags = V2X2_FLAG_FLIP;

		if(sub_protocol!=V2X2_MR101)
		{//V2X2 & JXD506
			// Channel 6
			if (CH6_SW)	flags |= V2X2_FLAG_LIGHT;
			// Channel 7
			if (CH7_SW)	flags |= V2X2_FLAG_CAMERA;
			// Channel 8
			if (CH8_SW)	flags |= V2X2_FLAG_VIDEO;

			//Flags2
			// Channel 9
			if (CH9_SW)
				flags2 = V2X2_FLAG_HEADLESS;
			if(sub_protocol==JXD506)
			{
				// Channel 11
				if (CH11_SW)
					flags2 |= V2X2_FLAG_EMERGENCY;
			}
			else
			{//V2X2
				// Channel 10
				if (CH10_SW)
					flags2 |= V2X2_FLAG_MAG_CAL_X;
				// Channel 11
				if (CH11_SW)
					flags2 |= V2X2_FLAG_MAG_CAL_Y;
			}
		}
	}
	// TX id
	packet[7] = rx_tx_addr[1];
	packet[8] = rx_tx_addr[2];
	packet[9] = rx_tx_addr[3];
	// flags
	packet[10] = flags2;
	packet[11] = 0x00;
	packet[12] = 0x00;
	packet[13] = 0x00;
	if(sub_protocol==JXD506)
	{
		// Channel 10
		if (CH10_SW)
			packet[11] = V2X2_FLAG_START_STOP;
		// Channel 12
		if(CH12_SW)
			packet[11] |= V2X2_FLAG_CAMERA_UP;
		else if(Channel_data[CH12] < CHANNEL_MIN_COMMAND)
			packet[11] |= V2X2_FLAG_CAMERA_DN;
		packet[12] = 0x40;
		packet[13] = 0x40;
	}
	else if(sub_protocol==V2X2_MR101)
	{
		
		if (CH10_SW) packet[11]  = 0x04;	// Motors start/stop
		if (CH11_SW) packet[11] |= 0x40;	// Auto Land=-100% Takeoff=+100%
		if (CH7_SW)	 flags |= 0x02;			// Picture
		if (CH8_SW)	 flags |= 0x01;			// Video
		if(IS_BIND_IN_PROGRESS)
			flags = 0x80;
		flags |= (hopping_frequency_no & 0x01)<<6;
	}
	packet[14] = flags;
	uint8_t sum = packet[0];
	for (uint8_t i = 1; i < 15;  ++i)
		sum += packet[i];
	packet[15] = sum;

	NRF24L01_FlushTx();
	NRF24L01_WritePayload(packet, V2X2_PAYLOADSIZE);
	//packet_sent = 1;

	if (! hopping_frequency_no)
		NRF24L01_SetPower();
}

uint16_t V2X2_callback()
{
	//if (packet_sent && NRF24L01_packet_ack() != PKT_ACKED)
	//	return V2X2_PACKET_CHKTIME;
	#ifdef MULTI_SYNC
		telemetry_set_input_sync(V2X2_PACKET_PERIOD);
	#endif
	if(bind_counter)
	{
		if (--bind_counter == 0)
		{
			BIND_DONE;
			if(sub_protocol==V2X2_MR101)
			{
				#ifdef V2X2_MR101_FORCE_ID
					NRF24L01_WriteRegisterMulti(NRF24L01_10_TX_ADDR, (uint8_t *)"\xC9\x59\xD2\x65\x34", 5);
					memcpy(hopping_frequency,"\x03\x05\x15\x0D\x06\x14\x0B",7);
				#endif
			}
			hopping_frequency_no = 0;
		}
	}
	V2X2_send_packet();
	// Packet every 4ms
	return V2X2_PACKET_PERIOD;
}

void V2X2_init()
{	
	if(sub_protocol==V2X2_MR101)
		BIND_IN_PROGRESS;
	//packet_sent = 0;
	hopping_frequency_no = 0;
	bind_counter = V2X2_BIND_COUNT;

	V2X2_RF_init();
	V2X2_set_tx_id();
}

#endif
