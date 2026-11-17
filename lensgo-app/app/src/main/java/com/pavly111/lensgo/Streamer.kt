package com.pavly111.lensgo

import android.media.MediaCodec
import android.media.MediaCodecInfo
import android.media.MediaFormat
import android.os.Build
import android.os.Bundle
import android.util.Log
import android.util.Size
import android.view.Surface

class Streamer(
    private val size: Size,
    private val mime: String,
    private val bitrate: Int,
    private val fps: Int,
    private val onClientChange: (Boolean) -> Unit
) {

    private val lock = Any()
    private var encoder: MediaCodec? = null
    private val transports = mutableListOf<VideoTransport>()
    private var config: ByteArray? = null
    private var drainThread: Thread? = null
    private var stopped = false

    lateinit var inputSurface: Surface
        private set

    val clientState = object : TransportClientListener {
        @Volatile var count = 0

        override fun onClientConnected() {
            synchronized(this) {
                count++
            }
            val cfg = synchronized(lock) { config }
            if (cfg != null) {
                transports.forEach { t ->
                    try {
                        t.requestConfig(cfg)
                    } catch (e: Exception) {
                        Log.e("LensGo", "RequestConfig error: ${e.message}")
                    }
                }
            }
            requestSyncFrame()
            onClientChange(true)
        }

        override fun onClientDisconnected() {
            synchronized(this) {
                if (count > 0) count--
            }
            onClientChange(false)
        }
    }

    fun addTransport(transport: VideoTransport) {
        transports.add(transport)
    }

    fun start() {
        val format = MediaFormat.createVideoFormat(mime, size.width, size.height)
        format.setInteger(
            MediaFormat.KEY_COLOR_FORMAT,
            MediaCodecInfo.CodecCapabilities.COLOR_FormatSurface
        )
        format.setInteger(MediaFormat.KEY_BIT_RATE, bitrate)
        format.setInteger(MediaFormat.KEY_FRAME_RATE, fps)
        format.setInteger(MediaFormat.KEY_I_FRAME_INTERVAL, 1)
        format.setInteger(MediaFormat.KEY_PRIORITY, 0)
        format.setInteger(MediaFormat.KEY_OPERATING_RATE, fps)
        format.setInteger("prepend-sps-pps-to-idr-frames", 1)
        if (mime == "video/hevc") {
            try {
                format.setInteger("prepend-vps-sps-pps-to-idr-frames", 1)
            } catch (e: Exception) {
                Log.e("LensGo", "HEVC prepend key error: ${e.message}")
            }
        }

        val codec = MediaCodec.createEncoderByType(mime)
        Log.d("LensGo", "Encoder in use: ${codec.name}")

        val encoderCaps = codec.codecInfo.getCapabilitiesForType(mime).encoderCapabilities
        if (encoderCaps != null &&
            encoderCaps.isBitrateModeSupported(MediaCodecInfo.EncoderCapabilities.BITRATE_MODE_CBR)
        ) {
            format.setInteger(
                MediaFormat.KEY_BITRATE_MODE,
                MediaCodecInfo.EncoderCapabilities.BITRATE_MODE_CBR
            )
            Log.d("LensGo", "Bitrate mode: CBR")
        }
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) {
            format.setInteger(MediaFormat.KEY_MAX_B_FRAMES, 0)
        }
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
            format.setInteger(MediaFormat.KEY_LATENCY, 1)
        }

        codec.configure(format, null, null, MediaCodec.CONFIGURE_FLAG_ENCODE)
        inputSurface = codec.createInputSurface()
        codec.start()
        encoder = codec

        val drain = Thread({ drainLoop(codec) }, "Streamer-drain")
        drainThread = drain
        drain.start()
    }

    private val hevcParamHeaders = setOf(0x40, 0x42, 0x44)

    private fun hasParamSets(data: ByteArray, headers: Set<Int>): Boolean {
        var i = 0
        while (i + 3 < data.size) {
            val b0 = data[i].toInt() and 0xFF
            val b1 = data[i + 1].toInt() and 0xFF
            val b2 = data[i + 2].toInt() and 0xFF
            if (b0 == 0 && b1 == 0 && b2 == 1) {
                if (headers.contains(data[i + 3].toInt() and 0xFF)) return true
                i += 3
            } else if (i + 4 < data.size && b0 == 0 && b1 == 0 && b2 == 0 &&
                (data[i + 3].toInt() and 0xFF) == 1
            ) {
                if (headers.contains(data[i + 4].toInt() and 0xFF)) return true
                i += 4
            } else {
                i++
            }
        }
        return false
    }

    private fun drainLoop(codec: MediaCodec) {
        val info = MediaCodec.BufferInfo()
        try {
            while (true) {
                val index = codec.dequeueOutputBuffer(info, 10_000)
                if (index >= 0) {
                    val buffer = codec.getOutputBuffer(index)
                    if (buffer != null && info.size > 0) {
                        val data = ByteArray(info.size)
                        buffer.position(info.offset)
                        buffer.limit(info.offset + info.size)
                        buffer.get(data)
                        val isConfig = (info.flags and MediaCodec.BUFFER_FLAG_CODEC_CONFIG) != 0
                        if (isConfig) {
                            synchronized(lock) { config = data }
                        } else if (mime == "video/hevc") {
                            synchronized(lock) {
                                if (config == null && hasParamSets(data, hevcParamHeaders)) {
                                    config = data
                                }
                            }
                        }
                        sendToTransports(data)
                    }
                    codec.releaseOutputBuffer(index, false)
                    if ((info.flags and MediaCodec.BUFFER_FLAG_END_OF_STREAM) != 0) break
                }
            }
        } catch (e: Exception) {
            Log.e("LensGo", "Drain error: ${e.message}")
        }
    }

    private fun sendToTransports(data: ByteArray) {
        val current = transports.toList()
        for (t in current) {
            try {
                t.send(data)
            } catch (e: Exception) {
                Log.e("LensGo", "Transport send error: ${e.message}")
            }
        }
    }

    private fun requestSyncFrame() {
        try {
            val params = Bundle()
            params.putInt(MediaCodec.PARAMETER_KEY_REQUEST_SYNC_FRAME, 0)
            encoder?.setParameters(params)
        } catch (e: Exception) {
            Log.e("LensGo", "Sync frame error: ${e.message}")
        }
    }

    fun stop() {
        if (stopped) return
        stopped = true
        try {
            encoder?.signalEndOfInputStream()
        } catch (e: Exception) {
            Log.e("LensGo", "Signal EOS error: ${e.message}")
        }
        drainThread?.join(2000)
        transports.forEach { t ->
            try {
                t.stop()
            } catch (e: Exception) {
                Log.e("LensGo", "Transport stop error: ${e.message}")
            }
        }
        try {
            encoder?.stop()
        } catch (e: Exception) {
            Log.e("LensGo", "Encoder stop error: ${e.message}")
        }
        encoder?.release()
        inputSurface.release()
    }
}