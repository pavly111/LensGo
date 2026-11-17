package com.pavly111.openphonecam.ui.theme

import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.darkColorScheme
import androidx.compose.runtime.Composable
import androidx.compose.ui.graphics.Color

private val FuturisticColorScheme = darkColorScheme(
    primary = NeonCyan,
    onPrimary = Color(0xFF04121A),
    secondary = NeonViolet,
    onSecondary = Color.White,
    tertiary = NeonMint,
    onTertiary = Color(0xFF04121A),
    background = BgDeep,
    onBackground = TextPrimary,
    surface = BgPanel,
    onSurface = TextPrimary,
    surfaceVariant = BgPanel,
    onSurfaceVariant = TextMuted,
    error = DangerRed,
    onError = Color.White
)

@Composable
fun OpenPhoneCamTheme(
    darkTheme: Boolean = true,
    content: @Composable () -> Unit
) {
    MaterialTheme(
        colorScheme = FuturisticColorScheme,
        typography = Typography,
        content = content
    )
}