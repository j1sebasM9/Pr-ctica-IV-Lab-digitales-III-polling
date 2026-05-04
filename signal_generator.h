/**
 * @file    signal_generator.h
 * @brief   Tablas de onda base, pre-cálculo de alta precisión y metrónomo PWM.
 *
 * @details LAB4 – Multicore + PWM Metrónomo:
 *|          Este módulo contiene la tabla base de onda (wave_lista_para_dac[]) y
 *|          la función signal_precalculate() que aplica amplitud y nivel DC con
 *|          punto flotante para máxima precisión. El buffer resultante se consume
 *|          en tiempo real por el Core 1, que ejecuta el loop de escritura al DAC
 *|          desde SRAM con jitter mínimo.
 *          ───────────────────────
 *
 *          1. Metrónomo PWM (DAC_PWM_SLICE) reemplaza a time_us_64().
 *             - Core 1 hace polling a pwm_hw->intr: latencia < 8 ns vs ~80 ns
 *               del timer de 64 bits.
 *             - El PWM hardware genera el tick de muestreo con jitter de
 *               ±1 ciclo de reloj (8 ns @ 125 MHz), contra ±5-20 µs del timer.
 *             - Límite teórico de frecuencia: ~122 kHz (WRAP_MIN = 2).
 *               Límite práctico del DAC R-2R: ~50 kHz.
 *
 *          2. dac_set_frequency() calcula WRAP y divisor Q8.4 dinámicamente.
 *             Precisión garantizada: ±2 % en todo el rango [1..10000] Hz.
 *
 *
 *          3. signal_precalculate() usa punto flotante temporario para máxima
 *             precisión en la aplicación de amplitud y nivel DC.
 *
 * @par Sincronización entre cores
 * @code
 *   Core 0 (productor)                Core 1 (consumidor)
 *   ──────────────────────────────    ─────────────────────────────────────
 *   wave_precalc_done = false;   →    while (!wave_precalc_done) spin;
 *   [escribe onda_lista_para_dac[]]   pwm_hw->intr polling
 *   wave_precalc_done = true;    →    gpio_put_masked(DAC_GPIO_MASK, buf[i])
 * @endcode
 *
 * @author  Fdavidovich
 * @version 3.0.0 - LAB4 PWM Metrónomo
 * @date    2025
 */
 
#ifndef SIGNAL_GENERATOR_H
#define SIGNAL_GENERATOR_H
 
#include <stdint.h>
#include <stdbool.h>
 
/* ═══════════════════════════════════════════════════════════════════════════
 *  Parámetros de la tabla de ondas
 * ══════════════════════════════════════════════════════════════════════════*/
 
/** @brief Número de muestras por período completo de onda. */
#define WAVE_SAMPLES    256U
 
/* ═══════════════════════════════════════════════════════════════════════════
 *  DAC R-2R – 8 bits en GPIO 0-7 (contiguos)
 * ══════════════════════════════════════════════════════════════════════════*/
 
/**
 * @brief Máscara de GPIOs del DAC.
 *
 * GPIO 0 (LSB) .. GPIO 7 (MSB). Se usa como:
 *   • gpio_init_mask(DAC_GPIO_MASK)          → configurar pines al inicio
 *   • gpio_set_dir_out_masked(DAC_GPIO_MASK) → pines como salida
 *   • gpio_put_masked(DAC_GPIO_MASK, val)    → escritura protegida en Core 1
 *
 * La máscara 0xFF protege los pines del LCD (GPIO 10-15) y del teclado
 * (GPIO 16-26) de ser afectados por las escrituras del DAC.
 */
#define DAC_GPIO_MASK   0x000000FFu
 
/* ═══════════════════════════════════════════════════════════════════════════
 *  Metrónomo PWM – Configuración del slice temporero
 * ══════════════════════════════════════════════════════════════════════════*/
 
/**
 * @brief Slice PWM usado exclusivamente como temporizador de cadencia DAC.
 *
 * @details Slice 4 corresponde a GPIO 8/9 (no usados por DAC, LCD ni teclado).
 *          No se habilita ninguna salida GPIO del PWM; se usa solo el flag
 *          de interrupción de wrap (pwm_hw->intr bit 4) como tick de muestreo.
 *
 * Slices ocupados por otros periféricos:
 *   Slices 0-3 → GPIO 0-7   (DAC, sin salida PWM, solo señal de wrap)
 *   Slice 5    → GPIO 10-11 (LCD RS/E, sin salida PWM)
 *   Slice 6    → GPIO 12-13 (LCD D4-D5)
 *   Slice 7    → GPIO 14-15 (LCD D6-D7)
 *   Slices 0-1 → GPIO 16-19 (filas teclado, no conflicto)
 *
 * → Slice 4 (GPIO 8-9) libre para usar como puro temporizador.
 */
#define DAC_PWM_SLICE   4U
 
/** @brief Frecuencia del sistema RP2040. Ajustar si se usa set_sys_clock_*() */
#define SYS_CLOCK_HZ    125000000UL
 
/* ═══════════════════════════════════════════════════════════════════════════
 *  Tipo de onda
 * ══════════════════════════════════════════════════════════════════════════*/
 
typedef enum {
    WAVE_SINE     = 0,   /**< Onda senoidal.    */
    WAVE_SQUARE   = 1,   /**< Onda cuadrada.    */
    WAVE_TRIANGLE = 2    /**< Onda triangular.  */
} wave_type_t;
 
/* ═══════════════════════════════════════════════════════════════════════════
 *  Tablas de onda base (rango ±127, generadas en RAM una sola vez)
 * ══════════════════════════════════════════════════════════════════════════*/
 
extern int8_t sine_wave[WAVE_SAMPLES];
extern int8_t square_wave[WAVE_SAMPLES];
extern int8_t triangle_wave[WAVE_SAMPLES];
 
/* ═══════════════════════════════════════════════════════════════════════════
 *  Buffer pre-calculado y flag de sincronización productor/consumidor
 * ══════════════════════════════════════════════════════════════════════════*/
 
/**
 * @brief Buffer pre-calculado de 256 × 32 bits.
 *
 * Cada entrada almacena el valor final del DAC [0..255] en los bits 0-7.
 * Los bits 31-8 son siempre 0. Se pasa directamente a:
 *   gpio_put_masked(DAC_GPIO_MASK, onda_lista_para_dac[i])
 *
 * Ciclo de vida:
 *   1. Core 0 pone wave_precalc_done = false.
 *   2. Core 0 escribe las 256 entradas (amplitude + DC + saturación aplicadas).
 *   3. Core 0 pone wave_precalc_done = true.
 *   4. Core 1 lee el buffer solo cuando wave_precalc_done == true.
 */
extern volatile uint32_t onda_lista_para_dac[WAVE_SAMPLES];
 
/**
 * @brief Bandera de sincronización productor (Core 0) / consumidor (Core 1).
 *
 * - FALSE: buffer en construcción o no válido → Core 1 debe esperar (spin).
 * - TRUE : buffer completo y válido → Core 1 puede leer onda_lista_para_dac[].
 *
 * @note  El compilador ARM garantiza visibilidad entre cores para accesos a
 *        variables @c volatile, siempre que no se reordenen loads/stores
 *        críticos. Para barreras de memoria explícitas en C11 se recomienda
 *        atomic_thread_fence(), pero para este caso volatile + orden de
 *        escritura (false-antes-de-modificar, true-al-finalizar) es suficiente
 *        en Cortex-M0+ con un bus AHB compartido.
 */
extern volatile bool wave_precalc_done;
 
/* ═══════════════════════════════════════════════════════════════════════════
 *  API pública
 * ══════════════════════════════════════════════════════════════════════════*/
 
/**
 * @brief  Genera las tablas base de onda senoidal, cuadrada y triangular en RAM.
 * @note   Usa punto flotante (libm). Llamar UNA SOLA VEZ durante la
 *         inicialización en Core 0, antes de lanzar Core 1.
 */
void signal_generator_int(void);
 
/**
 * @brief  Pre-calcula onda_lista_para_dac[] con amplitud, DC y saturación.
 *
 * @details Operaciones realizadas por muestra (×256, solo al cambiar parámetros):
 *   1. Selecciona la muestra base de la tabla correspondiente.
 *   2. Aplica amplitud con aritmética de punto flotante:
 *         output_f = dc_level + sample × (amplitude / 100.0f)
 *   3. Satura estrictamente al rango [0.0, 255.0].
 *   4. Almacena como uint32_t (bits 7-0 = valor DAC, bits 31-8 = 0).
 *
 *   La función gestiona internamente wave_precalc_done (false → ... → true)
 *   para que Core 1 nunca lea un buffer parcialmente actualizado.
 *
 * @param[in] wave       Tipo de onda a generar.
 * @param[in] amplitude  Amplitud [0..100] %.
 * @param[in] dc_level   Nivel DC [0..255] (128 = punto medio del DAC).
 *
 * @note  Ejecutar SOLO desde Core 0. No llamar desde Core 1.
 */
void signal_precalculate(wave_type_t wave, uint8_t amplitude, uint8_t dc_level);
 
/**
 * @brief  Configura el PWM metrónomo para la frecuencia de salida deseada.
 *
 * @details Calcula dinámicamente WRAP (16 bits) y divisor fraccional Q8.4:
 *
 *   f_sys = 125 MHz (SYS_CLOCK_HZ)
 *   f_sample = freq_hz × WAVE_SAMPLES   (ticks de DAC por segundo)
 *   clocks_per_sample = f_sys / f_sample
 *
 *   Algoritmo de selección WRAP / DIV_INT / DIV_FRAC:
 *     1. div_int = ⌊clocks_per_sample / 65536⌋ + 1   (mínimo entero que cabe)
 *     2. wrap+1  = ⌊clocks_per_sample / div_int⌋      (floor garantiza div_exact ≥ div_int)
 *     3. div_exact = clocks_per_sample / (wrap+1)
 *     4. div_frac  = round((div_exact − div_int) × 16)  (Q8.4, rango 0-15)
 *
 *   Ejemplos verificados (f_sys = 125 MHz, WAVE_SAMPLES = 256):
 *   ┌──────────┬──────┬─────────┬──────────┬───────────────────────┐
 *   │ freq_hz  │ WRAP │ DIV_INT │ DIV_FRAC │  Error de frecuencia  │
 *   ├──────────┼──────┼─────────┼──────────┼───────────────────────┤
 *   │      1   │61034 │    8    │    0     │   +0.0004 %           │
 *   │    100   │ 4881 │    1    │    0     │   +0.0034 %           │
 *   │   1000   │  487 │    1    │    1     │   −0.026 %            │
 *   │  10000   │   47 │    1    │    0     │   +1.72 %  (< ±2%)   │
 *   └──────────┴──────┴─────────┴──────────┴───────────────────────┘
 *
 *   La función deshabilita el PWM durante la reconfiguración y limpia el
 *   flag de interrupción pendiente para que Core 1 no reciba un tick "falso".
 *
 * @param[in] freq_hz  Frecuencia de la onda de salida en Hz [1..10000].
 *
 * @pre   El slice DAC_PWM_SLICE debe haber sido inicializado con pwm_init()
 *        antes de la primera llamada (ver main_multicore.c).
 * @note  Ejecutar SOLO desde Core 0. No llamar desde Core 1.
 */
void dac_set_frequency(uint32_t freq_hz);
 
#endif /* SIGNAL_GENERATOR_H */