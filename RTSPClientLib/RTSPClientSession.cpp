#include <arpa/inet.h>
//#include <stdlib.h>
#include "RTSPClientSession.h"
#include "OSMemory.h"
#include "SafeStdLib.h"
#include "OSHeaders.h"
#include "OS.h"
#include "RTPPacket.h"
#include "RTCPPacket.h"
#include "RTCPRRPacket.h"
#include "RTCPAckPacketFmt.h"
#include "RTCPNADUPacketFmt.h"



RTSPClientSession::RTSPClientSession(UInt32 inAddr, UInt16 inPort, char* inURL,
                        ClientType inClientType,                      
                        UInt32 inRTCPIntervalInMS, UInt32 inOptionsIntervalInSec,
                        UInt32 inReadInterval,UInt32 inSockRcvBufSize, 
                        Float32 inSpeed, char* inPacketRangePlayHeader, UInt32 inOverbufferWindowSizeInK,
                        Bool16 sendOptions, char *name = NULL, char *password = NULL, const char* streamName)
:   fSocket(NULL),
    RTSPClientSessionClient(NULL),
    fTimeoutTask(this, kIdleTimeoutInMsec),

    fRTCPIntervalInMs(inRTCPIntervalInMS),
    fOptionsIntervalInSec(inOptionsIntervalInSec),
    
    fOptions(sendOptions),
    fTransactionStartTimeMilli(0),

    fState(kSendingDescribe),
    fDeathReason(kDiedNormally),
    fNumSetups(0),
    fUDPSocketArray(NULL),
    
    fPlayTime(0),
    fTotalPlayTime(0),
    fLastRTCPTime(0),
    fTeardownImmediately(false),
    fReadInterval(inReadInterval),
    fSockRcvBufSize(inSockRcvBufSize),
    
    fSpeed(inSpeed),
    fPacketRangePlayHeader(inPacketRangePlayHeader),

	
    fStats(NULL),
    fOverbufferWindowSizeInK(inOverbufferWindowSizeInK),
    fCurRTCPTrack(0),
    fNumPacketsReceived(0),
	fNumBytesReceived(0),
    fVerboseLevel(verboseLevel),
	fPlayerSimulator(verboseLevel)
{
    this->SetTaskName("QTSSOnDemandRelayModule:RTSPClientSession");
    StrPtrLen theURL(inURL);
