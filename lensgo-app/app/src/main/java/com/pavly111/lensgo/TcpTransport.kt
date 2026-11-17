package com.pavly111.lensgo

import java.io.OutputStream
import java.net.InetSocketAddress
import java.net.ServerSocket
import java.net.Socket

class TcpTransport(
    private val port: Int,
    private val listener: TransportClientListener
) : VideoTransport {

    private val lock = Any()
    private var serverSocket: ServerSocket? = null
    private var client: Socket? = null
    private var clientOut: OutputStream? = null
    @Volatile private var running = false

    override fun start() {
        val server = ServerSocket()
        server.reuseAddress = true
        server.bind(InetSocketAddress(port))
        serverSocket = server
        running = true
        Thread({ acceptLoop(server) }, "TcpTransport-accept").start()
    }

    private fun acceptLoop(server: ServerSocket) {
        while (running) {
            try {
                val socket = server.accept()
                socket.tcpNoDelay = true
                try {
                    socket.sendBufferSize = 64 * 1024
                } catch (e: Exception) {
                }
                synchronized(lock) {
                    if (client != null) {
                        try {
                            client?.close()
                        } catch (e: Exception) {
                        }
                    }
                    client = socket
                    clientOut = socket.getOutputStream()
                }
                listener.onClientConnected()
            } catch (e: Exception) {
                if (server.isClosed || !running) break
            }
        }
    }

    override fun send(data: ByteArray) {
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

    override fun requestConfig(config: ByteArray) {
        synchronized(lock) {
            val out = clientOut ?: return
            try {
                out.write(config)
                out.flush()
            } catch (e: Exception) {
                dropClient()
            }
        }
    }

    private fun dropClient() {
        synchronized(lock) {
            try {
                client?.close()
            } catch (e: Exception) {
            }
            client = null
            clientOut = null
        }
        listener.onClientDisconnected()
    }

    override fun stop() {
        running = false
        try {
            serverSocket?.close()
        } catch (e: Exception) {
        }
        synchronized(lock) {
            try {
                client?.close()
            } catch (e: Exception) {
            }
            client = null
            clientOut = null
        }
    }

    override fun setOnClientChange(callback: (Boolean) -> Unit) {
    }
}