//
// Copyright (c) 2023 gznscy
//

#include "Samples.h"

extern PSampleConfiguration gSampleConfiguration;

INT32 main(INT32 argc, CHAR* argv[])
{
    STATUS retStatus = STATUS_SUCCESS;
    UINT32 frameSize;
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

//#ifndef __x86_64
//    netWlanConfigure("TP_LINK", "123456");
//    CHK_STATUS(netSyncNtp());
//#endif

    CHK_STATUS(createSampleConfiguration(pChannelName, SIGNALING_CHANNEL_ROLE_TYPE_MASTER, TRUE, TRUE, logLevel, &pSampleConfiguration));

    // Set the audio and video handlers
    pSampleConfiguration->audioSource = sendAudioPackets;
    pSampleConfiguration->videoSource = sendVideoPackets;
    pSampleConfiguration->receiveAudioVideoSource = sampleReceiveAudioVideoFrame;

    CHK_STATUS(readFrameFromDisk(NULL, &frameSize, "./h264SampleFrames/frame-0001.h264"));
    DLOGI("[CyIpc] Checked sample video frame availability....available");

    CHK_STATUS(readFrameFromDisk(NULL, &frameSize, "./pcmSample/Nocturne-8KHz-16bit-mono.pcm"));
    DLOGI("[CyIpc] Checked sample audio frame availability....available");

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

STATUS readFrameFromDisk(PBYTE pFrame, PUINT32 pSize, PCHAR frameFilePath)
{
    STATUS retStatus = STATUS_SUCCESS;
    UINT64 size = 0;
    CHK_ERR(pSize != NULL, STATUS_NULL_ARG, "[CyIpc] Invalid file size");
    size = *pSize;
    // Get the size and read into frame
    CHK_STATUS(readFile(frameFilePath, TRUE, pFrame, &size));
CleanUp:

    if (pSize != NULL) {
        *pSize = (UINT32) size;
    }

    return retStatus;
}

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

//#define CY_AUDIO_SAMPLES_FILE              "./wavSample/equinox-48KHz-16bit-stereo.wav"
#define CY_AUDIO_SAMPLES_FILE              "./pcmSample/Nocturne-8KHz-16bit-mono.pcm"

#include "g711.h"

VOID convertPcmToG711(PINT16 in, UINT32 in_size, PBYTE out, PUINT32 out_size)
{
    UINT32 inSampleLen = in_size / SIZEOF(INT16);
    UINT32 i, j;

    for (i=0, j=0; i<inSampleLen; i++, j++) {
        out[j] = linear2alaw(in[i]);
    }
    *out_size = j;
}

PVOID sendVideoPackets(PVOID args)
{
    STATUS retStatus = STATUS_SUCCESS;
    PSampleConfiguration pSampleConfiguration = (PSampleConfiguration) args;
    RtcEncoderStats encoderStats;
    Frame frame;
    UINT32 fileIndex = 0, frameSize;
    CHAR filePath[MAX_PATH_LEN + 1];
    STATUS status;
    UINT32 i;
    UINT64 startTime, lastFrameTime, elapsed;
    MEMSET(&encoderStats, 0x00, SIZEOF(RtcEncoderStats));
    CHK_ERR(pSampleConfiguration != NULL, STATUS_NULL_ARG, "[KVS Master] Streaming session is NULL");

    frame.presentationTs = 0;
    startTime = GETTIME();
    lastFrameTime = startTime;

    while (!ATOMIC_LOAD_BOOL(&pSampleConfiguration->appTerminateFlag)) {
        fileIndex = fileIndex % NUMBER_OF_H264_FRAME_FILES + 1;
        SNPRINTF(filePath, MAX_PATH_LEN, "./h264SampleFrames/frame-%04d.h264", fileIndex);

        CHK_STATUS(readFrameFromDisk(NULL, &frameSize, filePath));

        // Re-alloc if needed
        if (frameSize > pSampleConfiguration->videoBufferSize) {
            pSampleConfiguration->pVideoFrameBuffer = (PBYTE) MEMREALLOC(pSampleConfiguration->pVideoFrameBuffer, frameSize);
            CHK_ERR(pSampleConfiguration->pVideoFrameBuffer != NULL, STATUS_NOT_ENOUGH_MEMORY, "[KVS Master] Failed to allocate video frame buffer");
            pSampleConfiguration->videoBufferSize = frameSize;
        }

        frame.frameData = pSampleConfiguration->pVideoFrameBuffer;
        frame.size = frameSize;

        CHK_STATUS(readFrameFromDisk(frame.frameData, &frameSize, filePath));

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
    CHK_LOG_ERR(retStatus);

    return (PVOID) (ULONG_PTR) retStatus;
}

PVOID sendAudioPackets(PVOID args)
{
    STATUS retStatus = STATUS_SUCCESS;
    PSampleConfiguration pSampleConfiguration = (PSampleConfiguration) args;
    Frame frame;
    UINT32 fileIndex = 0, frameSize;
    CHAR filePath[MAX_PATH_LEN + 1];
    UINT32 i;
    STATUS status;
    BYTE buf[CY_AUDIO_SAMPLE_BUF_SIZE];// pcm size = 48 样本/ms  X 20ms X 2byte X 2 channel = 3840 byte
//    INT32 samplePerFrame = CY_AUDIO_SAMPLE_PER_FRAME;
    // 本地文件推流
    FILE* fp = NULL;
    BYTE g711Buf[CY_AUDIO_SAMPLE_BUF_SIZE / SIZEOF(INT16)];
    UINT32 g711DataSize;

    CHK_ERR(pSampleConfiguration != NULL, STATUS_NULL_ARG, "[KVS Master] Streaming session is NULL");
    frame.presentationTs = 0;

    frameSize = SIZEOF(buf);
    pSampleConfiguration->pAudioFrameBuffer = (UINT8*) MEMREALLOC(pSampleConfiguration->pAudioFrameBuffer, frameSize);
    CHK_ERR(pSampleConfiguration->pAudioFrameBuffer != NULL, STATUS_NOT_ENOUGH_MEMORY, "[KVS Master] Failed to allocate audio frame buffer");
    pSampleConfiguration->audioBufferSize = frameSize;
    frame.frameData = pSampleConfiguration->pAudioFrameBuffer;
    frame.size = frameSize;

    SNPRINTF(filePath, MAX_PATH_LEN, CY_AUDIO_SAMPLES_FILE);
    CHK(filePath != NULL, STATUS_NULL_ARG);
    fp = FOPEN(filePath, "rb");
    CHK(fp != NULL, STATUS_OPEN_FILE_FAILED);
//    FSEEK(fp, 44, 0);// skip wav header

    while (!ATOMIC_LOAD_BOOL(&pSampleConfiguration->appTerminateFlag)) {
        if (1 != FREAD(buf, (SIZE_T) SIZEOF(buf), 1, fp)) {
            fseek(fp, 0, SEEK_SET);
        }

        convertPcmToG711((PINT16) buf, SIZEOF(buf), frame.frameData, &g711DataSize);

        frame.size = SIZEOF(g711Buf);
        frame.presentationTs += SAMPLE_AUDIO_FRAME_DURATION;

        MUTEX_LOCK(pSampleConfiguration->streamingSessionListReadLock);
        for (i = 0; i < pSampleConfiguration->streamingSessionCount; ++i) {
            status = writeFrame(pSampleConfiguration->sampleStreamingSessionList[i]->pAudioRtcRtpTransceiver, &frame);
            if (status != STATUS_SRTP_NOT_READY_YET) {
                if (status != STATUS_SUCCESS) {
                    DLOGV("writeFrame() failed with 0x%08x", status);
                } else if (pSampleConfiguration->sampleStreamingSessionList[i]->firstFrame) {
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
    return (PVOID) (ULONG_PTR) retStatus;
}
