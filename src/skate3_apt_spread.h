#pragma once

// Wide-output factor the APT HUD is spread by: output aspect / (16/9) while
// skate3_apt_spread is on and the native renderer serves a wider-than-16:9
// output, else 1. The renderer widens the APT canvas clip by the same factor.
float Skate3AptSpreadFactor();
