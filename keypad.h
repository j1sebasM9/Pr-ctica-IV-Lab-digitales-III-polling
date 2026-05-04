/**
 * @file    keypad.h
 * @brief   Driver de teclado matricial 4×4 con FSM asíncrona (sin sleep_us).
 *
 * @author  Firmware Engineering Lab
 * @version 2.0.0 - LAB3 Multicore
 * @date    2026
 */

#ifndef KEYPAD_H
#define KEYPAD_H

#include <stdint.h>
#include <stdbool.h>
#include "hardware/gpio.h"

/* ═══════════════════════════════════════════════════════════════════════════
 *  Constantes del teclado
 * ══════════════════════════════════════════════════════════════════════════*/

#define KP_ROWS         4U          /**< Número de filas del teclado 4×4.        */
#define KP_COLS         4U          /**< Número de columnas del teclado 4×4.      */
#define KP_NO_KEY       '\0'        /**< Valor centinela: ninguna tecla activa.   */
#define KP_DEBOUNCE_US  50000ULL    /**< Ventana de anti-rebote: 50 ms.           */

/* ═══════════════════════════════════════════════════════════════════════════
 *  FSM asíncrona de escaneo por fila
 * ══════════════════════════════════════════════════════════════════════════*/


//
//   KP_PHASE_ACTIVATE ──► KP_PHASE_WAIT ──► KP_PHASE_READ ──► KP_PHASE_ACTIVATE
//        (pull LOW             (≥ 2 µs          (leer cols,        (siguiente fila
//       + timestamp)         no-bloqueante)    pull HIGH, avanzar)   o nuevo ciclo)
typedef enum {
    KP_PHASE_ACTIVATE = 0,  /**< Activar fila actual (pull LOW) y registrar timestamp. */
    KP_PHASE_WAIT,          /**< Esperar estabilización ≥ 2 µs sin bloquear ejecución. */
    KP_PHASE_READ           /**< Leer columnas, desactivar fila y avanzar al siguiente. */
} kp_scan_phase_t;

/* ═══════════════════════════════════════════════════════════════════════════
 *  Estructura del driver de teclado
 * ══════════════════════════════════════════════════════════════════════════*/

typedef struct {
    /* ── Hardware ──────────────────────────────────────────────────────── */
    uint8_t  row_pins[KP_ROWS];     /**< GPIOs de filas (salidas).                */
    uint8_t  col_pins[KP_COLS];     /**< GPIOs de columnas (entradas pull-up).    */

    /* ── Anti-rebote ───────────────────────────────────────────────────── */
    uint64_t debounce_us;           /**< Período mínimo entre eventos [µs].       */
    uint64_t last_event_us;         /**< Timestamp del último evento registrado.  */
    char     last_key;              /**< Última tecla detectada (o KP_NO_KEY).    */
    bool     key_ready;             /**< TRUE si hay una tecla sin consumir.      */

    /* ── Estado de la FSM asíncrona ────────────────────────────────────── */
    kp_scan_phase_t scan_phase;     /**< Fase actual del escaneo (ACTIVATE/WAIT/READ). */
    uint8_t         current_row;    /**< Fila que se está procesando [0..KP_ROWS-1].   */
    uint64_t        row_active_ts;  /**< Timestamp cuando se activó la fila actual.    */
    char            scan_detected;  /**< Primera tecla encontrada en el ciclo actual.  */
} keypad_t;

/* ═══════════════════════════════════════════════════════════════════════════
 *  API pública
 * ══════════════════════════════════════════════════════════════════════════*/

/**
 * @brief  Inicializa el hardware del teclado y el estado de la FSM.
 */
void kp_init(keypad_t        *kpad,
             const uint8_t    row_pins[KP_ROWS],
             const uint8_t    col_pins[KP_COLS],
             uint64_t         debounce_us);

/**
 * @brief  Ejecuta UNA transición de la FSM de escaneo (no bloqueante).
 *
 * @details Debe invocarse repetidamente desde el super-loop del Core 0.
 *          Un ciclo completo (4 filas × 3 fases) requiere múltiples llamadas;
 *          la duración de cada llamada es de nanosegundos a pocos microsegundos.
 *          No contiene ningún sleep_us() ni busy-wait.
 */
void kp_scan(keypad_t *kpad);

/**
 * @brief  Retorna TRUE si hay una tecla lista para ser leída.
 */
bool kp_has_key(const keypad_t *kpad);

/**
 * @brief  Retorna y consume la tecla disponible. Retorna KP_NO_KEY si no hay ninguna.
 */
char kp_get_key(keypad_t *kpad);

#endif /* KEYPAD_H */
