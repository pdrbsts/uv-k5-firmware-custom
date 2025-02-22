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

#include "app/action.h"
#include "app/app.h"
#ifdef ENABLE_FMRADIO
	#include "app/fm.h"
#endif
#include "app/generic.h"
#include "app/main.h"
#include "app/scanner.h"
#include "audio.h"
#include "board.h"
#include "driver/bk4819.h"
#include "dtmf.h"
#include "frequencies.h"
#include "misc.h"
#include "radio.h"
#include "settings.h"
#include "ui/inputbox.h"
#include "ui/ui.h"
#ifdef ENABLE_SPECTRUM
//	#include "app/spectrum.h"
#endif

void toggle_chan_scanlist(void)
{	// toggle the selected channels scanlist setting

	if (g_screen_to_display == DISPLAY_SCANNER || !IS_USER_CHANNEL(g_tx_vfo->channel_save))
		return;

	if (g_tx_vfo->scanlist_1_participation)
	{
		if (g_tx_vfo->scanlist_2_participation)
			g_tx_vfo->scanlist_1_participation = 0;
		else
			g_tx_vfo->scanlist_2_participation = 1;
	}
	else
	{
		if (g_tx_vfo->scanlist_2_participation)
			g_tx_vfo->scanlist_2_participation = 0;
		else
			g_tx_vfo->scanlist_1_participation = 1;
	}

	SETTINGS_UpdateChannel(g_tx_vfo->channel_save, g_tx_vfo, true);

	g_vfo_configure_mode = VFO_CONFIGURE;
	g_flag_reset_vfos    = true;
}

static void processFKeyFunction(const key_code_t Key)
{
	uint8_t Band;
	uint8_t Vfo = g_eeprom.tx_vfo;

	if (g_screen_to_display == DISPLAY_MENU)
	{
		g_beep_to_play = BEEP_500HZ_60MS_DOUBLE_BEEP_OPTIONAL;
		return;
	}

	switch (Key)
	{
		case KEY_0:
			#ifdef ENABLE_FMRADIO
				ACTION_FM();
			#else


				// TODO: make use of this function key


			#endif
			break;

		case KEY_1:
			if (!IS_FREQ_CHANNEL(g_tx_vfo->channel_save))
			{
				g_fkey_pressed = false;
				g_update_status   = true;
				g_beep_to_play     = BEEP_1KHZ_60MS_OPTIONAL;
				return;
			}

			Band = g_tx_vfo->band + 1;
			if (g_setting_350_enable || Band != BAND5_350MHz)
			{
				if (Band > BAND7_470MHz)
					Band = BAND1_50MHz;
			}
			else
				Band = BAND6_400MHz;
			g_tx_vfo->band = Band;

			g_eeprom.screen_channel[Vfo] = FREQ_CHANNEL_FIRST + Band;
			g_eeprom.freq_channel[Vfo]   = FREQ_CHANNEL_FIRST + Band;

			g_request_save_vfo   = true;
			g_vfo_configure_mode = VFO_CONFIGURE_RELOAD;

			g_beep_to_play = BEEP_1KHZ_60MS_OPTIONAL;

			g_request_display_screen = DISPLAY_MAIN;
			break;

		case KEY_2:
			if (g_eeprom.cross_vfo_rx_tx == CROSS_BAND_CHAN_A)
				g_eeprom.cross_vfo_rx_tx = CROSS_BAND_CHAN_B;
			else
			if (g_eeprom.cross_vfo_rx_tx == CROSS_BAND_CHAN_B)
				g_eeprom.cross_vfo_rx_tx = CROSS_BAND_CHAN_A;
			else
			if (g_eeprom.dual_watch == DUAL_WATCH_CHAN_A)
				g_eeprom.dual_watch = DUAL_WATCH_CHAN_B;
			else
			if (g_eeprom.dual_watch == DUAL_WATCH_CHAN_B)
				g_eeprom.dual_watch = DUAL_WATCH_CHAN_A;
			else
				g_eeprom.tx_vfo = (Vfo + 1) & 1u;

			g_request_save_settings = 1;
			g_flag_reconfigure_vfos = true;

			g_beep_to_play = BEEP_1KHZ_60MS_OPTIONAL;

			g_request_display_screen = DISPLAY_MAIN;
			break;

		case KEY_3:
			#ifdef ENABLE_NOAA
				if (g_eeprom.vfo_open && IS_NOT_NOAA_CHANNEL(g_tx_vfo->channel_save))
			#else
				if (g_eeprom.vfo_open)
			#endif
			{
				uint8_t Channel;

				if (IS_USER_CHANNEL(g_tx_vfo->channel_save))
				{	// swap to frequency mode
					g_eeprom.screen_channel[Vfo] = g_eeprom.freq_channel[g_eeprom.tx_vfo];

					#ifdef ENABLE_VOICE
						g_another_voice_id = VOICE_ID_FREQUENCY_MODE;
					#endif

					g_request_save_vfo   = true;
					g_vfo_configure_mode = VFO_CONFIGURE_RELOAD;

					g_beep_to_play = BEEP_1KHZ_60MS_OPTIONAL;
					break;
				}

				Channel = RADIO_FindNextChannel(g_eeprom.user_channel[g_eeprom.tx_vfo], 1, false, 0);
				if (Channel != 0xFF)
				{	// swap to channel mode
					g_eeprom.screen_channel[Vfo] = Channel;

					#ifdef ENABLE_VOICE
						AUDIO_SetVoiceID(0, VOICE_ID_CHANNEL_MODE);
						AUDIO_SetDigitVoice(1, Channel + 1);
						g_another_voice_id = (voice_id_t)0xFE;
					#endif

					g_request_save_vfo   = true;
					g_vfo_configure_mode = VFO_CONFIGURE_RELOAD;

					g_beep_to_play = BEEP_1KHZ_60MS_OPTIONAL;
					break;
				}
			}

			g_beep_to_play = BEEP_500HZ_60MS_DOUBLE_BEEP_OPTIONAL;

			break;

		case KEY_4:
			g_fkey_pressed           = false;
			g_flag_start_scan        = true;
			g_scan_single_frequency  = false;
			g_backup_cross_vfo_rx_tx = g_eeprom.cross_vfo_rx_tx;
			g_eeprom.cross_vfo_rx_tx = CROSS_BAND_OFF;
			g_update_status          = true;

			g_beep_to_play = BEEP_1KHZ_60MS_OPTIONAL;
			break;

		case KEY_5:
			#ifdef ENABLE_NOAA

				if (IS_NOT_NOAA_CHANNEL(g_tx_vfo->channel_save))
				{
					g_eeprom.screen_channel[Vfo] = g_eeprom.noaa_channel[g_eeprom.tx_vfo];
				}
				else
				{
					g_eeprom.screen_channel[Vfo] = g_eeprom.freq_channel[g_eeprom.tx_vfo];
					#ifdef ENABLE_VOICE
						g_another_voice_id = VOICE_ID_FREQUENCY_MODE;
					#endif
				}
				g_request_save_vfo   = true;
				g_vfo_configure_mode = VFO_CONFIGURE_RELOAD;

			#else
				#ifdef ENABLE_VOX
					toggle_chan_scanlist();
				#endif
			#endif

			g_beep_to_play = BEEP_1KHZ_60MS_OPTIONAL;
			break;

		case KEY_6:
			ACTION_Power();
			g_beep_to_play = BEEP_1KHZ_60MS_OPTIONAL;
			break;

		case KEY_7:
			#ifdef ENABLE_VOX
				ACTION_Vox();
			#else
				toggle_chan_scanlist();
			#endif
			g_beep_to_play = BEEP_1KHZ_60MS_OPTIONAL;
			break;

		case KEY_8:
			g_tx_vfo->frequency_reverse = g_tx_vfo->frequency_reverse == false;
			g_request_save_channel = 1;
			g_beep_to_play = BEEP_1KHZ_60MS_OPTIONAL;
			break;

		case KEY_9:
			if (RADIO_CheckValidChannel(g_eeprom.chan_1_call, false, 0))
			{
				g_eeprom.user_channel[Vfo]     = g_eeprom.chan_1_call;
				g_eeprom.screen_channel[Vfo] = g_eeprom.chan_1_call;
				#ifdef ENABLE_VOICE
					AUDIO_SetVoiceID(0, VOICE_ID_CHANNEL_MODE);
					AUDIO_SetDigitVoice(1, g_eeprom.chan_1_call + 1);
					g_another_voice_id        = (voice_id_t)0xFE;
				#endif
				g_request_save_vfo            = true;
				g_vfo_configure_mode          = VFO_CONFIGURE_RELOAD;
				break;
			}

			g_beep_to_play = BEEP_500HZ_60MS_DOUBLE_BEEP_OPTIONAL;
			break;

		default:
			g_update_status = true;
			g_fkey_pressed  = false;

			#ifdef ENABLE_FMRADIO
				if (!g_fm_radio_mode)
			#endif
					g_beep_to_play = BEEP_1KHZ_60MS_OPTIONAL;
			break;
	}
}

static void MAIN_Key_DIGITS(key_code_t Key, bool key_pressed, bool key_held)
{
	if (key_held)
	{	// key held down

		if (key_pressed)
		{
			if (g_screen_to_display == DISPLAY_MAIN)
			{
				if (g_input_box_index > 0)
				{	// delete any inputted chars
					g_input_box_index        = 0;
					g_request_display_screen = DISPLAY_MAIN;
				}

				g_fkey_pressed = false;
				g_update_status   = true;

				processFKeyFunction(Key);
			}
		}

		return;
	}

	if (key_pressed)
	{	// key is pressed
		g_beep_to_play = BEEP_1KHZ_60MS_OPTIONAL;  // beep when key is pressed
		return;                                 // don't use the key till it's released
	}

	if (!g_fkey_pressed)
	{	// F-key wasn't pressed

		const uint8_t Vfo = g_eeprom.tx_vfo;

		g_key_input_count_down = key_input_timeout_500ms;

		INPUTBOX_Append(Key);

		g_request_display_screen = DISPLAY_MAIN;

		if (IS_USER_CHANNEL(g_tx_vfo->channel_save))
		{	// user is entering channel number

			uint16_t Channel;

			if (g_input_box_index != 3)
			{
				#ifdef ENABLE_VOICE
					g_another_voice_id   = (voice_id_t)Key;
				#endif
				g_request_display_screen = DISPLAY_MAIN;
				return;
			}

			g_input_box_index = 0;

			Channel = ((g_input_box[0] * 100) + (g_input_box[1] * 10) + g_input_box[2]) - 1;

			if (!RADIO_CheckValidChannel(Channel, false, 0))
			{
				g_beep_to_play = BEEP_500HZ_60MS_DOUBLE_BEEP_OPTIONAL;
				return;
			}

			#ifdef ENABLE_VOICE
				g_another_voice_id        = (voice_id_t)Key;
			#endif

			g_eeprom.user_channel[Vfo]     = (uint8_t)Channel;
			g_eeprom.screen_channel[Vfo] = (uint8_t)Channel;
			g_request_save_vfo            = true;
			g_vfo_configure_mode          = VFO_CONFIGURE_RELOAD;

			return;
		}

//		#ifdef ENABLE_NOAA
//			if (IS_NOT_NOAA_CHANNEL(g_tx_vfo->channel_save))
//		#endif
		if (IS_FREQ_CHANNEL(g_tx_vfo->channel_save))
		{	// user is entering a frequency

			uint32_t Frequency;

			if (g_input_box_index < 6)
			{
				#ifdef ENABLE_VOICE
					g_another_voice_id = (voice_id_t)Key;
				#endif

				return;
			}

			g_input_box_index = 0;

			NUMBER_Get(g_input_box, &Frequency);

			// clamp the frequency entered to some valid value
			if (Frequency < FREQ_BAND_TABLE[0].lower)
			{
				Frequency = FREQ_BAND_TABLE[0].lower;
			}
			else
			if (Frequency >= BX4819_band1.upper && Frequency < BX4819_band2.lower)
			{
				const uint32_t center = (BX4819_band1.upper + BX4819_band2.lower) / 2;
				Frequency = (Frequency < center) ? BX4819_band1.upper : BX4819_band2.lower;
			}
			else
			if (Frequency > FREQ_BAND_TABLE[ARRAY_SIZE(FREQ_BAND_TABLE) - 1].upper)
			{
				Frequency = FREQ_BAND_TABLE[ARRAY_SIZE(FREQ_BAND_TABLE) - 1].upper;
			}

			{
				const FREQUENCY_Band_t band = FREQUENCY_GetBand(Frequency);

				#ifdef ENABLE_VOICE
					g_another_voice_id = (voice_id_t)Key;
				#endif

				if (g_tx_vfo->band != band)
				{
					g_tx_vfo->band               = band;
					g_eeprom.screen_channel[Vfo] = band + FREQ_CHANNEL_FIRST;
					g_eeprom.freq_channel[Vfo]   = band + FREQ_CHANNEL_FIRST;

					SETTINGS_SaveVfoIndices();

					RADIO_ConfigureChannel(Vfo, VFO_CONFIGURE_RELOAD);
				}

//				Frequency += 75;                        // is this meant to be rounding ?
				Frequency += g_tx_vfo->step_freq / 2; // no idea, but this is

				Frequency = FREQUENCY_FloorToStep(Frequency, g_tx_vfo->step_freq, FREQ_BAND_TABLE[g_tx_vfo->band].lower);

				if (Frequency >= BX4819_band1.upper && Frequency < BX4819_band2.lower)
				{	// clamp the frequency to the limit
					const uint32_t center = (BX4819_band1.upper + BX4819_band2.lower) / 2;
					Frequency = (Frequency < center) ? BX4819_band1.upper - g_tx_vfo->step_freq : BX4819_band2.lower;
				}

				g_tx_vfo->freq_config_rx.frequency = Frequency;

				g_request_save_channel = 1;
				return;
			}

		}
		#ifdef ENABLE_NOAA
			else
			if (IS_NOAA_CHANNEL(g_tx_vfo->channel_save))
			{	// user is entering NOAA channel

				uint8_t Channel;

				if (g_input_box_index != 2)
				{
					#ifdef ENABLE_VOICE
						g_another_voice_id   = (voice_id_t)Key;
					#endif
					g_request_display_screen = DISPLAY_MAIN;
					return;
				}

				g_input_box_index = 0;

				Channel = (g_input_box[0] * 10) + g_input_box[1];
				if (Channel >= 1 && Channel <= ARRAY_SIZE(NoaaFrequencyTable))
				{
					Channel                   += NOAA_CHANNEL_FIRST;
					#ifdef ENABLE_VOICE
						g_another_voice_id        = (voice_id_t)Key;
					#endif
					g_eeprom.noaa_channel[Vfo]   = Channel;
					g_eeprom.screen_channel[Vfo] = Channel;
					g_request_save_vfo            = true;
					g_vfo_configure_mode          = VFO_CONFIGURE_RELOAD;
					return;
				}
			}
		#endif

		g_request_display_screen = DISPLAY_MAIN;
		g_beep_to_play           = BEEP_500HZ_60MS_DOUBLE_BEEP_OPTIONAL;
		return;
	}

	g_fkey_pressed = false;
	g_update_status   = true;

	processFKeyFunction(Key);
}

static void MAIN_Key_EXIT(bool key_pressed, bool key_held)
{
	if (!key_held && key_pressed)
	{	// exit key pressed

		g_beep_to_play = BEEP_1KHZ_60MS_OPTIONAL;

		if (g_dtmf_call_state != DTMF_CALL_STATE_NONE && g_current_function != FUNCTION_TRANSMIT)
		{	// clear CALL mode being displayed
			g_dtmf_call_state = DTMF_CALL_STATE_NONE;
			g_update_display  = true;
			return;
		}

		#ifdef ENABLE_FMRADIO
			if (!g_fm_radio_mode)
		#endif
		{
			if (g_scan_state_dir == SCAN_OFF)
			{
				if (g_input_box_index == 0)
					return;
				g_input_box[--g_input_box_index] = 10;

				g_key_input_count_down = key_input_timeout_500ms;

				#ifdef ENABLE_VOICE
					if (g_input_box_index == 0)
						g_another_voice_id = VOICE_ID_CANCEL;
				#endif
			}
			else
			{
				SCANNER_Stop();

				#ifdef ENABLE_VOICE
					g_another_voice_id = VOICE_ID_SCANNING_STOP;
				#endif
			}

			g_request_display_screen = DISPLAY_MAIN;
			return;
		}

		#ifdef ENABLE_FMRADIO
			ACTION_FM();
		#endif

		return;
	}

	if (key_held && key_pressed)
	{	// exit key held down

		if (g_input_box_index > 0 || g_dtmf_input_box_index > 0 || g_dtmf_input_mode)
		{	// cancel key input mode (channel/frequency entry)
			g_dtmf_input_mode       = false;
			g_dtmf_input_box_index  = 0;
			memset(g_dtmf_string, 0, sizeof(g_dtmf_string));
			g_input_box_index        = 0;
			g_request_display_screen = DISPLAY_MAIN;
			g_beep_to_play           = BEEP_1KHZ_60MS_OPTIONAL;
		}
	}
}

static void MAIN_Key_MENU(const bool key_pressed, const bool key_held)
{
	if (key_pressed && !key_held)
		// menu key pressed
		g_beep_to_play = BEEP_1KHZ_60MS_OPTIONAL;

	if (key_held)
	{	// menu key held down (long press)

		if (key_pressed)
		{	// long press MENU key

			g_fkey_pressed = false;

			if (g_screen_to_display == DISPLAY_MAIN)
			{
				if (g_input_box_index > 0)
				{	// delete any inputted chars
					g_input_box_index        = 0;
					g_request_display_screen = DISPLAY_MAIN;
				}

				g_fkey_pressed = false;
				g_update_status   = true;

				#ifdef ENABLE_COPY_CHAN_TO_VFO

					if (g_eeprom.vfo_open && g_css_scan_mode == CSS_SCAN_MODE_OFF)
					{

						if (g_scan_state_dir != SCAN_OFF)
						{
							if (g_current_function != FUNCTION_INCOMING ||
							    g_rx_reception_mode == RX_MODE_NONE      ||
								g_scan_pause_delay_in_10ms == 0)
							{	// scan is running (not paused)
								return;
							}
						}

						const unsigned int vfo = get_RX_VFO();

						if (IS_USER_CHANNEL(g_eeprom.screen_channel[vfo]))
						{	// copy channel to VFO, then swap to the VFO

							const unsigned int channel = FREQ_CHANNEL_FIRST + g_eeprom.vfo_info[vfo].band;

							g_eeprom.screen_channel[vfo] = channel;
							g_eeprom.vfo_info[vfo].channel_save = channel;
							g_eeprom.tx_vfo = vfo;

							RADIO_SelectVfos();
							RADIO_ApplyOffset(g_rx_vfo);
							RADIO_ConfigureSquelchAndOutputPower(g_rx_vfo);
							RADIO_SetupRegisters(true);

							g_request_save_vfo = true;

							g_beep_to_play = BEEP_1KHZ_60MS_OPTIONAL;

							g_update_status  = true;
							g_update_display = true;
						}
					}
					else
					{
						g_beep_to_play = BEEP_500HZ_60MS_DOUBLE_BEEP_OPTIONAL;
					}

				#endif
			}
		}

		return;
	}

	if (!key_pressed && !g_dtmf_input_mode)
	{	// menu key released
		const bool flag = (g_input_box_index == 0);
		g_input_box_index   = 0;

		if (flag)
		{
			g_flag_refresh_menu = true;
			g_request_display_screen = DISPLAY_MENU;
			#ifdef ENABLE_VOICE
				g_another_voice_id   = VOICE_ID_MENU;
			#endif
		}
		else
		{
			g_request_display_screen = DISPLAY_MAIN;
		}
	}
}

static void MAIN_Key_STAR(bool key_pressed, bool key_held)
{
	if (g_current_function == FUNCTION_TRANSMIT)
		return;

	if (g_input_box_index > 0)
	{	// entering a frequency or DTMF string
		if (!key_held && key_pressed)
			g_beep_to_play = BEEP_500HZ_60MS_DOUBLE_BEEP_OPTIONAL;
		return;
	}

	if (key_held && !g_fkey_pressed)
	{	// long press .. toggle scanning
		if (!key_pressed)
			return; // released

		ACTION_Scan(false);

		g_beep_to_play = BEEP_1KHZ_60MS_OPTIONAL;
		return;
	}

	if (key_pressed)
	{	// just pressed
//		g_beep_to_play = BEEP_1KHZ_60MS_OPTIONAL;
		g_beep_to_play = BEEP_880HZ_40MS_OPTIONAL;
		return;
	}

	// just released

	if (!g_fkey_pressed)
	{	// pressed without the F-key

		#ifdef ENABLE_NOAA
			if (g_scan_state_dir == SCAN_OFF && IS_NOT_NOAA_CHANNEL(g_tx_vfo->channel_save))
		#else
			if (g_scan_state_dir == SCAN_OFF)
		#endif
		{	// start entering a DTMF string

			memmove(
				g_dtmf_input_box,
				g_dtmf_string,
				(sizeof(g_dtmf_input_box) <= (sizeof(g_dtmf_string) - 1)) ? sizeof(g_dtmf_input_box) : sizeof(g_dtmf_string) - 1);
			g_dtmf_input_box_index  = 0;
			g_dtmf_input_mode       = true;

			g_key_input_count_down    = key_input_timeout_500ms;

			g_request_display_screen = DISPLAY_MAIN;
		}
	}
	else
	{	// with the F-key
		g_fkey_pressed = false;

		#ifdef ENABLE_NOAA
			if (IS_NOAA_CHANNEL(g_tx_vfo->channel_save))
			{
				g_beep_to_play = BEEP_500HZ_60MS_DOUBLE_BEEP_OPTIONAL;
				return;
			}
		#endif

		// scan the CTCSS/DCS code
		g_flag_start_scan        = true;
		g_scan_single_frequency  = true;
		g_backup_cross_vfo_rx_tx = g_eeprom.cross_vfo_rx_tx;
		g_eeprom.cross_vfo_rx_tx = CROSS_BAND_OFF;
	}

//	g_ptt_was_released = true;	// why is this being set ?

	g_update_status   = true;
}

static void MAIN_Key_UP_DOWN(bool key_pressed, bool key_held, scan_state_dir_t Direction)
{
	uint8_t Channel = g_eeprom.screen_channel[g_eeprom.tx_vfo];

	if (key_held || !key_pressed)
	{	// long press

		if (g_input_box_index > 0)
			return;

		if (!key_pressed)
		{
			if (!key_held)
				return;

			if (IS_FREQ_CHANNEL(Channel))
				return;

			#ifdef ENABLE_VOICE
				AUDIO_SetDigitVoice(0, g_tx_vfo->channel_save + 1);
				g_another_voice_id = (voice_id_t)0xFE;
			#endif

			return;
		}
	}
	else
	{
		if (g_input_box_index > 0)
		{
			g_beep_to_play = BEEP_500HZ_60MS_DOUBLE_BEEP_OPTIONAL;
			return;
		}

		g_beep_to_play = BEEP_1KHZ_60MS_OPTIONAL;
	}

	if (g_scan_state_dir == SCAN_OFF)
	{
		#ifdef ENABLE_NOAA
			if (IS_NOT_NOAA_CHANNEL(Channel))
		#endif
		{
			uint8_t Next;

			if (IS_FREQ_CHANNEL(Channel))
			{	// step/down in frequency
				const uint32_t frequency = APP_SetFrequencyByStep(g_tx_vfo, Direction);

				if (RX_freq_check(frequency) < 0)
				{	// frequency not allowed
					g_beep_to_play = BEEP_500HZ_60MS_DOUBLE_BEEP_OPTIONAL;
					return;
				}

				g_tx_vfo->freq_config_rx.frequency = frequency;

				g_request_save_channel = 1;
				return;
			}

			Next = RADIO_FindNextChannel(Channel + Direction, Direction, false, 0);
			if (Next == 0xFF)
				return;

			if (Channel == Next)
				return;

			g_eeprom.user_channel[g_eeprom.tx_vfo]   = Next;
			g_eeprom.screen_channel[g_eeprom.tx_vfo] = Next;

			if (!key_held)
			{
				#ifdef ENABLE_VOICE
					AUDIO_SetDigitVoice(0, Next + 1);
					g_another_voice_id = (voice_id_t)0xFE;
				#endif
			}
		}
		#ifdef ENABLE_NOAA
			else
			{
				Channel = NOAA_CHANNEL_FIRST + NUMBER_AddWithWraparound(g_eeprom.screen_channel[g_eeprom.tx_vfo] - NOAA_CHANNEL_FIRST, Direction, 0, 9);
				g_eeprom.noaa_channel[g_eeprom.tx_vfo]   = Channel;
				g_eeprom.screen_channel[g_eeprom.tx_vfo] = Channel;
			}
		#endif

		g_request_save_vfo   = true;
		g_vfo_configure_mode = VFO_CONFIGURE_RELOAD;
		return;
	}

	// jump to the next channel
	CHANNEL_Next(false, Direction);
	g_scan_pause_delay_in_10ms = 1;
	g_schedule_scan_listen    = false;

//	g_ptt_was_released = true;    // why is this being set ?
}

void MAIN_ProcessKeys(key_code_t Key, bool key_pressed, bool key_held)
{
	#ifdef ENABLE_FMRADIO
		if (g_fm_radio_mode && Key != KEY_PTT && Key != KEY_EXIT)
		{
			if (!key_held && key_pressed)
				g_beep_to_play = BEEP_500HZ_60MS_DOUBLE_BEEP_OPTIONAL;
			return;
		}
	#endif

	if (g_dtmf_input_mode)
	{
		const char Character = DTMF_GetCharacter(Key);
		if (Character != 0xFF)
		{	// add key to DTMF string
			if (key_pressed && !key_held)
			{
				DTMF_Append(Character);
				g_key_input_count_down   = key_input_timeout_500ms;
				//g_ptt_was_released     = true;  // why is this being set ?
				g_beep_to_play           = BEEP_1KHZ_60MS_OPTIONAL;
				g_request_display_screen = DISPLAY_MAIN;
			}
			return;
		}
	}

	switch (Key)
	{
		case KEY_0:
		case KEY_1:
		case KEY_2:
		case KEY_3:
		case KEY_4:
		case KEY_5:
		case KEY_6:
		case KEY_7:
		case KEY_8:
		case KEY_9:
			MAIN_Key_DIGITS(Key, key_pressed, key_held);
			break;
		case KEY_MENU:
			MAIN_Key_MENU(key_pressed, key_held);
			break;
		case KEY_UP:
			MAIN_Key_UP_DOWN(key_pressed, key_held, 1);
			break;
		case KEY_DOWN:
			MAIN_Key_UP_DOWN(key_pressed, key_held, -1);
			break;
		case KEY_EXIT:
			MAIN_Key_EXIT(key_pressed, key_held);
			break;
		case KEY_STAR:
			MAIN_Key_STAR(key_pressed, key_held);
			break;
		case KEY_F:
			GENERIC_Key_F(key_pressed, key_held);
			break;
		case KEY_PTT:
			GENERIC_Key_PTT(key_pressed);
			break;
		default:
			if (!key_held && key_pressed)
				g_beep_to_play = BEEP_500HZ_60MS_DOUBLE_BEEP_OPTIONAL;
			break;
	}
}
