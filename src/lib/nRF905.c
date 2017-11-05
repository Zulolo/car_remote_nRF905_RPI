/*
 * nRF905.c
 *
 *  Created on: Oct 17, 2017
 *      Author: zulolo
 *      Description: Use wiringPi ISR and SPI feature to control nRF905
 */
//#include <stdint.h>
#include <sys/time.h>       /* for setitimer */
#include <signal.h>     /* for signal */
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "wiringPi.h"
#include "wiringPiSPI.h"
#include "system.h"
#include "nRF905.h"

#define NRF905_TX_EN_PIN				0
#define NRF905_TRX_CE_PIN				2
#define NRF905_PWR_UP_PIN				3
#define NRF905_DR_PIN					4

#define NRF905_TX_ADDR_LEN				4
#define NRF905_RX_ADDR_LEN				4
#define NRF905_RX_PAYLOAD_LEN			32
#define NRF905_TX_PAYLOAD_LEN			NRF905_RX_PAYLOAD_LEN
#define TEST_NRF905_TX_ADDR				0x87654321
#define TEST_NRF905_RX_ADDR				0x12345678

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
#define NRF905_CMD_WC(unWR_CFG_ByteIndex)		((unWR_CFG_ByteIndex) & NRF905_CMD_WC_MASK)	// Write Configuration register
#define NRF905_CMD_RC_MASK						0x0F
#define NRF905_CMD_RC(unRD_CFG_ByteIndex)		(((unRD_CFG_ByteIndex) & NRF905_CMD_RC_MASK) | 0x10)	// Read Configuration register
#define NRF905_CMD_WTP							0x20
#define NRF905_CMD_RTP							0x21
#define NRF905_CMD_WTA							0x22
#define NRF905_CMD_RTA							0x23
#define NRF905_CMD_RRP							0x24
#define NRF905_CMD_CC(unPwrChn)					((unPwrChn) | 0x8000)
#define CH_MSK_IN_CC_REG						0x01FF
#define NRF905_DR_IN_STATUS_REG(status)			((status) & (0x01 << 5))

#define NRF905STATUS_LOCK						111

typedef enum _nRF905Modes {
	NRF905_MODE_PWR_DOWN = 0,
	NRF905_MODE_STD_BY,
	NRF905_MODE_BURST_RX,
	NRF905_MODE_BURST_TX,
	NRF905_MODE_MAX
} nRF905Mode_t;

typedef struct _nRF905PinLevelInMode {
	int nPWR_UP_PIN;
	int nTRX_CE_PIN;
	int nTX_EN_PIN;
} nRF905PinLevelInMode_t;

// Pin status according to each nRF905 mode
static const nRF905PinLevelInMode_t unNRF905MODE_PIN_LEVEL[] = {
		{ LOW, LOW, LOW},
		{ HIGH, LOW, LOW },
		{ HIGH, HIGH, LOW },
		{ HIGH, HIGH, HIGH } };

typedef struct _nRF905Status {
	unsigned int unNRF905RecvFrameCNT;
	unsigned int unNRF905SendFrameCNT;
	unsigned int unNRF905HoppingCNT;
	unsigned int unNRF905TxAddr;
	unsigned int unNRF905RxAddr;
	unsigned short int unNRF905CHN_PWR;
	nRF905Mode_t tNRF905CurrentMode;
}nRF905Status_t;

static nRF905Status_t tNRF905Status = {0, 0, 0, 0, 0, NRF905_MODE_PWR_DOWN};

// MSB of CH_NO will always be 0
static const unsigned char NRF905_CR_DEFAULT[] = { 0x4C, 0x0C, // F=(422.4+(0x6C<<1)/10)*1; No retransmission; +6db; NOT reduce receive power
		(NRF905_RX_ADDR_LEN << 4) | NRF905_TX_ADDR_LEN,	// 4 bytes RX & TX address;
		NRF905_RX_PAYLOAD_LEN, NRF905_TX_PAYLOAD_LEN, // 16 bytes RX & TX package length;
		0x00, 0x0C, 0x40, 0x08,	// RX address is the calculation result of CH_NO
		0x58 };	// 16MHz crystal; enable CRC; CRC16

static int nRF905SPI_CHN = 0;
static int nRF905PipeFd[2];
static const unsigned short int* pRoamingTable;
static int nRoamingTableLen;
static unsigned char unNRF905Power;

static int setNRF905Mode(nRF905Mode_t tNRF905Mode) {
	if (tNRF905Mode >= NRF905_MODE_MAX){
		NRF905_LOG_ERR("nRF905 Mode error.");
		return (-1);
	}
	if (tNRF905Mode == tNRF905Status.tNRF905CurrentMode){
		NRF905_LOG_INFO("nRF905 Mode not changed, no need to set pin.");
		return 0;
	}

	digitalWrite(NRF905_TX_EN_PIN, unNRF905MODE_PIN_LEVEL[tNRF905Mode].nTX_EN_PIN);
	digitalWrite(NRF905_TRX_CE_PIN, unNRF905MODE_PIN_LEVEL[tNRF905Mode].nTRX_CE_PIN);
	digitalWrite(NRF905_PWR_UP_PIN, unNRF905MODE_PIN_LEVEL[tNRF905Mode].nPWR_UP_PIN);
	tNRF905Status.tNRF905CurrentMode = tNRF905Mode;

	return 0;
}

// No set mode in low level APIs because if you set Standby mode
// then after write SPI you don't know what to change back
static int nRF905SPI_WR_CMD(unsigned char unCMD, const unsigned char *pData, int nDataLen) {
	int nResult;
	nRF905Mode_t tPreMode;
	unsigned char* pBuff;
	if (nDataLen > 0) {
		pBuff = malloc(nDataLen + 1);
		if (pBuff) {
			pBuff[0] = unCMD;
			memcpy(pBuff + 1, pData, nDataLen);
			tPreMode = tNRF905Status.tNRF905CurrentMode;
			setNRF905Mode(NRF905_MODE_STD_BY);
			nResult = wiringPiSPIDataRW(nRF905SPI_CHN, pBuff, nDataLen + 1);
			setNRF905Mode(tPreMode);
			free(pBuff);
			return nResult;
		} else {
			return (-1);
		}
	} else {
		return (-1);
	}
}

static int readStatusReg(void) {
	int nResult;
	nRF905Mode_t tPreMode;
	unsigned char unStatus;
	unStatus = NRF905_CMD_RC(1);
	tPreMode = tNRF905Status.tNRF905CurrentMode;
	setNRF905Mode(NRF905_MODE_STD_BY);
	nResult = wiringPiSPIDataRW(nRF905SPI_CHN, &unStatus, 1);
	setNRF905Mode(tPreMode);
	if (0 == nResult) {
		return unStatus;
	} else {
		return (-1);
	}
}

static int readRxPayload(unsigned char* pBuff, int nBuffLen) {
	int nResult;
	unsigned char unReadBuff[33];
	nRF905Mode_t tPreMode;
	if ((nBuffLen > 0) && (nBuffLen < sizeof(unReadBuff))) {
		unReadBuff[0] = NRF905_CMD_RTP;
		tPreMode = tNRF905Status.tNRF905CurrentMode;
		setNRF905Mode(NRF905_MODE_STD_BY);
		nResult = wiringPiSPIDataRW(nRF905SPI_CHN, unReadBuff, nBuffLen + 1);
		setNRF905Mode(tPreMode);
		memcpy(pBuff, unReadBuff + 1, nBuffLen);
		return nResult;
	} else {
		return (-1);
	}
}

int readConfig(unsigned char unConfigAddr, unsigned char* pBuff, int nBuffLen) {
	int nResult;
	nRF905Mode_t tPreMode;
	unsigned char unReadBuff[33];
	if ((nBuffLen > 0) && (nBuffLen < sizeof(unReadBuff))) {
		unReadBuff[0] = NRF905_CMD_RC(unConfigAddr);
		tPreMode = tNRF905Status.tNRF905CurrentMode;
		setNRF905Mode(NRF905_MODE_STD_BY);
		nResult = wiringPiSPIDataRW(nRF905SPI_CHN, unReadBuff, nBuffLen + 1);
		setNRF905Mode(tPreMode);
		memcpy(pBuff, unReadBuff + 1, nBuffLen);
		return nResult;
	} else {
		return (-1);
	}
}

static int writeConfig(unsigned char unConfigAddr, const unsigned char* pBuff, int nBuffLen) {
	return nRF905SPI_WR_CMD(NRF905_CMD_WC(unConfigAddr), pBuff, nBuffLen);
}

static int writeTxAddr(unsigned int unTxAddr) {
	return nRF905SPI_WR_CMD(NRF905_CMD_WTA, (unsigned char*)(&unTxAddr), 4);
}

static int writeRxAddr(unsigned int unRxAddr) {
	return writeConfig(NRF905_RX_ADDRESS_IN_CR, (unsigned char*)(&unRxAddr), 4);
}

// TX and RX address are already configured during hopping
static int writeTxPayload(unsigned char* pBuff, int nBuffLen) {
	return writeConfig(NRF905_CMD_WTP, pBuff, nBuffLen);
}

static int writeFastConfig(unsigned short int unPA_PLL_CHN) {
	int nResult;
	nRF905Mode_t tPreMode;
	tPreMode = tNRF905Status.tNRF905CurrentMode;
	setNRF905Mode(NRF905_MODE_STD_BY);
	nResult = wiringPiSPIDataRW(nRF905SPI_CHN, (unsigned char *)(&unPA_PLL_CHN), 2);
	setNRF905Mode(tPreMode);
	return nResult;
}

static int setChannelMonitorTimer(void) {
	return 0;
//	struct itimerval tChannelMonitorTimer;  /* for setting itimer */
//	tChannelMonitorTimer.it_value.tv_sec = 1;
//	tChannelMonitorTimer.it_value.tv_usec = 0;
//	tChannelMonitorTimer.it_interval.tv_sec = 1;
//	tChannelMonitorTimer.it_interval.tv_usec = 0;
//	return setitimer(ITIMER_REAL, &tChannelMonitorTimer, NULL);
}

static void dataReadyHandler(void) {
	static unsigned char unReadBuff[32];
	static int nStatusReg;

	piLock(NRF905STATUS_LOCK);
	if (NRF905_MODE_BURST_RX == tNRF905Status.tNRF905CurrentMode) {
		piUnlock(NRF905STATUS_LOCK);
		setNRF905Mode(NRF905_MODE_STD_BY);
		// make sure DR was set
		nStatusReg = readStatusReg();
		if ((nStatusReg >= 0) && (NRF905_DR_IN_STATUS_REG(nStatusReg) == 0)) {
			// Strange happens, do something?
			NRF905_LOG_ERR("Strange happens. DR pin set but status register not.");
		} else {
			printf("Data ready rising edge detected.\n");
			// reset monitor timer since communication seems OK
	//		setChannelMonitorTimer();
	//		piLock(NRF905STATUS_LOCK);
	//		tNRF905Status.unNRF905RecvFrameCNT++;
	//		piUnlock(NRF905STATUS_LOCK);
			readRxPayload(unReadBuff, sizeof(unReadBuff));
			printf("New frame received: 0x%02X 0x%02X.\n", unReadBuff[0], unReadBuff[1]);
	//		if (write(nRF905PipeFd[1], unReadBuff, sizeof(unReadBuff)) != sizeof(unReadBuff)) {
	//			NRF905_LOG_ERR("Write nRF905 pipe error");
	//		}
		}
		setNRF905Mode(NRF905_MODE_BURST_RX);
	} else if (NRF905_MODE_BURST_TX == tNRF905Status.tNRF905CurrentMode) {
		piUnlock(NRF905STATUS_LOCK);
		setNRF905Mode(NRF905_MODE_BURST_RX);
	} else {
		piUnlock(NRF905STATUS_LOCK);
		NRF905_LOG_ERR("Data ready pin was set but status is neither TX nor RX.");
	}
}

static int regDR_Event(void) {
	return wiringPiISR (NRF905_DR_PIN, INT_EDGE_RISING, &dataReadyHandler) ;
}

static int nRF905CRInitial(int nRF905SPI_Fd) {
	return writeConfig(0, NRF905_CR_DEFAULT, sizeof(NRF905_CR_DEFAULT));
}

static unsigned int getTxAddrFromChnPwr(unsigned int unNRF905CHN_PWR) {
	return ((unNRF905CHN_PWR | (unNRF905CHN_PWR << 16)) & 0xA33D59AA);
}

static unsigned int getRxAddrFromChnPwr(unsigned short int unNRF905CHN_PWR) {
	return ((unNRF905CHN_PWR | (unNRF905CHN_PWR << 16)) & 0x5CA259AA);
}

#define GET_CHN_PWR_FAST_CONFIG(x, y) 		((x) | ((y) << 10))

static void roamNRF905(void) {
	static int nHoppingPoint = 0;

	setNRF905Mode(NRF905_MODE_STD_BY);
	piLock(NRF905STATUS_LOCK);
	tNRF905Status.unNRF905CHN_PWR = GET_CHN_PWR_FAST_CONFIG(pRoamingTable[nHoppingPoint], unNRF905Power);
	(nHoppingPoint < (nRoamingTableLen - 1)) ? (nHoppingPoint++):(nHoppingPoint = 0);
	tNRF905Status.unNRF905HoppingCNT++;
	tNRF905Status.unNRF905TxAddr = getTxAddrFromChnPwr(tNRF905Status.unNRF905CHN_PWR);
	tNRF905Status.unNRF905RxAddr = getRxAddrFromChnPwr(tNRF905Status.unNRF905CHN_PWR);
	piUnlock(NRF905STATUS_LOCK);
	writeFastConfig(tNRF905Status.unNRF905CHN_PWR);
	writeTxAddr(tNRF905Status.unNRF905TxAddr);
	writeRxAddr(tNRF905Status.unNRF905RxAddr);
	setNRF905Mode(NRF905_MODE_BURST_RX);
}

int nRF905Initial(int nSPI_Channel, int nSPI_Speed, unsigned char unPower) {
	int nRF905SPI_Fd;
	wiringPiSetup();
	(void)piHiPri(10);

	unNRF905Power = unPower;
	pinMode(NRF905_TX_EN_PIN, OUTPUT);
	pinMode(NRF905_TRX_CE_PIN, OUTPUT);
	pinMode(NRF905_PWR_UP_PIN, OUTPUT);
	pinMode(NRF905_DR_PIN, INPUT);
	setNRF905Mode(NRF905_MODE_STD_BY);
	nRF905SPI_Fd = wiringPiSPISetup(nSPI_Channel, nSPI_Speed);
	if (nRF905SPI_Fd < 0) {
		NRF905_LOG_ERR("nRF905 SPI initial error.");
		return (-1);
	}
	usleep(3000);
	nRF905SPI_CHN = nSPI_Channel;
	nRF905CRInitial(nRF905SPI_Fd);

	return 0;
}

int nRF905StartListen(const unsigned short int* pHoppingTable, int nTableLen) {
	// generate pip
	if (nRF905PipeFd[0] != 0) {
		close(nRF905PipeFd[0]);
	}
	if (nRF905PipeFd[1] != 0) {
		close(nRF905PipeFd[1]);
	}
	if (pipe(nRF905PipeFd) != 0) {
		NRF905_LOG_ERR("nRF905 pipe initial error.");
		return (-1);
	}
	pRoamingTable = pHoppingTable;
	nRoamingTableLen = nTableLen;

	// For test only, fix channel, TX and RX address, no hopping
	writeTxAddr(TEST_NRF905_TX_ADDR);
	writeRxAddr(TEST_NRF905_RX_ADDR);

	// Set nRF905 in RX mode
	setNRF905Mode(NRF905_MODE_BURST_RX);
	// register ISR to handle data receive when DR rise edge
	regDR_Event();

	// start timer to watch communication, if do RX during 1s, start hopping
//	if (signal(SIGALRM, (void (*)(int)) roamNRF905) == SIG_ERR) {
//		NRF905_LOG_ERR("Unable to catch SIGALRM");
//		close(nRF905PipeFd[0]);
//		close(nRF905PipeFd[1]);
//		return (-1);
//	}
//	raise(SIGALRM);

	if (setChannelMonitorTimer() != 0) {
		NRF905_LOG_ERR("error calling setitimer()");
		close(nRF905PipeFd[0]);
		close(nRF905PipeFd[1]);
		return (-1);
	}

	return 0;
}

void setNRF905Power(unsigned char unPower) {
	unNRF905Power = unPower;
}

// Block operation until there is any data in the pipe which was written in Data ready ISR
int nRF905ReadFrame(unsigned char* pReadBuff, int nBuffLen) {
	return read(nRF905PipeFd[0], pReadBuff, nBuffLen);
}

int nRF905SendFrame(unsigned char* pReadBuff, int nBuffLen) {
	setNRF905Mode(NRF905_MODE_STD_BY);
	writeTxPayload(pReadBuff, nBuffLen);
	setNRF905Mode(NRF905_MODE_BURST_TX);
	// TODO: Better to add timeout here in case DR will not be set.
	// My main task is to receive!
	piLock(NRF905STATUS_LOCK);
	tNRF905Status.unNRF905SendFrameCNT++;
	piUnlock(NRF905STATUS_LOCK);
	return 0;
}

int nRF905StopListen(void) {
	setNRF905Mode(NRF905_MODE_STD_BY);
	close(nRF905PipeFd[0]);
	close(nRF905PipeFd[1]);
	return 0;
}

unsigned int getNRF905StatusRecvFrameCNT(void) {
	unsigned int unNRF905RecvFrameCNT;
	piLock(NRF905STATUS_LOCK);
	unNRF905RecvFrameCNT = tNRF905Status.unNRF905RecvFrameCNT;
	piUnlock(NRF905STATUS_LOCK);
	return unNRF905RecvFrameCNT;
}

unsigned int getNRF905StatusSendFrameCNT(void) {
	unsigned int unNRF905SendFrameCNT;
	piLock(NRF905STATUS_LOCK);
	unNRF905SendFrameCNT = tNRF905Status.unNRF905SendFrameCNT;
	piUnlock(NRF905STATUS_LOCK);
	return unNRF905SendFrameCNT;
}

unsigned int getNRF905StatusHoppingCNT(void) {
	unsigned int unNRF905HoppingCNT;
	piLock(NRF905STATUS_LOCK);
	unNRF905HoppingCNT = tNRF905Status.unNRF905HoppingCNT;
	piUnlock(NRF905STATUS_LOCK);
	return unNRF905HoppingCNT;
}
