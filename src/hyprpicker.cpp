#include "hyprpicker.hpp"
#include "src/notify/Notify.hpp"
#include <csignal>
#include <cstddef>
#include <cstdio>
#include <format>

static void sigHandler(int sig) {
    g_pHyprpicker->m_vLayerSurfaces.clear();
    exit(0);
}

void CHyprpicker::init() {
    m_pXKBContext = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    if (!m_pXKBContext)
        Debug::log(ERR, "Failed to create xkb context");

    m_pWLDisplay = wl_display_connect(nullptr);

    if (!m_pWLDisplay) {
        Debug::log(CRIT, "No wayland compositor running!");
        exit(1);
        return;
    }

    signal(SIGTERM, sigHandler);

    m_pRegistry = makeShared<CCWlRegistry>((wl_proxy*)wl_display_get_registry(m_pWLDisplay));
    m_pRegistry->setGlobal([this](CCWlRegistry* r, uint32_t name, const char* interface, uint32_t version) {
        if (strcmp(interface, wl_compositor_interface.name) == 0) {
            m_pCompositor = makeShared<CCWlCompositor>((wl_proxy*)wl_registry_bind((wl_registry*)m_pRegistry->resource(), name, &wl_compositor_interface, 4));
        } else if (strcmp(interface, wl_shm_interface.name) == 0) {
            m_pSHM = makeShared<CCWlShm>((wl_proxy*)wl_registry_bind((wl_registry*)m_pRegistry->resource(), name, &wl_shm_interface, 1));
        } else if (strcmp(interface, wl_output_interface.name) == 0) {
            m_mtTickMutex.lock();

            const auto PMONITOR = g_pHyprpicker->m_vMonitors
                                      .emplace_back(std::make_unique<SMonitor>(
                                          makeShared<CCWlOutput>((wl_proxy*)wl_registry_bind((wl_registry*)m_pRegistry->resource(), name, &wl_output_interface, 4))))
                                      .get();
            PMONITOR->wayland_name = name;

            m_mtTickMutex.unlock();
        } else if (strcmp(interface, zwlr_layer_shell_v1_interface.name) == 0) {
            m_pLayerShell = makeShared<CCZwlrLayerShellV1>((wl_proxy*)wl_registry_bind((wl_registry*)m_pRegistry->resource(), name, &zwlr_layer_shell_v1_interface, 1));
        } else if (strcmp(interface, wl_seat_interface.name) == 0) {
            // Bind seat with compositor-provided version to receive repeat_info (v4+)
            const uint32_t SEAT_VER = std::min<uint32_t>(version, 7);
            m_pSeat = makeShared<CCWlSeat>((wl_proxy*)wl_registry_bind((wl_registry*)m_pRegistry->resource(), name, &wl_seat_interface, SEAT_VER));

            m_pSeat->setCapabilities([this](CCWlSeat* seat, uint32_t caps) {
                if (caps & WL_SEAT_CAPABILITY_POINTER) {
                    if (!m_pPointer) {
                        m_pPointer = makeShared<CCWlPointer>(m_pSeat->sendGetPointer());
                        initMouse();
                        if (m_pCursorShapeMgr)
                            m_pCursorShapeDevice = makeShared<CCWpCursorShapeDeviceV1>(m_pCursorShapeMgr->sendGetPointer(m_pPointer->resource()));
                    }
                } else {
                    Debug::log(CRIT, "Hyprpicker cannot work without a pointer!");
                    g_pHyprpicker->finish(1);
                }

                if (caps & WL_SEAT_CAPABILITY_KEYBOARD) {
                    if (!m_pKeyboard) {
                        m_pKeyboard = makeShared<CCWlKeyboard>(m_pSeat->sendGetKeyboard());
                        initKeyboard();
                    }
                } else
                    m_pKeyboard.reset();
            });

        } else if (strcmp(interface, zwlr_screencopy_manager_v1_interface.name) == 0) {
            m_pScreencopyMgr =
                makeShared<CCZwlrScreencopyManagerV1>((wl_proxy*)wl_registry_bind((wl_registry*)m_pRegistry->resource(), name, &zwlr_screencopy_manager_v1_interface, 1));
        } else if (strcmp(interface, wp_cursor_shape_manager_v1_interface.name) == 0) {
            m_pCursorShapeMgr =
                makeShared<CCWpCursorShapeManagerV1>((wl_proxy*)wl_registry_bind((wl_registry*)m_pRegistry->resource(), name, &wp_cursor_shape_manager_v1_interface, 1));
        } else if (strcmp(interface, wp_fractional_scale_manager_v1_interface.name) == 0) {
            m_pFractionalMgr =
                makeShared<CCWpFractionalScaleManagerV1>((wl_proxy*)wl_registry_bind((wl_registry*)m_pRegistry->resource(), name, &wp_fractional_scale_manager_v1_interface, 1));
        } else if (strcmp(interface, wp_viewporter_interface.name) == 0) {
            m_pViewporter = makeShared<CCWpViewporter>((wl_proxy*)wl_registry_bind((wl_registry*)m_pRegistry->resource(), name, &wp_viewporter_interface, 1));
        }
    });

    wl_display_roundtrip(m_pWLDisplay);

    // Cursor shape protocol is optional and not required when hiding cursor
    if (!m_pCursorShapeMgr)
        Debug::log(TRACE, "cursor_shape_v1 not present (unused)");

    if (!m_pScreencopyMgr) {
        Debug::log(CRIT, "zwlr_screencopy_v1 not supported, can't proceed");
        exit(1);
    }

    if (!m_pFractionalMgr) {
        Debug::log(WARN, "wp_fractional_scale_v1 not supported, fractional scaling won't work");
        m_bNoFractional = true;
    }
    if (!m_pViewporter) {
        Debug::log(WARN, "wp_viewporter not supported, fractional scaling won't work");
        m_bNoFractional = true;
    }

    // Use system cursor shape; no custom cursor drawing

    for (auto& m : m_vMonitors) {
        m_vLayerSurfaces.emplace_back(std::make_unique<CLayerSurface>(m.get()));

        m_pLastSurface = m_vLayerSurfaces.back().get();

        m->pSCFrame = makeShared<CCZwlrScreencopyFrameV1>(m_pScreencopyMgr->sendCaptureOutput(false, m->output->resource()));
        m->pLS      = m_vLayerSurfaces.back().get();
        m->initSCFrame();
    }

    wl_display_roundtrip(m_pWLDisplay);

    while (m_bRunning && wl_display_dispatch(m_pWLDisplay) != -1) {
        //renderSurface(m_pLastSurface);
    }

    if (m_pWLDisplay) {
        wl_display_disconnect(m_pWLDisplay);
        m_pWLDisplay = nullptr;
    }
}

// (removed) initCursorTheme — no custom cursor drawing

void CHyprpicker::finish(int code) {
    m_vLayerSurfaces.clear();

    if (m_pWLDisplay) {
        m_vLayerSurfaces.clear();
        m_vMonitors.clear();
        m_pCompositor.reset();
        m_pRegistry.reset();
        m_pSHM.reset();
        m_pLayerShell.reset();
        m_pScreencopyMgr.reset();
        m_pCursorShapeMgr.reset();
        m_pCursorShapeDevice.reset();
        m_pSeat.reset();
        m_pKeyboard.reset();
        m_pPointer.reset();
        m_pViewporter.reset();
        m_pFractionalMgr.reset();

        wl_display_disconnect(m_pWLDisplay);
        m_pWLDisplay = nullptr;
    }

    exit(code);
}

void CHyprpicker::recheckACK() {
    for (auto& ls : m_vLayerSurfaces) {
        if ((ls->wantsACK || ls->wantsReload) && ls->screenBuffer) {
            if (ls->wantsACK)
                ls->pLayerSurface->sendAckConfigure(ls->ACKSerial);
            ls->wantsACK    = false;
            ls->wantsReload = false;

            const auto MONITORSIZE =
                (ls->screenBuffer && !g_pHyprpicker->m_bNoFractional ? ls->m_pMonitor->size * ls->fractionalScale : ls->m_pMonitor->size * ls->m_pMonitor->scale).round();

            if (!ls->buffers[0] || ls->buffers[0]->pixelSize != MONITORSIZE) {
                Debug::log(TRACE, "making new buffers: size changed to %.0fx%.0f", MONITORSIZE.x, MONITORSIZE.y);
                ls->buffers[0] = makeShared<SPoolBuffer>(MONITORSIZE, WL_SHM_FORMAT_ARGB8888, MONITORSIZE.x * 4);
                ls->buffers[1] = makeShared<SPoolBuffer>(MONITORSIZE, WL_SHM_FORMAT_ARGB8888, MONITORSIZE.x * 4);
            }

            
        }
    }

    markDirty();
}

void CHyprpicker::markDirty() {
    for (auto& ls : m_vLayerSurfaces) {
        if (ls->frameCallback)
            continue;

        ls->markDirty();
    }
}

SP<SPoolBuffer> CHyprpicker::getBufferForLS(CLayerSurface* pLS) {
    SP<SPoolBuffer> returns = nullptr;

    for (auto i = 0; i < 2; ++i) {
        if (!pLS->buffers[i] || pLS->buffers[i]->busy)
            continue;

        returns = pLS->buffers[i];
    }

    return returns;
}

bool CHyprpicker::setCloexec(const int& FD) {
    long flags = fcntl(FD, F_GETFD);
    if (flags == -1) {
        return false;
    }

    if (fcntl(FD, F_SETFD, flags | FD_CLOEXEC) == -1) {
        return false;
    }

    return true;
}

int CHyprpicker::createPoolFile(size_t size, std::string& name) {
    const auto XDGRUNTIMEDIR = getenv("XDG_RUNTIME_DIR");
    if (!XDGRUNTIMEDIR) {
        Debug::log(CRIT, "XDG_RUNTIME_DIR not set!");
        g_pHyprpicker->finish(1);
    }

    name = std::string(XDGRUNTIMEDIR) + "/.hyprpicker_XXXXXX";

    const auto FD = mkstemp((char*)name.c_str());
    if (FD < 0) {
        Debug::log(CRIT, "createPoolFile: fd < 0");
        g_pHyprpicker->finish(1);
    }

    if (!setCloexec(FD)) {
        close(FD);
        Debug::log(CRIT, "createPoolFile: !setCloexec");
        g_pHyprpicker->finish(1);
    }

    if (ftruncate(FD, size) < 0) {
        close(FD);
        Debug::log(CRIT, "createPoolFile: ftruncate < 0");
        g_pHyprpicker->finish(1);
    }

    return FD;
}

void CHyprpicker::convertBuffer(SP<SPoolBuffer> pBuffer) {
    switch (pBuffer->format) {
        case WL_SHM_FORMAT_ARGB8888:
        case WL_SHM_FORMAT_XRGB8888: break;
        case WL_SHM_FORMAT_ABGR8888:
        case WL_SHM_FORMAT_XBGR8888: {
            uint8_t* data = (uint8_t*)pBuffer->data;

            for (int y = 0; y < pBuffer->pixelSize.y; ++y) {
                for (int x = 0; x < pBuffer->pixelSize.x; ++x) {
                    struct SPixel {
                        // little-endian ARGB
                        unsigned char blue;
                        unsigned char green;
                        unsigned char red;
                        unsigned char alpha;
                    }* px = (struct SPixel*)(data + (static_cast<ptrdiff_t>(y * (int)pBuffer->pixelSize.x * 4)) + (static_cast<ptrdiff_t>(x * 4)));

                    std::swap(px->red, px->blue);
                }
            }
        } break;
        case WL_SHM_FORMAT_XRGB2101010:
        case WL_SHM_FORMAT_XBGR2101010: {
            uint8_t*   data = (uint8_t*)pBuffer->data;

            const bool FLIP = pBuffer->format == WL_SHM_FORMAT_XBGR2101010;

            for (int y = 0; y < pBuffer->pixelSize.y; ++y) {
                for (int x = 0; x < pBuffer->pixelSize.x; ++x) {
                    uint32_t* px = (uint32_t*)(data + (static_cast<ptrdiff_t>(y * (int)pBuffer->pixelSize.x * 4)) + (static_cast<ptrdiff_t>(x * 4)));

                    // conv to 8 bit
                    uint8_t R = (uint8_t)std::round((255.0 * (((*px) & 0b00000000000000000000001111111111) >> 0) / 1023.0));
                    uint8_t G = (uint8_t)std::round((255.0 * (((*px) & 0b00000000000011111111110000000000) >> 10) / 1023.0));
                    uint8_t B = (uint8_t)std::round((255.0 * (((*px) & 0b00111111111100000000000000000000) >> 20) / 1023.0));
                    uint8_t A = (uint8_t)std::round((255.0 * (((*px) & 0b11000000000000000000000000000000) >> 30) / 3.0));

                    // write 8-bit values
                    *px = ((FLIP ? B : R) << 0) + (G << 8) + ((FLIP ? R : B) << 16) + (A << 24);
                }
            }
        } break;
        default: {
            Debug::log(CRIT, "Unsupported format %i", pBuffer->format);
        }
            g_pHyprpicker->finish(1);
    }
}

// Mallocs a new buffer, which needs to be free'd!
void* CHyprpicker::convert24To32Buffer(SP<SPoolBuffer> pBuffer) {
    uint8_t* newBuffer       = (uint8_t*)malloc((size_t)pBuffer->pixelSize.x * pBuffer->pixelSize.y * 4);
    int      newBufferStride = pBuffer->pixelSize.x * 4;
    uint8_t* oldBuffer       = (uint8_t*)pBuffer->data;

    switch (pBuffer->format) {
        case WL_SHM_FORMAT_BGR888: {
            for (int y = 0; y < pBuffer->pixelSize.y; ++y) {
                for (int x = 0; x < pBuffer->pixelSize.x; ++x) {
                    struct SPixel3 {
                        // little-endian RGB
                        unsigned char blue;
                        unsigned char green;
                        unsigned char red;
                    }* srcPx = (struct SPixel3*)(oldBuffer + (static_cast<size_t>(y * pBuffer->stride)) + (static_cast<ptrdiff_t>(x * 3)));
                    struct SPixel4 {
                        // little-endian ARGB
                        unsigned char blue;
                        unsigned char green;
                        unsigned char red;
                        unsigned char alpha;
                    }* dstPx = (struct SPixel4*)(newBuffer + (static_cast<ptrdiff_t>(y * newBufferStride)) + (static_cast<ptrdiff_t>(x * 4)));
                    *dstPx   = {.blue = srcPx->red, .green = srcPx->green, .red = srcPx->blue, .alpha = 0xFF};
                }
            }
        } break;
        case WL_SHM_FORMAT_RGB888: {
            for (int y = 0; y < pBuffer->pixelSize.y; ++y) {
                for (int x = 0; x < pBuffer->pixelSize.x; ++x) {
                    struct SPixel3 {
                        // big-endian RGB
                        unsigned char red;
                        unsigned char green;
                        unsigned char blue;
                    }* srcPx = (struct SPixel3*)(oldBuffer + (y * pBuffer->stride) + (x * 3));
                    struct SPixel4 {
                        // big-endian ARGB
                        unsigned char alpha;
                        unsigned char red;
                        unsigned char green;
                        unsigned char blue;
                    }* dstPx = (struct SPixel4*)(newBuffer + (y * newBufferStride) + (x * 4));
                    *dstPx   = {.alpha = 0xFF, .red = srcPx->red, .green = srcPx->green, .blue = srcPx->blue};
                }
            }
        } break;
        default: {
            Debug::log(CRIT, "Unsupported format for 24bit buffer %i", pBuffer->format);
        }
            g_pHyprpicker->finish(1);
    }
    return newBuffer;
}

void CHyprpicker::renderSurface(CLayerSurface* pSurface, bool forceInactive) {
    const auto PBUFFER = getBufferForLS(pSurface);

    if (!PBUFFER || !pSurface->screenBuffer) {
        // Spammy log, doesn't matter.
        // Debug::log(ERR, PBUFFER ? "renderSurface: pSurface->screenBuffer null" : "renderSurface: PBUFFER null");
        return;
    }

    PBUFFER->surface =
        cairo_image_surface_create_for_data((unsigned char*)PBUFFER->data, CAIRO_FORMAT_ARGB32, PBUFFER->pixelSize.x, PBUFFER->pixelSize.y, PBUFFER->pixelSize.x * 4);

    PBUFFER->cairo = cairo_create(PBUFFER->surface);

    const auto PCAIRO = PBUFFER->cairo;

    cairo_save(PCAIRO);

    cairo_set_source_rgba(PCAIRO, 0, 0, 0, 0);

    cairo_rectangle(PCAIRO, 0, 0, PBUFFER->pixelSize.x, PBUFFER->pixelSize.y);
    cairo_fill(PCAIRO);

    if (pSurface == m_pLastSurface && !forceInactive && m_bCoordsInitialized) {
        const auto SCALEBUFS      = pSurface->screenBuffer->pixelSize / PBUFFER->pixelSize;
        const auto MOUSECOORDSABS = m_vLastCoords.floor() / pSurface->m_pMonitor->size;
        const auto CLICKPOS       = MOUSECOORDSABS * PBUFFER->pixelSize;

        Debug::log(TRACE, "renderSurface: scalebufs %.2fx%.2f", SCALEBUFS.x, SCALEBUFS.y);

        const auto PATTERNPRE = cairo_pattern_create_for_surface(pSurface->screenBuffer->surface);
        cairo_pattern_set_filter(PATTERNPRE, CAIRO_FILTER_BILINEAR);
        cairo_matrix_t matrixPre;
        cairo_matrix_init_identity(&matrixPre);
        cairo_matrix_scale(&matrixPre, SCALEBUFS.x, SCALEBUFS.y);
        cairo_pattern_set_matrix(PATTERNPRE, &matrixPre);
        cairo_set_source(PCAIRO, PATTERNPRE);
        cairo_paint(PCAIRO);

        cairo_surface_flush(PBUFFER->surface);

        cairo_pattern_destroy(PATTERNPRE);

        // we draw the preview like this
        //
        //     200px        ZOOM: 10x
        // | --------- |
        // |           |
        // |     x     | 200px
        // |           |
        // | --------- |
        //
        // (hex code here)

        cairo_restore(PCAIRO);
        if (!m_bNoZoom) {
            cairo_save(PCAIRO);

            // Compute center position with keyboard nudge applied (in buffer pixels)
            const auto BASEPOSBUF = CLICKPOS / PBUFFER->pixelSize * pSurface->screenBuffer->pixelSize;
            Vector2D    centerBuf = BASEPOSBUF + m_vNudgeBufPx;
            // Clamp to valid pixel range
            centerBuf.x           = std::clamp(centerBuf.x, 0.0, pSurface->screenBuffer->pixelSize.x - 1.0);
            centerBuf.y           = std::clamp(centerBuf.y, 0.0, pSurface->screenBuffer->pixelSize.y - 1.0);

            // UI center should move with the nudge as well. Convert the nudge (screenBuffer pixels)
            // into this surface's buffer coordinates and offset the on-screen UI accordingly.
            const auto SCALEBUFS_INV = Vector2D{1.0 / SCALEBUFS.x, 1.0 / SCALEBUFS.y};
            Vector2D    uiCenter     = CLICKPOS + (m_vNudgeBufPx * SCALEBUFS_INV);
            uiCenter.x               = std::clamp(uiCenter.x, 0.0, PBUFFER->pixelSize.x - 1.0);
            uiCenter.y               = std::clamp(uiCenter.y, 0.0, PBUFFER->pixelSize.y - 1.0);

            const auto PIXCOLOR = getColorFromPixel(pSurface, centerBuf);
            cairo_set_source_rgba(PCAIRO, PIXCOLOR.r / 255.f, PIXCOLOR.g / 255.f, PIXCOLOR.b / 255.f, PIXCOLOR.a / 255.f);

            cairo_scale(PCAIRO, 1, 1);

            // Update spring animations for zoom radius and magnification
            {
                using namespace std::chrono;
                const auto now = steady_clock::now();
                if (!m_zoomAnimInitialized) {
                    m_zoomAnimInitialized = true;
                    m_zoomLastTick        = now;
                    m_zoomRadiusCurrentSrcPx = m_zoomRadiusTargetSrcPx;
                    m_zoomRadiusVel          = 0.0;
                    m_zoomMagCurrent         = m_zoomMagTarget;
                    m_zoomMagVel             = 0.0;
                    if (!m_zoomMagBaseSet) {
                        m_zoomMagBase    = m_zoomMagTarget;
                        m_zoomMagBaseSet = true;
                    }
                    // Aperture will be preserved on ALT zoom using current values at the event
                }
                const double dt = duration<double>(now - m_zoomLastTick).count();
                m_zoomLastTick  = now;
                if (dt > 0.0) {
                    // Snappier critically-damped spring
                    const double k    = SPRING_K;
                    const double zeta = SPRING_ZETA;
                    const double c    = 2.0 * std::sqrt(k) * zeta;
                    // Radius
                    {
                        const double x = m_zoomRadiusCurrentSrcPx - m_zoomRadiusTargetSrcPx;
                        const double a = (-k * x) - (c * m_zoomRadiusVel);
                        m_zoomRadiusVel += a * dt;
                        m_zoomRadiusCurrentSrcPx += m_zoomRadiusVel * dt;
                        // Snap when very close to avoid jitter
                        if (std::abs(m_zoomRadiusCurrentSrcPx - m_zoomRadiusTargetSrcPx) < 0.01 && std::abs(m_zoomRadiusVel) < 0.01) {
                            m_zoomRadiusCurrentSrcPx = m_zoomRadiusTargetSrcPx;
                            m_zoomRadiusVel          = 0.0;
                        }
                    }
                    // Magnification
                    {
                        const double xM = m_zoomMagCurrent - m_zoomMagTarget;
                        const double aM = (-k * xM) - (c * m_zoomMagVel);
                        m_zoomMagVel += aM * dt;
                        m_zoomMagCurrent += m_zoomMagVel * dt;
                        if (std::abs(m_zoomMagCurrent - m_zoomMagTarget) < 0.01 && std::abs(m_zoomMagVel) < 0.01) {
                            m_zoomMagCurrent = m_zoomMagTarget;
                            m_zoomMagVel     = 0.0;
                        }
                    }
                    // If locking aperture (ALT zoom transition), force radius to keep UI circle constant
                    if (m_lockAperture) {
                        const double targetR = (m_zoomMagCurrent > 0.01) ? (m_lockedAperture / m_zoomMagCurrent) : m_zoomRadiusCurrentSrcPx;
                        m_zoomRadiusCurrentSrcPx = targetR;
                        m_zoomRadiusTargetSrcPx  = targetR;
                        m_zoomRadiusVel          = 0.0;
                        // Release the lock once magnification settles at target
                        if (std::abs(m_zoomMagCurrent - m_zoomMagTarget) < 0.01 && std::abs(m_zoomMagVel) < 0.01)
                            m_lockAperture = false;
                    }
                }
            }

            // Keep the zoom circle centered at the (possibly nudged) UI center
            const double cellWForRadius = m_zoomMagCurrent / SCALEBUFS.x;
            const double zoomRadiusUI   = m_zoomRadiusCurrentSrcPx * cellWForRadius;
            const double onePxUI        = 1.0 / std::min(SCALEBUFS.x, SCALEBUFS.y);
            const double outerRadiusUI  = zoomRadiusUI + RING_OFFSET_UI_PX * onePxUI; // thin border ring
            cairo_arc(PCAIRO, uiCenter.x, uiCenter.y, outerRadiusUI, 0, 2 * M_PI);
            cairo_clip(PCAIRO);

            cairo_fill(PCAIRO);
            cairo_paint(PCAIRO);

            cairo_surface_flush(PBUFFER->surface);

            cairo_restore(PCAIRO);
            cairo_save(PCAIRO);

            const auto PATTERN = cairo_pattern_create_for_surface(pSurface->screenBuffer->surface);
            cairo_pattern_set_filter(PATTERN, CAIRO_FILTER_NEAREST);
            cairo_matrix_t matrix;
            cairo_matrix_init_identity(&matrix);
            cairo_matrix_translate(&matrix, centerBuf.x + 0.5f, centerBuf.y + 0.5f);
            const double invMag = 1.0 / std::max(0.01, m_zoomMagCurrent);
            cairo_matrix_scale(&matrix, invMag, invMag);
            cairo_matrix_translate(&matrix, (-centerBuf.x / SCALEBUFS.x) - 0.5f, (-centerBuf.y / SCALEBUFS.y) - 0.5f);
            cairo_pattern_set_matrix(PATTERN, &matrix);
            cairo_set_source(PCAIRO, PATTERN);
            // Keep the zoom circle centered at the (possibly nudged) UI center
            const double zoomRadius = zoomRadiusUI;
            cairo_arc(PCAIRO, uiCenter.x, uiCenter.y, zoomRadius, 0, 2 * M_PI);
            cairo_clip(PCAIRO);
            cairo_paint(PCAIRO);

            // Draw a faint pixel grid overlay aligned to the source pixels
            // One source pixel maps to m_zoomMagCurrent in the zoom (scale 1/mag above)
            {
                const double cellW = m_zoomMagCurrent / SCALEBUFS.x;
                const double cellH = m_zoomMagCurrent / SCALEBUFS.y;

                // Snap visual grid to the nearest source pixel so it stays static
                const double pxX           = std::floor(centerBuf.x);
                const double pxY           = std::floor(centerBuf.y);
                const double centerBoundX  = pxX + 0.5; // center of the pixel under the cursor
                const double centerBoundY  = pxY + 0.5;

                // Compute visible boundary index range around the zoom circle (boundaries at integers)
                const double minVXSrc = centerBoundX - (zoomRadius / cellW);
                const double maxVXSrc = centerBoundX + (zoomRadius / cellW);
                const long   vStart   = (long)std::floor(minVXSrc);
                const long   vEnd     = (long)std::ceil(maxVXSrc);

                const double minVYSrc = centerBoundY - (zoomRadius / cellH);
                const double maxVYSrc = centerBoundY + (zoomRadius / cellH);
                const long   hStart   = (long)std::floor(minVYSrc);
                const long   hEnd     = (long)std::ceil(maxVYSrc);

                cairo_save(PCAIRO);
                // Very faint lines; hairline width in UI pixels
                cairo_set_antialias(PCAIRO, CAIRO_ANTIALIAS_NONE);
                cairo_set_source_rgba(PCAIRO, 1.0, 1.0, 1.0, GRID_ALPHA);
                cairo_set_line_width(PCAIRO, onePxUI);

                // Vertical lines (at integer x boundaries)
                for (long j = vStart; j <= vEnd; ++j) {
                    const double srcX  = (double)j;
                    const double drawX = uiCenter.x + (srcX - centerBoundX) * cellW;
                    cairo_move_to(PCAIRO, drawX, uiCenter.y - zoomRadius);
                    cairo_line_to(PCAIRO, drawX, uiCenter.y + zoomRadius);
                }

                // Horizontal lines (at integer y boundaries)
                for (long i = hStart; i <= hEnd; ++i) {
                    const double srcY  = (double)i;
                    const double drawY = uiCenter.y + (srcY - centerBoundY) * cellH;
                    cairo_move_to(PCAIRO, uiCenter.x - zoomRadius, drawY);
                    cairo_line_to(PCAIRO, uiCenter.x + zoomRadius, drawY);
                }

                cairo_stroke(PCAIRO);

                // Highlight the central pixel with a solid white border, fixed at center
                cairo_set_source_rgba(PCAIRO, 1.0, 1.0, 1.0, 1.0);
                cairo_set_line_width(PCAIRO, 2.0 * onePxUI);
                cairo_set_line_join(PCAIRO, CAIRO_LINE_JOIN_ROUND);
                const double leftUI   = uiCenter.x - 0.5 * cellW;
                const double rightUI  = uiCenter.x + 0.5 * cellW;
                const double topUI    = uiCenter.y - 0.5 * cellH;
                const double bottomUI = uiCenter.y + 0.5 * cellH;
                cairo_rectangle(PCAIRO, leftUI, topUI, rightUI - leftUI, bottomUI - topUI);
                cairo_stroke(PCAIRO);
                cairo_restore(PCAIRO);
            }

            // (Ring border and shadow moved after clip restore so they are visible)

            if (!m_bDisablePreview) {
                const auto  currentColor = getColorFromPixel(pSurface, centerBuf);
                std::string previewBuffer;
                switch (m_bSelectedOutputMode) {
                    case OUTPUT_HEX: {
                        previewBuffer = std::format("#{:02X}{:02X}{:02X}", currentColor.r, currentColor.g, currentColor.b);
                        if (m_bUseLowerCase)
                            for (auto& c : previewBuffer)
                                c = std::tolower(c);
                        break;
                    };
                    case OUTPUT_RGB: {
                        previewBuffer = std::format("{} {} {}", currentColor.r, currentColor.g, currentColor.b);
                        break;
                    };
                    case OUTPUT_HSL: {
                        float h, s, l;
                        currentColor.getHSL(h, s, l);
                        previewBuffer = std::format("{} {}% {}%", h, s, l);
                        break;
                    };
                    case OUTPUT_HSV: {
                        float h, s, v;
                        currentColor.getHSV(h, s, v);
                        previewBuffer = std::format("{} {}% {}%", h, s, v);
                        break;
                    };
                    case OUTPUT_CMYK: {
                        float c, m, y, k;
                        currentColor.getCMYK(c, m, y, k);
                        previewBuffer = std::format("{}% {}% {}% {}%", c, m, y, k);
                        break;
                    };
                };
                cairo_set_source_rgba(PCAIRO, 0.0, 0.0, 0.0, 0.75);

                double x, y, width = 8 + (11 * previewBuffer.length()), height = 28, radius = 6;

                if (uiCenter.y > (PBUFFER->pixelSize.y - 50) && uiCenter.x > (PBUFFER->pixelSize.x - 100)) {
                    x = uiCenter.x - 80;
                    y = uiCenter.y - 40;
                } else if (uiCenter.y > (PBUFFER->pixelSize.y - 50)) {
                    x = uiCenter.x;
                    y = uiCenter.y - 40;
                } else if (uiCenter.x > (PBUFFER->pixelSize.x - 100)) {
                    x = uiCenter.x - 80;
                    y = uiCenter.y + 20;
                } else {
                    x = uiCenter.x;
                    y = uiCenter.y + 20;
                }
                x -= 5.5 * previewBuffer.length();
                cairo_move_to(PCAIRO, x + radius, y);
                cairo_arc(PCAIRO, x + width - radius, y + radius, radius, -M_PI_2, 0);
                cairo_arc(PCAIRO, x + width - radius, y + height - radius, radius, 0, M_PI_2);
                cairo_arc(PCAIRO, x + radius, y + height - radius, radius, M_PI_2, M_PI);
                cairo_arc(PCAIRO, x + radius, y + radius, radius, M_PI, -M_PI_2);

                cairo_close_path(PCAIRO);
                cairo_fill(PCAIRO);

                cairo_set_source_rgba(PCAIRO, 1.0, 1.0, 1.0, 1.0);
                cairo_select_font_face(PCAIRO, "monospace", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
                cairo_set_font_size(PCAIRO, 18);

                double padding = 5.0;
                double textX   = x + padding;

                if (uiCenter.y > (PBUFFER->pixelSize.y - 50) && uiCenter.x > (PBUFFER->pixelSize.x - 100))
                    cairo_move_to(PCAIRO, textX, uiCenter.y - 20);
                else if (uiCenter.y > (PBUFFER->pixelSize.y - 50))
                    cairo_move_to(PCAIRO, textX, uiCenter.y - 20);
                else if (uiCenter.x > (PBUFFER->pixelSize.x - 100))
                    cairo_move_to(PCAIRO, textX, uiCenter.y + 40);
                else
                    cairo_move_to(PCAIRO, textX, uiCenter.y + 40);

                cairo_show_text(PCAIRO, previewBuffer.c_str());

                cairo_surface_flush(PBUFFER->surface);
            }
            cairo_restore(PCAIRO);
            cairo_pattern_destroy(PATTERN);

            // Add a 2px white border and a subtle shadow around the color ring (outside any clip)
            {
                cairo_save(PCAIRO);
                cairo_set_antialias(PCAIRO, CAIRO_ANTIALIAS_DEFAULT);
                cairo_set_line_join(PCAIRO, CAIRO_LINE_JOIN_ROUND);

                const double ringOuterRad = zoomRadiusUI + RING_OFFSET_UI_PX * onePxUI; // aligns with thin ring offset

                // Shadow: soft halo outside the ring
                cairo_set_source_rgba(PCAIRO, 0.0, 0.0, 0.0, RING_SHADOW_ALPHA);
                cairo_set_line_width(PCAIRO, RING_SHADOW_PX * onePxUI);
                cairo_new_path(PCAIRO);
                cairo_arc(PCAIRO, uiCenter.x, uiCenter.y, ringOuterRad + 1.0 * onePxUI, 0, 2 * M_PI);
                cairo_stroke(PCAIRO);

                // White border: crisp 2px stroke around the ring
                cairo_set_source_rgba(PCAIRO, 1.0, 1.0, 1.0, 1.0);
                cairo_set_line_width(PCAIRO, RING_BORDER_PX * onePxUI);
                cairo_new_path(PCAIRO);
                cairo_arc(PCAIRO, uiCenter.x, uiCenter.y, ringOuterRad, 0, 2 * M_PI);
                cairo_stroke(PCAIRO);

                cairo_restore(PCAIRO);
            }

            // removed custom cursor overlay drawing
        }
    } else if (!m_bRenderInactive && m_bCoordsInitialized) {
        cairo_set_operator(PCAIRO, CAIRO_OPERATOR_SOURCE);
        cairo_set_source_rgba(PCAIRO, 0, 0, 0, 0);
        cairo_rectangle(PCAIRO, 0, 0, PBUFFER->pixelSize.x, PBUFFER->pixelSize.y);
        cairo_fill(PCAIRO);
    } else if (m_bCoordsInitialized) {
        const auto SCALEBUFS  = pSurface->screenBuffer->pixelSize / PBUFFER->pixelSize;
        const auto PATTERNPRE = cairo_pattern_create_for_surface(pSurface->screenBuffer->surface);
        cairo_pattern_set_filter(PATTERNPRE, CAIRO_FILTER_BILINEAR);
        cairo_matrix_t matrixPre;
        cairo_matrix_init_identity(&matrixPre);
        cairo_matrix_scale(&matrixPre, SCALEBUFS.x, SCALEBUFS.y);
        cairo_pattern_set_matrix(PATTERNPRE, &matrixPre);
        cairo_set_source(PCAIRO, PATTERNPRE);
        cairo_paint(PCAIRO);

        cairo_surface_flush(PBUFFER->surface);
        cairo_pattern_destroy(PATTERNPRE);

        // removed custom cursor overlay drawing
    }

    pSurface->sendFrame();
    cairo_destroy(PCAIRO);
    cairo_surface_destroy(PBUFFER->surface);

    PBUFFER->busy    = true;
    PBUFFER->cairo   = nullptr;
    PBUFFER->surface = nullptr;

    pSurface->rendered = true;
}

// Consolidated scroll helpers
void CHyprpicker::handleAltToggle(bool toTriple) {
    if (!m_zoomMagBaseSet) {
        m_zoomMagBase    = m_zoomMagTarget;
        m_zoomMagBaseSet = true;
    }
    const double targetMag = toTriple ? (m_zoomMagBase * ZOOM_TOGGLE_FACTOR) : m_zoomMagBase;
    m_zoomMagTarget        = std::clamp(targetMag, ZOOM_MAG_MIN, ZOOM_MAG_MAX);
    // Lock aperture across the animated transition
    m_lockedAperture = m_zoomRadiusCurrentSrcPx * m_zoomMagCurrent;
    m_lockAperture   = true;
    if (m_zoomMagTarget > 0.01)
        m_zoomRadiusTargetSrcPx = std::clamp(m_lockedAperture / m_zoomMagTarget, ZOOM_RADIUS_MIN, ZOOM_RADIUS_MAX);
}

void CHyprpicker::handleRadiusToggle(bool toDouble) {
    // Seed base UI aperture from the current circle size
    if (!m_apertureBaseSet) {
        m_apertureBaseUI = m_zoomRadiusCurrentSrcPx * m_zoomMagCurrent;
        m_apertureBaseSet = true;
    }
    const double desiredAperture = toDouble ? (m_apertureBaseUI * 2.0) : m_apertureBaseUI;
    // Convert desired aperture to a radius at the current magnification
    if (m_zoomMagCurrent > 0.01) {
        m_zoomRadiusTargetSrcPx = std::clamp(desiredAperture / m_zoomMagCurrent, ZOOM_RADIUS_MIN, ZOOM_RADIUS_MAX);
    }
    // Not an ALT zoom; ensure we aren't locking during radius toggles
    m_lockAperture = false;
}

CColor CHyprpicker::getColorFromPixel(CLayerSurface* pLS, Vector2D pix) {
    pix = pix.floor();

    if (pix.x >= pLS->screenBuffer->pixelSize.x || pix.y >= pLS->screenBuffer->pixelSize.y || pix.x < 0 || pix.y < 0)
        return CColor{.r = 0, .g = 0, .b = 0, .a = 0};

    void* dataSrc = pLS->screenBuffer->paddedData ? pLS->screenBuffer->paddedData : pLS->screenBuffer->data;

    struct SPixel {
        unsigned char blue;
        unsigned char green;
        unsigned char red;
        unsigned char alpha;
    }* px = (struct SPixel*)((char*)dataSrc + ((ptrdiff_t)pix.y * (int)pLS->screenBuffer->pixelSize.x * 4) + ((ptrdiff_t)pix.x * 4));

    return CColor{.r = px->red, .g = px->green, .b = px->blue, .a = px->alpha};
}

void CHyprpicker::finalizePickAtCurrent(bool forceFinalize) {
    if (!m_pLastSurface)
        return;
    // relative brightness of a color
    const auto FLUMI = [](const float& c) -> float { return c <= 0.03928 ? c / 12.92 : powf((c + 0.055) / 1.055, 2.4); };

    // get the px and print it (apply keyboard nudge in screen buffer pixels)
    const auto MOUSECOORDSABS = m_vLastCoords.floor() / m_pLastSurface->m_pMonitor->size;
    Vector2D   CLICKPOS       = MOUSECOORDSABS * m_pLastSurface->screenBuffer->pixelSize;
    CLICKPOS += m_vNudgeBufPx;
    CLICKPOS.x = std::clamp(CLICKPOS.x, 0.0, m_pLastSurface->screenBuffer->pixelSize.x - 1.0);
    CLICKPOS.y = std::clamp(CLICKPOS.y, 0.0, m_pLastSurface->screenBuffer->pixelSize.y - 1.0);

    const auto COL = getColorFromPixel(m_pLastSurface, CLICKPOS);

    const uint8_t FG = 0.2126 * FLUMI(COL.r / 255.0f) + 0.7152 * FLUMI(COL.g / 255.0f) + 0.0722 * FLUMI(COL.b / 255.0f) > 0.17913 ? 0 : 255;

    auto          toHex = [this](int i) -> std::string {
        const char* DS = m_bUseLowerCase ? "0123456789abcdef" : "0123456789ABCDEF";

        std::string result = "";

        result += DS[i / 16];
        result += DS[i % 16];

        return result;
    };

    std::string hexColor = std::format("#{0:02x}{1:02x}{2:02x}", COL.r, COL.g, COL.b);

    // Prepare outputs
    std::string formattedColor;
    switch (m_bSelectedOutputMode) {
        case OUTPUT_CMYK: {
            float c, m, y, k;
            COL.getCMYK(c, m, y, k);

            formattedColor = std::format("{}% {}% {}% {}%", c, m, y, k);
            break; 
        }
        case OUTPUT_HEX: {
            formattedColor = hexColor;
            break;
        }
        case OUTPUT_RGB: {
            formattedColor = std::format("{} {} {}", COL.r, COL.g, COL.b);
            break;
        }
        case OUTPUT_HSL:
        case OUTPUT_HSV: {
            float h, s, l_or_v;
            if (m_bSelectedOutputMode == OUTPUT_HSV)
                COL.getHSV(h, s, l_or_v);
            else
                COL.getHSL(h, s, l_or_v);

            formattedColor = std::format("{} {}% {}%", h, s, l_or_v);
            break;
        }
    }

    // Decide multi-pick vs single pick
    const bool withShift = (m_pXKBState && xkb_state_mod_name_is_active(m_pXKBState, XKB_MOD_NAME_SHIFT, XKB_STATE_MODS_EFFECTIVE));
    if (!forceFinalize) {
        if (withShift) {
            // Accumulate and keep running
            m_multiMode = true;
            m_multiBuffer.push_back(formattedColor);
            return;
        } else if (m_multiMode) {
            // Non-shift click after accumulating: add and finalize
            m_multiBuffer.push_back(formattedColor);
            std::string joined;
            for (size_t i = 0; i < m_multiBuffer.size(); ++i) {
                if (i)
                    joined += "\n";
                joined += m_multiBuffer[i];
            }
            if (m_bAutoCopy)
                NClipboard::copy(joined);
            else
                Debug::log(NONE, "%s", joined.c_str());
            finish();
            return;
        }
    } else if (m_multiMode) {
        // Forced finalize (Enter) while batching: include current and finish
        m_multiBuffer.push_back(formattedColor);
        std::string joined;
        for (size_t i = 0; i < m_multiBuffer.size(); ++i) {
            if (i)
                joined += "\n";
            joined += m_multiBuffer[i];
        }
        if (m_bAutoCopy)
            NClipboard::copy(joined);
        else
            Debug::log(NONE, "%s", joined.c_str());
        finish();
        return;
    }

    // Single pick legacy behavior
    switch (m_bSelectedOutputMode) {
        case OUTPUT_CMYK: {
            float c, m, y, k;
            COL.getCMYK(c, m, y, k);
            if (m_bFancyOutput)
                Debug::log(NONE, "\033[38;2;%i;%i;%i;48;2;%i;%i;%im%g%% %g%% %g%% %g%%\033[0m", FG, FG, FG, COL.r, COL.g, COL.b, c, m, y, k);
            else
                Debug::log(NONE, "%g%% %g%% %g%% %g%%", c, m, y, k);
            if (m_bAutoCopy)
                NClipboard::copy(formattedColor);
            if (m_bNotify)
                NNotify::send(hexColor, formattedColor);
            finish();
            break;
        }
        case OUTPUT_HEX: {
            if (m_bFancyOutput)
                Debug::log(NONE, "\033[38;2;%i;%i;%i;48;2;%i;%i;%im#%s%s%s\033[0m", FG, FG, FG, COL.r, COL.g, COL.b, toHex(COL.r).c_str(), toHex(COL.g).c_str(), toHex(COL.b).c_str());
            else
                Debug::log(NONE, "#%s%s%s", toHex(COL.r).c_str(), toHex(COL.g).c_str(), toHex(COL.b).c_str());
            if (m_bAutoCopy)
                NClipboard::copy(formattedColor);
            if (m_bNotify)
                NNotify::send(hexColor, hexColor);
            finish();
            break;
        }
        case OUTPUT_RGB: {
            if (m_bFancyOutput)
                Debug::log(NONE, "\033[38;2;%i;%i;%i;48;2;%i;%i;%im%i %i %i\033[0m", FG, FG, FG, COL.r, COL.g, COL.b, COL.r, COL.g, COL.b);
            else
                Debug::log(NONE, "%i %i %i", COL.r, COL.g, COL.b);
            if (m_bAutoCopy)
                NClipboard::copy(formattedColor);
            if (m_bNotify)
                NNotify::send(hexColor, formattedColor);
            finish();
            break;
        }
        case OUTPUT_HSL:
        case OUTPUT_HSV: {
            float h, s, l_or_v;
            if (m_bSelectedOutputMode == OUTPUT_HSV)
                COL.getHSV(h, s, l_or_v);
            else
                COL.getHSL(h, s, l_or_v);
            if (m_bFancyOutput)
                Debug::log(NONE, "\033[38;2;%i;%i;%i;48;2;%i;%i;%im%g %g%% %g%%\033[0m", FG, FG, FG, COL.r, COL.g, COL.b, h, s, l_or_v);
            else
                Debug::log(NONE, "%g %g%% %g%%", h, s, l_or_v);
            if (m_bAutoCopy)
                NClipboard::copy(formattedColor);
            if (m_bNotify)
                NNotify::send(hexColor, formattedColor);
            finish();
            break;
        }
    }
}

void CHyprpicker::startRepeatThread() {
    if (m_repeatThreadRunning.exchange(true))
        return;

    m_repeatThread = std::thread([this]() {
        using namespace std::chrono;
        bool                         anyPrev = false;
        steady_clock::time_point     pressStart{};
        steady_clock::time_point     nextRepeat{};

        while (true) {
            const bool any = m_keyLeft || m_keyRight || m_keyUp || m_keyDown;

            if (any) {
                const auto now = steady_clock::now();
                if (!anyPrev) {
                    anyPrev    = true;
                    pressStart = now;
                    nextRepeat = pressStart + milliseconds(std::max(0, m_repeatDelay.load()));
                } else {
                    const int rate = m_repeatRate.load();
                    if (rate > 0) {
                        if (now >= nextRepeat) {
                            const double step = (m_pXKBState && xkb_state_mod_name_is_active(m_pXKBState, XKB_MOD_NAME_SHIFT, XKB_STATE_MODS_EFFECTIVE)) ? 8.0 : 1.0;
                            if (!m_bNoZoom && m_bCoordsInitialized && m_pLastSurface) {
                                if (m_keyLeft)
                                    m_vNudgeBufPx.x -= step;
                                if (m_keyRight)
                                    m_vNudgeBufPx.x += step;
                                if (m_keyUp)
                                    m_vNudgeBufPx.y -= step;
                                if (m_keyDown)
                                    m_vNudgeBufPx.y += step;
                                markDirty();
                            }
                            // schedule next
                            nextRepeat = now + duration_cast<milliseconds>(duration<double>(1.0 / static_cast<double>(rate)));
                        }
                    }
                }
            } else {
                anyPrev = false;
            }

            std::this_thread::sleep_for(milliseconds(5));
        }
    });
    m_repeatThread.detach();
}

void CHyprpicker::initKeyboard() {
    m_pKeyboard->setKeymap([this](CCWlKeyboard* r, wl_keyboard_keymap_format format, int32_t fd, uint32_t size) {
        if (!m_pXKBContext)
            return;

        if (format != WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1) {
            Debug::log(ERR, "Could not recognise keymap format");
            return;
        }

        const char* buf = (const char*)mmap(nullptr, size, PROT_READ, MAP_SHARED, fd, 0);
        if (buf == MAP_FAILED) {
            Debug::log(ERR, "Failed to mmap xkb keymap: %d", errno);
            return;
        }

        m_pXKBKeymap = xkb_keymap_new_from_buffer(m_pXKBContext, buf, size - 1, XKB_KEYMAP_FORMAT_TEXT_V1, XKB_KEYMAP_COMPILE_NO_FLAGS);

        munmap((void*)buf, size);
        close(fd);

        if (!m_pXKBKeymap) {
            Debug::log(ERR, "Failed to compile xkb keymap");
            return;
        }

        m_pXKBState = xkb_state_new(m_pXKBKeymap);
        if (!m_pXKBState) {
            Debug::log(ERR, "Failed to create xkb state");
            return;
        }
        // Start repeat thread now that keyboard is ready
        startRepeatThread();
    });

    // Update xkb modifier state so Shift detection works
    m_pKeyboard->setModifiers([this](CCWlKeyboard* r, uint32_t serial, uint32_t mods_depressed, uint32_t mods_latched, uint32_t mods_locked, uint32_t group) {
        if (m_pXKBState)
            xkb_state_update_mask(m_pXKBState, mods_depressed, mods_latched, mods_locked, 0, 0, group);
    });

    // Receive repeat info (rate chars/sec, delay ms) from compositor (Hyprland)
    m_pKeyboard->setRepeatInfo([this](CCWlKeyboard* r, int32_t rate, int32_t delay) {
        m_repeatRate  = rate;
        m_repeatDelay = delay;
    });

    m_pKeyboard->setKey([this](CCWlKeyboard* r, uint32_t serial, uint32_t time, uint32_t key, uint32_t state) {
        if (m_pXKBState) {
            const xkb_keysym_t sym = xkb_state_key_get_one_sym(m_pXKBState, key + 8);
            if (state == WL_KEYBOARD_KEY_STATE_PRESSED) {
                if (sym == XKB_KEY_Escape) {
                    finish(2);
                    return;
                }
                if (sym == XKB_KEY_Return || sym == XKB_KEY_KP_Enter) {
                    // Enter finalizes the selection; include current under cursor
                    finalizePickAtCurrent(true);
                    return;
                }
                if (!m_bNoZoom && m_bCoordsInitialized && m_pLastSurface) {
                    const double step = xkb_state_mod_name_is_active(m_pXKBState, XKB_MOD_NAME_SHIFT, XKB_STATE_MODS_EFFECTIVE) ? 8.0 : 1.0;
                    bool         nudged = false;
                    switch (sym) {
                        case XKB_KEY_Left: m_vNudgeBufPx.x -= step; m_keyLeft = true; nudged = true; break;
                        case XKB_KEY_Right: m_vNudgeBufPx.x += step; m_keyRight = true; nudged = true; break;
                        case XKB_KEY_Up: m_vNudgeBufPx.y -= step; m_keyUp = true; nudged = true; break;
                        case XKB_KEY_Down: m_vNudgeBufPx.y += step; m_keyDown = true; nudged = true; break;
                    }
                    if (nudged)
                        markDirty();
                }
            } else if (state == WL_KEYBOARD_KEY_STATE_RELEASED) {
                switch (sym) {
                    case XKB_KEY_Left: m_keyLeft = false; break;
                    case XKB_KEY_Right: m_keyRight = false; break;
                    case XKB_KEY_Up: m_keyUp = false; break;
                    case XKB_KEY_Down: m_keyDown = false; break;
                }
            }
        } else if (key == 1 && state == WL_KEYBOARD_KEY_STATE_PRESSED) // Assume keycode 1 is escape
            finish(2);
    });
}

void CHyprpicker::initMouse() {
    m_pPointer->setEnter([this](CCWlPointer* r, uint32_t serial, wl_proxy* surface, wl_fixed_t surface_x, wl_fixed_t surface_y) {
        auto x = wl_fixed_to_double(surface_x);
        auto y = wl_fixed_to_double(surface_y);

        m_vLastCoords        = {x, y};
        m_bCoordsInitialized = true;
        m_vNudgeBufPx        = {0, 0};

        for (auto& ls : m_vLayerSurfaces) {
            if (ls->pSurface->resource() == surface) {
                m_pLastSurface = ls.get();
                break;
            }
        }

        // Hide the system cursor when hyprpicker is active
        // Wayland: set a null cursor surface to hide pointer
        m_pPointer->sendSetCursor(serial, nullptr, 0, 0);

        markDirty();
    });
    // Adjust zoom radius or magnification with scroll (Alt modifies magnification)
    m_pPointer->setAxisDiscrete([this](CCWlPointer* r, enum wl_pointer_axis axis, int32_t discrete) {
        if (m_bNoZoom || !m_bCoordsInitialized)
            return;
        if (axis != WL_POINTER_AXIS_VERTICAL_SCROLL)
            return;
        int steps = discrete; // wlroots: up is negative, down is positive
        if (steps == 0)
            return;
        // Modifiers
        // shift not used in toggled zoom modes
        const bool withAlt   = (m_pXKBState && xkb_state_mod_name_is_active(m_pXKBState, XKB_MOD_NAME_ALT, XKB_STATE_MODS_EFFECTIVE));
        if (withAlt) {
            handleAltToggle(steps < 0);
        } else {
            // Toggle between base radius and 2x base
            handleRadiusToggle(steps < 0);
        }
        markDirty();
    });
    m_pPointer->setAxisValue120([this](CCWlPointer* r, enum wl_pointer_axis axis, int32_t value120) {
        if (m_bNoZoom || !m_bCoordsInitialized)
            return;
        if (axis != WL_POINTER_AXIS_VERTICAL_SCROLL)
            return;
        if (value120 == 0)
            return;
        int steps = value120 / 120; // multiples of 120 per notch
        if (steps == 0)
            steps = (value120 > 0 ? 1 : -1);
        // shift not used in toggled zoom modes
        const bool withAlt   = (m_pXKBState && xkb_state_mod_name_is_active(m_pXKBState, XKB_MOD_NAME_ALT, XKB_STATE_MODS_EFFECTIVE));
        if (withAlt) {
            handleAltToggle(steps < 0);
        } else {
            handleRadiusToggle(steps < 0);
        }
        markDirty();
    });
    // Fallback for smooth axis if discrete not provided
    m_pPointer->setAxis([this](CCWlPointer* r, uint32_t timeMs, enum wl_pointer_axis axis, wl_fixed_t value) {
        if (m_bNoZoom || !m_bCoordsInitialized)
            return;
        if (axis != WL_POINTER_AXIS_VERTICAL_SCROLL)
            return;
        const double v = wl_fixed_to_double(value);
        if (std::abs(v) < 0.01)
            return;
        // shift not used in toggled zoom modes
        const bool withAlt   = (m_pXKBState && xkb_state_mod_name_is_active(m_pXKBState, XKB_MOD_NAME_ALT, XKB_STATE_MODS_EFFECTIVE));
        if (withAlt) {
            handleAltToggle(v < 0);
        } else {
            handleRadiusToggle(v < 0);
        }
        markDirty();
    });
    m_pPointer->setLeave([this](CCWlPointer* r, uint32_t timeMs, wl_proxy* surface) {
        for (auto& ls : m_vLayerSurfaces) {
            if (ls->pSurface->resource() == surface) {
                if (m_pLastSurface == ls.get())
                    m_pLastSurface = nullptr;
                break;
            }
        }

        markDirty();
    });
    m_pPointer->setMotion([this](CCWlPointer* r, uint32_t timeMs, wl_fixed_t surface_x, wl_fixed_t surface_y) {
        auto x = wl_fixed_to_double(surface_x);
        auto y = wl_fixed_to_double(surface_y);

        m_vLastCoords = {x, y};
        // reset nudge on mouse movement
        m_vNudgeBufPx = {0, 0};

        markDirty();
    });
    m_pPointer->setButton([this](CCWlPointer* r, uint32_t serial, uint32_t time, uint32_t button, uint32_t button_state) {
        // Only act on press to avoid duplicate actions on release
        if (button_state == WL_POINTER_BUTTON_STATE_PRESSED) {
            // Mouse click: Shift-click accumulates, plain click finalizes batch
            finalizePickAtCurrent(false);
        }
    });
}
