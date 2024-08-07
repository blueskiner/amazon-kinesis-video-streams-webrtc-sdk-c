/**
 * Implementation of a API calls based on LibWebSocket
 */
#define LOG_CLASS "LwsApiCalls"
#include "../Include_i.h"

extern CHAR gZlmOffer[MAX_SESSION_DESCRIPTION_INIT_SDP_LEN + 1];

static BOOL gInterruptedFlagBySignalingHandler;
VOID lwsSignalHandler(INT32 signal)
{
    UNUSED_PARAM(signal);
    gInterruptedFlagBySignalingHandler = TRUE;
}

STATUS signalingSignRequestInfo(PRequestInfo pRequestInfo)
{
    ENTERS();
    STATUS retStatus = STATUS_SUCCESS;
    UINT32 len;
    PCHAR pHostStart, pHostEnd, pSignatureInfo = NULL;
    CHAR dateTimeStr[17];
    CHAR contentLenBuf[16];

//    CHK(pRequestInfo != NULL && pRequestInfo->pAwsCredentials != NULL, STATUS_NULL_ARG);

    // Generate the time
//    CHK_STATUS(generateSignatureDateTime(pRequestInfo->currentTime, dateTimeStr));

    // Get the host header
    CHK_STATUS(getRequestHost(pRequestInfo->url, &pHostStart, &pHostEnd));
    len = (UINT32) (pHostEnd - pHostStart);

    CHK_STATUS(setRequestHeader(pRequestInfo, "Accept", 0, "application/json, text/plain, */*", 0));
//    CHK_STATUS(setRequestHeader(pRequestInfo, AWS_SIG_V4_HEADER_HOST, 0, pHostStart, len));
//    CHK_STATUS(setRequestHeader(pRequestInfo, AWS_SIG_V4_HEADER_AMZ_DATE, 0, dateTimeStr, 0));
    CHK_STATUS(setRequestHeader(pRequestInfo, "Content-Type", 0, "text/plain;charset=utf-8", 0));

    // Set the content-length
    if (pRequestInfo->body != NULL) {
        CHK_STATUS(ULTOSTR(pRequestInfo->bodySize, contentLenBuf, SIZEOF(contentLenBuf), 10, NULL));
        CHK_STATUS(setRequestHeader(pRequestInfo, "Content-Length", 0, contentLenBuf, 0));
    }

    // Generate the signature
//    CHK_STATUS(generateAwsSigV4Signature(pRequestInfo, dateTimeStr, TRUE, &pSignatureInfo, &len));

    // Set the header
//    CHK_STATUS(setRequestHeader(pRequestInfo, AWS_SIG_V4_HEADER_AUTH, 0, pSignatureInfo, len));

    // Set the security token header if provided
//    if (pRequestInfo->pAwsCredentials->sessionTokenLen != 0) {
//        CHK_STATUS(setRequestHeader(pRequestInfo, AWS_SIG_V4_HEADER_AMZ_SECURITY_TOKEN, 0, pRequestInfo->pAwsCredentials->sessionToken,
//                                    pRequestInfo->pAwsCredentials->sessionTokenLen));
//    }

CleanUp:

    SAFE_MEMFREE(pSignatureInfo);

    CHK_LOG_ERR(retStatus);

    LEAVES();
    return retStatus;
}

INT32 lwsHttpCallbackRoutine(struct lws* wsi, enum lws_callback_reasons reason, PVOID user, PVOID pDataIn, size_t dataSize)
{
    UNUSED_PARAM(user);
    STATUS retStatus = STATUS_SUCCESS;
    PVOID customData;
    INT32 status, retValue = 0, size;
    PCHAR pCurPtr, pBuffer;
    CHAR dateHdrBuffer[MAX_DATE_HEADER_BUFFER_LENGTH + 1];
    PBYTE pEndPtr;
    PBYTE* ppStartPtr;
    PLwsCallInfo pLwsCallInfo;
    PRequestInfo pRequestInfo = NULL;
    PSingleListNode pCurNode;
    UINT64 item, serverTime;
    UINT32 headerCount;
    UINT32 logLevel;
    PRequestHeader pRequestHeader;
    PSignalingClient pSignalingClient = NULL;
    BOOL locked = FALSE;
    time_t td;
    SIZE_T len;
    UINT64 nowTime, clockSkew = 0;
    PStateMachineState pStateMachineState;
    BOOL skewMapContains = FALSE;

    DLOGV("HTTPS callback with reason %d", reason);

    // Early check before accessing the custom data field to see if we are interested in processing the message
    switch (reason) {
        case LWS_CALLBACK_CLIENT_CONNECTION_ERROR:
        case LWS_CALLBACK_CLOSED_CLIENT_HTTP:
        case LWS_CALLBACK_ESTABLISHED_CLIENT_HTTP:
        case LWS_CALLBACK_RECEIVE_CLIENT_HTTP_READ:
        case LWS_CALLBACK_RECEIVE_CLIENT_HTTP:
        case LWS_CALLBACK_COMPLETED_CLIENT_HTTP:
        case LWS_CALLBACK_CLIENT_APPEND_HANDSHAKE_HEADER:
        case LWS_CALLBACK_CLIENT_HTTP_WRITEABLE:
            break;
        default:
            CHK(FALSE, retStatus);
    }

    customData = lws_get_opaque_user_data(wsi);
    pLwsCallInfo = (PLwsCallInfo) customData;

    lws_set_log_level(LLL_NOTICE | LLL_WARN | LLL_ERR, NULL);

    CHK(pLwsCallInfo != NULL &&
        pLwsCallInfo->pSignalingClient != NULL &&
        pLwsCallInfo->pSignalingClient->pLwsContext != NULL &&
        pLwsCallInfo->callInfo.pRequestInfo != NULL &&
        pLwsCallInfo->protocolIndex == PROTOCOL_INDEX_HTTPS,
        retStatus);

    // Quick check whether we need to exit
    if (ATOMIC_LOAD(&pLwsCallInfo->cancelService)) {
        retValue = 1;
        ATOMIC_STORE_BOOL(&pRequestInfo->terminating, TRUE);
        CHK(FALSE, retStatus);
    }

    pSignalingClient = pLwsCallInfo->pSignalingClient;
    nowTime = SIGNALING_GET_CURRENT_TIME(pSignalingClient);

    pRequestInfo = pLwsCallInfo->callInfo.pRequestInfo;
    pBuffer = pLwsCallInfo->buffer + LWS_PRE;

    logLevel = loggerGetLogLevel();

    MUTEX_LOCK(pSignalingClient->lwsServiceLock);
    locked = TRUE;

    switch (reason) {
        case LWS_CALLBACK_CLIENT_CONNECTION_ERROR:
            pCurPtr = pDataIn == NULL ? "(None)" : (PCHAR) pDataIn;
            DLOGW("Client connection failed. Connection error string: %s", pCurPtr);
            STRNCPY(pLwsCallInfo->callInfo.errorBuffer, pCurPtr, CALL_INFO_ERROR_BUFFER_LEN);

            // TODO: Attempt to get more meaningful service return code

            ATOMIC_STORE_BOOL(&pRequestInfo->terminating, TRUE);
            ATOMIC_STORE(&pLwsCallInfo->pSignalingClient->result, (SIZE_T) SERVICE_CALL_UNKNOWN);

            break;

        case LWS_CALLBACK_CLOSED_CLIENT_HTTP:
            DLOGD("Client http closed");
            ATOMIC_STORE_BOOL(&pRequestInfo->terminating, TRUE);

            break;

        case LWS_CALLBACK_ESTABLISHED_CLIENT_HTTP:
            status = (INT32)lws_http_client_http_response(wsi);
            getStateMachineCurrentState(pSignalingClient->pStateMachine, &pStateMachineState);

            DLOGD("Connected with server response: %d", status);
            //pLwsCallInfo->callInfo.callResult = getServiceCallResultFromHttpStatus((UINT32) status);
            pLwsCallInfo->callInfo.callResult = status;

            len = (SIZE_T) lws_hdr_copy(wsi, &dateHdrBuffer[0], MAX_DATE_HEADER_BUFFER_LENGTH, WSI_TOKEN_HTTP_DATE);

            time(&td);

            if (len) {
                // on failure to parse lws_http_date_unix returns non zero value
                if (0 == lws_http_date_parse_unix(&dateHdrBuffer[0], len, &td)) {
                    DLOGV("Date Header Returned By Server:  %s", dateHdrBuffer);

                    serverTime = ((UINT64) td) * HUNDREDS_OF_NANOS_IN_A_SECOND;

                    if (serverTime > nowTime + MIN_CLOCK_SKEW_TIME_TO_CORRECT) {
                        // Server time is ahead
                        clockSkew = (serverTime - nowTime);
                        DLOGD("Detected Clock Skew!  Server time is AHEAD of Device time: Server time: %" PRIu64 ", now time: %" PRIu64, serverTime,
                              nowTime);
                    } else if (nowTime > serverTime + MIN_CLOCK_SKEW_TIME_TO_CORRECT) {
                        clockSkew = (nowTime - serverTime);
                        clockSkew |= ((UINT64) (1ULL << 63));
                        DLOGD("Detected Clock Skew!  Device time is AHEAD of Server time: Server time: %" PRIu64 ", now time: %" PRIu64, serverTime,
                              nowTime);
                        // PIC hashTable implementation only stores UINT64 so I will flip the sign of the msb
                        // This limits the range of the max clock skew we can represent to just under 2925 years.
                    }

                    hashTableContains(pSignalingClient->diagnostics.pEndpointToClockSkewHashMap, pStateMachineState->state, &skewMapContains);
                    if (clockSkew > 0) {
                        hashTablePut(pSignalingClient->diagnostics.pEndpointToClockSkewHashMap, pStateMachineState->state, clockSkew);
                    } else if (clockSkew == 0 && skewMapContains) {
                        // This means the item is in the map so at one point there was a clock skew offset but it has been corrected
                        // So we should no longer be correcting for a clock skew, remove this item from the map
                        hashTableRemove(pSignalingClient->diagnostics.pEndpointToClockSkewHashMap, pStateMachineState->state);
                    }
                }
            }

            // Store the Request ID header
            if ((size = lws_hdr_custom_copy(wsi, pBuffer, LWS_SCRATCH_BUFFER_SIZE, SIGNALING_REQUEST_ID_HEADER_NAME,
                                            (SIZEOF(SIGNALING_REQUEST_ID_HEADER_NAME) - 1) * SIZEOF(CHAR))) > 0) {
                pBuffer[size] = '\0';
                DLOGI("Request ID: %s", pBuffer);
            }

            break;

        case LWS_CALLBACK_RECEIVE_CLIENT_HTTP_READ:
            DLOGD("Received client http read: %d bytes", (INT32) dataSize);
            lwsl_hexdump_info(pDataIn, dataSize);

            if (dataSize != 0) {
                CHK(NULL != (pLwsCallInfo->callInfo.responseData = (PCHAR) MEMALLOC(dataSize + 1)), STATUS_NOT_ENOUGH_MEMORY);
                MEMCPY(pLwsCallInfo->callInfo.responseData, pDataIn, dataSize);
                pLwsCallInfo->callInfo.responseData[dataSize] = '\0';
                pLwsCallInfo->callInfo.responseDataLen = (UINT32) dataSize;

                if (pLwsCallInfo->callInfo.callResult != SERVICE_CALL_RESULT_OK) {
                    DLOGW("Received client http read response:  %s", pLwsCallInfo->callInfo.responseData);
                    if (pLwsCallInfo->callInfo.callResult == SERVICE_CALL_FORBIDDEN) {
                        if (isCallResultSignatureExpired(&pLwsCallInfo->callInfo)) {
                            // Set more specific result, this is so in the state machine
                            // We don't call GetToken again rather RETRY the existing API (now with clock skew correction)
                            pLwsCallInfo->callInfo.callResult = SERVICE_CALL_SIGNATURE_EXPIRED;
                        } else if (isCallResultSignatureNotYetCurrent(&pLwsCallInfo->callInfo)) {
                            // server time is ahead
                            pLwsCallInfo->callInfo.callResult = SERVICE_CALL_SIGNATURE_NOT_YET_CURRENT;
                        }
                    }

                } else {
                    DLOGV("Received client http read response:  %s", pLwsCallInfo->callInfo.responseData);
                }
            }

            break;

        case LWS_CALLBACK_RECEIVE_CLIENT_HTTP:
            DLOGD("Received client http");
            size = LWS_SCRATCH_BUFFER_SIZE;

            if (lws_http_client_read(wsi, &pBuffer, &size) < 0) {
                retValue = -1;
            }

            break;

        case LWS_CALLBACK_COMPLETED_CLIENT_HTTP:
            DLOGD("Http client completed");
            break;

        case LWS_CALLBACK_CLIENT_APPEND_HANDSHAKE_HEADER:
            DLOGD("Client append handshake header\n");

            CHK_STATUS(singleListGetNodeCount(pRequestInfo->pRequestHeaders, &headerCount));
            ppStartPtr = (PBYTE*) pDataIn;
            pEndPtr = *ppStartPtr + dataSize - 1;

            // Iterate through the headers
            while (headerCount != 0) {
                CHK_STATUS(singleListGetHeadNode(pRequestInfo->pRequestHeaders, &pCurNode));
                CHK_STATUS(singleListGetNodeData(pCurNode, &item));

                pRequestHeader = (PRequestHeader) item;

                // Append the colon at the end of the name
                if (pRequestHeader->pName[pRequestHeader->nameLen - 1] != ':') {
                    STRCPY(pBuffer, pRequestHeader->pName);
                    pBuffer[pRequestHeader->nameLen] = ':';
                    pBuffer[pRequestHeader->nameLen + 1] = '\0';
                    pRequestHeader->pName = pBuffer;
                    pRequestHeader->nameLen++;
                }

                DLOGV("Appending header - %s %s", pRequestHeader->pName, pRequestHeader->pValue);

                status = lws_add_http_header_by_name(wsi, (PBYTE) pRequestHeader->pName, (PBYTE) pRequestHeader->pValue, pRequestHeader->valueLen,
                                                     ppStartPtr, pEndPtr);
                if (status != 0) {
                    retValue = 1;
                    CHK(FALSE, retStatus);
                }

                // Remove the head
                CHK_STATUS(singleListDeleteHead(pRequestInfo->pRequestHeaders));
                MEMFREE(pRequestHeader);

                // Decrement to iterate
                headerCount--;
            }

            lws_client_http_body_pending(wsi, 1);
            lws_callback_on_writable(wsi);

            break;

        case LWS_CALLBACK_CLIENT_HTTP_WRITEABLE:
            DLOGD("Sending the body %.*s, size %d", pRequestInfo->bodySize, pRequestInfo->body, pRequestInfo->bodySize);
            MEMCPY(pBuffer, pRequestInfo->body, pRequestInfo->bodySize);

            size = lws_write(wsi, (PBYTE) pBuffer, (SIZE_T) pRequestInfo->bodySize, LWS_WRITE_TEXT);

            if (size != (INT32) pRequestInfo->bodySize) {
                DLOGW("Failed to write out the body of POST request entirely. Expected to write %d, wrote %d", pRequestInfo->bodySize, size);
                if (size > 0) {
                    // Schedule again
                    lws_client_http_body_pending(wsi, 1);
                    lws_callback_on_writable(wsi);
                } else {
                    // Quit
                    retValue = 1;
                }
            } else {
                lws_client_http_body_pending(wsi, 0);
            }

            break;

        default:
            break;
    }

CleanUp:

    if (STATUS_FAILED(retStatus)) {
        DLOGW("Failed in HTTPS handling routine with 0x%08x", retStatus);
        if (pRequestInfo != NULL) {
            ATOMIC_STORE_BOOL(&pRequestInfo->terminating, TRUE);
        }

        lws_cancel_service(lws_get_context(wsi));

        retValue = -1;
    }

    if (locked) {
        MUTEX_UNLOCK(pSignalingClient->lwsServiceLock);
    }

    return retValue;
}

STATUS lwsCompleteSync(PLwsCallInfo pLwsCallInfo)
{
    ENTERS();
    STATUS retStatus = STATUS_SUCCESS;
    volatile INT32 retVal = 0;
    PCHAR pHostStart, pHostEnd, pVerb;
    struct lws_client_connect_info connectInfo;
    struct lws* clientLws;
    struct lws_context* pContext;
    BOOL secureConnection, locked = FALSE, serializerLocked = FALSE, iterate = TRUE;
    CHAR path[MAX_URI_CHAR_LEN + 1];
    CHAR contentLenBuf[16];

    CHK(pLwsCallInfo != NULL && pLwsCallInfo->callInfo.pRequestInfo != NULL && pLwsCallInfo->pSignalingClient != NULL, STATUS_NULL_ARG);

    CHK_STATUS(requestRequiresSecureConnection(pLwsCallInfo->callInfo.pRequestInfo->url, &secureConnection));
    DLOGV("Perform %s synchronous call for URL: %s", secureConnection ? "secure" : EMPTY_STRING, pLwsCallInfo->callInfo.pRequestInfo->url);

    pVerb = HTTP_REQUEST_VERB_POST_STRING;

    // Sign the request
    CHK_STATUS(signalingSignRequestInfo(pLwsCallInfo->callInfo.pRequestInfo));
//    CHK_STATUS(signAwsRequestInfo(pLwsCallInfo->callInfo.pRequestInfo));

    // Remove the header as it will be added back by LWS
//    CHK_STATUS(removeRequestHeader(pLwsCallInfo->callInfo.pRequestInfo, AWS_SIG_V4_HEADER_HOST));

    pContext = pLwsCallInfo->pSignalingClient->pLwsContext;

    // Execute the LWS REST call
    MEMSET(&connectInfo, 0x00, SIZEOF(struct lws_client_connect_info));
    connectInfo.context = pContext;
    connectInfo.ssl_connection = LCCSCF_USE_SSL;
    connectInfo.port = SIGNALING_DEFAULT_SSL_PORT;

    CHK_STATUS(getRequestHost(pLwsCallInfo->callInfo.pRequestInfo->url, &pHostStart, &pHostEnd));
//    CHK(pHostEnd == NULL || *pHostEnd == '/' || *pHostEnd == '?', STATUS_INTERNAL_ERROR);

    // Store the path
    path[MAX_URI_CHAR_LEN] = '\0';
    if (pHostEnd != NULL) {
        if (*pHostEnd == '/') {
            STRNCPY(path, pHostEnd, MAX_URI_CHAR_LEN);
        } else {
            path[0] = '/';
            STRNCPY(&path[1], pHostEnd, MAX_URI_CHAR_LEN - 1);
        }
    } else {
        path[0] = '/';
        path[1] = '\0';
    }

    // NULL terminate the host
    *pHostEnd = '\0';

    connectInfo.address = pHostStart;
    connectInfo.path = path;
    connectInfo.host = connectInfo.address;
    connectInfo.method = pVerb;
    connectInfo.protocol = pLwsCallInfo->pSignalingClient->signalingProtocols[pLwsCallInfo->protocolIndex].name;
    connectInfo.pwsi = &clientLws;

    connectInfo.opaque_user_data = pLwsCallInfo;

    // Attempt to iterate and acquire the locks
    // NOTE: The https protocol should be called sequentially only
    MUTEX_LOCK(pLwsCallInfo->pSignalingClient->lwsSerializerLock);
    serializerLocked = TRUE;

    // Ensure we are not running another https protocol
    // The WSIs for all of the protocols are set and cleared in this function only.
    // The HTTPS is serialized via the state machine lock and we should not encounter
    // another https protocol in flight. The only case is when we have an http request
    // and a wss is in progress. This is the case when we have a current websocket listener
    // and need to perform an https call due to ICE server config refresh for example.
    // If we have an ongoing wss operations, we can't call lws_client_connect_via_info API
    // due to threading model of LWS. WHat we need to do is to wake up the potentially blocked
    // ongoing wss handler for it to release the service lock which it holds while calling lws_service()
    // API so we can grab the lock in order to perform the lws_client_connect_via_info API call.
    // The need to wake up the wss handler (if any) to compete for the lock is the reason for this
    // loop. In order to avoid pegging of the CPU while the contention for the lock happes,
    // we are setting an atomic and releasing it to trigger a timed wait when the lws_service call
    // awakes to make sure we are not going to starve this thread.

    // NOTE: The THREAD_SLEEP calls in this routine are not designed to adjust
    // the execution timing/race conditions but to eliminate a busy wait in a spin-lock
    // type scenario for resource contention.

    // We should have HTTPS protocol serialized at the state machine level
    CHK_ERR(pLwsCallInfo->pSignalingClient->currentWsi[PROTOCOL_INDEX_HTTPS] == NULL, STATUS_INVALID_OPERATION,
            "HTTPS requests should be processed sequentially.");

    // Indicate that we are trying to acquire the lock
    ATOMIC_STORE_BOOL(&pLwsCallInfo->pSignalingClient->serviceLockContention, TRUE);
    while (iterate) {
        if (!MUTEX_TRYLOCK(pLwsCallInfo->pSignalingClient->lwsServiceLock)) {
            // Wake up the event loop
            CHK_STATUS(wakeLwsServiceEventLoop(pLwsCallInfo->pSignalingClient, PROTOCOL_INDEX_HTTPS));
        } else {
            locked = TRUE;
            iterate = FALSE;
        }
    }
    ATOMIC_STORE_BOOL(&pLwsCallInfo->pSignalingClient->serviceLockContention, FALSE);

    // Now we should be running with a lock
    CHK(NULL != (pLwsCallInfo->pSignalingClient->currentWsi[pLwsCallInfo->protocolIndex] = lws_client_connect_via_info(&connectInfo)),
        STATUS_SIGNALING_LWS_CLIENT_CONNECT_FAILED);
    if (locked) {
        MUTEX_UNLOCK(pLwsCallInfo->pSignalingClient->lwsServiceLock);
        locked = FALSE;
    }

    MUTEX_UNLOCK(pLwsCallInfo->pSignalingClient->lwsSerializerLock);
    serializerLocked = FALSE;

    while (retVal >= 0 && !gInterruptedFlagBySignalingHandler && pLwsCallInfo->callInfo.pRequestInfo != NULL &&
           !ATOMIC_LOAD_BOOL(&pLwsCallInfo->callInfo.pRequestInfo->terminating)) {
        if (!MUTEX_TRYLOCK(pLwsCallInfo->pSignalingClient->lwsServiceLock)) {
            THREAD_SLEEP(LWS_SERVICE_LOOP_ITERATION_WAIT);
        } else {
            retVal = lws_service(pContext, 0);
            MUTEX_UNLOCK(pLwsCallInfo->pSignalingClient->lwsServiceLock);

            // Add a minor timeout to relinquish the thread quota to eliminate thread starvation
            // when competing for the service lock
            if (ATOMIC_LOAD_BOOL(&pLwsCallInfo->pSignalingClient->serviceLockContention)) {
                THREAD_SLEEP(LWS_SERVICE_LOOP_ITERATION_WAIT);
            }
        }
    }

    // Clear the wsi on exit
    MUTEX_LOCK(pLwsCallInfo->pSignalingClient->lwsSerializerLock);
    pLwsCallInfo->pSignalingClient->currentWsi[pLwsCallInfo->protocolIndex] = NULL;
    MUTEX_UNLOCK(pLwsCallInfo->pSignalingClient->lwsSerializerLock);

CleanUp:

    // Reset the lock contention indicator in case of failure
    if (STATUS_FAILED(retStatus) && pLwsCallInfo != NULL && pLwsCallInfo->pSignalingClient != NULL) {
        ATOMIC_STORE_BOOL(&pLwsCallInfo->pSignalingClient->serviceLockContention, FALSE);
    }

    if (serializerLocked) {
        MUTEX_UNLOCK(pLwsCallInfo->pSignalingClient->lwsSerializerLock);
    }

    if (locked) {
        MUTEX_UNLOCK(pLwsCallInfo->pSignalingClient->lwsServiceLock);
    }

    LEAVES();
    return retStatus;
}

BOOL isCallResultSignatureExpired(PCallInfo pLwsCallInfo)
{
    return (STRNSTR(pLwsCallInfo->responseData, "Signature expired", pLwsCallInfo->responseDataLen) != NULL);
}

BOOL isCallResultSignatureNotYetCurrent(PCallInfo pLwsCallInfo)
{
    return (STRNSTR(pLwsCallInfo->responseData, "Signature not yet current", pLwsCallInfo->responseDataLen) != NULL);
}

STATUS checkAndCorrectForClockSkew(PSignalingClient pSignalingClient, PRequestInfo pRequestInfo)
{
    ENTERS();
    STATUS retStatus = STATUS_SUCCESS;

    PStateMachineState pStateMachineState;
    PHashTable pClockSkewMap;
    UINT64 clockSkewOffset;
    CHK_STATUS(getStateMachineCurrentState(pSignalingClient->pStateMachine, &pStateMachineState));

    pClockSkewMap = pSignalingClient->diagnostics.pEndpointToClockSkewHashMap;

    CHK_STATUS(hashTableGet(pClockSkewMap, pStateMachineState->state, &clockSkewOffset));

    // if we made it here that means there is clock skew
    if (clockSkewOffset & ((UINT64) (1ULL << 63))) {
        clockSkewOffset ^= ((UINT64) (1ULL << 63));
        DLOGV("Detected device time is AHEAD of server time!");
        pRequestInfo->currentTime -= clockSkewOffset;
    } else {
        DLOGV("Detected server time is AHEAD of device time!");
        pRequestInfo->currentTime += clockSkewOffset;
    }

    DLOGW("Clockskew corrected!");

CleanUp:

    LEAVES();
    return retStatus;
}

//////////////////////////////////////////////////////////////////////////
// API calls
//////////////////////////////////////////////////////////////////////////
STATUS describeChannelLws(PSignalingClient pSignalingClient, UINT64 time)
{
    ENTERS();
    STATUS retStatus = STATUS_SUCCESS;
    UNUSED_PARAM(time);

    PRequestInfo pRequestInfo = NULL;
    CHAR url[MAX_URI_CHAR_LEN + 1];
    PLwsCallInfo pLwsCallInfo = NULL;
    PCHAR pResponseStr;
    UINT32 resultLen;
    PSignalingMessageWrapper pSignalingMessageWrapper = NULL;
    TID receivedTid = INVALID_TID_VALUE;

    CHK(pSignalingClient != NULL, STATUS_NULL_ARG);

    // Create the API url
    STRCPY(url, pSignalingClient->pChannelInfo->pControlPlaneUrl);
    STRCAT(url, "/index/api/whip?app=live&stream=test");

    // Create the request info with the body
    CHK_STATUS(createRequestInfo(url, gZlmOffer, "", pSignalingClient->pChannelInfo->pCertPath, NULL, NULL,
                                 SSL_CERTIFICATE_TYPE_NOT_SPECIFIED, pSignalingClient->pChannelInfo->pUserAgent,
                                 SIGNALING_SERVICE_API_CALL_CONNECTION_TIMEOUT, SIGNALING_SERVICE_API_CALL_COMPLETION_TIMEOUT,
                                 DEFAULT_LOW_SPEED_LIMIT, DEFAULT_LOW_SPEED_TIME_LIMIT, pSignalingClient->pAwsCredentials, &pRequestInfo));

    // createRequestInfo does not have access to the getCurrentTime callback, this hook is used for tests.
    if (pSignalingClient->signalingClientCallbacks.getCurrentTimeFn != NULL) {
        pRequestInfo->currentTime = pSignalingClient->signalingClientCallbacks.getCurrentTimeFn(pSignalingClient->signalingClientCallbacks.customData);
    }

    checkAndCorrectForClockSkew(pSignalingClient, pRequestInfo);

    CHK_STATUS(createLwsCallInfo(pSignalingClient, pRequestInfo, PROTOCOL_INDEX_HTTPS, &pLwsCallInfo));

    // Make a blocking call
    CHK_STATUS(lwsCompleteSync(pLwsCallInfo));

    // Set the service call result
    ATOMIC_STORE(&pSignalingClient->result, (SIZE_T) pLwsCallInfo->callInfo.callResult);
    pResponseStr = pLwsCallInfo->callInfo.responseData;
    resultLen = pLwsCallInfo->callInfo.responseDataLen;

    // Early return if we have a non-success result
    CHK((SERVICE_CALL_RESULT) ATOMIC_LOAD(&pSignalingClient->result) == 201 && resultLen != 0 && pResponseStr != NULL,
        STATUS_SIGNALING_LWS_CALL_FAILED);

    // whip直接返回sdp，无须解json
    CHK(NULL != (pSignalingMessageWrapper = (PSignalingMessageWrapper) MEMCALLOC(1, SIZEOF(SignalingMessageWrapper))), STATUS_NOT_ENOUGH_MEMORY);
    pSignalingMessageWrapper->pSignalingClient = pSignalingClient;
    pSignalingMessageWrapper->receivedSignalingMessage.signalingMessage.version = SIGNALING_MESSAGE_CURRENT_VERSION;
    pSignalingMessageWrapper->receivedSignalingMessage.signalingMessage.messageType = SIGNALING_MESSAGE_TYPE_ANSWER;
    pSignalingMessageWrapper->receivedSignalingMessage.signalingMessage.payloadLen = resultLen;
    STRCPY(pSignalingMessageWrapper->receivedSignalingMessage.signalingMessage.payload, pResponseStr);

//    // Perform some validation on the channel description
//    CHK(pSignalingClient->answerDescription.channelStatus != ZLM_CHANNEL_STATUS_DELETING, STATUS_ZLM_CHANNEL_BEING_DELETED);

//    CHK(pSignalingMessageWrapper->receivedSignalingMessage.signalingMessage.payloadLen > 0 &&
//        pSignalingMessageWrapper->receivedSignalingMessage.signalingMessage.payloadLen <= MAX_SIGNALING_MESSAGE_LEN,
//        STATUS_ZLM_INVALID_PAYLOAD_LEN_IN_MESSAGE);
//    CHK(pSignalingMessageWrapper->receivedSignalingMessage.signalingMessage.payload[0] != '\0', STATUS_ZLM_NO_PAYLOAD_IN_MESSAGE);

    DLOGD("Client received message of type: %s",
          getMessageTypeInString(pSignalingMessageWrapper->receivedSignalingMessage.signalingMessage.messageType));

//    // Validate and process the ice config
//    if (jsonInIceServerList && STATUS_FAILED(validateIceConfiguration(pSignalingClient))) {
//        DLOGW("Failed to validate the ICE server configuration received with an Offer");
//    }

#ifdef KVS_USE_SIGNALING_CHANNEL_THREADPOOL
    CHK_STATUS(threadpoolPush(pSignalingClient->pThreadpool, receiveLwsMessageWrapper, (PVOID) pSignalingMessageWrapper));
#else
    // Issue the callback on a separate thread
    CHK_STATUS(THREAD_CREATE(&receivedTid, receiveLwsMessageWrapper, (PVOID) pSignalingMessageWrapper));
    CHK_STATUS(THREAD_DETACH(receivedTid));
#endif

CleanUp:

    if (STATUS_FAILED(retStatus)) {
        DLOGE("Call Failed with Status:  0x%08x", retStatus);
    }

    if (pSignalingClient != NULL && STATUS_FAILED(retStatus)) {
        ATOMIC_INCREMENT(&pSignalingClient->diagnostics.numberOfRuntimeErrors);
//        if (pSignalingClient->signalingClientCallbacks.errorReportFn != NULL) {
//            retStatus = pSignalingClient->signalingClientCallbacks.errorReportFn(pSignalingClient->signalingClientCallbacks.customData, retStatus,
//                                                                                 pMessage, messageLen);
//        }

        // Kill the receive thread on error
        if (IS_VALID_TID_VALUE(receivedTid)) {
            THREAD_CANCEL(receivedTid);
        }

        SAFE_MEMFREE(pSignalingMessageWrapper);
    }

    freeLwsCallInfo(&pLwsCallInfo);

    LEAVES();
    return retStatus;
}

STATUS createLwsCallInfo(PSignalingClient pSignalingClient, PRequestInfo pRequestInfo, UINT32 protocolIndex, PLwsCallInfo* ppLwsCallInfo)
{
    ENTERS();
    STATUS retStatus = STATUS_SUCCESS;
    PLwsCallInfo pLwsCallInfo = NULL;

    CHK(pSignalingClient != NULL && pRequestInfo != NULL && ppLwsCallInfo != NULL, STATUS_NULL_ARG);

    CHK(NULL != (pLwsCallInfo = (PLwsCallInfo) MEMCALLOC(1, SIZEOF(LwsCallInfo))), STATUS_NOT_ENOUGH_MEMORY);

    pLwsCallInfo->callInfo.pRequestInfo = pRequestInfo;
    pLwsCallInfo->pSignalingClient = pSignalingClient;
    pLwsCallInfo->protocolIndex = protocolIndex;

    *ppLwsCallInfo = pLwsCallInfo;

CleanUp:

    if (STATUS_FAILED(retStatus)) {
        freeLwsCallInfo(&pLwsCallInfo);
    }

    if (ppLwsCallInfo != NULL) {
        *ppLwsCallInfo = pLwsCallInfo;
    }

    LEAVES();
    return retStatus;
}

STATUS freeLwsCallInfo(PLwsCallInfo* ppLwsCallInfo)
{
    ENTERS();
    STATUS retStatus = STATUS_SUCCESS;
    PLwsCallInfo pLwsCallInfo;

    CHK(ppLwsCallInfo != NULL, STATUS_NULL_ARG);
    pLwsCallInfo = *ppLwsCallInfo;

    CHK(pLwsCallInfo != NULL, retStatus);

    freeRequestInfo(&pLwsCallInfo->callInfo.pRequestInfo);
    SAFE_MEMFREE(pLwsCallInfo->callInfo.responseData);

    MEMFREE(pLwsCallInfo);

    *ppLwsCallInfo = NULL;

CleanUp:

    LEAVES();
    return retStatus;
}

STATUS terminateConnectionWithStatus(PSignalingClient pSignalingClient, SERVICE_CALL_RESULT callResult)
{
    ENTERS();
    STATUS retStatus = STATUS_SUCCESS;
    UINT32 i;

    CHK(pSignalingClient != NULL, STATUS_NULL_ARG);

    ATOMIC_STORE_BOOL(&pSignalingClient->connected, FALSE);
    CVAR_BROADCAST(pSignalingClient->connectedCvar);
    CVAR_BROADCAST(pSignalingClient->receiveCvar);
    CVAR_BROADCAST(pSignalingClient->sendCvar);
    ATOMIC_STORE(&pSignalingClient->messageResult, (SIZE_T) SERVICE_CALL_UNKNOWN);
    ATOMIC_STORE(&pSignalingClient->result, (SIZE_T) callResult);

    if (pSignalingClient->pOngoingCallInfo != NULL) {
        ATOMIC_STORE_BOOL(&pSignalingClient->pOngoingCallInfo->cancelService, TRUE);
    }

    // Wake up the service event loop for all of the protocols
    for (i = 0; i < LWS_PROTOCOL_COUNT; i++) {
        CHK_STATUS(wakeLwsServiceEventLoop(pSignalingClient, i));
    }

    CHK_STATUS(awaitForThreadTermination(&pSignalingClient->listenerTracker, SIGNALING_CLIENT_SHUTDOWN_TIMEOUT));

CleanUp:

    LEAVES();
    return retStatus;
}

PCHAR getMessageTypeInString(SIGNALING_MESSAGE_TYPE messageType)
{
    switch (messageType) {
        case SIGNALING_MESSAGE_TYPE_OFFER:
            return SIGNALING_SDP_TYPE_OFFER;
        case SIGNALING_MESSAGE_TYPE_ANSWER:
            return SIGNALING_SDP_TYPE_ANSWER;
        case SIGNALING_MESSAGE_TYPE_STATUS_RESPONSE:
            return SIGNALING_STATUS_RESPONSE;
        case SIGNALING_MESSAGE_TYPE_UNKNOWN:
            return SIGNALING_MESSAGE_UNKNOWN;
    }
    return SIGNALING_MESSAGE_UNKNOWN;
}

STATUS terminateLwsListenerLoop(PSignalingClient pSignalingClient)
{
    ENTERS();
    STATUS retStatus = STATUS_SUCCESS;

    CHK(pSignalingClient != NULL, retStatus);

    if (pSignalingClient->pOngoingCallInfo != NULL) {
        // Check if anything needs to be done
        CHK(!ATOMIC_LOAD_BOOL(&pSignalingClient->listenerTracker.terminated), retStatus);

        // Terminate the listener
        terminateConnectionWithStatus(pSignalingClient, SERVICE_CALL_RESULT_OK);
    }

CleanUp:

    LEAVES();
    return retStatus;
}

PVOID receiveLwsMessageWrapper(PVOID args)
{
    STATUS retStatus = STATUS_SUCCESS;
    PSignalingMessageWrapper pSignalingMessageWrapper = (PSignalingMessageWrapper) args;
    PSignalingClient pSignalingClient = NULL;
    SIGNALING_MESSAGE_TYPE messageType = SIGNALING_MESSAGE_TYPE_UNKNOWN;

    CHK(pSignalingMessageWrapper != NULL, STATUS_NULL_ARG);

    messageType = pSignalingMessageWrapper->receivedSignalingMessage.signalingMessage.messageType;

    pSignalingClient = pSignalingMessageWrapper->pSignalingClient;

    CHK(pSignalingClient != NULL, STATUS_INTERNAL_ERROR);

    // Updating the diagnostics info before calling the client callback
    ATOMIC_INCREMENT(&pSignalingClient->diagnostics.numberOfMessagesReceived);

    // Calling client receive message callback if specified
    if (pSignalingClient->signalingClientCallbacks.messageReceivedFn != NULL) {
        if (messageType == SIGNALING_MESSAGE_TYPE_OFFER) {
            pSignalingClient->describeTime = GETTIME();
        }
        if (messageType == SIGNALING_MESSAGE_TYPE_ANSWER) {
            PROFILE_WITH_START_TIME_OBJ(pSignalingClient->describeTime, pSignalingClient->diagnostics.offerToAnswerTime, "Offer to answer time");
        }
        CHK_STATUS(pSignalingClient->signalingClientCallbacks.messageReceivedFn(pSignalingClient->signalingClientCallbacks.customData,
                                                                                &pSignalingMessageWrapper->receivedSignalingMessage));
    }

CleanUp:
    CHK_LOG_ERR(retStatus);

    SAFE_MEMFREE(pSignalingMessageWrapper);

    return (PVOID) (ULONG_PTR) retStatus;
}

STATUS wakeLwsServiceEventLoop(PSignalingClient pSignalingClient, UINT32 protocolIndex)
{
    ENTERS();
    STATUS retStatus = STATUS_SUCCESS;

    // Early exit in case we don't need to do anything
    CHK(pSignalingClient != NULL && pSignalingClient->pLwsContext != NULL, retStatus);

    if (pSignalingClient->currentWsi[protocolIndex] != NULL) {
        lws_callback_on_writable(pSignalingClient->currentWsi[protocolIndex]);
    }

CleanUp:

    LEAVES();
    return retStatus;
}
