/**
 * @file    keypad.c
 * @brief   Driver de teclado matricial 4×4 – FSM asíncrona sin bloqueos.
 *
 * @details LAB3: Refactorización completa del escaneo para eliminar todo sleep_us().
 *
 *          ANTES (versión polling):
 *            kp_scan() → for(4 filas) { pull_LOW; sleep_us(2); leer_cols; pull_HIGH }
 *            Tiempo bloqueado: 4 × 2 µs = 8 µs mínimo por llamada. Si se añadía
 *            retardo de lectura GPIO, podía llegar a 50+ µs.
 *
 *          AHORA
 *            kp_scan() → ejecuta UNA transición de la FSM de 3 estados.
 *            Tiempo de ejecución por llamada: < 500 ns (solo lectura de timer + GPIO).
 *            Un ciclo completo de 4 filas toma N llamadas distribuidas en el tiempo;
 *            el Core 0 nunca se detiene esperando estabilización eléctrica.
 *
 *          Diagrama de la FSM por fila:
 *          ┌─────────────────────────────────────────────────────────────┐
 *          │                                                             │
 *          │  [ACTIVATE] ──► [WAIT] ──► [READ] ──► [ACTIVATE(next_row)] │
 *          │   pull LOW       ≥2µs?      leer       pull HIGH           │
 *          │   timestamp      no→return  cols       avanzar fila        │
 *          │                            process     ───────────────────►│
 *          │                            (al cerrar el ciclo de 4 filas) │
 *          └─────────────────────────────────────────────────────────────┘
 *
 * @author  
 * @version 2.0.0 - LAB4 Multicore
 * @date    
 */

#include "keypad.h"
#include "hardware/timer.h"     /* time_us_64() */

/* ═══════════════════════════════════════════════════════════════════════════
 *  Mapa de caracteres
 * ══════════════════════════════════════════════════════════════════════════*/

/** @brief Tabla de caracteres del teclado 4×4. Índices: [fila][columna]. */
static const char KP_MAP[KP_ROWS][KP_COLS] = {
    {'1', '2', '3', 'A'},
    {'4', '5', '6', 'B'},
    {'7', '8', '9', 'C'},
    {'*', '0', '#', 'D'}
};

/* ═══════════════════════════════════════════════════════════════════════════
 *  API pública
 * ══════════════════════════════════════════════════════════════════════════*/

void kp_init(keypad_t        *kpad,
             const uint8_t    row_pins[KP_ROWS],
             const uint8_t    col_pins[KP_COLS],
             uint64_t         debounce_us)
{
    /* Anti-rebote y estado de tecla */
    kpad->debounce_us    = debounce_us;
    kpad->last_event_us  = 0U;
    kpad->last_key       = KP_NO_KEY;
    kpad->key_ready      = false;

    /* ── Estado inicial de la FSM asíncrona ─────────────────────────────── */
    // Inicializar la FSM en el estado de arranque de un ciclo.
    kpad->scan_phase    = KP_PHASE_ACTIVATE;
    kpad->current_row   = 0U;
    kpad->row_active_ts = 0U;
    kpad->scan_detected = KP_NO_KEY;

    /* ── Filas: salidas en nivel alto (reposo = inactivo) ────────────────── */
    for (uint8_t r = 0U; r < KP_ROWS; r++) {
        kpad->row_pins[r] = row_pins[r];
        gpio_init(row_pins[r]);
        gpio_set_dir(row_pins[r], GPIO_OUT);
        gpio_put(row_pins[r], 1);           /* HIGH = inactivo */
    }

    /* ── Columnas: entradas con pull-up interno ──────────────────────────── */
    for (uint8_t c = 0U; c < KP_COLS; c++) {
        kpad->col_pins[c] = col_pins[c];
        gpio_init(col_pins[c]);
        gpio_set_dir(col_pins[c], GPIO_IN);
        gpio_pull_up(col_pins[c]);          /* LOW = tecla presionada */
    }
}

/* ─────────────────────────────────────────────────────────────────────────── */

// [ Implementación de la FSM asíncrona de 3 estados.
void kp_scan(keypad_t *kpad)
{
    uint64_t now = time_us_64();

    switch (kpad->scan_phase) {

        /* ══ Estado ACTIVATE: Activar fila y registrar timestamp ══════════ */
        case KP_PHASE_ACTIVATE:

            // Al inicio de cada ciclo completo (fila 0),
            //               limpiar el resultado del ciclo anterior.
            if (kpad->current_row == 0U) {
                kpad->scan_detected = KP_NO_KEY;
            }

            /* Pull LOW para energizar la fila actual */
            gpio_put(kpad->row_pins[kpad->current_row], 0);

            //  Guardar timestamp del momento de activación.
            //               La espera de estabilización se hará de forma no-bloqueante
            //               en el siguiente estado. Sin sleep_us() aquí.
            kpad->row_active_ts = now;
            kpad->scan_phase    = KP_PHASE_WAIT;
            break;

        /* ══ Estado WAIT: Espera no-bloqueante de estabilización (≥ 2 µs) ═ */
        case KP_PHASE_WAIT:

            // [Comparar tiempo transcurrido sin detener la ejecución.
            //               Si aún no pasaron 2 µs, RETORNAR INMEDIATAMENTE.
            //               El Core 0 continúa con otras tareas y volverá aquí
            //               en la próxima iteración del super-loop.
            if ((now - kpad->row_active_ts) < 2U) {
                return;   /* ← Punto de retorno no-bloqueante: Core 0 libre */
            }

            /* Han pasado ≥ 2 µs: transicionar a lectura */
            kpad->scan_phase = KP_PHASE_READ;

            /* FALL-THROUGH intencional al estado READ */
            /* fall through */

        /* ══ Estado READ: Leer columnas, desactivar fila, avanzar ═════════ */
        case KP_PHASE_READ: {

            //  Solo registrar la primera tecla detectada en el ciclo.
            //               Política "primer contacto gana" para manejo de una tecla.
            if (kpad->scan_detected == KP_NO_KEY) {
                for (uint8_t c = 0U; c < KP_COLS; c++) {
                    if (!gpio_get(kpad->col_pins[c])) {  /* LOW → tecla presionada */
                        kpad->scan_detected = KP_MAP[kpad->current_row][c];
                        break;
                    }
                }
            }

            /* Desactivar fila (pull HIGH = reposo) */
            gpio_put(kpad->row_pins[kpad->current_row], 1);

            /* Avanzar al siguiente número de fila */
            kpad->current_row++;

            /* ── ¿Se completó el ciclo de 4 filas? ─────────────────────── */
            if (kpad->current_row >= KP_ROWS) {
                kpad->current_row = 0U;     /* Reiniciar para el próximo ciclo */

                char detected = kpad->scan_detected;

                // nti-rebote temporal: idéntica lógica que en la
                //               versión original pero integrada en la FSM asíncrona.
                //               Se evalúa una vez por ciclo completo de 4 filas.
                if ((now - kpad->last_event_us) >= kpad->debounce_us) {

                    if (!kpad->key_ready) {
                        if (detected != KP_NO_KEY && detected != kpad->last_key) {
                            /* Tecla nueva detectada: registrar y señalizar */
                            kpad->last_key      = detected;
                            kpad->key_ready     = true;
                            kpad->last_event_us = now;
                        } else if (detected == KP_NO_KEY &&
                                   kpad->last_key != KP_NO_KEY) {
                            /* Sin tecla activa: limpiar para permitir re-pulsación */
                            kpad->last_key      = KP_NO_KEY;
                            kpad->last_event_us = now;
                        }
                    }
                }
            }

            /* Volver a ACTIVATE para la siguiente fila (o nuevo ciclo) */
            kpad->scan_phase = KP_PHASE_ACTIVATE;
            break;
        }

        default:
            /* Estado inválido: recuperar */
            kpad->scan_phase  = KP_PHASE_ACTIVATE;
            kpad->current_row = 0U;
            break;
    }
}

/* ─────────────────────────────────────────────────────────────────────────── */

bool kp_has_key(const keypad_t *kpad)
{
    return kpad->key_ready;
}

/* ─────────────────────────────────────────────────────────────────────────── */

char kp_get_key(keypad_t *kpad)
{
    if (!kpad->key_ready) {
        return KP_NO_KEY;
    }
    kpad->key_ready = false;    /* Consumir la tecla */
    return kpad->last_key;
}
