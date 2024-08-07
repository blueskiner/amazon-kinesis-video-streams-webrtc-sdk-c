#define LOG_CLASS "Signaling"
#include "../Include_i.h"

extern StateMachineState SIGNALING_STATE_MACHINE_STATES[];
extern UINT32 SIGNALING_STATE_MACHINE_STATE_COUNT;

STATUS createSignalingSync(PSignalingClientInfoInternal pClientInfo, PChannelInfo pChannelInfo, PSignalingClientCallbacks pCallbacks,
                     PAwsCredentialProvider pCredentialProvider, PSignalingClient* ppSignalingClient)
{
    ENTERS();
    STATUS retStatus = STATUS_SUCCESS;
    PSignalingClient pSignalingClient = NULL;
    PCHAR userLogLevelStr = NULL;
    UINT32 userLogLevel;
    struct lws_context_creation_info creationInfo;
    const lws_retry_bo_t retryPolicy = {
            .secs_since_valid_ping = SIGNALING_SERVICE_WSS_PING_PONG_INTERVAL_IN_SECONDS,
            .secs_since_valid_hangup = SIGNALING_SERVICE_WSS_HANGUP_IN_SECONDS,
    };
    PStateMachineState pStateMachineState;
    PSignalingFileCacheEntry pFileCacheEntry = NULL;

    CHK(pClientInfo != NULL && pChannelInfo != NULL && pCallbacks != NULL && pCredentialProvider != NULL && ppSignalingClient != NULL,
        STATUS_NULL_ARG);
    CHK(pChannelInfo->version <= CHANNEL_INFO_CURRENT_VERSION, STATUS_SIGNALING_INVALID_CHANNEL_INFO_VERSION);
    CHK(NULL != (pFileCacheEntry = (PSignalingFileCacheEntry) MEMALLOC(SIZEOF(SignalingFileCacheEntry))), STATUS_NOT_ENOUGH_MEMORY);

    // Allocate enough storage
    CHK(NULL != (pSignalingClient = (PSignalingClient) MEMCALLOC(1, SIZEOF(SignalingClient))), STATUS_NOT_ENOUGH_MEMORY);

    // Initialize the listener and restart thread trackers
    CHK_STATUS(initializeThreadTracker(&pSignalingClient->listenerTracker));
    CHK_STATUS(initializeThreadTracker(&pSignalingClient->reconnecterTracker));

    // Validate and store the input
    CHK_STATUS(createValidateChannelInfo(pChannelInfo, &pSignalingClient->pChannelInfo));
    CHK_STATUS(validateSignalingCallbacks(pSignalingClient, pCallbacks));
    CHK_STATUS(validateSignalingClientInfo(pSignalingClient, pClientInfo));
#ifdef KVS_USE_SIGNALING_CHANNEL_THREADPOOL
    CHK_STATUS(threadpoolCreate(&pSignalingClient->pThreadpool, pClientInfo->signalingClientInfo.signalingMessagesMinimumThreads,
                                pClientInfo->signalingClientInfo.signalingMessagesMaximumThreads));
#endif

    pSignalingClient->version = SIGNALING_CLIENT_CURRENT_VERSION;

    // Set invalid call times
    pSignalingClient->describeTime = INVALID_TIMESTAMP_VALUE;

    // Attempting to get the logging level from the env var and if it fails then set it from the client info
    if ((userLogLevelStr = GETENV(DEBUG_LOG_LEVEL_ENV_VAR)) != NULL && STATUS_SUCCEEDED(STRTOUI32(userLogLevelStr, NULL, 10, &userLogLevel))) {
        userLogLevel = userLogLevel > LOG_LEVEL_SILENT ? LOG_LEVEL_SILENT : userLogLevel < LOG_LEVEL_VERBOSE ? LOG_LEVEL_VERBOSE : userLogLevel;
    } else {
        userLogLevel = pClientInfo->signalingClientInfo.loggingLevel;
    }

    SET_LOGGER_LOG_LEVEL(userLogLevel);

    // Store the credential provider
    pSignalingClient->pCredentialProvider = pCredentialProvider;

    CHK_STATUS(configureRetryStrategyForSignalingStateMachine(pSignalingClient));

    // Create the state machine
    CHK_STATUS(createStateMachine(SIGNALING_STATE_MACHINE_STATES, SIGNALING_STATE_MACHINE_STATE_COUNT,
                                  CUSTOM_DATA_FROM_SIGNALING_CLIENT(pSignalingClient), signalingGetCurrentTime,
                                  CUSTOM_DATA_FROM_SIGNALING_CLIENT(pSignalingClient), &pSignalingClient->pStateMachine));

    // Prepare the signaling channel protocols array
    pSignalingClient->signalingProtocols[PROTOCOL_INDEX_HTTPS].name = HTTPS_SCHEME_NAME;
    pSignalingClient->signalingProtocols[PROTOCOL_INDEX_HTTPS].callback = lwsHttpCallbackRoutine;

    pSignalingClient->currentWsi[PROTOCOL_INDEX_HTTPS] = NULL;

    MEMSET(&creationInfo, 0x00, SIZEOF(struct lws_context_creation_info));
    creationInfo.options = LWS_SERVER_OPTION_DO_SSL_GLOBAL_INIT;
    creationInfo.port = CONTEXT_PORT_NO_LISTEN;
    creationInfo.protocols = pSignalingClient->signalingProtocols;
    creationInfo.timeout_secs = SIGNALING_SERVICE_API_CALL_TIMEOUT_IN_SECONDS;
    creationInfo.gid = -1;
    creationInfo.uid = -1;
    creationInfo.client_ssl_ca_filepath = pChannelInfo->pCertPath;
    creationInfo.client_ssl_cipher_list = "HIGH:!PSK:!RSP:!eNULL:!aNULL:!RC4:!MD5:!DES:!3DES:!aDH:!kDH:!DSS";
    creationInfo.ka_time = SIGNALING_SERVICE_TCP_KEEPALIVE_IN_SECONDS;
    creationInfo.ka_probes = SIGNALING_SERVICE_TCP_KEEPALIVE_PROBE_COUNT;
    creationInfo.ka_interval = SIGNALING_SERVICE_TCP_KEEPALIVE_PROBE_INTERVAL_IN_SECONDS;
    creationInfo.retry_and_idle_policy = &retryPolicy;

    ATOMIC_STORE_BOOL(&pSignalingClient->clientReady, FALSE);
    ATOMIC_STORE_BOOL(&pSignalingClient->shutdown, FALSE);
    ATOMIC_STORE_BOOL(&pSignalingClient->connected, FALSE);
    ATOMIC_STORE_BOOL(&pSignalingClient->deleting, FALSE);
    ATOMIC_STORE_BOOL(&pSignalingClient->deleted, FALSE);
    ATOMIC_STORE_BOOL(&pSignalingClient->serviceLockContention, FALSE);

    // Add to the signal handler
    // signal(SIGINT, lwsSignalHandler);

    // Create the sync primitives
    pSignalingClient->connectedCvar = CVAR_CREATE();
    CHK(IS_VALID_CVAR_VALUE(pSignalingClient->connectedCvar), STATUS_INVALID_OPERATION);
    pSignalingClient->connectedLock = MUTEX_CREATE(FALSE);
    CHK(IS_VALID_MUTEX_VALUE(pSignalingClient->connectedLock), STATUS_INVALID_OPERATION);
    pSignalingClient->sendCvar = CVAR_CREATE();
    CHK(IS_VALID_CVAR_VALUE(pSignalingClient->sendCvar), STATUS_INVALID_OPERATION);
    pSignalingClient->sendLock = MUTEX_CREATE(FALSE);
    CHK(IS_VALID_MUTEX_VALUE(pSignalingClient->sendLock), STATUS_INVALID_OPERATION);
    pSignalingClient->receiveCvar = CVAR_CREATE();
    CHK(IS_VALID_CVAR_VALUE(pSignalingClient->receiveCvar), STATUS_INVALID_OPERATION);
    pSignalingClient->receiveLock = MUTEX_CREATE(FALSE);
    CHK(IS_VALID_MUTEX_VALUE(pSignalingClient->receiveLock), STATUS_INVALID_OPERATION);

    pSignalingClient->stateLock = MUTEX_CREATE(TRUE);
    CHK(IS_VALID_MUTEX_VALUE(pSignalingClient->stateLock), STATUS_INVALID_OPERATION);

    pSignalingClient->messageQueueLock = MUTEX_CREATE(TRUE);
    CHK(IS_VALID_MUTEX_VALUE(pSignalingClient->messageQueueLock), STATUS_INVALID_OPERATION);

    pSignalingClient->lwsServiceLock = MUTEX_CREATE(TRUE);
    CHK(IS_VALID_MUTEX_VALUE(pSignalingClient->lwsServiceLock), STATUS_INVALID_OPERATION);

    pSignalingClient->lwsSerializerLock = MUTEX_CREATE(TRUE);
    CHK(IS_VALID_MUTEX_VALUE(pSignalingClient->lwsSerializerLock), STATUS_INVALID_OPERATION);

    pSignalingClient->diagnosticsLock = MUTEX_CREATE(TRUE);
    CHK(IS_VALID_MUTEX_VALUE(pSignalingClient->diagnosticsLock), STATUS_INVALID_OPERATION);

    // Create the ongoing message list
    CHK_STATUS(stackQueueCreate(&pSignalingClient->pMessageQueue));

    pSignalingClient->pLwsContext = lws_create_context(&creationInfo);
    CHK(pSignalingClient->pLwsContext != NULL, STATUS_SIGNALING_LWS_CREATE_CONTEXT_FAILED);

    // Initializing the diagnostics mostly is taken care of by zero-mem in MEMCALLOC
    pSignalingClient->diagnostics.createTime = SIGNALING_GET_CURRENT_TIME(pSignalingClient);
    CHK_STATUS(hashTableCreateWithParams(SIGNALING_CLOCKSKEW_HASH_TABLE_BUCKET_COUNT, SIGNALING_CLOCKSKEW_HASH_TABLE_BUCKET_LENGTH,
                                         &pSignalingClient->diagnostics.pEndpointToClockSkewHashMap));

    // At this point we have constructed the main object and we can assign to the returned pointer
    *ppSignalingClient = pSignalingClient;

    // Notify of the state change initially as the state machinery is already in the NEW state
    if (pSignalingClient->signalingClientCallbacks.stateChangeFn != NULL) {
        CHK_STATUS(getStateMachineCurrentState(pSignalingClient->pStateMachine, &pStateMachineState));
        CHK_STATUS(pSignalingClient->signalingClientCallbacks.stateChangeFn(pSignalingClient->signalingClientCallbacks.customData,
                                                                            getSignalingStateFromStateMachineState(pStateMachineState->state)));
    }

    // Do not force ice config state
    ATOMIC_STORE_BOOL(&pSignalingClient->refreshIceConfig, FALSE);

    // We do not cache token in file system, so we will always have to retrieve one after creating the client.
    CHK_STATUS(signalingStateMachineIterator(pSignalingClient, pSignalingClient->diagnostics.createTime + SIGNALING_CONNECT_STATE_TIMEOUT,
                                             SIGNALING_STATE_GET_TOKEN));

CleanUp:
    if (pClientInfo != NULL && pSignalingClient != NULL) {
        pClientInfo->signalingClientInfo.stateMachineRetryCountReadOnly = pSignalingClient->diagnostics.stateMachineRetryCount;
    }
    CHK_LOG_ERR(retStatus);

    if (STATUS_FAILED(retStatus)) {
        freeSignaling(&pSignalingClient);
    }

    if (ppSignalingClient != NULL) {
        *ppSignalingClient = pSignalingClient;
    }
    SAFE_MEMFREE(pFileCacheEntry);
    LEAVES();
    return retStatus;
}

STATUS freeSignaling(PSignalingClient* ppSignalingClient)
{
    ENTERS();
    STATUS retStatus = STATUS_SUCCESS;
    PSignalingClient pSignalingClient;

    CHK(ppSignalingClient != NULL, STATUS_NULL_ARG);

    pSignalingClient = *ppSignalingClient;
    CHK(pSignalingClient != NULL, retStatus);

    ATOMIC_STORE_BOOL(&pSignalingClient->shutdown, TRUE);

    terminateOngoingOperations(pSignalingClient);

    if (pSignalingClient->pLwsContext != NULL) {
        MUTEX_LOCK(pSignalingClient->lwsServiceLock);
        lws_context_destroy(pSignalingClient->pLwsContext);
        pSignalingClient->pLwsContext = NULL;
        MUTEX_UNLOCK(pSignalingClient->lwsServiceLock);
    }

    freeStateMachine(pSignalingClient->pStateMachine);

    freeClientRetryStrategy(pSignalingClient);

    freeChannelInfo(&pSignalingClient->pChannelInfo);

    stackQueueFree(pSignalingClient->pMessageQueue);

    hashTableFree(pSignalingClient->diagnostics.pEndpointToClockSkewHashMap);

#ifdef KVS_USE_SIGNALING_CHANNEL_THREADPOOL
    threadpoolFree(pSignalingClient->pThreadpool);
#endif

    if (IS_VALID_MUTEX_VALUE(pSignalingClient->connectedLock)) {
        MUTEX_FREE(pSignalingClient->connectedLock);
    }

    if (IS_VALID_CVAR_VALUE(pSignalingClient->connectedCvar)) {
        CVAR_FREE(pSignalingClient->connectedCvar);
    }

    if (IS_VALID_MUTEX_VALUE(pSignalingClient->sendLock)) {
        MUTEX_FREE(pSignalingClient->sendLock);
    }

    if (IS_VALID_CVAR_VALUE(pSignalingClient->sendCvar)) {
        CVAR_FREE(pSignalingClient->sendCvar);
    }

    if (IS_VALID_MUTEX_VALUE(pSignalingClient->receiveLock)) {
        MUTEX_FREE(pSignalingClient->receiveLock);
    }

    if (IS_VALID_CVAR_VALUE(pSignalingClient->receiveCvar)) {
        CVAR_FREE(pSignalingClient->receiveCvar);
    }

    if (IS_VALID_MUTEX_VALUE(pSignalingClient->stateLock)) {
        MUTEX_FREE(pSignalingClient->stateLock);
    }

    if (IS_VALID_MUTEX_VALUE(pSignalingClient->messageQueueLock)) {
        MUTEX_FREE(pSignalingClient->messageQueueLock);
    }

    if (IS_VALID_MUTEX_VALUE(pSignalingClient->lwsServiceLock)) {
        MUTEX_FREE(pSignalingClient->lwsServiceLock);
    }

    if (IS_VALID_MUTEX_VALUE(pSignalingClient->lwsSerializerLock)) {
        MUTEX_FREE(pSignalingClient->lwsSerializerLock);
    }

    if (IS_VALID_MUTEX_VALUE(pSignalingClient->diagnosticsLock)) {
        MUTEX_FREE(pSignalingClient->diagnosticsLock);
    }

    uninitializeThreadTracker(&pSignalingClient->reconnecterTracker);
    uninitializeThreadTracker(&pSignalingClient->listenerTracker);

    MEMFREE(pSignalingClient);

    *ppSignalingClient = NULL;

CleanUp:

    LEAVES();
    return retStatus;
}

STATUS setupDefaultRetryStrategyForSignalingStateMachine(PSignalingClient pSignalingClient)
{
    ENTERS();
    STATUS retStatus = STATUS_SUCCESS;
    PKvsRetryStrategyCallbacks pKvsRetryStrategyCallbacks = &(pSignalingClient->clientInfo.signalingStateMachineRetryStrategyCallbacks);

    // Use default as exponential backoff wait
    pKvsRetryStrategyCallbacks->createRetryStrategyFn = exponentialBackoffRetryStrategyCreate;
    pKvsRetryStrategyCallbacks->freeRetryStrategyFn = exponentialBackoffRetryStrategyFree;
    pKvsRetryStrategyCallbacks->executeRetryStrategyFn = getExponentialBackoffRetryStrategyWaitTime;
    pKvsRetryStrategyCallbacks->getCurrentRetryAttemptNumberFn = getExponentialBackoffRetryCount;

    // Use a default exponential backoff config for state machine level retries
    pSignalingClient->clientInfo.signalingStateMachineRetryStrategy.pRetryStrategyConfig =
            (PRetryStrategyConfig) &DEFAULT_SIGNALING_STATE_MACHINE_EXPONENTIAL_BACKOFF_RETRY_CONFIGURATION;

    LEAVES();
    return retStatus;
}

STATUS configureRetryStrategyForSignalingStateMachine(PSignalingClient pSignalingClient)
{
    ENTERS();
    STATUS retStatus = STATUS_SUCCESS;
    PKvsRetryStrategyCallbacks pKvsRetryStrategyCallbacks = NULL;

    CHK(pSignalingClient != NULL, STATUS_NULL_ARG);
    pKvsRetryStrategyCallbacks = &(pSignalingClient->clientInfo.signalingStateMachineRetryStrategyCallbacks);

    // If the callbacks for retry strategy are already set, then use that otherwise
    // build the client with a default retry strategy.
    if (pKvsRetryStrategyCallbacks->createRetryStrategyFn == NULL || pKvsRetryStrategyCallbacks->freeRetryStrategyFn == NULL ||
        pKvsRetryStrategyCallbacks->executeRetryStrategyFn == NULL || pKvsRetryStrategyCallbacks->getCurrentRetryAttemptNumberFn == NULL) {
        CHK_STATUS(setupDefaultRetryStrategyForSignalingStateMachine(pSignalingClient));
    }

    CHK_STATUS(pKvsRetryStrategyCallbacks->createRetryStrategyFn(&(pSignalingClient->clientInfo.signalingStateMachineRetryStrategy)));

CleanUp:

    LEAVES();
    return retStatus;
}

STATUS freeClientRetryStrategy(PSignalingClient pSignalingClient)
{
    ENTERS();
    STATUS retStatus = STATUS_SUCCESS;
    PKvsRetryStrategyCallbacks pKvsRetryStrategyCallbacks = NULL;

    CHK(pSignalingClient != NULL, STATUS_NULL_ARG);

    pKvsRetryStrategyCallbacks = &(pSignalingClient->clientInfo.signalingStateMachineRetryStrategyCallbacks);
    CHK(pKvsRetryStrategyCallbacks->freeRetryStrategyFn != NULL, STATUS_SUCCESS);

    CHK_STATUS(pKvsRetryStrategyCallbacks->freeRetryStrategyFn(&(pSignalingClient->clientInfo.signalingStateMachineRetryStrategy)));

CleanUp:

    LEAVES();
    return retStatus;
}

STATUS terminateOngoingOperations(PSignalingClient pSignalingClient)
{
    ENTERS();
    STATUS retStatus = STATUS_SUCCESS;

    CHK(pSignalingClient != NULL, STATUS_NULL_ARG);

    // Terminate the listener thread if alive
    terminateLwsListenerLoop(pSignalingClient);

    // Await for the reconnect thread to exit
    awaitForThreadTermination(&pSignalingClient->reconnecterTracker, SIGNALING_CLIENT_SHUTDOWN_TIMEOUT);

CleanUp:

    CHK_LOG_ERR(retStatus);

    LEAVES();
    return retStatus;
}

STATUS signalingFetchSync(PSignalingClient pSignalingClient)
{
    ENTERS();
    STATUS retStatus = STATUS_SUCCESS;
    SIZE_T result;

    CHK(pSignalingClient != NULL, STATUS_NULL_ARG);

    // Check if we are already not connected
    if (ATOMIC_LOAD_BOOL(&pSignalingClient->connected)) {
        CHK_STATUS(terminateOngoingOperations(pSignalingClient));
    }

    // move to the fromGetToken() so we can move to the necessary step
    // We start from get token to keep the design consistent with how it was when the constructor (create)
    // would bring you to the READY state, but this is a two-way door and can be redone later.
    setStateMachineCurrentState(pSignalingClient->pStateMachine, SIGNALING_STATE_GET_TOKEN);

    // if we're not failing from a bad token, set the result to OK to that fromGetToken will move
    // to getEndpoint, describe, or create. If it is bad, keep reiterating on token.
    result = ATOMIC_LOAD(&pSignalingClient->result);
    if (result != SERVICE_CALL_NOT_AUTHORIZED) {
        ATOMIC_STORE(&pSignalingClient->result, (SIZE_T) SERVICE_CALL_RESULT_OK);
    }
    CHK_STATUS(signalingStateMachineIterator(pSignalingClient, SIGNALING_GET_CURRENT_TIME(pSignalingClient) + SIGNALING_CONNECT_STATE_TIMEOUT,
                                             SIGNALING_STATE_READY));

CleanUp:

    if (STATUS_FAILED(retStatus)) {
        resetStateMachineRetryCount(pSignalingClient->pStateMachine);
    }
    CHK_LOG_ERR(retStatus);
    LEAVES();
    return retStatus;
}

STATUS validateSignalingCallbacks(PSignalingClient pSignalingClient, PSignalingClientCallbacks pCallbacks)
{
    ENTERS();
    STATUS retStatus = STATUS_SUCCESS;

    CHK(pSignalingClient != NULL && pCallbacks != NULL, STATUS_NULL_ARG);
    CHK(pCallbacks->version <= SIGNALING_CLIENT_CALLBACKS_CURRENT_VERSION, STATUS_SIGNALING_INVALID_SIGNALING_CALLBACKS_VERSION);

    // Store and validate
    pSignalingClient->signalingClientCallbacks = *pCallbacks;

CleanUp:

    CHK_LOG_ERR(retStatus);

    LEAVES();
    return retStatus;
}

STATUS validateSignalingClientInfo(PSignalingClient pSignalingClient, PSignalingClientInfoInternal pClientInfo)
{
    ENTERS();
    STATUS retStatus = STATUS_SUCCESS;

    CHK(pSignalingClient != NULL && pClientInfo != NULL, STATUS_NULL_ARG);
    CHK(pClientInfo->signalingClientInfo.version <= SIGNALING_CLIENT_INFO_CURRENT_VERSION, STATUS_SIGNALING_INVALID_CLIENT_INFO_VERSION);
    CHK(STRNLEN(pClientInfo->signalingClientInfo.clientId, MAX_SIGNALING_CLIENT_ID_LEN + 1) <= MAX_SIGNALING_CLIENT_ID_LEN,
        STATUS_SIGNALING_INVALID_CLIENT_INFO_CLIENT_LENGTH);

    // Copy and store internally
    pSignalingClient->clientInfo = *pClientInfo;

    // V1 features
    switch (pSignalingClient->clientInfo.signalingClientInfo.version) {
        case 0:
            // Set the default path
            STRCPY(pSignalingClient->clientInfo.cacheFilePath, DEFAULT_CACHE_FILE_PATH);

            break;

        case 1:
            // explicit-fallthrough
        case 2:
            // If the path is specified and not empty then we validate and copy/store
            if (pSignalingClient->clientInfo.signalingClientInfo.cacheFilePath != NULL &&
                pSignalingClient->clientInfo.signalingClientInfo.cacheFilePath[0] != '\0') {
                CHK(STRNLEN(pSignalingClient->clientInfo.signalingClientInfo.cacheFilePath, MAX_PATH_LEN + 1) <= MAX_PATH_LEN,
                    STATUS_SIGNALING_INVALID_CLIENT_INFO_CACHE_FILE_PATH_LEN);
                STRCPY(pSignalingClient->clientInfo.cacheFilePath, pSignalingClient->clientInfo.signalingClientInfo.cacheFilePath);
            } else {
                // Set the default path
                STRCPY(pSignalingClient->clientInfo.cacheFilePath, DEFAULT_CACHE_FILE_PATH);
            }

            break;

        default:
            CHK_ERR(FALSE, STATUS_INTERNAL_ERROR, "Internal error checking and validating the ClientInfo version");
    }

CleanUp:

    CHK_LOG_ERR(retStatus);
    LEAVES();
    return retStatus;
}

STATUS initializeThreadTracker(PThreadTracker pThreadTracker)
{
    STATUS retStatus = STATUS_SUCCESS;
    CHK(pThreadTracker != NULL, STATUS_NULL_ARG);

    pThreadTracker->threadId = INVALID_TID_VALUE;

    pThreadTracker->lock = MUTEX_CREATE(FALSE);
    CHK(IS_VALID_MUTEX_VALUE(pThreadTracker->lock), STATUS_INVALID_OPERATION);

    pThreadTracker->await = CVAR_CREATE();
    CHK(IS_VALID_CVAR_VALUE(pThreadTracker->await), STATUS_INVALID_OPERATION);

    ATOMIC_STORE_BOOL(&pThreadTracker->terminated, TRUE);

CleanUp:
    return retStatus;
}

STATUS uninitializeThreadTracker(PThreadTracker pThreadTracker)
{
    STATUS retStatus = STATUS_SUCCESS;
    CHK(pThreadTracker != NULL, STATUS_NULL_ARG);

    if (IS_VALID_MUTEX_VALUE(pThreadTracker->lock)) {
        MUTEX_FREE(pThreadTracker->lock);
    }

    if (IS_VALID_CVAR_VALUE(pThreadTracker->await)) {
        CVAR_FREE(pThreadTracker->await);
    }

CleanUp:
    return retStatus;
}

STATUS awaitForThreadTermination(PThreadTracker pThreadTracker, UINT64 timeout)
{
    STATUS retStatus = STATUS_SUCCESS;
    BOOL locked = FALSE;

    CHK(pThreadTracker != NULL, STATUS_NULL_ARG);

    MUTEX_LOCK(pThreadTracker->lock);
    locked = TRUE;
    // Await for the termination
    while (!ATOMIC_LOAD_BOOL(&pThreadTracker->terminated)) {
        CHK_STATUS(CVAR_WAIT(pThreadTracker->await, pThreadTracker->lock, timeout));
    }

    MUTEX_UNLOCK(pThreadTracker->lock);
    locked = FALSE;

CleanUp:

    if (locked) {
        MUTEX_UNLOCK(pThreadTracker->lock);
    }

    return retStatus;
}

STATUS describeChannel(PSignalingClient pSignalingClient, UINT64 time)
{
    ENTERS();
    STATUS retStatus = STATUS_SUCCESS;
    BOOL apiCall = TRUE;

    CHK(pSignalingClient != NULL, STATUS_NULL_ARG);

    THREAD_SLEEP_UNTIL(time);
    // Check for the stale credentials
    CHECK_SIGNALING_CREDENTIALS_EXPIRATION(pSignalingClient);

    ATOMIC_STORE(&pSignalingClient->result, (SIZE_T) SERVICE_CALL_RESULT_NOT_SET);

    switch (pSignalingClient->pChannelInfo->cachingPolicy) {
        case SIGNALING_API_CALL_CACHE_TYPE_NONE:
            break;

            /* explicit fall-through */
        case SIGNALING_API_CALL_CACHE_TYPE_FILE:
            if (IS_VALID_TIMESTAMP(pSignalingClient->describeTime) &&
                time <= pSignalingClient->describeTime + pSignalingClient->pChannelInfo->cachingPeriod) {
                apiCall = FALSE;
            }

            break;
    }

    // Call PostOffer API
    if (STATUS_SUCCEEDED(retStatus)) {
        if (apiCall) {
            DLOGI("Calling because call is uncached");
            // Call pre hook func
//            if (pSignalingClient->clientInfo.postOfferPreHookFn != NULL) {
//                retStatus = pSignalingClient->clientInfo.postOfferPreHookFn(pSignalingClient->clientInfo.hookCustomData);
//            }

            if (STATUS_SUCCEEDED(retStatus)) {
                retStatus = describeChannelLws(pSignalingClient, time);
                // Store the last call time on success
                if (STATUS_SUCCEEDED(retStatus)) {
                    pSignalingClient->describeTime = time;
                }

                // Calculate the latency whether the call succeeded or not
                SIGNALING_API_LATENCY_CALCULATION(pSignalingClient, time, TRUE);
            }

            // Call post hook func
//            if (pSignalingClient->clientInfo.postOfferPreHookFn != NULL) {
//                retStatus = pSignalingClient->clientInfo.postOfferPreHookFn(pSignalingClient->clientInfo.hookCustomData);
//            }
        } else {
            ATOMIC_STORE(&pSignalingClient->result, (SIZE_T) SERVICE_CALL_RESULT_OK);
        }
    }

CleanUp:

    LEAVES();
    return retStatus;
}

UINT64 signalingGetCurrentTime(UINT64 customData)
{
    UNUSED_PARAM(customData);
    return GETTIME();
}

STATUS signalingGetMetrics(PSignalingClient pSignalingClient, PSignalingClientMetrics pSignalingClientMetrics)
{
    ENTERS();
    STATUS retStatus = STATUS_SUCCESS;
    UINT64 curTime;

    curTime = SIGNALING_GET_CURRENT_TIME(pSignalingClient);

    CHK(pSignalingClient != NULL && pSignalingClientMetrics != NULL, STATUS_NULL_ARG);

    if (pSignalingClientMetrics->version > SIGNALING_CLIENT_METRICS_CURRENT_VERSION) {
        DLOGW("Invalid signaling client metrics version...setting to highest supported by default version %d",
              SIGNALING_CLIENT_METRICS_CURRENT_VERSION);
        pSignalingClientMetrics->version = SIGNALING_CLIENT_METRICS_CURRENT_VERSION;
    }

    // Interlock the threading due to data race possibility
    MUTEX_LOCK(pSignalingClient->diagnosticsLock);

    MEMSET(&pSignalingClientMetrics->signalingClientStats, 0x00, SIZEOF(pSignalingClientMetrics->signalingClientStats));

    switch (pSignalingClientMetrics->version) {
        case 1:
            pSignalingClientMetrics->signalingClientStats.getTokenCallTime = pSignalingClient->diagnostics.getTokenCallTime;
            pSignalingClientMetrics->signalingClientStats.describeCallTime = pSignalingClient->diagnostics.describeCallTime;
            pSignalingClientMetrics->signalingClientStats.createCallTime = pSignalingClient->diagnostics.createCallTime;
            pSignalingClientMetrics->signalingClientStats.getEndpointCallTime = pSignalingClient->diagnostics.getEndpointCallTime;
            pSignalingClientMetrics->signalingClientStats.getIceConfigCallTime = pSignalingClient->diagnostics.getIceConfigCallTime;
            pSignalingClientMetrics->signalingClientStats.connectCallTime = pSignalingClient->diagnostics.connectCallTime;
            pSignalingClientMetrics->signalingClientStats.createClientTime = pSignalingClient->diagnostics.createClientTime;
            pSignalingClientMetrics->signalingClientStats.fetchClientTime = pSignalingClient->diagnostics.fetchClientTime;
            pSignalingClientMetrics->signalingClientStats.connectClientTime = pSignalingClient->diagnostics.connectClientTime;
            pSignalingClientMetrics->signalingClientStats.offerToAnswerTime = pSignalingClient->diagnostics.offerToAnswerTime;
        case 0:
            // Fill in the data structures according to the version of the requested structure
            pSignalingClientMetrics->signalingClientStats.signalingClientUptime = curTime - pSignalingClient->diagnostics.createTime;
            pSignalingClientMetrics->signalingClientStats.numberOfMessagesSent = (UINT32) pSignalingClient->diagnostics.numberOfMessagesSent;
            pSignalingClientMetrics->signalingClientStats.numberOfMessagesReceived = (UINT32) pSignalingClient->diagnostics.numberOfMessagesReceived;
            pSignalingClientMetrics->signalingClientStats.iceRefreshCount = (UINT32) pSignalingClient->diagnostics.iceRefreshCount;
            pSignalingClientMetrics->signalingClientStats.numberOfErrors = (UINT32) pSignalingClient->diagnostics.numberOfErrors;
            pSignalingClientMetrics->signalingClientStats.numberOfRuntimeErrors = (UINT32) pSignalingClient->diagnostics.numberOfRuntimeErrors;
            pSignalingClientMetrics->signalingClientStats.numberOfReconnects = (UINT32) pSignalingClient->diagnostics.numberOfReconnects;
            pSignalingClientMetrics->signalingClientStats.cpApiCallLatency = pSignalingClient->diagnostics.cpApiLatency;
            pSignalingClientMetrics->signalingClientStats.dpApiCallLatency = pSignalingClient->diagnostics.dpApiLatency;

            pSignalingClientMetrics->signalingClientStats.connectionDuration =
                            ATOMIC_LOAD_BOOL(&pSignalingClient->connected) ? curTime - pSignalingClient->diagnostics.connectTime : 0;
            pSignalingClientMetrics->signalingClientStats.apiCallRetryCount = pSignalingClient->diagnostics.stateMachineRetryCount;
        default:
            break;
    }
    MUTEX_UNLOCK(pSignalingClient->diagnosticsLock);

CleanUp:

    LEAVES();
    return retStatus;
}
