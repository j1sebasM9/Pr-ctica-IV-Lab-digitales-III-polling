/**
 * @file    LCD_16x2_H.h
 * @brief   Driver para LCD 16×2 Hitachi HD44780 en modo de 4 bits (GPIO bit-bang).
 *
 * @note    Este driver es bloqueante por diseño (usa sleep_us/sleep_ms).
 *          En la arquitectura LAB3 Multicore, esto es ACEPTABLE porque el LCD
 *          opera exclusivamente en el Core 0, y el Core 1 (DAC) es completamente
 *          independiente. Las demoras del LCD ya NO introducen jitter en la señal.
 *
 * @author  Firmware Engineering Lab
 * @version 1.0.0
 * @date    2026
 */

#ifndef LCD_16X2_H_H
#define LCD_16X2_H_H

#include <stdint.h>
#include <stdbool.h>
#include "hardware/gpio.h"
#include "pico/time.h"

/* ═══════════════════════════════════════════════════════════════════════════
 *  Estructura de control del LCD
 * ══════════════════════════════════════════════════════════════════════════*/

/**
 * @brief  Descriptor de instancia del LCD 16×2 en modo 4 bits.
 */
typedef struct {
    uint8_t rs_pin;     /**< GPIO del pin Register Select (RS).  */
    uint8_t e_pin;      /**< GPIO del pin Enable (E).            */
    uint8_t d4_pin;     /**< GPIO del pin de dato D4.            */
    uint8_t d5_pin;     /**< GPIO del pin de dato D5.            */
    uint8_t d6_pin;     /**< GPIO del pin de dato D6.            */
    uint8_t d7_pin;     /**< GPIO del pin de dato D7.            */
} lcd_t;

/* ═══════════════════════════════════════════════════════════════════════════
 *  API pública
 * ══════════════════════════════════════════════════════════════════════════*/

/**
 * @brief  Inicializa el LCD siguiendo la secuencia de arranque del datasheet HD44780.
 * @param[out] lcd  Puntero a la estructura de instancia a inicializar.
 * @param[in]  rs   GPIO para Register Select.
 * @param[in]  e    GPIO para Enable.
 * @param[in]  d4-d7 GPIOs para los 4 bits de dato.
 */
void lcd_init(lcd_t *lcd,
              uint8_t rs, uint8_t e,
              uint8_t d4, uint8_t d5, uint8_t d6, uint8_t d7);

/**
 * @brief  Envía un byte de comando al LCD (RS = 0).
 */
void lcd_command(lcd_t *lcd, uint8_t cmd);

/**
 * @brief  Envía un carácter visible al LCD (RS = 1).
 */
void lcd_char(lcd_t *lcd, char c);

/**
 * @brief  Imprime una cadena de caracteres a partir de la posición actual del cursor.
 */
void lcd_print(lcd_t *lcd, const char *str);

/**
 * @brief  Posiciona el cursor en la fila y columna especificadas.
 * @param[in] row  Fila: 0 = primera línea, 1 = segunda línea.
 * @param[in] col  Columna: 0..15.
 */
void lcd_set_cursor(lcd_t *lcd, uint8_t row, uint8_t col);

/**
 * @brief  Borra la pantalla y retorna el cursor al origen.
 */
void lcd_clear(lcd_t *lcd);

#endif /* LCD_16X2_H_H */
