/**
 * @file    main_multicore.c
 * @brief   LAB4 – Generador de Señales: Multicore + PWM Metrónomo.
 *
 * @details Arquitectura de alta fidelidad que supera la limitación de 3.9 kHz
 *          del timer de 64 bits (time_us_64) y elimina el jitter de ±5-20 µs:
 *
 *  ┌──────────────────────────────────────────────────────────────────────┐
 *  │  CORE 0 – Interfaz de Usuario + Gestor de Parámetros                │
 *  │                                                                      │
 *  │  ① kp_scan()              FSM asíncrona: UNA transición por llamada  │
 *  │  ② menu_process_key()     FSM del menú de usuario                   │
 *  │  ③ signal_precalculate()  Pre-cálculo con float (256 muestras × 1)  │
 *  │  ④ dac_set_frequency()    Recalcula WRAP/DIV del PWM metrónomo      │
 *  │  ⑤ menu_update_display()  Actualización LCD (bloqueante, OK aquí)   │
 *  │                                                                      │
 *  │  Produce: onda_lista_para_dac[] + configura el slice PWM            │
 *  └──────────────────────────────┬───────────────────────────────────────┘
 *                                 │  wave_precalc_done (volatile bool)
 *                                 │  onda_lista_para_dac[] (volatile u32[256])
 *                                 │  pwm_hw->intr  (registro hardware)
 *                                 ▼
 *  ┌──────────────────────────────────────────────────────────────────────┐
 *  │  CORE 1 – DAC Tiempo-Real (__not_in_flash_func, RAM)                 │
 *  │                                                                      │
 *  │  while (!(pwm_hw->intr & tick)) { }  ← polling hardware, ~8 ns     │
 *  │  pwm_hw->intr = tick;                ← limpiar flag                 │
 *  │  gpio_put_masked(DAC_GPIO_MASK,      ← escritura GPIO protegida     │
 *  │                  buf[idx]);                                          │
 *  │  idx = (idx + 1) & 0xFF;            ← avance sin branch ni módulo  │
 *  │                                                                      │
 *  │  Sin LCD. Sin teclado. Sin float. Sin multiplicaciones. Sin switch. │
 *  │  Jitter máximo = ±1 ciclo de reloj (±8 ns @ 125 MHz).              │
 *  └──────────────────────────────────────────────────────────────────────┘
 *
 * @par ¿Por qué __not_in_flash_func mejora el jitter?
 *
 *  El RP2040 ejecuta código desde Flash a través del XIP (Execute-In-Place)
 *  con caché de 16 KB. Un cache miss introduce una latencia variable de
 *  14-20 ciclos (~112-160 ns), lo que causa jitter impredecible. Al marcar
 *  core1_dac_task con __not_in_flash_func, el linker la ubica en la sección
 *  .time_critical (SRAM), eliminando por completo el acceso a Flash durante
 *  el loop. Las funciones inline (gpio_put_masked, tight_loop_contents) se
 *  compilan dentro de la función y también quedan en SRAM.
 *
 * @par Asignación de pines (LAB4 – sin conflictos)
 * @code
 *  DAC R-2R 8 bits (contiguo)  : GPIO 0-7   (antes disperso: 0-3 + 22,26,27,28)
 *  LCD 16x2 (modo 4 bits)      : RS=GPIO10, E=GPIO11, D4=GPIO12..D7=GPIO15
 *  Keypad 4×4 – Filas (out)    : GPIO 16, 17, 18, 19
 *  Keypad 4×4 – Columnas (in)  : GPIO 20, 21, 22, 26
 *  PWM metrónomo (sin GPIO out): Slice 4  (GPIO 8-9 libres, no conectados)
 *
 *  CORRECCIÓN vs. LAB3: DAC_PIN_B4..B7 en LAB3 tenían valores 4,5,6,7
 *  en lugar de 22,26,27,28 (bug pre-existente). DAC_GPIO_MASK resultaba
 *  0xFF de todas formas, por lo que el DAC ya operaba en GPIO 0-7. LAB4
 *  formaliza esto, libera GPIO 22/26 para las columnas del teclado (su
 *  posición original) y elimina dac_byte_to_gpio_mask().
 * @endcode
 *
 * @par Para compilar:
 * @code
 *  # CMakeLists.txt – agregar hardware_pwm al target:
 *  target_link_libraries(signal_multicore
 *      pico_stdlib pico_multicore hardware_pwm m)
 *
 *  mkdir build && cd build
 *  cmake .. -DPICO_SDK_PATH=/ruta/al/sdk
 *  make signal_multicore
 * @endcode
 *
 * @author  davidovich
 * @version 3.0.0 - LAB4 PWM Metrónomo
 * @date    2025
 */
 
#include <stdio.h>
 
#include "pico/stdlib.h"
#include "pico/multicore.h"         /* multicore_launch_core1()             */
#include "hardware/gpio.h"          /* gpio_init_mask(), gpio_put_masked()  */
#include "hardware/pwm.h"           /* pwm_config, pwm_init(), pwm_hw      */
 
#include "signal_generator.h"       /* onda_lista_para_dac, dac_set_frequency */
#include "LCD_16x2_H.h"
#include "keypad.h"
#include "menu.h"
 
/* ═══════════════════════════════════════════════════════════════════════════
 *  Asignación de pines
 * ══════════════════════════════════════════════════════════════════════════*/
 
/* ── LCD 16×2 (modo 4 bits) ─────────────────────────────────────────────── */
#define LCD_RS_PIN   10U
#define LCD_E_PIN    11U
#define LCD_D4_PIN   12U
#define LCD_D5_PIN   13U
#define LCD_D6_PIN   14U
#define LCD_D7_PIN   15U
 
/* ── Teclado matricial 4×4 ──────────────────────────────────────────────── */
 
/** @brief Filas del teclado (salidas, activo en LOW). */
static const uint8_t KP_ROW_PINS[KP_ROWS] = {16U, 17U, 18U, 19U};
 
/**
 * @brief Columnas del teclado (entradas con pull-up interno).
 *

 */
static const uint8_t KP_COL_PINS[KP_COLS] = {20U, 21U, 22U, 26U};
 
/* ═══════════════════════════════════════════════════════════════════════════
 *  CORE 1 – Tarea del DAC (ejecutada desde SRAM)
 * ══════════════════════════════════════════════════════════════════════════*/
 
/**
 * @brief  Loop de tiempo-crítico del DAC. Ejecuta desde SRAM, no desde Flash.
 *
 * @details __not_in_flash_func coloca esta función en la sección .time_critical
 *          (SRAM). Al no requerir accesos al XIP se elimina el jitter por
 *          cache miss de Flash (hasta ±160 ns). Las llamadas inline de
 *          gpio_put_masked y tight_loop_contents también quedan en SRAM.
 *
 *          Loop principal (3 instrucciones activas tras el wait):
 *          ┌──────────────────────────────────────────────────────────┐
 *          │  while (!(pwm_hw->intr & BIT)) {}  │ spin <8 ns/iter    │
 *          │  pwm_hw->intr = BIT;               │ clear flag         │
 *          │  gpio_put_masked(MASK, buf[idx]);   │ escritura DAC      │
 *          │  idx = (idx + 1) & 0xFF;           │ avance cíclico     │
 *          └──────────────────────────────────────────────────────────┘
 *
 *          El AND bit a bit `& 0xFF` para el avance del índice es idéntico
 *          a `% 256` pero sin instrucción de división: el compilador genera
 *          un ANDS de un solo ciclo.
 *
 *          Esta función NO retorna nunca.
 */
static void __not_in_flash_func(core1_dac_task)(void)
{
    /* Pre-computar la máscara del slice para no acceder al símbolo en Flash */
    const uint32_t pwm_irq_bit = (1u << DAC_PWM_SLICE);
 
    uint32_t idx = 0U;
 
    /* ── Barrera de arranque: esperar el primer pre-cálculo de Core 0 ──── */
    /*
     * Core 0 configura el buffer y el PWM antes de lanzar Core 1, pero la
     * barrera protege contra cualquier reordenamiento imprevisto del enlazador.
     * tight_loop_contents() es una barrera de compilador (previene que el
     * optimizador elimine el bucle) sin costo en hardware en Cortex-M0+.
     */
    while (!wave_precalc_done) {
        tight_loop_contents();
    }
 
    /* Limpiar cualquier flag acumulado durante la espera de inicialización */
    pwm_hw->intr = pwm_irq_bit;
 
    /* ══════════════════════════════════════════════════════════════════════
     *  LOOP DAC – CORE 1 (SRAM, sin Flash)
     *
     *  Timing real estimado @ 125 MHz:
     *    - Ciclo idle (spin en while): ~1 ciclo = 8 ns por iteración
     *    - pwm_hw->intr = BIT (clear):   1 ciclo (write MMR)
     *    - gpio_put_masked():             ~4 ciclos (inline, SIO single-cycle)
     *    - idx = (idx+1) & 0xFF:         1 ciclo (ANDS inmediato)
     *    Total activo por muestra: ~6-8 ciclos = ~50-64 ns
     *
     *  Jitter real: ±1 ciclo de reloj (±8 ns). El flag PWM se set en hardware
     *  en el ciclo exacto del wrap; no hay comparación de software.
     * ═════════════════════════════════════════════════════════════════════ */
    while (1) {
 
        /* ── 1. Esperar el tick del metrónomo PWM (polling no-bloqueante) ── */
        /*
         * pwm_hw->intr es el registro de estado de interrupción del PWM.
         * El hardware lo pone en '1' en el ciclo exacto en que el contador
         * alcanza WRAP (overflow del período). Es una lectura de registro MMIO
         * de 32 bits: determinista, sin latencia de caché, sin llamada a función.
         */
        while (!(pwm_hw->intr & pwm_irq_bit)) {
            tight_loop_contents();
        }
 
        /* ── 2. Limpiar el flag inmediatamente ──────────────────────────── */
        /*
         * Escribir 1 al bit del slice en INTR lo limpia (write-to-clear).
         * Debe hacerse ANTES de la escritura GPIO para minimizar la latencia
         * entre el tick y la actualización del DAC.
         * Si se limpiara después de gpio_put_masked, el jitter aumentaría
         * en los ~4 ciclos de la escritura GPIO.
         */
        pwm_hw->intr = pwm_irq_bit;
 
        /* ── 3. Verificar validez del buffer (Core 0 re-calculando?) ─────── */
        /*
         * Si Core 0 está actualizando el buffer (params_changed),
         * wave_precalc_done cae a FALSE. Core 1 entra en spin-wait.
         * Al retomar, reinicia desde idx=0 y limpia flags acumulados.
         * Este código se activa solo en eventos de usuario (infrecuentes).
         */
        if (!wave_precalc_done) {
            while (!wave_precalc_done) {
                tight_loop_contents();
            }
            idx = 0U;
            /* Descartar ticks acumulados mientras el buffer se actualizaba */
            pwm_hw->intr = pwm_irq_bit;
            continue;
        }
 
        /* ── 4. Escribir muestra al DAC ──────────────────────────────────── */
        /*
         * gpio_put_masked(mask, value) es una función inline del SDK que
         * compila a:  sio_hw->gpio_togl = (sio_hw->gpio_out ^ value) & mask
         * → Una sola escritura al SIO (Single-cycle IO), latencia 1 ciclo.
         *
         * La máscara DAC_GPIO_MASK (0xFF) garantiza que solo se modifican
         * GPIO 0-7; los pines del LCD (10-15) y teclado (16-26) no se alteran.
         */
        gpio_put_masked(DAC_GPIO_MASK, onda_lista_para_dac[idx]);
 
        /* ── 5. Avanzar índice cíclico con AND bit a bit ─────────────────── */
        /*
         * (idx + 1) & 0xFF  es equivalente a  (idx + 1) % 256
         * pero genera un ANDS de 1 ciclo en Thumb-2 (sin instrucción UDIV).
         * Funciona porque WAVE_SAMPLES = 256 = 2^8, potencia de 2 exacta.
         */
        idx = (idx + 1U) & 0xFFU;
        

        busy_wait_us(5000); /* Simulación de carga en Core 1: 5 ms de trabajo por muestra (LAB4) */ 

    } /* fin while(1) – Core 1 */
}
 
/* ═══════════════════════════════════════════════════════════════════════════
 *  MAIN – Core 0 (Punto de entrada del programa)
 * ══════════════════════════════════════════════════════════════════════════*/
 
int main(void)
{
    /* ── 0. Inicialización del sistema ──────────────────────────────────── */
    stdio_init_all();
 
    /* ── 1. Generar tablas base de onda en RAM ──────────────────────────── */
    /*
     * Usa sin() y round() de libm. Ejecuta solo aquí, durante el boot;
     * nunca en el loop del DAC. Core 1 no se ha lanzado todavía.
     */
    signal_generator_int();
 
    /* ── 2. Configurar GPIO 0-7 como salidas del DAC R-2R ───────────────── */
    /*
     * DAC_GPIO_MASK = 0xFF (GPIO 0 = bit 0 = LSB, GPIO 7 = bit 7 = MSB).
     * Arrancar con 0 V en el DAC (código 0x00 = punto más bajo).
     */
    gpio_init_mask(DAC_GPIO_MASK);
    gpio_set_dir_out_masked(DAC_GPIO_MASK);
    gpio_put_masked(DAC_GPIO_MASK, 0U);
 
    /* ── 3. Inicializar el slice PWM como temporizador puro ─────────────── */
    /*
     * Modo free-running: el contador incrementa cada ciclo de reloj dividido
     * por (DIV_INT + DIV_FRAC/16). No se habilita ninguna salida GPIO del PWM.
     * dac_set_frequency() configurará WRAP y DIVISOR más adelante.
     *
     * pwm_config_set_phase_correct(&cfg, false) → modo up-count estándar.
     * No usar phase-correct (up-down) porque duplicaría el período efectivo.
     */
    pwm_config pwm_cfg = pwm_get_default_config();
    pwm_config_set_clkdiv_mode(&pwm_cfg, PWM_DIV_FREE_RUNNING);
    pwm_config_set_phase_correct(&pwm_cfg, false);
    pwm_init(DAC_PWM_SLICE, &pwm_cfg, false);   /* Configurar, NO iniciar aún */
 
    /* ── 4. Inicializar LCD 16×2 ────────────────────────────────────────── */
    lcd_t lcd;
    lcd_init(&lcd,
             LCD_RS_PIN, LCD_E_PIN,
             LCD_D4_PIN, LCD_D5_PIN, LCD_D6_PIN, LCD_D7_PIN);
 
    /* ── 5. Inicializar teclado matricial (FSM asíncrona, sin sleep_us) ─── */
    keypad_t keypad;
    kp_init(&keypad, KP_ROW_PINS, KP_COL_PINS, KP_DEBOUNCE_US);
 
    /* ── 6. Inicializar FSM del menú (params_changed = true por defecto) ── */
    menu_t menu;
    menu_init(&menu);
 
    /* ── 7. Pre-cálculo inicial y configuración del metrónomo ───────────── */
    /*
     * Orden obligatorio:
     *   a) signal_precalculate() → llena el buffer y pone wave_precalc_done=true
     *   b) dac_set_frequency()   → configura WRAP/DIV e inicia el PWM
     *   c) multicore_launch_core1() → Core 1 empieza a consumir el buffer
     *
     * Si el orden fuera b→a, Core 1 podría recibir ticks del PWM antes de
     * que el buffer esté listo (wave_precalc_done=false los descartaría de
     * todas formas, pero el orden correcto es más claro y robusto).
     */
    signal_precalculate(menu.params.wave,
                        menu.params.amplitude,
                        menu.params.dc_level);
 
    dac_set_frequency(menu.params.freq_hz);
 
    menu.params_changed       = false;
    menu.needs_display_update = true;
 
    /* ── 8. Lanzar Core 1 (loop del DAC desde SRAM) ─────────────────────── */
    /*
     * A partir de aquí, core1_dac_task() corre de forma completamente
     * independiente en Core 1. No comparte stack ni registros con Core 0.
     * El DAC mantiene la señal incluso si Core 0 se bloquea 10 ms en el LCD.
     */
    multicore_launch_core1(core1_dac_task);
 
    /* ── 9. Mostrar pantalla inicial ────────────────────────────────────── */
    menu_update_display(&menu, &lcd);
 
    /* ══════════════════════════════════════════════════════════════════════
     *  SUPER-LOOP – CORE 0 (Interfaz de Usuario)
     *
     *  Sin restricciones temporales estrictas. Puede bloquearse hasta ~10 ms
     *  en menu_update_display() (protocolo HD44780) sin afectar la señal DAC,
     *  porque Core 1 es completamente independiente.
     *
     *  Flujo ante un cambio de parámetros:
     *    params_changed = true
     *    ↓
     *    signal_precalculate()   → wave_precalc_done: TRUE→FALSE→TRUE
     *    dac_set_frequency()     → PWM: stop → reconfigura → start
     *    ↓
     *    Core 1 detecta wave_precalc_done=false → spin
     *    Core 1 detecta wave_precalc_done=true  → reanuda con nueva onda y freq
     * ═════════════════════════════════════════════════════════════════════ */
    while (1) {
 
        /* ── A. Escaneo del teclado (FSM asíncrona, < 500 ns por llamada) ── */
        kp_scan(&keypad);
 
        /* ── B. Procesar tecla si hay una disponible ─────────────────────── */
        if (kp_has_key(&keypad)) {
            char key = kp_get_key(&keypad);
            menu_process_key(&menu, key);
        }
 
        /* ── C. Actualizar onda y frecuencia si el usuario confirmó cambios ─ */
        if (menu.params_changed) {
            menu.params_changed = false;
 
            /*
             * Orden: primero pre-calcular (onda), luego actualizar frecuencia.
             *
             * signal_precalculate():
             *   - Baja wave_precalc_done → Core 1 entra en spin-wait.
             *   - Aplica amplitude/DC con float sobre la tabla base.
             *   - Sube wave_precalc_done → Core 1 reanuda.
             *
             * dac_set_frequency():
             *   - Detiene el PWM, recalcula WRAP y DIV, reinicia el PWM.
             *   - Core 1 continuará recibiendo ticks con la nueva cadencia.
             *   - Si wave_precalc_done está en FALSE durante esta ventana,
             *     Core 1 ya está en spin-wait y los ticks PWM se descartan
             *     limpiamente en la sección del check de validez del buffer.
             */
            signal_precalculate(menu.params.wave,
                                menu.params.amplitude,
                                menu.params.dc_level);
 
            dac_set_frequency(menu.params.freq_hz);
        }
 
        /* ── D. Actualizar el LCD si fue solicitado ─────────────────────── */
        /*
         * Puede tardar hasta ~10 ms (protocolo HD44780 con sleep_us/ms).
         * En LAB4 esto no introduce jitter en el DAC: Core 1 es independiente.
         */
        if (menu.needs_display_update) {
            menu_update_display(&menu, &lcd);
        }
 
    } /* fin while(1) – Core 0 */
 
    return 0;   /* Inalcanzable */
}
 