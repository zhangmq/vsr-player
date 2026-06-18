"""Constants and adaptive scale calculation."""

from nvvfx import VideoSuperRes

QUALITY_MAP = {
    "LOW": VideoSuperRes.QualityLevel.LOW,
    "MEDIUM": VideoSuperRes.QualityLevel.MEDIUM,
    "HIGH": VideoSuperRes.QualityLevel.HIGH,
    "ULTRA": VideoSuperRes.QualityLevel.ULTRA,
}

MIN_SCALE = 1
MAX_SCALE = 4
DEBOUNCE_MS = 500  # delay before VSR reload on resize
DEFAULT_FPS = 30.0
DEFAULT_QUALITY = "HIGH"


def adaptive_scale(in_w: int, in_h: int, win_w: int, win_h: int) -> int:
    """Compute the best integer upscale factor to fill the window.

    Uses ceiling division so that 720p in a 1080p window yields ×2
    (VSR oversample → display downscale looks sharper than ×1 +
    GL upscale).
    """
    if in_w <= 0 or in_h <= 0:
        return 1
    sw = (win_w + in_w - 1) // in_w
    sh = (win_h + in_h - 1) // in_h
    s = min(sw, sh)
    return max(MIN_SCALE, min(MAX_SCALE, s))
