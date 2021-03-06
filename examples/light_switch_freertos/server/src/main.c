/* Copyright (c) 2010 - 2018, Nordic Semiconductor ASA
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without modification,
 * are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice, this
 * list of conditions and the following disclaimer.
 *
 * 2. Redistributions in binary form, except as embedded into a Nordic
 *    Semiconductor ASA integrated circuit in a product or a software update for
 *    such product, must reproduce the above copyright notice, this list of
 *    conditions and the following disclaimer in the documentation and/or other
 *    materials provided with the distribution.
 *
 * 3. Neither the name of Nordic Semiconductor ASA nor the names of its
 *    contributors may be used to endorse or promote products derived from this
 *    software without specific prior written permission.
 *
 * 4. This software, with or without modification, must only be used with a
 *    Nordic Semiconductor ASA integrated circuit.
 *
 * 5. Any software provided in binary form under this license must not be reverse
 *    engineered, decompiled, modified and/or disassembled.
*
 * THIS SOFTWARE IS PROVIDED BY NORDIC SEMICONDUCTOR ASA "AS IS" AND ANY EXPRESS
 * OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY, NONINFRINGEMENT, AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL NORDIC SEMICONDUCTOR ASA OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE
 * GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT
 * OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <stdint.h>
#include <string.h>

/* HAL */
#include "boards.h"
#include "simple_hal.h"
#include "app_timer.h"

/* FreeRTOS and dependencies */
#include "FreeRTOS.h"
#include "task.h"
#include "nrf_sdh_freertos.h"
#include "nrf_drv_clock.h"

/* Core */
#include "nrf_mesh_config_core.h"
#include "nrf_mesh_gatt.h"
#include "nrf_mesh_configure.h"
#include "nrf_mesh.h"
#include "mesh_stack.h"
#include "device_state_manager.h"
#include "access_config.h"
#include "proxy.h"
#include "bearer_event.h"

/* Provisioning and configuration */
#include "mesh_provisionee.h"
#include "mesh_app_utils.h"

/* Models */
#include "generic_onoff_server.h"

/* Logging and RTT */
#include "log.h"
#include "rtt_input.h"

/* Example specific includes */
#include "nrf_sdh_soc.h"
#include "app_config.h"
#include "example_common.h"
#include "nrf_mesh_config_examples.h"
#include "light_switch_example_common.h"
#include "app_onoff.h"
#include "ble_softdevice_support.h"


#define ONOFF_SERVER_0_LED                  (BSP_LED_0)
#define APP_ONOFF_ELEMENT_INDEX             (0)


static bool m_device_provisioned;

/* The handle for the Mesh processing task */
static TaskHandle_t m_mesh_process_task;
/* The handle for the button handler thread */
static TaskHandle_t m_button_handler_thread;

/*************************************************************************************************/

static void clock_init(void)
{
    ret_code_t err_code = nrf_drv_clock_init();
    ERROR_CHECK(err_code);
}

/*************************************************************************************************/
static void app_onoff_server_set_cb(const app_onoff_server_t * p_server, bool onoff);
static void app_onoff_server_get_cb(const app_onoff_server_t * p_server, bool * p_present_onoff);

/* Generic OnOff server structure definition and initialization */
APP_ONOFF_SERVER_DEF(m_onoff_server_0,
                     APP_CONFIG_FORCE_SEGMENTATION,
                     APP_CONFIG_MIC_SIZE,
                     app_onoff_server_set_cb,
                     app_onoff_server_get_cb)

/* Callback for updating the hardware state */
static void app_onoff_server_set_cb(const app_onoff_server_t * p_server, bool onoff)
{
    /* Resolve the server instance here if required, this example uses only 1 instance. */

    __LOG(LOG_SRC_APP, LOG_LEVEL_INFO, "Setting GPIO value: %d\n", onoff)

    hal_led_pin_set(ONOFF_SERVER_0_LED, onoff);
}

/* Callback for reading the hardware state */
static void app_onoff_server_get_cb(const app_onoff_server_t * p_server, bool * p_present_onoff)
{
    /* Resolve the server instance here if required, this example uses only 1 instance. */

    *p_present_onoff = hal_led_pin_get(ONOFF_SERVER_0_LED);
}

static void app_model_init(void)
{
    /* Instantiate onoff server on element index APP_ONOFF_ELEMENT_INDEX */
    ERROR_CHECK(app_onoff_init(&m_onoff_server_0, APP_ONOFF_ELEMENT_INDEX));
    __LOG(LOG_SRC_APP, LOG_LEVEL_INFO, "App OnOff Model Handle: %d\n", m_onoff_server_0.server.model_handle);
}

/*************************************************************************************************/

static void node_reset(void)
{
    __LOG(LOG_SRC_APP, LOG_LEVEL_INFO, "----- Node reset  -----\n");
    hal_led_blink_ms(LEDS_MASK, LED_BLINK_INTERVAL_MS, LED_BLINK_CNT_RESET);
    /* This function may return if there are ongoing flash operations. */
    mesh_stack_device_reset();
}

static void config_server_evt_cb(const config_server_evt_t * p_evt)
{
    if (p_evt->type == CONFIG_SERVER_EVT_NODE_RESET)
    {
        node_reset();
    }
}

static void button_event_handler(uint32_t button_number)
{
    __LOG(LOG_SRC_APP, LOG_LEVEL_INFO, "Button %u pressed\n", button_number);
    switch (button_number)
    {
        /* Pressing SW1 on the Development Kit will result in LED state to toggle and trigger
        the STATUS message to inform client about the state change. This is a demonstration of
        state change publication due to local event. */
        case 0:
        {
            __LOG(LOG_SRC_APP, LOG_LEVEL_INFO, "User action \n");
            hal_led_pin_set(ONOFF_SERVER_0_LED, !hal_led_pin_get(ONOFF_SERVER_0_LED));
            app_onoff_status_publish(&m_onoff_server_0);
            break;
        }

        /* Initiate node reset */
        case 3:
        {
            /* Clear all the states to reset the node. */
            if (mesh_stack_is_device_provisioned())
            {
#if MESH_FEATURE_GATT_PROXY_ENABLED
                (void) proxy_stop();
#endif
                mesh_stack_config_clear();
                node_reset();
            }
            else
            {
                __LOG(LOG_SRC_APP, LOG_LEVEL_INFO, "The device is unprovisioned. Resetting has no effect.\n");
            }
            break;
        }

        default:
            break;
    }
}

static void device_identification_start_cb(uint8_t attention_duration_s)
{
    hal_led_mask_set(LEDS_MASK, false);
    hal_led_blink_ms(BSP_LED_2_MASK  | BSP_LED_3_MASK,
                     LED_BLINK_ATTENTION_INTERVAL_MS,
                     LED_BLINK_ATTENTION_COUNT(attention_duration_s));
}

static void provisioning_aborted_cb(void)
{
    hal_led_blink_stop();
}

static void provisioning_complete_cb(void)
{
    __LOG(LOG_SRC_APP, LOG_LEVEL_INFO, "Successfully provisioned\n");

#if MESH_FEATURE_GATT_ENABLED
    /* Restores the application parameters after switching from the Provisioning
     * service to the Proxy  */
    gap_params_init();
    conn_params_init();
#endif

    dsm_local_unicast_address_t node_address;
    dsm_local_unicast_addresses_get(&node_address);
    __LOG(LOG_SRC_APP, LOG_LEVEL_INFO, "Node Address: 0x%04x \n", node_address.address_start);

    hal_led_blink_stop();
    hal_led_mask_set(LEDS_MASK, LED_MASK_STATE_OFF);
    hal_led_blink_ms(LEDS_MASK, LED_BLINK_INTERVAL_MS, LED_BLINK_CNT_PROV);
}

static void models_init_cb(void)
{
    __LOG(LOG_SRC_APP, LOG_LEVEL_INFO, "Initializing and adding models\n");
    app_model_init();
}

static void mesh_init(void)
{
    mesh_stack_init_params_t init_params =
    {
        .core.irq_priority       = NRF_MESH_IRQ_PRIORITY_THREAD,
        .core.lfclksrc           = DEV_BOARD_LF_CLK_CFG,
        .core.p_uuid             = NULL,
        .models.models_init_cb   = models_init_cb,
        .models.config_server_cb = config_server_evt_cb
    };
    ERROR_CHECK(mesh_stack_init(&init_params, &m_device_provisioned));
}

/*************************************************************************************************/

static void button_thread_notify(uint32_t button)
{
    BaseType_t result = pdFAIL;
    BaseType_t switch_required = pdFALSE;

    result = xTaskNotifyFromISR(m_button_handler_thread, button, eSetValueWithoutOverwrite, &switch_required);

    if (result == pdPASS)
    {
        portYIELD_FROM_ISR(switch_required);
    }
}

static void button_handler_thread(void * p_arg)
{
    UNUSED_VARIABLE(p_arg);

    uint32_t notification = 0;

    for (;;)
    {
        if (xTaskNotifyWait(0, 0, &notification, portMAX_DELAY) == pdTRUE)
        {
            button_event_handler(notification);
        }
    }
}

static void app_rtt_input_handler(int key)
{
    if (key >= '0' && key <= '4')
    {
        uint32_t button_number = key - '0';
        button_thread_notify(button_number);
    }
}

#if !MESH_FREERTOS_IDLE_HANDLER_RESUME
static bool current_priority_is_rtos_safe(void)
{
    volatile IRQn_Type active_irq = hal_irq_active_get();
    if (active_irq != HAL_IRQn_NONE)
    {
        volatile uint32_t prio = NVIC_GetPriority(active_irq);
        return prio >= configMAX_SYSCALL_INTERRUPT_PRIORITY;
    }
    else
    {
        return true;
    }
}

/* bearer_event callback for resuming the Mesh processing task when events are received */
static void mesh_freertos_trigger_event_callback(void)
{
    BaseType_t yield = pdFALSE;

    APP_ERROR_CHECK_BOOL(current_priority_is_rtos_safe());

    vTaskNotifyGiveFromISR(m_mesh_process_task, &yield);

    /* Switch the task if required. */
    portYIELD_FROM_ISR(yield);
}
#endif

void vApplicationIdleHook( void )
{
#if MESH_FREERTOS_IDLE_HANDLER_RESUME
    (void) xTaskNotifyGive(m_mesh_process_task);
#endif
}

static void mesh_process_thread(void * arg)
{
    UNUSED_PARAMETER(arg);

    bool done = false;

    for (;;)
    {
        done = nrf_mesh_process();
        if (done)
        {
            (void) ulTaskNotifyTake(pdTRUE,         /* Clear the notification value before exiting (equivalent to the binary semaphore). */
                                    portMAX_DELAY); /* Block indefinitely (INCLUDE_vTaskSuspend has to be enabled).*/
        }
    }
}

static uint32_t nrf_mesh_process_thread_init(void)
{
    BaseType_t result;

    result = xTaskCreate(mesh_process_thread,
                         "MESH",
                         MESH_FREERTOS_TASK_STACK_DEPTH,
                         NULL,
                         MESH_FREERTOS_TASK_PRIO,
                         &m_mesh_process_task);

    if (result != pdPASS)
    {
        return NRF_ERROR_NO_MEM;
    }

#if !MESH_FREERTOS_IDLE_HANDLER_RESUME
    bearer_event_trigger_event_callback_set(APP_IRQ_PRIORITY_LOW, mesh_freertos_trigger_event_callback);
#endif

    return NRF_SUCCESS;
}

/*************************************************************************************************/

// Forward declaration
static void start(void *);

static void initialize(void)
{
    __LOG_INIT(LOG_SRC_APP | LOG_SRC_ACCESS | LOG_SRC_BEARER, LOG_LEVEL_INFO, LOG_CALLBACK_DEFAULT);
    __LOG(LOG_SRC_APP, LOG_LEVEL_INFO, "----- BLE Mesh Light Switch Server Demo -----\n");

    ERROR_CHECK(app_timer_init());
    hal_leds_init();
    clock_init();

#if BUTTON_BOARD
    ERROR_CHECK(hal_buttons_init(button_thread_notify));
#endif

    ble_stack_init();

#if MESH_FEATURE_GATT_ENABLED
    gap_params_init();
    conn_params_init();
#endif

    mesh_init();

    // Set up the Mesh processing task for FreeRTOS
    ERROR_CHECK(nrf_mesh_process_thread_init());

    // Set up a task for processing button/RTT events
    if (pdPASS != xTaskCreate(button_handler_thread, "BTN", 512, NULL, 1, &m_button_handler_thread))
    {
        APP_ERROR_HANDLER(NRF_ERROR_NO_MEM);
    }

    // Create a FreeRTOS task for handling SoftDevice events.
    // That task will call the start function when the task is started.
    nrf_sdh_freertos_init(start, &m_device_provisioned);
}

static void start(void * p_device_provisioned)
{
    bool device_provisioned = *(bool *)p_device_provisioned;

    rtt_input_enable(app_rtt_input_handler, RTT_INPUT_POLL_PERIOD_MS);
    
    if (!device_provisioned)
    {
        static const uint8_t static_auth_data[NRF_MESH_KEY_SIZE] = STATIC_AUTH_DATA;
        mesh_provisionee_start_params_t prov_start_params =
        {
            .p_static_data    = static_auth_data,
            .prov_complete_cb = provisioning_complete_cb,
            .prov_device_identification_start_cb = device_identification_start_cb,
            .prov_device_identification_stop_cb = NULL,
            .prov_abort_cb = provisioning_aborted_cb,
            .p_device_uri = EX_URI_LS_SERVER
        };
        ERROR_CHECK(mesh_provisionee_prov_start(&prov_start_params));
    }

    mesh_app_uuid_print(nrf_mesh_configure_device_uuid_get());

    ERROR_CHECK(mesh_stack_start());

    hal_led_mask_set(LEDS_MASK, LED_MASK_STATE_OFF);
    hal_led_blink_ms(LEDS_MASK, LED_BLINK_INTERVAL_MS, LED_BLINK_CNT_START);
}

int main(void)
{
    initialize();

    // Start the FreeRTOS scheduler.
    vTaskStartScheduler();

    for (;;)
    {
        // The app should stay in the FreeRTOS scheduler loop and should never reach this point.
        APP_ERROR_HANDLER(NRF_ERROR_FORBIDDEN);
    }
}
