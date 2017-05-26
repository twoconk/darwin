/*
 *
 * @APPLE_LICENSE_HEADER_START@
 *
 * Copyright (c) 1999-2008 Apple Inc.  All Rights Reserved.
 *
 * This file contains Original Code and/or Modifications of Original Code
 * as defined in and that are subject to the Apple Public Source License
 * Version 2.0 (the 'License'). You may not use this file except in
 * compliance with the License. Please obtain a copy of the License at
 * http://www.opensource.apple.com/apsl/ and read it before using this
 * file.
 * 
 * The Original Code and all software distributed under the License are
 * distributed on an 'AS IS' basis, WITHOUT WARRANTY OF ANY KIND, EITHER
 * EXPRESS OR IMPLIED, AND APPLE HEREBY DISCLAIMS ALL SUCH WARRANTIES,
 * INCLUDING WITHOUT LIMITATION, ANY WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE, QUIET ENJOYMENT OR NON-INFRINGEMENT.
 * Please see the License for the specific language governing rights and
 * limitations under the License.
 * 
 * @APPLE_LICENSE_HEADER_END@
 *
 */
/*
    File:       ClientSession.cpp

    Contains:   .  
                    
    
    
*/

#include <arpa/inet.h>
//#include <stdlib.h>
#include "ClientSession.h"
#include "OSMemory.h"
#include "SafeStdLib.h"
#include "OSHeaders.h"
#include "OS.h"
#include "RTPPacket.h"
#include "RTCPPacket.h"
#include "RTCPRRPacket.h"
#include "RTCPAckPacketFmt.h"
#include "RTCPNADUPacketFmt.h"
    
//These two parameters governs how the client determines whether an incoming RTP packet is within the range of the sequence number or not.
enum {
    kMaxDropOut = 3000,             //The sequence number can be kMaxDropOut ahead of the reference.
    kMaxMisorder = 100,             //The sequence number can be kMaxMisorder behind of the reference.
    kMaxUDPPacketSize = 1450
};

#define CLIENT_SESSION_DEBUG 0

static const SInt64 kMaxWaitTimeInMsec = 5000;
static const SInt64 kIdleTimeoutInMsec = 20000; // Time out in 20 seconds if nothing's doing
static const SInt16 kSanitySeqNumDifference = 3000; //how large a difference can two sequence number be and still be considered to be in range

UInt32          ClientSession::sActiveConnections = 0;
UInt32          ClientSession::sPlayingConnections = 0;
UInt32          ClientSession::sTotalConnectionAttempts = 0;

UInt32          ClientSession::sBytesReceived = 0;
UInt32          ClientSession::sPacketsReceived = 0;


char* ConvertBytesToCHexString( void* inValue, const UInt32 inValueLen)
{
    static const char* kHEXChars={ "0123456789ABCDEF" };

    UInt8* theDataPtr = (UInt8*) inValue;
    UInt32 len = inValueLen *2;
    
    char *theString = NEW char[len+1];
    char *resultStr = theString;
    if (theString != NULL)
    {
        UInt8 temp;
        UInt32 count = 0;
        for (count = 0; count < inValueLen; count++)
        {
            temp = *theDataPtr++;
            *theString++ = kHEXChars[temp >> 4];
            *theString++ = kHEXChars[temp & 0xF];
        }
        *theString = 0;
    }
    return resultStr;
}


ClientSession::ClientSession(   UInt32 inAddr, UInt16 inPort, char* inURL,
                                ClientType inClientType,
                                UInt32 inDurationInSec, UInt32 inStartPlayTimeInSec,
                                UInt32 inRTCPIntervalInMS, UInt32 inOptionsIntervalInSec,
                                UInt32 inHTTPCookie, Bool16 inAppendJunkData, UInt32 inReadInterval,
                                UInt32 inSockRcvBufSize, Float32 inLateTolerance, char* inMetaInfoFields,
                                Float32 inSpeed, UInt32 verboseLevel, char* inPacketRangePlayHeader, UInt32 inOverbufferWindowSizeInK,
                                Bool16 sendOptions, Bool16 requestRandomData, SInt32 randomDataSize, Bool16 enable3GPP,
                                UInt32 GBW, UInt32 MBW, UInt32 MTD, Bool16 enableForcePlayoutDelay, UInt32 playoutDelay, 
								UInt32 bandwidth, UInt32 bufferSpace, UInt32 delayTime, UInt32 startPlayDelay,
                                char *controlID, char *name, char *password, char* streamName)
:   fSocket(NULL),
    fClient(NULL),
    fTimeoutTask(this, kIdleTimeoutInMsec),

    //fDurationInSec(inDurationInSec - inStartPlayTimeInSec),
    fDurationInSec(6000),
    fStartPlayTimeInSec(inStartPlayTimeInSec),
    fRTCPIntervalInMs(inRTCPIntervalInMS),
   // fOptionsIntervalInSec(inOptionsIntervalInSec),
    
    //fOptions(sendOptions),
    fOptionsRequestRandomData(requestRandomData),
    fOptionsRandomDataSize(randomDataSize),
    fTransactionStartTimeMilli(0),

    fState(kSendingDescribe),//kSendingDescribe
    fDeathReason(kDiedNormally),
    fNumSetups(0),
    fUDPSocketArray(NULL),
    
    fPlayTime(0),
    fTotalPlayTime(0),
    fLastRTCPTime(0),
    fTeardownImmediately(false),
    fAppendJunk(inAppendJunkData),
    fReadInterval(inReadInterval),
    fSockRcvBufSize(inSockRcvBufSize),
    
    fSpeed(inSpeed),
    fPacketRangePlayHeader(inPacketRangePlayHeader),
	
    //fGuarenteedBitRate(GBW),
    //fMaxBitRate(MBW),
    //fMaxTransferDelay(MTD),
	fEnableForcePlayoutDelay(enableForcePlayoutDelay),
	fPlayoutDelay(playoutDelay),
    //fBandwidth(bandwidth),
	fBufferSpace(bufferSpace),
	fDelayTime(delayTime),
	fStartPlayDelay(startPlayDelay),
	fEnable3GPP(enable3GPP),
	
    //fStats(NULL),
    fOverbufferWindowSizeInK(inOverbufferWindowSizeInK),
    fCurRTCPTrack(0),
    fNumPacketsReceived(0),
	fNumBytesReceived(0),
    fVerboseLevel(verboseLevel),
	fPlayerSimulator(verboseLevel), pLibUVClient(NULL)/*,
	aid(0), vid(0), mWorkerVideoLoop(NULL), refVideoLoop(0), 
	mWorkerAudioLoop(NULL),refAudioLoop(0), mWorkerAudioThread(0), 
	mWorkerVideoThread(0)*/
{    
    this->SetTaskName("RTSPClientLib:ClientSession");
    StrPtrLen theURL(inURL);

    if (true == sendOptions)
       fState = kSendingOptions;
    
#if CLIENT_SESSION_DEBUG
    fVerboseLevel = kUInt32_Max;  //maximum possible unsigned int value in 2's complement
#endif
    if ( fVerboseLevel >= 2)
    {
        in_addr inAddrStruct;
        inAddrStruct.s_addr = inAddr;
        qtss_printf("Connecting to: %s, port %d\n", inet_ntoa(inAddrStruct), inPort);
    }

    //
    // Construct the appropriate ClientSocket type depending on what type of client we are supposed to be
    switch (inClientType)
    {
        case kRTSPUDPClientType:
        {
            fControlType = kRawRTSPControlType;
            fTransportType = kUDPTransportType;            
            fSocket = NEW TCPClientSocket(Socket::kNonBlockingSocketType);
            break;
        }
        case kRTSPTCPClientType:
        {
            fControlType = kRawRTSPControlType;
            fTransportType = kTCPTransportType;
            fSocket = NEW TCPClientSocket(Socket::kNonBlockingSocketType);
            break;
        }
        case kRTSPHTTPClientType:
        {
            fControlType = kRTSPHTTPControlType;
            fTransportType = kTCPTransportType;
            fSocket = NEW HTTPClientSocket(theURL, inHTTPCookie, Socket::kNonBlockingSocketType);
            break;
        }
        case kRTSPHTTPDropPostClientType:
        {
            fControlType = kRTSPHTTPDropPostControlType;
            fTransportType = kTCPTransportType;
            fSocket = NEW HTTPClientSocket(theURL, inHTTPCookie, Socket::kNonBlockingSocketType);
            break;
        }
        case kRTSPReliableUDPClientType:
        {
            fControlType = kRawRTSPControlType;
            fTransportType = kReliableUDPTransportType;
            fSocket = NEW TCPClientSocket(Socket::kNonBlockingSocketType);
            break;
        }
        default:
        {
            qtss_printf("ClientSession: Attempt to create unsupported client type.\n");
            //::exit(-1);
        }
    }

    //LYZ ADD
    pSDPStr = NULL;
    initLibUVClient();

    fid_vthread = -1;
    fid_athread = -1;
    fid_rthread = -1;
    //add end.
    
    fSocket->Set(inAddr, inPort);

    //
    // Construct the client object using this socket.
    fClient = NEW RTSPClient(fSocket);
    fClient->SetVerboseLevel(fVerboseLevel);
    fClient->Set(theURL);
    fClient->SetSetupParams(inLateTolerance, inMetaInfoFields);
	fClient->SetBandwidth(bandwidth);
	if(controlID != NULL)
		fClient->SetControlID(controlID);

    if (enable3GPP)
    {
        fClient->Set3GPPLinkChars(GBW, MBW, MTD);
	    fClient->Set3GPPRateAdaptation(fBufferSpace, fDelayTime);
    }
    
    //user name and password
    if (name != NULL && password != NULL)
    {
        fClient->SetName(name);
        fClient->SetPassword(password);
    }

    StrPtrLen strName(streamName);
    fRef.Set(strName, this);
    //
    // Start the connection process going
    //this->Signal(Task::kStartEvent);
}

ClientSession::~ClientSession()
{
    qtss_printf("ClientSession::~ClientSession \n"); 
    if (fUDPSocketArray != NULL)
    {
        for (UInt32 x = 0; x < fSDPParser.GetNumStreams() * 2; x++)
        {
            OS_Error theErr = OS_NoErr;
            
            while (theErr == OS_NoErr)
            {
                    UInt32 theRemoteAddr = 0;
                    UInt32 theLength = 0;
                    UInt16 theRemotePort = 0;
                    char thePacketBuf[2048];
            
                    // Get a packet from one of the UDP sockets.
                    theErr = fUDPSocketArray[x]->RecvFrom(&theRemoteAddr, &theRemotePort,
                                                                &thePacketBuf[0], 2048,
                                                                &theLength);
            }
            delete fUDPSocketArray[x];
        }
    }
            
    delete [] fUDPSocketArray;
    delete fClient;
    delete fSocket;

    //lyz@xdja.com add
    if (pLibUVClient != NULL){
        if (pLibUVClient->mWorkerVideoLoop){
            uv_loop_close(pLibUVClient->mWorkerVideoLoop);       
            free(pLibUVClient->mWorkerAudioLoop);
        }
        pLibUVClient->mWorkerVideoLoop = NULL;
        if (pLibUVClient->mWorkerAudioLoop){
            uv_loop_close(pLibUVClient->mWorkerAudioLoop);
            free(pLibUVClient->mWorkerAudioLoop);
        }
        pLibUVClient->mWorkerAudioLoop = NULL;
        free(pLibUVClient);
        pLibUVClient = NULL;
    }
    //add end

    fvoutput.wipe();
    faoutput.wipe();
	//delete fStats;
}

void ClientSession::initLibUVClient(){
    pLibUVClient = (LibUVClient *)malloc(sizeof(LibUVClient));
    if (pLibUVClient){
        qtss_printf("initLibUVClient pLibUVClient:%08x  \n", pLibUVClient); 
        pLibUVClient->aid = 1;
        pLibUVClient->vid = 1;
        pLibUVClient->mWorkerVideoLoop = NULL;
        pLibUVClient->refVideoLoop= 1;
        pLibUVClient->mWorkerAudioLoop = NULL;
        pLibUVClient->refAudioLoop = 1;
        pLibUVClient->mWorkerAudioThread = 0;
        pLibUVClient->mWorkerVideoThread = 0;
    }
}


void ClientSession::delLibUVClient(){
    if (pLibUVClient){
        destroyVideoLoop();
        destroyAudioLoop();
    }
}

void ClientSession::destroyVideoLoop(){
//void destroyVideoLoop(){
        
    if (pLibUVClient->mWorkerVideoLoop == NULL){
        return;
    }

    pLibUVClient->refVideoLoop--;
    if (pLibUVClient->refVideoLoop <= 0){
        qtss_printf("release  pLibUVClient->mWorkerVideoLoop:%08x\n", pLibUVClient->mWorkerVideoLoop); 
        uv_loop_close(pLibUVClient->mWorkerVideoLoop);
        free(pLibUVClient->mWorkerVideoLoop);
        pLibUVClient->refVideoLoop = 0;
        pLibUVClient->mWorkerVideoLoop = NULL;

        pLibUVClient->mWorkerVideoThread = 0;
    }
}

uv_loop_t* ClientSession::createVideoLoop(){
//uv_loop_t* createVideoLoop(){
    if (pLibUVClient->mWorkerVideoLoop != NULL){
        pLibUVClient->refVideoLoop++;
        return pLibUVClient->mWorkerVideoLoop;
    }
    
    pLibUVClient->mWorkerVideoLoop = (uv_loop_t *)malloc(sizeof(uv_loop_t));
    if (pLibUVClient->mWorkerVideoLoop){
        uv_loop_init(pLibUVClient->mWorkerVideoLoop);
    }
    qtss_printf("createVideoLoop pLibUVClient->mWorkerVideoLoop:%08x  \n", pLibUVClient->mWorkerVideoLoop); 
    pLibUVClient->refVideoLoop = 1;
    return pLibUVClient->mWorkerVideoLoop;
}

void ClientSession::destroyAudioLoop(){
//void destroyAudioLoop(){
        
    if (pLibUVClient->mWorkerVideoLoop == NULL){
        return;
    }

    pLibUVClient->refAudioLoop--;
    if (pLibUVClient->refAudioLoop <= 0){
        qtss_printf("release  pLibUVClient->mWorkerAudioLoop:%08x\n", pLibUVClient->mWorkerAudioLoop); 
        uv_loop_close(pLibUVClient->mWorkerAudioLoop);
        free(pLibUVClient->mWorkerAudioLoop);
        pLibUVClient->refAudioLoop = 0;
        pLibUVClient->mWorkerAudioLoop = NULL;

        pLibUVClient->mWorkerAudioThread = 0;
    }
}

uv_loop_t* ClientSession::createAudioLoop(){
//uv_loop_t* createAudioLoop(){
    if (pLibUVClient->mWorkerAudioLoop != NULL){
        pLibUVClient->refAudioLoop++;
        return pLibUVClient->mWorkerAudioLoop;
    }
    
    pLibUVClient->mWorkerAudioLoop = (uv_loop_t *)malloc(sizeof(uv_loop_t));
    if (pLibUVClient->mWorkerAudioLoop){
        qtss_printf("createAudioLoop pLibUVClient->mWorkerAudioLoop:%08x  \n", pLibUVClient->mWorkerAudioLoop); 
        uv_loop_init(pLibUVClient->mWorkerAudioLoop);
    }

    pLibUVClient->refAudioLoop = 1;
    return pLibUVClient->mWorkerAudioLoop;
}

void threadWorkerVideo(void *userp){
    qtss_printf("start threadWorkerVideo \n"); 
    uv_loop_t* loop = (uv_loop_t* )userp;
    if (loop == NULL){
        return;
    }
    uv_run(loop, UV_RUN_DEFAULT);
}

void threadWorkerAudio(void *userp){
    qtss_printf("start threadWorkerAudio \n"); 
    uv_loop_t* loop = (uv_loop_t* )userp;
    if (loop == NULL){
        return;
    }
    uv_run(loop, UV_RUN_DEFAULT);
}


void sendEnd(uv_work_t *req, int status){
}

void sendRtpToOutput(uv_work_t *req){
    if (req == NULL){
        return;
    }
    
    UVWorkData *data = (UVWorkData *)req->data;

    if (data == NULL || data->outPut == NULL){
        return;
    }
    OSMutexLocker locker(data->fOutputMutex);
    
    SInt64 timeToSendPacket = -1;
    SInt64      fTimeArrived = 0;
    
    StrPtrLen strPacket(data->inPacket, data->inLength);
    // qtss_printf("sendRtpToOutput, req:%08x, data:%08x, output=%08x, &data->id=%d, data->inLength=%d\n", req, data->inPacket , data->outPut, data->id, data->inLength);
    
    data->outPut->WritePacket(&strPacket, NULL, data->inFlag, 0, &timeToSendPacket, &data->id, &fTimeArrived, false);//(&strPacket, NULL, qtssWriteFlagsIsRTP, 0, &timeToSendPacket, &id, &fTimeArrived, id==0?true:false )
    if (data != NULL){
        if (data->inPacket != NULL){
            free(data->inPacket);
            data->inPacket = NULL;
        }
        free(data);
        data = NULL;
    }
    
    free(req);
}

void ClientSession::putRtpToVideoQueue(char* inPacket, UInt32 inLength, UInt32 inFlag){
    if (pLibUVClient->mWorkerVideoLoop == NULL){
        return;
    }
    
    //fOutputMutex.Lock();     
    for(int i=0; i<fvoutput.size(); i++)
    {
        if(fvoutput[i]->fState == kPlaying)
        {    
            uv_work_t *req = (uv_work_t *)malloc(sizeof(uv_work_t)); 
            if (req == NULL){
                continue;
            }
            UVWorkData *workData = (UVWorkData *)malloc(sizeof(UVWorkData));
        
            if (workData == NULL){
                qtss_printf("eRROR, NO MEMORY");
                continue;
            }
            char *sendData = (char *)malloc(inLength);
            if (sendData == NULL){
                // fOutputMutex.Unlock();
                return;
            }
            
            // qtss_printf("i=%d, sendData:%08x\n", i, sendData);
            memcpy(sendData, inPacket, inLength);
            workData->inPacket = sendData;
            workData->outPut = fvoutput[i];
            workData->inLength = inLength;
            workData->id = pLibUVClient->vid;
            workData->inFlag = inFlag;
            workData->fOutputMutex = &fOutputMutex;
            
            req->data = workData;
            
            uv_queue_work(pLibUVClient->mWorkerVideoLoop, req, sendRtpToOutput, sendEnd);
            uv_run(pLibUVClient->mWorkerVideoLoop, UV_RUN_DEFAULT);
        }
    }
    pLibUVClient->vid++;
    // fOutputMutex.Unlock();    
}

void ClientSession::putRtpToAudioQueue(char* inPacket, UInt32 inLength, UInt32 inFlag){    
    if (pLibUVClient->mWorkerAudioLoop == NULL){
        return;
    }   

    //fOutputMutex.Lock();
    for(int i=0; i<faoutput.size(); i++)
    {
        if(faoutput[i]->fState == kPlaying)
        {    
            uv_work_t *req = (uv_work_t *)malloc(sizeof(uv_work_t)); 
            if (req == NULL){
                continue;
            }
            UVWorkData *workData = (UVWorkData *)malloc(sizeof(UVWorkData));
        
            if (workData == NULL){
                qtss_printf("eRROR, NO MEMORY1");
                continue; 
            }
            char *sendData = (char *)malloc(inLength);
            if (sendData == NULL){
                 //fOutputMutex.Unlock();
                return;    
            } 
            
            memcpy(sendData, inPacket, inLength);
            workData->inPacket = sendData;
            workData->outPut = faoutput[i];
            workData->inLength = inLength;
            workData->id = pLibUVClient->aid;
            workData->inFlag = inFlag;
            workData->fOutputMutex = &fOutputMutex;            
            req->data = workData;
             
            uv_queue_work(pLibUVClient->mWorkerAudioLoop, req, sendRtpToOutput, sendEnd);
            uv_run(pLibUVClient->mWorkerAudioLoop, UV_RUN_DEFAULT);
        }
    }
     
    pLibUVClient->aid++;
    // fOutputMutex.Unlock(); 
}

void ClientSession::startConnect(ReflectorOutput* vOutPut, ReflectorOutput* aOutPut){
    qtss_printf("ClientSession::startConnect.\n");
    //start one thread_worker for send rtp to output
    //gWorkerLoop = create_loop();//uv_default_loop();
    createVideoLoop();
    createAudioLoop();
    

    UInt32 pos = fvoutput.find(vOutPut);
    if(pos != fvoutput.size())
    {
        fvoutput[pos]->fState = kPlaying;
    }

    pos = faoutput.find(aOutPut);
    if(pos != faoutput.size())
    {
        faoutput[pos]->fState = kPlaying;
    }
        
    if (pLibUVClient->mWorkerVideoThread == 0){
        
        pLibUVClient->aid = 1;
        pLibUVClient->vid = 1;

        uv_thread_create(&(pLibUVClient->mWorkerVideoThread), threadWorkerVideo, pLibUVClient->mWorkerVideoLoop);
        uv_thread_create(&(pLibUVClient->mWorkerAudioThread), threadWorkerAudio, pLibUVClient->mWorkerAudioLoop);
    }

    if (fState != kPlaying){
        fState = kSendingDescribe;
        this->GetClient()->SetState(0);
        this->Signal(Task::kStartEvent);        
    }
}

void ClientSession::stopConnect(ReflectorOutput* vOutPut, ReflectorOutput* aOutPut){
    qtss_printf("ClientSession::stopConnect.\n");
    delLibUVClient();

    UInt32 pos = fvoutput.find(vOutPut);
    if(pos != fvoutput.size())
    {
        fvoutput[pos]->fState = 0;
    }

    pos = faoutput.find(aOutPut);
    if(pos != faoutput.size())
    {
        faoutput[pos]->fState = 0;
    }

    if (pLibUVClient->refVideoLoop <= 0){
        pLibUVClient->mWorkerVideoThread == 0;

        fState = kDone;
        fNumSetups = 0;
        this->Signal(Task::kStartEvent);        
    }
}

SInt64 ClientSession::Run()
{ 
    // qtss_printf("ClientSession::Run.\n");
    EventFlags theEvents = this->GetEvents();
    if (theEvents & Task::kStartEvent)
    {
        sActiveConnections++;
        sTotalConnectionAttempts++;
		//Sometimes the clientSession can be told to stop before it has a chance to start the connection
		if (theEvents & ClientSession::kTeardownEvent)
			fTeardownImmediately = true;
		else
		{
			Assert(theEvents == Task::kStartEvent);
			// Determine a random connection interval, and go away until that time comes around.
			// Next time the event received would be Task::kIdleEvent, and the initial state is kSendingDescribe
			return ((UInt32) ::rand()) % kMaxWaitTimeInMsec + 1;
		}
    }
    
    // 
    if (theEvents & Task::kTimeoutEvent)
    {
		if(fState == kDone)
			return 0;
		if ( fVerboseLevel >= 2)
        	qtss_printf("Session timing out.\n");
        fDeathReason = kSessionTimedout;
        fState = kDone;
        return 0;
    }
                
    //
    // If we've been told to TEARDOWN, do so.
    if (theEvents & ClientSession::kTeardownEvent)
    {
		if ( fVerboseLevel >= 2)
        	qtss_printf("Session tearing down immediately.\n");
        fTeardownImmediately = true;
    }
    
    // We have been told to delete ourselves. Do so... NOW!!!!!!!!!!!!!!!
    if (theEvents & Task::kKillEvent)
    {
	 if ( fVerboseLevel >= 2)
        	qtss_printf("Session killed.\n");
        sActiveConnections--;
        return -1;
    }   
    
    // Refresh the timeout. There is some legit activity going on...
    fTimeoutTask.RefreshTimeout();
    
    OS_Error theErr = OS_NoErr;
    
    //qtss_printf("Session fState:%d\n", fState);
    while ((theErr == OS_NoErr) && (fState != kDone))
    {
        //
        // Do the appropriate thing depending on our current state
        switch (fState)
        {
            case kSendingOptions:
            {
                if (true == fOptionsRequestRandomData)
                    theErr = fClient->SendOptionsWithRandomDataRequest(fOptionsRandomDataSize);
                else
                    theErr = fClient->SendOptions();

				if ( fVerboseLevel >= 3)
            		qtss_printf("Sent OPTIONS. Result = %"_U32BITARG_". Response code = %"_U32BITARG_"\n", theErr, fClient->GetStatus());
                if (0 == fTransactionStartTimeMilli) 
                   fTransactionStartTimeMilli = OS::Milliseconds();

                if (theErr == OS_NoErr)
                {
                        
                    // Check that the OPTIONS response is a 200 OK. If not, bail
                    if (fClient->GetStatus() != 200)
                    {
                        theErr = ENOTCONN; // Exit the state machine
                        break;
                    }
                    else
                    {
						if ( fVerboseLevel >= 3)
                        {
                            qtss_printf("--- Options transaction time ms = %qd  ---\n", (SInt64) ( OS::Milliseconds() - fTransactionStartTimeMilli) );
                            SInt32 receivedLength = (SInt32) fClient->GetContentLength();
                            if (receivedLength != 0)
                                qtss_printf("--- Options Request Random Data Received requested = %"_S32BITARG_" received = %"_S32BITARG_"  ---\n", fOptionsRandomDataSize, receivedLength);
                                
                            StrPtrLenDel theBody(ConvertBytesToCHexString(fClient->GetContentBody(), receivedLength));
                            theBody.PrintStr("\n");
                        }
                        fState = kSendingDescribe;
                    }
                }
                
                break;
            }
            case kSendingDescribe:
            {
                theErr = fClient->SendDescribe(fAppendJunk);
				if ( fVerboseLevel >= 3)
                	qtss_printf("Sent DESCRIBE. Result = %"_U32BITARG_". Response code = %"_U32BITARG_"\n", theErr, fClient->GetStatus());
                if (theErr == OS_NoErr)
                {
                    // Check that the DESCRIBE response is a 200 OK. If not, bail
                    if (fClient->GetStatus() != 200)
                    {
                        theErr = ENOTCONN; // Exit the state machine
                        break;
                    }
                    else
                    {
                        //
                        // We've sent a describe and gotten a response from the server.
                        // Parse the response and look for track information.

                        fSDPParser.Parse(fClient->GetContentBody(), fClient->GetContentLength());
                        // printf("############ kSendingDescribe SDP=%s\n", fSDPParser.GetSDPData()->Ptr);
                        //
                        // The SDP must have been misformatted.
                        if (fSDPParser.GetNumStreams() == 0)
                            fDeathReason = kBadSDP;
                            
                        //
                        // We have valid SDP. If this is a UDP connection, construct a UDP
                        // socket array to act as incoming sockets.
                        if ((fTransportType == kUDPTransportType) || (fTransportType == kReliableUDPTransportType))
                            //if(fUDPSocketArray == NULL)                                
                            //{
                                this->SetupUDPSockets();
                           // }
                            
                        //
                        // Setup client stats
						//delete fStats;
                        //fStats = NEW TrackStats[fSDPParser.GetNumStreams()];
                        fStats.resize(fSDPParser.GetNumStreams());
						//::memset(fStats, 0, sizeof(TrackStats) * fSDPParser.GetNumStreams());
                    }
					fPlayerSimulator.Setup(fSDPParser.GetNumStreams(), fDelayTime, fStartPlayDelay);
                    fState = kSendingSetup;
                }
                break;
            }
            case kSendingSetup:
            {
                // The SETUP request is different depending on whether we are interleaving or not
                if (fTransportType == kUDPTransportType)
                {
                    if(fUDPSocketArray != NULL)
                    {
                       // qtss_printf("#### kSendingSetup , fUDPSocketArray =%8x fNumSetups=%d \n", fUDPSocketArray, fNumSetups);
                        theErr = fClient->SendUDPSetup(fSDPParser.GetStreamInfo(fNumSetups)->fTrackID,                      
                                                fUDPSocketArray[fNumSetups*2]->GetLocalPort());
                    }
                }
                else if (fTransportType == kTCPTransportType)
                {
                    fSocket->SetRcvSockBufSize(fSockRcvBufSize); // Make the rcv buf really big
                    theErr = fClient->SendTCPSetup(fSDPParser.GetStreamInfo(fNumSetups)->fTrackID,fNumSetups * 2, (fNumSetups * 2) +1);
                }
                else if (fTransportType == kReliableUDPTransportType)
                {
                    theErr = fClient->SendReliableUDPSetup(fSDPParser.GetStreamInfo(fNumSetups)->fTrackID,
                                                fUDPSocketArray[fNumSetups*2]->GetLocalPort());
                }
				if ( fVerboseLevel >= 3)
                	qtss_printf("Sent SETUP #%"_U32BITARG_". Result = %"_U32BITARG_". Response code = %"_U32BITARG_"\n",
							fNumSetups, theErr, fClient->GetStatus());
                //
                // If this SETUP request / response is complete, check for errors, and if
                // it succeeded, move onto the next SETUP. If we're done setting up all tracks,
                // move onto PLAY.
                if (theErr == OS_NoErr)
                {
                    if (fClient->GetStatus() != 200)
                    {
                        theErr = ENOTCONN; // Exit the state machine
                        break;
                    }
                    else
                    {
                        // Record the server port for RTCPs.
                        fStats[fNumSetups].fDestRTCPPort = fClient->GetServerPort() + 1;
                        
						//obtain the sampling rate of this stream
						StringParser parser = StringParser(&fSDPParser.GetStreamInfo(fNumSetups)->fPayloadName);
						parser.GetThru(NULL, '/');
						UInt32 samplingRate = parser.ConsumeInteger(NULL);
						Assert(samplingRate != 0);
						
						//Generates the client SSRC
						SInt64 ms = OS::Microseconds();
						UInt32 ssrc = static_cast<UInt32>((ms >> 32) ^ ms) + ::rand();
						fStats[fNumSetups].fClientSSRC = ssrc + fNumSetups;
						
						fPlayerSimulator.SetupTrack(fNumSetups, samplingRate, fBufferSpace);
                        fNumSetups++;                    
                        if (fNumSetups == fSDPParser.GetNumStreams())
                        {
                            fState = kSendingPlay;
                            fNumSetups = 0;
                        }
                    }               
                }
                break;
            }
            case kSendingPlay:
            {
                if (fPacketRangePlayHeader != NULL)
                    theErr = fClient->SendPacketRangePlay(fPacketRangePlayHeader, fSpeed);
                else
                    theErr = fClient->SendPlay(fStartPlayTimeInSec, fSpeed);
				if ( fVerboseLevel >= 3)
                	qtss_printf("Sent PLAY. Result = %"_U32BITARG_". Response code = %"_U32BITARG_"\n", theErr, fClient->GetStatus());
				//
                // If this PLAY request / response is complete, then we are done with setting
                // up all the client crap. Now all we have to do is receive the data until it's
                // time to send the TEARDOWN
                if (theErr == OS_NoErr)
                {
                    if (fClient->GetStatus() != 200)
                    {
                        theErr = ENOTCONN; // Exit the state machine
                        break;
                    }
                        
                    // Mark down the SSRC for each track, if possible. 
                    for (UInt32 ssrcCount = 0; ssrcCount < fSDPParser.GetNumStreams(); ssrcCount++)
                        fStats[ssrcCount].fServerSSRC = fClient->GetSSRCByTrack(fSDPParser.GetStreamInfo(ssrcCount)->fTrackID);
                    
                    fState = kPlaying;
                    sPlayingConnections++;
                    
                    //
                    // Start our duration timer. Use this to figure out when to send a teardown
                    fPlayTime = fLastRTCPTime = OS::Milliseconds();   
					
					if(fVerboseLevel >= 1)
					{
						for (UInt32 i = 0; i < fSDPParser.GetNumStreams(); i++)
						{
							QTSS_RTPPayloadType type = fSDPParser.GetStreamInfo(i)->fPayloadType;
							qtss_printf("Receiving track %"_U32BITARG_", trackID=%"_U32BITARG_", %s at time %"_S64BITARG_"\n",
								i, fSDPParser.GetStreamInfo(i)->fTrackID,
								type == qtssVideoPayloadType ? "video" : type == qtssAudioPayloadType ? "audio" : "unknown",
								OS::Milliseconds());
						}
					}
                }
                break;
            }
            case kPlaying:
            {
                // Should we send a teardown? We should if either we've been told to teardown, or if our time has run out
                SInt64 curTime = OS::Milliseconds();
                fTotalPlayTime = curTime - fPlayTime;
                if (((curTime - fPlayTime) > fDurationInSec * 1000) || (fTeardownImmediately))
                {
                    qtss_printf("#### kPlaying , 00 fDurationInSec =%ld fPlayTime=%ld, curTime=%ld , cha=%ld\n", fDurationInSec, fPlayTime, curTime, (curTime - fPlayTime));
                    //sPlayingConnections--;
                    //fState = kSendingTeardown;
                    //break;
                }
                if (fTeardownImmediately){
                    qtss_printf("#### kPlaying fTeardownImmediately=true, fState = kSendingTeardown \n");
                    fState = kSendingTeardown;
                    break;
                }

                //Send RTCP if necessary; if we are using TCP for media transport, than 1 set of RTCP total is enough
		        Bool16 sendRTCP = ((curTime - fLastRTCPTime) > fRTCPIntervalInMs) && (fTransportType != kTCPTransportType);
		        sendRTCP |= (fPlayTime == fLastRTCPTime);		//send the first RTCP ASAP.
                if (sendRTCP)
                {
                    //(void) fClient->SendSetParameter(); // test for keep alives and error responses
                    //(void) fClient->SendOptions(); // test for keep alives  and error responses
                    for ( ; fCurRTCPTrack < fSDPParser.GetNumStreams(); fCurRTCPTrack++)
                    {                        
                        OS_Error err = this->SendRTCPPackets(fCurRTCPTrack);
        			   if (fTransportType == kTCPTransportType && err != OS_NoErr)
        			   {
        				  theErr = err; //if error happens on a TCP RTCP socket, then bail        				  
        				  break;
        			   }
                       
                    }
        		      if (theErr != OS_NoErr)
                      {
                        qtss_printf("#### kPlaying , 22 theErr != OS_NoErr \n");
        				break;
                      }
    					
                    //Done sending the RTCP's
                    fCurRTCPTrack = 0;
                    fLastRTCPTime = (curTime == fLastRTCPTime) ? curTime + 1 : curTime;
    				
                    //Drop the POST portion of the HTTP connection after every send
                    if (fControlType == kRTSPHTTPDropPostControlType)
                    {
                         qtss_printf("#### kPlaying , 33 fSocket)->ClosePost\n"); 
					    ((HTTPClientSocket*)fSocket)->ClosePost();
                    }
                }


                // qtss_printf("#### kPlaying , this->ReadMediaData() \n"); 
                //Now read the media data
                // theErr = this->ReadMediaData();

                theErr = this->ReadMediaData2();                
                //return OS_NoErr;
                
                if ((theErr == EINPROGRESS) || (theErr == EAGAIN) || (theErr == OS_NoErr))
					theErr = OS_NoErr; //ignore control flow errors here
				else
				{
				    qtss_printf("#### kPlaying , 44 ReadMediaData theErr=%d\n", theErr); 
                    sPlayingConnections--;
					break;
				}
				curTime = OS::Milliseconds();
				SInt64 nextRTCPTime = fLastRTCPTime + fRTCPIntervalInMs;
                //return curTime < nextRTCPTime ? nextRTCPTime - curTime : fReadInterval;                
				return curTime < nextRTCPTime ? MIN(nextRTCPTime - curTime, fReadInterval) : 1;
            }
			
            case kSendingTeardown:
            {                
                theErr = fClient->SendTeardown();
				//if ( fVerboseLevel >= 3)
                	qtss_printf("Sending TEARDOWN. Result = %"_U32BITARG_". Response code = %"_U32BITARG_"\n", theErr, fClient->GetStatus());
                // Once the TEARDOWN is complete, we are done, so mark ourselves as dead, and wait
                // for the owner of this object to delete us
                if (theErr == OS_NoErr)
                    fState = kDone;
                    
                break;
            }               
        }
    }
    
    if ((theErr == EINPROGRESS) || (theErr == EAGAIN))
    {
        //
        // Request an async event
        fSocket->GetSocket()->SetTask(this);
        fSocket->GetSocket()->RequestEvent(fSocket->GetEventMask());
    }
    else if (theErr != OS_NoErr)
    {
        //
        // We encountered some fatal error with the socket. Record this as a connection failure
        if (fState == kSendingTeardown)
            fDeathReason = kTeardownFailed;
        else if (fState == kPlaying)
            fDeathReason = kDiedWhilePlaying;
        else if (fClient->GetStatus() != 200)
            fDeathReason = kRequestFailed;
        else
            fDeathReason = kConnectionFailed;

        fState = kDone;
    }

	if ( fVerboseLevel >= 2)
    	if (fState == kDone)
        	qtss_printf("Client connection complete. Death reason = %"_U32BITARG_"\n", fDeathReason);

    return 0;
}

void ClientSession::SetupUDPSockets()
{

    static UInt16 sCurrentRTPPortToUse = 16970;
    static const UInt16 kMinRTPPort = 16970;
    static const UInt16 kMaxRTPPort = 36970;

    OS_Error theErr = OS_NoErr;
    
    //
    // Create a UDP socket pair (RTP, RTCP) for each stream
    fUDPSocketArray = NEW UDPSocket*[fSDPParser.GetNumStreams() * 2];
    for (UInt32 x = 0; x < fSDPParser.GetNumStreams() * 2; x++)
    {
        fUDPSocketArray[x] = NEW UDPSocket(this, Socket::kNonBlockingSocketType);
        theErr = fUDPSocketArray[x]->Open();
        if (theErr != OS_NoErr)
        {
            qtss_printf("ClientSession: Failed to open a UDP socket.\n");
            ::exit(-1);
        }
    }
    
    for (UInt32 y = 0; y < fSDPParser.GetNumStreams(); y++)
    {   
         int times = 0;
        for (UInt32 portCheck = 6970; true; portCheck++)
        {            
            theErr = fUDPSocketArray[y * 2]->Bind(INADDR_ANY, sCurrentRTPPortToUse);      
            if (theErr == OS_NoErr)
                {
                    theErr = fUDPSocketArray[(y*2)+1]->Bind(INADDR_ANY, sCurrentRTPPortToUse + 1);
                }
            sCurrentRTPPortToUse += 2;
            if (sCurrentRTPPortToUse > 30000)
                sCurrentRTPPortToUse = 6970;
                
            if (theErr == OS_NoErr)
            {
                 qtss_printf(" ######  fUDPSocketArray:Bind( sCurrentRTPPortToUse = %d) \n", sCurrentRTPPortToUse);
                // This is a good pair. Set the rcv buf on the RTP socket to be really big
                fUDPSocketArray[y * 2]->SetSocketRcvBufSize(fSockRcvBufSize);
                break;
            }
                
            if (sCurrentRTPPortToUse == kMaxRTPPort)
                sCurrentRTPPortToUse = kMinRTPPort;
            times++;
            if (times == 5000)           
            {
                // Make sure we don't loop forever trying to bind a UDP socket. If we can't
                // after a certain point, just bail...
                qtss_printf("ClientSession: Failed to bind a UDP socket.\n");
                ::exit(-1);
            }
        }
    }                       
	if ( fVerboseLevel >= 3)
    	qtss_printf("Opened UDP sockets for %"_U32BITARG_" streams\n", fSDPParser.GetNumStreams());
}


OS_Error ClientSession::ReadMediaData2()
{
    start_thread_for_getmedia();
    
    return OS_NoErr;
}

//Will keep reading until all the packets buffered up has been read.
OS_Error    ClientSession::ReadMediaData()
{
    // For iterating over the array of UDP sockets
    UInt32 theUDPSockIndex = 0;
    OS_Error theErr = OS_NoErr;
	
	while (true)
    {
        //
        // If the media data is being interleaved, get it from the control connection
        UInt32 theTrackID = 0;
        UInt32 theLength = 0;
        Bool16 isRTCP = false;
        char* thePacket = NULL;

        if (fTransportType == kTCPTransportType)
        {
            thePacket = NULL;
            theErr = fClient->GetMediaPacket(&theTrackID, &isRTCP, &thePacket, &theLength);
            // qtss_printf("---------ReadMediaData, kTCPTransportType ,theTrackID=%d\n", theTrackID );
            if (thePacket == NULL)
                break;
        }
        else
        {
            static const UInt32 kMaxPacketSize = 2048;
           // qtss_printf(" ReadMediaData, fTransportType =%d, theUDPSockIndex=%d\n", fTransportType, theUDPSockIndex );
            UInt32 theRemoteAddr = 0;
            UInt16 theRemotePort = 0;
            char thePacketBuf[kMaxPacketSize];
            
            // Get a packet from one of the UDP sockets.
            theErr = fUDPSocketArray[theUDPSockIndex]->RecvFrom(&theRemoteAddr, &theRemotePort,
                                                                &thePacketBuf[0], kMaxPacketSize,
                                                                &theLength);
            if ((theErr != OS_NoErr) || (theLength == 0))
            {
                //Finished processing all the UDP packets that have been buffered up by the lower layer.
                //if ((fTransportType == kReliableUDPTransportType) && (!(theUDPSockIndex & 1)))
                //{
                    // UInt32 trackIndex = TrackID2TrackIndex(fSDPParser.GetStreamInfo(theUDPSockIndex / 2)->fTrackID);
                    // SendAckPackets(trackIndex);
                    /*
                    for (UInt32 trackIndex = 0; trackIndex < fSDPParser.GetNumStreams(); trackIndex++)
                    {
                        if (fSDPParser.GetStreamInfo(trackIndex)->fTrackID == fSDPParser.GetStreamInfo(theUDPSockIndex / 2)->fTrackID)
                        {
                            SendAckPackets(trackIndex);

                            //if (fStats[trackCount].fHighestSeqNumValid)
                                // If we are supposed to be sending acks, and we just finished
                                // receiving all packets for this track that are available at this time,
                                // send an ACK packet
                                //this->AckPackets(trackCount, 0, false);
                         }
                     }
        		*/
                //}
                
                theUDPSockIndex++;
                // qtss_printf(" ReadMediaData,theUDPSockIndex=%d \n", theUDPSockIndex );
                if (theUDPSockIndex == fSDPParser.GetNumStreams() * 2){
                    break;
                }
                continue;
            }
        
            theTrackID = fSDPParser.GetStreamInfo(theUDPSockIndex / 2)->fTrackID;           
            isRTCP = (theUDPSockIndex & 1);
            thePacket = &thePacketBuf[0];
            // UInt32 trackIndex = TrackID2TrackIndex(theTrackID);
            // qtss_printf(" ReadMediaData, fTransportType =%d, theUDPSockIndex=%d isRTCP=%d, theTrackID=%d, trackIndex=%d\n", fTransportType, theUDPSockIndex, isRTCP, theTrackID, trackIndex);
        }

        // qtss_printf(" ReadMediaData, fTransportType =%d, theUDPSockIndex=%d isRTCP=%d\n", fTransportType, theUDPSockIndex, isRTCP );
        //
        // We have a valid packet. Invoke the packet handler function
        if (isRTCP){
            this->ProcessRTCPPacket(thePacket, theLength, theTrackID);
        }else{
            this->ProcessRTPPacket(thePacket, theLength, theTrackID);
        }
    }
    return theErr;
}


void    ClientSession::ProcessRTPPacket(char* inPacket, UInt32 inLength, UInt32 inTrackID)
{
	UInt32 trackIndex = TrackID2TrackIndex(inTrackID);
	if (trackIndex == kUInt32_Max)
	{	if(fVerboseLevel >= 3) 
			qtss_printf("ClientSession::ProcessRTPPacket fatal packet processing error. unknown track\n");
		return;
	}
	
    if(!fvoutput.empty() && trackIndex== 0)
    {
        // qtss_printf("###### vvvvvvvv WritePacket: fvoutput.size()=%d, inTrackID=%d, inLength=%d,  \n", faoutput.size(), inTrackID, inLength);
        putRtpToVideoQueue(inPacket, inLength, qtssWriteFlagsIsRTP);
         
        // for(int i=0; i<fvoutput.size(); i++)
        // {
        //     StrPtrLen strPacket(inPacket, inLength);
        //     SInt64 timeToSendPacket = -1;
        //     SInt64      fTimeArrived = 0;
        //     static UInt64 id = 0;
        //     fvoutput[i]->WritePacket(&strPacket, NULL, qtssWriteFlagsIsRTP, 0, &timeToSendPacket, &id, &fTimeArrived, id==0?true:false );
        //     id++; 
        // }
        // qtss_printf("###### WritePacket: id=%ld, timeToSendPacket=%ld,fTimeArrived=%ld \n", id, timeToSendPacket, fTimeArrived); 
        return;
    }

    if(!faoutput.empty() && trackIndex== 1)
    {
         // qtss_printf("###### WritePacket: faoutput.size()=%d, inTrackID=%d, inLength=%d \n", faoutput.size(), inTrackID, inLength);
         putRtpToAudioQueue(inPacket, inLength, qtssWriteFlagsIsRTP);
         /*
        for(int i=0; i<faoutput.size(); i++)
        { 
            StrPtrLen strPacket(inPacket, inLength);
            SInt64 timeToSendPacket = -1;
            SInt64      fTimeArrived = 0;
            static UInt64 id2 = 0;
            faoutput[i]->WritePacket(&strPacket, NULL, qtssWriteFlagsIsRTP, 0, &timeToSendPacket, &id2, &fTimeArrived, id2==0?true:false );
            id2++; 
        }*/
        // qtss_printf("###### WritePacket: id=%ld, timeToSendPacket=%ld,fTimeArrived=%ld \n", id, timeToSendPacket, fTimeArrived); 
        return ;
    }

    TrackStats &trackStats = fStats[trackIndex];
	if(fVerboseLevel >= 3)
	{
		SInt64 curTime = OS::Milliseconds();
		qtss_printf("Processing incoming packets at time %"_S64BITARG_"\n", curTime);
	}

    //first validate the header and check the SSRC
	Bool16 badPacket = false;
    RTPPacket rtpPacket = RTPPacket(inPacket, inLength);
    if (!rtpPacket.HeaderIsValid())
		badPacket = true;
	else
	{
		if (trackStats.fServerSSRC != 0)
		{
			if (rtpPacket.GetSSRC() != trackStats.fServerSSRC)
				badPacket = true;
		}
		else	//obtain the SSRC from the first packet if it's not available
			trackStats.fServerSSRC = rtpPacket.GetSSRC();
	}
		
	if(badPacket)
	{
        trackStats.fNumMalformedPackets++;
		if (fVerboseLevel >= 1)
			qtss_printf("TrackID=%"_U32BITARG_", len=%"_U32BITARG_"; malformed packet\n", inTrackID, inLength);
        return;
	}

    //Now check the sequence number
    UInt32 packetSeqNum = kUInt32_Max;
    if (trackStats.fHighestSeqNum == kUInt32_Max)     //this is the first sequence number received
        packetSeqNum = trackStats.fBaseSeqNum = trackStats.fHighestSeqNum = static_cast<UInt32>(rtpPacket.GetSeqNum());
    else
        packetSeqNum = CalcSeqNum(trackStats.fHighestSeqNum, rtpPacket.GetSeqNum());

    if (packetSeqNum == kUInt32_Max)            //sequence number is out of range
	{
        trackStats.fNumOutOfBoundPackets++;
		if (fVerboseLevel >= 2)
			qtss_printf("TrackID=%"_U32BITARG_", len=%"_U32BITARG_", seq=%u"", ref(32)=%"_U32BITARG_"; out of bound packet\n",
				inTrackID, inLength, rtpPacket.GetSeqNum(), trackStats.fHighestSeqNum);
	}
    else
    {
		//the packet is good -- update statisics
		Bool16 packetIsOutOfOrder = false;
        if (trackStats.fHighestSeqNum <= packetSeqNum)
            trackStats.fHighestSeqNum = packetSeqNum;
		else
			packetIsOutOfOrder = true;

        fNumPacketsReceived++;
		sPacketsReceived++;
        trackStats.fNumPacketsReceived++;
		fNumBytesReceived += inLength;
		sBytesReceived += inLength;
        trackStats.fNumBytesReceived += inLength;

		//record this sequence number so that it can be acked later on
        if (fTransportType == kReliableUDPTransportType)
            trackStats.fPacketsToAck.push_back(packetSeqNum);
		
		if (fVerboseLevel >= 3)
			qtss_printf("TrackID=%"_U32BITARG_", len=%"_U32BITARG_", seq(32)=%"_U32BITARG_", ref(32)=%"_U32BITARG_"; good packet\n",
				inTrackID, inLength, packetSeqNum, trackStats.fHighestSeqNum);

        //RTP-Meta-Info
        RTPMetaInfoPacket::FieldID* theMetaInfoFields = fClient->GetFieldIDArrayByTrack(inTrackID);
        if (theMetaInfoFields != NULL)
        {
            //
            // This packet is an RTP-Meta-Info packet. Parse it out and print out the results
            RTPMetaInfoPacket theMetaInfoPacket;
            Bool16 packetOK = theMetaInfoPacket.ParsePacket((UInt8*)inPacket, inLength, theMetaInfoFields);
            if (!packetOK)
            {
                if( fVerboseLevel >= 2)
                    qtss_printf("Received invalid RTP-Meta-Info packet\n");
            }
            else if( fVerboseLevel >= 2)
            {
                qtss_printf("---\n");
                qtss_printf("TrackID: %"_U32BITARG_"\n", inTrackID);
                qtss_printf("Packet transmit time: %"_64BITARG_"d\n", theMetaInfoPacket.GetTransmitTime());
                qtss_printf("Frame type: %u\n", theMetaInfoPacket.GetFrameType());
                qtss_printf("Packet number: %"_64BITARG_"u\n", theMetaInfoPacket.GetPacketNumber());
                qtss_printf("Packet position: %"_64BITARG_"u\n", theMetaInfoPacket.GetPacketPosition());
                qtss_printf("Media data length: %"_U32BITARG_"\n", theMetaInfoPacket.GetMediaDataLen());
            }
        }


        if (fEnable3GPP)
        {
            UInt32 timeStamp = rtpPacket.GetTimeStamp();
            Bool16 packetIsDuplicate = fPlayerSimulator.ProcessRTPPacket(trackIndex, inLength, rtpPacket.GetBody().Len, packetSeqNum, timeStamp);
            if(packetIsOutOfOrder && !packetIsDuplicate)
                trackStats.fNumOutOfOrderPackets++;
        }
    }
}


void    ClientSession::ProcessRTCPPacket(char* inPacket, UInt32 inLength, UInt32 inTrackID)
{
	UInt32 trackIndex = TrackID2TrackIndex(inTrackID);
	if (trackIndex == kUInt32_Max)
	{			
		if (fVerboseLevel >= 3) 
			qtss_printf("ClientSession::ProcessRTCPPacket fatal packet processing error. unknown track\n");
		return;
	}
	
    if(!fvoutput.empty() && trackIndex== 0)
    {
        // qtss_printf("###### vvvvvvvv WritePacket: fvoutput.size()=%d, inTrackID=%d, inLength=%d,  \n", faoutput.size(), inTrackID, inLength);
        putRtpToVideoQueue(inPacket, inLength, qtssWriteFlagsIsRTCP);            
        return;
    }

    if(!faoutput.empty() && trackIndex== 1)
    {
         // qtss_printf("###### WritePacket: faoutput.size()=%d, inTrackID=%d, inLength=%d \n", faoutput.size(), inTrackID, inLength);
        putRtpToAudioQueue(inPacket, inLength, qtssWriteFlagsIsRTCP);
        return ;
    }
    
    TrackStats &trackStats = fStats[trackIndex];
	SInt64 curTime = OS::Milliseconds();
	if(fVerboseLevel >= 2)
		qtss_printf("Processing incoming RTCP packets on track %"_U32BITARG_" at time %"_S64BITARG_"\n", trackIndex, curTime);

    //first validate the header and check the SSRC
	RTCPSenderReportPacket packet;
	Bool16 badPacket = !packet.ParseReport(reinterpret_cast<UInt8 *>(inPacket), inLength);
	if (!badPacket)
	{
		if (trackStats.fServerSSRC != 0)
		{
			if (packet.GetPacketSSRC() != trackStats.fServerSSRC){
				badPacket = true;
                     }
		}
		else	//obtain the SSRC from the first packet if it's not available
			trackStats.fServerSSRC = packet.GetPacketSSRC();
	}
		
	if(badPacket)
	{
		if (fVerboseLevel >= 1)
              {
			qtss_printf("TrackID=%"_U32BITARG_", len=%"_U32BITARG_"; malformed RTCP packet\n", inTrackID, inLength);
              }
              return;
	}

	//Now obtains the NTP timestamp and the current time -- we'll need it for the LSR field of the receiver report
	trackStats.fLastSenderReportNTPTime = packet.GetNTPTimeStamp();
	trackStats.fLastSenderReportLocalTime = curTime;
}


void ClientSession::SendAckPackets(UInt32 inTrackIndex)
{
	TrackStats &trackStats = fStats[inTrackIndex];

	if (trackStats.fPacketsToAck.empty())
		return;
    trackStats.fNumAcks++;

	if(fVerboseLevel >= 3)
	{
		SInt64 curTime = OS::Milliseconds();
		qtss_printf("Sending %"_U32BITARG_" acks at time %"_S64BITARG_" on track %"_U32BITARG_"\n",
			trackStats.fPacketsToAck.size(), curTime, inTrackIndex);
	}
		
    char sendBuffer[kMaxUDPPacketSize];
	
    //First send an empty Receivor Report
    RTCPRRPacket RRPacket = RTCPRRPacket(sendBuffer, kMaxUDPPacketSize);
    RRPacket.SetSSRC(trackStats.fClientSSRC);
    StrPtrLen buffer = RRPacket.GetBufferRemaining();

    //Now send the Ack packets
    RTCPAckPacketFmt ackPacket = RTCPAckPacketFmt(buffer);
    ackPacket.SetSSRC(trackStats.fClientSSRC);
    ackPacket.SetAcks(trackStats.fPacketsToAck, trackStats.fServerSSRC);
    trackStats.fPacketsToAck.clear();

    UInt32 packetLength = RRPacket.GetPacketLen() + ackPacket.GetPacketLen();

    // Send the packet
    Assert(trackStats.fDestRTCPPort != 0);
    fUDPSocketArray[(inTrackIndex*2)+1]->SendTo(fSocket->GetHostAddr(), trackStats.fDestRTCPPort, sendBuffer, packetLength);
}

OS_Error ClientSession::SendRTCPPackets(UInt32 trackIndex)
{
	TrackStats &trackStats = fStats[trackIndex];

	char buffer[kMaxUDPPacketSize];
	//::memset(buffer, 0, kMaxUDPPacketSize);

    //First send the RTCP Receiver Report packet
    RTCPRRPacket RRPacket = RTCPRRPacket(buffer, kMaxUDPPacketSize);
    RRPacket.SetSSRC(trackStats.fClientSSRC);

	UInt8 fracLost = 0;
	SInt32 cumLostPackets = 0;
	UInt32 lsr = 0;
	UInt32 dlsr = 0;
	SInt64 curTime = OS::Milliseconds();
	if (trackStats.fHighestSeqNum != kUInt32_Max)
	{
		CalcRTCPRRPacketsLost(trackIndex, fracLost, cumLostPackets);

		//Now get the middle 32 bits of the NTP time stamp and send it as the LSR
		lsr = static_cast<UInt32>(trackStats.fLastSenderReportNTPTime >> 16);

		//Get the time difference expressed as units of 1/65536 seconds
		if (trackStats.fLastSenderReportLocalTime != 0)
			dlsr = static_cast<UInt32>(OS::TimeMilli_To_Fixed64Secs(curTime - trackStats.fLastSenderReportLocalTime) >> 16);

		RRPacket.AddReportBlock(trackStats.fServerSSRC, fracLost, cumLostPackets, trackStats.fHighestSeqNum, lsr, dlsr);
	}

    StrPtrLen remainingBuf = RRPacket.GetBufferRemaining();
	UInt32 *theWriter = reinterpret_cast<UInt32 *>(remainingBuf.Ptr);

    // RECEIVER REPORT
	/*
    *(theWriter++) = htonl(0x81c90007);     // 1 src RR packet
    *(theWriter++) = htonl(0);
    *(theWriter++) = htonl(0);
    *(theWriter++) = htonl(0);
    *(theWriter++) = htonl(trackStats.fHighestSeqNum == kUInt32_Max ? 0 : trackStats.fHighestSeqNum);				//EHSN
    *(theWriter++) = 0;                         // don't do jitter yet.
    *(theWriter++) = 0;                         // don't do last SR timestamp
    *(theWriter++) = 0;                         // don't do delay since last SR
	*/

	//The implementation should be sending an SDES to conform to the standard...but its not done.
	
	if(fTransportType == kRTSPReliableUDPClientType)
	{
		// APP PACKET - QoS info
		*(theWriter++) = htonl(0x80CC000C); 
		//*(ia++) = htonl(trk[i].TrackSSRC);
		*(theWriter++) = htonl(trackStats.fClientSSRC);
	// this QTSS changes after beta to 'qtss'
		*(theWriter++) = htonl(FOUR_CHARS_TO_INT('Q', 'T', 'S', 'S'));
		//*(ia++) = toBigEndian_ulong(trk[i].rcvrSSRC);
		*(theWriter++) = htonl(trackStats.fServerSSRC);
		*(theWriter++) = htonl(8);          // number of 4-byte quants below
	#define RR 0x72720004
	#define PR 0x70720004
	#define PD 0x70640002
	#define OB 0x6F620004
		*(theWriter++) = htonl(RR);
		//unsigned int now, secs;
		//now = microseconds();
		//secs = now - trk[i].last_rtcp_packet_sent_us / USEC_PER_SEC;
		//if (secs)
		//  temp = trk[i].bytes_received_since_last_rtcp / secs;
		//else
		//  temp = 0;
		//*(ia++) = htonl(temp);
		*(theWriter++) = htonl(0);
		*(theWriter++) = htonl(PR);
		//*(ia++) = htonl(trk[i].rtp_num_received);
		*(theWriter++) = htonl(0);
		//*(theWriter++) = htonl(PL);
		//*(ia++) = htonl(trk[i].rtp_num_lost);
		//*(theWriter++) = htonl(0);
		*(theWriter++) = htonl(OB);
		*(theWriter++) = htonl(fOverbufferWindowSizeInK * 1024);
		*(theWriter++) = htonl(PD);
		*(theWriter++) = htonl(0);      // should be a short, but we need to pad to a long for the entire RTCP app packet
	}

	char *buf = reinterpret_cast<char *>(theWriter);
	UInt32 packetLen = buf - buffer;
	UInt32 playoutDelay = 0;

	if (fEnable3GPP && trackStats.fHighestSeqNum != kUInt32_Max)
	{
		//now add the RTCP NADU packet
		RTCPNADUPacketFmt nadu = RTCPNADUPacketFmt(buf, kMaxUDPPacketSize - packetLen);
		nadu.SetSSRC(trackStats.fClientSSRC);
		
		//If there is no packet in the buffer, use the extended highest sequence number received + 1
		UInt32 seqNum = fPlayerSimulator.GetNextSeqNumToDecode(trackIndex);
		if(seqNum == kUInt32_Max)
			seqNum = trackStats.fHighestSeqNum + 1;
			
		if (fEnableForcePlayoutDelay)
			playoutDelay = fPlayoutDelay;
		else
			playoutDelay = fPlayerSimulator.GetPlayoutDelay(trackIndex);

		nadu.AddNADUBlock(0, seqNum, 0, fPlayerSimulator.GetFreeBufferSpace(trackIndex), playoutDelay); 

		packetLen += nadu.GetPacketLen();
	}
	
	if(fVerboseLevel >= 2)
	{
		qtss_printf("Sending receiver report at time %"_S64BITARG_" on track %"_U32BITARG_"; lostFrac=%u, lost=%"_S32BITARG_
			", FBS=%"_U32BITARG_", delay=%"_U32BITARG_", lsr=%"_U32BITARG_", dlsr=%"_U32BITARG_"\n",
			curTime, trackIndex, fracLost, cumLostPackets,
			fPlayerSimulator.GetFreeBufferSpace(trackIndex),
			playoutDelay,
			lsr, dlsr);
	}
		
    // Send the packet
    if (fUDPSocketArray != NULL)
    {
        Assert(trackStats.fDestRTCPPort != 0);
        fUDPSocketArray[(trackIndex*2)+1]->SendTo(fSocket->GetHostAddr(), trackStats.fDestRTCPPort, buffer, packetLen);
    }
    else
    {
        OS_Error theErr = fClient->PutMediaPacket(fSDPParser.GetStreamInfo(trackIndex)->fTrackID, true, buffer, packetLen);
        if (theErr != OS_NoErr)
            return theErr;

    }
    return OS_NoErr;
}


//The lost packets in the RTCP RR are defined differently then the GetNumPacketsLost function. See  RFC 3550 6.4.1 and A.3
void ClientSession::CalcRTCPRRPacketsLost(UInt32 trackIndex, UInt8 &outFracLost, SInt32 &outCumLostPackets)
{
	TrackStats &trackStats = fStats[trackIndex];

	if (trackStats.fHighestSeqNum == kUInt32_Max)
	{
		outFracLost = 0;
		outCumLostPackets = 0;
		return;
	}

	UInt32 expected = trackStats.fHighestSeqNum - trackStats.fBaseSeqNum + 1;
	UInt32 expectedInterval = expected - trackStats.fExpectedPrior;
	UInt32 receivedInterval = trackStats.fNumPacketsReceived - trackStats.fReceivedPrior;

	trackStats.fExpectedPrior = expected;
	trackStats.fReceivedPrior = trackStats.fNumPacketsReceived;

	if (expectedInterval == 0 || expectedInterval < receivedInterval)
		outFracLost = 0;
	else
	{
		UInt32 lostInterval = expectedInterval - receivedInterval;
		outFracLost = static_cast<UInt8>((lostInterval << 8) / expectedInterval);
	}
	outCumLostPackets = expected - trackStats.fNumPacketsReceived;
}

//If newSeqNum is no more than kMaxMisorder behind and kMaxDropOut ahead of referenceSeqNum(modulo 2^16), returns the corresponding
//32 bit sequence number.  Otherwise returns kUInt32_Max
UInt32 ClientSession::CalcSeqNum(UInt32 referenceSeqNum, UInt16 newSeqNum)
{
		
    UInt16 refSeqNum16 = static_cast<UInt16>(referenceSeqNum);
    UInt32 refSeqNumHighBits = referenceSeqNum >> 16;

	if (static_cast<UInt16>(newSeqNum - refSeqNum16) <= kMaxDropOut)
	{
		//new sequence number is ahead and is in range
		if (newSeqNum >= refSeqNum16)
			return (refSeqNumHighBits << 16) | newSeqNum;			//no wrap-around
		else
			return ((refSeqNumHighBits + 1) << 16) | newSeqNum;		//wrapped-around
	}
	else if (static_cast<UInt16>(refSeqNum16 - newSeqNum) < kMaxMisorder)
	{
		//new sequence number is behind and is in range
		if (newSeqNum < refSeqNum16)
			return (refSeqNumHighBits << 16) | newSeqNum;			//no wrap-around
		else if (refSeqNumHighBits != 0)
			return ((refSeqNumHighBits - 1) << 16) | newSeqNum;		//wrapped-around
	}
	return kUInt32_Max;												//bad sequence number(out of range)
	/*
    if (refSeqNum16 <= newSeqNum)
    {
        UInt16 diff = newSeqNum - refSeqNum16;
        if (diff <= kMaxDropOut)                                    //new sequence number is ahead and is in range, no overflow
            return (refSeqNumHighBits << 16) | newSeqNum;

        diff = kUInt16_Max - newSeqNum;
        diff += refSeqNum16 + 1;
        if (diff <= kMaxMisorder && refSeqNumHighBits != 0)         //new sequence number is behind, underflow
            return ((refSeqNumHighBits - 1) << 16) | newSeqNum;
    }
    else
    {
        UInt16 diff = refSeqNum16 - newSeqNum;
        if (diff <= kMaxMisorder)                                   //new sequence number is behind and is in range, no underflow
            return (refSeqNumHighBits << 16) | newSeqNum;

        diff = kUInt16_Max - refSeqNum16;
        diff += newSeqNum + 1;
        if (diff <= kMaxDropOut)                                    //new sequence number is ahead and is in range, overflow
            return ((refSeqNumHighBits + 1) << 16) | newSeqNum;
    }
    return kUInt32_Max;                                             //Bad sequence number
	*/
}

void ClientSession::AddOutput(ReflectorOutput* inOutput, Bool16 isVideo)
{    
    //OSMutexLocker locker(&fOutputMutex);
    if (NULL != inOutput)
    {
        inOutput->fState = 0; 
        if(isVideo)
            fvoutput.push_back(inOutput);
        else
            faoutput.push_back(inOutput);
    }
} 

void ClientSession::RemoveOutput(ReflectorOutput* inOutput, Bool16 isVideo)
{
    //OSMutexLocker locker(&fOutputMutex);
    if (NULL != inOutput)
    {        
        //inOutput->TearDown();//lyz@xdja.com 

        if(isVideo)
        {
            UInt32 pos = fvoutput.find(inOutput);
            if(pos != fvoutput.size())
                fvoutput.erase(pos);
        }
        else
        {
            UInt32 pos = faoutput.find(inOutput);
            if(pos != faoutput.size())
                faoutput.erase(pos);
        }

        qtss_printf("remove output:%08x \n", inOutput);
        
        delete inOutput;
    }
}

void* thread_for_video(void *parms){
    ClientSession* clises = (ClientSession*) parms;

    UInt32 theUDPSockIndex = 0; // video
    OS_Error theErr = OS_NoErr;
    
    // If the media data is being interleaved, get it from the control connection
    UInt32 theTrackID = 0;
    UInt32 theLength = 0;
    Bool16 isRTCP = false;
    char* thePacket = NULL;
        
    static const UInt32 kMaxPacketSize = 2048;
    qtss_printf(" thread_for_video, clises->fState=%d \n", clises->fState);
    UInt32 theRemoteAddr = 0;
    UInt16 theRemotePort = 0;
    char thePacketBuf[kMaxPacketSize];

    theTrackID = clises->fSDPParser.GetStreamInfo(theUDPSockIndex / 2)->fTrackID;    
    
    while(clises->fState == ClientSession::kPlaying )
    {
        if (clises->fTransportType == ClientSession::kTCPTransportType)
        {
            thePacket = NULL;
            theErr = clises->fClient->GetMediaPacket(&theTrackID, &isRTCP, &thePacket, &theLength);
            // qtss_printf("---------ReadMediaData, kTCPTransportType ,theTrackID=%d\n", theTrackID );
            if (thePacket == NULL)
                //break;
                continue;     
        }
        else
        {           
            // Get a packet from one of the UDP sockets.
            theErr = clises->fUDPSocketArray[theUDPSockIndex]->RecvFrom(&theRemoteAddr, &theRemotePort,
                                                                &thePacketBuf[0], kMaxPacketSize,
                                                                &theLength);
            if ((theErr != OS_NoErr) || (theLength == 0))
            {
                 continue;  
            }
        
            // theTrackID = clises->fSDPParser.GetStreamInfo(theUDPSockIndex / 2)->fTrackID;           
            // isRTCP = (theUDPSockIndex & 1);
            thePacket = &thePacketBuf[0];
            // UInt32 trackIndex = TrackID2TrackIndex(theTrackID);
            // qtss_printf(" ReadMediaData, fTransportType =%d, theUDPSockIndex=%d isRTCP=%d, theTrackID=%d, trackIndex=%d\n", fTransportType, theUDPSockIndex, isRTCP, theTrackID, trackIndex);        
         }
          //qtss_printf(" thread_for_video,  theUDPSockIndex=%d, theTrackID=%d, theLength=%d\n",  theUDPSockIndex, theTrackID , theLength);
          clises->fthreadMutex.Lock();   
          clises->ProcessRTPPacket(thePacket, theLength, theTrackID);
          clises->fthreadMutex.Unlock();
    }
    pthread_join(clises->fid_vthread, NULL);
    clises->fid_vthread = -1;
    return NULL;
}

void* thread_for_audio(void *parms){
    ClientSession* clises = (ClientSession*) parms;

    UInt32 theUDPSockIndex = 2; //audio
    OS_Error theErr = OS_NoErr;
    
    // If the media data is being interleaved, get it from the control connection
    UInt32 theTrackID = 0;
    UInt32 theLength = 0;
    Bool16 isRTCP = false;
    char* thePacket = NULL;
        
    static const UInt32 kMaxPacketSize = 2048;
    // qtss_printf(" ReadMediaData, fTransportType =%d, theUDPSockIndex=%d\n", fTransportType, theUDPSockIndex );
    UInt32 theRemoteAddr = 0;
    UInt16 theRemotePort = 0;
    char thePacketBuf[kMaxPacketSize];

    theTrackID = clises->fSDPParser.GetStreamInfo(theUDPSockIndex / 2)->fTrackID;    
    
    while(clises->fState == ClientSession::kPlaying )
    {
        if (clises->fTransportType == ClientSession::kTCPTransportType)
        {
            thePacket = NULL;
            theErr = clises->fClient->GetMediaPacket(&theTrackID, &isRTCP, &thePacket, &theLength);
            // qtss_printf("---------ReadMediaData, kTCPTransportType ,theTrackID=%d\n", theTrackID );
            if (thePacket == NULL)
                //break;
                continue;     
        }
        else
        {
            // Get a packet from one of the UDP sockets.
            theErr = clises->fUDPSocketArray[theUDPSockIndex]->RecvFrom(&theRemoteAddr, &theRemotePort,
                                                                &thePacketBuf[0], kMaxPacketSize,
                                                                &theLength);
            if ((theErr != OS_NoErr) || (theLength == 0))
            {
                 continue;  
            }
        
            // theTrackID = clises->fSDPParser.GetStreamInfo(theUDPSockIndex / 2)->fTrackID;           
            // isRTCP = (theUDPSockIndex & 1);
            thePacket = &thePacketBuf[0];
            // UInt32 trackIndex = TrackID2TrackIndex(theTrackID);
            // qtss_printf(" ReadMediaData, fTransportType =%d, theUDPSockIndex=%d isRTCP=%d, theTrackID=%d, trackIndex=%d\n", fTransportType, theUDPSockIndex, isRTCP, theTrackID, trackIndex);
        
         }
         // qtss_printf(" thread_for_audio,  theUDPSockIndex=%d, theTrackID=%d, theLength=%d\n",  theUDPSockIndex, theTrackID , theLength);
          clises->fthreadMutex.Lock();   
          clises->ProcessRTPPacket(thePacket, theLength, theTrackID);
          clises->fthreadMutex.Unlock();

    }
    pthread_join(clises->fid_athread, NULL);
    clises->fid_athread = -1;
    return NULL;
}

void* thread_for_rtcp(void *parms){
    ClientSession* clises = (ClientSession*) parms;

    UInt32 theUDPSockIndex = 1; //1 :video rtcp; 3: audio rtcp
    OS_Error theErr = OS_NoErr;
    
    // If the media data is being interleaved, get it from the control connection
    UInt32 theTrackID = 0;
    UInt32 theLength = 0;
    Bool16 isRTCP = true;
    char* thePacket = NULL;
        
    static const UInt32 kMaxPacketSize = 2048;
    // qtss_printf(" ReadMediaData, fTransportType =%d, theUDPSockIndex=%d\n", fTransportType, theUDPSockIndex );
    UInt32 theRemoteAddr = 0;
    UInt16 theRemotePort = 0;
    char thePacketBuf[kMaxPacketSize];    
    
    while(clises->fState == ClientSession::kPlaying )
    {
        if (clises->fTransportType == ClientSession::kTCPTransportType)
        {
            thePacket = NULL;
            theErr = clises->fClient->GetMediaPacket(&theTrackID, &isRTCP, &thePacket, &theLength);
            // qtss_printf("---------ReadMediaData, kTCPTransportType ,theTrackID=%d\n", theTrackID );
            if (thePacket == NULL)
                //break;
                continue;     
        }
        else
        {          
            // Get a packet from one of the UDP sockets.
            theErr = clises->fUDPSocketArray[theUDPSockIndex]->RecvFrom(&theRemoteAddr, &theRemotePort,
                                                                &thePacketBuf[0], kMaxPacketSize,
                                                                &theLength);
            if ((theErr != OS_NoErr) || (theLength == 0))
            {                 
                theUDPSockIndex += 2;
                // qtss_printf(" ReadMediaData,theUDPSockIndex=%d \n", theUDPSockIndex );
                if (theUDPSockIndex >= clises->fSDPParser.GetNumStreams() * 2){
                    theUDPSockIndex = 1;
                }                
                continue;
            }
        
             theTrackID = clises->fSDPParser.GetStreamInfo(theUDPSockIndex / 2)->fTrackID;           
            // isRTCP = (theUDPSockIndex & 1);
            thePacket = &thePacketBuf[0];
            // UInt32 trackIndex = TrackID2TrackIndex(theTrackID);            
        
         } 
         // qtss_printf(" thread_for_rtcp,  theUDPSockIndex=%d, theTrackID=%d, theLength=%d\n",  theUDPSockIndex, theTrackID , theLength);
          clises->fthreadMutex.Lock(); 
          clises->ProcessRTCPPacket(thePacket, theLength, theTrackID);        
          clises->fthreadMutex.Unlock(); 
    }
    pthread_join(clises->fid_rthread, NULL);
    clises->fid_rthread = -1;
    return NULL;
}


void ClientSession::start_thread_for_getmedia()
{
    if(fid_vthread == -1)
    {
        int ret = pthread_create(&fid_vthread, NULL, thread_for_video, (void*)this);
        if (ret != 0){
            qtss_printf("pthread_create_video err:%d\n", ret);
        }else{
            qtss_printf("thread_for_video create OK\n");
        }
    }

    if(fid_athread == -1)
    {
        int ret = pthread_create(&fid_athread, NULL, thread_for_audio, (void*)this);
        if (ret != 0){
            qtss_printf("pthread_create_audio err:%d\n", ret);
        }else{
            qtss_printf("thread_for_audio create OK\n");
        }
    }
    
    if(fid_rthread == -1)
    {
        int ret = pthread_create(&fid_rthread, NULL, thread_for_rtcp, (void*)this);
        if (ret != 0){
            qtss_printf("pthread_create_rtcp err:%d\n", ret);
        }else{
            qtss_printf("thread_for_rtcp create OK\n");
        }
    }
}

