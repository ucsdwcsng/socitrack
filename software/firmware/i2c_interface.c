
#include "stm32f0xx_i2c_cpal.h"

#include "board.h"
#include "firmware.h"
#include "i2c_interface.h"

#define BUFFER_SIZE 128
uint8_t rxBuffer[BUFFER_SIZE];
uint8_t txBuffer[BUFFER_SIZE];


// Save a callback to use when data comes in from i2c for the main application
static i2c_interface_callback callback;


/* CPAL local transfer structures */
CPAL_TransferTypeDef rxStructure;
CPAL_TransferTypeDef txStructure;


uint32_t i2c_interface_init (i2c_interface_callback cb) {

	// Save callback
	callback = cb;

	// Enabled the Interrupt pin
	GPIO_InitTypeDef  GPIO_InitStructure;
	RCC_AHBPeriphClockCmd(INTERRUPT_CLK, ENABLE);

	GPIO_InitStructure.GPIO_Pin = INTERRUPT_PIN;
	GPIO_InitStructure.GPIO_Mode = GPIO_Mode_OUT;
	GPIO_InitStructure.GPIO_OType = GPIO_OType_PP;
	GPIO_InitStructure.GPIO_Speed = GPIO_Speed_50MHz;
	GPIO_InitStructure.GPIO_PuPd = GPIO_PuPd_NOPULL;
	GPIO_Init(INTERRUPT_PORT, &GPIO_InitStructure);
	INTERRUPT_PORT->BRR = INTERRUPT_PIN; // clear


	/* Start CPAL communication configuration ***********************************/
	/* Initialize local Reception structures */
	rxStructure.wNumData = BUFFER_SIZE;   /* Maximum Number of data to be received */
	rxStructure.pbBuffer = rxBuffer;      /* Common Rx buffer for all received data */
	rxStructure.wAddr1 = 0;               /* Not needed */
	rxStructure.wAddr2 = 0;               /* Not needed */

	/* Initialize local Transmission structures */
	txStructure.wNumData = BUFFER_SIZE;   /* Maximum Number of data to be received */
	txStructure.pbBuffer = txBuffer;      /* Common Rx buffer for all received data */
	txStructure.wAddr1 = I2C_OWN_ADDRESS; /* The own board address */
	txStructure.wAddr2 = 0;               /* Not needed */

	/* Set SYSCLK as I2C clock source */
	RCC_I2CCLKConfig(RCC_I2C1CLK_SYSCLK);

	/* Configure the device structure */
	CPAL_I2C_StructInit(&I2C1_DevStructure);      /* Set all fields to default values */
	I2C1_DevStructure.CPAL_Dev = 0;
	I2C1_DevStructure.CPAL_Mode = CPAL_MODE_SLAVE;
	I2C1_DevStructure.wCPAL_Options =  CPAL_OPT_NO_MEM_ADDR | CPAL_OPT_DMATX_TCIT | CPAL_OPT_DMARX_TCIT;
	I2C1_DevStructure.CPAL_ProgModel = CPAL_PROGMODEL_INTERRUPT;
	I2C1_DevStructure.pCPAL_I2C_Struct->I2C_Timing = I2C_TIMING;
	I2C1_DevStructure.pCPAL_I2C_Struct->I2C_OwnAddress1 = I2C_OWN_ADDRESS;
	I2C1_DevStructure.pCPAL_TransferRx = &rxStructure;
	I2C1_DevStructure.pCPAL_TransferTx = &txStructure;

	/* Initialize CPAL device with the selected parameters */
	return CPAL_I2C_Init(&I2C1_DevStructure);

}

uint32_t i2c_interface_listen () {
	uint32_t ret;

	// Setup the listen buffer
	rxStructure.wNumData = BUFFER_SIZE;      // Maximum Number of data to be received
	rxStructure.pbBuffer = txBuffer;         // Common Rx buffer for all received data

	// Reconfigure device for slave receiver mode
	I2C1_DevStructure.CPAL_Mode = CPAL_MODE_SLAVE;
	I2C1_DevStructure.CPAL_State = CPAL_STATE_READY;

	// Start waiting for data to be received in slave mode
	ret = CPAL_I2C_Read(&I2C1_DevStructure);
	return ret;
}


uint32_t i2c_interface_send (uint16_t address, uint8_t length, uint8_t* buf) {
	uint32_t ret;

	rxStructure.wNumData = BUFFER_SIZE;       /* Maximum Number of data to be received */
	rxStructure.pbBuffer = rxBuffer;        /* Common Rx buffer for all received data */

	/* Reconfigure device for slave receiver mode */
	I2C1_DevStructure.CPAL_Mode = CPAL_MODE_SLAVE;
	I2C1_DevStructure.CPAL_State = CPAL_STATE_READY;



	// INTERRUPT_PORT->BSRR = INTERRUPT_PIN; // set

	/* Start waiting for data to be received in slave mode */
	ret = CPAL_I2C_Read(&I2C1_DevStructure);

	INTERRUPT_PORT->BSRR = INTERRUPT_PIN; // set

	return ret;
}


// Called when the I2C interface receives a message on the bus
void i2c_interface_rx_fired () {
	// Keep listening
	i2c_interface_listen();
}

// Called after timeout
void i2c_interface_timeout_fired () {

}


/**
  * @brief  User callback that manages the Timeout error
  * @param  pDevInitStruct
  * @retval None.
  */
uint32_t CPAL_TIMEOUT_UserCallback(CPAL_InitTypeDef* pDevInitStruct) {
	// Handle this interrupt on the main thread
	mark_interrupt(INTERRUPT_I2C_TIMEOUT);

  return CPAL_PASS;
}


/**
  * @brief  Manages the End of Rx transfer event.
  * @param  pDevInitStruct
  * @retval None
  */
void CPAL_I2C_RXTC_UserCallback(CPAL_InitTypeDef* pDevInitStruct) {
	// Handle this interrupt on the main thread
	mark_interrupt(INTERRUPT_I2C_RX);
}

/**
  * @brief  User callback that manages the I2C device errors.
  * @note   Make sure that the define USE_SINGLE_ERROR_CALLBACK is uncommented in
  *         the cpal_conf.h file, otherwise this callback will not be functional.
  * @param  pDevInitStruct.
  * @param  DeviceError.
  * @retval None
  */
void CPAL_I2C_ERR_UserCallback(CPAL_DevTypeDef pDevInstance, uint32_t DeviceError) {

}

/**
  * @brief  Manages the End of Tx transfer event.
  * @param  pDevInitStruct
  * @retval None
  */
void CPAL_I2C_TXTC_UserCallback(CPAL_InitTypeDef* pDevInitStruct) {
}


