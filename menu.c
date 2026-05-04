/**
 * @file    menu.c
 * @brief   Implementación de la máquina de estados del menú de usuario.
 *
 * @note    LAB4: Este módulo opera exclusivamente en Core 0. La bandera
 *          params_changed dispara el pre-cálculo (signal_precalculate()) que
 *          produce el nuevo buffer para el Core 1.
 * @author  Firmware Engineering Lab
 * @version 1.0.0
 * @date    2026
 */

#include "menu.h"

#include <stdio.h>      /* snprintf()  */
#include <stdlib.h>     /* atoi()      */
#include <string.h>     /* memset()    */

/* ═══════════════════════════════════════════════════════════════════════════
 *  Funciones auxiliares internas
 * ══════════════════════════════════════════════════════════════════════════*/

static const char *wave_name(wave_type_t wave)
{
    switch (wave) {
        case WAVE_SINE:     return "SEN";
        case WAVE_SQUARE:   return "SQR";
        case WAVE_TRIANGLE: return "TRI";
        default:            return "???";
    }
}

static void go_to_main(menu_t *menu)
{
    menu->state                = MENU_MAIN;
    menu->input_len            = 0U;
    menu->input_buf[0]         = '\0';
    menu->needs_display_update = true;
}

static void handle_numeric_entry(menu_t *menu, char key)
{
    if (key >= '0' && key <= '9') {

        if (menu->input_len < (MENU_INPUT_BUF_LEN - 1U)) {
            menu->input_buf[menu->input_len++] = key;
            menu->input_buf[menu->input_len]   = '\0';
            menu->needs_display_update = true;
        }

    } else if (key == '*') {

        if (menu->input_len > 0U) {
            menu->input_buf[--menu->input_len] = '\0';
            menu->needs_display_update = true;
        } else {
            go_to_main(menu);
        }

    } else if (key == '#') {

        if (menu->input_len > 0U) {
            uint32_t value = (uint32_t)atoi(menu->input_buf);

            switch (menu->state) {

                case MENU_ENTER_FREQ:
                    if (value >= MENU_FREQ_MIN && value <= MENU_FREQ_MAX) {
                        menu->params.freq_hz = value;
                        //  params_changed ahora dispara signal_precalculate()
                        //               en main_multicore.c (no solo recalcula el período).
                        menu->params_changed = true;
                    }
                    break;

                case MENU_ENTER_AMP:
                    if (value <= MENU_AMP_MAX) {
                        menu->params.amplitude = (uint8_t)value;
                        menu->params_changed   = true;
                    }
                    break;

                case MENU_ENTER_DC:
                    if (value <= MENU_DC_MAX) {
                        menu->params.dc_level = (uint8_t)value;
                        menu->params_changed  = true;
                    }
                    break;

                default:
                    break;
            }
        }
        go_to_main(menu);
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  API pública
 * ══════════════════════════════════════════════════════════════════════════*/

void menu_init(menu_t *menu)
{
    memset(menu, 0, sizeof(menu_t));

    /* Parámetros por defecto */
    menu->params.wave      = WAVE_SINE;
    menu->params.freq_hz   = 100U;
    menu->params.amplitude = 100U;
    menu->params.dc_level  = 128U;

    menu->state                = MENU_MAIN;
    menu->needs_display_update = true;
    menu->params_changed       = true;  /* Forzar pre-cálculo inicial */
}

/* ─────────────────────────────────────────────────────────────────────────── */

void menu_process_key(menu_t *menu, char key)
{
    switch (menu->state) {

        case MENU_MAIN:
            switch (key) {
                case 'A':
                    menu->state                = MENU_SEL_WAVE;
                    menu->needs_display_update = true;
                    break;
                case 'B':
                    menu->state                = MENU_ENTER_FREQ;
                    menu->input_len            = 0U;
                    menu->input_buf[0]         = '\0';
                    menu->needs_display_update = true;
                    break;
                case 'C':
                    menu->state                = MENU_ENTER_AMP;
                    menu->input_len            = 0U;
                    menu->input_buf[0]         = '\0';
                    menu->needs_display_update = true;
                    break;
                case 'D':
                    menu->state                = MENU_ENTER_DC;
                    menu->input_len            = 0U;
                    menu->input_buf[0]         = '\0';
                    menu->needs_display_update = true;
                    break;
                default:
                    break;
            }
            break;

        case MENU_SEL_WAVE:
            switch (key) {
                case '1':
                    menu->params.wave    = WAVE_SINE;
                    menu->params_changed = true;
                    go_to_main(menu);
                    break;
                case '2':
                    menu->params.wave    = WAVE_SQUARE;
                    menu->params_changed = true;
                    go_to_main(menu);
                    break;
                case '3':
                    menu->params.wave    = WAVE_TRIANGLE;
                    menu->params_changed = true;
                    go_to_main(menu);
                    break;
                case '*':
                    go_to_main(menu);
                    break;
                default:
                    break;
            }
            break;

        case MENU_ENTER_FREQ:
        case MENU_ENTER_AMP:
        case MENU_ENTER_DC:
            handle_numeric_entry(menu, key);
            break;

        default:
            go_to_main(menu);
            break;
    }
}

/* ─────────────────────────────────────────────────────────────────────────── */

void menu_update_display(menu_t *menu, lcd_t *lcd)
{
    char line1[17];
    char line2[17];

    switch (menu->state) {

        case MENU_MAIN:
            snprintf(line1, sizeof(line1), "%-3s       %4u Hz",
                     wave_name(menu->params.wave),
                     (unsigned)menu->params.freq_hz);
            snprintf(line2, sizeof(line2), "AMP:%3u%%  DC:%3u",
                     (unsigned)menu->params.amplitude,
                     (unsigned)menu->params.dc_level);
            break;

        case MENU_SEL_WAVE:
            snprintf(line1, sizeof(line1), "Tipo de onda:   ");
            snprintf(line2, sizeof(line2), "1:SEN 2:SQR 3:TRI");
            break;

        case MENU_ENTER_FREQ:
            snprintf(line1, sizeof(line1), "Frec. (1-17000Hz)");
            snprintf(line2, sizeof(line2), ">%-4s           ", menu->input_buf);
            break;

        case MENU_ENTER_AMP:
            snprintf(line1, sizeof(line1), "Amplitud (0-100)");
            snprintf(line2, sizeof(line2), ">%-4s %%          ", menu->input_buf);
            break;

        case MENU_ENTER_DC:
            snprintf(line1, sizeof(line1), "Nivel DC(0-255) ");
            snprintf(line2, sizeof(line2), ">%-4s           ", menu->input_buf);
            break;

        default:
            snprintf(line1, sizeof(line1), "  Error estado  ");
            snprintf(line2, sizeof(line2), "                ");
            break;
    }

    line1[16] = '\0';
    line2[16] = '\0';

    lcd_set_cursor(lcd, 0, 0);
    lcd_print(lcd, line1);
    lcd_set_cursor(lcd, 1, 0);
    lcd_print(lcd, line2);

    menu->needs_display_update = false;
}
