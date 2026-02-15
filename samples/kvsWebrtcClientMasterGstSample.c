#include "Samples.h"
#include <gst/gst.h>
#include <gst/app/gstappsink.h>

extern PSampleConfiguration gSampleConfiguration;
// #define VERBOSE

GstElement* senderPipeline = NULL;

GstFlowReturn on_new_sample(GstElement* sink, gpointer data, UINT64 trackid)
{
    GstBuffer* buffer;
    STATUS retStatus = STATUS_SUCCESS;
    BOOL isDroppable, delta;
    GstFlowReturn ret = GST_FLOW_OK;
    GstSample* sample = NULL;
    GstMapInfo info;
    GstSegment* segment;
    GstClockTime buf_pts;
    Frame frame;
    STATUS status;
    PSampleConfiguration pSampleConfiguration = (PSampleConfiguration) data;
    PSampleStreamingSession pSampleStreamingSession = NULL;
    PRtcRtpTransceiver pRtcRtpTransceiver = NULL;
    UINT32 i;
    guint bitrate;

    CHK_ERR(pSampleConfiguration != NULL, STATUS_NULL_ARG, "NULL sample configuration");

    info.data = NULL;
    sample = gst_app_sink_pull_sample(GST_APP_SINK(sink));

    buffer = gst_sample_get_buffer(sample);
    isDroppable = GST_BUFFER_FLAG_IS_SET(buffer, GST_BUFFER_FLAG_CORRUPTED) || GST_BUFFER_FLAG_IS_SET(buffer, GST_BUFFER_FLAG_DECODE_ONLY) ||
        (GST_BUFFER_FLAGS(buffer) == GST_BUFFER_FLAG_DISCONT) ||
        (GST_BUFFER_FLAG_IS_SET(buffer, GST_BUFFER_FLAG_DISCONT) && GST_BUFFER_FLAG_IS_SET(buffer, GST_BUFFER_FLAG_DELTA_UNIT)) ||
        // drop if buffer contains header only and has invalid timestamp
        !GST_BUFFER_PTS_IS_VALID(buffer);

    if (!isDroppable) {
        delta = GST_BUFFER_FLAG_IS_SET(buffer, GST_BUFFER_FLAG_DELTA_UNIT);

        frame.flags = delta ? FRAME_FLAG_NONE : FRAME_FLAG_KEY_FRAME;

        // convert from segment timestamp to running time in live mode.
        segment = gst_sample_get_segment(sample);
        buf_pts = gst_segment_to_running_time(segment, GST_FORMAT_TIME, buffer->pts);
        if (!GST_CLOCK_TIME_IS_VALID(buf_pts)) {
            DLOGE("[KVS GStreamer Master] Frame contains invalid PTS dropping the frame");
        }

        if (!(gst_buffer_map(buffer, &info, GST_MAP_READ))) {
            DLOGE("[KVS GStreamer Master] on_new_sample(): Gst buffer mapping failed");
            goto CleanUp;
        }

        frame.trackId = trackid;
        frame.duration = 0;
        frame.version = FRAME_CURRENT_VERSION;
        frame.size = (UINT32) info.size;
        frame.frameData = (PBYTE) info.data;

        MUTEX_LOCK(pSampleConfiguration->streamingSessionListReadLock);
        for (i = 0; i < pSampleConfiguration->streamingSessionCount; ++i) {
            pSampleStreamingSession = pSampleConfiguration->sampleStreamingSessionList[i];
            frame.index = (UINT32) ATOMIC_INCREMENT(&pSampleStreamingSession->frameIndex);

            if (trackid == DEFAULT_AUDIO_TRACK_ID) {
                if (pSampleStreamingSession->pSampleConfiguration->enableTwcc && senderPipeline != NULL) {
                    GstElement* encoder = gst_bin_get_by_name(GST_BIN(senderPipeline), "sampleAudioEncoder");
                    if (encoder != NULL) {
                        g_object_get(G_OBJECT(encoder), "bitrate", &bitrate, NULL);
                        MUTEX_LOCK(pSampleStreamingSession->twccMetadata.updateLock);
                        pSampleStreamingSession->twccMetadata.currentAudioBitrate = (UINT64) bitrate;
                        if (pSampleStreamingSession->twccMetadata.newAudioBitrate != 0) {
                            bitrate = (guint) (pSampleStreamingSession->twccMetadata.newAudioBitrate);
                            pSampleStreamingSession->twccMetadata.newAudioBitrate = 0;
                            g_object_set(G_OBJECT(encoder), "bitrate", bitrate, NULL);
                        }
                        MUTEX_UNLOCK(pSampleStreamingSession->twccMetadata.updateLock);
                    }
                }
                pRtcRtpTransceiver = pSampleStreamingSession->pAudioRtcRtpTransceiver;
                frame.presentationTs = pSampleStreamingSession->audioTimestamp;
                frame.decodingTs = frame.presentationTs;
                pSampleStreamingSession->audioTimestamp +=
                    SAMPLE_AUDIO_FRAME_DURATION; // assume audio frame size is 20ms, which is default in opusenc
            } else {
                if (pSampleStreamingSession->pSampleConfiguration->enableTwcc && senderPipeline != NULL) {
                    GstElement* encoder = gst_bin_get_by_name(GST_BIN(senderPipeline), "sampleVideoEncoder");
                    if (encoder != NULL) {
                        g_object_get(G_OBJECT(encoder), "bitrate", &bitrate, NULL);
                        MUTEX_LOCK(pSampleStreamingSession->twccMetadata.updateLock);
                        pSampleStreamingSession->twccMetadata.currentVideoBitrate = (UINT64) bitrate;
                        if (pSampleStreamingSession->twccMetadata.newVideoBitrate != 0) {
                            bitrate = (guint) (pSampleStreamingSession->twccMetadata.newVideoBitrate);
                            pSampleStreamingSession->twccMetadata.newVideoBitrate = 0;
                            g_object_set(G_OBJECT(encoder), "bitrate", bitrate, NULL);
                        }
                        MUTEX_UNLOCK(pSampleStreamingSession->twccMetadata.updateLock);
                    }
                }
                pRtcRtpTransceiver = pSampleStreamingSession->pVideoRtcRtpTransceiver;
                frame.presentationTs = pSampleStreamingSession->videoTimestamp;
                frame.decodingTs = frame.presentationTs;
                pSampleStreamingSession->videoTimestamp += SAMPLE_VIDEO_FRAME_DURATION; // assume video fps is 25
            }
            status = writeFrame(pRtcRtpTransceiver, &frame);
            if (status != STATUS_SRTP_NOT_READY_YET && status != STATUS_SUCCESS) {
#ifdef VERBOSE
                DLOGE("[KVS GStreamer Master] writeFrame() failed with 0x%08x", status);
#endif
            } else if (status == STATUS_SUCCESS && pSampleStreamingSession->firstFrame) {
                PROFILE_WITH_START_TIME(pSampleStreamingSession->offerReceiveTime, "Time to first frame");
                pSampleStreamingSession->firstFrame = FALSE;
            } else if (status == STATUS_SRTP_NOT_READY_YET) {
                DLOGI("[KVS GStreamer Master] SRTP not ready yet, dropping frame");
            }
        }
        MUTEX_UNLOCK(pSampleConfiguration->streamingSessionListReadLock);
    }

CleanUp:

    if (info.data != NULL) {
        gst_buffer_unmap(buffer, &info);
    }

    if (sample != NULL) {
        gst_sample_unref(sample);
    }

    if (ATOMIC_LOAD_BOOL(&pSampleConfiguration->appTerminateFlag)) {
        ret = GST_FLOW_EOS;
    }

    return ret;
}

GstFlowReturn on_new_sample_video(GstElement* sink, gpointer data)
{
    return on_new_sample(sink, data, DEFAULT_VIDEO_TRACK_ID);
}

GstFlowReturn on_new_sample_audio(GstElement* sink, gpointer data)
{
    return on_new_sample(sink, data, DEFAULT_AUDIO_TRACK_ID);
}

PVOID sendGstreamerAudioVideo(PVOID args)
{
    STATUS retStatus = STATUS_SUCCESS;
    GstElement *appsinkVideo = NULL, *appsinkAudio = NULL;
    GstBus* bus;
    GstMessage* msg;
    GError* error = NULL;
    PSampleConfiguration pSampleConfiguration = (PSampleConfiguration) args;

    CHK_ERR(pSampleConfiguration != NULL, STATUS_NULL_ARG, "[KVS Gstreamer Master] Streaming session is NULL");

    /**
     * Use x264enc as its available on mac, pi, ubuntu and windows
     * mac pipeline fails if resolution is not 720p
     *
     * For alaw
     * audiotestsrc is-live=TRUE ! queue leaky=2 max-size-buffers=400 ! audioconvert ! audioresample !
     * audio/x-raw, rate=8000, channels=1, format=S16LE, layout=interleaved ! alawenc ! appsink sync=TRUE emit-signals=TRUE name=appsink-audio
     *
     * For VP8
     * videotestsrc is-live=TRUE ! video/x-raw,width=1280,height=720,framerate=30/1 !
     * vp8enc error-resilient=partitions keyframe-max-dist=10 auto-alt-ref=true cpu-used=5 deadline=1 !
     * appsink sync=TRUE emit-signals=TRUE name=appsink-video
     *
     *
     * Raspberry Pi Hardware Encode Example
     * "v4l2src device=\"/dev/video0\" ! queue ! v4l2convert ! "
     * "video/x-raw,format=I420,width=640,height=480,framerate=30/1 ! "
     * "v4l2h264enc ! "
     * "h264parse ! "
     * "video/x-h264,stream-format=byte-stream,alignment=au,width=640,height=480,framerate=30/1,profile=baseline,level=(string)4 ! "
     * "appsink sync=TRUE emit-signals=TRUE name=appsink-video"
     */

    CHAR rtspPipeLineBuffer[RTSP_PIPELINE_MAX_CHAR_COUNT];

    // Read video pipeline configuration from environment variables (with sensible defaults).
    // This allows tuning for different hardware (e.g. Raspberry Pi 5 vs laptop) without recompiling.
    CHAR* envVal = NULL;
    INT32 videoWidth = 1280;
    INT32 videoHeight = 720;
    INT32 videoFps = 25;
    INT32 videoBitrate = 512;
    CHAR encoderPreset[32] = "veryfast";
    CHAR pipelineBuf[1024];

    envVal = GETENV("KVS_VIDEO_WIDTH");
    if (envVal != NULL && envVal[0] != '\0') videoWidth = (INT32) STRTOUL(envVal, NULL, 10);
    envVal = GETENV("KVS_VIDEO_HEIGHT");
    if (envVal != NULL && envVal[0] != '\0') videoHeight = (INT32) STRTOUL(envVal, NULL, 10);
    envVal = GETENV("KVS_VIDEO_FPS");
    if (envVal != NULL && envVal[0] != '\0') videoFps = (INT32) STRTOUL(envVal, NULL, 10);
    envVal = GETENV("KVS_VIDEO_BITRATE");
    if (envVal != NULL && envVal[0] != '\0') videoBitrate = (INT32) STRTOUL(envVal, NULL, 10);
    envVal = GETENV("KVS_ENCODER_PRESET");
    if (envVal != NULL && envVal[0] != '\0') {
        STRNCPY(encoderPreset, envVal, SIZEOF(encoderPreset) - 1);
        encoderPreset[SIZEOF(encoderPreset) - 1] = '\0';
    }

    DLOGI("[KVS GStreamer Master] Pipeline config: %dx%d @ %d fps, bitrate=%d, preset=%s",
          videoWidth, videoHeight, videoFps, videoBitrate, encoderPreset);

    switch (pSampleConfiguration->mediaType) {
        case SAMPLE_STREAMING_VIDEO_ONLY:
            switch (pSampleConfiguration->srcType) {
                case TEST_SOURCE: {
                    if (pSampleConfiguration->videoCodec == RTC_CODEC_H265) {
                        senderPipeline = gst_parse_launch("videotestsrc pattern=ball is-live=TRUE ! timeoverlay ! queue ! videoconvert ! "
                                                          "video/x-raw,width=1280,height=720,framerate=25/1 ! queue ! "
                                                          "x265enc speed-preset=veryfast bitrate=512 tune=zerolatency ! "
                                                          "video/x-h265,stream-format=byte-stream,alignment=au,profile=main ! appsink sync=TRUE "
                                                          "emit-signals=TRUE name=appsink-video",
                                                          &error);
                    } else {
                        senderPipeline = gst_parse_launch(
                            "videotestsrc pattern=ball is-live=TRUE ! "
                            "queue ! videoconvert ! videoscale ! video/x-raw,width=1280,height=720 ! "
                            "clockoverlay halignment=right valignment=top time-format=\"%Y-%m-%d %H:%M:%S\" ! "
                            "videorate ! video/x-raw,framerate=25/1 ! "
                            "x264enc name=sampleVideoEncoder bframes=0 speed-preset=veryfast bitrate=512 byte-stream=TRUE tune=zerolatency ! "
                            "video/x-h264,stream-format=byte-stream,alignment=au,profile=baseline ! "
                            "appsink sync=TRUE emit-signals=TRUE name=appsink-video",
                            &error);
                    }
                    break;
                }
                case DEVICE_SOURCE: {
                    GstElement* videoSrc = NULL;
                    GstElement* downstreamBin = NULL;

                    if (pSampleConfiguration->pSelectedVideoDevice != NULL) {
                        videoSrc = gst_device_create_element((GstDevice*) pSampleConfiguration->pSelectedVideoDevice, "videoSrc");
                    }
                    if (videoSrc == NULL) {
                        DLOGI("[KVS GStreamer Master] No specific device selected, falling back to autovideosrc");
                        videoSrc = gst_element_factory_make("autovideosrc", "videoSrc");
                    }

                    // Build pipeline dynamically from env-var-configurable parameters.
                    // videoscale + videorate ensure the camera output is converted to our target
                    // resolution/framerate regardless of the camera's native capabilities.
                    SNPRINTF(pipelineBuf, SIZEOF(pipelineBuf),
                        "queue ! videoconvert ! videoscale ! video/x-raw,width=%d,height=%d ! "
                        "videorate ! video/x-raw,framerate=%d/1 ! "
                        "x264enc name=sampleVideoEncoder bframes=0 speed-preset=%s bitrate=%d byte-stream=TRUE tune=zerolatency ! "
                        "video/x-h264,stream-format=byte-stream,alignment=au,profile=baseline ! "
                        "appsink sync=TRUE emit-signals=TRUE name=appsink-video",
                        videoWidth, videoHeight, videoFps, encoderPreset, videoBitrate);

                    downstreamBin = gst_parse_bin_from_description(pipelineBuf, TRUE, &error);

                    if (videoSrc != NULL && downstreamBin != NULL) {
                        senderPipeline = gst_pipeline_new("device-sender-pipeline");
                        gst_bin_add_many(GST_BIN(senderPipeline), videoSrc, downstreamBin, NULL);
                        if (!gst_element_link(videoSrc, downstreamBin)) {
                            DLOGE("[KVS GStreamer Master] Failed to link video source to downstream pipeline");
                            gst_object_unref(senderPipeline);
                            senderPipeline = NULL;
                        }
                    } else {
                        if (videoSrc != NULL) gst_object_unref(videoSrc);
                        if (downstreamBin != NULL) gst_object_unref(downstreamBin);
                    }
                    break;
                }
                case RTSP_SOURCE: {
                    UINT16 stringOutcome =
                        SNPRINTF(rtspPipeLineBuffer, RTSP_PIPELINE_MAX_CHAR_COUNT,
                                 "uridecodebin uri=%s ! "
                                 "videoconvert ! "
                                 "x264enc name=sampleVideoEncoder bframes=0 speed-preset=veryfast bitrate=512 byte-stream=TRUE tune=zerolatency ! "
                                 "video/x-h264,stream-format=byte-stream,alignment=au,profile=baseline ! queue ! "
                                 "appsink sync=TRUE emit-signals=TRUE name=appsink-video ",
                                 pSampleConfiguration->rtspUri);

                    if (stringOutcome > RTSP_PIPELINE_MAX_CHAR_COUNT) {
                        DLOGE("[KVS GStreamer Master] ERROR: rtsp uri entered exceeds maximum allowed length set by RTSP_PIPELINE_MAX_CHAR_COUNT");
                        goto CleanUp;
                    }
                    senderPipeline = gst_parse_launch(rtspPipeLineBuffer, &error);

                    break;
                }
            }
            break;

        case SAMPLE_STREAMING_AUDIO_VIDEO:
            switch (pSampleConfiguration->srcType) {
                case TEST_SOURCE: {
                    if (pSampleConfiguration->videoCodec == RTC_CODEC_H264_PROFILE_42E01F_LEVEL_ASYMMETRY_ALLOWED_PACKETIZATION_MODE &&
                        pSampleConfiguration->audioCodec == RTC_CODEC_OPUS) {
                        senderPipeline = gst_parse_launch(
                            "videotestsrc pattern=ball is-live=TRUE ! "
                            "queue ! videorate ! videoscale ! videoconvert ! video/x-raw,width=1280,height=720,framerate=25/1 ! "
                            "clockoverlay halignment=right valignment=top time-format=\"%Y-%m-%d %H:%M:%S\" ! "
                            "x264enc name=sampleVideoEncoder bframes=0 speed-preset=veryfast bitrate=512 byte-stream=TRUE tune=zerolatency ! "
                            "video/x-h264,stream-format=byte-stream,alignment=au,profile=baseline ! "
                            "appsink sync=TRUE emit-signals=TRUE name=appsink-video audiotestsrc wave=ticks is-live=TRUE ! "
                            "queue leaky=2 max-size-buffers=400 ! audioconvert ! audioresample ! opusenc name=sampleAudioEncoder ! "
                            "audio/x-opus,rate=48000,channels=2 ! appsink sync=TRUE emit-signals=TRUE name=appsink-audio",
                            &error);
                    } else if (pSampleConfiguration->videoCodec == RTC_CODEC_H265 && pSampleConfiguration->audioCodec == RTC_CODEC_OPUS) {
                        senderPipeline =
                            gst_parse_launch("videotestsrc pattern=ball is-live=TRUE ! timeoverlay ! queue ! videoconvert ! "
                                             "video/x-raw,width=1280,height=720,framerate=25/1 ! queue ! "
                                             "x265enc speed-preset=veryfast bitrate=512 tune=zerolatency ! "
                                             "video/x-h265,stream-format=byte-stream,alignment=au,profile=main ! appsink sync=TRUE "
                                             "emit-signals=TRUE name=appsink-video audiotestsrc is-live=TRUE ! "
                                             "queue leaky=2 max-size-buffers=400 ! audioconvert ! audioresample ! opusenc ! "
                                             "audio/x-opus,rate=48000,channels=2 ! appsink sync=TRUE emit-signals=TRUE name=appsink-audio",
                                             &error);
                    }
                    // TODO: test and add more such combinations
                    break;
                }
                case DEVICE_SOURCE: {
                    GstElement* videoSrc = NULL;
                    GstElement* downstreamBin = NULL;

                    if (pSampleConfiguration->pSelectedVideoDevice != NULL) {
                        videoSrc = gst_device_create_element((GstDevice*) pSampleConfiguration->pSelectedVideoDevice, "videoSrc");
                    }
                    if (videoSrc == NULL) {
                        DLOGI("[KVS GStreamer Master] No specific device selected, falling back to autovideosrc");
                        videoSrc = gst_element_factory_make("autovideosrc", "videoSrc");
                    }

                    // Build pipeline dynamically from env-var-configurable parameters (audio+video)
                    SNPRINTF(pipelineBuf, SIZEOF(pipelineBuf),
                        "queue ! videoconvert ! videoscale ! video/x-raw,width=%d,height=%d ! "
                        "videorate ! video/x-raw,framerate=%d/1 ! "
                        "x264enc name=sampleVideoEncoder bframes=0 speed-preset=%s bitrate=%d byte-stream=TRUE tune=zerolatency ! "
                        "video/x-h264,stream-format=byte-stream,alignment=au,profile=baseline ! appsink sync=TRUE emit-signals=TRUE "
                        "name=appsink-video autoaudiosrc ! "
                        "queue leaky=2 max-size-buffers=400 ! audioconvert ! audioresample ! opusenc name=sampleAudioEncoder ! "
                        "audio/x-opus,rate=48000,channels=2 ! appsink sync=TRUE emit-signals=TRUE name=appsink-audio",
                        videoWidth, videoHeight, videoFps, encoderPreset, videoBitrate);

                    downstreamBin = gst_parse_bin_from_description(pipelineBuf, TRUE, &error);

                    if (videoSrc != NULL && downstreamBin != NULL) {
                        senderPipeline = gst_pipeline_new("device-sender-pipeline");
                        gst_bin_add_many(GST_BIN(senderPipeline), videoSrc, downstreamBin, NULL);
                        if (!gst_element_link(videoSrc, downstreamBin)) {
                            DLOGE("[KVS GStreamer Master] Failed to link video source to downstream pipeline");
                            gst_object_unref(senderPipeline);
                            senderPipeline = NULL;
                        }
                    } else {
                        if (videoSrc != NULL) gst_object_unref(videoSrc);
                        if (downstreamBin != NULL) gst_object_unref(downstreamBin);
                    }
                    break;
                }
                case RTSP_SOURCE: {
                    UINT16 stringOutcome =
                        SNPRINTF(rtspPipeLineBuffer, RTSP_PIPELINE_MAX_CHAR_COUNT,
                                 "uridecodebin uri=%s name=src ! videoconvert ! "
                                 "x264enc name=sampleVideoEncoder bframes=0 speed-preset=veryfast bitrate=512 byte-stream=TRUE tune=zerolatency ! "
                                 "video/x-h264,stream-format=byte-stream,alignment=au,profile=baseline ! queue ! "
                                 "appsink sync=TRUE emit-signals=TRUE name=appsink-video "
                                 "src. ! audioconvert ! "
                                 "audioresample ! opusenc name=sampleAudioEncoder ! audio/x-opus,rate=48000,channels=2 ! queue ! "
                                 "appsink sync=TRUE emit-signals=TRUE name=appsink-audio",
                                 pSampleConfiguration->rtspUri);

                    if (stringOutcome > RTSP_PIPELINE_MAX_CHAR_COUNT) {
                        DLOGE("[KVS GStreamer Master] ERROR: rtsp uri entered exceeds maximum allowed length set by RTSP_PIPELINE_MAX_CHAR_COUNT");
                        goto CleanUp;
                    }
                    senderPipeline = gst_parse_launch(rtspPipeLineBuffer, &error);

                    break;
                }
            }
            break;
    }

    CHK_ERR(senderPipeline != NULL, STATUS_NULL_ARG, "[KVS Gstreamer Master] Pipeline is NULL");

    appsinkVideo = gst_bin_get_by_name(GST_BIN(senderPipeline), "appsink-video");
    appsinkAudio = gst_bin_get_by_name(GST_BIN(senderPipeline), "appsink-audio");

    if (!(appsinkVideo != NULL || appsinkAudio != NULL)) {
        DLOGE("[KVS GStreamer Master] sendGstreamerAudioVideo(): cant find appsink, operation returned status code: 0x%08x", STATUS_INTERNAL_ERROR);
        goto CleanUp;
    }

    if (appsinkVideo != NULL) {
        g_signal_connect(appsinkVideo, "new-sample", G_CALLBACK(on_new_sample_video), (gpointer) pSampleConfiguration);
    }
    if (appsinkAudio != NULL) {
        g_signal_connect(appsinkAudio, "new-sample", G_CALLBACK(on_new_sample_audio), (gpointer) pSampleConfiguration);
    }
    gst_element_set_state(senderPipeline, GST_STATE_PLAYING);

    /* block until error or EOS */
    bus = gst_element_get_bus(senderPipeline);
    msg = gst_bus_timed_pop_filtered(bus, GST_CLOCK_TIME_NONE, GST_MESSAGE_ERROR | GST_MESSAGE_EOS);

    /* Free resources */
    if (msg != NULL) {
        gst_message_unref(msg);
    }
    if (bus != NULL) {
        gst_object_unref(bus);
    }
    if (senderPipeline != NULL) {
        gst_element_set_state(senderPipeline, GST_STATE_NULL);
        gst_object_unref(senderPipeline);
    }
    if (appsinkAudio != NULL) {
        gst_object_unref(appsinkAudio);
    }
    if (appsinkVideo != NULL) {
        gst_object_unref(appsinkVideo);
    }

CleanUp:

    if (error != NULL) {
        DLOGE("[KVS GStreamer Master] %s", error->message);
        g_clear_error(&error);
    }

    return (PVOID) (ULONG_PTR) retStatus;
}

/**
 * Enumerate available video capture devices using GstDeviceMonitor and
 * let the user select one interactively. If requestedIndex >= 0, that
 * device is selected automatically without prompting.
 *
 * Returns a GstDevice* with an extra ref (caller must gst_object_unref), or NULL.
 */
/**
 * Check whether a GstDevice looks like a real, usable USB/external camera.
 * Filters out:
 *  - Raspberry Pi ISP backend nodes ("pispbe")
 *  - HEVC decoder nodes ("rpi-hevc-dec")
 *  - V4L2 metadata nodes (device path ending in odd number for paired USB cameras)
 */
static gboolean isRealCameraDevice(GstDevice* device)
{
    gchar* name = gst_device_get_display_name(device);
    gboolean dominated = FALSE;

    if (name == NULL) return FALSE;

    // Skip Pi ISP backend and decoder nodes
    if (g_strcmp0(name, "pispbe") == 0 ||
        g_str_has_prefix(name, "rpi-hevc") ||
        g_str_has_prefix(name, "bcm2835")) {
        dominated = TRUE;
    }

    g_free(name);
    if (dominated) return FALSE;

    // Also check via GstStructure properties if available
    GstStructure* props = gst_device_get_properties(device);
    if (props != NULL) {
        const gchar* driver = gst_structure_get_string(props, "v4l2.device.driver");
        if (driver != NULL) {
            // Skip Pi-internal drivers
            if (g_strcmp0(driver, "pispbe") == 0 ||
                g_strcmp0(driver, "rpivid") == 0 ||
                g_strcmp0(driver, "bcm2835-codec") == 0) {
                gst_structure_free(props);
                return FALSE;
            }
        }

        // Skip V4L2 metadata-only nodes (device_caps with META_CAPTURE flag)
        const gchar* devCaps = gst_structure_get_string(props, "v4l2.device.device_caps");
        // Metadata-only nodes have a very different capabilities set; for USB cameras
        // the driver is "uvcvideo" and the card has the camera name.
        // A simple heuristic: if the device path is /dev/videoN and the next device
        // in the pair is the metadata node. We rely on the caps check below instead.

        gst_structure_free(props);
    }

    // Check that the device advertises at least one caps with a real resolution
    // (metadata nodes and pispbe tend to advertise width=[0,0] or no video caps)
    GstCaps* caps = gst_device_get_caps(device);
    if (caps != NULL) {
        gboolean hasRealCaps = FALSE;
        guint nStructs = gst_caps_get_size(caps);
        for (guint i = 0; i < nStructs; i++) {
            GstStructure* s = gst_caps_get_structure(caps, i);
            gint width = 0;
            if (gst_structure_get_int(s, "width", &width) && width > 0) {
                hasRealCaps = TRUE;
                break;
            }
        }
        gst_caps_unref(caps);
        if (!hasRealCaps) return FALSE;
    }

    return TRUE;
}

static GstDevice* selectVideoDevice(INT32 requestedIndex)
{
    GstDeviceMonitor* monitor = NULL;
    GList *devices = NULL, *iter = NULL;
    GstDevice* selectedDevice = NULL;
    gint count = 0;
    INT32 selection = -1;
    CHAR inputBuffer[16];

    // Filtered list of real cameras (pointers into `devices`, do NOT free individually)
    GList* cameras = NULL;

    monitor = gst_device_monitor_new();
    gst_device_monitor_add_filter(monitor, "Video/Source", NULL);

    if (!gst_device_monitor_start(monitor)) {
        DLOGE("[KVS GStreamer Master] Failed to start device monitor");
        gst_object_unref(monitor);
        return NULL;
    }

    devices = gst_device_monitor_get_devices(monitor);

    if (devices == NULL) {
        DLOGI("[KVS GStreamer Master] No video capture devices found");
        gst_device_monitor_stop(monitor);
        gst_object_unref(monitor);
        return NULL;
    }

    // Filter to only real cameras (skip Pi ISP, metadata nodes, etc.)
    for (iter = devices; iter != NULL; iter = g_list_next(iter)) {
        GstDevice* device = GST_DEVICE(iter->data);
        if (isRealCameraDevice(device)) {
            cameras = g_list_append(cameras, device);
        }
    }

    if (cameras == NULL) {
        DLOGI("[KVS GStreamer Master] No usable camera devices found (all filtered out)");
        g_list_free_full(devices, (GDestroyNotify) gst_object_unref);
        gst_device_monitor_stop(monitor);
        gst_object_unref(monitor);
        return NULL;
    }

    // Print available cameras (filtered)
    printf("\n=== Available Video Devices ===\n");
    for (iter = cameras; iter != NULL; iter = g_list_next(iter)) {
        GstDevice* device = GST_DEVICE(iter->data);
        gchar* name = gst_device_get_display_name(device);
        printf("  [%d] %s\n", count, name);
        g_free(name);
        count++;
    }
    printf("===============================\n\n");

    if (requestedIndex >= 0) {
        // Use the provided device index directly
        if (requestedIndex < count) {
            selection = requestedIndex;
        } else {
            DLOGE("[KVS GStreamer Master] Device index %d out of range (0-%d)", requestedIndex, count - 1);
        }
    } else {
        // Prompt user for selection
        printf("Select a video device [0-%d]: ", count - 1);
        fflush(stdout);
        if (fgets(inputBuffer, sizeof(inputBuffer), stdin) != NULL) {
            CHAR* endPtr = NULL;
            long val = strtol(inputBuffer, &endPtr, 10);
            if (endPtr != inputBuffer && val >= 0 && val < count) {
                selection = (INT32) val;
            } else {
                DLOGE("[KVS GStreamer Master] Invalid selection");
            }
        }
    }

    if (selection >= 0) {
        selectedDevice = GST_DEVICE(g_list_nth_data(cameras, (guint) selection));
        if (selectedDevice != NULL) {
            gchar* name = gst_device_get_display_name(selectedDevice);
            DLOGI("[KVS GStreamer Master] Selected video device [%d]: %s", selection, name);
            printf("Using video device [%d]: %s\n\n", selection, name);
            g_free(name);
            gst_object_ref(selectedDevice); // Extra ref so it survives g_list_free_full
        }
    }

    g_list_free(cameras); // shallow free -- actual GstDevice objects owned by `devices`
    g_list_free_full(devices, (GDestroyNotify) gst_object_unref);
    gst_device_monitor_stop(monitor);
    gst_object_unref(monitor);

    return selectedDevice;
}

INT32 main(INT32 argc, CHAR* argv[])
{
    STATUS retStatus = STATUS_SUCCESS;
    PSampleConfiguration pSampleConfiguration = NULL;
    PCHAR pChannelName;
    RTC_CODEC audioCodec = RTC_CODEC_OPUS;
    RTC_CODEC videoCodec = RTC_CODEC_H264_PROFILE_42E01F_LEVEL_ASYMMETRY_ALLOWED_PACKETIZATION_MODE;
    INT32 requestedDeviceIndex = -1; // -1 means prompt; >= 0 selects that device directly

    SET_INSTRUMENTED_ALLOCATORS();
    UINT32 logLevel = setLogLevel();

    // Disable stdio buffering so output is visible immediately when not on a TTY
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);

    signal(SIGINT, sigintHandler);

#ifdef IOT_CORE_ENABLE_CREDENTIALS
    CHK_ERR((pChannelName = argc > 1 ? argv[1] : GETENV(IOT_CORE_THING_NAME)) != NULL, STATUS_INVALID_OPERATION,
            "AWS_IOT_CORE_THING_NAME must be set");
#else
    pChannelName = argc > 1 ? argv[1] : SAMPLE_CHANNEL_NAME;
#endif

    CHK_STATUS(createSampleConfiguration(pChannelName, SIGNALING_CHANNEL_ROLE_TYPE_MASTER, TRUE, TRUE, logLevel, &pSampleConfiguration));

    if (GETENV(DEFAULT_REGION_ENV_VAR) == NULL) {
        pSampleConfiguration->channelInfo.pRegion = SAMPLE_DEFAULT_REGION;
        DLOGI("[KVS GStreamer Master] Defaulting region to %s", pSampleConfiguration->channelInfo.pRegion);
    }

    if (argc > 3 && STRCMP(argv[3], "testsrc") == 0) {
        if (argc > 4) {
            if (!STRCMP(argv[4], AUDIO_CODEC_NAME_OPUS)) {
                audioCodec = RTC_CODEC_OPUS;
            }
        }

        if (argc > 5) {
            if (!STRCMP(argv[5], VIDEO_CODEC_NAME_H265)) {
                videoCodec = RTC_CODEC_H265;
            }
        }
    }

    pSampleConfiguration->videoSource = sendGstreamerAudioVideo;
    pSampleConfiguration->mediaType = SAMPLE_STREAMING_VIDEO_ONLY;
    pSampleConfiguration->audioCodec = audioCodec;
    pSampleConfiguration->videoCodec = videoCodec;

#ifdef ENABLE_DATA_CHANNEL
    pSampleConfiguration->onDataChannel = onDataChannel;
#endif
    pSampleConfiguration->customData = (UINT64) pSampleConfiguration;
    pSampleConfiguration->srcType = DEVICE_SOURCE; // Default to device source (autovideosrc and autoaudiosrc)
    /* Initialize GStreamer */
    gst_init(&argc, &argv);
    DLOGI("[KVS Gstreamer Master] Finished initializing GStreamer and handlers");

    if (argc > 2) {
        if (STRCMP(argv[2], "video-only") == 0) {
            pSampleConfiguration->mediaType = SAMPLE_STREAMING_VIDEO_ONLY;
            DLOGI("[KVS Gstreamer Master] Streaming video only");
        } else if (STRCMP(argv[2], "audio-video-storage") == 0) {
            pSampleConfiguration->mediaType = SAMPLE_STREAMING_AUDIO_VIDEO;
            pSampleConfiguration->channelInfo.useMediaStorage = TRUE;
            DLOGI("[KVS Gstreamer Master] Streaming audio and video");
        } else if (STRCMP(argv[2], "audio-video") == 0) {
            pSampleConfiguration->mediaType = SAMPLE_STREAMING_AUDIO_VIDEO;
            DLOGI("[KVS Gstreamer Master] Streaming audio and video");
        } else {
            DLOGI("[KVS Gstreamer Master] Unrecognized streaming type. Default to video-only");
        }
    } else {
        DLOGI("[KVS Gstreamer Master] Streaming video only");
    }

    if (argc > 3) {
        if (STRCMP(argv[3], "testsrc") == 0) {
            DLOGI("[KVS GStreamer Master] Using test source in GStreamer");
            pSampleConfiguration->srcType = TEST_SOURCE;
        } else if (STRCMP(argv[3], "devicesrc") == 0) {
            DLOGI("[KVS GStreamer Master] Using device source in GStreamer");
            pSampleConfiguration->srcType = DEVICE_SOURCE;
            if (argc > 4) {
                requestedDeviceIndex = (INT32) strtol(argv[4], NULL, 10);
                DLOGI("[KVS GStreamer Master] Requested device index: %d", requestedDeviceIndex);
            }
        } else if (STRCMP(argv[3], "rtspsrc") == 0) {
            DLOGI("[KVS GStreamer Master] Using RTSP source in GStreamer");
            if (argc < 5) {
                DLOGI("[KVS GStreamer Master] No RTSP source URI included. Defaulting to device source");
                DLOGI("[KVS GStreamer Master] Usage: ./kvsWebrtcClientMasterGstSample <channel name> audio-video rtspsrc rtsp://<rtsp uri>"
                      "or ./kvsWebrtcClientMasterGstSample <channel name> video-only rtspsrc <rtsp://<rtsp uri>");
                pSampleConfiguration->srcType = DEVICE_SOURCE;
            } else {
                pSampleConfiguration->srcType = RTSP_SOURCE;
                pSampleConfiguration->rtspUri = argv[4];
            }
        } else {
            DLOGI("[KVS Gstreamer Master] Unrecognized source type. Defaulting to device source in GStreamer");
        }
    } else {
        DLOGI("[KVS GStreamer Master] Using device source in GStreamer");
    }

    BOOL sourceSpecified = argc > 3;
    BOOL hasRtspUriArg = argc > 4 && !IS_EMPTY_STRING(argv[4]);

    if (!sourceSpecified) {
        pSampleConfiguration->srcType = DEVICE_SOURCE;
        DLOGI("[KVS GStreamer Master] Defaulting to device source (USB camera)");
    } else if (pSampleConfiguration->srcType == RTSP_SOURCE && !hasRtspUriArg) {
        pSampleConfiguration->rtspUri = SAMPLE_RTSP_URI;
        DLOGI("[KVS GStreamer Master] Using default RTSP URI %s", pSampleConfiguration->rtspUri);
    }

    // If using device source, enumerate cameras and let the user select one
    if (pSampleConfiguration->srcType == DEVICE_SOURCE) {
        GstDevice* selectedDevice = selectVideoDevice(requestedDeviceIndex);
        pSampleConfiguration->pSelectedVideoDevice = (PVOID) selectedDevice;
        if (selectedDevice == NULL) {
            DLOGI("[KVS GStreamer Master] No specific device selected, will fall back to autovideosrc");
        }
    }

    switch (pSampleConfiguration->mediaType) {
        case SAMPLE_STREAMING_VIDEO_ONLY:
            DLOGI("[KVS GStreamer Master] streaming type video-only");
            break;
        case SAMPLE_STREAMING_AUDIO_VIDEO:
            DLOGI("[KVS GStreamer Master] streaming type audio-video");
            break;
    }

    // Initalize KVS WebRTC. This must be done before anything else, and must only be done once.
    CHK_STATUS(initKvsWebRtc());
    DLOGI("[KVS GStreamer Master] KVS WebRTC initialization completed successfully");

    CHK_STATUS(initSignaling(pSampleConfiguration, SAMPLE_MASTER_CLIENT_ID));
    DLOGI("[KVS GStreamer Master] Channel %s set up done ", pChannelName);

    // Checking for termination
    CHK_STATUS(sessionCleanupWait(pSampleConfiguration));
    DLOGI("[KVS GStreamer Master] Streaming session terminated");

CleanUp:

    if (retStatus != STATUS_SUCCESS) {
        DLOGE("[KVS GStreamer Master] Terminated with status code 0x%08x", retStatus);
    }

    DLOGI("[KVS GStreamer Master] Cleaning up....");

    if (pSampleConfiguration != NULL) {
        // Kick of the termination sequence
        ATOMIC_STORE_BOOL(&pSampleConfiguration->appTerminateFlag, TRUE);

        if (pSampleConfiguration->mediaSenderTid != INVALID_TID_VALUE) {
            THREAD_JOIN(pSampleConfiguration->mediaSenderTid, NULL);
        }

        if (pSampleConfiguration->enableFileLogging) {
            freeFileLogger();
        }
        retStatus = freeSignalingClient(&pSampleConfiguration->signalingClientHandle);
        if (retStatus != STATUS_SUCCESS) {
            DLOGE("[KVS GStreamer Master] freeSignalingClient(): operation returned status code: 0x%08x", retStatus);
        }

        // Clean up selected video device (GstDevice*)
        if (pSampleConfiguration->pSelectedVideoDevice != NULL) {
            gst_object_unref((GstDevice*) pSampleConfiguration->pSelectedVideoDevice);
            pSampleConfiguration->pSelectedVideoDevice = NULL;
        }

        retStatus = freeSampleConfiguration(&pSampleConfiguration);
        if (retStatus != STATUS_SUCCESS) {
            DLOGE("[KVS GStreamer Master] freeSampleConfiguration(): operation returned status code: 0x%08x", retStatus);
        }
    }
    DLOGI("[KVS Gstreamer Master] Cleanup done");

    RESET_INSTRUMENTED_ALLOCATORS();

    // https://www.gnu.org/software/libc/manual/html_node/Exit-Status.html
    // We can only return with 0 - 127. Some platforms treat exit code >= 128
    // to be a success code, which might give an unintended behaviour.
    // Some platforms also treat 1 or 0 differently, so it's better to use
    // EXIT_FAILURE and EXIT_SUCCESS macros for portability.
    return STATUS_FAILED(retStatus) ? EXIT_FAILURE : EXIT_SUCCESS;
}
