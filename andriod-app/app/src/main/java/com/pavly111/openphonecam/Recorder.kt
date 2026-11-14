package com.pavly111.openphonecam

import android.media.MediaCodec
import android.media.MediaCodecInfo
import android.media.MediaFormat
import android.media.MediaMuxer
import android.util.Log
import android.util.Size
import android.view.Surface
import java.io.File

class Recorder(
    private val file: File,
    private val size: Size,
    private val mime: String,
    private val bitrate: Int,
    private val fps: Int,
    private val rotation: Int
) {
    private var encoder: MediaCodec? = null
    private var muxer: MediaMuxer? = null
    private var drainThread: Thread? = null
    private var finished = false

    lateinit var inputSurface: Surface
        private set

    fun prepare() {
        val format = MediaFormat.createVideoFormat(mime, size.width, size.height)
        format.setInteger(
            MediaFormat.KEY_COLOR_FORMAT,
            MediaCodecInfo.CodecCapabilities.COLOR_FormatSurface
        )
        format.setInteger(MediaFormat.KEY_BIT_RATE, bitrate)
        format.setInteger(MediaFormat.KEY_FRAME_RATE, fps)
        format.setInteger(MediaFormat.KEY_I_FRAME_INTERVAL, 1)

        val codec = MediaCodec.createEncoderByType(mime)
        codec.configure(format, null, null, MediaCodec.CONFIGURE_FLAG_ENCODE)
        inputSurface = codec.createInputSurface()
        codec.start()
        encoder = codec

        file.parentFile?.mkdirs()
        if (file.exists()) file.delete()
        val m = MediaMuxer(file.absolutePath, MediaMuxer.OutputFormat.MUXER_OUTPUT_MPEG_4)
        m.setOrientationHint(rotation)
        muxer = m

        val thread = Thread { drain(codec, m) }
        drainThread = thread
        thread.start()
    }

    private fun drain(codec: MediaCodec, m: MediaMuxer) {
        val info = MediaCodec.BufferInfo()
        var track = -1
        var started = false
        try {
            while (true) {
                val index = codec.dequeueOutputBuffer(info, 10_000)
                if (index == MediaCodec.INFO_OUTPUT_FORMAT_CHANGED) {
                    track = m.addTrack(codec.outputFormat)
                    m.start()
                    started = true
                } else if (index >= 0) {
                    val buffer = codec.getOutputBuffer(index)
                    val isConfig = (info.flags and MediaCodec.BUFFER_FLAG_CODEC_CONFIG) != 0
                    if (buffer != null && info.size > 0 && started && !isConfig) {
                        buffer.position(info.offset)
                        buffer.limit(info.offset + info.size)
                        m.writeSampleData(track, buffer, info)
                    }
                    codec.releaseOutputBuffer(index, false)
                    if ((info.flags and MediaCodec.BUFFER_FLAG_END_OF_STREAM) != 0) break
                }
            }
        } catch (e: Exception) {
            Log.e("OpenPhoneCam", "Drain error: ${e.message}")
        }
    }

    fun finish() {
        if (finished) return
        finished = true
        try {
            encoder?.signalEndOfInputStream()
        } catch (e: Exception) {
            Log.e("OpenPhoneCam", "Signal EOS error: ${e.message}")
        }
        drainThread?.join(3000)
        try {
            encoder?.stop()
        } catch (e: Exception) {
            Log.e("OpenPhoneCam", "Encoder stop error: ${e.message}")
        }
        encoder?.release()
        try {
            muxer?.stop()
        } catch (e: Exception) {
            Log.e("OpenPhoneCam", "Muxer stop error: ${e.message}")
        }
        muxer?.release()
        inputSurface.release()
    }
}