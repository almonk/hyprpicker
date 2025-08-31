#pragma once

#include "defines.hpp"
#include "helpers/LayerSurface.hpp"
#include "helpers/PoolBuffer.hpp"
#include <atomic>

enum eOutputMode {
    OUTPUT_CMYK = 0,
    OUTPUT_HEX,
    OUTPUT_RGB,
    OUTPUT_HSL,
    OUTPUT_HSV
};

class CHyprpicker {
  public:
    void                                        init();
    

    std::mutex                                  m_mtTickMutex;

    SP<CCWlCompositor>                          m_pCompositor;
    SP<CCWlRegistry>                            m_pRegistry;
    SP<CCWlShm>                                 m_pSHM;
    SP<CCZwlrLayerShellV1>                      m_pLayerShell;
    SP<CCZwlrScreencopyManagerV1>               m_pScreencopyMgr;
    SP<CCWpCursorShapeManagerV1>                m_pCursorShapeMgr;
    SP<CCWpCursorShapeDeviceV1>                 m_pCursorShapeDevice;
    SP<CCWlSeat>                                m_pSeat;
    SP<CCWlKeyboard>                            m_pKeyboard;
    SP<CCWlPointer>                             m_pPointer;
    SP<CCWpFractionalScaleManagerV1>            m_pFractionalMgr;
    SP<CCWpViewporter>                          m_pViewporter;
    wl_display*                                 m_pWLDisplay = nullptr;

    xkb_context*                                m_pXKBContext = nullptr;
    xkb_keymap*                                 m_pXKBKeymap  = nullptr;
    xkb_state*                                  m_pXKBState   = nullptr;

    eOutputMode                                 m_bSelectedOutputMode = OUTPUT_HEX;

    bool                                        m_bFancyOutput = true;

    bool                                        m_bAutoCopy       = false;
    bool                                        m_bNotify         = false;
    bool                                        m_bRenderInactive = false;
    bool                                        m_bNoZoom         = false;
    bool                                        m_bNoFractional   = false;
    bool                                        m_bDisablePreview = false;
    bool                                        m_bUseLowerCase   = false;

    bool                                        m_bRunning = true;

    std::vector<std::unique_ptr<SMonitor>>      m_vMonitors;
    std::vector<std::unique_ptr<CLayerSurface>> m_vLayerSurfaces;

    CLayerSurface*                              m_pLastSurface;

    Vector2D                                    m_vLastCoords;
    bool                                        m_bCoordsInitialized = false;

    // Nudge offset for keyboard-controlled fine movement in screen buffer pixels
    Vector2D                                    m_vNudgeBufPx = {0, 0};

    

    // Keyboard repeat handling
    std::atomic<bool>                           m_keyLeft{false}, m_keyRight{false}, m_keyUp{false}, m_keyDown{false};
    std::atomic<int>                            m_repeatRate{0};   // chars/sec (0 or negative disables)
    std::atomic<int>                            m_repeatDelay{600}; // ms
    std::atomic<bool>                           m_repeatThreadRunning{false};
    std::thread                                  m_repeatThread;
    void                                        startRepeatThread();

    // Zoom UI radius spring animation (source pixels before 10x scaling)
    double                                      m_zoomRadiusTargetSrcPx = 10.0;
    double                                      m_zoomRadiusCurrentSrcPx = 10.0;
    double                                      m_zoomRadiusVel = 0.0; // src px / s
    std::chrono::steady_clock::time_point       m_zoomLastTick{};
    bool                                        m_zoomAnimInitialized = false;

    // Zoom magnification (UI pixels per source pixel), animated for smoothness
    double                                      m_zoomMagTarget = 10.0;   // default 10x
    double                                      m_zoomMagCurrent = 10.0;
    double                                      m_zoomMagVel = 0.0;       // ui px per src px per s
    // Base magnification for discrete toggle (ALT-scroll). Initialized on first use.
    double                                      m_zoomMagBase = 10.0;
    bool                                        m_zoomMagBaseSet = false;
    // Keep UI circle size constant during ALT zoom
    bool                                        m_lockAperture = false;
    double                                      m_lockedAperture = 0.0; // ui px per src px * src px = ui px radius

    // Helpers to consolidate scroll handling
    void                                        handleAltToggle(bool toTriple);
    void                                        handleRadiusToggle(bool toDouble);

    // Base UI aperture (radius * magnification) for discrete radius toggle
    double                                      m_apertureBaseUI = 0.0;
    bool                                        m_apertureBaseSet = false;

    void                                        renderSurface(CLayerSurface*, bool forceInactive = false);

    int                                         createPoolFile(size_t, std::string&);
    bool                                        setCloexec(const int&);
    void                                        recheckACK();
    void                                        initKeyboard();
    void                                        initMouse();

    SP<SPoolBuffer>                             getBufferForLS(CLayerSurface*);

    void                                        convertBuffer(SP<SPoolBuffer>);
    void*                                       convert24To32Buffer(SP<SPoolBuffer>);

    void                                        markDirty();

    void                                        finish(int code = 0);
    void                                        finalizePickAtCurrent(bool forceFinalize);

    CColor                                      getColorFromPixel(CLayerSurface*, Vector2D);

    // Multi-pick accumulation (Shift-click)
    std::vector<std::string>                    m_multiBuffer;
    bool                                        m_multiMode = false;

    struct SLabelStackItem {
        std::string text;
        double      offsetCurrentUI = 0.0; // animated vertical offset in UI pixels
        double      offsetTargetUI  = 0.0; // target offset in UI pixels
    };
    std::vector<SLabelStackItem>                m_previewStack;
    std::chrono::steady_clock::time_point       m_uiAnimLastTick{};
    bool                                        m_uiAnimInitialized = false;

  private:
};

inline std::unique_ptr<CHyprpicker> g_pHyprpicker;
