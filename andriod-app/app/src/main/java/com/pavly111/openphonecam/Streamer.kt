package com.pavly111.openphonecam

import android.media.MediaCodec
import android.media.MediaCodecInfo
import android.media.MediaFormat
import android.os.Build
import android.os.Bundle
import android.util.Log
import android.util.Size
import android.view.Surface
import java.io.OutputStream
import java.net.InetSocketAddress
import java.net.ServerSocket
import java.net.Socket

class Streamer(
    private val size: Size,
    private val mime: String,
    private val bitrate: Int,
    private val fps: Int,
    private val port: Int,
    private val onClientChange: (Boolean) -> Unit
) {
    private val lock = Any()
    private var encoder: MediaCodec? = null
    private var serverSocket: ServerSocket? = null
    private var client: Socket? = null
    private var clientOut: OutputStream? = null
    private var config: ByteArray? = null
    private var drainThread: Thread? = null
    private var acceptThread: Thread? = null
    @Volatile private var running = false
    private var stopped = false

    lateinit var inputSurface: Surface
        private set

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

        val codec = MediaCodec.createEncoderByType(mime)
        Log.d("OpenPhoneCam", "Encoder in use: ${codec.name}")

        val encoderCaps = codec.codecInfo.getCapabilitiesForType(mime).encoderCapabilities
        if (encoderCaps != null &&
            encoderCaps.isBitrateModeSupported(MediaCodecInfo.EncoderCapabilities.BITRATE_MODE_CBR)
        ) {
            format.setInteger(
                MediaFormat.KEY_BITRATE_MODE,
                MediaCodecInfo.EncoderCapabilities.BITRATE_MODE_CBR
            )
            Log.d("OpenPhoneCam", "Bitrate mode: CBR")
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

        val server = ServerSocket()
        server.reuseAddress = true
        server.bind(InetSocketAddress(port))
        serverSocket = server

        running = true

        val drain = Thread { drainLoop(codec) }
        drainThread = drain
        drain.start()

        val accept = Thread { acceptLoop(server) }
        acceptThread = accept
        accept.start()
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
                        }
                        send(data)
                    }
                    codec.releaseOutputBuffer(index, false)
                    if ((info.flags and MediaCodec.BUFFER_FLAG_END_OF_STREAM) != 0) break
                }
            }
        } catch (e: Exception) {
            Log.e("OpenPhoneCam", "Drain error: ${e.message}")
        }
    }

    private fun acceptLoop(server: ServerSocket) {
        while (running) {
            try {
                val socket = server.accept()
                socket.tcpNoDelay = true
                try {
                    socket.sendBufferSize = 64 * 1024
                } catch (e: Exception) {
                    Log.e("OpenPhoneCam", "Send buffer error: ${e.message}")
                }
                var connected = false
                synchronized(lock) {
                    if (client != null) dropClient()
                    client = socket
                    val out = socket.getOutputStream()
                    clientOut = out
                    connected = true
                    val cfg = config
                    if (cfg != null) {
                        try {
                            out.write(cfg)
                            out.flush()
                        } catch (e: Exception) {
                            dropClient()
                            connected = false
                        }
                    }
                }
                requestSyncFrame()
                if (connected) onClientChange(true)
            } catch (e: Exception) {
                if (server.isClosed || !running) break
                Log.e("OpenPhoneCam", "Accept error: ${e.message}")
            }
        }
    }

    private fun requestSyncFrame() {
        try {
            val params = Bundle()
            params.putInt(MediaCodec.PARAMETER_KEY_REQUEST_SYNC_FRAME, 0)
            encoder?.setParameters(params)
        } catch (e: Exception) {
            Log.e("OpenPhoneCam", "Sync frame error: ${e.message}")
        }
    }

    private fun send(data: ByteArray) {
        synchronized(lock) {
            val out = clientOut ?: return
            try {
                out.write(data)
                out.flush()
            } catch (e: Exception) {
                dropClient()
            }
        }
    }

    private fun dropClient() {
        try {
            client?.close()
        } catch (e: Exception) {
            Log.e("OpenPhoneCam", "Close client error: ${e.message}")
        }
        client = null
        clientOut = null
        onClientChange(false)
    }

    fun stop() {
        if (stopped) return
        stopped = true
        running = false
        try {
            encoder?.signalEndOfInputStream()
        } catch (e: Exception) {
            Log.e("OpenPhoneCam", "Signal EOS error: ${e.message}")
        }
        drainThread?.join(2000)
        try {
            serverSocket?.close()
        } catch (e: Exception) {
            Log.e("OpenPhoneCam", "Close server error: ${e.message}")
        }
        synchronized(lock) {
            try {
                client?.close()
            } catch (e: Exception) {
                Log.e("OpenPhoneCam", "Close client error: ${e.message}")
            }
            client = null
            clientOut = null
        }
        try {
            encoder?.stop()
        } catch (e: Exception) {
            Log.e("OpenPhoneCam", "Encoder stop error: ${e.message}")
        }
        encoder?.release()
        inputSurface.release()
    }
}