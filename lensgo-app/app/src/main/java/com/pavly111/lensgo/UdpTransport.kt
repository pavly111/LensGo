package com.pavly111.lensgo

import android.util.Log
import java.net.DatagramPacket
import java.net.DatagramSocket
import java.net.InetAddress
import java.net.InetSocketAddress

interface TransportClientListener {
    fun onClientConnected()
    fun onClientDisconnected()
}

interface VideoTransport {
    fun send(data: ByteArray)
    fun requestConfig(config: ByteArray)
    fun start()
    fun stop()
    fun setOnClientChange(callback: (Boolean) -> Unit)
}

class UdpTransport(
    private val port: Int,
    private val listener: TransportClientListener
) : VideoTransport {

    companion object {
        private const val TAG = "LensGo/UdpTransport"
        private const val MAX_PAYLOAD = 1400
        private const val FEC_K = 8
        private const val FEC_BLOCK = FEC_K + 1
        private const val HEADER_SIZE = 12
        private const val MAGIC = 0x4F50434D.toInt()
    }

    private val socket: DatagramSocket
    @Volatile private var clientAddress: InetAddress? = null
    @Volatile private var clientPort: Int = 0
    @Volatile private var notified = false
    @Volatile private var running = false

    private var seq = 0
    private var blockBuf = ByteArray(MAX_PAYLOAD * FEC_K)
    private var blockCount = 0

    private val sendLock = Any()
    private val recvLock = Any()

    init {
        socket = DatagramSocket(null).also {
            it.reuseAddress = true
            it.bind(InetSocketAddress(port))
            it.soTimeout = 1000
        }
    }

    override fun start() {
        running = true
        Thread({ recvLoop() }, "UdpTransport-recv").start()
    }

    private fun recvLoop() {
        val buf = ByteArray(128)
        while (running) {
            try {
                val pkt = DatagramPacket(buf, buf.size)
                socket.receive(pkt)
                val addr = pkt.address
                val port = pkt.port
                var shouldNotify = false
                synchronized(recvLock) {
                    if (clientAddress == null || !addr.equals(clientAddress) || port != clientPort) {
                        clientAddress = addr
                        clientPort = port
                        if (!notified) {
                            notified = true
                            shouldNotify = true
                        }
                    }
                }
                if (shouldNotify) {
                    listener.onClientConnected()
                }
            } catch (e: java.net.SocketTimeoutException) {
                continue
            } catch (e: Exception) {
                if (!running) break
                Log.e(TAG, "Recv error: ${e.message}")
            }
        }
    }

    override fun send(data: ByteArray) {
        if (!hasClient()) return
        synchronized(sendLock) {
            var offset = 0
            while (offset < data.size) {
                val len = minOf(MAX_PAYLOAD, data.size - offset)
                val chunk = ByteArray(len)
                System.arraycopy(data, offset, chunk, 0, len)
                offset += len
                sendChunk(chunk, len, isFinal = false)
            }
        }
    }

    override fun requestConfig(config: ByteArray) {
        if (!hasClient()) return
        synchronized(sendLock) {
            var offset = 0
            while (offset < config.size) {
                val len = minOf(MAX_PAYLOAD, config.size - offset)
                val chunk = ByteArray(len)
                System.arraycopy(config, offset, chunk, 0, len)
                offset += len
                sendChunk(chunk, len, isFinal = false)
            }
        }
    }

    private fun hasClient(): Boolean {
        return clientAddress != null
    }

    private fun sendChunk(chunk: ByteArray, len: Int, isFinal: Boolean) {
        val pos = seq % FEC_BLOCK
        val isParity = (pos == FEC_K)

        if (isParity) {
            val parity = ByteArray(MAX_PAYLOAD)
            for (i in 0 until MAX_PAYLOAD) {
                var b = 0
                for (j in 0 until FEC_K) {
                    val idx = j * MAX_PAYLOAD + i
                    if (idx < blockCount * MAX_PAYLOAD) {
                        b = b xor (blockBuf[idx].toInt() and 0xFF)
                    }
                }
                parity[i] = b.toByte()
            }
            sendDatagram(seq, parity, MAX_PAYLOAD, false)
            blockCount = 0
        } else {
            val slotStart = blockCount * MAX_PAYLOAD
            for (i in 0 until MAX_PAYLOAD) {
                blockBuf[slotStart + i] = 0
            }
            System.arraycopy(chunk, 0, blockBuf, slotStart, len)
            blockCount++
            sendDatagram(seq, chunk, len, isFinal)
        }
        seq++
    }

    private fun sendDatagram(seqNum: Int, payload: ByteArray, len: Int, isFinal: Boolean) {
        val addr = clientAddress ?: return
        val port = clientPort
        val buf = ByteArray(HEADER_SIZE + MAX_PAYLOAD)
        var idx = 0
        buf[idx++] = (MAGIC shr 24).toByte()
        buf[idx++] = (MAGIC shr 16).toByte()
        buf[idx++] = (MAGIC shr 8).toByte()
        buf[idx++] = (MAGIC and 0xFF).toByte()
        buf[idx++] = (seqNum shr 24).toByte()
        buf[idx++] = (seqNum shr 16).toByte()
        buf[idx++] = (seqNum shr 8).toByte()
        buf[idx++] = (seqNum and 0xFF).toByte()
        var flags = 0
        if (isFinal) flags = flags or 0x01
        buf[idx++] = flags.toByte()
        buf[idx++] = (len shr 8).toByte()
        buf[idx++] = (len and 0xFF).toByte()
        buf[idx++] = 0
        System.arraycopy(payload, 0, buf, HEADER_SIZE, len)
        val packet = DatagramPacket(buf, HEADER_SIZE + len, addr, port)
        try {
            socket.send(packet)
        } catch (e: Exception) {
            Log.e(TAG, "Send error: ${e.message}")
            synchronized(recvLock) {
                clientAddress = null
                clientPort = 0
                notified = false
            }
            listener.onClientDisconnected()
        }
    }

    override fun stop() {
        running = false
        socket.close()
    }

    override fun setOnClientChange(callback: (Boolean) -> Unit) {
    }
}