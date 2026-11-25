/***************************************************************************
 *   TEKSMK.C  -   smack flic stuff etc. for Tekwar                        *
 *                                                                         *
 *                                                                         *
 ***************************************************************************/

#include "build.h"
#include "compat.h"
#include "baselayer.h"
#include "pragmas.h"
#include "cache1d.h"
#include "tekwar.h"
#include "keyboard.h"
#ifdef JTEKWAR_GAMESTATION_GO
# include "joystick.h"
# include "fx_man.h"
#endif
#include "SmackerDecoder.h"

extern int nobriefflag;

SmackerHandle hSMK;
uint8_t* pFrame = nullptr;
uint8_t* pMenuBackground = nullptr;
uint32_t nWidth, nHeight;

#define kSMKPal   5
#define kSMKTile  (MAXTILES-1)
#define MAXSMKFRAMESIZE (640 * 480)

// Movie palette must stay out of the engine's `palette` global: basepaltable[0]
// aliases it, so fetching the SMK palette into `palette` permanently corrupts
// the game's base palette for everything rendered after the movie.
static uint8_t smkpalette[768];


#ifdef JTEKWAR_GAMESTATION_GO
// ---- SMK audio over audiolib demand-feed -----------------------------------
// The C++ SmackerDecoder exposes per-frame PCM, but this port never wired it
// up, so TekWar's movies were silent. Ring-buffer each frame's audio and let
// audiolib pull it; video stays clocked by the frame timer as before.

#define GSG_SMK_AUDIOBUF_SIZE (128 * 1024)
static uint8_t  gsgSmkAudioBuf[GSG_SMK_AUDIOBUF_SIZE];
static uint32_t gsgSmkAudioWrite, gsgSmkAudioRead; // monotonic ring positions
static int16_t  gsgSmkFrameAudio[24 * 1024];       // one frame's PCM staging

// A/V sync: the video loop clocks itself off the audio playback position
// (samples served by the mixer thread), so the two cannot drift apart.
// gsgSmkAudioServed counts every byte served, including underrun silence.
static volatile uint32_t gsgSmkAudioServed;
static uint32_t          gsgSmkAudioBytesPerSec;
static int32_t           gsgSmkAvOffsetMs = 100; // est. mixer pipeline latency

static void gsgSmkServeAudio(const char **ptr, uint32_t *length, void *)
{
    static uint8_t const silence[512] = {};
    uint32_t const avail  = gsgSmkAudioWrite - gsgSmkAudioRead;
    uint32_t const offset = gsgSmkAudioRead % GSG_SMK_AUDIOBUF_SIZE;
    uint32_t const chunk  = std::min(std::min(avail, (uint32_t)GSG_SMK_AUDIOBUF_SIZE - offset), 4096u);

    if (!chunk)
    {
        // keep the voice alive through decode gaps
        *ptr = (const char *)silence;
        *length = sizeof(silence);
        gsgSmkAudioServed += sizeof(silence);
        return;
    }

    *ptr = (const char *)&gsgSmkAudioBuf[offset];
    *length = chunk;
    gsgSmkAudioRead += chunk;
    gsgSmkAudioServed += chunk;
}

static void gsgSmkQueueAudio(SmackerHandle &handle)
{
    uint32_t bytes = Smacker_GetAudioData(handle, 0, gsgSmkFrameAudio);
    uint32_t const used = gsgSmkAudioWrite - gsgSmkAudioRead;

    if (bytes > GSG_SMK_AUDIOBUF_SIZE - used)
        bytes = GSG_SMK_AUDIOBUF_SIZE - used; // ring full: drop the tail

    uint32_t done = 0;
    while (done < bytes)
    {
        uint32_t const offset = gsgSmkAudioWrite % GSG_SMK_AUDIOBUF_SIZE;
        uint32_t const chunk  = std::min(bytes - done, (uint32_t)GSG_SMK_AUDIOBUF_SIZE - offset);
        memcpy(&gsgSmkAudioBuf[offset], (const uint8_t *)gsgSmkFrameAudio + done, chunk);
        gsgSmkAudioWrite += chunk;
        done += chunk;
    }
}
#endif

void smkplayseq(const char *name)
{
    char filename[BMAX_PATH];

    if ((generalplay != 0) || (nobriefflag != 0)) {
        return;
    }

    strcpy(filename, "/SMK/");
    strcat(filename, name);
    strcat(filename, ".SMK");

    hSMK = Smacker_Open(filename);
    if (!hSMK.isValid)
    {
        // data sets commonly carry the movies at the root instead
        Bsnprintf(filename, sizeof(filename), "%s.SMK", name);
        hSMK = Smacker_Open(filename);
        if (!hSMK.isValid)
        {
            Bsnprintf(filename, sizeof(filename), "SMK/%s.SMK", name);
            hSMK = Smacker_Open(filename);
            if (!hSMK.isValid)
                return;
        }
    }

    Smacker_GetFrameSize(hSMK, nWidth, nHeight);
    pFrame = (uint8_t*)Xmalloc(nWidth * nHeight);
#ifdef JTEKWAR_GAMESTATION_GO
    // The stock path declares the tile with swapped dimensions and draws it
    // rotated 90 degrees so the decoder's row-major output presents upright.
    // In the classic renderer that path scrambles 320x200 content (the
    // 640x480 menu movies survive it; intros do not), so keep a properly
    // transposed column-major tile and draw it unrotated instead.
    static uint8_t *pFrameT = NULL;
    if (!pFrameT)
        pFrameT = (uint8_t*)Xmalloc(MAXSMKFRAMESIZE);
    if (!pFrameT || nWidth * nHeight > MAXSMKFRAMESIZE)
    {
        Smacker_Close(hSMK);
        return;
    }
    waloff[kSMKTile] = (intptr_t)pFrameT;
    tileSetSize(kSMKTile, nWidth, nHeight);
#else
    waloff[kSMKTile] = (intptr_t)pFrame;
    tileSetSize(kSMKTile, nHeight, nWidth);
#endif
    walock[kSMKTile] = CACHE1D_PERMANENT;

    if (!pFrame)
    {
        Smacker_Close(hSMK);
        return;
    }

    int nFrameRate = Smacker_GetFrameRate(hSMK);
    int nFrames = Smacker_GetNumFrames(hSMK);

    auto const oyxaspect = yxaspect;

    int nScale = tabledivide32(scale(65536, ydim << 2, xdim * 3), ((max(nHeight, 240 + 1u) + 239) / 240));
    int nStat = 2|4|8|64|1024;
#ifndef JTEKWAR_GAMESTATION_GO
    renderSetAspect(viewingrange, 65536);
#endif

    #if 0
    Smacker_GotoFrame(hSMK, 16);

    rgb24_t quickpal[256];

    Smacker_GetPalette(hSMK, (uint8_t*)&quickpal);
    Smacker_GetFrame(hSMK, pFrame);

    FILE* fw = fopen("c:/temp/wilshart.raw", "wb");
    if (fw)
    {
        char pixel[3];
        for (int i = 0; i < nWidth * nHeight; i++)
        {
            memcpy(pixel, &quickpal[pFrame[i]], 3);
            fwrite(pixel, 3, 1, fw);
        }


        fclose(fw);
    }
    #endif

//    paletteSetColorTable(kSMKPal, palette);
//    videoSetPalette(/*gBrightness >> 2*/8, kSMKPal, 8 + 2);

    ClockTicks nStartTime = totalclock;

#ifdef JTEKWAR_GAMESTATION_GO
    // Any keypad button skips playback, but only after all buttons have been
    // released once so the press that started playback doesn't skip it too.
    bool gsgSkipArmed = (JOYSTICK_GetButtons() == 0);

    int gsgSmkVoice = -1;
    if (Smacker_GetNumAudioTracks(hSMK) > 0)
    {
        SmackerAudioInfo const info = Smacker_GetAudioTrackDetails(hSMK, 0);
        if (info.sampleRate >= 4000 && (info.bitsPerSample == 8 || info.bitsPerSample == 16))
        {
            gsgSmkAudioWrite = gsgSmkAudioRead = gsgSmkAudioServed = 0;
            gsgSmkAudioBytesPerSec = info.sampleRate * (info.nChannels ? info.nChannels : 1) * (info.bitsPerSample >> 3);
            if (char const * const env = getenv("GSG_SMK_AV_OFFSET_MS"))
                gsgSmkAvOffsetMs = Bstrtol(env, NULL, 10);
            int const musicVol = 255;
            gsgSmkVoice = FX_StartDemandFeedPlayback(gsgSmkServeAudio, info.bitsPerSample,
                                                     info.nChannels ? info.nChannels : 1, info.sampleRate,
                                                     0, musicVol, musicVol, musicVol,
                                                     FX_MUSIC_PRIORITY, fix16_one, -1, nullptr);
            if (gsgSmkVoice < 0)
                LOG_F(WARNING, "smkplayseq: failed to start movie audio (%s)", filename);
        }
    }
#endif

    int nFrame = 0;
    do
    {
        handleevents();
#ifdef JTEKWAR_GAMESTATION_GO
        // When the movie has an audio track, derive the ideal frame from the
        // mixer's playback position so audio and video cannot drift apart.
        // (gsgSmkAvOffsetMs accounts for the mixer pipeline's latency:
        // served bytes are heard ~that long after being served.)
        int idealFrame;
        if (gsgSmkVoice >= 0)
        {
            int64_t const heardMs = (int64_t)gsgSmkAudioServed * 1000 / gsgSmkAudioBytesPerSec - gsgSmkAvOffsetMs;
            idealFrame = heardMs > 0 ? (int)(scale((int)heardMs, nFrameRate, 1000)) : 0;
        }
        else
            idealFrame = scale((int)(totalclock - nStartTime), nFrameRate, CLKIPS);
#else
        int const idealFrame = scale((int)(totalclock - nStartTime), nFrameRate, CLKIPS);
#endif
        if (idealFrame < nFrame)
            continue;

        if (KB_KeyPressed(sc_Escape) || KB_KeyPressed(sc_Enter))
            break;

#ifdef JTEKWAR_GAMESTATION_GO
        if (!gsgSkipArmed)
            gsgSkipArmed = (JOYSTICK_GetButtons() == 0);
        else if (JOYSTICK_GetButtons() != 0)
            break;

        // Behind schedule (CPU-bound present at 1080p): fast-forward the
        // decoder without drawing, but still queue each skipped frame's
        // audio so the soundtrack stays in sync instead of stuttering.
        while (nFrame < idealFrame && nFrame + 1 < nFrames)
        {
            Smacker_GetNextFrame(hSMK);
            if (gsgSmkVoice >= 0)
                gsgSmkQueueAudio(hSMK);
            nFrame++;
        }
#endif

        Smacker_GetPalette(hSMK, smkpalette);
        paletteSetColorTable(kSMKPal, smkpalette);
        videoSetPalette(0, kSMKPal, 8+2);

        Smacker_GetFrame(hSMK, pFrame);
        tileInvalidate(kSMKTile, -1, -1);

#ifdef JTEKWAR_GAMESTATION_GO
        // Hand the frame to the presenter's direct-565 movie path (see
        // sdlayer.cpp); it stretch-converts into the scanout buffer during
        // videoNextPage, so no engine-framebuffer pass is needed at all.
        {
            extern void gsgSetMovieFrame(uint8_t const *frame, uint8_t const *pal, int32_t w, int32_t h);
            gsgSetMovieFrame(pFrame, smkpalette, nWidth, nHeight);
        }
        if (0)
        {
            videoClearViewableArea(0);
            int32_t const z = min(divscale16(320, tilesiz[kSMKTile].x), divscale16(200, tilesiz[kSMKTile].y));
            int32_t drawStat = RS_AUTO;
            if (getenv("GSG_SMK_PLAIN"))  // local diagnostic: bypass RS_AUTO transform
                drawStat = 0;
            rotatesprite(160 << 16, 100 << 16, z,
                         0, kSMKTile, 0, 0, drawStat, 0, 0, xdim - 1, ydim - 1);
        }
#else
        rotatesprite_fs(160 << 16, 100 << 16, nScale, 512, kSMKTile, 0, 0, nStat);
#endif

        videoNextPage();

        nFrame++;
        Smacker_GetNextFrame(hSMK);
#ifdef JTEKWAR_GAMESTATION_GO
        if (gsgSmkVoice >= 0)
            gsgSmkQueueAudio(hSMK);
#endif
    } while (nFrame < nFrames);

#ifdef JTEKWAR_GAMESTATION_GO
    if (gsgSmkVoice >= 0)
        FX_StopSound(gsgSmkVoice);
#endif

    KB_FlushKeyboardQueue();
    KB_ClearKeysDown();

    Smacker_Close(hSMK);
    renderSetAspect(viewingrange, oyxaspect);
    videoSetPalette(0, 0, 8 + 2);
    walock[kSMKTile] = 0;
    waloff[kSMKTile] = 0;
    tileSetSize(kSMKTile, 0, 0);
    Xfree(pFrame);
    pFrame = nullptr;

//    debugprintf("smkplayseq(\"%s\")\n", name);
}

void smkopenmenu(const char *name)
{
    hSMK = Smacker_Open(name);
    if (!hSMK.isValid)
    {
        return;
    }

    Smacker_GetFrameSize(hSMK, nWidth, nHeight);
    Smacker_GotoFrame(hSMK, 0);

    Smacker_GetPalette(hSMK, smkpalette);
    paletteSetColorTable(kSMKPal, smkpalette);
    videoSetPalette(/*gBrightness >> 2*/8, kSMKPal, 8 + 2);

    pFrame = (uint8_t*)Xmalloc(nWidth * nHeight);
    pMenuBackground = (uint8_t*)Xmalloc(nWidth * nHeight);

    if (!pFrame || !pMenuBackground)
    {
        Smacker_Close(hSMK);
        return;
    }

    walock[kSMKTile] = CACHE1D_PERMANENT;
    waloff[kSMKTile] = (intptr_t)pFrame;
    tileSetSize(kSMKTile, nHeight, nWidth);
    tileInvalidate(kSMKTile, 0, 1 << 4);  // JBF 20031228
    
    // first frame is menu background - keep a copy around for later
    Smacker_GetFrame(hSMK, pMenuBackground);
    memcpy(pFrame, pMenuBackground, nWidth * nHeight);
}

void smkmenuframe(int fn)
{
    if (!hSMK.isValid)
        return;

    Smacker_GetPalette(hSMK, smkpalette);
    paletteSetColorTable(kSMKPal, smkpalette);
    videoSetPalette(/*gBrightness >> 2*/8, kSMKPal, 8 + 2);

    tileInvalidate(kSMKTile, 0, 1 << 4);  // JBF 20031228

    Smacker_GotoFrame(hSMK, fn - 1);
    Smacker_GetFrame(hSMK, pFrame);

    // replace transparent pixels on overlay with background pixel
    for (int y = 0; y < nHeight; y++)
    {
        for (int x = 0; x < nWidth; x++)
        {
            uint32_t nPos = (y * nWidth) + x;
            if (pFrame[nPos] == 232) {
                pFrame[nPos] = pMenuBackground[nPos];
            }
        }
    }
}

void smkshowmenu()
{
    if (!hSMK.isValid)
        return;

    videoSetViewableArea(0, 0, xdim - 1, ydim - 1);
    videoClearViewableArea(0);
    rotatesprite(160 << 16, 100 << 16, divscale16(200, tilesiz[kSMKTile].x),
        512, kSMKTile, 0, 0, 2 + 4, 0, 0, xdim - 1, ydim - 1);

    videoNextPage();
}

void smkclosemenu()
{
    if (!hSMK.isValid)
        return;

    Smacker_Close(hSMK);

    walock[kSMKTile] = 0;
    waloff[kSMKTile] = 0;
    tileSetSize(kSMKTile, 0, 0);
    Xfree(pFrame);
    Xfree(pMenuBackground);
}
