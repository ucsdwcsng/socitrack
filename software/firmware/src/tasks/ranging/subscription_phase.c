// Header Inclusions ---------------------------------------------------------------------------------------------------

#include "logging.h"
#include "ranging_phase.h"
#include "schedule_phase.h"
#include "subscription_phase.h"


// Static Global Variables ---------------------------------------------------------------------------------------------

static scheduler_phase_t current_phase;
static subscription_packet_t subscription_packet;
static uint8_t schedule_index, schedule_length;
static uint64_t reference_time;


// Public API Functions ------------------------------------------------------------------------------------------------

void subscription_phase_initialize(const uint8_t *uid)
{
   // Initialize all Subscription Phase parameters
   subscription_packet = (subscription_packet_t){ .header = { .frameCtrl = { 0x41, 0x88 }, .msgType = SUBSCRIPTION_PACKET,
         .panID = { MODULE_PANID & 0xFF, MODULE_PANID >> 8 }, .destAddr = { 0xFF, 0xFF }, .sourceAddr = { 0 } }, .footer = { { 0 } } };
   memcpy(subscription_packet.header.sourceAddr, uid, sizeof(subscription_packet.header.sourceAddr));
   srand(dwt_readsystimestamphi32());
}

scheduler_phase_t subscription_phase_begin(uint8_t scheduled_slot, uint8_t schedule_size, uint32_t start_delay_dwt)
{
   // Initialize the Subscription Phase start time for calculating timing offsets
   current_phase = SUBSCRIPTION_PHASE;
   schedule_index = scheduled_slot;
   schedule_length = schedule_size;
   reference_time = ((uint64_t)start_delay_dwt) << 8;
   dwt_setreferencetrxtime(start_delay_dwt);
   ranging_radio_choose_antenna(0);

   // Reset the necessary Subscription Phase parameters
   if (schedule_index == UNSCHEDULED_SLOT)
   {
      dwt_writetxfctrl(sizeof(subscription_packet_t), 0, 0);
      dwt_setdelayedtrxtime((uint32_t)((US_TO_DWT(RECEIVE_EARLY_START_US + (rand() % (SUBSCRIPTION_TIMEOUT_US - 100))) - TX_ANTENNA_DELAY) >> 8) & 0xFFFFFFFE);
      if ((dwt_writetxdata(sizeof(subscription_packet_t) - sizeof(ieee154_footer_t), (uint8_t*)&subscription_packet, 0) != DWT_SUCCESS) || (dwt_starttx(DWT_START_TX_DLY_REF) != DWT_SUCCESS))
         print("ERROR: Failed to transmit SUBSCRIPTION request packet\n");
      else
         return SUBSCRIPTION_PHASE;
   }
   else if (!schedule_index)
   {
      dwt_setdelayedtrxtime(0);
      dwt_setpreambledetecttimeout(0);
      dwt_setrxtimeout(DW_TIMEOUT_FROM_US(RECEIVE_EARLY_START_US + SUBSCRIPTION_TIMEOUT_US));
      if (dwt_rxenable(DWT_START_RX_DLY_REF | DWT_IDLE_ON_DLY_ERR) != DWT_SUCCESS)
         print("ERROR: Unable to start listening for SUBSCRIPTION packets\n");
      else
         return SUBSCRIPTION_PHASE;
   }

   // Transition to the Ranging Phase at the appropriate future time
   current_phase = RANGING_PHASE;
   return ranging_phase_begin(schedule_index, schedule_length, (uint32_t)((reference_time + US_TO_DWT(SUBSCRIPTION_BROADCAST_PERIOD_US)) >> 8) & 0xFFFFFFFE);
}

scheduler_phase_t subscription_phase_tx_complete(void)
{
   // Forward this request to the next phase if not currently in the Subscription Phase
   if (current_phase != SUBSCRIPTION_PHASE)
      return ranging_phase_tx_complete();
   current_phase = RANGING_PHASE;
   return ranging_phase_begin(schedule_index, schedule_length, (uint32_t)((reference_time + US_TO_DWT(SUBSCRIPTION_BROADCAST_PERIOD_US)) >> 8) & 0xFFFFFFFE);
}

scheduler_phase_t subscription_phase_rx_complete(subscription_packet_t* packet)
{
   // Ensure that this packet is of the expected type and schedule the requesting device
   if (current_phase != SUBSCRIPTION_PHASE)
      return ranging_phase_rx_complete((ranging_packet_t*)packet);
   else if (packet->header.msgType != SUBSCRIPTION_PACKET)
   {
      print("ERROR: Received an unexpected message type during SUBSCRIPTION phase...possible network collision\n");
      return MESSAGE_COLLISION;
   }
   schedule_phase_add_device(packet->header.sourceAddr[0]);
   return subscription_phase_rx_error();
}

scheduler_phase_t subscription_phase_rx_error(void)
{
   // Forward this request to the next phase if not currently in the Subscription Phase
   if (current_phase != SUBSCRIPTION_PHASE)
      return ranging_phase_rx_error();

   // Attempt to re-enable listening for additional Subscription packets
   register const uint32_t time_elapsed_us = DWT_TO_US((uint64_t)(dwt_readsystimestamphi32() - (uint32_t)(reference_time >> 8)) << 8);
   if ((time_elapsed_us + 600) <= (RECEIVE_EARLY_START_US + SUBSCRIPTION_TIMEOUT_US))
   {
      print("INFO: More time left in the Subscription phase...listening again\n");
      dwt_setrxtimeout(DW_TIMEOUT_FROM_US(RECEIVE_EARLY_START_US + SUBSCRIPTION_TIMEOUT_US - time_elapsed_us));
      if (dwt_rxenable(DWT_START_RX_IMMEDIATE) != DWT_SUCCESS)
         print("ERROR: Unable to restart listening for SUBSCRIPTION packets\n");
      else
         return SUBSCRIPTION_PHASE;
   }

   // Move on to the Ranging phase
   current_phase = RANGING_PHASE;
   return ranging_phase_begin(schedule_index, schedule_length, (uint32_t)((reference_time + US_TO_DWT(SUBSCRIPTION_BROADCAST_PERIOD_US)) >> 8) & 0xFFFFFFFE);
}
