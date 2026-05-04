/**
 * @file    menu.h
 * @brief   Máquina de estados de la interfaz de usuario del Generador de Señales.
 *
 * @note    LAB4: Este módulo opera exclusivamente en Core 0. La bandera
 *          params_changed dispara el pre-cálculo (signal_precalculate()) que
 *          produce el nuevo buffer para el Core 1.
 *
 * @author  Firmware Engineering Lab
 * @version 1.0.0
 * @date    2026
 */

#ifndef MENU_H
#define MENU_H

#include <stdint.h>
#include <stdbool.h>
#include "signal_generator.h"   /* wave_type_t */
#include "LCD_16x2_H.h"         /* lcd_t       */

/* ═══════════════════════════════════════════════════════════════════════════
 *  Límites de parámetros del generador
 * ══════════════════════════════════════════════════════════════════════════*/

#define MENU_FREQ_MIN       1U      /**< Frecuencia mínima [Hz].        */
#define MENU_FREQ_MAX       17000U   /**< Frecuencia máxima [Hz].        */
#define MENU_AMP_MAX        100U    /**< Amplitud máxima [%].           */
#define MENU_DC_MAX         255U    /**< Nivel DC máximo (8 bits).      */
#define MENU_INPUT_BUF_LEN  6U     /**< 4 dígitos + '\0' + margen.     */

/* ═══════════════════════════════════════════════════════════════════════════
 *  Estados de la FSM del menú
 * ══════════════════════════════════════════════════════════════════════════*/

typedef enum {
    MENU_MAIN        = 0,   /**< Pantalla principal: estado actual del generador. */
    MENU_SEL_WAVE,          /**< Submenú: selección de tipo de onda.              */
    MENU_ENTER_FREQ,        /**< Submenú: ingreso numérico de frecuencia.         */
    MENU_ENTER_AMP,         /**< Submenú: ingreso numérico de amplitud.           */
    MENU_ENTER_DC           /**< Submenú: ingreso numérico de nivel DC.           */
} menu_state_t;

/* ═══════════════════════════════════════════════════════════════════════════
 *  Parámetros del generador de señales
 * ══════════════════════════════════════════════════════════════════════════*/

typedef struct {
    wave_type_t wave;       /**< Tipo de onda activa.                */
    uint32_t    freq_hz;    /**< Frecuencia fundamental [Hz].        */
    uint8_t     amplitude;  /**< Amplitud en porcentaje [0..100].    */
    uint8_t     dc_level;   /**< Nivel DC [0..255] (mitad = 128).    */
} signal_params_t;

/* ═══════════════════════════════════════════════════════════════════════════
 *  Estructura de la FSM del menú
 * ══════════════════════════════════════════════════════════════════════════*/

typedef struct {
    menu_state_t    state;                      /**< Estado actual de la FSM.         */
    signal_params_t params;                     /**< Parámetros activos del generador.*/
    bool            params_changed;             /**< TRUE cuando el usuario confirmó  */
                                                /*   un cambio → disparar pre-cálculo.*/
    bool            needs_display_update;       /**< TRUE cuando el LCD debe refrescarse.*/
    char            input_buf[MENU_INPUT_BUF_LEN]; /**< Buffer de entrada numérica.  */
    uint8_t         input_len;                  /**< Caracteres en input_buf.         */
} menu_t;

/* ═══════════════════════════════════════════════════════════════════════════
 *  API pública
 * ══════════════════════════════════════════════════════════════════════════*/

/**
 * @brief  Inicializa la FSM del menú con los parámetros por defecto.
 */
void menu_init(menu_t *menu);

/**
 * @brief  Procesa una pulsación de tecla y actualiza el estado de la FSM.
 * @param[in,out] menu  Puntero a la estructura del menú.
 * @param[in]     key   Carácter recibido del teclado.
 */
void menu_process_key(menu_t *menu, char key);

/**
 * @brief  Escribe el contenido del estado actual en el LCD 16×2.
 * @note   Esta función es bloqueante (~5-10 ms). En LAB3 esto es aceptable
 *         porque Core 1 mantiene la señal independientemente.
 */
void menu_update_display(menu_t *menu, lcd_t *lcd);

#endif /* MENU_H */
