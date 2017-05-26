#include "QTSSOnDemandRelayModule.h"
#include "QTSSModuleUtils.h"
#include "ReflectorSession.h"
#include "OSArrayObjectDeleter.h"
#include "QTSS_Private.h"
#include "QTSSMemoryDeleter.h"
#include "OSMemory.h"
#include "OSRef.h"
#include "IdleTask.h"
#include "Task.h"
#include "OS.h"
#include "Socket.h"
#include "SocketUtils.h"
#include "FilePrefsSource.h"
#include "ResizeableStringFormatter.h"
#include "StringParser.h"
#include "QTAccessFile.h"
#include "QTSSModuleUtils.h"
#include "QTSS3GPPModuleUtils.h"

//ReflectorOutput objects
#include "RTPSessionOutput.h"

//SourceInfo objects
#include "SDPSourceInfo.h"

#include "SDPUtils.h"

#ifndef __Win32__
    #include <unistd.h>
#endif

#include <stdio.h>
#include "ClientSession.h"

static StrPtrLen  sSDPSuffix(".sdp");

static ClientSession::ClientType theClientType = ClientSession::kRTSPUDPClientType; //kRTSPTCPClientType;
static UInt32 theMovieLength = 600000;
static Bool16 theMovieLengthIsSpecified = false;
static UInt32 theHTTPCookie = 1;

static Bool16 appendJunk = false;
static UInt32 theReadInterval = 50;
static UInt32 sockRcvBuf = 32768;
static Float32 lateTolerance = 0;
static char* rtpMetaInfo = NULL;
static Float32 speed = 1;
static UInt32 verboseLevel = 4;
static char* packetPlayHeader = NULL;
static UInt32 overbufferwindowInK = 0;
static Bool16 randomThumb = false;
static Bool16 sendOptions = false; 
static Bool16 requestRandomData = false; 
static SInt32 randomDataSize = 0;
static UInt32 rtcpInterval = 5000;
static UInt32 bandwidth = 0;
static UInt32 guarenteedBitRate = 0;
static UInt32 maxBitRate = 0;
static UInt32 maxTransferDelay = 0;
static Bool16 enableForcePlayoutDelay = false;
static UInt32 playoutDelay = 0;
static UInt32 bufferSpace = 100000;
static UInt32 delayTime = 10000;
static Float32 startDelayFrac = 0.5;
static char* controlID = NULL;
static Bool16 sEnable3GPP = false;

static QTSS_AttributeID  sStreamCookieAttr   =   qtssIllegalAttrID;
static QTSS_AttributeID        svOutputAttr  =   qtssIllegalAttrID;
static QTSS_AttributeID        saOutputAttr  =   qtssIllegalAttrID;
static QTSS_AttributeID  sClientDeviceNameAttr =   qtssIllegalAttrID;

static const StrPtrLen  kCacheControlHeader("no-cache");
static char* DeviceFilePath = NULL;
// static SVector<DeviceInfo*>     vDeviceInfo;
static SVector<RTPSessionOutput*>     vVOutput;
static SVector<RTPSessionOutput*>     vAOutput;

static StrPtrLen  sSDPNotValidMessage("describe SDP is not a valid SDP");


static QTSS_ServerObject                sServer         = NULL;
static QTSS_PrefsObject                 sServerPrefs    = NULL;
static QTSS_ModulePrefsObject           sPrefs = NULL;

static OSRefTable*                      sSessionMap     = NULL;
static OSRefTable*                    sClientSessionMap = NULL;
 

// static ParseDevice* parseDevice = NULL;

// FUNC
static QTSS_Error QTSSOnDemandRelayModuleDispatch(QTSS_Role inRole, QTSS_RoleParamPtr inParams);

static QTSS_Error Register(QTSS_Register_Params* inParams);
static QTSS_Error Initialize(QTSS_Initialize_Params* inParams);
static QTSS_Error ProcessRTSPRequest(QTSS_StandardRTSP_Params* inParams);
static QTSS_Error RereadPrefs();
static QTSS_Error ProcessRealyRTPData(QTSS_StandardRTSP_Params* inParams);
static QTSS_Error DestroySession(QTSS_ClientSessionClosing_Params* inParams);
static QTSS_Error Shutdown();

static QTSS_Error DoDescribe(QTSS_StandardRTSP_Params* inParams);
static QTSS_Error DoSetup(QTSS_StandardRTSP_Params* inParams);
static QTSS_Error DoPlay(QTSS_StandardRTSP_Params* inParams);

static QTSS_Error AddRTPStream(ReflectorSession* theSession,QTSS_StandardRTSP_Params* inParams, QTSS_RTPStreamObject *newStreamPtr);

static QTSS_Error CreateClients();

static UInt32 CalcStartTime(Bool16 inRandomThumb, UInt32 inMovieLength);
UInt32 CalcStartTime(Bool16 inRandomThumb, UInt32 inMovieLength)
{
	UInt32 theMovieLength = inMovieLength;
	if (theMovieLength > 1)
		theMovieLength--;
		
	if (inRandomThumb)
		return ::rand() % theMovieLength;
	else
		return 0;
}

ParseDevice::ParseDevice( char* inPath)
{
    pDoc =  xmlReadFile(inPath, "UTF-8", XML_PARSE_RECOVER); //»ñÈ¡XMLÎÄµµµÄÖ¸Õë();
	if(NULL == pDoc)
	{
		printf("xmlParseFile Error in %s %d\n",__FUNCTION__, __LINE__);
	}
}

DeviceInfo* ParseDevice::GetDeviceInfoByIdName(const char* name)
{
	if(NULL == pDoc) return NULL;
	xmlNodePtr pRoot = xmlDocGetRootElement(pDoc);//»ñÈ¡¸ù½Úµã
	if(NULL == pRoot)
	{
		fprintf(stderr, "xmlDocGetRootElement Error in %s %d\n", __FUNCTION__, __LINE__);
		xmlFreeDoc(pDoc);
		return NULL;
	}
	printf("Node name is %s!\n", pRoot->name);

	xmlNodePtr pFirst = pRoot->xmlChildrenNode;//»ñÈ¡×Ó½Úµã
	while(NULL != pFirst)
	{
		if(!xmlStrcmp(pFirst->name, (const xmlChar *)("Device")))
		{
			xmlNodePtr propNodePtr = pFirst;

			xmlAttrPtr attrPtr = propNodePtr->properties;
			while (attrPtr != NULL)
			{
				if (!xmlStrcmp(attrPtr->name, BAD_CAST "name"))
				{
					xmlChar* szAttr = xmlGetProp(propNodePtr,BAD_CAST "name");
					printf("\n name --> %s\n", szAttr);  
					xmlFree(szAttr);
				}
				attrPtr = attrPtr->next;
			}
		}
		pFirst = pFirst->next;
	}
    return NULL;
}

SVector<DeviceInfo*> ParseDevice::GetAllDevice()
{
	SVector<DeviceInfo*> vDeviceInfo;
	
	// if(NULL == pDoc) return NULL;
	xmlNodePtr pRoot = xmlDocGetRootElement(pDoc);//»ñÈ¡¸ù½Úµã
	if(NULL == pRoot)
	{
		fprintf(stderr, "xmlDocGetRootElement Error in %s %d\n", __FUNCTION__, __LINE__);
		xmlFreeDoc(pDoc);
		return vDeviceInfo;
	}
	// printf("Node name is %s!\n", pRoot->name);

	xmlNodePtr pFirst = pRoot->xmlChildrenNode;//»ñÈ¡×Ó½Úµã
	while(NULL != pFirst)
	{
		if(!xmlStrcmp(pFirst->name, (const xmlChar *)("Device")))
		{
			DeviceInfo* di = new DeviceInfo();
			xmlNodePtr propNodePtr = pFirst;

			xmlAttrPtr attrPtr = propNodePtr->properties;
			while (attrPtr != NULL)
			{	
				if (!xmlStrcmp(attrPtr->name, BAD_CAST "name"))
				{
					xmlChar* szAttr = xmlGetProp(propNodePtr,BAD_CAST "name");
					printf("name --> %s\n", szAttr); 
					strcpy(di->m_szDeviceName, (char*)szAttr);
					xmlFree(szAttr);
				}
				if (!xmlStrcmp(attrPtr->name, BAD_CAST "url"))
				{
					xmlChar* szAttr = xmlGetProp(propNodePtr,BAD_CAST "url");
					printf("url --> %s\n", szAttr);
					strcpy(di->m_szSourceUrl, (char*)szAttr);
					xmlFree(szAttr);
				}

				if (!xmlStrcmp(attrPtr->name, BAD_CAST "ip"))
				{
					xmlChar* szAttr = xmlGetProp(propNodePtr,BAD_CAST "ip");
					// printf("ip --> %s\n", szAttr); 
					strcpy(di->m_szIP, (char*)szAttr);					
					xmlFree(szAttr);
				}
				if (!xmlStrcmp(attrPtr->name, BAD_CAST "port"))
				{
					xmlChar* szAttr = xmlGetProp(propNodePtr,BAD_CAST "port");
					// printf("port --> %s\n", szAttr);
					di->m_nPort = atoi((char*)szAttr);
					xmlFree(szAttr);
				}

				if (!xmlStrcmp(attrPtr->name, BAD_CAST "user"))
				{
					xmlChar* szAttr = xmlGetProp(propNodePtr,BAD_CAST "user");
					// printf("user --> %s\n", szAttr);
					strcpy(di->m_szUser, (char*)szAttr);					
					xmlFree(szAttr);
				}
				if (!xmlStrcmp(attrPtr->name, BAD_CAST "pwd"))
				{
					xmlChar* szAttr = xmlGetProp(propNodePtr,BAD_CAST "pwd");
					// printf("pwd --> %s\n", szAttr);
					strcpy(di->m_szPassword, (char*)szAttr);
					xmlFree(szAttr);
				}
				attrPtr = attrPtr->next;
			}
			vDeviceInfo.push_back(di);
		}
		pFirst = pFirst->next;
	}
	
	return vDeviceInfo;
}

// FUNCTION IMPLEMENTATIONS
QTSS_Error QTSSOnDemandRelayModule_Main(void* inPrivateArgs)
{
    return _stublibrary_main(inPrivateArgs, QTSSOnDemandRelayModuleDispatch);
}

QTSS_Error  QTSSOnDemandRelayModuleDispatch(QTSS_Role inRole, QTSS_RoleParamPtr inParams)
{
    switch (inRole)
    {
        case QTSS_Register_Role:            
            return Register(&inParams->regParams);
        case QTSS_Initialize_Role:            
            return Initialize(&inParams->initParams);
        case QTSS_RereadPrefs_Role:           
            return RereadPrefs();
        case QTSS_RTSPPreProcessor_Role:
             qtss_printf("QTSSOnDemandRelayModuleDisPathcha  QTSS_RTSPPreProcessor_Role \n" );
             return ProcessRTSPRequest(&inParams->rtspRequestParams);
        case QTSS_RTSPRelayingData_Role:           
            return ProcessRealyRTPData(&inParams->rtspRelayingDataParams);
        case QTSS_ClientSessionClosing_Role:
        	qtss_printf("QTSSOnDemandRelayModuleDisPathcha QTSS_ClientSessionClosing_Role \n" );        
            return DestroySession(&inParams->clientSessionClosingParams);
        case QTSS_Shutdown_Role:
             qtss_printf("QTSSOnDemandRelayModuleDisPathcha   QTSS_Shutdown_Role\n");
            return Shutdown(); 
   }
    return QTSS_NoErr;
}


QTSS_Error Register(QTSS_Register_Params* inParams)
{
    // Do role & attribute setup
    (void)QTSS_AddRole(QTSS_Initialize_Role);  
    (void)QTSS_AddRole(QTSS_RTSPPreProcessor_Role);
    (void)QTSS_AddRole(QTSS_ClientSessionClosing_Role);
    (void)QTSS_AddRole(QTSS_RTSPRelayingData_Role); // call me with interleaved RTP streams on the RTSP session   
    (void)QTSS_AddRole(QTSS_RereadPrefs_Role);  

    static char*        sStreamCookieName   = "QTSSOnDemandRelayModuleStreamCookie";
    static char*        sClientDeviceName= "QTSSOnDemandRelayModuleClientDeviceName";
    static char*        saOutputAttrName= "QTSSOnDemandRelayModuleaOutputAttr";
    static char*        svOutputAttrName= "QTSSOnDemandRelayModulevOutputAttr";
    
    (void)QTSS_AddStaticAttribute(qtssRTPStreamObjectType, sStreamCookieName, NULL, qtssAttrDataTypeVoidPointer);
    (void)QTSS_IDForAttr(qtssRTPStreamObjectType, sStreamCookieName, &sStreamCookieAttr);

	(void)QTSS_AddStaticAttribute(qtssClientSessionObjectType, sClientDeviceName, NULL, qtssAttrDataTypeVoidPointer);
	(void)QTSS_IDForAttr(qtssClientSessionObjectType, sClientDeviceName, &sClientDeviceNameAttr);


    (void)QTSS_AddStaticAttribute(qtssClientSessionObjectType, svOutputAttrName, NULL, qtssAttrDataTypeVoidPointer);
	(void)QTSS_IDForAttr(qtssClientSessionObjectType, svOutputAttrName, &svOutputAttr);

    
    (void)QTSS_AddStaticAttribute(qtssClientSessionObjectType, saOutputAttrName, NULL, qtssAttrDataTypeVoidPointer);
	(void)QTSS_IDForAttr(qtssClientSessionObjectType, saOutputAttrName, &saOutputAttr);
    

    // Reflector session needs to setup some parameters too.
    ReflectorStream::Register();
    // RTPSessionOutput needs to do the same
    RTPSessionOutput::Register();

    // Tell the server our name!
    static char* sModuleName = "QTSSOnDemandRelayModule";
    ::strcpy(inParams->outModuleName, sModuleName);

    return QTSS_NoErr;
}

QTSS_Error Initialize(QTSS_Initialize_Params* inParams)
{
    // Setup module utils
    QTSSModuleUtils::Initialize(inParams->inMessages, inParams->inServer, inParams->inErrorLogStream);
    sSessionMap = NEW OSRefTable();
    sClientSessionMap = NEW OSRefTable();   
    sServerPrefs = inParams->inPrefs;
    sServer = inParams->inServer;
#if QTSS_REFLECTOR_EXTERNAL_MODULE
    // The reflector is dependent on a number of objects in the Common Utilities
    // library that get setup by the server if the reflector is internal to the
    // server.
    //
    // So, if the reflector is being built as a code fragment, it must initialize
    // those pieces itself
#if !MACOSXEVENTQUEUE
    ::select_startevents();//initialize the select() implementation of the event queue
#endif
    OS::Initialize();
    Socket::Initialize();
    SocketUtils::Initialize();

    const UInt32 kNumReflectorThreads = 8;
    TaskThreadPool::AddThreads(kNumReflectorThreads);
    IdleTask::Initialize();
    Socket::StartThread();
#endif
    
    sPrefs = QTSSModuleUtils::GetModulePrefsObject(inParams->inModule);

    // Call helper class initializers
    ReflectorStream::Initialize(sPrefs);
    ReflectorSession::Initialize();
    
    // Report to the server that this module handles DESCRIBE, SETUP, PLAY, PAUSE, and TEARDOWN
	static QTSS_RTSPMethod sSupportedMethods[] = { qtssDescribeMethod, qtssSetupMethod, qtssTeardownMethod, qtssPlayMethod, qtssPauseMethod, qtssAnnounceMethod, qtssRecordMethod };
	QTSSModuleUtils::SetupSupportedMethods(inParams->inServer, sSupportedMethods, 7);

    RereadPrefs();

    CreateClients();
   return QTSS_NoErr;
}

QTSS_Error CreateClients()
{
    ParseDevice* parseDevice = new ParseDevice(DeviceFilePath);
    delete [] DeviceFilePath;

    SVector<DeviceInfo*>  vDeviceInfo = parseDevice->GetAllDevice();

    delete parseDevice;

    printf("## vDeviceInfo.size() =%d  \n", vDeviceInfo.size());
    for(int i=0; i<vDeviceInfo.size();i++)
    {
    	// new a cliSession
    	ClientSession* cliSession  = NEW ClientSession(
									SocketUtils::ConvertStringToAddr(vDeviceInfo[i]->m_szIP),   
									vDeviceInfo[i]->m_nPort,  
                            		vDeviceInfo[i]->m_szSourceUrl,
									theClientType,		// Client type 1
									theMovieLength,		// Movie length
									CalcStartTime(randomThumb, theMovieLength),		// Movie start time
									rtcpInterval,
									0,					// Options interval
									theHTTPCookie++,	// HTTP cookie
									appendJunk,
									theReadInterval,	// Interval between data reads	
									sockRcvBuf,			// socket recv buffer size
									lateTolerance,		// late tolerance
									rtpMetaInfo,
									speed,
									4,                  // log level
									packetPlayHeader,
									overbufferwindowInK,
									sendOptions,        // send options request before the Describe
									requestRandomData,  // request random data in the options request
									randomDataSize, 	// size of the random data to request
									sEnable3GPP,
									guarenteedBitRate,
									maxBitRate,
									maxTransferDelay,
									enableForcePlayoutDelay,
									playoutDelay,
									bandwidth,
									bufferSpace,
									delayTime,
									static_cast<UInt32>(startDelayFrac * delayTime),
									controlID,
									vDeviceInfo[i]->m_szUser,
									vDeviceInfo[i]->m_szPassword,
									vDeviceInfo[i]->m_szDeviceName); 

		// printf("set Key string =%s\n", vDeviceInfo[i]->m_szDeviceName);
		// push into map
		// printf("Key string =%s\n", cliSession->GetRef()->GetString()->Ptr);
		sClientSessionMap->Register(cliSession->GetRef());
		printf("GetNumRefsInTable =%d\n", sClientSessionMap->GetNumRefsInTable());
    }	 

	return QTSS_NoErr;
}

QTSS_Error RereadPrefs()
{
	// char* Url = QTSSModuleUtils::GetStringAttribute(sPrefs, "zfltest", "");
	// printf("####### RereadPrefs Url =%s\n", Url);	

    //ondemandrelay_deviceinfo_xml  
    DeviceFilePath = QTSSModuleUtils::GetStringAttribute(sPrefs, "ondemandrelay_deviceinfo_xml", "");
    printf("RereadPrefs DeviceFilePath =%s\n", DeviceFilePath);
    
    return QTSS_NoErr;
}

QTSS_Error ProcessRTSPRequest(QTSS_StandardRTSP_Params* inParams)
{
    // OSMutexLocker locker (sSessionMap->GetMutex()); //operating on sOutputAttr     

    QTSS_RTSPMethod* theMethod = NULL;
    UInt32 theLen = 0;
    if ((QTSS_GetValuePtr(inParams->inRTSPRequest, qtssRTSPReqMethod, 0,
            (void**)&theMethod, &theLen) != QTSS_NoErr) || (theLen != sizeof(QTSS_RTSPMethod)))
    {
        Assert(0);        
        return QTSS_RequestFailed;
    }

    printf("[ProcessRTSPRequest] Method=%d  \n", *theMethod); // ffmpeg push: 6, 1,1,10(qtssRecordMethod); vlc deamon: 0,1,1,3(qtssPlayMethod)
    if (*theMethod == qtssDescribeMethod) // 0
        return DoDescribe(inParams);
    if (*theMethod == qtssSetupMethod) // 1
    	// return QTSS_NoErr;
        return DoSetup(inParams);
    if(*theMethod == qtssPlayMethod) // 3
   		return DoPlay(inParams);

   switch (*theMethod)
    {
        case qtssTeardownMethod:  // 2
            // Tell the server that this session should be killed, and send a TEARDOWN response
            (void)QTSS_Teardown(inParams->inClientSession); 
            (void)QTSS_SendStandardRTSPResponse(inParams->inRTSPRequest, inParams->inClientSession, 0);
            break; 
        case qtssPauseMethod: // 4
            (void)QTSS_Pause(inParams->inClientSession);
            (void)QTSS_SendStandardRTSPResponse(inParams->inRTSPRequest, inParams->inClientSession, 0);
            break;
        default:
            break;
    } 
    return QTSS_NoErr;
}

QTSS_Error DoDescribe(QTSS_StandardRTSP_Params* inParams)
{
	char* theFileName = NULL;
	QTSS_Error err = QTSS_GetValueAsString(inParams->inRTSPRequest, qtssRTSPReqFileName, 0, &theFileName);  
    if(err != QTSS_NoErr)  
        return QTSSModuleUtils::SendErrorResponse(inParams->inRTSPRequest, qtssClientBadRequest, 0);  
    printf("[ProcessRTSPRequest] DoDescribe,  theFileName=%s  [ProcessRTSPRequest] \n", theFileName);
    QTSSCharArrayDeleter theUriStrDeleter(theFileName);

    // Get the full path to this file
    char* theFullPathStr = NULL;
    (void)QTSS_GetValueAsString(inParams->inRTSPRequest, qtssRTSPReqLocalPath, 0, &theFullPathStr);
    QTSSCharArrayDeleter theFullPathStrDeleter(theFullPathStr);
    printf("[ProcessRTSPRequest] DoDescribe,  theFullPathStr=%s  [ProcessRTSPRequest] \n", theFullPathStr);
    StrPtrLen theFullPath(theFullPathStr);

    // char* result = theFullPath.FindStringIgnoreCase("sdp");
    // printf("[ProcessRTSPRequest] DoDescribe,  result=%s  [ProcessRTSPRequest] \n", result);
    char theSDPFilePath[1024] = {0};
    strcpy(theSDPFilePath, theFullPath.Ptr);
	strcat(theSDPFilePath, sSDPSuffix.Ptr);
	printf("[ProcessRTSPRequest] DoDescribe,  theSDPFilePath=%s  [ProcessRTSPRequest] \n", theSDPFilePath);

    // SDPSourceInfo fSDPParser;
    SDPSourceInfo* pSDPParser = NULL;
    ClientSession* clientSes = NULL;  
    StrPtrLen SDPData;  
	QTSS_TimeVal outModDate = 0;
    QTSS_TimeVal inModDate = -1;
    
    StrPtrLen strDeviceName(theFileName);  
    OSRef* clientSesRef = sClientSessionMap->Resolve(&strDeviceName);  
    if(clientSesRef != NULL)  
    {  
    	printf("[ProcessRTSPRequest] set clientSesRef strDeviceName=%s\n", strDeviceName.Ptr);
        clientSes = (ClientSession*)clientSesRef->GetObject();
        if(clientSes == NULL) 
        {
        	printf("[ProcessRTSPRequest] DoDescribe, clientSes == NULL \n");
        	return QTSS_RequestFailed;
        }
	    QTSS_SetValue(inParams->inClientSession, sClientDeviceNameAttr, 0, strDeviceName.Ptr, strDeviceName.Len);

	   QTSS_Error theErr = OS_NoErr;	   
	   if ( clientSes->GetSDPContext() == NULL) // if (FileExists(pathBuff)) 
	   {
		   do
		   {
		   		theErr= clientSes->GetClient()->SendDescribe(appendJunk);
		   		// printf("[ProcessRTSPRequest] onDemandRelay SendDescribe() ok, theErr=%d\n", theErr); 
		   		if(theErr == 111) // ECONNREFUSED  
		   			return QTSS_RequestFailed;  	
		                 
		   }while(theErr != OS_NoErr);

		   	 pSDPParser = clientSes->GetSDPInfo();
		   	 pSDPParser->Parse(clientSes->GetClient()->GetContentBody(), clientSes->GetClient()->GetContentLength());
		    // fSDPParser.Parse(clientSes->GetClient()->GetContentBody(), clientSes->GetClient()->GetContentLength());	
	   		// printf(" [ProcessRTSPRequest]# DoDescribe  SDP=%s\n", fSDPParser.GetSDPData()->Ptr); 
   
       		SDPData = *(pSDPParser->GetSDPData());
       		// printf(" [ProcessRTSPRequest]# 0000 SDP=%s\n", SDPData.Ptr); 
       		clientSes->SetSDPContext(SDPData);

       		// write the file !! need error reporting
		    FILE* theSDPFile= ::fopen(theSDPFilePath, "wb");//open 
		    if (theSDPFile != NULL)
		    {  		        
		        qtss_fprintf(theSDPFile, "%s", SDPData.Ptr);
		        ::fflush(theSDPFile);
		        ::fclose(theSDPFile);   
		    }
		}
		else
		{
			// StrPtrLen sdpSdp(clientSes->GetSDPContext());
			SDPData.Set(clientSes->GetSDPContext());
			// printf(" [ProcessRTSPRequest]# 1111 SDP=%s\n", SDPData.Ptr); 
		   
		    // (void)QTSSModuleUtils::ReadEntireFile(theSDPFilePath, &SDPData, inModDate, &outModDate);
		    // OSCharArrayDeleter fileDataDeleter(SDPData.Ptr);
		    // printf(" [ProcessRTSPRequest]# 222 SDPData=%s\n", SDPData.Ptr); 
		}
	    // printf("[ProcessRTSPRequest] onDemandRelay SendDescribe() ok, GetResponse=%s  \n\n\n", clientSes->GetClient()->GetResponse()); 
	    // pSDPParser = clientSes->GetSDPInfo();
	    // pSDPParser->Parse(clientSes->GetClient()->GetContentBody(), clientSes->GetClient()->GetContentLength());
	  	
	    // printf(" [ProcessRTSPRequest]# kSendingDescribe SDP=%s\n", pSDPParser->GetSDPData()->Ptr);  
	    // return   QTSSModuleUtils::SendErrorResponseWithMessage(inParams->inRTSPRequest, qtssUnsupportedMediaType, &sSDPNotValidMessage); 	   
    }  
    else  
    {
    	 qtss_printf("sClientSessionMap not found DeviceName = %s\n", theFileName);
    	 return QTSS_RequestFailed;  
    }  

    // SDP send
    // StrPtrLen theFileData(SDP);
    // printf("theFileData  =  %s\n", theFileData.Ptr);
    // OSCharArrayDeleter fileDataDeleter(theFileData.Ptr); 

// [ProcessRTSPRequest][ProcessRTSPRequest]--  Clean up missing required SDP lines

    ResizeableStringFormatter editedSDP(NULL,0);
    editedSDP.Put(SDPData.Ptr);    
	// editedSDP.Put(fSDPParser.GetSDPData()->Ptr);
	// editedSDP.Put(pSDPParser->GetSDPData()->Ptr);
    StrPtrLen editedSDPSPL(editedSDP.GetBufPtr(),editedSDP.GetBytesWritten());
     // printf(" 00000000000 editedSDP  =  %s \n", editedSDP.GetBufPtr());
// [ProcessRTSPRequest][ProcessRTSPRequest]-- Check the headers

    SDPContainer checkedSDPContainer;
    checkedSDPContainer.SetSDPBuffer( &editedSDPSPL );  
    // printf(" 1111111111111 editedSDPSPL  =  %s \n", editedSDPSPL.Ptr);
    if (!checkedSDPContainer.IsSDPBufferValid())
    {  
    	printf("[ProcessRTSPRequest]# checkedSDPContainer error\n" );
        return QTSSModuleUtils::SendErrorResponseWithMessage(inParams->inRTSPRequest, qtssUnsupportedMediaType, &sSDPNotValidMessage);
    }       
 	// SDPSourceInfo theSDPSourceInfo(editedSDPSPL.Ptr, editedSDPSPL.Len );
    //   OSCharArrayDeleter charArrayPathDeleter(theFileData.Ptr);

    SDPLineSorter sortedSDP(&checkedSDPContainer );
 
    //above function has signalled that this request belongs to us, so let's respond
    iovec theDescribeVec[3] = { {0 }};
// [ProcessRTSPRequest][ProcessRTSPRequest]-- Write the SDP 

    UInt32 sessLen = sortedSDP.GetSessionHeaders()->Len;
    UInt32 mediaLen = sortedSDP.GetMediaHeaders()->Len;
    theDescribeVec[1].iov_base = sortedSDP.GetSessionHeaders()->Ptr;
    theDescribeVec[1].iov_len = sortedSDP.GetSessionHeaders()->Len;

     // printf("sessLen  =  %d, mediaLen=%d\n", sessLen, mediaLen);
     // printf("sessLen  =  %s\n, mediaLen=%s\n", sortedSDP.GetSessionHeaders()->Ptr, sortedSDP.GetMediaHeaders()->Ptr);
    theDescribeVec[2].iov_base = sortedSDP.GetMediaHeaders()->Ptr;
    theDescribeVec[2].iov_len = sortedSDP.GetMediaHeaders()->Len;

   (void)QTSS_AppendRTSPHeader(inParams->inRTSPRequest, qtssCacheControlHeader,
                                kCacheControlHeader.Ptr, kCacheControlHeader.Len);
    QTSSModuleUtils::SendDescribeResponse(inParams->inRTSPRequest, inParams->inClientSession,
                                            &theDescribeVec[0], 3, sessLen + mediaLen ); 
 
	return QTSS_NoErr;
}

QTSS_Error DoSetup(QTSS_StandardRTSP_Params* inParams)
{
    // See if this is a push from a Broadcaster
    UInt32 theLen = 0;
    UInt32 *transportModePtr = NULL;
    (void)QTSS_GetValuePtr(inParams->inRTSPRequest, qtssRTSPReqTransportMode, 0, (void**)&transportModePtr, &theLen);
	Bool16 isPush = (transportModePtr != NULL && *transportModePtr == qtssRTPTransportModeRecord) ? true : false;

     //unless there is a digit at the end of this path (representing trackID), don't
    //even bother with the request
    char* theDigitStr = NULL;
    (void)QTSS_GetValueAsString(inParams->inRTSPRequest, qtssRTSPReqFileDigit, 0, &theDigitStr);
    QTSSCharArrayDeleter theDigitStrDeleter(theDigitStr);
    

    UInt32 theTrackID = ::strtol(theDigitStr, NULL, 10);  
    printf("[ProcessRTSPRequest] DoSetup, theTrackID=%d \n", theTrackID);

    //1. create rtpstream
    QTSS_RTPStreamObject newStream = NULL;
    QTSS_Error theErr = AddRTPStream(NULL,inParams,&newStream);
    Assert(theErr == QTSS_NoErr);
    if (theErr != QTSS_NoErr)
    {   
        return QTSSModuleUtils::SendErrorResponse(inParams->inRTSPRequest, qtssClientBadRequest, 0);
    }
    
    if (!isPush)
    {      
	    ClientSession* clientSes = NULL;
	    char strNameBuf[32]  = { 0 };  
		StrPtrLen strDeviceName(strNameBuf, 31);
	  	theErr = QTSS_GetValue(inParams->inClientSession, sClientDeviceNameAttr, 0, strDeviceName.Ptr, &strDeviceName.Len);	    
	   
	    if (theErr != QTSS_NoErr) 
	    {
	    	qtss_printf(" QTSS_GetValue  sClientDeviceNameAttr error\n");
	    	return theErr;
		}

		qtss_printf("DoSetup,clientSes ok, strDeviceName:%s\n", strDeviceName.Ptr);
		OSRef* clientSesRef = sClientSessionMap->Resolve(&strDeviceName); 

	    if(clientSesRef != NULL)  
	    {
	        clientSes = (ClientSession*)clientSesRef->GetObject();	     
		
			UInt32 trackIndex = clientSes->TrackID2TrackIndex(theTrackID);
			QTSS_RTPPayloadType loadType = clientSes->GetTrackType(trackIndex);

			qtss_printf("DoSetup,clientSes, trackIndex=%d, loadType=%d\n",trackIndex, loadType);
	        // if(theTrackID == 65536 || theTrackID == 3) // video
	        if(loadType == qtssVideoPayloadType) // video
	        {
		        // This is not an incoming data session...
		        // Do the standard ReflectorSession setup, create an RTPSessionOutput
		        RTPSessionOutput* theNewOutput = NEW RTPSessionOutput(inParams->inClientSession, NULL, sServerPrefs, 0);
		        clientSes->AddOutput(theNewOutput, true);
                //vVOutput.push_back(theNewOutput);
		        (void)QTSS_SetValue(inParams->inClientSession, svOutputAttr, 0, &theNewOutput, sizeof(theNewOutput));
		        qtss_printf("DoSetup inParams->inClientSession =%08x VNewOutput=%08x\n", &inParams->inClientSession, theNewOutput);
	    	}
	    	else if(loadType == qtssAudioPayloadType)
	    	// else
	    	{
	    		RTPSessionOutput* theNewOutput = NEW RTPSessionOutput(inParams->inClientSession, NULL, sServerPrefs, 1);
		        clientSes->AddOutput(theNewOutput, false);
                //vAOutput.push_back(theNewOutput);
		        (void)QTSS_SetValue(inParams->inClientSession, saOutputAttr, 0, &theNewOutput, sizeof(theNewOutput));
		        qtss_printf("DoSetup inParams->inClientSession =%08x ANewOutput=%08x\n", &inParams->inClientSession, theNewOutput);
	    	}	
    	}    	 
    }    
   
   //send the setup response
    (void)QTSS_AppendRTSPHeader(inParams->inRTSPRequest, qtssCacheControlHeader, kCacheControlHeader.Ptr, kCacheControlHeader.Len);
    (void)QTSS_SendStandardRTSPResponse(inParams->inRTSPRequest, newStream, qtssSetupRespDontWriteSSRC);
	return QTSS_NoErr;
}

QTSS_Error AddRTPStream(ReflectorSession* theSession,QTSS_StandardRTSP_Params* inParams, QTSS_RTPStreamObject *newStreamPtr)
{       
    // Ok, this is completely crazy but I can't think of a better way to do this that's
    // safe so we'll do it this way for now. Because the ReflectorStreams use this session's
    // stream queue, we need to make sure that each ReflectorStream is not reflecting to this
    // session while we call QTSS_AddRTPStream. One brutal way to do this is to grab each
    // ReflectorStream's mutex, which will stop every reflector stream from running.
    Assert(newStreamPtr != NULL);
    
    if (theSession != NULL)
        for (UInt32 x = 0; x < theSession->GetNumStreams(); x++)
            theSession->GetStreamByIndex(x)->GetMutex()->Lock();
    
    //
    // Turn off reliable UDP transport, because we are not yet equipped to
    // do overbuffering.
    QTSS_Error theErr = QTSS_AddRTPStream(inParams->inClientSession, inParams->inRTSPRequest, newStreamPtr, qtssASFlagsForceUDPTransport);

    if (theSession != NULL)
        for (UInt32 y = 0; y < theSession->GetNumStreams(); y++)
            theSession->GetStreamByIndex(y)->GetMutex()->Unlock();

    return theErr;
}

QTSS_Error DoPlay(QTSS_StandardRTSP_Params* inParams)
{ 
	RTPSessionOutput**  vOutput = NULL;
	RTPSessionOutput**  aOutput = NULL;

	UInt32 theLen = 0;

    QTSS_Error theErr = QTSS_Play(inParams->inClientSession, inParams->inRTSPRequest,  qtssPlayFlagsSendRTCP);//qtssPlayFlagsSendRTCP qtssPlayFlagsAppendServerInfo
    if (theErr != QTSS_NoErr){
        return theErr;         
    }

    ClientSession* clientSes = NULL;
    char strNameBuf[32]  = { 0 };  
	StrPtrLen strDeviceName(strNameBuf, 31);
  	theErr = QTSS_GetValue(inParams->inClientSession, sClientDeviceNameAttr, 0, strDeviceName.Ptr, &strDeviceName.Len);
    if (theErr != QTSS_NoErr) return theErr;


	qtss_printf("[DoPlay],clientSes ok, strDeviceName:%s\n", strDeviceName.Ptr);
	OSRef* clientSesRef = sClientSessionMap->Resolve(&strDeviceName);
    if(clientSesRef != NULL)  
    { 
		clientSes = (ClientSession*)clientSesRef->GetObject();
		qtss_printf("[DoPlay] clientSes->Signal ----\n");
		QTSS_GetValuePtr(inParams->inClientSession, svOutputAttr, 0, (void**)&vOutput, &theLen);	
		QTSS_GetValuePtr(inParams->inClientSession, saOutputAttr, 0, (void**)&aOutput, &theLen);
		if((vOutput == NULL) || (*vOutput == NULL)  || (aOutput == NULL) || (*aOutput == NULL))
		{
			QTSS_RequestFailed ;
		}

		clientSes->startConnect(*vOutput, *aOutput);//Signal(Task::kStartEvent);
	}

	(void)QTSS_SendStandardRTSPResponse(inParams->inRTSPRequest, inParams->inClientSession, qtssPlayRespWriteTrackInfo);
    return QTSS_NoErr;
}

QTSS_Error ProcessRealyRTPData(QTSS_StandardRTSP_Params* inParams)
{
	return QTSS_NoErr;
}

QTSS_Error DestroySession(QTSS_ClientSessionClosing_Params* inParams)
{
     RTPSessionOutput**  theOutput = NULL;
     ReflectorOutput*    voutputPtr = NULL;
     ReflectorOutput*    aoutputPtr = NULL;
          
    // OSMutexLocker locker (sSessionMap->GetMutex());  

    ClientSession* clientSes = NULL;
    char strNameBuf[32]  = { 0 };  
	StrPtrLen strDeviceName(strNameBuf, 31);
  	QTSS_Error theErr = QTSS_GetValue(inParams->inClientSession, sClientDeviceNameAttr, 0, strDeviceName.Ptr, &strDeviceName.Len);
    if (theErr != QTSS_NoErr) return theErr;

	qtss_printf(" DestroySession,clientSes ok, strDeviceName:%s\n", strDeviceName.Ptr);
	OSRef* clientSesRef = sClientSessionMap->Resolve(&strDeviceName);
    if(clientSesRef != NULL)  
    { 
		clientSes = (ClientSession*)clientSesRef->GetObject();

		// get video output 
	 	UInt32 theLen = 0;

	    theErr = QTSS_GetValuePtr(inParams->inClientSession, svOutputAttr, 0, (void**)&theOutput, &theLen);
	    if ((theErr != QTSS_NoErr) || (theLen != sizeof(RTPSessionOutput*)) || (theOutput == NULL) || (*theOutput == NULL))
	        return QTSS_RequestFailed;

	    qtss_printf("DestroySession, inParams->inClientSession =%08x vNewOutput=%08x\n", &inParams->inClientSession, *theOutput);
	    if (theOutput != NULL)
	        voutputPtr = (ReflectorOutput*) *theOutput;

	    // get audio output 
	    theErr = QTSS_GetValuePtr(inParams->inClientSession, saOutputAttr, 0, (void**)&theOutput, &theLen);
	    if ((theErr != QTSS_NoErr) || (theLen != sizeof(RTPSessionOutput*)) || (theOutput == NULL) || (*theOutput == NULL))
	        return QTSS_RequestFailed;

	    qtss_printf("DestroySession, inParams->inClientSession =%08x aNewOutput=%08x\n", &inParams->inClientSession, *theOutput);
	    if (theOutput != NULL)
	        aoutputPtr = (ReflectorOutput*) *theOutput;
	     
	    clientSes->stopConnect(voutputPtr, aoutputPtr);

	    if (voutputPtr != NULL)
	    {    
	        clientSes->RemoveOutput(voutputPtr, true); 
            // qtss_printf(" DestroySession 000\n");
	        RTPSessionOutput* theOutput = NULL;
	        (void)QTSS_SetValue(inParams->inClientSession, svOutputAttr, 0, &theOutput, sizeof(theOutput));
	        
	    }

	    if (aoutputPtr != NULL)
	    {    
	        clientSes->RemoveOutput(aoutputPtr, false);
            // qtss_printf(" DestroySession 111\n");
	        RTPSessionOutput* theOutput = NULL;
	        (void)QTSS_SetValue(inParams->inClientSession, saOutputAttr, 0, &theOutput, sizeof(theOutput));
	    }

	    //clientSes->GetClient()->SendTeardown();
	}	
    return QTSS_NoErr;
}

QTSS_Error Shutdown()
{
// #if QTSS_REFLECTOR_EXTERNAL_MODULE
//     TaskThreadPool::RemoveThreads();
// #endif
    return QTSS_NoErr;
}