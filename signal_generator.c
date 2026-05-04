/**
 * @file    signal_generator.c
 * @brief   Tablas de onda, pre-cálculo flotante y metrónomo PWM con WRAP dinámico.
 *
 * @details LAB4 – Multicore + PWM Metrónomo:
 *
 *  ┌─────────────────────────────────────────────────────────────────────┐
 *  │  Qué hace este módulo                                               │
 *  │                                                                     │
 *  │  signal_generator_int()   → tablas base en RAM, una sola vez       │
 *  │  signal_precalculate()    → aplica amp/DC con float, satura, guarda │
 *  │                             256 valores listos para gpio_put_masked │
 *  │  dac_set_frequency()      → calcula WRAP + DIV Q8.4, reconfigura   │
 *  │                             el slice PWM como metrónomo de muestreo │
 *  └─────────────────────────────────────────────────────────────────────┘
 *
 *  Diseño de dac_set_frequency (sin búsqueda iterativa):
 *
 *    Sea T = f_sys / (freq_hz × N)   [clocks por muestra]
 *
 *    El PWM interrumpe cada  (WRAP+1) × (DIV_INT + DIV_FRAC/16)  ciclos.
 *    Queremos que esa cantidad sea T.
 *
 *    Paso 1: DIV_INT_min = ⌊T / 65536⌋ + 1    (garantiza WRAP+1 ≤ 65536)
 *    Paso 2: WRAP+1 = ⌊T / DIV_INT_min⌋       (floor: div_exact ≥ DIV_INT)
 *    Paso 3: div_exact = T / (WRAP+1)
 *    Paso 4: DIV_FRAC = round((div_exact − DIV_INT) × 16)  →  [0..15]
 *
 *    El floor en el Paso 2 garantiza que div_exact nunca sea menor que
 *    DIV_INT, haciendo que la parte fraccional sea siempre ≥ 0.
 *
 * @author  davidovich
 * @version 3.0.0 - LAB4 PWM Metrónomo
 * @date    2025
 */
 
#include "signal_generator.h"
 
#include "hardware/pwm.h"     /* pwm_set_wrap, pwm_set_clkdiv_int_frac, etc. */
#include <math.h>             /* sin(), round() — solo en signal_generator_int */
#include <stdint.h>
 
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif
 
/* ═══════════════════════════════════════════════════════════════════════════
 *  Variables globales (declaradas extern en signal_generator.h)
 * ══════════════════════════════════════════════════════════════════════════*/
 
int8_t sine_wave[WAVE_SAMPLES];
int8_t square_wave[WAVE_SAMPLES];
int8_t triangle_wave[WAVE_SAMPLES];
 
/**
 * Buffer pre-calculado. Cada entrada = valor DAC final [0..255] en bits 0-7.
 * Inicializado a false: Core 1 debe esperar el primer signal_precalculate().
 */
volatile uint32_t onda_lista_para_dac[WAVE_SAMPLES];
volatile bool     wave_precalc_done = false;
 
/* ═══════════════════════════════════════════════════════════════════════════
 *  signal_generator_int
 * ══════════════════════════════════════════════════════════════════════════*/
 
void signal_generator_int(void)
{
    for (uint16_t i = 0U; i < WAVE_SAMPLES; i++) {
 
        /* ── Senoidal ─────────────────────────────────────────────────────── */
        sine_wave[i] = (int8_t)round(
            sin((2.0 * M_PI * (double)i) / (double)WAVE_SAMPLES) * 127.0);
 
        /* ── Cuadrada ─────────────────────────────────────────────────────── */
        square_wave[i] = (i < (WAVE_SAMPLES / 2U)) ? 127 : -127;
 
        /* ── Triangular ───────────────────────────────────────────────────── */
        if (i < (WAVE_SAMPLES / 2U)) {
            triangle_wave[i] = (int8_t)(-127 + (int16_t)(i * 2U));
        } else {
            triangle_wave[i] = (int8_t)(127 - (int16_t)((i - 128U) * 2U));
        }
    }
}
 
/* ═══════════════════════════════════════════════════════════════════════════
 *  signal_precalculate
 * ══════════════════════════════════════════════════════════════════════════*/
 
void signal_precalculate(wave_type_t wave, uint8_t amplitude, uint8_t dc_level)
{
    /* ── Paso 1: Señalizar a Core 1 que el buffer NO es válido ───────────── */
    /*
     * Este write a FALSE debe ocurrir antes de cualquier modificación del
     * buffer. Al ser volatile, el compilador no reordenará las escrituras
     * posteriores por encima de ésta.
     * Core 1 detecta FALSE y entra en spin-wait, evitando leer datos parciales.
     */
    wave_precalc_done = false;
 
    /* ── Paso 2: Pre-computar el factor de escala (una sola división) ─────── */
    /*
     * Se usa float temporario para máxima precisión en la multiplicación
     * muestra × amplitud. Esto ocurre 256 veces por cambio de parámetros
     * (evento de usuario infrecuente), nunca en el loop del DAC.
     */
    const float amp_scale = (float)amplitude / 100.0f;
    const float dc_f      = (float)dc_level;
 
    /* ── Paso 3: Calcular y almacenar las 256 muestras ────────────────────── */
    for (uint16_t i = 0U; i < WAVE_SAMPLES; i++) {
 
        /* 3a. Muestra base según tipo de onda */
        int8_t sample;
        switch (wave) {
            case WAVE_SINE:     sample = sine_wave[i];     break;
            case WAVE_SQUARE:   sample = square_wave[i];   break;
            case WAVE_TRIANGLE: sample = triangle_wave[i]; break;
            default:            sample = 0;                break;
        }
 
        /*
         * 3b. Aplicar amplitud y nivel DC con aritmética flotante.
         *
         *   output = DC + sample × (amplitude / 100)
         *
         * Usar float (en lugar de aritmética entera × / 100) evita el error
         * de truncamiento que se acumula especialmente en amplitudes bajas.
         * Ej: amplitude=1 → amp_scale=0.01; sample=100 → output_f = DC+1.0
         *     Con enteros: (100 * 1) / 100 = 1 ✓, pero (99 * 1) / 100 = 0 ✗
         */
        float output_f = dc_f + ((float)sample * amp_scale);
 
        /* 3c. Saturación estricta al rango del DAC [0..255] */
        if (output_f > 255.0f) { output_f = 255.0f; }
        if (output_f <   0.0f) { output_f =   0.0f; }
 
        /*
         * 3d. Almacenar como uint32_t para gpio_put_masked().
         *
         * DAC en GPIO 0-7 → el valor de 8 bits ocupa exactamente los bits 0-7.
         * Los bits 31-8 quedan en 0. gpio_put_masked(DAC_GPIO_MASK, val) con
         * DAC_GPIO_MASK = 0xFF garantiza que solo se alteran los pines del DAC,
         * sin tocar los pines del LCD (GPIO 10-15) ni del teclado (GPIO 16-26).
         *
         * La conversión (uint8_t)output_f es segura porque output_f ∈ [0, 255].
         */
        onda_lista_para_dac[i] = (uint32_t)(uint8_t)output_f;
    }
 
    /* ── Paso 4: Publicar el buffer ──────────────────────────────────────── */
    /*
     * Este write a TRUE debe ocurrir después de que todas las entradas del
     * buffer hayan sido escritas. volatile + Cortex-M0+ (sin reordenamiento
     * de stores en la arquitectura) garantiza visibilidad correcta en Core 1.
     */
    wave_precalc_done = true;
}
 
/* ═══════════════════════════════════════════════════════════════════════════
 *  dac_set_frequency – Metrónomo PWM con WRAP y divisor Q8.4 dinámicos
 * ══════════════════════════════════════════════════════════════════════════*/
 
void dac_set_frequency(uint32_t freq_hz)
{
    /*
     * ── Fundamento matemático ───────────────────────────────────────────────
     *
     * f_sample  = freq_hz × WAVE_SAMPLES    [muestras/segundo]
     * T_sample  = f_sys / f_sample          [ciclos de reloj por muestra]
     *
     * El PWM genera el flag de interrupción (intr) cada:
     *   (WRAP + 1) × (DIV_INT + DIV_FRAC / 16)  ciclos
     *
     * Queremos igualar T_sample. Los registros del RP2040:
     *   WRAP:     uint16_t → máx 65535  ⟹  WRAP+1 ∈ [2, 65536]
     *   DIV_INT:  uint8_t  → rango [1, 255]  (0 no permitido en modo free-running)
     *   DIV_FRAC: nibble   → rango [0, 15]   (representa 0 a 15/16)
     *
     * ── Algoritmo (sin búsqueda iterativa) ─────────────────────────────────
     *
     *   Paso 1 (DIV_INT):
     *     div_int = ⌊T_sample / 65536⌋ + 1
     *     Con este valor mínimo, WRAP+1 = T_sample/div_int ≤ 65536.
     *
     *   Paso 2 (WRAP):
     *     wrap_plus_one = ⌊T_sample / div_int⌋   ← floor, NO round
     *     El floor garantiza div_exact = T_sample/(WRAP+1) ≥ div_int,
     *     por lo tanto (div_exact - div_int) ≥ 0 → DIV_FRAC siempre válido.
     *
     *   Paso 3 (DIV_FRAC):
     *     div_exact = T_sample / wrap_plus_one
     *     frac_part = div_exact − div_int          ∈ [0, 1)  por construcción
     *     div_frac  = round(frac_part × 16)        ∈ [0, 16]
     *     Si div_frac == 16: div_frac=0, div_int++ (carry al entero)
     */
 
    const float f_sys           = (float)SYS_CLOCK_HZ;
    const float f_sample        = (float)freq_hz * (float)WAVE_SAMPLES;
    const float clocks_per_samp = f_sys / f_sample;
 
    /* ── Paso 1: DIV_INT mínimo ──────────────────────────────────────────── */
    uint32_t div_int = (uint32_t)(clocks_per_samp / 65536.0f) + 1U;
    if (div_int < 1U)   { div_int = 1U;   }   /* Sanity: mínimo absoluto    */
    if (div_int > 255U) { div_int = 255U; }   /* Límite del registro Q8.4   */
 
    /* ── Paso 2: WRAP+1 con floor (garantiza DIV_FRAC ≥ 0) ──────────────── */
    uint32_t wrap_plus_one = (uint32_t)(clocks_per_samp / (float)div_int);
    if (wrap_plus_one > 65536U) { wrap_plus_one = 65536U; }   /* Límite WRAP */
    if (wrap_plus_one < 2U)     { wrap_plus_one = 2U;     }   /* Mín útil    */
 
    /* ── Paso 3: Divisor fraccional Q8.4 ─────────────────────────────────── */
    /*
     * div_exact ≥ div_int  (garantizado por el floor del paso 2)
     * frac_part ∈ [0.0, 1.0)
     */
    const float div_exact = clocks_per_samp / (float)wrap_plus_one;
    const float frac_part = div_exact - (float)div_int;
 
    uint32_t div_frac = (uint32_t)(frac_part * 16.0f + 0.5f);  /* round */
    if (div_frac > 15U) {
        /* Carry: la fracción redondeó a 1 → incrementar el entero */
        div_frac = 0U;
        if (div_int < 255U) { div_int++; }
    }
 
    const uint16_t wrap = (uint16_t)(wrap_plus_one - 1U);
 
    /* ── Paso 4: Aplicar al hardware PWM ─────────────────────────────────── */
    /*
     * Secuencia de escritura segura:
     *   1. Deshabilitar el PWM: el contador se detiene, no genera nuevos ticks.
     *   2. Limpiar cualquier flag de interrupción pendiente en el slice.
     *      (Si Core 1 está en spin-wait del precalc, el flag acumulado se descarta
     *       aquí; Core 1 limpiará el flag al reentrar en su loop principal.)
     *   3. Actualizar WRAP y DIVISOR.
     *   4. Re-habilitar: el contador reinicia desde 0 con los nuevos parámetros.
     *
     * No es necesario proteger con sección crítica: el único escritor de estos
     * registros es Core 0 y el único lector de intr es Core 1 (y solo lee).
     */
    pwm_set_enabled(DAC_PWM_SLICE, false);
    pwm_hw->intr = (1u << DAC_PWM_SLICE);                            /* Clear */
    pwm_set_wrap(DAC_PWM_SLICE, wrap);
    pwm_set_clkdiv_int_frac(DAC_PWM_SLICE, (uint8_t)div_int, (uint8_t)div_frac);
    pwm_set_enabled(DAC_PWM_SLICE, true);
}