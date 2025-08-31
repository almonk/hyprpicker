#pragma once

#include "debug/Log.hpp"
#include "includes.hpp"
#include "helpers/Monitor.hpp"
#include "helpers/Color.hpp"
#include "clipboard/Clipboard.hpp"
#include "notify/Notify.hpp"

// git stuff
#ifndef GIT_COMMIT_HASH
#define GIT_COMMIT_HASH "?"
#endif
#ifndef GIT_BRANCH
#define GIT_BRANCH "?"
#endif
#ifndef GIT_COMMIT_MESSAGE
#define GIT_COMMIT_MESSAGE "?"
#endif
#ifndef GIT_DIRTY
#define GIT_DIRTY "?"
#endif

#include <sys/types.h>

#include <hyprutils/math/Vector2D.hpp>
using namespace Hyprutils::Math;

// UI/Zoom constants
constexpr double ZOOM_TOGGLE_FACTOR = 3.0;
constexpr double ZOOM_MAG_MIN       = 2.0;
constexpr double ZOOM_MAG_MAX       = 60.0;
constexpr double ZOOM_RADIUS_MIN    = 4.0;
constexpr double ZOOM_RADIUS_MAX    = 60.0;

// Critically-damped spring parameters
constexpr double SPRING_K    = 1000.0; // stiffness
constexpr double SPRING_ZETA = 1.0;    // damping ratio

// Ring and grid styling (in UI pixels, converted per-scale)
constexpr double RING_OFFSET_UI_PX   = 5.0;
constexpr double RING_BORDER_PX      = 2.0;
constexpr double RING_SHADOW_PX      = 4.0;
constexpr double RING_SHADOW_ALPHA   = 0.25;
constexpr double GRID_ALPHA          = 0.12;
