/***********************************************************************************************************************
* DISCLAIMER
* This software is supplied by Renesas Electronics Corporation and is only intended for use with Renesas products.
* No other uses are authorized. This software is owned by Renesas Electronics Corporation and is protected under all
* applicable laws, including copyright laws.
* THIS SOFTWARE IS PROVIDED "AS IS" AND RENESAS MAKES NO WARRANTIES REGARDING THIS SOFTWARE, WHETHER EXPRESS, IMPLIED
* OR STATUTORY, INCLUDING BUT NOT LIMITED TO WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
* NON-INFRINGEMENT.  ALL SUCH WARRANTIES ARE EXPRESSLY DISCLAIMED.TO THE MAXIMUM EXTENT PERMITTED NOT PROHIBITED BY
* LAW, NEITHER RENESAS ELECTRONICS CORPORATION NOR ANY OF ITS AFFILIATED COMPANIES SHALL BE LIABLE FOR ANY DIRECT,
* INDIRECT, SPECIAL, INCIDENTAL OR CONSEQUENTIAL DAMAGES FOR ANY REASON RELATED TO THIS SOFTWARE, EVEN IF RENESAS OR
* ITS AFFILIATES HAVE BEEN ADVISED OF THE POSSIBILITY OF SUCH DAMAGES.
* Renesas reserves the right, without notice, to make changes to this software and to discontinue the availability
* of this software. By using this software, you agree to the additional terms and conditions found by accessing the
* following link:
* http://www.renesas.com/disclaimer
*
* Copyright (C) 2015 Renesas Electronics Corporation. All rights reserved.
***********************************************************************************************************************/


#include "hal_data.h"
#include "common_data.h"
#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"
#include "semphr.h"
#include "serial.h"
#include "r_uart_api.h"
#include "r_sci_uart.h"


/***********************************************************************************************************************
* Global variables and local functions
***********************************************************************************************************************/

/* Characters received from the UART are stored in this queue, ready to be
 * received by the application.  ***NOTE*** Using a queue in this way is very
 * convenient, but also very inefficient.  It can be used here because characters
 * will only arrive slowly.  In a higher bandwidth system a circular RAM buffer or
 * DMA should be used in place of this queue.
 */
static QueueHandle_t xConsoleRxQueue = NULL;

/* We also use mutex to provide UART access to a single task at a time
 * if multiple tasks are used to access this API.
 */
extern SemaphoreHandle_t g_xConsoleMutex;

 /* Another semaphore is used to block task calling vSerialPutString() until
  * the character is put into UART buffer. This is keep puts() semantics
  * which assumes buffer may be overwritten/released when the call returns
  */
extern SemaphoreHandle_t g_xConsoleTxCompleteSem;

static TaskHandle_t xConsoleOwner = NULL;
static uint32_t     xConsoleLockLevel = 0;

/* Temporary buffer used to satisfy RX API */

static uint8_t      rx_char;

/* Lock out other tasks from accessing console */
static void console_acquire(void);

/* Unlock console access. */
static void console_release(void);

/* Wait for TX transfer to complete */
static void transfer_wait(void);

/* Error handling function */
static void handle_error(fsp_err_t err);

/* Interrupt callback function */
void vConsoleUartCallback(uart_callback_args_t *p_args);


/***********************************************************************************************************************
* API functions
***********************************************************************************************************************/

/* Function required in order to link UARTCommandConsole.c - which is used by
multiple different demo application. */
xComPortHandle xSerialPortInitMinimal( unsigned long ulWantedBaud, unsigned portBASE_TYPE uxQueueLength )
{
    ( void ) ulWantedBaud;  /* Currently relying on FSP e2 studio configurator to set the baudrate. */
    ( void ) uxQueueLength;

    /* Characters received from the UART are stored in this queue, ready to be
    received by the application.  ***NOTE*** Using a queue in this way is very
    convenient, but also very inefficient.  It can be used here because
    characters will only arrive slowly.  In a higher bandwidth system a circular
    RAM buffer or DMA should be used in place of this queue. */
    xConsoleRxQueue = xQueueCreate( uxQueueLength, sizeof( char ) );
    configASSERT( xConsoleRxQueue );

    R_SCI_UART_Open( &g_xConsoleUart_ctrl, &g_xConsoleUart_cfg);

    /* Set up SCI receive buffer (which is not used by application as the last received char
     * is passed directly to RX callback function.*/
    R_SCI_UART_Read( &g_xConsoleUart_cfg, NULL, 0);

    /* Only one UART is supported, so it doesn't matter what is returned
    here. */
    return 0;
}


/* Function required in order to link UARTCommandConsole.c - which is used by
multiple different demo application. */
void vSerialPutString( xComPortHandle pxPort, const signed char * const pcString, unsigned short usStringLength )
{
    fsp_err_t err;

    /* Only one port is supported. */
    ( void ) pxPort;

    if ( usStringLength > 0 )
    {

        console_acquire();

        err = R_SCI_UART_Write(&g_xConsoleUart_ctrl, (uint8_t *)pcString, usStringLength);
        handle_error(err);

        transfer_wait();

        console_release();
    }
}

/* Function required in order to link UARTCommandConsole.c - which is used by
multiple different demo application. */
signed portBASE_TYPE xSerialGetChar( xComPortHandle pxPort, signed char *pcRxedChar, TickType_t xBlockTime )
{
    BaseType_t status;
    /* Only one UART is supported. */
    ( void ) pxPort;

    /* Return a received character, if any are available.  Otherwise block to
    wait for a character. */
    status =  xQueueReceive( xConsoleRxQueue, pcRxedChar, xBlockTime );
    return status;
}

/* Function required in order to link UARTCommandConsole.c - which is used by
multiple different demo application. */
signed portBASE_TYPE xSerialPutChar( xComPortHandle pxPort, signed char cOutChar, TickType_t xBlockTime )
{
    /* Just mapped to vSerialPutString() so the block time is not used. */
    ( void ) xBlockTime;

    vSerialPutString( pxPort, &cOutChar, sizeof( cOutChar ) );
    return pdPASS;
}


/***********************************************************************************************************************
* Local functions
***********************************************************************************************************************/

static void console_acquire(void)
{
    if (xTaskGetCurrentTaskHandle() != xConsoleOwner)
    {
        while (xSemaphoreTake(g_xConsoleMutex, portMAX_DELAY) != pdTRUE)
        {
            /* wait here */
        }
        xConsoleOwner = xTaskGetCurrentTaskHandle();
    }
    ++xConsoleLockLevel;
}

/* Unlock console access. */
void console_release(void)
{
    configASSERT(xTaskGetCurrentTaskHandle() == xConsoleOwner);
    configASSERT(xConsoleLockLevel > 0);

    --xConsoleLockLevel;

    if (0 == xConsoleLockLevel)
    {
        xConsoleOwner = NULL;
        xSemaphoreGive(g_xConsoleMutex);
    }
}

/* Wait for TX transfer to complete */
void transfer_wait(void)
{
    while (xSemaphoreTake(g_xConsoleTxCompleteSem, portMAX_DELAY) != pdTRUE)
    {
        /* wait here */
    }
}

static void handle_error(fsp_err_t err)
{
    if(err != FSP_SUCCESS)
        __BKPT(0);
}

/* interrupt callback function */
void vConsoleUartCallback(uart_callback_args_t *p_args)
{
    static signed long xHigherPriorityTaskWoken = pdFALSE;

    switch (p_args->event)
    {
        case UART_EVENT_RX_CHAR:
            configASSERT( xConsoleRxQueue );

            xQueueSendFromISR( xConsoleRxQueue, &(p_args->data), &xHigherPriorityTaskWoken );

            /* Set up SCI1 receive buffer again */
            //R_SCI_UART_Read(&g_xConsoleUart_ctrl, NULL, 1);

            /* See http://www.freertos.org/xQueueOverwriteFromISR.html for information
            on the semantics of this ISR. */
            portYIELD_FROM_ISR( xHigherPriorityTaskWoken );

            break;
        case UART_EVENT_TX_COMPLETE:
            xSemaphoreGiveFromISR( g_xConsoleTxCompleteSem, &xHigherPriorityTaskWoken );
            portYIELD_FROM_ISR( xHigherPriorityTaskWoken );
            break;
        default:
            /* Just ignore errors and other events. */
            break;
    }
}



