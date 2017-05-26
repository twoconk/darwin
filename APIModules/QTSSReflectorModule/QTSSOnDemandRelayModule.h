#ifndef _QTSSONDEMANDRELAYMODULE_H_
#define _QTSSONDEMANDRELAYMODULE_H_

#include "QTSS.h"

#include <libxml/xpath.h>
#include <libxml/parser.h>
#include <libxml/xmlmemory.h>
#include <libxml/xmlstring.h>
#include <stdlib.h>
#include <string.h>
#include "SVector.h"

extern "C"
{
    EXPORT QTSS_Error QTSSOnDemandRelayModule_Main(void* inPrivateArgs);
}

#define URL_MAX_LENGTH 255
#define GEN_LENGTH 20

typedef struct 
{
	UInt32 m_nPort;
	char m_szIP[GEN_LENGTH];
	char m_szSourceUrl[URL_MAX_LENGTH];
	char m_szUser[GEN_LENGTH];
	char m_szPassword[GEN_LENGTH];
  char m_szDeviceName[GEN_LENGTH];
} DeviceInfo;


class ParseDevice
{
public:
   ParseDevice(char* inPath);
  
   ~ParseDevice() { if(NULL != pDoc) xmlFreeDoc(pDoc);};

   DeviceInfo* GetDeviceInfoByIdName(const char* name);
   SVector<DeviceInfo*> GetAllDevice();
   
private:
  xmlDocPtr pDoc; 
}; 

#endif //_QTSSONDEMANDRELAYMODULE_H_
