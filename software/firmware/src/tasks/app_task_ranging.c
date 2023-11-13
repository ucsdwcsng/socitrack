// Header Inclusions ---------------------------------------------------------------------------------------------------

#include "app_tasks.h"
#include "battery.h"
#include "bluetooth.h"
#include "buzzer.h"
#include "imu.h"
#include "led.h"
#include "logging.h"
#include "ranging.h"
#include "rtc.h"
#include "storage.h"
#include "system.h"


// Static Global Variables ---------------------------------------------------------------------------------------------

static uint8_t device_uid_short;
static TaskHandle_t app_task_handle = 0;
static uint8_t discovered_devices[MAX_NUM_RANGING_DEVICES][1+EUI_LEN];
static volatile uint32_t seconds_to_activate_buzzer;
static volatile uint8_t num_discovered_devices;
static volatile bool devices_found;


// Private Helper Functions --------------------------------------------------------------------------------------------

static void verify_app_configuration(void)
{
   // Retrieve the current state of the application
   print("INFO: Verifying TotTag application configuration\n");
   const bool is_scanning = bluetooth_is_scanning(), is_ranging = ranging_active();

   // Advertising should always be enabled
   if (!bluetooth_is_advertising())
      bluetooth_start_advertising();

   // Scanning should only be enabled if we are not already ranging with a network
   if (!is_ranging && !is_scanning)
      bluetooth_start_scanning();
   else if (is_ranging && is_scanning)
      bluetooth_stop_scanning();
}

static void handle_notification(app_notification_t notification)
{
   // Handle the notification based on which bits are set
   if (((notification & APP_NOTIFY_NETWORK_LOST) != 0) || ((notification & APP_NOTIFY_VERIFY_CONFIGURATION) != 0))
      verify_app_configuration();
   if ((notification & APP_NOTIFY_NETWORK_FOUND) != 0)
   {
      // Determine if an actively ranging device was located
      bool ranging_device_located = false, idle_device_located = false;
      for (uint8_t i = 0; !ranging_device_located && (i < num_discovered_devices); ++i)
         if ((discovered_devices[i][EUI_LEN] == ROLE_MASTER) || (discovered_devices[i][EUI_LEN] == ROLE_PARTICIPANT))
            ranging_device_located = true;
         else if (discovered_devices[i][EUI_LEN] == ROLE_IDLE)
            idle_device_located = true;

      // Start the ranging task based on the state of the detected devices
      if (ranging_device_located)
         ranging_begin(ROLE_PARTICIPANT);
      else if (idle_device_located)
      {
         // Search for the non-sleeping device with the highest ID that is higher than our own
         int32_t best_device_idx = -1;
         uint8_t highest_device_id = device_uid_short;
         for (uint8_t i = 0; i < num_discovered_devices; ++i)
            if ((discovered_devices[i][EUI_LEN] != ROLE_ASLEEP) && (discovered_devices[i][0] > highest_device_id))
            {
               best_device_idx = i;
               highest_device_id = discovered_devices[i][0];
            }

         // If a potential master candidate device was found, attempt to connect to it
         if (best_device_idx >= 0)
            ranging_begin(ROLE_PARTICIPANT);
         else
            ranging_begin(ROLE_MASTER);
      }

      // Reset the devices-found flag and verify the app configuration
      devices_found = false;
      verify_app_configuration();
   }
   if ((notification & APP_NOTIFY_BATTERY_EVENT) != 0)
      storage_flush_and_shutdown();
   if ((notification & APP_NOTIFY_FIND_MY_TOTTAG_ACTIVATED) != 0)
      for (uint32_t seconds = 0; seconds < seconds_to_activate_buzzer; ++seconds)
      {
         buzzer_indicate_location();
         vTaskDelay(pdMS_TO_TICKS(1000));
      }
}

static void battery_event_handler(battery_event_t battery_event)
{
   // Store the battery event to non-volatile memory and notify the app
   storage_write_charging_event(battery_event);
   if ((battery_event == BATTERY_PLUGGED) || (battery_event == BATTERY_UNPLUGGED))
      app_notify(APP_NOTIFY_BATTERY_EVENT, true);
}

static void motion_change_handler(bool in_motion)
{
   // Store the motion change to non-volatile memory
   storage_write_motion_status(in_motion);
}

static void ble_discovery_handler(const uint8_t ble_address[EUI_LEN], uint8_t ranging_role)
{
   // Keep track of all newly discovered devices
   if (!devices_found)
   {
      devices_found = true;
      num_discovered_devices = 1;
      memcpy(discovered_devices[0], ble_address, EUI_LEN);
      discovered_devices[0][EUI_LEN] = ranging_role;
      am_hal_timer_clear(BLE_SCANNING_TIMER_NUMBER);
   }
   else if (num_discovered_devices < MAX_NUM_RANGING_DEVICES)
   {
      for (uint8_t i = 0; i < num_discovered_devices; ++i)
         if (memcmp(discovered_devices[i], ble_address, EUI_LEN) == 0)
            return;
      memcpy(discovered_devices[num_discovered_devices], ble_address, EUI_LEN);
      discovered_devices[num_discovered_devices++][EUI_LEN] = ranging_role;
   }
}

void am_timer04_isr(void)
{
   // Immediately stop scanning for additional devices
   bluetooth_stop_scanning();

   // Notify the main task to handle the interrupt
   BaseType_t xHigherPriorityTaskWoken = pdFALSE;
   am_hal_timer_interrupt_clear(AM_HAL_TIMER_MASK(BLE_SCANNING_TIMER_NUMBER, AM_HAL_TIMER_COMPARE_BOTH));
   xTaskNotifyFromISR(app_task_handle, APP_NOTIFY_NETWORK_FOUND, eSetBits, &xHigherPriorityTaskWoken);
   portYIELD_FROM_ISR(xHigherPriorityTaskWoken);
}


// Public API Functions ------------------------------------------------------------------------------------------------

extern void app_maintenance_activate_find_my_tottag(uint32_t seconds_to_activate);

void app_notify(app_notification_t notification, bool from_isr)
{
   // Call the correct notification function based on the current ISR context
   if (app_task_handle)
   {
      if (from_isr)
      {
         BaseType_t xHigherPriorityTaskWoken = pdFALSE;
         xTaskNotifyFromISR(app_task_handle, notification, eSetBits, &xHigherPriorityTaskWoken);
         portYIELD_FROM_ISR(xHigherPriorityTaskWoken);
      }
      else
         xTaskNotify(app_task_handle, notification, eSetBits);
   }
}

void app_activate_find_my_tottag(uint32_t seconds_to_activate)
{
   // Notify application of the request to active FindMyTottag
   if (app_task_handle)
   {
      seconds_to_activate_buzzer = seconds_to_activate;
      app_notify(APP_NOTIFY_FIND_MY_TOTTAG_ACTIVATED, false);
   }
   else
      app_maintenance_activate_find_my_tottag(seconds_to_activate);
}

void AppTaskRanging(void *uid)
{
   // Store the UID and application task handle
   device_uid_short = ((uint8_t*)uid)[0];
   app_task_handle = xTaskGetCurrentTaskHandle();
   uint32_t notification_bits = 0;

   // Initialize the BLE scanning window timer
   am_hal_timer_config_t scanning_timer_config;
   am_hal_timer_default_config_set(&scanning_timer_config);
   scanning_timer_config.ui32Compare0 = (uint32_t)(BLE_SCANNING_TIMER_TICK_RATE_HZ / 4);
   am_hal_timer_config(BLE_SCANNING_TIMER_NUMBER, &scanning_timer_config);
   am_hal_timer_interrupt_enable(AM_HAL_TIMER_MASK(BLE_SCANNING_TIMER_NUMBER, AM_HAL_TIMER_COMPARE0));
   NVIC_SetPriority(TIMER0_IRQn + BLE_SCANNING_TIMER_NUMBER, NVIC_configMAX_SYSCALL_INTERRUPT_PRIORITY + 1);
   NVIC_EnableIRQ(TIMER0_IRQn + BLE_SCANNING_TIMER_NUMBER);

   // Register handlers for motion detection, battery status changes, and BLE events
   bluetooth_register_discovery_callback(ble_discovery_handler);
#ifndef _TEST_BLE_RANGING_TASK
   battery_register_event_callback(battery_event_handler);
   imu_register_motion_change_callback(motion_change_handler);
   if (battery_monitor_is_plugged_in())
      storage_flush_and_shutdown();
#endif

   // Retrieve current experiment details from non-volatile storage
   experiment_details_t current_experiment;
   storage_retrieve_experiment_details(&current_experiment);

   // Wait until the BLE stack has been fully initialized
   devices_found = false;
   while (!bluetooth_is_initialized())
      vTaskDelay(1);

   // Update the BLE address whitelist
   bluetooth_clear_whitelist();
   for (uint8_t i = 0; i < current_experiment.num_devices; ++i)
      bluetooth_add_device_to_whitelist(current_experiment.uids[i]);
   bluetooth_set_current_ranging_role(ROLE_IDLE);
   verify_app_configuration();

   // Loop forever, sleeping until an application notification is received
   while (true)
      if (xTaskNotifyWait(pdFALSE, 0xffffffff, &notification_bits, portMAX_DELAY) == pdTRUE)
         handle_notification(notification_bits);
}
