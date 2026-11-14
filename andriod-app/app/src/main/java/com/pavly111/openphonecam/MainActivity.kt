package com.pavly111.openphonecam

import android.Manifest
import android.annotation.SuppressLint
import android.content.Context
import android.content.pm.ActivityInfo
import android.content.pm.PackageManager
import android.graphics.SurfaceTexture
import android.hardware.camera2.CameraCaptureSession
import android.hardware.camera2.CameraCharacteristics
import android.hardware.camera2.CameraDevice
import android.hardware.camera2.CameraManager
import android.hardware.camera2.CaptureRequest
import android.media.MediaCodecList
import android.os.Bundle
import android.os.Handler
import android.os.HandlerThread
import android.os.Looper
import android.util.Log
import android.util.Size
import android.view.Surface
import android.view.TextureView
import androidx.activity.ComponentActivity
import androidx.activity.compose.setContent
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.aspectRatio
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.material3.Button
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.unit.dp
import androidx.compose.ui.viewinterop.AndroidView
import androidx.core.content.ContextCompat

data class ResolutionOption(
    val label: String,
    val size: Size,
    val h264: Boolean,
    val hevc: Boolean
)

class MainActivity : ComponentActivity() {

    private lateinit var cameraManager: CameraManager
    private lateinit var cameraId: String
    private var previewSize by mutableStateOf(Size(1920, 1080))
    private var options: List<ResolutionOption> = emptyList()

    private var cameraDevice: CameraDevice? = null
    private var captureSession: CameraCaptureSession? = null
    private var cameraThread: HandlerThread? = null
    private var cameraHandler: Handler? = null
    private var textureView: TextureView? = null
    private var opening = false
    private var permissionGranted by mutableStateOf(false)

    private val permissionLauncher =
        registerForActivityResult(ActivityResultContracts.RequestPermission()) { granted ->
            permissionGranted = granted
        }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        requestedOrientation = ActivityInfo.SCREEN_ORIENTATION_PORTRAIT
        cameraManager = getSystemService(Context.CAMERA_SERVICE) as CameraManager
        pickCamera()

        permissionGranted = ContextCompat.checkSelfPermission(
            this, Manifest.permission.CAMERA
        ) == PackageManager.PERMISSION_GRANTED
        if (!permissionGranted) {
            permissionLauncher.launch(Manifest.permission.CAMERA)
        }

        setContent {
            MaterialTheme {
                Box(modifier = Modifier.fillMaxSize()) {
                    if (permissionGranted) {
                        AndroidView(
                            modifier = Modifier
                                .align(Alignment.Center)
                                .aspectRatio(previewSize.height.toFloat() / previewSize.width.toFloat()),
                            factory = { ctx ->
                                TextureView(ctx).also { view ->
                                    textureView = view
                                    view.surfaceTextureListener = object : TextureView.SurfaceTextureListener {
                                        override fun onSurfaceTextureAvailable(
                                            surface: SurfaceTexture, width: Int, height: Int
                                        ) {
                                            startCameraIfReady()
                                        }

                                        override fun onSurfaceTextureSizeChanged(
                                            surface: SurfaceTexture, width: Int, height: Int
                                        ) {
                                        }

                                        override fun onSurfaceTextureDestroyed(surface: SurfaceTexture): Boolean {
                                            return true
                                        }

                                        override fun onSurfaceTextureUpdated(surface: SurfaceTexture) {
                                        }
                                    }
                                }
                            }
                        )
                        Column(
                            modifier = Modifier
                                .align(Alignment.BottomCenter)
                                .fillMaxWidth()
                                .background(Color(0x99000000))
                                .padding(12.dp)
                        ) {
                            val current = options.firstOrNull { it.size == previewSize }
                            Text(
                                text = "Preview ${previewSize.width}x${previewSize.height}",
                                color = Color.White
                            )
                            if (current != null) {
                                Text(
                                    text = "H.264: ${if (current.h264) "yes" else "no"}   HEVC: ${if (current.hevc) "yes" else "no"}",
                                    color = Color.White
                                )
                            }
                            Row(
                                modifier = Modifier.padding(top = 8.dp),
                                horizontalArrangement = Arrangement.spacedBy(6.dp)
                            ) {
                                options.forEach { option ->
                                    Button(
                                        onClick = { changeResolution(option.size) },
                                        contentPadding = PaddingValues(horizontal = 10.dp)
                                    ) {
                                        Text(option.label)
                                    }
                                }
                            }
                        }
                    } else {
                        Text(
                            text = "Camera permission is needed",
                            modifier = Modifier.align(Alignment.Center)
                        )
                    }
                }
            }
        }
    }

    override fun onResume() {
        super.onResume()
        startThread()
        startCameraIfReady()
    }

    override fun onPause() {
        closeCamera()
        stopThread()
        super.onPause()
    }

    private fun pickCamera() {
        val ids = cameraManager.cameraIdList
        cameraId = ids.firstOrNull { id ->
            cameraManager.getCameraCharacteristics(id)
                .get(CameraCharacteristics.LENS_FACING) == CameraCharacteristics.LENS_FACING_BACK
        } ?: ids.first()

        val map = cameraManager.getCameraCharacteristics(cameraId)
            .get(CameraCharacteristics.SCALER_STREAM_CONFIGURATION_MAP)!!
        val sizes = map.getOutputSizes(SurfaceTexture::class.java).toList()
        Log.d("OpenPhoneCam", "Camera sizes: " + sizes.joinToString { "${it.width}x${it.height}" })

        val targets = listOf(
            "720p" to Size(1280, 720),
            "1080p" to Size(1920, 1080),
            "1440p" to Size(2560, 1440),
            "4K" to Size(3840, 2160),
            "8K" to Size(7680, 4320)
        )
        options = targets
            .filter { (_, size) -> sizes.contains(size) }
            .map { (label, size) ->
                ResolutionOption(
                    label,
                    size,
                    encoderSupports("video/avc", size),
                    encoderSupports("video/hevc", size)
                )
            }
        options.forEach {
            Log.d("OpenPhoneCam", "${it.label}: H.264=${it.h264} HEVC=${it.hevc}")
        }

        previewSize = options.firstOrNull { it.label == "1080p" }?.size
            ?: sizes.filter { it.width * 9 == it.height * 16 }.maxByOrNull { it.width }
                    ?: sizes.first()
    }

    private fun encoderSupports(mime: String, size: Size): Boolean {
        val infos = MediaCodecList(MediaCodecList.REGULAR_CODECS).codecInfos
        return infos.any { info ->
            info.isEncoder &&
                    info.supportedTypes.any { it.equals(mime, ignoreCase = true) } &&
                    info.getCapabilitiesForType(mime).videoCapabilities
                        ?.isSizeSupported(size.width, size.height) == true
        }
    }

    private fun changeResolution(size: Size) {
        if (size == previewSize) return
        closeCamera()
        previewSize = size
        Handler(Looper.getMainLooper()).postDelayed({ startCameraIfReady() }, 400)
    }

    private fun startThread() {
        if (cameraThread != null) return
        val thread = HandlerThread("CameraThread")
        thread.start()
        cameraThread = thread
        cameraHandler = Handler(thread.looper)
    }

    private fun stopThread() {
        cameraThread?.quitSafely()
        cameraThread = null
        cameraHandler = null
    }

    private fun startCameraIfReady() {
        if (!permissionGranted) return
        val view = textureView ?: return
        if (!view.isAvailable) return
        if (cameraDevice != null || opening) return
        startThread()
        openCamera(view)
    }

    @SuppressLint("MissingPermission")
    private fun openCamera(view: TextureView) {
        opening = true
        cameraManager.openCamera(cameraId, object : CameraDevice.StateCallback() {
            override fun onOpened(camera: CameraDevice) {
                opening = false
                cameraDevice = camera
                startPreview(camera, view)
            }

            override fun onDisconnected(camera: CameraDevice) {
                opening = false
                camera.close()
                cameraDevice = null
            }

            override fun onError(camera: CameraDevice, error: Int) {
                opening = false
                camera.close()
                cameraDevice = null
            }
        }, cameraHandler)
    }

    @Suppress("DEPRECATION")
    private fun startPreview(camera: CameraDevice, view: TextureView) {
        val texture = view.surfaceTexture ?: return
        texture.setDefaultBufferSize(previewSize.width, previewSize.height)
        val surface = Surface(texture)

        val requestBuilder = camera.createCaptureRequest(CameraDevice.TEMPLATE_PREVIEW)
        requestBuilder.addTarget(surface)
        requestBuilder.set(CaptureRequest.CONTROL_MODE, CaptureRequest.CONTROL_MODE_AUTO)

        camera.createCaptureSession(
            listOf(surface),
            object : CameraCaptureSession.StateCallback() {
                override fun onConfigured(session: CameraCaptureSession) {
                    captureSession = session
                    session.setRepeatingRequest(requestBuilder.build(), null, cameraHandler)
                }

                override fun onConfigureFailed(session: CameraCaptureSession) {
                }
            },
            cameraHandler
        )
    }

    private fun closeCamera() {
        opening = false
        captureSession?.close()
        captureSession = null
        cameraDevice?.close()
        cameraDevice = null
    }
}