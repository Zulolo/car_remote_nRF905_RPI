/*
 * nRF905.c
 *
 *  Created on: Oct 17, 2017
 *      Author: zulolo
 *      Description: Use wiringPi ISR and SPI feature to control nRF905
 */
#include "wiringPi.h"
#include "wiringPiSPI.h"

#define NRF905_TX_EN_PIN				17
#define NRF905_TRX_CE_PIN				18
#define NRF905_PWR_UP_PIN				27

#define NRF905_CD_PIN					22
#define NRF905_AM_PIN					23
#define NRF905_DR_PIN					24

#define US_PER_SECONDE					1000000
#define NRF905_TX_ADDR_LEN				4
#define NRF905_RX_ADDR_LEN				4
#define NRF905_RX_PAYLOAD_LEN			16
#define NRF905_TX_PAYLOAD_LEN			NRF905_RX_PAYLOAD_LEN

typedef enum _nRF905Boolean {
	NRF905_FALSE = 0,
	NRF905_TRUE = !NRF905_FALSE
}nRF905Boolean_t;

typedef enum _RF_CMD {
	RF_READ_SENSOR_VALUE = 0,
	RF_WRITE_MOTOR_PAR,
	RF_CMD_FAILED
}RF_Command_t;

typedef enum _nRF905CommType {
	NRF905_COMM_TYPE_RX_PKG = 0,
	NRF905_COMM_TYPE_TX_PKG
}nRF905CommType_t;

typedef struct _CommTask {
	nRF905CommType_t tCommType;
	uint8_t unCommByteNum;
	uint8_t* pTX_Frame;
	uint8_t* pRX_Frame;
}nRF905CommTask_t;

/* Here is how the RF works:
 * UP keeps monitoring if there is any valid frame available (CD, AM, DR should be all SET) on certain channel for 300ms.
 * If yes, receive frame and continuously monitoring on this channel. If no, hop to next channel according to the hopping table.
 *
 * Down keeps transmitting frames every 100ms. If transmitting failed, (can not get valid response) down start hopping procedure.
 * Hopping procedure is to trying burst transmitting ACK frame continuously.
 * If transmitting failed, jump to next channel according to table and start to transmit again.
 * The TX&RX address is generated by some algorithm and is set at start up or during hopping
 *
 */
#define NRF905_RX_ADDRESS_IN_CR					5
#define NRF905_CMD_WC_MASK						0x0F
#define NRF905_CMD_WC(unWR_CFG_ByteIndex)		((unWR_CFG_ByteIndex) & NRF905_CMD_WC_MASK)
#define NRF905_CMD_RC_MASK						0x0F
#define NRF905_CMD_RC(unRD_CFG_ByteIndex)		(((unRD_CFG_ByteIndex) & NRF905_CMD_RC_MASK) | 0x10)
#define NRF905_CMD_WTP							0x20
#define NRF905_CMD_RTP							0x21
#define NRF905_CMD_WTA							0x22
#define NRF905_CMD_RTA							0x23
#define NRF905_CMD_RRP							0x24
#define NRF905_CMD_CC(unPwrChn)					((unPwrChn) | 0x1000)
#define CH_MSK_IN_CC_REG						0x01FF

#define HOPPING_MAX_CD_RETRY_NUM				100
#define HOPPING_MAX_RETRY_NUM					HOPPING_MAX_CD_RETRY_NUM
#define EXEC_TSK_MAX_CD_RETRY_NUM				HOPPING_MAX_CD_RETRY_NUM
#define CD_RETRY_DELAY_US						50
#define HOPPING_TX_RETRY_DELAY_US				2000

#define AFTER_SET_BURST_TX_MAX_DELAY_US			30000
//	#define AFTER_SET_BURST_RX_MAX_CD_DELAY_US		50000
#define AFTER_RX_MODE_MAX_DR_DELAY_US			80000	// Since there maybe interference in tha air, the CD may be set very soon
//	#define AFTER_AM_MAX_DR_DELAY_US				80000

#define NRF905_MAX_COMM_ERR_BEFORE_HOPPING		20
#define MAX_CONNECTION_PENDING 					8    /* Max connection requests */
#define PID_EMPTY								0
#define RECEIVE_BUFFER_LENGTH					256
#define ACK_TASK_INTERVAL_US					200000

typedef enum _nRF905Modes {
	NRF905_MODE_PWR_DOWN = 0,
	NRF905_MODE_STD_BY,
	NRF905_MODE_RD_RX,
	NRF905_MODE_BURST_RX,
	NRF905_MODE_BURST_TX,
	NRF905_MODE_MAX
} nRF905Mode_t;

typedef struct _NRF905CommThreadPara {
	int nTaskReadPipe;
	int nBeforeIsRF905SPI_Fd_NowDoNotUse;
} nRF905ThreadPara_t;

// MSB of CH_NO will always be 0
static uint8_t NRF905_CR_DEFAULT[] = { 0x4C, 0x0C, // F=(422.4+(0x6C<<1)/10)*1; No retransmission; +6db; NOT reduce receive power
		(NRF905_RX_ADDR_LEN << 4) | NRF905_TX_ADDR_LEN,	// 4 bytes RX & TX address;
		NRF905_RX_PAYLOAD_LEN, NRF905_TX_PAYLOAD_LEN, // 16 bytes RX & TX package length;
		0x00, 0x0C, 0x40, 0x08,	// RX address is the calculation result of CH_NO
		0x58 };	// 16MHz crystal; enable CRC; CRC16

enum _nRF905PinPosInModeLevel {
	NRF905_PWR_UP_PIN_POS = 0, NRF905_TRX_CE_PIN_POS, NRF905_TX_EN_PIN_POS, NRF905_TX_POS_MAX
};

static const uint8_t unNRF905MODE_PIN_LEVEL[][NRF905_TX_POS_MAX] = {
		{ GPIO_LEVEL_LOW, GPIO_LEVEL_LOW, GPIO_LEVEL_LOW },
		{ GPIO_LEVEL_HIGH, GPIO_LEVEL_LOW, GPIO_LEVEL_LOW },
		{ GPIO_LEVEL_HIGH, GPIO_LEVEL_LOW, GPIO_LEVEL_LOW },
		{ GPIO_LEVEL_HIGH, GPIO_LEVEL_HIGH, GPIO_LEVEL_LOW },
		{ GPIO_LEVEL_HIGH, GPIO_LEVEL_HIGH, GPIO_LEVEL_HIGH } };

static uint8_t unSPI_Mode = SPI_MODE_0;
static uint8_t unSPI_Bits = 8;
static uint32_t unSPI_Speed = 5000000;
static uint16_t unSPI_Delay = 0;
uint8_t unNeedtoClose = NRF905_FALSE;

static int nRF905SPI_CHN = 0;

static int nRF905SPI_DataWR(unsigned char *pData, int nDataLen) {
	return wiringPiSPIDataRW(nRF905SPI_CHN, pData, nDataLen);
}

static int nRF905CRInitial(int nRF905SPI_Fd) {

}

int nRF905Initial(int nSPI_Channel, int nSPI_Speed) {
	int nRF905SPI_Fd = wiringPiSPISetup(nSPI_Channel, nSPI_Speed);
	nRF905SPI_CHN = nSPI_Channel;
	nRF905CRInitial(nRF905SPI_Fd);

	return 0;
}
