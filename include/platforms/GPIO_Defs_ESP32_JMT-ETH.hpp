#pragma once

#define SUPPORT_ETHERNET
#include <ETH.h>
//Output Manager
#define DEFAULT_RMT_0_GPIO      gpio_num_t::GPIO_NUM_2
#define DEFAULT_RMT_1_GPIO      gpio_num_t::GPIO_NUM_4
#define DEFAULT_RMT_2_GPIO      gpio_num_t::GPIO_NUM_5
#define DEFAULT_RMT_3_GPIO      gpio_num_t::GPIO_NUM_16

#define DEFAULT_RMT_4_GPIO      gpio_num_t::GPIO_NUM_0
#define DEFAULT_RMT_5_GPIO      gpio_num_t::GPIO_NUM_12
#define DEFAULT_RMT_6_GPIO      gpio_num_t::GPIO_NUM_3
#define DEFAULT_RMT_7_GPIO      gpio_num_t::GPIO_NUM_1

#ifndef ESP_SCL_PIN 
    #define ESP_SCL_PIN             gpio_num_t::GPIO_NUM_33
#endif
#ifndef ESP_SDA_PIN 
    #define ESP_SDA_PIN             gpio_num_t::GPIO_NUM_32
#endif
// File Manager
#define SUPPORT_SD
#define SD_CARD_MISO_PIN         gpio_num_t::GPIO_NUM_34  // test
#define SD_CARD_MOSI_PIN         gpio_num_t::GPIO_NUM_13
#define SD_CARD_CLK_PIN          gpio_num_t::GPIO_NUM_14
#define SD_CARD_CS_PIN           gpio_num_t::GPIO_NUM_15



#define DEFAULT_ETH_CLK_MODE           eth_clock_mode_t::ETH_CLOCK_GPIO17_OUT
#define DEFAULT_ETH_POWER_PIN          gpio_num_t::GPIO_NUM_20
#define DEFAULT_ETH_POWER_PIN_ACTIVE   HIGH
#define DEFAULT_ETH_TYPE               eth_phy_type_t::ETH_PHY_LAN8720
#define ETH_ADDR_PHY_LAN8720           1
#define DEFAULT_ETH_ADDR               ETH_ADDR_PHY_LAN8720
#define DEFAULT_ETH_TXEN               gpio_num_t::GPIO_NUM_21
#define DEFAULT_ETH_TXD0               gpio_num_t::GPIO_NUM_19
#define DEFAULT_ETH_TXD1               gpio_num_t::GPIO_NUM_22
#define DEFAULT_ETH_CRSDV              gpio_num_t::GPIO_NUM_27
#define DEFAULT_ETH_RXD0               gpio_num_t::GPIO_NUM_25
#define DEFAULT_ETH_RXD1               gpio_num_t::GPIO_NUM_26
#define DEFAULT_ETH_MDC_PIN            gpio_num_t::GPIO_NUM_23
#define DEFAULT_ETH_MDIO_PIN           gpio_num_t::GPIO_NUM_18


#define SUPPORT_OutputType_DMX            // UART
#define SUPPORT_OutputType_GECE           // UART
#define SUPPORT_OutputType_GS8208         // UART / RMT
#define SUPPORT_OutputType_Renard         // UART
#define SUPPORT_OutputType_Serial         // UART
#define SUPPORT_OutputType_TM1814         // UART / RMT
#define SUPPORT_OutputType_UCS1903        // UART / RMT
#define SUPPORT_OutputType_UCS8903        // UART / RMT
#define SUPPORT_OutputType_WS2811         // UART / RMT
#define SUPPORT_OutputType_RELAY         // UART / RMT
#define SUPPORT_OutputType_Servo_PCA9685