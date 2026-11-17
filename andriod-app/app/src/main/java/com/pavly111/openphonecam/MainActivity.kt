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
import android.media.MediaCodec
import android.media.MediaCodecInfo
import android.media.MediaCodecList
import android.net.ConnectivityManager
import android.net.NetworkCapabilities
import android.os.Bundle
import android.os.Handler
import android.os.HandlerThread
import android.os.Looper
import android.util.Log
import android.util.Range
import android.util.Size
import android.view.Surface
import android.view.TextureView
import android.view.WindowManager
import androidx.activity.ComponentActivity
import androidx.activity.compose.setContent
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.clickable
import androidx.compose.foundation.horizontalScroll
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.aspectRatio
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Brush
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import androidx.compose.ui.viewinterop.AndroidView
import androidx.core.content.ContextCompat
import com.pavly111.openphonecam.ui.theme.AccentGradient
import com.pavly111.openphonecam.ui.theme.AccentRed
import com.pavly111.openphonecam.ui.theme.AccentRedDark
import com.pavly111.openphonecam.ui.theme.ActionGradient
import com.pavly111.openphonecam.ui.theme.ActionGradientStop
import com.pavly111.openphonecam.ui.theme.BackgroundGradient
import com.pavly111.openphonecam.ui.theme.BgBlack
import com.pavly111.openphonecam.ui.theme.OpenPhoneCamTheme
import com.pavly111.openphonecam.ui.theme.PanelGradient
import com.pavly111.openphonecam.ui.theme.Stroke
import com.pavly111.openphonecam.ui.theme.Surface2
import com.pavly111.openphonecam.ui.theme.Surface3
import com.pavly111.openphonecam.ui.theme.TextDim
import com.pavly111.openphonecam.ui.theme.TextMuted
import com.pavly111.openphonecam.ui.theme.TextPrimary
import java.net.Inet4Address
import java.net.NetworkInterface

data class ResolutionOption(
    val label: String,
    val size: Size,
    val h264: Boolean,
    val hevc: Boolean
)

data class QualityOption(
    val label: String,
    val size: Size,
    val h264: Boolean,
    val hevc: Boolean
) {
    val codec: String
        get() = when {
            h264 && hevc -> "H.264 / HEVC"
            h264 -> "H.264"
            hevc -> "HEVC"
            else -> "—"
        }
}

private val TIER_ORDER = listOf("480p", "720p", "1080p", "2K", "4K", "8K")

class MainActivity : ComponentActivity() {

    private val streamPort = 5000

    private lateinit var cameraManager: CameraManager
    private lateinit var cameraId: String
    private var cameraCharacteristics: CameraCharacteristics? = null
    private var previewSize by mutableStateOf(Size(1920, 1080))
    private var options: List<ResolutionOption> = emptyList()
    private var qualityOptions: List<QualityOption> = emptyList()
    private var streamFps by mutableStateOf(30)
    private var fpsOptions by mutableStateOf(listOf(30))
    private var maxFps by mutableStateOf(30)

    private var cameraDevice: CameraDevice? = null
    private var captureSession: CameraCaptureSession? = null
    private var cameraThread: HandlerThread? = null
    private var cameraHandler: Handler? = null
    private var textureView: TextureView? = null
    private var opening = false
    private var permissionGranted by mutableStateOf(false)

    private var streamer: Streamer? = null
    private var streaming by mutableStateOf(false)
    private var streamInfo by mutableStateOf("")
    private var clientConnected by mutableStateOf(false)
    private var status by mutableStateOf("")
    private var deviceIp by mutableStateOf<String?>(null)

    private val permissionLauncher =
        registerForActivityResult(ActivityResultContracts.RequestPermission()) { granted ->
            permissionGranted = granted
        }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        requestedOrientation = ActivityInfo.SCREEN_ORIENTATION_PORTRAIT
        window.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
        cameraManager = getSystemService(Context.CAMERA_SERVICE) as CameraManager
        pickCamera()

        permissionGranted = ContextCompat.checkSelfPermission(
            this, Manifest.permission.CAMERA
        ) == PackageManager.PERMISSION_GRANTED
        if (!permissionGranted) {
            permissionLauncher.launch(Manifest.permission.CAMERA)
        }

        setContent {
            OpenPhoneCamTheme {
                Box(
                    modifier = Modifier
                        .fillMaxSize()
                        .background(BackgroundGradient)
                ) {
                    if (!permissionGranted) {
                        PermissionNeeded()
                        return@Box
                    }
                    Column(modifier = Modifier.fillMaxSize()) {
                        PreviewArea(modifier = Modifier.weight(1f))
                        ControlPanel()
                    }
                }
            }
        }
    }

    @Composable
    private fun PreviewArea(modifier: Modifier = Modifier) {
        Box(
            modifier = modifier.fillMaxWidth(),
            contentAlignment = Alignment.Center
        ) {
            Box(
                modifier = Modifier
                    .aspectRatio(previewSize.height.toFloat() / previewSize.width.toFloat())
                    .clip(RoundedCornerShape(24.dp))
                    .background(BgBlack)
                    .border(1.dp, Stroke, RoundedCornerShape(24.dp))
            ) {
                AndroidView(
                    modifier = Modifier.fillMaxSize(),
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

                if (streaming) {
                    StreamNotch(modifier = Modifier.align(Alignment.TopCenter))
                } else {
                    PreviewTopBar(modifier = Modifier.align(Alignment.TopCenter))
                }
            }
        }
    }

    @Composable
    private fun StreamNotch(modifier: Modifier = Modifier) {
        val quality = qualityOptions.firstOrNull { it.size == previewSize }
        Row(
            modifier = modifier
                .padding(top = 12.dp)
                .clip(RoundedCornerShape(50))
                .background(Color(0xE6000000))
                .border(1.dp, AccentRed.copy(alpha = 0.8f), RoundedCornerShape(50))
                .padding(horizontal = 14.dp, vertical = 7.dp),
            verticalAlignment = Alignment.CenterVertically
        ) {
            Box(
                modifier = Modifier
                    .size(7.dp)
                    .clip(CircleShape)
                    .background(AccentRed)
            )
            Spacer(Modifier.width(7.dp))
            Text(
                text = quality?.label ?: "${previewSize.width}x${previewSize.height}",
                color = TextPrimary,
                fontSize = 13.sp,
                fontWeight = FontWeight.Bold
            )
            Spacer(Modifier.width(8.dp))
            Text(text = "·", color = TextDim, fontSize = 13.sp)
            Spacer(Modifier.width(8.dp))
            Text(
                text = "${effectiveFps()} fps",
                color = TextMuted,
                fontSize = 13.sp
            )
        }
    }

    @Composable
    private fun PreviewTopBar(modifier: Modifier = Modifier) {
        Row(
            modifier = modifier
                .fillMaxWidth()
                .background(
                    Brush.verticalGradient(
                        listOf(Color(0xCC000000), Color.Transparent)
                    )
                )
                .padding(horizontal = 16.dp, vertical = 14.dp),
            verticalAlignment = Alignment.CenterVertically
        ) {
            Column(modifier = Modifier.weight(1f)) {
                Text(
                    text = "OpenPhoneCam",
                    color = Color.White,
                    fontWeight = FontWeight.Black,
                    fontSize = 20.sp,
                    letterSpacing = 0.5.sp
                )
                Text(
                    text = "USB / WI-FI LINK",
                    color = TextMuted,
                    fontSize = 9.sp,
                    fontWeight = FontWeight.Bold,
                    letterSpacing = 3.sp
                )
            }
            StatusPill()
        }
    }

    @Composable
    private fun StatusPill() {
        val label: String
        val color: Color
        when {
            streaming && clientConnected -> {
                label = "LIVE"; color = AccentRed
            }
            streaming -> {
                label = "WAITING"; color = AccentRed
            }
            else -> {
                label = "IDLE"; color = TextMuted
            }
        }
        Row(
            modifier = Modifier
                .clip(RoundedCornerShape(50))
                .background(Color(0x99000000))
                .border(1.dp, color, RoundedCornerShape(50))
                .padding(horizontal = 12.dp, vertical = 6.dp),
            verticalAlignment = Alignment.CenterVertically
        ) {
            Box(
                modifier = Modifier
                    .size(7.dp)
                    .clip(CircleShape)
                    .background(color)
            )
            Spacer(Modifier.width(6.dp))
            Text(
                text = label,
                color = color,
                fontSize = 11.sp,
                fontWeight = FontWeight.Bold,
                letterSpacing = 1.5.sp
            )
        }
    }

    @Composable
    private fun ControlPanel() {
        Column(
            modifier = Modifier
                .fillMaxWidth()
                .background(PanelGradient)
                .padding(horizontal = 12.dp, vertical = 8.dp)
        ) {
            Box(
                modifier = Modifier
                    .fillMaxWidth()
                    .height(1.dp)
                    .background(AccentGradient)
            )
            Spacer(Modifier.height(6.dp))

            if (streaming) {
                Box(
                    modifier = Modifier.fillMaxWidth(),
                    contentAlignment = Alignment.Center
                ) {
                    ActionButton(
                        text = "STOP STREAMING",
                        modifier = Modifier.width(168.dp),
                        colors = ActionGradientStop,
                        onClick = { stopStreaming() }
                    )
                }
            } else {
                SectionLabel("QUALITY")
                Spacer(Modifier.height(4.dp))
                Row(
                    modifier = Modifier
                        .fillMaxWidth()
                        .horizontalScroll(rememberScrollState()),
                    horizontalArrangement = Arrangement.spacedBy(6.dp)
                ) {
                    qualityOptions.forEach { q ->
                        QualityCard(
                            quality = q,
                            selected = q.size == previewSize,
                            onClick = { changeResolution(q.size) }
                        )
                    }
                }

                Spacer(Modifier.height(8.dp))
                SectionLabel("FRAME RATE")
                Spacer(Modifier.height(4.dp))
                Row(
                    modifier = Modifier
                        .fillMaxWidth()
                        .horizontalScroll(rememberScrollState()),
                    horizontalArrangement = Arrangement.spacedBy(6.dp)
                ) {
                    fpsOptions.forEach { fps ->
                        FpsChip(
                            fps = fps,
                            selected = fps == streamFps,
                            onClick = { streamFps = fps }
                        )
                    }
                }

                Spacer(Modifier.height(8.dp))
                WifiCard()

                if (status.isNotEmpty()) {
                    Spacer(Modifier.height(6.dp))
                    Text(
                        text = status,
                        color = AccentRed,
                        fontSize = 11.sp
                    )
                }

                Spacer(Modifier.height(8.dp))
                Box(
                    modifier = Modifier.fillMaxWidth(),
                    contentAlignment = Alignment.Center
                ) {
                    ActionButton(
                        text = "START STREAMING",
                        modifier = Modifier.width(200.dp),
                        colors = ActionGradient,
                        onClick = { startStreaming() }
                    )
                }
            }
        }
    }

    @Composable
    private fun WifiCard() {
        Column(
            modifier = Modifier
                .fillMaxWidth()
                .clip(RoundedCornerShape(12.dp))
                .background(Surface2)
                .border(1.dp, Stroke, RoundedCornerShape(12.dp))
                .padding(horizontal = 12.dp, vertical = 6.dp)
        ) {
            Text(
                text = "WI-FI",
                color = TextMuted,
                fontSize = 9.sp,
                fontWeight = FontWeight.Bold,
                letterSpacing = 2.sp
            )
            Spacer(Modifier.height(2.dp))
            Row(verticalAlignment = Alignment.CenterVertically) {
                Box(
                    modifier = Modifier
                        .size(6.dp)
                        .clip(CircleShape)
                        .background(if (deviceIp != null) AccentRed else TextDim)
                )
                Spacer(Modifier.width(8.dp))
                Text(
                    text = deviceIp?.let { "$it:$streamPort" } ?: "not connected",
                    color = if (deviceIp != null) TextPrimary else TextDim,
                    fontSize = 15.sp,
                    fontWeight = FontWeight.Bold
                )
            }
        }
    }

    @Composable
    private fun SectionLabel(text: String) {
        Text(
            text = text,
            color = TextMuted,
            fontSize = 10.sp,
            fontWeight = FontWeight.Bold,
            letterSpacing = 2.sp
        )
    }

    @Composable
    private fun QualityCard(
        quality: QualityOption,
        selected: Boolean,
        onClick: () -> Unit
    ) {
        Column(
            modifier = Modifier
                .width(70.dp)
                .clip(RoundedCornerShape(14.dp))
                .background(if (selected) Color(0x1AFF2D2D) else Surface2)
                .border(
                    width = 1.dp,
                    color = if (selected) AccentRed else Stroke,
                    shape = RoundedCornerShape(14.dp)
                )
                .clickable(onClick = onClick)
                .padding(vertical = 7.dp, horizontal = 4.dp),
            horizontalAlignment = Alignment.CenterHorizontally
        ) {
            Text(
                text = quality.label,
                color = if (selected) AccentRed else TextPrimary,
                fontSize = 13.sp,
                fontWeight = FontWeight.Bold
            )
            Spacer(Modifier.height(1.dp))
            Text(
                text = "${quality.size.width}×${quality.size.height}",
                color = TextMuted,
                fontSize = 9.sp,
                textAlign = TextAlign.Center
            )
        }
    }

    @Composable
    private fun FpsChip(
        fps: Int,
        selected: Boolean,
        onClick: () -> Unit
    ) {
        Box(
            modifier = Modifier
                .clip(RoundedCornerShape(50))
                .background(if (selected) Color(0x1AFF2D2D) else Surface2)
                .border(
                    width = 1.dp,
                    color = if (selected) AccentRed else Stroke,
                    shape = RoundedCornerShape(50)
                )
                .clickable(onClick = onClick)
                .padding(horizontal = 12.dp, vertical = 6.dp),
            contentAlignment = Alignment.Center
        ) {
            Text(
                text = "$fps fps",
                color = if (selected) AccentRed else TextPrimary,
                fontSize = 12.sp,
                fontWeight = FontWeight.Medium
            )
        }
    }

    @Composable
    private fun ActionButton(
        text: String,
        modifier: Modifier = Modifier,
        colors: Brush,
        onClick: () -> Unit
    ) {
        Box(
            modifier = modifier
                .height(42.dp)
                .clip(RoundedCornerShape(14.dp))
                .background(colors)
                .clickable(onClick = onClick),
            contentAlignment = Alignment.Center
        ) {
            Text(
                text = text,
                color = Color.White,
                fontSize = 13.sp,
                fontWeight = FontWeight.Black,
                letterSpacing = 1.5.sp
            )
        }
    }

    @Composable
    private fun PermissionNeeded() {
        Column(
            modifier = Modifier
                .fillMaxSize()
                .padding(32.dp),
            horizontalAlignment = Alignment.CenterHorizontally,
            verticalArrangement = Arrangement.Center
        ) {
            Text(
                text = "CAMERA ACCESS",
                color = AccentRed,
                fontSize = 13.sp,
                fontWeight = FontWeight.Bold,
                letterSpacing = 3.sp
            )
            Spacer(Modifier.height(12.dp))
            Text(
                text = "OpenPhoneCam needs the camera to send video to OBS.",
                color = TextPrimary,
                fontSize = 15.sp,
                textAlign = TextAlign.Center
            )
        }
    }

    override fun onResume() {
        super.onResume()
        deviceIp = localIpv4()
        startThread()
        startCameraIfReady()
    }

    override fun onPause() {
        closeCamera()
        streamer?.stop()
        streamer = null
        streaming = false
        clientConnected = false
        stopThread()
        super.onPause()
    }

    private fun localIpv4(): String? {
        val cm = getSystemService(Context.CONNECTIVITY_SERVICE) as? ConnectivityManager
        if (cm != null) {
            val networks = try {
                cm.allNetworks.toList()
            } catch (e: Exception) {
                emptyList()
            }
            val wifi = networks.firstOrNull { net ->
                cm.getNetworkCapabilities(net)
                    ?.hasTransport(NetworkCapabilities.TRANSPORT_WIFI) == true
            } ?: cm.activeNetwork
            val props = wifi?.let { net ->
                try {
                    cm.getLinkProperties(net)
                } catch (e: Exception) {
                    null
                }
            }
            val address = props?.linkAddresses
                ?.map { it.address }
                ?.filterIsInstance<Inet4Address>()
                ?.firstOrNull { !it.isLoopbackAddress }
                ?.hostAddress
            if (address != null) return address
        }
        return try {
            NetworkInterface.getNetworkInterfaces().toList()
                .filter { it.isUp && !it.isLoopback }
                .sortedByDescending { it.name.startsWith("wlan", ignoreCase = true) }
                .flatMap { it.inetAddresses.toList() }
                .filterIsInstance<Inet4Address>()
                .firstOrNull { !it.isLoopbackAddress }
                ?.hostAddress
        } catch (e: Exception) {
            Log.e("OpenPhoneCam", "IP lookup error: ${e.message}")
            null
        }
    }

    private fun pickCamera() {
        val ids = cameraManager.cameraIdList
        cameraId = ids.firstOrNull { id ->
            cameraManager.getCameraCharacteristics(id)
                .get(CameraCharacteristics.LENS_FACING) == CameraCharacteristics.LENS_FACING_BACK
        } ?: ids.first()

        val characteristics = cameraManager.getCameraCharacteristics(cameraId)
        cameraCharacteristics = characteristics
        val map = characteristics.get(CameraCharacteristics.SCALER_STREAM_CONFIGURATION_MAP)!!
        val sizes = map.getOutputSizes(SurfaceTexture::class.java).toList()
        Log.d("OpenPhoneCam", "Camera sizes: " + sizes.joinToString { "${it.width}x${it.height}" })

        val allSizes = sizes
            .filter { it.width >= 640 && it.height >= 360 }
            .distinct()
            .sortedByDescending { it.width.toLong() * it.height }
        options = allSizes
            .map { size ->
                ResolutionOption(
                    qualityLabel(size),
                    size,
                    encoderSupports("video/avc", size),
                    encoderSupports("video/hevc", size)
                )
            }
            .filter { it.h264 || it.hevc }
        options.forEach {
            Log.d(
                "OpenPhoneCam",
                "${it.label} ${it.size.width}x${it.size.height}: H.264=${it.h264} HEVC=${it.hevc}"
            )
        }
        if (options.isEmpty()) {
            options = listOf(ResolutionOption("1080p", Size(1920, 1080), false, false))
        }

        qualityOptions = buildQualityOptions(options)
        qualityOptions.forEach {
            Log.d("OpenPhoneCam", "Quality tier ${it.label} -> ${it.size.width}x${it.size.height}")
        }

        previewSize = qualityOptions.firstOrNull { it.label == "1080p" }?.size
            ?: qualityOptions.firstOrNull { isWidescreen(it.size) }?.size
            ?: qualityOptions.first().size

        val ranges = characteristics
            .get(CameraCharacteristics.CONTROL_AE_AVAILABLE_TARGET_FPS_RANGES)
        val uppers = ranges
            ?.map { it.upper }
            ?.filter { it >= 24 }
            ?.distinct()
            ?.sorted()
            ?: emptyList()
        fpsOptions = if (uppers.isNotEmpty()) uppers else listOf(30)
        maxFps = fpsOptions.last()
        if (streamFps !in fpsOptions) {
            streamFps = fpsOptions.first()
        }
        Log.d("OpenPhoneCam", "FPS options: $fpsOptions")
    }

    private fun qualityLabel(size: Size): String {
        val w = maxOf(size.width, size.height)
        val h = minOf(size.width, size.height)
        val aspect = w.toFloat() / h.toFloat()
        return when {
            w >= 7000 || h >= 3200 -> "8K"
            w >= 3600 || h >= 1600 -> "4K"
            w >= 2000 && h in 1000..1599 && aspect <= 2.1f -> "2K"
            h >= 1250 -> "2K"
            h >= 950 -> "1080p"
            h >= 600 -> "720p"
            else -> "480p"
        }
    }

    private fun buildQualityOptions(all: List<ResolutionOption>): List<QualityOption> {
        val groups = LinkedHashMap<String, MutableList<ResolutionOption>>()
        all.forEach { opt -> groups.getOrPut(opt.label) { mutableListOf() }.add(opt) }

        val result = mutableListOf<QualityOption>()
        TIER_ORDER.forEach { tier ->
            val group = groups.remove(tier) ?: return@forEach
            val chosen = group.filter { isWidescreen(it.size) }
                .maxByOrNull { it.size.width.toLong() * it.size.height }
                ?: group.maxByOrNull { it.size.width.toLong() * it.size.height }!!
            result.add(QualityOption(tier, chosen.size, chosen.h264, chosen.hevc))
        }
        groups.values.flatten().forEach { opt ->
            result.add(QualityOption(opt.label, opt.size, opt.h264, opt.hevc))
        }
        return result
    }

    private fun isWidescreen(size: Size): Boolean {
        val w = maxOf(size.width, size.height).toFloat()
        val h = minOf(size.width, size.height).toFloat()
        val aspect = w / h
        return aspect in 1.7f..1.85f
    }

    private val encoderCache = mutableMapOf<String, List<MediaCodecInfo>>()

    private fun encodersFor(mime: String): List<MediaCodecInfo> {
        return encoderCache.getOrPut(mime) {
            MediaCodecList(MediaCodecList.REGULAR_CODECS).codecInfos.filter { info ->
                info.isEncoder &&
                        info.supportedTypes.any { it.equals(mime, ignoreCase = true) }
            }
        }
    }

    private fun encoderSupports(mime: String, size: Size): Boolean {
        return encodersFor(mime).any { info ->
            info.getCapabilitiesForType(mime).videoCapabilities
                ?.isSizeSupported(size.width, size.height) == true
        }
    }

    private fun chooseFpsRange(fps: Int): Range<Int>? {
        val ranges = cameraCharacteristics
            ?.get(CameraCharacteristics.CONTROL_AE_AVAILABLE_TARGET_FPS_RANGES)
            ?: return null
        if (ranges.isEmpty()) return null
        val max = ranges.maxOfOrNull { it.upper } ?: fps
        val target = minOf(fps, max)
        val exact = ranges.firstOrNull { it.lower == target && it.upper == target }
        if (exact != null) return exact
        val withUpper = ranges.filter { it.upper >= target }
        return if (withUpper.isNotEmpty()) {
            withUpper.maxByOrNull { it.lower }
        } else {
            ranges.maxByOrNull { it.upper }
        }
    }

    private fun effectiveFps(): Int {
        return minOf(streamFps, maxFps)
    }

    private fun changeResolution(size: Size) {
        if (size == previewSize) return
        closeCamera()
        previewSize = size
        status = ""
        Handler(Looper.getMainLooper()).postDelayed({ startCameraIfReady() }, 400)
    }

    private fun startStreaming() {
        deviceIp = localIpv4()
        val option = options.firstOrNull { it.size == previewSize }
        if (option == null) {
            status = "No supported option selected"
            return
        }
        if (!option.h264 && !option.hevc) {
            status = "This phone cannot encode ${option.label}"
            return
        }
        val useHevc = option.hevc && (!option.h264 || option.size.width >= 2560)
        val mime = if (useHevc) "video/hevc" else if (option.h264) "video/avc" else "video/hevc"
        val bitrate = when {
            option.size.width >= 7680 -> if (useHevc) 60_000_000 else 120_000_000
            option.size.width >= 3840 -> if (useHevc) 25_000_000 else 40_000_000
            option.size.width >= 2560 -> if (useHevc) 12_000_000 else 20_000_000
            option.size.width >= 1920 -> if (useHevc) 6_000_000 else 10_000_000
            else -> if (useHevc) 3_000_000 else 6_000_000
        }

        val fps = effectiveFps()
        Log.d(
            "OpenPhoneCam",
            "Stream: ${option.size.width}x${option.size.height} mime=$mime bitrate=$bitrate fps=$fps"
        )
        try {
            val newStreamer = Streamer(option.size, mime, bitrate, fps, streamPort) { connected ->
                runOnUiThread { clientConnected = connected }
            }
            newStreamer.start()
            streamer = newStreamer
        } catch (e: Exception) {
            Log.e("OpenPhoneCam", "Streamer error: ${e.message}")
            status = "Encoder or port error: ${e.message}"
            return
        }

        streamInfo = "${option.label} ${if (mime == "video/hevc") "HEVC" else "H.264"}"
        streaming = true
        status = ""
        closeCamera()
        Handler(Looper.getMainLooper()).postDelayed({ startCameraIfReady() }, 400)
    }

    private fun stopStreaming() {
        if (!streaming) return
        closeCamera()
        streamer?.stop()
        streamer = null
        streaming = false
        clientConnected = false
        status = "Streaming stopped"
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
        val activeStreamer = streamer
        val bufferSize = if (activeStreamer != null && previewSize.width > 1920) {
            Size(1280, 720)
        } else {
            previewSize
        }
        texture.setDefaultBufferSize(bufferSize.width, bufferSize.height)
        val previewSurface = Surface(texture)

        val template = if (activeStreamer != null) {
            CameraDevice.TEMPLATE_RECORD
        } else {
            CameraDevice.TEMPLATE_PREVIEW
        }
        val requestBuilder = camera.createCaptureRequest(template)
        val surfaces = mutableListOf(previewSurface)
        requestBuilder.addTarget(previewSurface)
        if (activeStreamer != null) {
            surfaces.add(activeStreamer.inputSurface)
            requestBuilder.addTarget(activeStreamer.inputSurface)
        }
        requestBuilder.set(CaptureRequest.CONTROL_MODE, CaptureRequest.CONTROL_MODE_AUTO)
        val fpsRange = chooseFpsRange(streamFps)
        if (fpsRange != null) {
            requestBuilder.set(CaptureRequest.CONTROL_AE_TARGET_FPS_RANGE, fpsRange)
            Log.d("OpenPhoneCam", "Target fps ${effectiveFps()}, range $fpsRange")
        }

        camera.createCaptureSession(
            surfaces,
            object : CameraCaptureSession.StateCallback() {
                override fun onConfigured(session: CameraCaptureSession) {
                    captureSession = session
                    session.setRepeatingRequest(requestBuilder.build(), null, cameraHandler)
                }

                override fun onConfigureFailed(session: CameraCaptureSession) {
                    Log.e("OpenPhoneCam", "Camera session configuration failed")
                    runOnUiThread { status = "Camera session failed for this setting" }
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