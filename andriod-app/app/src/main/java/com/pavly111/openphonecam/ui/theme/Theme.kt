package com.pavly111.openphonecam.ui.theme

import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.darkColorScheme
import androidx.compose.runtime.Composable
import androidx.compose.ui.graphics.Color

private val MonochromeColorScheme = darkColorScheme(
    primary = AccentRed,
    onPrimary = Color.White,
    secondary = AccentRedDark,
    onSecondary = Color.White,
    tertiary = AccentRed,
    onTertiary = Color.White,
    background = BgBlack,
    onBackground = TextPrimary,
    surface = Surface1,
    onSurface = TextPrimary,
    surfaceVariant = Surface2,
    onSurfaceVariant = TextMuted,
    error = AccentRed,
    onError = Color.White
)

@Composable
fun OpenPhoneCamTheme(
    darkTheme: Boolean = true,
    content: @Composable () -> Unit
) {
    MaterialTheme(
        colorScheme = MonochromeColorScheme,
        typography = Typography,
        content = content
    )
}