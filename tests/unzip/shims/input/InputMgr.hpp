#pragma once

class c_InputMgr
{
public:
    bool extractionPaused = false;
    unsigned pauseCount = 0;
    unsigned resumeCount = 0;
    void SetFileExtractionPaused(bool Paused)
    {
        extractionPaused = Paused;
        if (Paused) ++pauseCount;
        else ++resumeCount;
    }
};
inline c_InputMgr InputMgr;
