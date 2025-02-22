/* Copyright 2023 Dual Tachyon
 * https://github.com/DualTachyon
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 *     Unless required by applicable law or agreed to in writing, software
 *     distributed under the License is distributed on an "AS IS" BASIS,
 *     WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *     See the License for the specific language governing permissions and
 *     limitations under the License.
 */

#include <string.h>
#include <stdlib.h>  // abs()

#include "app/action.h"
#ifdef ENABLE_AIRCOPY
	#include "app/aircopy.h"
#endif
#include "app/app.h"
#include "app/dtmf.h"
#ifdef ENABLE_FMRADIO
	#include "app/fm.h"
#endif
#include "app/generic.h"
#include "app/main.h"
#include "app/menu.h"
#include "app/scanner.h"
#include "app/uart.h"
#include "ARMCM0.h"
#include "audio.h"
#include "board.h"
#include "bsp/dp32g030/gpio.h"
#include "driver/backlight.h"
#ifdef ENABLE_FMRADIO
	#include "driver/bk1080.h"
#endif
#include "driver/bk4819.h"
#include "driver/gpio.h"
#include "driver/keyboard.h"
#include "driver/st7565.h"
#include "driver/system.h"
#include "driver/uart.h"
#include "am_fix.h"
#include "dtmf.h"
#include "external/printf/printf.h"
#include "frequencies.h"
#include "functions.h"
#include "helper/battery.h"
#include "misc.h"
#include "radio.h"
#include "settings.h"
#if defined(ENABLE_OVERLAY)
	#include "sram-overlay.h"
#endif
#include "ui/battery.h"
#include "ui/inputbox.h"
#include "ui/main.h"
#include "ui/menu.h"
#include "ui/status.h"
#include "ui/ui.h"

// original QS front end register settings
const uint8_t orig_lna_short = 3;   //   0dB
const uint8_t orig_lna       = 2;   // -14dB
const uint8_t orig_mixer     = 3;   //   0dB
const uint8_t orig_pga       = 6;   //  -3dB

static void APP_ProcessKey(const key_code_t Key, const bool key_pressed, const bool key_held);

static void updateRSSI(const int vfo)
{
	int16_t rssi = BK4819_GetRSSI();

	#ifdef ENABLE_AM_FIX
		// add RF gain adjust compensation
		if (g_eeprom.vfo_info[vfo].am_mode && g_setting_am_fix)
			rssi -= rssi_gain_diff[vfo];
	#endif

	if (g_current_rssi[vfo] == rssi)
		return;     // no change

	g_current_rssi[vfo] = rssi;

	UI_UpdateRSSI(rssi, vfo);
}

static void APP_CheckForIncoming(void)
{
	if (!g_squelch_lost)
		return;          // squelch is closed

	// squelch is open

	if (g_scan_state_dir == SCAN_OFF)
	{	// not RF scanning

		if (g_css_scan_mode != CSS_SCAN_MODE_OFF && g_rx_reception_mode == RX_MODE_NONE)
		{	// CTCSS/DTS scanning

			g_scan_pause_delay_in_10ms = scan_pause_delay_in_5_10ms;
			g_schedule_scan_listen    = false;
			g_rx_reception_mode       = RX_MODE_DETECTED;
		}

		if (g_eeprom.dual_watch == DUAL_WATCH_OFF)
		{	// dual watch is disabled

			#ifdef ENABLE_NOAA
				if (g_is_noaa_mode)
				{
					g_noaa_count_down_10ms = noaa_count_down_3_10ms;
					g_schedule_noaa        = false;
				}
			#endif

			if (g_current_function != FUNCTION_INCOMING)
			{
				FUNCTION_Select(FUNCTION_INCOMING);
				//g_update_display = true;

				updateRSSI(g_eeprom.rx_vfo);
				g_update_rssi = true;
			}

			return;
		}

		// dual watch is enabled and we're RX'ing a signal

		if (g_rx_reception_mode != RX_MODE_NONE)
		{
			if (g_current_function != FUNCTION_INCOMING)
			{
				FUNCTION_Select(FUNCTION_INCOMING);
				//g_update_display = true;

				updateRSSI(g_eeprom.rx_vfo);
				g_update_rssi = true;
			}
			return;
		}

		g_dual_watch_count_down_10ms = dual_watch_count_after_rx_10ms;
		g_schedule_dual_watch       = false;

		// let the user see DW is not active
		g_dual_watch_active = false;
		g_update_status    = true;
	}
	else
	{	// RF scanning
		if (g_rx_reception_mode != RX_MODE_NONE)
		{
			if (g_current_function != FUNCTION_INCOMING)
			{
				FUNCTION_Select(FUNCTION_INCOMING);
				//g_update_display = true;

				updateRSSI(g_eeprom.rx_vfo);
				g_update_rssi = true;
			}
			return;
		}

		g_scan_pause_delay_in_10ms = scan_pause_delay_in_3_10ms;
		g_schedule_scan_listen    = false;
	}

	g_rx_reception_mode = RX_MODE_DETECTED;

	if (g_current_function != FUNCTION_INCOMING)
	{
		FUNCTION_Select(FUNCTION_INCOMING);
		//g_update_display = true;

		updateRSSI(g_eeprom.rx_vfo);
		g_update_rssi = true;
	}
}

static void APP_HandleIncoming(void)
{
	bool flag;

	if (!g_squelch_lost)
	{	// squelch is closed

		if (g_dtmf_rx_index > 0)
			DTMF_clear_RX();

		if (g_current_function != FUNCTION_FOREGROUND)
		{
			FUNCTION_Select(FUNCTION_FOREGROUND);
			g_update_display = true;
		}
		return;
	}

	flag = (g_scan_state_dir == SCAN_OFF && g_current_code_type == CODE_TYPE_OFF);

	#ifdef ENABLE_NOAA
		if (IS_NOAA_CHANNEL(g_rx_vfo->channel_save) && g_noaa_count_down_10ms > 0)
		{
			g_noaa_count_down_10ms = 0;
			flag = true;
		}
	#endif

	if (g_CTCSS_lost && g_current_code_type == CODE_TYPE_CONTINUOUS_TONE)
	{
		flag = true;
		g_found_CTCSS = false;
	}

	if (g_CDCSS_lost && g_CDCSS_code_type == CDCSS_POSITIVE_CODE && (g_current_code_type == CODE_TYPE_DIGITAL || g_current_code_type == CODE_TYPE_REVERSE_DIGITAL))
	{
		g_found_CDCSS = false;
	}
	else
	if (!flag)
		return;

	if (g_scan_state_dir == SCAN_OFF && g_css_scan_mode == CSS_SCAN_MODE_OFF)
	{	// not scanning
		if (g_rx_vfo->dtmf_decoding_enable || g_setting_killed)
		{	// DTMF DCD is enabled

			DTMF_HandleRequest();

			if (g_dtmf_call_state == DTMF_CALL_STATE_NONE)
			{
				if (g_rx_reception_mode == RX_MODE_DETECTED)
				{
					g_dual_watch_count_down_10ms = dual_watch_count_after_1_10ms;
					g_schedule_dual_watch       = false;

					g_rx_reception_mode = RX_MODE_LISTENING;

					// let the user see DW is not active
					g_dual_watch_active = false;
					g_update_status    = true;

					g_update_display = true;
					return;
				}
			}
		}
	}

	APP_StartListening(g_monitor_enabled ? FUNCTION_MONITOR : FUNCTION_RECEIVE, false);
}

static void APP_HandleReceive(void)
{
	#define END_OF_RX_MODE_SKIP 0
	#define END_OF_RX_MODE_END  1
	#define END_OF_RX_MODE_TTE  2

	uint8_t Mode = END_OF_RX_MODE_SKIP;

	if (g_flag_tail_tone_elimination_complete)
	{
		Mode = END_OF_RX_MODE_END;
		goto Skip;
	}

	if (g_scan_state_dir != SCAN_OFF && IS_FREQ_CHANNEL(g_next_channel))
	{
		if (g_squelch_lost)
			return;

		Mode = END_OF_RX_MODE_END;
		goto Skip;
	}

	switch (g_current_code_type)
	{
		default:
		case CODE_TYPE_OFF:
			break;

		case CODE_TYPE_CONTINUOUS_TONE:
			if (g_found_CTCSS && g_found_CTCSS_count_down_10ms == 0)
			{
				g_found_CTCSS = false;
				g_found_CDCSS = false;
				Mode        = END_OF_RX_MODE_END;
				goto Skip;
			}
			break;

		case CODE_TYPE_DIGITAL:
		case CODE_TYPE_REVERSE_DIGITAL:
			if (g_found_CDCSS && g_found_CDCSS_count_down_10ms == 0)
			{
				g_found_CTCSS = false;
				g_found_CDCSS = false;
				Mode        = END_OF_RX_MODE_END;
				goto Skip;
			}
			break;
	}

	if (g_squelch_lost)
	{
		#ifdef ENABLE_NOAA
			if (!g_end_of_rx_detected_maybe && IS_NOT_NOAA_CHANNEL(g_rx_vfo->channel_save))
		#else
			if (!g_end_of_rx_detected_maybe)
		#endif
		{
			switch (g_current_code_type)
			{
				case CODE_TYPE_OFF:
					if (g_eeprom.squelch_level)
					{
						if (g_CxCSS_tail_found)
						{
							Mode = END_OF_RX_MODE_TTE;
							g_CxCSS_tail_found = false;
						}
					}
					break;

				case CODE_TYPE_CONTINUOUS_TONE:
					if (g_CTCSS_lost)
					{
						g_found_CTCSS = false;
					}
					else
					if (!g_found_CTCSS)
					{
						g_found_CTCSS = true;
						g_found_CTCSS_count_down_10ms = 100;   // 1 sec
					}

					if (g_CxCSS_tail_found)
					{
						Mode = END_OF_RX_MODE_TTE;
						g_CxCSS_tail_found = false;
					}
					break;

				case CODE_TYPE_DIGITAL:
				case CODE_TYPE_REVERSE_DIGITAL:
					if (g_CDCSS_lost && g_CDCSS_code_type == CDCSS_POSITIVE_CODE)
					{
						g_found_CDCSS = false;
					}
					else
					if (!g_found_CDCSS)
					{
						g_found_CDCSS = true;
						g_found_CDCSS_count_down_10ms = 100;   // 1 sec
					}

					if (g_CxCSS_tail_found)
					{
						if (BK4819_GetCTCType() == 1)
							Mode = END_OF_RX_MODE_TTE;

						g_CxCSS_tail_found = false;
					}

					break;
			}
		}
	}
	else
		Mode = END_OF_RX_MODE_END;

	if (!g_end_of_rx_detected_maybe &&
	     Mode == END_OF_RX_MODE_SKIP &&
	     g_next_time_slice_40ms &&
	     g_eeprom.tail_note_elimination &&
	    (g_current_code_type == CODE_TYPE_DIGITAL || g_current_code_type == CODE_TYPE_REVERSE_DIGITAL) &&
	     BK4819_GetCTCType() == 1)
	{
		Mode = END_OF_RX_MODE_TTE;
	}
	else
	{
		g_next_time_slice_40ms = false;
	}

Skip:
	switch (Mode)
	{
		case END_OF_RX_MODE_SKIP:
			break;

		case END_OF_RX_MODE_END:
			RADIO_SetupRegisters(true);

			#ifdef ENABLE_NOAA
				if (IS_NOAA_CHANNEL(g_rx_vfo->channel_save))
					g_noaa_count_down_10ms = 300;         // 3 sec
			#endif

			g_update_display = true;

			if (g_scan_state_dir != SCAN_OFF)
			{
				switch (g_eeprom.scan_resume_mode)
				{
					case SCAN_RESUME_TO:
						break;

					case SCAN_RESUME_CO:
						g_scan_pause_delay_in_10ms = scan_pause_delay_in_7_10ms;
						g_schedule_scan_listen    = false;
						break;

					case SCAN_RESUME_SE:
						SCANNER_Stop();
						break;
				}
			}

			break;

		case END_OF_RX_MODE_TTE:
			if (g_eeprom.tail_note_elimination)
			{
				GPIO_ClearBit(&GPIOC->DATA, GPIOC_PIN_AUDIO_PATH);

				g_tail_tone_elimination_count_down_10ms = 20;
				g_flag_tail_tone_elimination_complete   = false;
				g_end_of_rx_detected_maybe = true;
				g_enable_speaker        = false;
			}
			break;
	}
}

static void APP_HandleFunction(void)
{
	switch (g_current_function)
	{
		case FUNCTION_FOREGROUND:
			APP_CheckForIncoming();
			break;

		case FUNCTION_TRANSMIT:
			break;

		case FUNCTION_MONITOR:
			break;

		case FUNCTION_INCOMING:
			APP_HandleIncoming();
			break;

		case FUNCTION_RECEIVE:
			APP_HandleReceive();
			break;

		case FUNCTION_POWER_SAVE:
			if (!g_rx_idle_mode)
				APP_CheckForIncoming();
			break;

		case FUNCTION_BAND_SCOPE:
			break;
	}
}

void APP_StartListening(function_type_t Function, const bool reset_am_fix)
{
	const unsigned int chan = g_eeprom.rx_vfo;
//	const unsigned int chan = g_rx_vfo->channel_save;

	if (g_setting_killed)
		return;

	#ifdef ENABLE_FMRADIO
		if (g_fm_radio_mode)
			BK1080_Init(0, false);
	#endif

	// clear the other vfo's rssi level (to hide the antenna symbol)
	g_vfo_rssi_bar_level[(chan + 1) & 1u] = 0;

	GPIO_SetBit(&GPIOC->DATA, GPIOC_PIN_AUDIO_PATH);

	g_enable_speaker = true;

	if (g_setting_backlight_on_tx_rx >= 2)
		backlight_turn_on();

	if (g_scan_state_dir != SCAN_OFF)
	{
		switch (g_eeprom.scan_resume_mode)
		{
			case SCAN_RESUME_TO:
				if (!g_scan_pause_mode)
				{
					g_scan_pause_delay_in_10ms = scan_pause_delay_in_1_10ms;
					g_schedule_scan_listen    = false;
					g_scan_pause_mode         = true;
				}
				break;

			case SCAN_RESUME_CO:
			case SCAN_RESUME_SE:
				g_scan_pause_delay_in_10ms = 0;
				g_schedule_scan_listen    = false;
				break;
		}

		g_scan_keep_frequency = true;
	}

	#ifdef ENABLE_NOAA
		if (IS_NOAA_CHANNEL(g_rx_vfo->channel_save) && g_is_noaa_mode)
		{
			g_rx_vfo->channel_save        = g_noaa_channel + NOAA_CHANNEL_FIRST;
			g_rx_vfo->pRX->frequency      = NoaaFrequencyTable[g_noaa_channel];
			g_rx_vfo->pTX->frequency      = NoaaFrequencyTable[g_noaa_channel];
			g_eeprom.screen_channel[chan] = g_rx_vfo->channel_save;
			g_noaa_count_down_10ms        = 500;   // 5 sec
			g_schedule_noaa               = false;
		}
	#endif

	if (g_css_scan_mode != CSS_SCAN_MODE_OFF)
		g_css_scan_mode = CSS_SCAN_MODE_FOUND;

	if (g_scan_state_dir == SCAN_OFF &&
	    g_css_scan_mode == CSS_SCAN_MODE_OFF &&
	    g_eeprom.dual_watch != DUAL_WATCH_OFF)
	{	// not scanning, dual watch is enabled

		g_dual_watch_count_down_10ms = dual_watch_count_after_2_10ms;
		g_schedule_dual_watch       = false;

		g_rx_vfo_is_active = true;

		// let the user see DW is not active
		g_dual_watch_active = false;
		g_update_status    = true;
	}

	{	// RF RX front end gain

		// original setting
		uint16_t lna_short = orig_lna_short;
		uint16_t lna       = orig_lna;
		uint16_t mixer     = orig_mixer;
		uint16_t pga       = orig_pga;

		#ifdef ENABLE_AM_FIX
			if (g_rx_vfo->am_mode && g_setting_am_fix)
			{	// AM RX mode
				if (reset_am_fix)
					AM_fix_reset(chan);      // TODO: only reset it when moving channel/frequency
				AM_fix_10ms(chan);
			}
			else
			{	// FM RX mode
				BK4819_WriteRegister(BK4819_REG_13, (lna_short << 8) | (lna << 5) | (mixer << 3) | (pga << 0));
			}
		#else
			BK4819_WriteRegister(BK4819_REG_13, (lna_short << 8) | (lna << 5) | (mixer << 3) | (pga << 0));
		#endif
	}

	// AF gain - original QS values
	BK4819_WriteRegister(BK4819_REG_48,
		(11u << 12)                |     // ??? .. 0 to 15, doesn't seem to make any difference
		( 0u << 10)                |     // AF Rx Gain-1
		(g_eeprom.volume_gain << 4) |     // AF Rx Gain-2
		(g_eeprom.dac_gain    << 0));     // AF DAC Gain (after Gain-1 and Gain-2)

	#ifdef ENABLE_VOICE
		#ifdef MUTE_AUDIO_FOR_VOICE
			if (g_voice_write_index == 0)
				BK4819_SetAF(g_rx_vfo->am_mode ? BK4819_AF_AM : BK4819_AF_FM);
		#else
			BK4819_SetAF(g_rx_vfo->am_mode ? BK4819_AF_AM : BK4819_AF_FM);
		#endif
	#else
		BK4819_SetAF(g_rx_vfo->am_mode ? BK4819_AF_AM : BK4819_AF_FM);
	#endif

	FUNCTION_Select(Function);

	#ifdef ENABLE_FMRADIO
		if (Function == FUNCTION_MONITOR || g_fm_radio_mode)
	#else
		if (Function == FUNCTION_MONITOR)
	#endif
	{	// squelch is disabled
		if (g_screen_to_display != DISPLAY_MENU)     // 1of11 .. don't close the menu
			GUI_SelectNextDisplay(DISPLAY_MAIN);
	}
	else
		g_update_display = true;

	g_update_status = true;
}

uint32_t APP_SetFrequencyByStep(vfo_info_t *pInfo, int8_t Step)
{
	uint32_t Frequency = pInfo->freq_config_rx.frequency + (Step * pInfo->step_freq);

	if (pInfo->step_freq == 833)
	{
		const uint32_t Lower = FREQ_BAND_TABLE[pInfo->band].lower;
		const uint32_t Delta = Frequency - Lower;
		uint32_t       Base  = (Delta / 2500) * 2500;
		const uint32_t Index = ((Delta - Base) % 2500) / 833;

		if (Index == 2)
			Base++;

		Frequency = Lower + Base + (Index * 833);
	}

	if (Frequency >= FREQ_BAND_TABLE[pInfo->band].upper)
		Frequency =  FREQ_BAND_TABLE[pInfo->band].lower;
	else
	if (Frequency < FREQ_BAND_TABLE[pInfo->band].lower)
		Frequency = FREQUENCY_FloorToStep(FREQ_BAND_TABLE[pInfo->band].upper, pInfo->step_freq, FREQ_BAND_TABLE[pInfo->band].lower);

	return Frequency;
}

static void FREQ_NextChannel(void)
{
	g_rx_vfo->freq_config_rx.frequency = APP_SetFrequencyByStep(g_rx_vfo, g_scan_state_dir);

	RADIO_ApplyOffset(g_rx_vfo);
	RADIO_ConfigureSquelchAndOutputPower(g_rx_vfo);
	RADIO_SetupRegisters(true);

	#ifdef ENABLE_FASTER_CHANNEL_SCAN
		g_scan_pause_delay_in_10ms = 9;   // 90ms
	#else
		g_scan_pause_delay_in_10ms = scan_pause_delay_in_6_10ms;
	#endif

	g_scan_keep_frequency = false;
	g_update_display     = true;
}

static void USER_NextChannel(void)
{
	static unsigned int prevChannel = 0;
	const bool          enabled     = (g_eeprom.scan_list_default < 2) ? g_eeprom.scan_list_enabled[g_eeprom.scan_list_default] : true;
	const int           chan1       = (g_eeprom.scan_list_default < 2) ? g_eeprom.scan_list_priority_ch1[g_eeprom.scan_list_default] : -1;
	const int           chan2       = (g_eeprom.scan_list_default < 2) ? g_eeprom.scan_list_priority_ch2[g_eeprom.scan_list_default] : -1;
	const unsigned int  prev_chan   = g_next_channel;
	unsigned int        chan        = 0;

	if (enabled)
	{
		#pragma GCC diagnostic push
		#pragma GCC diagnostic ignored "-Wimplicit-fallthrough="

		switch (g_current_scan_list)
		{
			case SCAN_NEXT_CHAN_SCANLIST1:
				prevChannel = g_next_channel;

				if (chan1 >= 0)
				{
					if (RADIO_CheckValidChannel(chan1, false, 0))
					{
						g_current_scan_list = SCAN_NEXT_CHAN_SCANLIST1;
						g_next_channel      = chan1;
						break;
					}
				}

			case SCAN_NEXT_CHAN_SCANLIST2:
				if (chan2 >= 0)
				{
					if (RADIO_CheckValidChannel(chan2, false, 0))
					{
						g_current_scan_list = SCAN_NEXT_CHAN_SCANLIST2;
						g_next_channel      = chan2;
						break;
					}
				}

			// this bit doesn't yet work if the other VFO is a frequency
			case SCAN_NEXT_CHAN_DUAL_WATCH:
				// dual watch is enabled - include the other VFO in the scan
//				if (g_eeprom.dual_watch != DUAL_WATCH_OFF)
//				{
//					chan = (g_eeprom.rx_vfo + 1) & 1u;
//					chan = g_eeprom.screen_channel[chan];
//					if (chan <= USER_CHANNEL_LAST)
//					{
//						g_current_scan_list = SCAN_NEXT_CHAN_DUAL_WATCH;
//						g_next_channel   = chan;
//						break;
//					}
//				}

			default:
			case SCAN_NEXT_CHAN_USER:
				g_current_scan_list = SCAN_NEXT_CHAN_USER;
				g_next_channel     = prevChannel;
				chan             = 0xff;
				break;
		}

		#pragma GCC diagnostic pop
	}

	if (!enabled || chan == 0xff)
	{
		chan = RADIO_FindNextChannel(g_next_channel + g_scan_state_dir, g_scan_state_dir, (g_eeprom.scan_list_default < 2) ? true : false, g_eeprom.scan_list_default);
		if (chan == 0xFF)
		{	// no valid channel found

			chan = USER_CHANNEL_FIRST;
//			return;
		}

		g_next_channel = chan;
	}

	if (g_next_channel != prev_chan)
	{
		g_eeprom.user_channel[g_eeprom.rx_vfo]  = g_next_channel;
		g_eeprom.screen_channel[g_eeprom.rx_vfo] = g_next_channel;

		RADIO_ConfigureChannel(g_eeprom.rx_vfo, VFO_CONFIGURE_RELOAD);
		RADIO_SetupRegisters(true);

		g_update_display = true;
	}

	#ifdef ENABLE_FASTER_CHANNEL_SCAN
		g_scan_pause_delay_in_10ms = 9;  // 90ms .. <= ~60ms it misses signals (squelch response and/or PLL lock time) ?
	#else
		g_scan_pause_delay_in_10ms = scan_pause_delay_in_3_10ms;
	#endif

	g_scan_keep_frequency = false;

	if (enabled)
		if (++g_current_scan_list >= SCAN_NEXT_NUM)
			g_current_scan_list = SCAN_NEXT_CHAN_SCANLIST1;  // back round we go
}

#ifdef ENABLE_NOAA
	static void NOAA_IncreaseChannel(void)
	{
		if (++g_noaa_channel >= ARRAY_SIZE(NoaaFrequencyTable))
			g_noaa_channel = 0;
	}
#endif

static void DUALWATCH_Alternate(void)
{
	#ifdef ENABLE_NOAA
		if (g_is_noaa_mode)
		{
			if (IS_NOT_NOAA_CHANNEL(g_eeprom.screen_channel[0]) || IS_NOT_NOAA_CHANNEL(g_eeprom.screen_channel[1]))
				g_eeprom.rx_vfo = (g_eeprom.rx_vfo + 1) & 1;
			else
				g_eeprom.rx_vfo = 0;

			g_rx_vfo = &g_eeprom.vfo_info[g_eeprom.rx_vfo];

			if (g_eeprom.vfo_info[0].channel_save >= NOAA_CHANNEL_FIRST)
				NOAA_IncreaseChannel();
		}
		else
	#endif
	{	// toggle between VFO's
		g_eeprom.rx_vfo = (g_eeprom.rx_vfo + 1) & 1;
		g_rx_vfo             = &g_eeprom.vfo_info[g_eeprom.rx_vfo];

		if (!g_dual_watch_active)
		{	// let the user see DW is active
			g_dual_watch_active = true;
			g_update_status    = true;
		}
	}

	RADIO_SetupRegisters(false);

	#ifdef ENABLE_NOAA
		g_dual_watch_count_down_10ms = g_is_noaa_mode ? dual_watch_count_noaa_10ms : dual_watch_count_toggle_10ms;
	#else
		g_dual_watch_count_down_10ms = dual_watch_count_toggle_10ms;
	#endif
}

void APP_CheckRadioInterrupts(void)
{
	if (g_screen_to_display == DISPLAY_SCANNER)
		return;

	while (BK4819_ReadRegister(BK4819_REG_0C) & 1u)
	{	// BK chip interrupt request

		uint16_t interrupt_status_bits;

		// reset the interrupt ?
		BK4819_WriteRegister(BK4819_REG_02, 0);

		// fetch the interrupt status bits
		interrupt_status_bits = BK4819_ReadRegister(BK4819_REG_02);

		// 0 = no phase shift
		// 1 = 120deg phase shift
		// 2 = 180deg phase shift
		// 3 = 240deg phase shift
//		const uint8_t ctcss_shift = BK4819_GetCTCShift();
//		if (ctcss_shift > 0)
//			g_CTCSS_lost = true;

		if (interrupt_status_bits & BK4819_REG_02_DTMF_5TONE_FOUND)
		{	// save the RX'ed DTMF character
			const char c = DTMF_GetCharacter(BK4819_GetDTMF_5TONE_Code());
			if (c != 0xff)
			{
				if (g_current_function != FUNCTION_TRANSMIT)
				{
					if (g_setting_live_dtmf_decoder)
					{
						size_t len = strlen(g_dtmf_rx_live);
						if (len >= (sizeof(g_dtmf_rx_live) - 1))
						{	// make room
							memmove(&g_dtmf_rx_live[0], &g_dtmf_rx_live[1], sizeof(g_dtmf_rx_live) - 1);
							len--;
						}
						g_dtmf_rx_live[len++]  = c;
						g_dtmf_rx_live[len]    = 0;
						g_dtmf_rx_live_timeout = dtmf_rx_live_timeout_500ms;  // time till we delete it
						g_update_display        = true;
					}

					if (g_rx_vfo->dtmf_decoding_enable || g_setting_killed)
					{
						if (g_dtmf_rx_index >= (sizeof(g_dtmf_rx) - 1))
						{	// make room
							memmove(&g_dtmf_rx[0], &g_dtmf_rx[1], sizeof(g_dtmf_rx) - 1);
							g_dtmf_rx_index--;
						}
						g_dtmf_rx[g_dtmf_rx_index++] = c;
						g_dtmf_rx[g_dtmf_rx_index]   = 0;
						g_dtmf_rx_timeout           = dtmf_rx_timeout_500ms;  // time till we delete it
						g_dtmf_rx_pending           = true;

						DTMF_HandleRequest();
					}
				}
			}
		}

		if (interrupt_status_bits & BK4819_REG_02_CxCSS_TAIL)
			g_CxCSS_tail_found = true;

		if (interrupt_status_bits & BK4819_REG_02_CDCSS_LOST)
		{
			g_CDCSS_lost = true;
			g_CDCSS_code_type = BK4819_get_CDCSS_code_type();
		}

		if (interrupt_status_bits & BK4819_REG_02_CDCSS_FOUND)
			g_CDCSS_lost = false;

		if (interrupt_status_bits & BK4819_REG_02_CTCSS_LOST)
			g_CTCSS_lost = true;

		if (interrupt_status_bits & BK4819_REG_02_CTCSS_FOUND)
			g_CTCSS_lost = false;

		#ifdef ENABLE_VOX
			if (interrupt_status_bits & BK4819_REG_02_VOX_LOST)
			{
				g_vox_lost = true;
				g_vox_pause_count_down = 10;

				if (g_eeprom.vox_switch)
				{
					if (g_current_function == FUNCTION_POWER_SAVE && !g_rx_idle_mode)
					{
						g_power_save_10ms = power_save2_10ms;
						g_power_save_expired = false;
					}

					if (g_eeprom.dual_watch != DUAL_WATCH_OFF &&
					   (g_schedule_dual_watch || g_dual_watch_count_down_10ms < dual_watch_count_after_vox_10ms))
					{
						g_dual_watch_count_down_10ms = dual_watch_count_after_vox_10ms;
						g_schedule_dual_watch = false;

						// let the user see DW is not active
						g_dual_watch_active = false;
						g_update_status     = true;
					}
				}
			}

			if (interrupt_status_bits & BK4819_REG_02_VOX_FOUND)
			{
				g_vox_lost         = false;
				g_vox_pause_count_down = 0;
			}
		#endif

		if (interrupt_status_bits & BK4819_REG_02_SQUELCH_LOST)
		{
			g_squelch_lost = true;
			BK4819_set_GPIO_pin(BK4819_GPIO0_PIN28_GREEN, true);
		}

		if (interrupt_status_bits & BK4819_REG_02_SQUELCH_FOUND)
		{
			g_squelch_lost = false;
			BK4819_set_GPIO_pin(BK4819_GPIO0_PIN28_GREEN, false);
		}

		#ifdef ENABLE_AIRCOPY
			if (interrupt_status_bits & BK4819_REG_02_FSK_FIFO_ALMOST_FULL)
			{
				if (g_screen_to_display == DISPLAY_AIRCOPY && g_aircopy_state == AIRCOPY_RX)
				{
					unsigned int i;
					for (i = 0; i < 4; i++)
						g_aircopy_fsk_buffer[g_aircopy_fsk_write_index++] = BK4819_ReadRegister(BK4819_REG_5F);
					AIRCOPY_StorePacket();
				}
			}
		#endif
	}
}

void APP_EndTransmission(void)
{	// back to RX mode

	RADIO_SendEndOfTransmission();

	if (g_current_vfo->pTX->code_type != CODE_TYPE_OFF)
	{	// CTCSS/DCS is enabled

		//if (g_eeprom.tail_note_elimination && g_eeprom.repeater_tail_tone_elimination > 0)
		if (g_eeprom.tail_note_elimination)
		{	// send the CTCSS/DCS tail tone - allows the receivers to mute the usual FM squelch tail/crash
			RADIO_EnableCxCSS();
		}
		#if 0
			else
			{	// TX a short blank carrier
				// this gives the receivers time to mute RX audio before we drop carrier
				BK4819_ExitSubAu();
				SYSTEM_DelayMs(200);
			}
		#endif
	}

	RADIO_SetupRegisters(false);
}

#ifdef ENABLE_VOX
	static void APP_HandleVox(void)
	{
		if (g_setting_killed)
			return;

		if (g_vox_resume_count_down == 0)
		{
			if (g_vox_pause_count_down)
				return;
		}
		else
		{
			g_vox_lost         = false;
			g_vox_pause_count_down = 0;
		}

		#ifdef ENABLE_FMRADIO
			if (g_fm_radio_mode)
				return;
		#endif

		if (g_current_function == FUNCTION_RECEIVE || g_current_function == FUNCTION_MONITOR)
			return;

		if (g_scan_state_dir != SCAN_OFF || g_css_scan_mode != CSS_SCAN_MODE_OFF)
			return;

		if (g_vox_noise_detected)
		{
			if (g_vox_lost)
				g_vox_stop_count_down_10ms = vox_stop_count_down_10ms;
			else
			if (g_vox_stop_count_down_10ms == 0)
				g_vox_noise_detected = false;

			if (g_current_function == FUNCTION_TRANSMIT &&
			   !g_ptt_is_pressed &&
			   !g_vox_noise_detected)
			{
				if (g_flag_end_tx)
				{
					//if (g_current_function != FUNCTION_FOREGROUND)
						FUNCTION_Select(FUNCTION_FOREGROUND);
				}
				else
				{
					APP_EndTransmission();

					if (g_eeprom.repeater_tail_tone_elimination == 0)
					{
						//if (g_current_function != FUNCTION_FOREGROUND)
							FUNCTION_Select(FUNCTION_FOREGROUND);
					}
					else
						g_rtte_count_down = g_eeprom.repeater_tail_tone_elimination * 10;
				}

				g_update_status        = true;
				g_update_display       = true;
				g_flag_end_tx = false;
			}
			return;
		}

		if (g_vox_lost)
		{
			g_vox_noise_detected = true;

			if (g_current_function == FUNCTION_POWER_SAVE)
				FUNCTION_Select(FUNCTION_FOREGROUND);

			if (g_current_function != FUNCTION_TRANSMIT && g_serial_config_count_down_500ms == 0)
			{
				g_dtmf_reply_state = DTMF_REPLY_NONE;
				RADIO_PrepareTX();
				g_update_display = true;
			}
		}
	}
#endif

void APP_Update(void)
{
	#ifdef ENABLE_VOICE
		if (g_flag_play_queued_voice)
		{
			AUDIO_PlayQueuedVoice();
			g_flag_play_queued_voice = false;
		}
	#endif

	if (g_current_function == FUNCTION_TRANSMIT && (g_tx_timeout_reached || g_serial_config_count_down_500ms > 0))
	{	// transmitter timed out or must de-key
		g_tx_timeout_reached = false;

		g_flag_end_tx = true;
		APP_EndTransmission();

		AUDIO_PlayBeep(BEEP_880HZ_60MS_TRIPLE_BEEP);

		RADIO_Setg_vfo_state(VFO_STATE_TIMEOUT);

		GUI_DisplayScreen();
	}

	if (g_reduced_service || g_serial_config_count_down_500ms > 0)
		return;

	if (g_current_function != FUNCTION_TRANSMIT)
		APP_HandleFunction();

	#ifdef ENABLE_FMRADIO
		if (g_fm_radio_mode && g_fm_radio_count_down_500ms > 0)
			return;
	#endif

	#ifdef ENABLE_VOICE
		if (g_voice_write_index == 0)
	#endif
	{
		if (g_screen_to_display != DISPLAY_SCANNER &&
		    g_scan_state_dir != SCAN_OFF &&
		    g_schedule_scan_listen &&
		    !g_ptt_is_pressed)
		{	// scanning

			if (IS_FREQ_CHANNEL(g_next_channel))
			{
				if (g_current_function == FUNCTION_INCOMING)
					APP_StartListening(g_monitor_enabled ? FUNCTION_MONITOR : FUNCTION_RECEIVE, true);
				else
					FREQ_NextChannel();  // switch to next frequency
			}
			else
			{
				if (g_current_code_type == CODE_TYPE_OFF && g_current_function == FUNCTION_INCOMING)
					APP_StartListening(g_monitor_enabled ? FUNCTION_MONITOR : FUNCTION_RECEIVE, true);
				else
					USER_NextChannel();    // switch to next channel
			}

			g_scan_pause_mode      = false;
			g_rx_reception_mode    = RX_MODE_NONE;
			g_schedule_scan_listen = false;
		}
	}

	#ifdef ENABLE_VOICE
		if (g_css_scan_mode == CSS_SCAN_MODE_SCANNING && g_schedule_scan_listen && g_voice_write_index == 0)
	#else
		if (g_css_scan_mode == CSS_SCAN_MODE_SCANNING && g_schedule_scan_listen)
	#endif
	{
		MENU_SelectNextCode();

		g_schedule_scan_listen = false;
	}

	#ifdef ENABLE_NOAA
		#ifdef ENABLE_VOICE
			if (g_voice_write_index == 0)
		#endif
		{
			if (g_eeprom.dual_watch == DUAL_WATCH_OFF && g_is_noaa_mode && g_schedule_noaa)
			{
				NOAA_IncreaseChannel();
				RADIO_SetupRegisters(false);

				g_noaa_count_down_10ms = 7;      // 70ms
				g_schedule_noaa        = false;
			}
		}
	#endif

	// toggle between the VFO's if dual watch is enabled
	if (g_screen_to_display != DISPLAY_SCANNER && g_eeprom.dual_watch != DUAL_WATCH_OFF)
	{
		#if defined(ENABLE_UART) && defined(ENABLE_UART_DEBUG)
			//UART_SendText("dual watch\r\n");
		#endif

	#ifdef ENABLE_VOICE
		if (g_voice_write_index == 0)
	#endif
		{
			if (g_schedule_dual_watch)
			{
				if (g_scan_state_dir == SCAN_OFF && g_css_scan_mode == CSS_SCAN_MODE_OFF)
				{
				#ifdef ENABLE_FMRADIO
					if (!g_fm_radio_mode)
				#endif
					{
						if (!g_ptt_is_pressed &&
							g_dtmf_call_state == DTMF_CALL_STATE_NONE &&
							g_current_function != FUNCTION_POWER_SAVE)
						{
							DUALWATCH_Alternate();    // toggle between the two VFO's

							if (g_rx_vfo_is_active && g_screen_to_display == DISPLAY_MAIN)
								GUI_SelectNextDisplay(DISPLAY_MAIN);

							g_rx_vfo_is_active     = false;
							g_scan_pause_mode     = false;
							g_rx_reception_mode   = RX_MODE_NONE;
							g_schedule_dual_watch = false;
						}
					}
				}
			}
		}
	}

#ifdef ENABLE_FMRADIO
	if (g_schedule_fm                          &&
		g_fm_scan_state    != FM_SCAN_OFF      &&
		g_current_function != FUNCTION_MONITOR &&
		g_current_function != FUNCTION_RECEIVE &&
		g_current_function != FUNCTION_TRANSMIT)
	{	// switch to FM radio mode
		FM_Play();
		g_schedule_fm = false;
	}
#endif

#ifdef ENABLE_VOX
	if (g_eeprom.vox_switch)
		APP_HandleVox();
#endif

	if (g_schedule_power_save)
	{
		#ifdef ENABLE_NOAA
			if (
			#ifdef ENABLE_FMRADIO
			    g_fm_radio_mode ||
			#endif
				g_ptt_is_pressed                     ||
			    g_key_held                           ||
				g_eeprom.battery_save == 0           ||
			    g_scan_state_dir != SCAN_OFF         ||
			    g_css_scan_mode != CSS_SCAN_MODE_OFF ||
			    g_screen_to_display != DISPLAY_MAIN  ||
			    g_dtmf_call_state != DTMF_CALL_STATE_NONE)
			{
				g_battery_save_count_down_10ms   = battery_save_count_10ms;
			}
			else
			if ((IS_NOT_NOAA_CHANNEL(g_eeprom.screen_channel[0]) &&
			     IS_NOT_NOAA_CHANNEL(g_eeprom.screen_channel[1])) ||
			     !g_is_noaa_mode)
			{
				FUNCTION_Select(FUNCTION_POWER_SAVE);
			}
			else
			{
				g_battery_save_count_down_10ms = battery_save_count_10ms;
			}
		#else
			if (
				#ifdef ENABLE_FMRADIO
					g_fm_radio_mode ||
			    #endif
				g_ptt_is_pressed                     ||
			    g_key_held                           ||
				g_eeprom.battery_save == 0           ||
			    g_scan_state_dir != SCAN_OFF         ||
			    g_css_scan_mode != CSS_SCAN_MODE_OFF ||
			    g_screen_to_display != DISPLAY_MAIN  ||
			    g_dtmf_call_state != DTMF_CALL_STATE_NONE)
			{
				g_battery_save_count_down_10ms = battery_save_count_10ms;
			}
			else
			{
				FUNCTION_Select(FUNCTION_POWER_SAVE);
			}
		#endif

		g_schedule_power_save = false;
	}

#ifdef ENABLE_VOICE
	if (g_voice_write_index == 0)
#endif
	{
		if (g_power_save_expired && g_current_function == FUNCTION_POWER_SAVE)
		{	// wake up, enable RX then go back to sleep
			if (g_rx_idle_mode)
			{
				#if defined(ENABLE_UART) && defined(ENABLE_UART_DEBUG)
					//UART_SendText("ps wake up\r\n");
				#endif

				BK4819_Conditional_RX_TurnOn_and_GPIO6_Enable();

				#ifdef ENABLE_VOX
					if (g_eeprom.vox_switch)
						BK4819_EnableVox(g_eeprom.vox1_threshold, g_eeprom.vox0_threshold);
				#endif

				if (g_eeprom.dual_watch != DUAL_WATCH_OFF &&
					g_scan_state_dir == SCAN_OFF &&
					g_css_scan_mode == CSS_SCAN_MODE_OFF)
				{	// dual watch mode, toggle between the two VFO's
					DUALWATCH_Alternate();

					g_update_rssi = false;
				}

				FUNCTION_Init();

				g_power_save_10ms = power_save1_10ms; // come back here in a bit
				g_rx_idle_mode    = false;           // RX is awake
			}
			else
			if (g_eeprom.dual_watch == DUAL_WATCH_OFF ||
				g_scan_state_dir != SCAN_OFF ||
				g_css_scan_mode != CSS_SCAN_MODE_OFF ||
				g_update_rssi)
			{	// dual watch mode, go back to sleep

				updateRSSI(g_eeprom.rx_vfo);

				// go back to sleep

				g_power_save_10ms = g_eeprom.battery_save * 10;
				g_rx_idle_mode    = true;

				BK4819_DisableVox();
				BK4819_Sleep();
				BK4819_set_GPIO_pin(BK4819_GPIO6_PIN2, false);

				// Authentic device checked removed

			}
			else
			{
				// toggle between the two VFO's
				DUALWATCH_Alternate();

				g_update_rssi     = true;
				g_power_save_10ms = power_save1_10ms;
			}

			g_power_save_expired = false;
		}
	}
}

// called every 10ms
void APP_CheckKeys(void)
{
	const bool ptt_pressed = !GPIO_CheckBit(&GPIOC->DATA, GPIOC_PIN_PTT) && (g_serial_config_count_down_500ms == 0) && g_setting_tx_enable;

	key_code_t key;

	#ifdef ENABLE_AIRCOPY
		if (g_setting_killed ||
		   (g_screen_to_display == DISPLAY_AIRCOPY && g_aircopy_state != AIRCOPY_READY))
			return;
	#else
		if (g_setting_killed)
			return;
	#endif

	// *****************
	// PTT is treated completely separately from all the other buttons

	if (ptt_pressed)
	{	// PTT pressed
		if (!g_ptt_is_pressed)
		{
			if (++g_ptt_debounce >= 3)      // 30ms
			{	// start TX'ing

				g_boot_counter_10ms = 0;    // cancel the boot-up screen
				g_ptt_is_pressed    = true;
				g_ptt_was_released  = false;
				g_ptt_debounce      = 0;

				APP_ProcessKey(KEY_PTT, true, false);

				#if defined(ENABLE_UART) && defined(ENABLE_UART_DEBUG)
					UART_printf(" ptt key %3u %u %u\r\n", KEY_PTT, g_ptt_is_pressed, g_ptt_was_released);
				#endif
			}
		}
		else
			g_ptt_debounce = 0;
	}
	else
	{	// PTT released
		if (g_ptt_is_pressed)
		{
			if (++g_ptt_debounce >= 3)  // 30ms
			{	// stop TX'ing

				g_ptt_is_pressed   = false;
				g_ptt_was_released = true;
				g_ptt_debounce     = 0;

				APP_ProcessKey(KEY_PTT, false, false);

				#if defined(ENABLE_UART) && defined(ENABLE_UART_DEBUG)
					UART_printf(" ptt key %3u %u %u\r\n", KEY_PTT, g_ptt_is_pressed, g_ptt_was_released);
				#endif
			}
		}
		else
			g_ptt_debounce = 0;
	}

	// *****************
	// button processing (non-PTT)

	// scan the hardware keys
	key = KEYBOARD_Poll();

	g_boot_counter_10ms = 0;   // cancel boot screen/beeps

	if (g_serial_config_count_down_500ms > 0)
	{	// config upload/download in progress
		g_key_debounce_press  = 0;
		g_key_debounce_repeat = 0;
		g_key_prev            = KEY_INVALID;
		g_key_held            = false;
		g_fkey_pressed        = false;
		return;
	}

	if (key == KEY_INVALID || (g_key_prev != KEY_INVALID && key != g_key_prev))
	{	// key not pressed or different key pressed
		if (g_key_debounce_press > 0)
		{
			if (--g_key_debounce_press == 0)
			{
				if (g_key_prev != KEY_INVALID)
				{	// key now fully released

					#if defined(ENABLE_UART) && defined(ENABLE_UART_DEBUG)
						UART_printf(" old key %3u %3u, %3u %3u, %u\r\n", key, g_key_prev, g_key_debounce_press, g_key_debounce_repeat, g_key_held);
					#endif

					APP_ProcessKey(g_key_prev, false, g_key_held);
					g_key_debounce_press  = 0;
					g_key_debounce_repeat = 0;
					g_key_prev            = KEY_INVALID;
					g_key_held            = false;
					g_boot_counter_10ms   = 0;         // cancel the boot-up screen

					g_update_status       = true;
					g_update_display      = true;
				}
			}
			if (g_key_debounce_repeat > 0)
				g_key_debounce_repeat--;
		}
	}
	else
	{	// key pressed
		if (g_key_debounce_press < key_debounce_10ms)
		{
			if (++g_key_debounce_press >= key_debounce_10ms)
			{
				if (key != g_key_prev)
				{	// key now fully pressed
					g_key_debounce_repeat = key_debounce_10ms;
					g_key_held            = false;

					#if defined(ENABLE_UART) && defined(ENABLE_UART_DEBUG)
						UART_printf("\r\n new key %3u %3u, %3u %3u, %u\r\n", key, g_key_prev, g_key_debounce_press, g_key_debounce_repeat, g_key_held);
					#endif

					g_key_prev = key;

					APP_ProcessKey(g_key_prev, true, g_key_held);

					g_update_status  = true;
					g_update_display = true;
				}
			}
		}
		else
		if (g_key_debounce_repeat < key_long_press_10ms)
		{
			if (++g_key_debounce_repeat >= key_long_press_10ms)
			{	// key long press
				g_key_held = true;

				#if defined(ENABLE_UART) && defined(ENABLE_UART_DEBUG)
					UART_printf("long key %3u %3u, %3u %3u, %u\r\n", key, g_key_prev, g_key_debounce_press, g_key_debounce_repeat, g_key_held);
				#endif

				APP_ProcessKey(g_key_prev, true, g_key_held);

				//g_update_status  = true;
				//g_update_display = true;
			}
		}
		else
		if (key == KEY_UP || key == KEY_DOWN)
		{	// only the up and down keys are repeatable
			if (++g_key_debounce_repeat >= (key_long_press_10ms + key_repeat_10ms))
			{	// key repeat
				g_key_debounce_repeat -= key_repeat_10ms;

				#if defined(ENABLE_UART) && defined(ENABLE_UART_DEBUG)
					UART_printf("rept key %3u %3u, %3u %3u, %u\r\n", key, g_key_prev, g_key_debounce_press, g_key_debounce_repeat, g_key_held);
				#endif

				APP_ProcessKey(g_key_prev, true, g_key_held);

				//g_update_status  = true;
				//g_update_display = true;
			}
		}
	}

	// *****************
}

void APP_TimeSlice10ms(void)
{
	g_flash_light_blink_counter++;

	if (UART_IsCommandAvailable())
	{
		__disable_irq();
		UART_HandleCommand();
		__enable_irq();
	}

	// ***********

	if (g_flag_SaveVfo)
	{
		SETTINGS_SaveVfoIndices();
		g_flag_SaveVfo = false;
	}

	if (g_flag_SaveSettings)
	{
		SETTINGS_SaveSettings();
		g_flag_SaveSettings = false;
	}

	#ifdef ENABLE_FMRADIO
		if (g_flag_SaveFM)
		{
			SETTINGS_SaveFM();
			g_flag_SaveFM = false;
		}
	#endif

	if (g_flag_save_channel)
	{
		SETTINGS_SaveChannel(g_tx_vfo->channel_save, g_eeprom.tx_vfo, g_tx_vfo, g_flag_save_channel);
		g_flag_save_channel = false;

		RADIO_ConfigureChannel(g_eeprom.tx_vfo, VFO_CONFIGURE);
		RADIO_SetupRegisters(true);

		GUI_SelectNextDisplay(DISPLAY_MAIN);
	}

	// ***********

	if (g_serial_config_count_down_500ms > 0)
	{	// config upload/download is running
		if (g_update_display)
			GUI_DisplayScreen();
		if (g_update_status)
			UI_DisplayStatus(false);
		return;
	}

	// ***********

	#ifdef ENABLE_BOOT_BEEPS
		if (g_boot_counter_10ms > 0 && (g_boot_counter_10ms % 25) == 0)
			AUDIO_PlayBeep(BEEP_880HZ_40MS_OPTIONAL);
	#endif

	if (g_reduced_service)
		return;


	#ifdef ENABLE_AIRCOPY
		if (g_screen_to_display == DISPLAY_AIRCOPY && g_aircopy_state == AIRCOPY_TX)
		{
			if (g_aircopy_send_count_down_10ms > 0)
			{
				if (--g_aircopy_send_count_down_10ms == 0)
				{
					AIRCOPY_SendMessage(0xff);
					GUI_DisplayScreen();
				}
			}
		}
	#endif

	#ifdef ENABLE_AM_FIX
//		if (g_eeprom.vfo_info[g_eeprom.rx_vfo].am_mode && g_setting_am_fix)
		if (g_rx_vfo->am_mode && g_setting_am_fix)
			AM_fix_10ms(g_eeprom.rx_vfo);
	#endif

	if (g_current_function != FUNCTION_POWER_SAVE || !g_rx_idle_mode)
		APP_CheckRadioInterrupts();

	if (g_current_function == FUNCTION_TRANSMIT)
	{	// transmitting
		#ifdef ENABLE_AUDIO_BAR
			if (g_setting_mic_bar && (g_flash_light_blink_counter % (150 / 10)) == 0) // once every 150ms
				UI_DisplayAudioBar(true);
		#endif
	}

	if (g_update_display)
		GUI_DisplayScreen();

	if (g_update_status)
		UI_DisplayStatus(false);

	// Skipping authentic device checks

	#ifdef ENABLE_FMRADIO
		if (g_fm_radio_mode && g_fm_radio_count_down_500ms > 0)
			return;
	#endif

	if (g_flash_light_state == FLASHLIGHT_BLINK && (g_flash_light_blink_counter & 15u) == 0)
		GPIO_FlipBit(&GPIOC->DATA, GPIOC_PIN_FLASHLIGHT);

	#ifdef ENABLE_VOX
		if (g_vox_resume_count_down > 0)
			g_vox_resume_count_down--;

		if (g_vox_pause_count_down > 0)
			g_vox_pause_count_down--;
	#endif

	if (g_current_function == FUNCTION_TRANSMIT)
	{
		#ifdef ENABLE_ALARM
			if (g_alarm_state == ALARM_STATE_TXALARM || g_alarm_state == ALARM_STATE_ALARM)
			{
				uint16_t Tone;

				g_alarm_running_counter++;
				g_alarm_tone_counter++;

				Tone = 500 + (g_alarm_tone_counter * 25);
				if (Tone > 1500)
				{
					Tone = 500;
					g_alarm_tone_counter = 0;
				}

				BK4819_SetScrambleFrequencyControlWord(Tone);

				if (g_eeprom.alarm_mode == ALARM_MODE_TONE && g_alarm_running_counter == 512)
				{
					g_alarm_running_counter = 0;

					if (g_alarm_state == ALARM_STATE_TXALARM)
					{
						g_alarm_state = ALARM_STATE_ALARM;

						RADIO_EnableCxCSS();
						BK4819_SetupPowerAmplifier(0, 0);
						BK4819_set_GPIO_pin(BK4819_GPIO5_PIN1, false);
						BK4819_Enable_AfDac_DiscMode_TxDsp();
						BK4819_set_GPIO_pin(BK4819_GPIO1_PIN29_RED, false);

						GUI_DisplayScreen();
					}
					else
					{
						g_alarm_state = ALARM_STATE_TXALARM;

						GUI_DisplayScreen();

						BK4819_set_GPIO_pin(BK4819_GPIO1_PIN29_RED, true);
						RADIO_SetTxParameters();
						BK4819_TransmitTone(true, 500);
						SYSTEM_DelayMs(2);
						GPIO_SetBit(&GPIOC->DATA, GPIOC_PIN_AUDIO_PATH);

						g_enable_speaker    = true;
						g_alarm_tone_counter = 0;
					}
				}
			}
		#endif

		// repeater tail tone elimination
		if (g_rtte_count_down > 0)
		{
			if (--g_rtte_count_down == 0)
			{
				FUNCTION_Select(FUNCTION_FOREGROUND);
				g_update_status  = true;
				g_update_display = true;
			}
		}
	}

	#ifdef ENABLE_FMRADIO
		if (g_fm_radio_mode && g_fm_restore_count_down_10ms > 0)
		{
			if (--g_fm_restore_count_down_10ms == 0)
			{	// switch back to FM radio mode
				FM_Start();
				GUI_SelectNextDisplay(DISPLAY_FM);
			}
		}
	#endif

	if (g_screen_to_display == DISPLAY_SCANNER)
	{
		uint32_t                 Result;
		int32_t                  Delta;
		uint16_t                 CtcssFreq;
		BK4819_CSS_scan_result_t ScanResult;

		g_scan_freq_css_timer_10ms++;

		if (g_scan_delay_10ms > 0)
		{
			if (--g_scan_delay_10ms > 0)
			{
				APP_CheckKeys();
				return;
			}
		}

		if (g_scanner_edit_state != SCAN_EDIT_STATE_NONE)
		{	// waiting for user input choice
			APP_CheckKeys();
			return;
		}

		g_update_display = true;
		GUI_SelectNextDisplay(DISPLAY_SCANNER);

		switch (g_scan_css_state)
		{
			case SCAN_CSS_STATE_OFF:

				if (g_scan_freq_css_timer_10ms >= scan_freq_css_timeout_10ms)
				{	// freq/css scan timeout
					#ifdef ENABLE_CODE_SCAN_TIMEOUT
						BK4819_DisableFrequencyScan();
						g_scan_css_state = SCAN_CSS_STATE_FREQ_FAILED;
						g_update_status  = true;
						g_update_display = true;
						break;
					#endif
				}

				if (!BK4819_GetFrequencyScanResult(&Result))
					break;   // still scanning

				// accept only within 1kHz
				Delta = Result - g_scan_frequency;
				g_scan_hit_count = (abs(Delta) < 100) ? g_scan_hit_count + 1 : 0;

				BK4819_DisableFrequencyScan();

				g_scan_frequency = Result;

				if (g_scan_hit_count < 3)
				{	// keep scanning for an RF carrier
					BK4819_EnableFrequencyScan();
				}
				else
				{	// RF carrier found
					// stop RF the scan and move on too the CTCSS/CDCSS scan

					BK4819_SetScanFrequency(g_scan_frequency);

					g_scan_css_result_code     = 0xFF;
					g_scan_css_result_type     = 0xFF;
					g_scan_hit_count           = 0;
					g_scan_use_css_result      = false;
					g_scan_freq_css_timer_10ms = 0;
					g_scan_css_state           = SCAN_CSS_STATE_SCANNING;

					GUI_SelectNextDisplay(DISPLAY_SCANNER);
					g_update_status  = true;
					g_update_display = true;
				}

				g_scan_delay_10ms = scan_freq_css_delay_10ms;
				break;

			case SCAN_CSS_STATE_SCANNING:

				if (g_scan_freq_css_timer_10ms >= scan_freq_css_timeout_10ms)
				{	// timeout
					#if defined(ENABLE_CODE_SCAN_TIMEOUT)
						BK4819_Disable();
						g_scan_css_state = SCAN_CSS_STATE_FAILED;
						g_update_status  = true;
						g_update_display = true;
						break;
					#elif defined(ENABLE_FREQ_CODE_SCAN_TIMEOUT)
						if (!g_scan_single_frequency)
						{
							BK4819_Disable();
							g_scan_css_state = SCAN_CSS_STATE_FAILED;
							g_update_status  = true;
							g_update_display = true;
							break;
						}
					#endif
				}

				ScanResult = BK4819_GetCxCSSScanResult(&Result, &CtcssFreq);
				if (ScanResult == BK4819_CSS_RESULT_NOT_FOUND)
					break;

				BK4819_Disable();

				if (ScanResult == BK4819_CSS_RESULT_CDCSS)
				{	// found a CDCSS code
					const uint8_t Code = DCS_GetCdcssCode(Result);
					if (Code != 0xFF)
					{
						g_scan_css_result_code = Code;
						g_scan_css_result_type = CODE_TYPE_DIGITAL;
						g_scan_css_state       = SCAN_CSS_STATE_FOUND;
						g_scan_use_css_result  = true;
						g_update_status        = true;
						g_update_display       = true;
					}
				}
				else
				if (ScanResult == BK4819_CSS_RESULT_CTCSS)
				{	// found a CTCSS tone
					const uint8_t code = DCS_GetCtcssCode(CtcssFreq);
					if (code != 0xFF)
					{
						if (code == g_scan_css_result_code &&
						    g_scan_css_result_type == CODE_TYPE_CONTINUOUS_TONE)
						{
							if (++g_scan_hit_count >= 2)
							{
								g_scan_css_state      = SCAN_CSS_STATE_FOUND;
								g_scan_use_css_result = true;
								g_update_status       = true;
								g_update_display      = true;
							}
						}
						else
							g_scan_hit_count = 0;

						g_scan_css_result_type = CODE_TYPE_CONTINUOUS_TONE;
						g_scan_css_result_code = code;
					}
				}

				if (g_scan_css_state == SCAN_CSS_STATE_OFF || g_scan_css_state == SCAN_CSS_STATE_SCANNING)
				{	// re-start scan
					BK4819_SetScanFrequency(g_scan_frequency);
					g_scan_delay_10ms = scan_freq_css_delay_10ms;
				}

				GUI_SelectNextDisplay(DISPLAY_SCANNER);
				break;

			//case SCAN_CSS_STATE_FOUND:
			//case SCAN_CSS_STATE_FAILED:
			//case SCAN_CSS_STATE_FREQ_FAILED:
			default:
				break;
		}
	}

	APP_CheckKeys();
}

void cancelUserInputModes(void)
{
	if (g_ask_to_save)
	{
		g_ask_to_save  = false;
		g_update_display = true;
	}
	if (g_ask_to_delete)
	{
		g_ask_to_delete  = false;
		g_update_display = true;
	}

	if (g_dtmf_input_mode || g_dtmf_input_box_index > 0)
	{
		DTMF_clear_input_box();
		#ifdef ENABLE_FMRADIO
			if (g_fm_radio_mode)
				g_request_display_screen = DISPLAY_FM;
			else
				g_request_display_screen = DISPLAY_MAIN;
		#else
			g_request_display_screen = DISPLAY_MAIN;
		#endif
		g_update_display         = true;
	}

	if (g_fkey_pressed || g_key_input_count_down > 0 || g_input_box_index > 0)
	{
		g_fkey_pressed         = false;
		g_input_box_index      = 0;
		g_key_input_count_down = 0;
		g_update_status        = true;
		g_update_display       = true;
	}
}

// this is called once every 500ms
void APP_TimeSlice500ms(void)
{
	bool exit_menu = false;

	// Skipped authentic device check

	if (g_serial_config_count_down_500ms > 0)
	{	// config upload/download is running
		return;
	}

	if (g_keypad_locked > 0)
		if (--g_keypad_locked == 0)
			g_update_display = true;

	if (g_key_input_count_down > 0)
	{
		if (--g_key_input_count_down == 0)
		{
			cancelUserInputModes();

			if (g_beep_to_play != BEEP_NONE)
			{
				AUDIO_PlayBeep(g_beep_to_play);
				g_beep_to_play = BEEP_NONE;
			}
		}
	}

	if (g_dtmf_rx_live_timeout > 0)
	{
		#ifdef ENABLE_RSSI_BAR
			if (center_line == CENTER_LINE_DTMF_DEC ||
				center_line == CENTER_LINE_NONE)  // wait till the center line is free for us to use before timing out
		#endif
		{
			if (--g_dtmf_rx_live_timeout == 0)
			{
				if (g_dtmf_rx_live[0] != 0)
				{
					memset(g_dtmf_rx_live, 0, sizeof(g_dtmf_rx_live));
					g_update_display   = true;
				}
			}
		}
	}

	if (g_menu_count_down > 0)
		if (--g_menu_count_down == 0)
			exit_menu = (g_screen_to_display == DISPLAY_MENU);	// exit menu mode

	if (g_dtmf_rx_timeout > 0)
		if (--g_dtmf_rx_timeout == 0)
			DTMF_clear_RX();

	// Skipped authentic device check

	#ifdef ENABLE_FMRADIO
		if (g_fm_radio_count_down_500ms > 0)
		{
			g_fm_radio_count_down_500ms--;
			if (g_fm_radio_mode)           // 1of11
				return;
		}
	#endif

	if (g_backlight_count_down > 0 && !g_ask_to_save && g_css_scan_mode == CSS_SCAN_MODE_OFF)
		if (g_screen_to_display != DISPLAY_MENU || g_menu_cursor != MENU_ABR) // don't turn off backlight if user is in backlight menu option
			if (--g_backlight_count_down == 0)
				if (g_eeprom.backlight < (ARRAY_SIZE(g_sub_menu_backlight) - 1))
					GPIO_ClearBit(&GPIOB->DATA, GPIOB_PIN_BACKLIGHT);   // turn backlight off

	if (g_reduced_service)
	{
		BOARD_ADC_GetBatteryInfo(&g_usb_current_voltage, &g_usb_current);

		if (g_usb_current > 500 || g_battery_calibration[3] < g_usb_current_voltage)
		{
			#ifdef ENABLE_OVERLAY
				overlay_FLASH_RebootToBootloader();
			#else
				NVIC_SystemReset();
			#endif
		}

		return;
	}

	g_battery_check_counter++;

	// Skipped authentic device check

	if ((g_battery_check_counter & 1) == 0)
	{
		BOARD_ADC_GetBatteryInfo(&g_battery_voltages[g_battery_voltage_index++], &g_usb_current);
		if (g_battery_voltage_index > 3)
			g_battery_voltage_index = 0;
		BATTERY_GetReadings(true);
	}

	// regular display updates (once every 2 sec) - if need be
	if ((g_battery_check_counter & 3) == 0)
	{
		if (g_charging_with_type_c || g_setting_battery_text > 0)
			g_update_status = true;
		#ifdef ENABLE_SHOW_CHARGE_LEVEL
			if (g_charging_with_type_c)
				g_update_display = true;
		#endif
	}

#ifdef ENABLE_FMRADIO
	if (g_fm_scan_state == FM_SCAN_OFF || g_ask_to_save)
#endif
	{
	#ifdef ENABLE_AIRCOPY
		if (g_screen_to_display != DISPLAY_AIRCOPY)
	#endif
		{
			if (g_css_scan_mode == CSS_SCAN_MODE_OFF &&
			    g_scan_state_dir == SCAN_OFF         &&
			   (g_screen_to_display != DISPLAY_SCANNER    ||
				g_scan_css_state == SCAN_CSS_STATE_FOUND  ||
				g_scan_css_state == SCAN_CSS_STATE_FAILED ||
				g_scan_css_state == SCAN_CSS_STATE_FREQ_FAILED))
			{

				if (g_eeprom.auto_keypad_lock       &&
				    g_key_lock_count_down_500ms > 0 &&
				   !g_dtmf_input_mode               &&
				    g_input_box_index == 0          &&
				    g_screen_to_display != DISPLAY_MENU)
				{
					if (--g_key_lock_count_down_500ms == 0)
					{	// lock the keyboard
						g_eeprom.key_lock = true;
						g_update_status   = true;
					}
				}

				if (exit_menu)
				{
					g_menu_count_down = 0;

					if (g_eeprom.backlight == 0)
					{
						g_backlight_count_down = 0;
						GPIO_ClearBit(&GPIOB->DATA, GPIOB_PIN_BACKLIGHT);	// turn the backlight OFF
					}

					if (g_input_box_index > 0 || g_dtmf_input_mode)
						AUDIO_PlayBeep(BEEP_500HZ_60MS_DOUBLE_BEEP_OPTIONAL);
/*
					if (g_screen_to_display == DISPLAY_SCANNER)
					{
						BK4819_StopScan();

						RADIO_ConfigureChannel(0, VFO_CONFIGURE_RELOAD);
						RADIO_ConfigureChannel(1, VFO_CONFIGURE_RELOAD);

						RADIO_SetupRegisters(true);
					}
*/
					DTMF_clear_input_box();

					g_fkey_pressed    = false;
					g_input_box_index = 0;

					g_ask_to_save     = false;
					g_ask_to_delete   = false;

					g_update_status   = true;
					g_update_display  = true;

					{
						gui_display_type_t disp = DISPLAY_INVALID;

						#ifdef ENABLE_FMRADIO
							if (g_fm_radio_mode &&
								g_current_function != FUNCTION_RECEIVE &&
								g_current_function != FUNCTION_MONITOR &&
								g_current_function != FUNCTION_TRANSMIT)
							{
								disp = DISPLAY_FM;
							}
						#endif

						if (disp == DISPLAY_INVALID)
						{
							#ifndef ENABLE_CODE_SCAN_TIMEOUT
								if (g_screen_to_display != DISPLAY_SCANNER)
							#endif
									disp = DISPLAY_MAIN;
						}

						if (disp != DISPLAY_INVALID)
							GUI_SelectNextDisplay(disp);
					}
				}
			}
		}
	}

	if (g_current_function != FUNCTION_POWER_SAVE && g_current_function != FUNCTION_TRANSMIT)
		updateRSSI(g_eeprom.rx_vfo);

	#ifdef ENABLE_FMRADIO
		if (!g_ptt_is_pressed && g_fm_resume_count_down_500ms > 0)
		{
			if (--g_fm_resume_count_down_500ms == 0)
			{
				RADIO_Setg_vfo_state(VFO_STATE_NORMAL);

				if (g_current_function != FUNCTION_RECEIVE  &&
				    g_current_function != FUNCTION_TRANSMIT &&
				    g_current_function != FUNCTION_MONITOR  &&
					g_fm_radio_mode)
				{	// switch back to FM radio mode
					FM_Start();
					GUI_SelectNextDisplay(DISPLAY_FM);
				}
			}
		}
	#endif

	if (g_low_battery)
	{
		g_low_battery_blink = ++g_low_batteryCountdown & 1;

		UI_DisplayBattery(0, g_low_battery_blink);

		if (g_current_function != FUNCTION_TRANSMIT)
		{	// not transmitting

			if (g_low_batteryCountdown < 30)
			{
				if (g_low_batteryCountdown == 29 && !g_charging_with_type_c)
					AUDIO_PlayBeep(BEEP_500HZ_60MS_DOUBLE_BEEP);
			}
			else
			{
				g_low_batteryCountdown = 0;

				if (!g_charging_with_type_c)
				{	// not on charge

					AUDIO_PlayBeep(BEEP_500HZ_60MS_DOUBLE_BEEP);

					#ifdef ENABLE_VOICE
						AUDIO_SetVoiceID(0, VOICE_ID_LOW_VOLTAGE);
					#endif

					if (g_battery_display_level == 0)
					{
						#ifdef ENABLE_VOICE
							AUDIO_PlaySingleVoice(true);
						#endif

						g_reduced_service = true;

						FUNCTION_Select(FUNCTION_POWER_SAVE);

						ST7565_HardwareReset();

						if (g_eeprom.backlight < (ARRAY_SIZE(g_sub_menu_backlight) - 1))
							GPIO_ClearBit(&GPIOB->DATA, GPIOB_PIN_BACKLIGHT);  // turn the backlight off
					}
					#ifdef ENABLE_VOICE
						else
							AUDIO_PlaySingleVoice(false);
					#endif
				}
			}
		}
	}

	if (g_current_function != FUNCTION_TRANSMIT)
	{
		if (g_dtmf_decode_ring_count_down_500ms > 0)
		{	// make "ring-ring" sound
			g_dtmf_decode_ring_count_down_500ms--;
			AUDIO_PlayBeep(BEEP_880HZ_200MS);
		}
	}
	else
		g_dtmf_decode_ring_count_down_500ms = 0;

	if (g_dtmf_call_state  != DTMF_CALL_STATE_NONE &&
	    g_current_function != FUNCTION_TRANSMIT &&
	    g_current_function != FUNCTION_RECEIVE)
	{
		if (g_dtmf_auto_reset_time_500ms > 0)
		{
			if (--g_dtmf_auto_reset_time_500ms == 0)
			{
				if (g_dtmf_call_state == DTMF_CALL_STATE_RECEIVED && g_eeprom.dtmf_auto_reset_time >= DTMF_HOLD_MAX)
					g_dtmf_call_state = DTMF_CALL_STATE_RECEIVED_STAY;     // keep message on-screen till a key is pressed
				else
					g_dtmf_call_state = DTMF_CALL_STATE_NONE;
				g_update_display  = true;
			}
		}

//		if (g_dtmf_call_state != DTMF_CALL_STATE_RECEIVED_STAY)
//		{
//			g_dtmf_call_state = DTMF_CALL_STATE_NONE;
//			g_update_display  = true;
//		}
	}

	if (g_dtmf_is_tx && g_dtmf_tx_stop_count_down_500ms > 0)
	{
		if (--g_dtmf_tx_stop_count_down_500ms == 0)
		{
			g_dtmf_is_tx     = false;
			g_update_display = true;
		}
	}

	#ifdef ENABLE_SHOW_TX_TIMEOUT
		if (g_current_function == FUNCTION_TRANSMIT && (g_tx_timer_count_down_500ms & 1))
			UI_DisplayTXCountdown(true);
	#endif
}

#if defined(ENABLE_ALARM) || defined(ENABLE_TX1750)
	static void ALARM_Off(void)
	{
		g_alarm_state = ALARM_STATE_OFF;

		GPIO_ClearBit(&GPIOC->DATA, GPIOC_PIN_AUDIO_PATH);
		g_enable_speaker = false;

		if (g_eeprom.alarm_mode == ALARM_MODE_TONE)
		{
			RADIO_SendEndOfTransmission();
			RADIO_EnableCxCSS();
		}

		#ifdef ENABLE_VOX
			g_vox_resume_count_down = 80;
		#endif

		SYSTEM_DelayMs(5);

		RADIO_SetupRegisters(true);

		if (g_screen_to_display != DISPLAY_MENU)     // 1of11 .. don't close the menu
			g_request_display_screen = DISPLAY_MAIN;
	}
#endif

void CHANNEL_Next(const bool flag, const scan_state_dir_t scan_direction)
{
	RADIO_SelectVfos();

	g_next_channel   = g_rx_vfo->channel_save;
	g_current_scan_list = SCAN_NEXT_CHAN_SCANLIST1;
	g_scan_state_dir    = scan_direction;

	if (g_next_channel <= USER_CHANNEL_LAST)
	{	// channel mode
		if (flag)
			g_restore_channel = g_next_channel;
		USER_NextChannel();
	}
	else
	{	// frequency mode
		if (flag)
			g_restore_frequency = g_rx_vfo->freq_config_rx.frequency;
		FREQ_NextChannel();
	}

	g_scan_pause_delay_in_10ms = scan_pause_delay_in_2_10ms;
	g_schedule_scan_listen    = false;
	g_rx_reception_mode       = RX_MODE_NONE;
	g_scan_pause_mode         = false;
	g_scan_keep_frequency     = false;
}

static void APP_ProcessKey(const key_code_t Key, const bool key_pressed, const bool key_held)
{
	bool flag = false;

	if (Key == KEY_INVALID && !key_pressed && !key_held)
		return;

	// reset the state so as to remove it from the screen
	if (Key != KEY_INVALID && Key != KEY_PTT)
		RADIO_Setg_vfo_state(VFO_STATE_NORMAL);
/*
	// remember the current backlight state (on / off)
	const bool backlight_was_on = GPIO_CheckBit(&GPIOB->DATA, GPIOB_PIN_BACKLIGHT);

	if (Key == KEY_EXIT && !backlight_was_on && g_eeprom.backlight > 0)
	{	// just turn the light on for now so the user can see what's what
		backlight_turn_on();
		g_beep_to_play = BEEP_NONE;
		return;
	}
*/
	// turn the backlight on
	if (key_pressed)
		if (Key != KEY_PTT || g_setting_backlight_on_tx_rx == 1 || g_setting_backlight_on_tx_rx == 3)
			backlight_turn_on();

	if (g_current_function == FUNCTION_POWER_SAVE)
		FUNCTION_Select(FUNCTION_FOREGROUND);

	// stay awake - for now
	g_battery_save_count_down_10ms = battery_save_count_10ms;

	// keep the auto keylock at bay
	if (g_eeprom.auto_keypad_lock)
		g_key_lock_count_down_500ms = key_lock_timeout_500ms;

	if (g_fkey_pressed && (Key == KEY_PTT || Key == KEY_EXIT || Key == KEY_SIDE1 || Key == KEY_SIDE2))
	{	// cancel the F-key
		g_fkey_pressed  = false;
		g_update_status = true;
	}

	// ********************

	if (g_eeprom.key_lock && g_current_function != FUNCTION_TRANSMIT && Key != KEY_PTT)
	{	// keyboard is locked

		if (Key == KEY_F)
		{	// function/key-lock key

			if (!key_pressed)
				return;

			if (key_held)
			{	// unlock the keypad
				g_eeprom.key_lock       = false;
				g_request_save_settings = true;
				g_update_status         = true;

				#ifdef ENABLE_VOICE
					g_another_voice_id = VOICE_ID_UNLOCK;
				#endif

				AUDIO_PlayBeep(BEEP_1KHZ_60MS_OPTIONAL);
			}

			return;
		}

		if (Key != KEY_SIDE1 && Key != KEY_SIDE2)
		{
			if (!key_pressed || key_held)
				return;

			// keypad is locked, let the user know
			g_keypad_locked  = 4;          // 2 second pop-up
			g_update_display = true;

			#ifdef ENABLE_FMRADIO
				if (!g_fm_radio_mode)  // don't beep when the FM radio is on, it cause bad gaps and loud clicks
			#endif
					g_beep_to_play = BEEP_500HZ_60MS_DOUBLE_BEEP_OPTIONAL;

			return;
		}
	}

	// key beep
	if (Key != KEY_PTT && !key_held && key_pressed)
		g_beep_to_play = BEEP_1KHZ_60MS_OPTIONAL;

	// ********************

	if (Key == KEY_EXIT && key_held && key_pressed)
	{	// exit key held pressed

		// clear the live DTMF decoder
		if (g_dtmf_rx_live[0] != 0)
		{
			memset(g_dtmf_rx_live, 0, sizeof(g_dtmf_rx_live));
			g_dtmf_rx_live_timeout = 0;
			g_update_display       = true;
		}

		// cancel user input
		cancelUserInputModes();
	}

	if (key_pressed && g_screen_to_display == DISPLAY_MENU)
		g_menu_count_down = menu_timeout_500ms;

	// cancel the ringing
	if (key_pressed && g_dtmf_decode_ring_count_down_500ms > 0)
		g_dtmf_decode_ring_count_down_500ms = 0;

	// ********************

	#pragma GCC diagnostic push
	#pragma GCC diagnostic ignored "-Wtype-limits"

	if (g_scan_state_dir != SCAN_OFF || g_css_scan_mode != CSS_SCAN_MODE_OFF)
	{	// FREQ/CTCSS/CDCSS scanning

		if ((Key >= KEY_0 && Key <= KEY_9) || Key == KEY_F)
		{
			if (key_pressed && !key_held)
				AUDIO_PlayBeep(BEEP_500HZ_60MS_DOUBLE_BEEP_OPTIONAL);
			return;
		}
	}

	#pragma GCC diagnostic pop

	// ********************

	if (Key == KEY_PTT && g_ptt_was_pressed)
	{
		flag = key_held;
		if (!key_pressed)
		{
			flag = true;
			g_ptt_was_pressed = false;
		}
	}

	// this bit of code has caused soooooo many problems due
	// to this causing key releases to be ignored :( .. 1of11
	if (Key != KEY_PTT && g_ptt_was_released)
	{
		if (key_held)
			flag = true;
		if (key_pressed)	// I now use key released for button press detections
		{
			flag = true;
			g_ptt_was_released = false;
		}

		#if defined(ENABLE_UART) && defined(ENABLE_UART_DEBUG)
			UART_printf("proc key 1 %3u %u %u %u %u\r\n", Key, key_pressed, key_held, g_fkey_pressed, flag);
		#endif
	}

	#if defined(ENABLE_UART) && defined(ENABLE_UART_DEBUG)
		UART_printf("proc key 2 %3u %u %u %u %u\r\n", Key, key_pressed, key_held, g_fkey_pressed, flag);
	#endif

	if (!flag)  // this flag is responsible for keys being ignored :(
	{
		if (g_current_function == FUNCTION_TRANSMIT)
		{	// transmitting

			#if defined(ENABLE_ALARM) || defined(ENABLE_TX1750)
				if (g_alarm_state == ALARM_STATE_OFF)
			#endif
			{
				char Code;

				if (Key == KEY_PTT)
				{
					GENERIC_Key_PTT(key_pressed);
					goto Skip;
				}

				if (Key == KEY_SIDE2)
				{	// transmit 1750Hz tone
					Code = 0xFE;
				}
				else
				{
					Code = DTMF_GetCharacter(Key - KEY_0);
					if (Code == 0xFF)
						goto Skip;

					// transmit DTMF keys
				}

				if (!key_pressed || key_held)
				{
					if (!key_pressed)
					{
						GPIO_ClearBit(&GPIOC->DATA, GPIOC_PIN_AUDIO_PATH);

						g_enable_speaker = false;

						BK4819_ExitDTMF_TX(false);

						if (g_current_vfo->scrambling_type == 0 || !g_setting_scramble_enable)
							BK4819_DisableScramble();
						else
							BK4819_EnableScramble(g_current_vfo->scrambling_type - 1);
					}
				}
				else
				{
					if (g_eeprom.dtmf_side_tone)
					{	// user will here the DTMF tones in speaker
						GPIO_SetBit(&GPIOC->DATA, GPIOC_PIN_AUDIO_PATH);
						g_enable_speaker = true;
					}

					BK4819_DisableScramble();

					if (Code == 0xFE)
						BK4819_TransmitTone(g_eeprom.dtmf_side_tone, 1750);
					else
						BK4819_PlayDTMFEx(g_eeprom.dtmf_side_tone, Code);
				}
			}
			#if defined(ENABLE_ALARM) || defined(ENABLE_TX1750)
				else
				if ((!key_held && key_pressed) || (g_alarm_state == ALARM_STATE_TX1750 && key_held && !key_pressed))
				{
					ALARM_Off();

					if (g_eeprom.repeater_tail_tone_elimination == 0)
						FUNCTION_Select(FUNCTION_FOREGROUND);
					else
						g_rtte_count_down = g_eeprom.repeater_tail_tone_elimination * 10;

					if (Key == KEY_PTT)
						g_ptt_was_pressed  = true;
					else
					if (!key_held)
						g_ptt_was_released = true;
				}
			#endif
		}
		else
		if (Key != KEY_SIDE1 && Key != KEY_SIDE2)
		{
			switch (g_screen_to_display)
			{
				case DISPLAY_MAIN:
					MAIN_ProcessKeys(Key, key_pressed, key_held);
					break;

				#ifdef ENABLE_FMRADIO
					case DISPLAY_FM:
						FM_ProcessKeys(Key, key_pressed, key_held);
						break;
				#endif

				case DISPLAY_MENU:
					MENU_ProcessKeys(Key, key_pressed, key_held);
					break;

				case DISPLAY_SCANNER:
					SCANNER_ProcessKeys(Key, key_pressed, key_held);
					break;

				#ifdef ENABLE_AIRCOPY
					case DISPLAY_AIRCOPY:
						AIRCOPY_ProcessKeys(Key, key_pressed, key_held);
						break;
				#endif

				case DISPLAY_INVALID:
				default:
					break;
			}
		}
		else
		#ifdef ENABLE_AIRCOPY
			if (g_screen_to_display != DISPLAY_SCANNER && g_screen_to_display != DISPLAY_AIRCOPY)
		#else
			if (g_screen_to_display != DISPLAY_SCANNER)
		#endif
		{
			ACTION_Handle(Key, key_pressed, key_held);
		}
		else
		{
			#ifdef ENABLE_FMRADIO
				if (!g_fm_radio_mode)
			#endif
					if (!key_held && key_pressed)
						g_beep_to_play = BEEP_500HZ_60MS_DOUBLE_BEEP_OPTIONAL;
		}
	}

Skip:
	if (g_beep_to_play != BEEP_NONE)
	{
		AUDIO_PlayBeep(g_beep_to_play);
		g_beep_to_play = BEEP_NONE;
	}

	if (g_flag_AcceptSetting)
	{
		g_menu_count_down = menu_timeout_500ms;

		MENU_AcceptSetting();

		g_flag_refresh_menu = true;
		g_flag_AcceptSetting  = false;
	}

	if (g_flag_stop_scan)
	{
		BK4819_StopScan();
		g_flag_stop_scan = false;
	}

	if (g_request_save_settings)
	{
		if (!key_held)
			SETTINGS_SaveSettings();
		else
			g_flag_SaveSettings = 1;
		g_request_save_settings = false;
		g_update_status        = true;
	}

	#ifdef ENABLE_FMRADIO
		if (g_request_save_fm)
		{
			if (!key_held)
				SETTINGS_SaveFM();
			else
				g_flag_SaveFM = true;
			g_request_save_fm = false;
		}
	#endif

	if (g_request_save_vfo)
	{
		if (!key_held)
			SETTINGS_SaveVfoIndices();
		else
			g_flag_SaveVfo = true;
		g_request_save_vfo = false;
	}

	if (g_request_save_channel > 0)
	{
		if (!key_held)
		{
			SETTINGS_SaveChannel(g_tx_vfo->channel_save, g_eeprom.tx_vfo, g_tx_vfo, g_request_save_channel);

			if (g_screen_to_display != DISPLAY_SCANNER)
				if (g_vfo_configure_mode == VFO_CONFIGURE_NONE)  // 'if' is so as we don't wipe out previously setting this variable elsewhere
					g_vfo_configure_mode = VFO_CONFIGURE;
		}
		else
		{
			g_flag_save_channel = g_request_save_channel;

			if (g_request_display_screen == DISPLAY_INVALID)
				g_request_display_screen = DISPLAY_MAIN;
		}

		g_request_save_channel = 0;
	}

	if (g_vfo_configure_mode != VFO_CONFIGURE_NONE)
	{
		if (g_flag_reset_vfos)
		{
			RADIO_ConfigureChannel(0, g_vfo_configure_mode);
			RADIO_ConfigureChannel(1, g_vfo_configure_mode);
		}
		else
			RADIO_ConfigureChannel(g_eeprom.tx_vfo, g_vfo_configure_mode);

		if (g_request_display_screen == DISPLAY_INVALID)
			g_request_display_screen = DISPLAY_MAIN;

		g_flag_reconfigure_vfos = true;
		g_vfo_configure_mode    = VFO_CONFIGURE_NONE;
		g_flag_reset_vfos       = false;
	}

	if (g_flag_reconfigure_vfos)
	{
		RADIO_SelectVfos();

		#ifdef ENABLE_NOAA
			RADIO_ConfigureNOAA();
		#endif

		RADIO_SetupRegisters(true);

		g_dtmf_auto_reset_time_500ms = 0;
		g_dtmf_call_state             = DTMF_CALL_STATE_NONE;
		g_dtmf_tx_stop_count_down_500ms = 0;
		g_dtmf_is_tx                  = false;

		g_vfo_rssi_bar_level[0]      = 0;
		g_vfo_rssi_bar_level[1]      = 0;

		g_flag_reconfigure_vfos        = false;

		if (g_monitor_enabled)
			ACTION_Monitor();   // 1of11
	}

	if (g_flag_refresh_menu)
	{
		g_flag_refresh_menu = false;
		g_menu_count_down      = menu_timeout_500ms;

		MENU_ShowCurrentSetting();
	}

	if (g_flag_start_scan)
	{
		g_flag_start_scan = false;

		g_monitor_enabled = false;

		#ifdef ENABLE_VOICE
			AUDIO_SetVoiceID(0, VOICE_ID_SCANNING_BEGIN);
			AUDIO_PlaySingleVoice(true);
		#endif

		SCANNER_Start();

		g_request_display_screen = DISPLAY_SCANNER;
	}

	if (g_flag_prepare_tx)
	{
		RADIO_PrepareTX();
		g_flag_prepare_tx = false;
	}

	#ifdef ENABLE_VOICE
		if (g_another_voice_id != VOICE_ID_INVALID)
		{
			if (g_another_voice_id < 76)
				AUDIO_SetVoiceID(0, g_another_voice_id);
			AUDIO_PlaySingleVoice(false);
			g_another_voice_id = VOICE_ID_INVALID;
		}
	#endif

	GUI_SelectNextDisplay(g_request_display_screen);
	g_request_display_screen = DISPLAY_INVALID;

	g_update_display = true;
}
