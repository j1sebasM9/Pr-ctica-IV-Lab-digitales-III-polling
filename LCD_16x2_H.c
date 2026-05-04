/**
 * @file    LCD_16x2_H.c
 * @brief   Implementación del driver LCD 16×2 HD44780 en modo 4 bits.
 *
 * @note     El driver es bloqueante
 *          por naturaleza del protocolo HD44780, pero en la arquitectura multicore
 *          esas demoras solo afectan al Core 0 (UI), mientras Core 1 (DAC) es
 *          completamente independiente. El jitter en la señal queda eliminado.
 *
 * @author  Firmware Engineering Lab
 * @version 1.0.0
 * @date    2025
 */

#include "LCD_16x2_H.h"
#include "hardware/timer.h"
#include "pico/time.h"

/* ─── Lógica de envío ─────────────────────────────────────────────────────── */

static void lcd_send_nibble(lcd_t *lcd, uint8_t nibble)
{
    gpio_put(lcd->d4_pin, (nibble >> 0) & 0x01);
    gpio_put(lcd->d5_pin, (nibble >> 1) & 0x01);
    gpio_put(lcd->d6_pin, (nibble >> 2) & 0x01);
    gpio_put(lcd->d7_pin, (nibble >> 3) & 0x01);

    /* Pulso de Enable */
    gpio_put(lcd->e_pin, 1);
    sleep_us(1);    /* Tiempo de hold de datos (datasheet HD44780: tPW ≥ 230 ns) */
    gpio_put(lcd->e_pin, 0);
    sleep_us(100);  /* Tiempo de ciclo mínimo entre pulsos Enable */
}

static void lcd_send_byte(lcd_t *lcd, uint8_t val, bool is_data)
{
    gpio_put(lcd->rs_pin, is_data);
    lcd_send_nibble(lcd, val >> 4);     /* Nibble alto: bits 7-4 */
    lcd_send_nibble(lcd, val & 0x0F);   /* Nibble bajo: bits 3-0 */
}

/* ─── Inicialización ─────────────────────────────────────────────────────── */

void lcd_init(lcd_t *lcd, uint8_t rs, uint8_t e,
              uint8_t d4, uint8_t d5, uint8_t d6, uint8_t d7)
{
    /* Guardar pines y configurar como salidas */
    lcd->rs_pin = rs;
    lcd->e_pin  = e;
    lcd->d4_pin = d4;
    lcd->d5_pin = d5;
    lcd->d6_pin = d6;
    lcd->d7_pin = d7;

    uint8_t pins[] = {rs, e, d4, d5, d6, d7};
    for (int i = 0; i < 6; i++) {
        gpio_init(pins[i]);
        gpio_set_dir(pins[i], GPIO_OUT);
        gpio_put(pins[i], 0);
    }

    /* Secuencia de inicialización del datasheet (espera > 15 ms tras Vcc) */
    sleep_ms(50);
    gpio_put(lcd->rs_pin, 0);

    /* Secuencia de sincronización a modo 4 bits */
    lcd_send_nibble(lcd, 0x03); sleep_ms(5);
    lcd_send_nibble(lcd, 0x03); sleep_us(150);
    lcd_send_nibble(lcd, 0x03); sleep_us(150);
    lcd_send_nibble(lcd, 0x02); /* Establecer modo 4 bits */

    /* Configuración de pantalla: 2 líneas, fuente 5×8, display ON, cursor OFF */
    lcd_command(lcd, 0x28);     /* Function Set: 4-bit, 2 líneas, 5×8 dots */
    lcd_command(lcd, 0x0C);     /* Display ON, Cursor OFF, Blink OFF */
    lcd_command(lcd, 0x06);     /* Entry Mode: incrementar, sin desplazamiento */
    lcd_clear(lcd);
}

/* ─── API pública ────────────────────────────────────────────────────────── */

void lcd_command(lcd_t *lcd, uint8_t cmd)
{
    lcd_send_byte(lcd, cmd, false);
}

void lcd_char(lcd_t *lcd, char c)
{
    lcd_send_byte(lcd, (uint8_t)c, true);
}

void lcd_print(lcd_t *lcd, const char *str)
{
    while (*str) {
        lcd_char(lcd, *str++);
    }
}

void lcd_set_cursor(lcd_t *lcd, uint8_t row, uint8_t col)
{
    uint8_t addr = (row == 0) ? (0x80 + col) : (0xC0 + col);
    lcd_command(lcd, addr);
}

void lcd_clear(lcd_t *lcd)
{
    lcd_command(lcd, 0x01);
    sleep_ms(2);    /* El comando Clear Display requiere hasta 1.52 ms (datasheet) */
}
