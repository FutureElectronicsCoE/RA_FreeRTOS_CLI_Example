#include "cli_thread.h"

void vUARTCommandConsoleStart( uint16_t usStackSize, UBaseType_t uxPriority );

/* CLI Thread entry function */
/* pvParameters contains TaskHandle_t */
void cli_thread_entry(void *pvParameters)
{
    FSP_PARAMETER_NOT_USED (pvParameters);

    /* parameters are not used here */
    vUARTCommandConsoleStart( 0, 0 );
}
