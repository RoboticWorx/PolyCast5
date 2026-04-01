#include "TCA9535.h"
#include "esp_err.h"
#include "esp_log.h"
#include "string.h"

#include "polycast5_gpios.h" // I2C pins

// Bus handle shared with lcd_gpio.c (scanner, terminal)
i2c_master_bus_handle_t i2c_bus_handle = NULL;

// Device handle for the on-board TCA9535
static i2c_master_dev_handle_t tca9535_dev_handle = NULL;

// ****************************************************************************
//! @brief        Initializes the I2C interface
//! @param        None
//! @return
//!             - ESP_OK if erase operation was successful
//!               - i2c driver error
// ****************************************************************************
esp_err_t TCA9535Init(void)
{
    esp_err_t ret;

    // Create I2C master bus
    i2c_master_bus_config_t bus_config = {
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .i2c_port = I2C_MASTER_NUM,
        .scl_io_num = I2C_MASTER_SCL_PIN,
        .sda_io_num = I2C_MASTER_SDA_PIN,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    ret = i2c_new_master_bus(&bus_config, &i2c_bus_handle);
    if (ret != ESP_OK) {
        return ret;
    }

    // Add TCA9535 device to the bus
    i2c_device_config_t dev_config = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = TCA9535_ADDRESS,
        .scl_speed_hz = I2C_MASTER_FREQ_HZ,
    };
    ret = i2c_master_bus_add_device(i2c_bus_handle, &dev_config, &tca9535_dev_handle);

    return ret;
}

// ****************************************************************************
//! @brief        Reads single byte from specified register
//! @param        Register address
//! @return        Byte from register
// ****************************************************************************
esp_err_t TCA9535ReadSingleRegister(tca9535_reg_t address, uint8_t *out)
{
    uint8_t reg_addr = (uint8_t)address;
    *out = 0;

    return i2c_master_transmit_receive(tca9535_dev_handle, &reg_addr, 1, out, 1, 100);
}

// ****************************************************************************
//! @brief        Writes single byte to specified register
//! @param        Register address
//! @return
//!             - ESP_OK if erase operation was successful
//!               - i2c driver error
// ****************************************************************************
esp_err_t TCA9535WriteSingleRegister(tca9535_reg_t address, unsigned short regVal)
{
    uint8_t write_buf[2] = { (uint8_t)address, (uint8_t)regVal };

    return i2c_master_transmit(tca9535_dev_handle, write_buf, sizeof(write_buf), 100);
}

// ****************************************************************************
//! @brief        Reads whole register and puts result into struct
//! @param        reg: struct pointer
//!                reg_num: register type
//! @return
//!             - ESP_OK if erase operation was successful
//!               - i2c driver error
// ****************************************************************************
esp_err_t TCA9535ReadStruct(TCA9535_Register *reg, tca9535_reg_t reg_num)
{
    uint8_t reg_addr = (uint8_t)reg_num;

    return i2c_master_transmit_receive(tca9535_dev_handle, &reg_addr, 1, (uint8_t *)&reg->asInt, 2, 100);
}

// ****************************************************************************
//! @brief        Writes whole register with data from struct
//! @param        reg: struct pointer
//!                reg_num: register type
//! @return
//!             - ESP_OK if erase operation was successful
//!               - i2c driver error
// ****************************************************************************
esp_err_t TCA9535WriteStruct(TCA9535_Register *reg, tca9535_reg_t reg_num)
{
    uint8_t write_buf[3];
    write_buf[0] = (uint8_t)reg_num;
    memcpy(&write_buf[1], reg, 2);

    return i2c_master_transmit(tca9535_dev_handle, write_buf, sizeof(write_buf), 100);
}

esp_err_t TCA9535WriteOutput(TCA9535_Register *reg)
{
    return TCA9535WriteStruct(reg, TCA9535_OUTPUT_REG0);
}

esp_err_t TCA9535WritePolarity(TCA9535_Register *reg)
{
    return TCA9535WriteStruct(reg, TCA9535_POLARITY_REG0);
}

esp_err_t TCA9535WriteConfig(TCA9535_Register *reg)
{
    return TCA9535WriteStruct(reg, TCA9535_CONFIG_REG0);
}

esp_err_t TCA9535ReadInput(TCA9535_Register *reg)
{
    return TCA9535ReadStruct(reg, TCA9535_INPUT_REG0);
}

esp_err_t TCA9535ReadOutput(TCA9535_Register *reg)
{
    return TCA9535ReadStruct(reg, TCA9535_OUTPUT_REG0);
}

esp_err_t TCA9535ReadPolarity(TCA9535_Register *reg)
{
    return TCA9535ReadStruct(reg, TCA9535_POLARITY_REG0);
}

esp_err_t TCA9535ReadConfig(TCA9535_Register *reg)
{
    return TCA9535ReadStruct(reg, TCA9535_CONFIG_REG0);
}
