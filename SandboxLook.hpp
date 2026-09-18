#pragma once

// Shared sandbox chrome flags (glow playground + ULW frame).
struct SandboxLook {
    bool light = true;   // white ribbon / window — the glow-on-light test
    bool glass = true;   // α<255 body so the desktop shows through
    bool shadow = true;  // soft drop shadow in a transparent gutter
};

inline SandboxLook& Sandbox()
{
    static SandboxLook s;
    return s;
}

inline int ShadowPadPx()
{
    return Sandbox().shadow ? 18 : 0;
}
