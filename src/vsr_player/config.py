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
    """Compute the best integer upscale factor to fill the window."""
    if in_w <= 0 or in_h <= 0:
        return 1
    s = min(win_w // in_w, win_h // in_h)
    return max(MIN_SCALE, min(MAX_SCALE, s))
