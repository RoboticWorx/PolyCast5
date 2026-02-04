#ifndef POLYCAST5_GPIOS_H
#define POLYCAST5_GPIOS_H


/* SPI */
// Shared SPI pins
#define SPI_MISO_PIN 27 // MISO for SX126x
#define SPI_MOSI_PIN 11 // SPI2 MOSI
#define SPI_SCLK_PIN 25 // SPI2 SCLK

// SX1262 SPI pins
#define SX126X_BUSY_PIN 23 // Busy pin
#define SX126X_DIO1_PIN 24 // DIO1 pin for IRQ
#define SX126X_CS_PIN 4 // CS for SX126x

// ST7789 LCD SPI pins
#define ST7789_CS_PIN 26 // CS
#define ST7789_DC_PIN 12 // D/C
#define ST7789_LEDA_PIN 2 // Backlight


/* TCA9535 GPIO expander */
// Interrupt pin for TCA9535
#define TCA9535_INT_PIN 1

// TCA9535 input port pins
#define TCA9535_USER_BUTTON_POWER_PIN 0
#define TCA9535_USER_BUTTON_UP_PIN 1
#define TCA9535_USER_BUTTON_RIGHT_PIN 2
#define TCA9535_USER_BUTTON_SELECT_PIN 3
#define TCA9535_USER_BUTTON_LEFT_PIN 4
#define TCA9535_USER_BUTTON_HOME_PIN 5
#define TCA9535_USER_BUTTON_DOWN_PIN 6
#define TCA9535_CHG_IND_PIN 7

// TCA9535 output port pins
#define TCA9535_HAPTIC_PIN 0
#define TCA9535_RED_RGB_LED_PIN 1
#define TCA9535_GREEN_RGB_LED_PIN 3
#define TCA9535_TSOP_EN_PIN 4
#define TCA9535_BLUE_RGB_LED_PIN 5
#define TCA9535_SX126X_NRST_PIN 6
#define TCA9535_LCD_NRST_PIN 7


/* Infrared */
// RMT infrared pins for IR transmit and receive
#define RMT_RX_GPIO_PIN 6
#define RMT_TX_GPIO_PIN 7


/* I2C */
// I2C pins for TCA9535 GPIO expander, scanner, etc.
#define I2C_MASTER_SCL_PIN 0
#define I2C_MASTER_SDA_PIN 3


/* ADC */
// ADC pin for battery voltage reading
#define ADC_PIN 5


/* I2S T5848 */
#define I2S_T5848_SCK_PIN 8
#define I2S_T5848_SD_PIN 9
#define I2S_T5848_WS_PIN 10


#endif // POLYCAST5_GPIOS_H