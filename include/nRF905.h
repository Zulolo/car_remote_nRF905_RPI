#ifndef NRF905_H_
#define NRF905_H_

#include <syslog.h>

#define NRF905D_LOG_ERR(arg...)			openlog("nRF905.D.err", LOG_PID, 0);\
										syslog(LOG_USER | LOG_INFO, arg);\
										closelog()

#define NRF905D_LOG_INFO(arg...)		openlog("nRF905.D.info", LOG_PID, 0);\
										syslog(LOG_USER | LOG_INFO, arg);\
										closelog()


int nRF905Initial(int nSPI_Channel, int nSPI_Speed);

#endif
