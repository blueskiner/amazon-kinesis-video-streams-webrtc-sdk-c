//
// Copyright (c) 2023 gznscy
//

#include "Samples.h"

#define CY_AUDIO_SAMPLE_RATE        8000// 8k ~ 48k
#define CY_AUDIO_CHANNELS           1// 音频通道数
#define CY_AUDIO_SAMPLE_BIT_WIDTH   16// 音频样本位宽
#define CY_AUDIO_SAMPLE_INTERVAL    20// 采样间隔 单位:ms
#define CY_BITS_PER_BYTE            8// 每个字节所占位数
#define CY_AUDIO_BITRATE            (CY_AUDIO_SAMPLE_RATE * CY_AUDIO_SAMPLE_BIT_WIDTH * CY_AUDIO_CHANNELS)// 码率 V = sample_rate * bit_width * channels   4k 8k 32k 64k 96k 128k
#define CY_AUDIO_SAMPLE_SIZE        (CY_AUDIO_SAMPLE_BIT_WIDTH / CY_BITS_PER_BYTE)// 样本大小
#define CY_AUDIO_SAMPLE_BUF_SIZE    ((CY_AUDIO_SAMPLE_RATE / 1000) * CY_AUDIO_SAMPLE_INTERVAL * CY_AUDIO_SAMPLE_SIZE * CY_AUDIO_CHANNELS)// 单次采样出来的尺寸
#define CY_AUDIO_SAMPLE_PER_FRAME   (CY_AUDIO_SAMPLE_BUF_SIZE / (CY_AUDIO_CHANNELS * CY_AUDIO_SAMPLE_SIZE))// 送入编码器的单帧样本数量

#define CY_VIDEO_WIDTH              640
#define CY_VIDEO_HEIGHT             480

extern PSampleConfiguration gSampleConfiguration;

INT32 main(INT32 argc, CHAR* argv[])
{
    STATUS retStatus = STATUS_SUCCESS;
    PSampleConfiguration pSampleConfiguration = NULL;
    PCHAR pChannelName;

    SET_INSTRUMENTED_ALLOCATORS();
    UINT32 logLevel = setLogLevel();

#ifndef _WIN32
    signal(SIGINT, sigintHandler);
#endif

#ifdef IOT_CORE_ENABLE_CREDENTIALS
    CHK_ERR((pChannelName = getenv(IOT_CORE_THING_NAME)) != NULL, STATUS_INVALID_OPERATION, "AWS_IOT_CORE_THING_NAME must be set");
#else
    pChannelName = argc > 1 ? argv[1] : SAMPLE_CHANNEL_NAME;
#endif

    CHK_STATUS(createSampleConfiguration(pChannelName, SIGNALING_CHANNEL_ROLE_TYPE_MASTER, TRUE, TRUE, logLevel, &pSampleConfiguration));

    // Set the audio and video handlers
    pSampleConfiguration->audioSource = sendAudioPackets;
    pSampleConfiguration->videoSource = sendVideoPackets;
    pSampleConfiguration->receiveAudioVideoSource = sampleReceiveAudioVideoFrame;

    // Initialize KVS WebRTC. This must be done before anything else, and must only be done once.
    CHK_STATUS(initKvsWebRtc());
    DLOGI("[CyIpc] KVS WebRTC initialization completed successfully");

    CHK_STATUS(initSignaling(pSampleConfiguration, SAMPLE_MASTER_CLIENT_ID));
    DLOGI("[CyIpc] Signaling channel set up done ");

    // Checking for termination
    CHK_STATUS(sessionCleanupWait(pSampleConfiguration));
    DLOGI("[CyIpc] Streaming session terminated");

CleanUp:

    if (retStatus != STATUS_SUCCESS) {
        DLOGE("[CyIpc] Terminated with status code 0x%08x", retStatus);
    }

    DLOGI("[CyIpcMain] Cleanup done");
    CHK_LOG_ERR(retStatus);

    RESET_INSTRUMENTED_ALLOCATORS();

    return STATUS_FAILED(retStatus) ? EXIT_FAILURE : EXIT_SUCCESS;
}

PVOID sampleReceiveAudioVideoFrame(PVOID args)
{
    STATUS retStatus = STATUS_SUCCESS;
    PSampleStreamingSession pSampleStreamingSession = (PSampleStreamingSession) args;
    CHK_ERR(pSampleStreamingSession != NULL, STATUS_NULL_ARG, "[KVS Master] Streaming session is NULL");
    CHK_STATUS(transceiverOnFrame(pSampleStreamingSession->pVideoRtcRtpTransceiver, (UINT64) pSampleStreamingSession, sampleVideoFrameHandler));
    CHK_STATUS(transceiverOnFrame(pSampleStreamingSession->pAudioRtcRtpTransceiver, (UINT64) pSampleStreamingSession, sampleAudioFrameHandler));

CleanUp:

    return (PVOID) (ULONG_PTR) retStatus;
}

#include "JzCommon.h"
#include <imp/imp_audio.h>
#include <imp/imp_system.h>

#define INGENIC_AUDIO_DEVICE_ID         1// 0: 数字MIC，1：代表模拟MIC
#define INGENIC_AUDIO_CHANNEL_ID        0
#define INGENIC_AUDIO_CHANNEL_VOLUME    60// 音量
#define INGENIC_AUDIO_GAIN              28// 音频增益

static STATUS audioInputInit(VOID)
{
    STATUS retStatus = STATUS_SUCCESS;

    INT32 devID = INGENIC_AUDIO_DEVICE_ID;// devID:0:数字MIC，1：代表模拟MIC
    IMPAudioIOAttr attr;
    INT32 chnID = INGENIC_AUDIO_CHANNEL_ID;
    IMPAudioIChnParam chnParam;
    INT32 chnVol = INGENIC_AUDIO_CHANNEL_VOLUME;
    INT32 aigain = INGENIC_AUDIO_GAIN;

    /* Step 1: set public attribute of AI device. */
    attr.samplerate = CY_AUDIO_SAMPLE_RATE;
    attr.bitwidth = CY_AUDIO_SAMPLE_BIT_WIDTH;
    attr.soundmode = AUDIO_SOUND_MODE_MONO;
    attr.frmNum = 20;// 音频帧缓存数量
    attr.numPerFrm = CY_AUDIO_SAMPLE_PER_FRAME;
    attr.chnCnt = CY_AUDIO_CHANNELS;
    CHK_STATUS(IMP_AI_SetPubAttr(devID, &attr));

    MEMSET(&attr, 0x0, SIZEOF(attr));
    CHK_STATUS(IMP_AI_GetPubAttr(devID, &attr));

    DLOGV("Audio In GetPubAttr samplerate : %d\n", attr.samplerate);
    DLOGV("Audio In GetPubAttr bitwidth : %d\n",   attr.bitwidth);
    DLOGV("Audio In GetPubAttr soundmode : %d\n",  attr.soundmode);
    DLOGV("Audio In GetPubAttr frmNum : %d\n",     attr.frmNum);
    DLOGV("Audio In GetPubAttr numPerFrm : %d\n",  attr.numPerFrm);
    DLOGV("Audio In GetPubAttr chnCnt : %d\n",     attr.chnCnt);

    /* Step 2: enable AI device. */
    CHK_STATUS(IMP_AI_Enable(devID));

    /* Step 3: set audio channel attribute of AI device. */
    chnParam.usrFrmDepth = 20;
    CHK_STATUS(IMP_AI_SetChnParam(devID, chnID, &chnParam));

    MEMSET(&chnParam, 0x0, SIZEOF(chnParam));
    CHK_STATUS(IMP_AI_GetChnParam(devID, chnID, &chnParam));

    DLOGV("Audio In GetChnParam usrFrmDepth : %d\n", chnParam.usrFrmDepth);

    /* Step 4: enable AI channel. */
    CHK_STATUS(IMP_AI_EnableChn(devID, chnID));

    /* Step 5: Set audio channel volume. */
    CHK_STATUS(IMP_AI_SetVol(devID, chnID, chnVol));

    CHK_STATUS(IMP_AI_GetVol(devID, chnID, &chnVol));
    DLOGV("Audio In GetVol    vol : %d\n", chnVol);

    CHK_STATUS(IMP_AI_SetGain(devID, chnID, aigain));

    CHK_STATUS(IMP_AI_GetGain(devID, chnID, &aigain));
    DLOGV("Audio In GetGain    gain : %d\n", aigain);

CleanUp:
    return retStatus;
}

static STATUS audioInputDeInit(VOID)
{
    STATUS retStatus = STATUS_SUCCESS;
CleanUp:
    return retStatus;
}

static STATUS audioG711EncoderInit(PINT32 handle, INT32 ch)
{
    STATUS retStatus = STATUS_SUCCESS;
    IMPAudioEncEncoder audioEnc;
    IMPAudioEncChnAttr encChnAttr;

    CHK(NULL != handle, STATUS_NULL_ARG);

    audioEnc.maxFrmLen = 1024;
    SPRINTF(audioEnc.name, "%s", "CY_G711A");
    audioEnc.openEncoder = NULL;
    //audioEnc.encoderFrm = CY_G711A_Encode_Frm;
    audioEnc.encoderFrm = NULL;
    audioEnc.closeEncoder = NULL;

    //encChnAttr.type = *handle; /* Use the My method to encoder. if use the system method is encChnAttr.type = PT_G711A; */
    encChnAttr.type = PT_G711A;
    encChnAttr.bufSize = 5;

    CHK_STATUS(IMP_AENC_RegisterEncoder(handle, &audioEnc));
    CHK_STATUS(IMP_AENC_CreateChn(ch, &encChnAttr));

CleanUp:
    return retStatus;
}

static STATUS audioG711EncoderDeInit(PINT32 handle, INT32 ch)
{
    STATUS retStatus = STATUS_SUCCESS;

    CHK(NULL != handle, STATUS_NULL_ARG);

    CHK_STATUS(IMP_AENC_DestroyChn(ch));
    CHK_STATUS(IMP_AENC_UnRegisterEncoder(handle));

CleanUp:
    return retStatus;
}

PVOID sendAudioPackets(PVOID args)
{
    STATUS retStatus = STATUS_SUCCESS;
    PSampleConfiguration pSampleConfiguration = (PSampleConfiguration) args;
    Frame frame;
    UINT32 frameSize;
    UINT32 i;
    STATUS status;

    INT32 devID = INGENIC_AUDIO_DEVICE_ID;
    INT32 chnID = INGENIC_AUDIO_CHANNEL_ID;
    INT32 AeChn = 0;
    INT32 encHandle;

    CHK_ERR(pSampleConfiguration != NULL, STATUS_NULL_ARG, "[KVS Master] Streaming session is NULL");
    frame.presentationTs = 0;

    CHK_STATUS(audioInputInit());
    CHK_STATUS(audioG711EncoderInit(&encHandle, AeChn));

    frameSize = CY_AUDIO_SAMPLE_BUF_SIZE;
    pSampleConfiguration->pAudioFrameBuffer = (UINT8*) MEMREALLOC(pSampleConfiguration->pAudioFrameBuffer, frameSize);
    CHK_ERR(pSampleConfiguration->pAudioFrameBuffer != NULL, STATUS_NOT_ENOUGH_MEMORY, "[KVS Master] Failed to allocate audio frame buffer");
    pSampleConfiguration->audioBufferSize = frameSize;

    frame.frameData = pSampleConfiguration->pAudioFrameBuffer;
    frame.size = frameSize;

    while (!ATOMIC_LOAD_BOOL(&pSampleConfiguration->appTerminateFlag)) {
        CHK_STATUS(IMP_AI_PollingFrame(devID, chnID, 1000));
        IMPAudioFrame frm;
        CHK_STATUS(IMP_AI_GetFrame(devID, chnID, &frm, BLOCK));

        if (NULL == frm.virAddr || 0 == frm.len) {
            continue;
        }

        /* Send a frame to encode. */
        CHK_STATUS(IMP_AENC_SendFrame(AeChn, &frm));
        /* get audio encode frame. */
        IMPAudioStream stream;
        CHK_STATUS(IMP_AENC_PollingStream(AeChn, 1000));
        CHK_STATUS(IMP_AENC_GetStream(AeChn, &stream, BLOCK));

        MEMCPY(frame.frameData, stream.stream, stream.len);
        frame.size = stream.len;
        frame.presentationTs += SAMPLE_AUDIO_FRAME_DURATION;

        /* release stream and frame. */
        CHK_STATUS(IMP_AENC_ReleaseStream(AeChn, &stream));
        CHK_STATUS(IMP_AI_ReleaseFrame(devID, chnID, &frm));

        MUTEX_LOCK(pSampleConfiguration->streamingSessionListReadLock);
        for (i = 0; i < pSampleConfiguration->streamingSessionCount; ++i) {
            status = writeFrame(pSampleConfiguration->sampleStreamingSessionList[i]->pAudioRtcRtpTransceiver, &frame);
            if (status != STATUS_SRTP_NOT_READY_YET) {
                if (status != STATUS_SUCCESS) {
                    DLOGV("writeFrame() failed with 0x%08x", status);
                } else if (pSampleConfiguration->sampleStreamingSessionList[i]->firstFrame && status == STATUS_SUCCESS) {
                    PROFILE_WITH_START_TIME(pSampleConfiguration->sampleStreamingSessionList[i]->offerReceiveTime, "Time to first frame");
                    pSampleConfiguration->sampleStreamingSessionList[i]->firstFrame = FALSE;
                }
            }
        }
        MUTEX_UNLOCK(pSampleConfiguration->streamingSessionListReadLock);
        THREAD_SLEEP(SAMPLE_AUDIO_FRAME_DURATION);
    }

CleanUp:
    DLOGI("[KVS Master] closing audio thread");

    retStatus = audioG711EncoderDeInit(&encHandle, AeChn);
    //retStatus = audioInputDeInit();

    return (PVOID) (ULONG_PTR) retStatus;
}

extern struct chn_conf chn[];

PVOID sendVideoPackets(PVOID args)
{
    STATUS retStatus = STATUS_SUCCESS;
    PSampleConfiguration pSampleConfiguration = (PSampleConfiguration) args;
    RtcEncoderStats encoderStats;
    Frame frame;
    UINT32 frameSize = 0;
    STATUS status;
    UINT32 i;
    UINT64 startTime, lastFrameTime, elapsed;
    MEMSET(&encoderStats, 0x00, SIZEOF(RtcEncoderStats));
    CHK_ERR(pSampleConfiguration != NULL, STATUS_NULL_ARG, "[KVS Master] Streaming session is NULL");

    frame.presentationTs = 0;
    startTime = GETTIME();
    lastFrameTime = startTime;

    i = 0;
    init_chn();

    /* Step.1 System init */
    CHK_STATUS_ERR(sample_system_init(), -1, "IMP_System_Init() failed\n");

    /* Step.2 FrameSource init */
    CHK_STATUS_ERR(sample_framesource_init(), -1, "FrameSource init failed\n");
    retStatus = sample_framesource_init();

    /* Step.3 Encoder init */
    //for (i = 0; i < FS_CHN_NUM; i++) {
        if (chn[i].enable) {
            CHK_STATUS_ERR(IMP_Encoder_CreateGroup(chn[i].index), -1, "IMP_Encoder_CreateGroup(%d) error !\n", chn[i].index);
        }
    //}

    CHK_STATUS_ERR(sample_encoder_init(), -1, "Encoder init failed\n");

    /* Step.4 Bind */
    //for (i = 0; i < FS_CHN_NUM; i++) {
        if (chn[i].enable) {
            CHK_STATUS_ERR(IMP_System_Bind(&chn[i].framesource_chn, &chn[i].imp_encoder),
                           -1, "Bind FrameSource channel%d and Encoder failed\n", i);
        }
    //}

    int val, chnNum, ret;
    IMPEncoderEncType encType;
    val = (int) (((chn[i].payloadType >> 24) << 16) | chn[i].index);
    chnNum = val & 0xffff;
    encType = (IMPEncoderEncType)((val >> 16) & 0xffff);
    ret = sample_framesource_streamon();
    ret = IMP_Encoder_StartRecvPic(chnNum);
    if (ret < 0) {
        DLOGE("IMP_Encoder_StartRecvPic(%d) failed\n", chnNum);
        return NULL;
    }

    while (!ATOMIC_LOAD_BOOL(&pSampleConfiguration->appTerminateFlag)) {
        ret = IMP_Encoder_PollingStream(chnNum, 1000);
        if (ret < 0) {
            DLOGE("IMP_Encoder_PollingStream(%d) timeout\n", chnNum);
            continue;
        }

        IMPEncoderStream stream;
        /* Get H264 or H265 Stream */
        ret = IMP_Encoder_GetStream(chnNum, &stream, 1);
        if (ret < 0) {
            DLOGE("IMP_Encoder_GetStream(%d) failed\n", chnNum);
            break;
        }

        frameSize = 0;
        for (i=0; i<stream.packCount; i++) {
            IMPEncoderPack* pack = &stream.pack[i];
            frameSize += pack->length;
        }

        // Re-alloc if needed
        if (frameSize > pSampleConfiguration->videoBufferSize) {
            pSampleConfiguration->pVideoFrameBuffer = (PBYTE) MEMREALLOC(pSampleConfiguration->pVideoFrameBuffer, frameSize);
            CHK_ERR(pSampleConfiguration->pVideoFrameBuffer != NULL, STATUS_NOT_ENOUGH_MEMORY, "[KVS Master] Failed to allocate video frame buffer");
            pSampleConfiguration->videoBufferSize = frameSize;
        }

        frame.frameData = pSampleConfiguration->pVideoFrameBuffer;
        frame.size = 0;

        for (i=0; i<stream.packCount; i++) {
            IMPEncoderPack* pack = &stream.pack[i];
            if (0 == pack->length) {
                continue;
            }

            MEMCPY(frame.frameData + frame.size, (void *)(stream.virAddr + pack->offset), pack->length);
            frame.size += pack->length;
        }
        IMP_Encoder_ReleaseStream(chnNum, &stream);

        // based on bitrate of samples/h264SampleFrames/frame-*
        encoderStats.width = CY_VIDEO_WIDTH;
        encoderStats.height = CY_VIDEO_HEIGHT;
        encoderStats.targetBitrate = 262000;
        frame.presentationTs += SAMPLE_VIDEO_FRAME_DURATION;
        MUTEX_LOCK(pSampleConfiguration->streamingSessionListReadLock);
        for (i = 0; i < pSampleConfiguration->streamingSessionCount; ++i) {
            status = writeFrame(pSampleConfiguration->sampleStreamingSessionList[i]->pVideoRtcRtpTransceiver, &frame);
            if (pSampleConfiguration->sampleStreamingSessionList[i]->firstFrame && status == STATUS_SUCCESS) {
                PROFILE_WITH_START_TIME(pSampleConfiguration->sampleStreamingSessionList[i]->offerReceiveTime, "Time to first frame");
                pSampleConfiguration->sampleStreamingSessionList[i]->firstFrame = FALSE;
            }
            encoderStats.encodeTimeMsec = 4; // update encode time to an arbitrary number to demonstrate stats update
            updateEncoderStats(pSampleConfiguration->sampleStreamingSessionList[i]->pVideoRtcRtpTransceiver, &encoderStats);
            if (status != STATUS_SRTP_NOT_READY_YET) {
                if (status != STATUS_SUCCESS) {
                    DLOGV("writeFrame() failed with 0x%08x", status);
                }
            }
        }
        MUTEX_UNLOCK(pSampleConfiguration->streamingSessionListReadLock);

        // Adjust sleep in the case the sleep itself and writeFrame take longer than expected. Since sleep makes sure that the thread
        // will be paused at least until the given amount, we can assume that there's no too early frame scenario.
        // Also, it's very unlikely to have a delay greater than SAMPLE_VIDEO_FRAME_DURATION, so the logic assumes that this is always
        // true for simplicity.
        elapsed = lastFrameTime - startTime;
        THREAD_SLEEP(SAMPLE_VIDEO_FRAME_DURATION - elapsed % SAMPLE_VIDEO_FRAME_DURATION);
        lastFrameTime = GETTIME();
    }

CleanUp:
    DLOGI("[KVS Master] Closing video thread");

    /* Step.b UnBind */

    if (chn[0].enable) {
        CHK_STATUS_ERR(IMP_System_UnBind(&chn[0].framesource_chn, &chn[0].imp_encoder),
                       -1, "UnBind FrameSource channel0 and Encoder failed\n");
//        ret = IMP_System_UnBind(&chn[0].framesource_chn, &chn[0].imp_encoder);
//        if (ret < 0) {
//            DLOGE("UnBind FrameSource channel0 and Encoder failed\n");
//        }
    }

    /* Step.c Encoder exit */
    CHK_STATUS_ERR(sample_encoder_exit(), -1, "Encoder exit failed\n");
//    ret = sample_encoder_exit();
//    if (ret < 0) {
//        DLOGE("Encoder exit failed\n");
//        return;
//    }

    /* Step.d FrameSource exit */
    CHK_STATUS_ERR(sample_framesource_exit(), -1, "FrameSource exit failed\n");
//    ret = sample_framesource_exit();
//    if (ret < 0) {
//        DLOGE("FrameSource exit failed\n");
//        return;
//    }

    /* Step.e System exit */
    CHK_STATUS_ERR(sample_system_exit(), -1, "sample_system_exit() failed\n");
//    ret = sample_system_exit();
//    if (ret < 0) {
//        DLOGE("sample_system_exit() failed\n");
//        return;
//    }

    CHK_LOG_ERR(retStatus);

    return (PVOID) (ULONG_PTR) retStatus;
}
