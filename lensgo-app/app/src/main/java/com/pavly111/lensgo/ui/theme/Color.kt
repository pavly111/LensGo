package com.pavly111.lensgo.ui.theme

import androidx.compose.ui.graphics.Brush
import androidx.compose.ui.graphics.Color

// Monochrome + red palette
val BgBlack = Color(0xFF000000)
val Surface1 = Color(0xFF0C0C0C)
val Surface2 = Color(0xFF141414)
val Surface3 = Color(0xFF1E1E1E)
val Stroke = Color(0xFF2A2A2A)
val StrokeStrong = Color(0xFF3A3A3A)

val TextPrimary = Color(0xFFF2F2F2)
val TextMuted = Color(0xFF8A8A8A)
val TextDim = Color(0xFF5A5A5A)

val AccentRed = Color(0xFFFF2D2D)
val AccentRedDark = Color(0xFFB00020)

val BackgroundGradient = Brush.verticalGradient(
    listOf(Color(0xFF000000), Color(0xFF0A0A0A), Color(0xFF000000))
)

val PanelGradient = Brush.verticalGradient(
    listOf(Surface2, BgBlack)
)

val AccentGradient = Brush.horizontalGradient(
    listOf(AccentRedDark, AccentRed, AccentRedDark)
)

val ActionGradient = Brush.horizontalGradient(
    listOf(AccentRed, AccentRedDark)
)

val ActionGradientStop = Brush.horizontalGradient(
    listOf(AccentRedDark, AccentRed)
)