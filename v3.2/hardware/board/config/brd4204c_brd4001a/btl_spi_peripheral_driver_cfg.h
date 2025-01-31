/***************************************************************************//**
 * @file
 * @brief Configuration header for bootloader Spi Peripheral Driver
 *******************************************************************************
 * # License
 * <b>Copyright 2021 Silicon Laboratories Inc. www.silabs.com</b>
 *******************************************************************************
 *
 * The licensor of this software is Silicon Laboratories Inc.  Your use of this
 * software is governed by the terms of Silicon Labs Master Software License
 * Agreement (MSLA) available at
 * www.silabs.com/about-us/legal/master-software-license-agreement.  This
 * software is distributed to you in Source Code format and is governed by the
 * sections of the MSLA applicable to Source Code.
 *
 ******************************************************************************/
#ifndef BTL_SPI_PERIPHERAL_DRIVER_CONFIG_H
#define BTL_SPI_PERIPHERAL_DRIVER_CONFIG_H

// <<< Use Configuration Wizard in Context Menu >>>

// <h>SPI Peripheral Driver

// <o SL_SPI_PERIPHERAL_RX_BUFFER_SIZE> Receive buffer size:[0-2048] <0-2048>
// <i> Default: 300
#define SL_SPI_PERIPHERAL_RX_BUFFER_SIZE    300

// <o SL_SPI_PERIPHERAL_TX_BUFFER_SIZE> Transmit buffer size:[0-2048] <0-2048>
// <i> Default: 50
#define SL_SPI_PERIPHERAL_TX_BUFFER_SIZE    50

// <o SL_SPI_PERIPHERAL_LDMA_RX_CHANNEL> LDMA channel for SPI RX:[0-1] <0-1>
// <i> Default: 0
#define SL_SPI_PERIPHERAL_LDMA_RX_CHANNEL    0

// <o SL_SPI_PERIPHERAL_LDMA_TX_CHANNEL> LDMA channel for SPI TX:[0-1] <0-1>
// <i> Default: 1
#define SL_SPI_PERIPHERAL_LDMA_TX_CHANNEL    1

// </h>

// <<< end of configuration section >>>

// <<< sl:start pin_tool >>>
// <usart signal=TX,RX,CS,CLK> SL_SPINCP

// $[USART_SL_SPINCP]
#define SL_SPINCP_PERIPHERAL                     USART0
#define SL_SPINCP_PERIPHERAL_NO                  0

// USART0 TX on PC01
#define SL_SPINCP_TX_PORT                        gpioPortC
#define SL_SPINCP_TX_PIN                         1

// USART0 RX on PC02
#define SL_SPINCP_RX_PORT                        gpioPortC
#define SL_SPINCP_RX_PIN                         2

// USART0 CS on PC00
#define SL_SPINCP_CS_PORT                        gpioPortC
#define SL_SPINCP_CS_PIN                         0

// USART0 CLK on PC03
#define SL_SPINCP_CLK_PORT                       gpioPortC
#define SL_SPINCP_CLK_PIN                        3

// [USART_SL_SPINCP]$

// <<< sl:end pin_tool >>>

#endif // BTL_SPI_PERIPHERAL_DRIVER_CONFIG_H