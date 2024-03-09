#include <math.h>
#include <string.h>
#include "stm32f0xx.h"
#include "stm32f0xx_spi.h"
#include "stm32f0xx_dma.h"
#include "stm32f0xx_exti.h"
#include "stm32f0xx_syscfg.h"
#include "stm32f0xx_misc.h"
#include "stm32f0xx_gpio.h"
#include "stm32f0xx_rcc.h"
#include "stm32f0xx_usart.h"

#include "deca_device_api.h"
#include "deca_regs.h"

#include "configuration.h"
#include "dw1000.h"
#include "delay.h"
#include "scheduler.h"
#include "SEGGER_RTT.h"
#include "squarepoint.h"

/******************************************************************************/
// Constants for the DW1000
/******************************************************************************/

#define XTAL_FREQ_HZ 38400000
#define RADIO_STATE_INIT 0x000000
#define RADIO_STATE_IDLE 0x010000
#define RADIO_STATE_TX_WAIT 0x020000
#define RADIO_STATE_RX_WAIT 0x030000
#define RADIO_STATE_TX 0x040000
#define RADIO_STATE_RX 0x050000

uint8_t eui[EUI_LEN] = { 0 };
const uint8_t pgDelay[DW1000_NUM_CHANNELS] = { 0x0, 0xc9, 0xc2, 0xc5, 0x95, 0xc0, 0x0, 0x93 };

//NOTE: THIS IS DEPENDENT ON BAUDRATE
// Max power: 0x1F1F1F1FUL
// TODO: Modify 1, 3, 5 from ranging.c to different power to c0 for power
const uint8_t gain_delta = 0;
const uint32_t tx_power[3] = { 0x67676767UL, 0x8B8B8B8BUL, 0x85858585UL };
const uint32_t txPower_smartDisabled[DW1000_NUM_CHANNELS] = { 0x0, tx_power[0], 0x67676767UL, tx_power[1], 0x9A9A9A9AUL, tx_power[2], 0x0, 0xD1D1D1D1UL };

#ifndef DW1000_MAXIMIZE_TX_POWER
const uint32_t txPower_smartEnabled[DW1000_NUM_CHANNELS] = { 0x0, 0x07274767UL, 0x07274767UL, 0x2B4B6B8BUL, 0x3A5A7A9AUL, 0x25456585UL, 0x0, 0x5171B1D1UL };
#else
const uint32_t txPower_smartEnabled[DW1000_NUM_CHANNELS] = { 0x0, 0x1F1F1F1FUL, 0x1F1F1F1FUL, 0x1F1F1F1FUL, 0x1F1F1F1FUL, 0x1F1F1F1FUL, 0x0, 0x1F1F1F1FUL };
#endif

// Structures present in the native DW1000 driver
extern const uint8 chan_idx[8];
extern const uint32 fs_pll_cfg[6];
extern const uint8 fs_pll_tune[6];
extern const uint32 tx_config[6];

/******************************************************************************/
// Data structures used in multiple functions
/******************************************************************************/

// These are for configuring the hardware peripherals on the STM32F0
static DMA_InitTypeDef DMA_InitStructure;
static SPI_InitTypeDef SPI_InitStructure;

// Setup TX/RX settings on the DW1000
static dwt_config_t _dw1000_config;
static dwt_txconfig_t global_tx_config;

// tion values and other things programmed in with flash
static dw1000_programmed_values_t _prog_values;

static uint64_t _last_dw_timestamp;
static uint64_t _dw_timestamp_overflow;

/******************************************************************************/
// Internal state for this file
/******************************************************************************/

// Keep track of whether we have inited the STM hardware
static bool _stm_dw1000_interface_setup = FALSE;

// Whether or not interrupts are enabled.
decaIrqStatus_t dw1000_irq_onoff = 0;

// Whether the DW1000 is in SLEEP mode
static bool _dw1000_asleep = FALSE;

// Scratch buffers for DMA transfers
static uint8_t throwAwayRx[MAX_SPI_TRANSACTION_BYTES] = { 0 };
static uint8_t throwAwayTx[MAX_SPI_TRANSACTION_BYTES] = { 0 };

/******************************************************************************/
// STM32F0 Hardware setup functions
/******************************************************************************/

static void dw1000_interrupt_enable(void)
{
   // Enable and set EXTIx Interrupt
   NVIC_InitTypeDef NVIC_InitStructure;
   NVIC_InitStructure.NVIC_IRQChannel = DW_INTERRUPT_EXTI_IRQn;
   NVIC_InitStructure.NVIC_IRQChannelPriority = 0x00;
   NVIC_InitStructure.NVIC_IRQChannelCmd = ENABLE;
   NVIC_Init(&NVIC_InitStructure);
}

static void dw1000_interrupt_disable(void)
{
   // Disable the EXTIx Interrupt
   NVIC_InitTypeDef NVIC_InitStructure;
   NVIC_InitStructure.NVIC_IRQChannel = DW_INTERRUPT_EXTI_IRQn;
   NVIC_InitStructure.NVIC_IRQChannelPriority = 0x00;
   NVIC_InitStructure.NVIC_IRQChannelCmd = DISABLE;
   NVIC_Init(&NVIC_InitStructure);
}

// Configure SPI + GPIOs for SPI. Also preset some DMA constants.
static void setup(void)
{
   GPIO_InitTypeDef GPIO_InitStructure;
   EXTI_InitTypeDef EXTI_InitStructure;

   // Enable the SPI peripheral
   RCC_APB2PeriphClockCmd(SPI1_CLK, ENABLE);

   // Enable the DMA peripheral
   RCC_AHBPeriphClockCmd(DMA1_CLK, ENABLE);

   // Enable SCK, MOSI, MISO and NSS GPIO clocks
   RCC_AHBPeriphClockCmd(SPI1_SCK_GPIO_CLK | SPI1_MISO_GPIO_CLK | SPI1_MOSI_GPIO_CLK | SPI1_NSS_GPIO_CLK, ENABLE);

   // SPI pin mappings
   GPIO_PinAFConfig(SPI1_SCK_GPIO_PORT, SPI1_SCK_SOURCE, SPI1_SCK_AF);
   GPIO_PinAFConfig(SPI1_MOSI_GPIO_PORT, SPI1_MOSI_SOURCE, SPI1_MOSI_AF);
   GPIO_PinAFConfig(SPI1_MISO_GPIO_PORT, SPI1_MISO_SOURCE, SPI1_MISO_AF);

   // Configure SPI pins
   GPIO_InitStructure.GPIO_Mode = GPIO_Mode_AF;
   GPIO_InitStructure.GPIO_OType = GPIO_OType_PP;
   GPIO_InitStructure.GPIO_PuPd = GPIO_PuPd_NOPULL;
   GPIO_InitStructure.GPIO_Speed = GPIO_Speed_50MHz;

   // SPI SCK pin configuration
   GPIO_InitStructure.GPIO_Pin = SPI1_SCK_PIN;
   GPIO_Init(SPI1_SCK_GPIO_PORT, &GPIO_InitStructure);

   // SPI MOSI pin configuration
   GPIO_InitStructure.GPIO_Pin = SPI1_MOSI_PIN;
   GPIO_Init(SPI1_MOSI_GPIO_PORT, &GPIO_InitStructure);

   // SPI MISO pin configuration
   GPIO_InitStructure.GPIO_Pin = SPI1_MISO_PIN;
   GPIO_Init(SPI1_MISO_GPIO_PORT, &GPIO_InitStructure);

   // SPI NSS pin configuration
   GPIO_InitStructure.GPIO_Mode = GPIO_Mode_OUT;
   GPIO_InitStructure.GPIO_Pin = SPI1_NSS_PIN;
   GPIO_Init(SPI1_NSS_GPIO_PORT, &GPIO_InitStructure);
   GPIO_WriteBit(SPI1_NSS_GPIO_PORT, SPI1_NSS_PIN, Bit_SET);

   // SPI configuration
   SPI_I2S_DeInit(SPI1);
   SPI_InitStructure.SPI_Direction = SPI_Direction_2Lines_FullDuplex;
   SPI_InitStructure.SPI_DataSize = SPI_DataSize_8b;
   SPI_InitStructure.SPI_CPOL = SPI_CPOL_Low;
   SPI_InitStructure.SPI_CPHA = SPI_CPHA_1Edge;
   SPI_InitStructure.SPI_NSS = SPI_NSS_Soft;
   SPI_InitStructure.SPI_BaudRatePrescaler = SPI_BaudRatePrescaler_32;
   SPI_InitStructure.SPI_FirstBit = SPI_FirstBit_MSB;
   SPI_InitStructure.SPI_CRCPolynomial = 7;
   SPI_InitStructure.SPI_Mode = SPI_Mode_Master;
   SPI_Init(SPI1, &SPI_InitStructure);

   // Initialize the FIFO threshold
   // This is critical for 8 bit transfers
   SPI_RxFIFOThresholdConfig(SPI1, SPI_RxFIFOThreshold_QF);

   // Setup interrupt from the DW1000
   // Enable GPIOB clock
   RCC_AHBPeriphClockCmd(DW_INTERRUPT_CLK, ENABLE);

   // Configure DW1000 interrupt pin as input floating
   GPIO_InitStructure.GPIO_Pin = DW_INTERRUPT_PIN;
   GPIO_InitStructure.GPIO_Mode = GPIO_Mode_IN;
   GPIO_InitStructure.GPIO_PuPd = GPIO_PuPd_NOPULL;
   GPIO_Init(DW_INTERRUPT_PORT, &GPIO_InitStructure);

   // Enable SYSCFG clock
   RCC_APB2PeriphClockCmd(RCC_APB2Periph_SYSCFG, ENABLE);
   // Connect EXTIx Line to DW Int pin
   SYSCFG_EXTILineConfig(DW_INTERRUPT_EXTI_PORT, DW_INTERRUPT_EXTI_PIN);

   // Configure EXTIx line for interrupt
   EXTI_InitStructure.EXTI_Line = DW_INTERRUPT_EXTI_LINE;
   EXTI_InitStructure.EXTI_Mode = EXTI_Mode_Interrupt;
   EXTI_InitStructure.EXTI_Trigger = EXTI_Trigger_Rising;
   EXTI_InitStructure.EXTI_LineCmd = ENABLE;
   EXTI_Init(&EXTI_InitStructure);

   // Enable interrupt from the DW1000
   dw1000_interrupt_enable();
   dw1000_irq_onoff = 1;

   // Setup reset pin. Make it input unless we need it
   RCC_AHBPeriphClockCmd(DW_RESET_CLK, ENABLE);
   GPIO_InitStructure.GPIO_Pin = DW_RESET_PIN;
   GPIO_InitStructure.GPIO_Mode = GPIO_Mode_IN;
   GPIO_InitStructure.GPIO_OType = GPIO_OType_PP;
   GPIO_InitStructure.GPIO_Speed = GPIO_Speed_Level_1;
   GPIO_InitStructure.GPIO_PuPd = GPIO_PuPd_NOPULL;
   GPIO_Init(DW_RESET_PORT, &GPIO_InitStructure);
   GPIO_WriteBit(DW_RESET_PORT, DW_RESET_PIN, Bit_SET);

   // Setup wakeup pin
   RCC_AHBPeriphClockCmd(DW_WAKEUP_CLK, ENABLE);
   GPIO_InitStructure.GPIO_Pin = DW_WAKEUP_PIN;
   GPIO_InitStructure.GPIO_Mode = GPIO_Mode_OUT;
   GPIO_InitStructure.GPIO_OType = GPIO_OType_PP;
   GPIO_InitStructure.GPIO_Speed = GPIO_Speed_Level_3;
   GPIO_InitStructure.GPIO_PuPd = GPIO_PuPd_NOPULL;
   GPIO_Init(DW_WAKEUP_PORT, &GPIO_InitStructure);
   GPIO_WriteBit(DW_WAKEUP_PORT, DW_WAKEUP_PIN, Bit_RESET);

   // Setup antenna pins - select no antennas
   RCC_AHBPeriphClockCmd(ANT_SEL0_CLK, ENABLE);
   RCC_AHBPeriphClockCmd(ANT_SEL1_CLK, ENABLE);
   RCC_AHBPeriphClockCmd(ANT_SEL2_CLK, ENABLE);

   GPIO_InitStructure.GPIO_Pin = ANT_SEL0_PIN;
   GPIO_InitStructure.GPIO_Mode = GPIO_Mode_OUT;
   GPIO_InitStructure.GPIO_OType = GPIO_OType_PP;
   GPIO_InitStructure.GPIO_Speed = GPIO_Speed_50MHz;
   GPIO_InitStructure.GPIO_PuPd = GPIO_PuPd_NOPULL;
   GPIO_Init(ANT_SEL0_PORT, &GPIO_InitStructure);

   GPIO_InitStructure.GPIO_Pin = ANT_SEL1_PIN;
   GPIO_InitStructure.GPIO_Mode = GPIO_Mode_OUT;
   GPIO_InitStructure.GPIO_OType = GPIO_OType_PP;
   GPIO_InitStructure.GPIO_Speed = GPIO_Speed_50MHz;
   GPIO_InitStructure.GPIO_PuPd = GPIO_PuPd_NOPULL;
   GPIO_Init(ANT_SEL1_PORT, &GPIO_InitStructure);

   GPIO_InitStructure.GPIO_Pin = ANT_SEL2_PIN;
   GPIO_InitStructure.GPIO_Mode = GPIO_Mode_OUT;
   GPIO_InitStructure.GPIO_OType = GPIO_OType_PP;
   GPIO_InitStructure.GPIO_Speed = GPIO_Speed_50MHz;
   GPIO_InitStructure.GPIO_PuPd = GPIO_PuPd_NOPULL;
   GPIO_Init(ANT_SEL2_PORT, &GPIO_InitStructure);

   // Initialize the RF Switch
   GPIO_WriteBit(ANT_SEL0_PORT, ANT_SEL0_PIN, Bit_SET);
   GPIO_WriteBit(ANT_SEL1_PORT, ANT_SEL1_PIN, Bit_RESET);
   GPIO_WriteBit(ANT_SEL2_PORT, ANT_SEL2_PIN, Bit_RESET);

   // Pre-populate DMA fields that don't need to change
   DMA_InitStructure.DMA_PeripheralBaseAddr = (uint32_t) SPI1_DR_ADDRESS;
   DMA_InitStructure.DMA_PeripheralDataSize = DMA_PeripheralDataSize_Byte;
   DMA_InitStructure.DMA_MemoryDataSize = DMA_MemoryDataSize_Byte;
   DMA_InitStructure.DMA_PeripheralInc = DMA_PeripheralInc_Disable;
   DMA_InitStructure.DMA_Mode = DMA_Mode_Normal;
   DMA_InitStructure.DMA_M2M = DMA_M2M_Disable;

   // Set default calibration values
   for (uint8_t i = 0; i < 3; i++)
      for (uint8_t j = 0; j < 3; j++)
         _prog_values.calibration_values[i][j] = DW1000_DEFAULT_CALIBRATION;

   // Mark that this function has run so we don't do it again.
   _stm_dw1000_interface_setup = TRUE;
}

// Apply a suite of baseline settings that we care about.
// This is split out so we can call it after sleeping.
static dw1000_err_e dw1000_configure_settings(void)
{
   // Also need the SPI slow here.
   dw1000_spi_slow();

   // Initialize the dw1000 hardware
   uint32_t err = dwt_initialise(DWT_LOADUCODE);
   if (err != DWT_SUCCESS)
   {
      dw1000_spi_fast();
      return DW1000_COMM_ERR;
   }

   // Configure interrupts
   dwt_setinterrupt(0xFFFFFFFF, 0);
   dwt_setinterrupt(DWT_INT_TFRS | DWT_INT_RFCG | DWT_INT_RPHE | DWT_INT_RFCE | DWT_INT_RFSL | DWT_INT_RFTO | DWT_INT_RXPTO | DWT_INT_SFDT | DWT_INT_ARFE, 1);

   // Set the parameters of ranging and channel and whatnot
   _dw1000_config.chan = 1;
   _dw1000_config.prf = DWT_PRF_64M;
   _dw1000_config.txPreambLength = DW1000_PREAMBLE_LENGTH;
   _dw1000_config.rxPAC = DW1000_PAC_SIZE;
   _dw1000_config.txCode = 9;  // preamble code
   _dw1000_config.rxCode = 9;  // preamble code
   _dw1000_config.nsSFD = 0;
   _dw1000_config.dataRate = DW1000_DATA_RATE;
   _dw1000_config.phrMode = DWT_PHRMODE_EXT;  //Enable extended PHR mode (up to 1024-byte packets)
   _dw1000_config.sfdTO = DW1000_SFD_TO;

   // Configure DW1000 and enable PLLLDT control bit
   dwt_configure(&_dw1000_config);
   dwt_write32bitoffsetreg(EXT_SYNC_ID, EC_CTRL_OFFSET, 0x04);

   // Configure TX power based on the channel used
   dwt_setsmarttxpower(DW1000_SMART_PWR_EN);
   global_tx_config.PGdly = pgDelay[_dw1000_config.chan];
   if (DW1000_SMART_PWR_EN)
      global_tx_config.power = txPower_smartEnabled[_dw1000_config.chan];
   else
      global_tx_config.power = txPower_smartDisabled[_dw1000_config.chan];
   dwt_configuretxrf(&global_tx_config);

   // Set some radio properties
   dwt_setxtaltrim(DW1000_DEFAULT_XTALTRIM);
   dwt_setrxantennadelay(DW1000_ANTENNA_DELAY_RX);
   dwt_settxantennadelay(DW1000_ANTENNA_DELAY_TX);

   // Set this node's ID and the PAN ID for our DW1000 ranging system
   uint8_t eui_array[8];
   dw1000_read_eui(eui_array);
   dwt_seteui(eui_array);
   dwt_setpanid(MODULE_PANID);

   // Always good to make sure we don't trap the SPI speed too slow
   dw1000_spi_fast();
   return DW1000_NO_ERR;
}

// Functions to configure the SPI speed
void dw1000_spi_fast(void)
{
   SPI_InitStructure.SPI_BaudRatePrescaler = SPI_BaudRatePrescaler_4;
   SPI_Init(SPI1, &SPI_InitStructure);
}

void dw1000_spi_slow(void)
{
   SPI_InitStructure.SPI_BaudRatePrescaler = SPI_BaudRatePrescaler_32;
   SPI_Init(SPI1, &SPI_InitStructure);
}

// Only write data to the DW1000, and use DMA to do it.
static void setup_dma_write(uint32_t length, const uint8_t *tx)
{
   // DMA channel Rx of SPI Configuration
   DMA_InitStructure.DMA_BufferSize = length;
   DMA_InitStructure.DMA_MemoryBaseAddr = (uint32_t)throwAwayRx;
   DMA_InitStructure.DMA_DIR = DMA_DIR_PeripheralSRC;
   DMA_InitStructure.DMA_MemoryInc = DMA_MemoryInc_Disable;
   DMA_InitStructure.DMA_Priority = DMA_Priority_High;
   DMA_Init(SPI1_RX_DMA_CHANNEL, &DMA_InitStructure);

   // DMA channel Tx of SPI Configuration
   DMA_InitStructure.DMA_BufferSize = length;
   DMA_InitStructure.DMA_MemoryBaseAddr = (uint32_t)tx;
   DMA_InitStructure.DMA_DIR = DMA_DIR_PeripheralDST;
   DMA_InitStructure.DMA_MemoryInc = DMA_MemoryInc_Enable;
   DMA_InitStructure.DMA_Priority = DMA_Priority_High;
   DMA_Init(SPI1_TX_DMA_CHANNEL, &DMA_InitStructure);
}

// Setup just a read over SPI using DMA.
static void setup_dma_read(uint32_t length, uint8_t *rx)
{
   // DMA channel Rx of SPI Configuration
   DMA_InitStructure.DMA_BufferSize = length;
   DMA_InitStructure.DMA_MemoryBaseAddr = (uint32_t)rx;
   DMA_InitStructure.DMA_DIR = DMA_DIR_PeripheralSRC;
   DMA_InitStructure.DMA_MemoryInc = DMA_MemoryInc_Enable;
   DMA_InitStructure.DMA_Priority = DMA_Priority_High;
   DMA_Init(SPI1_RX_DMA_CHANNEL, &DMA_InitStructure);

   // DMA channel Tx of SPI Configuration
   DMA_InitStructure.DMA_BufferSize = length;
   DMA_InitStructure.DMA_MemoryBaseAddr = (uint32_t)throwAwayTx;
   DMA_InitStructure.DMA_DIR = DMA_DIR_PeripheralDST;
   DMA_InitStructure.DMA_MemoryInc = DMA_MemoryInc_Disable;
   DMA_InitStructure.DMA_Priority = DMA_Priority_High;
   DMA_Init(SPI1_TX_DMA_CHANNEL, &DMA_InitStructure);
}

/******************************************************************************/
// Interrupt callbacks
/******************************************************************************/

// Not needed, but handle the interrupt from the SPI DMA
void DMA1_Channel2_3_IRQHandler(void)
{
}

// HW interrupt for the interrupt pin from the DW1000
void EXTI2_3_IRQHandler(void)
{
   if (EXTI_GetITStatus(DW_INTERRUPT_EXTI_LINE) != RESET)
   {
      // Mark this interrupt as had occurred in the main thread.
      mark_interrupt(INTERRUPT_DW1000);

      // Clear the EXTI line 2 pending bit
      EXTI_ClearITPendingBit(DW_INTERRUPT_EXTI_LINE);
   }
}

// Main thread interrupt handler for the interrupt from the DW1000. Basically
// just passes knowledge of the interrupt on to the DW1000 library.
void dw1000_interrupt_fired(void)
{
   // Keep calling the decawave interrupt handler as long as the interrupt pin
   // is asserted, but add an escape hatch so we don't get stuck forever.
   uint8_t count = 0;
   do { dwt_isr(); }
   while (GPIO_ReadInputDataBit(DW_INTERRUPT_PORT, DW_INTERRUPT_PIN) && (++count < DW1000_NUM_CONSECUTIVE_INTERRUPTS_BEFORE_RESET));

   if (count >= DW1000_NUM_CONSECUTIVE_INTERRUPTS_BEFORE_RESET)
   {
      // Well this is not good. It looks like the interrupt got stuck high,
      // so we'd spend the rest of the time just reading this interrupt.
      // Not much we can do here but reset everything.
      debug_msg("ERROR: The DW1000 radio appears to be stuck in an interrupt handler!\n");
      module_reset();
   }
}

/******************************************************************************/
// Required API implementation for the DecaWave library
/******************************************************************************/

// Blocking SPI transfer
static int spi_transfer(void)
{
   DMA_Cmd(SPI1_RX_DMA_CHANNEL, ENABLE);
   DMA_Cmd(SPI1_TX_DMA_CHANNEL, ENABLE);

   // Wait for everything to finish
   uint32_t loop = 0;
   while ((DMA_GetFlagStatus(SPI1_RX_DMA_FLAG_TC) == RESET) && loop < 100000) ++loop;
   if (loop < 100000)
   {
      while ((DMA_GetFlagStatus(SPI1_TX_DMA_FLAG_TC) == RESET)) ;
      while ((SPI_I2S_GetFlagStatus(SPI1, SPI_I2S_FLAG_TXE) == RESET)) ;
      while ((SPI_I2S_GetFlagStatus(SPI1, SPI_I2S_FLAG_BSY) == SET)) ;
   }

   // End the SPI transaction and DMA
   // Clear DMA1 global flags
   DMA_ClearFlag(SPI1_TX_DMA_FLAG_GL);
   DMA_ClearFlag(SPI1_RX_DMA_FLAG_GL);

   // Disable the DMA channels
   DMA_Cmd(SPI1_RX_DMA_CHANNEL, DISABLE);
   DMA_Cmd(SPI1_TX_DMA_CHANNEL, DISABLE);

   return ((loop >= 100000) ? -1 : 0);
}

static void dma_enable(void)
{
   SPI_Cmd(SPI1, ENABLE);
   GPIO_WriteBit(SPI1_NSS_GPIO_PORT, SPI1_NSS_PIN, Bit_RESET);

   // Enable NSS output for master mode
   SPI_SSOutputCmd(SPI1, ENABLE);

   // Enable the DMA channels
   SPI_I2S_DMACmd(SPI1, SPI_I2S_DMAReq_Rx, ENABLE);
   SPI_I2S_DMACmd(SPI1, SPI_I2S_DMAReq_Tx, ENABLE);

}

static void dma_disable(void)
{
   // Disable the SPI Rx and Tx DMA requests
   SPI_I2S_DMACmd(SPI1, SPI_I2S_DMAReq_Rx, DISABLE);
   SPI_I2S_DMACmd(SPI1, SPI_I2S_DMAReq_Tx, DISABLE);

   GPIO_WriteBit(SPI1_NSS_GPIO_PORT, SPI1_NSS_PIN, Bit_SET);
   SPI_Cmd(SPI1, DISABLE);
}

// Called by the DW1000 library to issue a read command to the DW1000.
int readfromspi(uint16 headerLength, const uint8 *headerBuffer, uint32 readlength, uint8 *readBuffer)
{
   for (uint8_t tries = 0; tries < 3; ++tries)
   {
      // Enable DMA for both of the two following transactions
      dma_enable();

      // Write the SPI header
      setup_dma_write(headerLength, headerBuffer);
      if (spi_transfer())
      {
         dma_disable();
         continue;
      }

      setup_dma_read(readlength, readBuffer);
      if (spi_transfer())
      {
         dma_disable();
         continue;
      }

      // Disable DMA again
      dma_disable();
      return 0;
   }

   debug_msg("ERROR: Something went wrong reading from SPI!\n");
   module_reset();
   return -1;
}

// Called by the DW1000 library to issue a write to the DW1000.
int writetospi(uint16 headerLength, const uint8 *headerBuffer, uint32 bodylength, const uint8 *bodyBuffer)
{
   for (uint8_t tries = 0; tries < 3; ++tries)
   {
      // Enable DMA for both of the two following transactions
      dma_enable();

      setup_dma_write(headerLength, headerBuffer);
      if (spi_transfer())
      {
         dma_disable();
         continue;
      }

      setup_dma_write(bodylength, bodyBuffer);
      if (spi_transfer())
      {
         dma_disable();
         continue;
      }

      // Disable DMA again
      dma_disable();
      return 0;
   }

   debug_msg("ERROR: Something went wrong writing to SPI!\n");
   module_reset();
   return -1;
}

// Atomic blocks for the DW1000 library
decaIrqStatus_t decamutexon(void)
{
   if (dw1000_irq_onoff == 1)
   {
      dw1000_interrupt_disable();
      dw1000_irq_onoff = 0;
      return 1;
   }
   return 0;
}

// Atomic blocks for the DW1000 library
void decamutexoff(decaIrqStatus_t s)
{
   if (s)
   {
      dw1000_interrupt_enable();
      dw1000_irq_onoff = 1;
   }
}

void deca_sleep(unsigned int time_ms)
{
   // Expected to be blocking
   mDelay(time_ms);
}

// Rename this function for the DW1000 library.
void usleep(uint32_t u)
{
   uDelay(u);
}

/******************************************************************************/
// Generic DW1000 functions - shared with anchor and tag
/******************************************************************************/

// Hard reset the DW1000 using its reset pin
void dw1000_reset_hard(bool reinit)
{
   // Set reset pin to OUTPUT mode so that we can manually reset device
   GPIO_InitTypeDef GPIO_InitStructure;
   GPIO_InitStructure.GPIO_Pin = DW_RESET_PIN;
   GPIO_InitStructure.GPIO_Mode = GPIO_Mode_OUT;
   GPIO_InitStructure.GPIO_OType = GPIO_OType_PP;
   GPIO_InitStructure.GPIO_Speed = GPIO_Speed_50MHz;
   GPIO_InitStructure.GPIO_PuPd = GPIO_PuPd_NOPULL;
   GPIO_Init(DW_RESET_PORT, &GPIO_InitStructure);

   // Assert the reset pin for 10ms
   GPIO_WriteBit(DW_RESET_PORT, DW_RESET_PIN, Bit_RESET);
   mDelay(10);
   GPIO_WriteBit(DW_RESET_PORT, DW_RESET_PIN, Bit_SET);

   // Make reset pin INPUT again so that the device can wakeup using WAKEUP pin
   GPIO_InitStructure.GPIO_Mode = GPIO_Mode_IN;
   GPIO_Init(DW_RESET_PORT, &GPIO_InitStructure);
   GPIO_WriteBit(DW_RESET_PORT, DW_RESET_PIN, Bit_SET);

   // Re-initialize all HW connections if requested
   _dw1000_asleep = FALSE;
   if (reinit)
      setup();
}

// Choose which antenna to connect to the radio
void dw1000_choose_antenna(uint8_t antenna_number)
{
   // Return immediately if no change necessary
   if (antenna_number == DO_NOT_CHANGE_FLAG)
      return;

   // Enable the desired antenna
   switch (antenna_number)
   {
      case 0:
         GPIO_WriteBit(ANT_SEL0_PORT, ANT_SEL0_PIN, Bit_SET);
         GPIO_WriteBit(ANT_SEL1_PORT, ANT_SEL1_PIN, Bit_RESET);
         GPIO_WriteBit(ANT_SEL2_PORT, ANT_SEL2_PIN, Bit_RESET);
         break;
      case 1:
         GPIO_WriteBit(ANT_SEL0_PORT, ANT_SEL0_PIN, Bit_RESET);
         GPIO_WriteBit(ANT_SEL1_PORT, ANT_SEL1_PIN, Bit_SET);
         GPIO_WriteBit(ANT_SEL2_PORT, ANT_SEL2_PIN, Bit_RESET);
         break;
      case 2:
         GPIO_WriteBit(ANT_SEL0_PORT, ANT_SEL0_PIN, Bit_RESET);
         GPIO_WriteBit(ANT_SEL1_PORT, ANT_SEL1_PIN, Bit_RESET);
         GPIO_WriteBit(ANT_SEL2_PORT, ANT_SEL2_PIN, Bit_SET);
         break;
   }
}

// Read this node's EUI from the correct address in flash
void dw1000_read_eui(uint8_t *eui_buf)
{
   // Determine if the EUI has already been loaded
   uint8_t eui_loaded = 0;
   for (int i = 0; i < EUI_LEN; ++i)
      if (eui[i])
      {
         eui_loaded = 1;
         break;
      }

   // Copy the EUI into the specified buffer
   if (!eui_loaded)
   {
      memcpy(eui, (uint8_t*)FLASH_LOCATION_EUI, EUI_LEN);
      memcpy(eui_buf, (uint8_t*)FLASH_LOCATION_EUI, EUI_LEN);
   }
   else
      memcpy(eui_buf, eui, EUI_LEN);
}

void dw1000_update_runtime_eui(uint8_t *eui_buf)
{
   // Set this node's ID and the PAN ID for our DW1000 ranging system
   dw1000_spi_slow();
   memcpy(eui, eui_buf, EUI_LEN);
   dwt_seteui(eui_buf);
   dw1000_spi_fast();
}

bool dw1000_radio_disable(void)
{
   // Turn off the radio and ensure that it is IDLE
   dwt_forcetrxoff();
   uint32_t reg = dwt_read32bitreg(SYS_STATE_ID);
   if (!(reg & RADIO_STATE_IDLE))
   {
      debug_msg("ERROR: Unable to turn radio OFF\n");
      return FALSE;
   }
   return TRUE;
}

bool dw1000_rxenable(int mode, uint8_t channel, uint8_t antenna)
{
   // Turn off the radio and ensure that it is IDLE
   if (!dw1000_radio_disable())
      return FALSE;

   // Update the channel and antenna
   dw1000_update_channel(channel);
   dw1000_choose_antenna(antenna);

   // Attempt to enable the receiver
   if (dwt_rxenable(mode) == DWT_SUCCESS)
   {
      uint32_t reg = dwt_read32bitreg(SYS_STATE_ID);
      if (reg & (RADIO_STATE_RX | ((mode & DWT_START_RX_DELAYED) ? RADIO_STATE_RX_WAIT : 0)))
         return TRUE;
   }

   // Disable RX mode if a successful transition was not reported
   debug_msg("ERROR: Could not enable radio RX mode\n");
   dw1000_radio_disable();
   return FALSE;
}

// Return the TX+RX delay calibration value for this particular node
// in DW1000 time format.
uint64_t dw1000_get_tx_delay(uint8_t channel_index, uint8_t antenna_index)
{
   // Make sure that antenna and channel are 0<=index<3
   channel_index = channel_index % 3;
   antenna_index = antenna_index % 3;

   return (uint64_t) (_prog_values.calibration_values[channel_index][antenna_index] * DW1000_DELAY_TX) / 100;
}

uint64_t dw1000_get_rx_delay(uint8_t channel_index, uint8_t antenna_index)
{
   // Make sure that antenna and channel are 0<=index<3
   channel_index = channel_index % 3;
   antenna_index = antenna_index % 3;

   return (uint64_t) (_prog_values.calibration_values[channel_index][antenna_index] * DW1000_DELAY_RX) / 100;
}

// Get access to the pointer of calibration values. Used for the host interface.
uint8_t* dw1000_get_txrx_delay_raw(void)
{
   return (uint8_t*)_prog_values.calibration_values;
}

// First (generic) init of the DW1000
dw1000_err_e dw1000_init(void)
{
   // Do the STM setup that initializes pin and peripherals
   _dw1000_asleep = FALSE;
   if (!_stm_dw1000_interface_setup)
      setup();

   // Make sure the SPI clock is slow so that the DW1000 doesn't miss any edges
   dw1000_spi_slow();

   // Reset the dw1000...for some reason
   dwt_softreset();

   // Make sure we can talk to the DW1000
   if (dwt_readdevid() != DWT_DEVICE_ID)
      uDelay(100);
   if (dwt_readdevid() != DWT_DEVICE_ID)
      return DW1000_COMM_ERR;

   // Put the SPI back
   dw1000_spi_fast();

   // Choose antenna 0 as a default
   dw1000_choose_antenna(0);
   _last_dw_timestamp = 0;
   _dw_timestamp_overflow = 0;

   // Setup our settings for the DW1000
   return dw1000_configure_settings();
}

uint16_t dw1000_preamble_time_in_us(void)
{
   //value is X*0.99359 for 16 MHz PRF and X*1.01763 for 64 MHz PRF
   uint16_t preamble_len;
   float preamble_time;
   switch (_dw1000_config.txPreambLength)
   {
      case DWT_PLEN_64:
         preamble_len = 64;
         break;
      case DWT_PLEN_128:
         preamble_len = 128;
         break;
      case DWT_PLEN_256:
         preamble_len = 256;
         break;
      case DWT_PLEN_512:
         preamble_len = 512;
         break;
      case DWT_PLEN_1024:
         preamble_len = 1024;
         break;
      case DWT_PLEN_2048:
         preamble_len = 2048;
         break;
      default:
         preamble_len = 4096;
         break;
   }
   if (_dw1000_config.prf == DWT_PRF_16M)
      preamble_time = (float) preamble_len * 0.99359 + 0.5;
   else
      preamble_time = (float) preamble_len * 1.01763 + 0.5;
   return (uint16_t) preamble_time;
}

uint32_t dw1000_packet_data_time_in_us(uint16_t data_len)
{
   float time_per_byte;
   switch (_dw1000_config.dataRate)
   {
      case DWT_BR_110K:
         time_per_byte = 8.0 / 110e3;
         break;
      case DWT_BR_850K:
         time_per_byte = 8.0 / 850e3;
         break;
      default:
         time_per_byte = 8.0 / 6.8e6;
         break;
   }

   return (uint32_t) (time_per_byte * data_len * 1e6 + 0.5);
}

// Put the DW1000 into sleep mode
void dw1000_sleep(bool deep_sleep)
{
   // Check if the chip is already asleep
   if (_dw1000_asleep)
      return;

   // Make sure radio is off and Wakeup pin is low
   if (!dw1000_radio_disable())
      return;
   GPIO_WriteBit(DW_WAKEUP_PORT, DW_WAKEUP_PIN, Bit_RESET);

   // Clear interrupt mask so we don't get any unwanted events
   dwt_setinterrupt(0, 2);
   dwt_write32bitreg(SYS_STATUS_ID, SYS_STATUS_MASK_32);

   // Clear the AON_CFG1 register as described in the user manual
   dwt_write8bitoffsetreg(AON_ID, AON_CFG1_OFFSET, 0x00);

   // Put the TAG into sleep mode at this point
   dwt_configuresleep(DWT_CONFIG | DWT_LOADEUI, (deep_sleep ? 0 : DWT_XTAL_EN) | DWT_WAKE_WK | DWT_SLP_EN);
   dwt_entersleep();

   // Mark that we put the DW1000 to sleep.
   _dw1000_asleep = TRUE;
}

void dw1000_force_deepsleep(void)
{
   // Force the radio to wakeup, disable all antennas, then enter DEEPSLEEP mode
   dw1000_force_wakeup();
   GPIO_WriteBit(ANT_SEL0_PORT, ANT_SEL0_PIN, Bit_RESET);
   GPIO_WriteBit(ANT_SEL1_PORT, ANT_SEL1_PIN, Bit_RESET);
   GPIO_WriteBit(ANT_SEL2_PORT, ANT_SEL2_PIN, Bit_RESET);
   dw1000_sleep(TRUE);
}

// Wake the DW1000 from sleep by asserting the WAKEUP pin
bool dw1000_wakeup(void)
{
   // Check if the chip is already awake
   if (!_dw1000_asleep)
      return TRUE;

   // Assert the WAKEUP pin for >500us
   GPIO_WriteBit(DW_WAKEUP_PORT, DW_WAKEUP_PIN, Bit_SET);
   uDelay(600);
   GPIO_WriteBit(DW_WAKEUP_PORT, DW_WAKEUP_PIN, Bit_RESET);

   // Utilize the slow SPI
   dw1000_spi_slow();

   // Now wait for the chip to move from the wakeup to the idle state
   for (uint8_t tries = 0; tries <= 10; ++tries)
   {
      uint32_t reg = dwt_read32bitoffsetreg(SYS_STATUS_ID, SYS_STATUS_OFFSET);
      if ((reg & SYS_STATUS_CPLOCK) && (dwt_readdevid() == DWT_DEVICE_ID))
         break;
      else if (tries == 10)
      {
         dw1000_spi_fast();
         return FALSE;
      }
      else
         mDelay(1);
   }
   dwt_write32bitoffsetreg(EXT_SYNC_ID, EC_CTRL_OFFSET, 0x04);
   dwt_write32bitoffsetreg(SYS_STATUS_ID, SYS_STATUS_OFFSET, SYS_STATUS_SLP2INIT);

   // Re-enable allowable interrupts
   dwt_setinterrupt(DWT_INT_TFRS | DWT_INT_RFCG | DWT_INT_RPHE | DWT_INT_RFCE | DWT_INT_RFSL | DWT_INT_RFTO | DWT_INT_RXPTO | DWT_INT_SFDT | DWT_INT_ARFE, 1);

   // Restore TX antenna delay
   dwt_settxantennadelay(DW1000_ANTENNA_DELAY_TX);

   // Ensure that the fast SPI is enabled
   dw1000_spi_fast();

   // No longer asleep
   _dw1000_asleep = FALSE;
   return TRUE;
}

bool dw1000_force_wakeup(void)
{
   // Disable check and wake up
   _dw1000_asleep = TRUE;
   return dw1000_wakeup();
}

// Call to change the DW1000 channel and force set all of the configs that are needed when changing channels
void dw1000_update_channel(uint8_t chan)
{
   // Return immediately if no change necessary
   if (chan == DO_NOT_CHANGE_FLAG)
      return;

   // Only send commands to radio if channel was actually changed
   if (_dw1000_config.chan != chan)
   {
      // Set the current radio channel
      _dw1000_config.chan = chan;

      // Configure PLL2/RF PLL block CFG/TUNE for the given channel
      dwt_write32bitoffsetreg(FS_CTRL_ID, FS_PLLCFG_OFFSET, fs_pll_cfg[chan_idx[_dw1000_config.chan]]);
      dwt_write8bitoffsetreg(FS_CTRL_ID, FS_PLLTUNE_OFFSET, fs_pll_tune[chan_idx[_dw1000_config.chan]]);
      dwt_write32bitoffsetreg(RF_CONF_ID, RF_TXCTRL_OFFSET, tx_config[chan_idx[_dw1000_config.chan]]);

      // Update the actual channel number to use
      uint32_t regval = dwt_read32bitreg(CHAN_CTRL_ID);
      regval &= ~(CHAN_CTRL_TX_CHAN_MASK | CHAN_CTRL_RX_CHAN_MASK);
      regval |= (CHAN_CTRL_TX_CHAN_MASK & (_dw1000_config.chan << CHAN_CTRL_TX_CHAN_SHIFT)) | (CHAN_CTRL_RX_CHAN_MASK & (_dw1000_config.chan << CHAN_CTRL_RX_CHAN_SHIFT));
      dwt_write32bitreg(CHAN_CTRL_ID, regval);

      // Update the power configuration for the given channel
      global_tx_config.PGdly = pgDelay[_dw1000_config.chan];
      global_tx_config.power = DW1000_SMART_PWR_EN ? txPower_smartEnabled[_dw1000_config.chan] : txPower_smartDisabled[_dw1000_config.chan];
      dwt_configuretxrf(&global_tx_config);
   }
}

double dw1000_get_received_signal_strength_db(void)
{
   dwt_rxdiag_t diag;
   dwt_readdiagnostics(&diag);
   double test = log10((diag.maxGrowthCIR * 131072.0) / (diag.rxPreamCount * diag.rxPreamCount));
   return (10 * test) - ((_dw1000_config.prf == DWT_PRF_16M) ? 113.77 : 121.74);
}

// Called to go get information on the current status of the DW
uint32_t dw1000_get_status_register(void)
{
   uint32_t status = dwt_read32bitreg(SYS_STATUS_ID);  // Read status register low 32bits

   debug_msg("\n-----------------------\n");
   debug_msg("INFO: Status register: ");
   debug_msg_uint(status);
   debug_msg("\n-----------------------\n\n");

   return status;
}

/******************************************************************************/
// Decawave specific utility functions
/******************************************************************************/

// Convert a time of flight measurement to millimeters
int dwtime_to_millimeters(double dwtime)
{
   // Get meters using the speed of light
   double dist = dwtime * SPEED_OF_LIGHT * DWT_TIME_UNITS;

   // And return millimeters
   return (int)(dist * 1000.0);
}

/******************************************************************************/
// Misc Utility
/******************************************************************************/

// Shoved this here for now.
// Insert an element into a sorted array.
// end is the number of elements in the array.
void insert_sorted(int arr[], int new, unsigned end)
{
   unsigned insert_at = 0;
   while ((insert_at < end) && (new >= arr[insert_at]))
      ++insert_at;
   if (insert_at == end)
      arr[insert_at] = new;
   else
      while (insert_at <= end)
      {
         int temp = arr[insert_at];
         arr[insert_at] = new;
         new = temp;
         ++insert_at;
      }
}

// Get linearly increasing 40bit timestamps; ATTENTION: when comparing to the current time (32bit), make sure to cast it to uint64_t and shift it so it corresponds to a 40bit timestamp
uint64_t dw1000_correct_timestamp(uint64_t dw_timestamp)
{
   // Due to frequent overflow in the Decawave system time counter, we must keep a running total of the number of times it's overflown
   if (dw_timestamp < _last_dw_timestamp)
      _dw_timestamp_overflow += 0x10000000000ULL;
   _last_dw_timestamp = dw_timestamp;

   return _dw_timestamp_overflow + dw_timestamp;
}

uint64_t dw1000_readrxtimestamp(void)
{
   uint64_t cur_dw_timestamp = 0;
   dwt_readrxtimestamp((uint8_t*)&cur_dw_timestamp);

   // Check to see if an overflow has occurred.
   if (cur_dw_timestamp < _last_dw_timestamp)
      _dw_timestamp_overflow += 0x10000000000ULL;
   _last_dw_timestamp = cur_dw_timestamp;

   return _dw_timestamp_overflow + cur_dw_timestamp;
}

uint64_t dw1000_readtxtimestamp(void)
{
   uint64_t cur_dw_timestamp = 0;
   dwt_readtxtimestamp((uint8_t*)&cur_dw_timestamp);

   // Check to see if an overflow has occurred.
   if (cur_dw_timestamp < _last_dw_timestamp)
      _dw_timestamp_overflow += 0x10000000000ULL;
   _last_dw_timestamp = cur_dw_timestamp;

   return _dw_timestamp_overflow + cur_dw_timestamp;
}

uint64_t dw1000_setdelayedtrxtime(uint32_t delay_time)
{
   // Check to see if an overflow has occurred, but do not save the overflow since this delay time may be in the future
   dwt_setdelayedtrxtime(delay_time);
   uint64_t cur_dw_timestamp = (uint64_t)(delay_time & 0xFFFFFFFE) << 8;
   if (cur_dw_timestamp < _last_dw_timestamp)
      cur_dw_timestamp += 0x10000000000ULL;
   return _dw_timestamp_overflow + cur_dw_timestamp;
}

uint64_t dw1000_gettimestampoverflow(void)
{
   return _dw_timestamp_overflow;
}

void dw1000_calculatediagnostics(void)
{

   // Estimate First path and Rx signal strength; see User Manual chapter 4.7
   //uint32_t A_16MHz = 11377 / 100;
   //uint32_t A_64MHz = 12174 / 100;

   // Get data
   dwt_rxdiag_t diagnostics = { 0 };
   dwt_readdiagnostics(&diagnostics);

   // Print variables
   /*debug_msg("DEBUG: Printing diagnostics values:\n");
    debug_msg_uint(diagnostics.firstPathAmp1);
    debug_msg("\n");
    debug_msg_uint(diagnostics.firstPathAmp2);
    debug_msg("\n");
    debug_msg_uint(diagnostics.firstPathAmp3);
    debug_msg("\n");
    debug_msg_uint(diagnostics.rxPreamCount);
    debug_msg("\n");
    debug_msg_uint(diagnostics.maxGrowthCIR);
    debug_msg("\n");*/

   uint32_t N_squared = (uint32_t) diagnostics.rxPreamCount * (uint32_t) diagnostics.rxPreamCount;

   // First path signal (leading edge -> LOS path)
   uint32_t fp_signal = (uint32_t) diagnostics.firstPathAmp1 * (uint32_t) diagnostics.firstPathAmp1;
   fp_signal += (uint32_t) diagnostics.firstPathAmp2 * (uint32_t) diagnostics.firstPathAmp2;
   fp_signal += (uint32_t) diagnostics.firstPathAmp3 * (uint32_t) diagnostics.firstPathAmp3;

   // Rx signal
   uint32_t rx_signal = (uint32_t) diagnostics.maxGrowthCIR;
   rx_signal *= (uint32_t) (0x1 << 17);

   // Calculate dBm
   //int fp_signal_dBm = (uint32_t)(10 * log10((double)fp_signal / N_squared)) - A_64MHz;
   //int rx_signal_dBm = (uint32_t)(10 * log10((double)rx_signal / N_squared)) - A_64MHz;

   debug_msg("DEBUG: Estimated signal strength: First path - ");
   debug_msg_uint(fp_signal / N_squared);
   //debug_msg_int(fp_signal_dBm);
   debug_msg(" ; Rx - ");
   debug_msg_uint(rx_signal / N_squared);
   //debug_msg_int(rx_signal_dBm);
   debug_msg("\n");
}

void dw1000_printCalibrationValues(void)
{

   const uint8_t nr_channels = 3;
   const uint8_t nr_antennas = 3;
   uint8_t i, j;

   debug_msg("DEBUG: Calibration values are:\n");
   for (i = 0; i < nr_channels; i++)
   {
      debug_msg("DEBUG: ");
      for (j = 0; j < nr_antennas; j++)
      {
         debug_msg_int(_prog_values.calibration_values[i][j]);
         debug_msg("\t\t");
      }
      debug_msg("\n");
   }
}
