/*
 * d3d9_methods.c - every COM interface this layer serves, with its methods in
 * vtable order (the declaration order of the D3D9, DirectDraw 7, D3DX and
 * DirectInput 8 headers; d3dx9_25's for ID3DXSprite and ID3DXFont) and its
 * IID. d3d_vtable() builds a guest vtable from one of these; a method nobody
 * implemented stops loudly, naming itself.
 */
#include "d3d_com.h"

#define UNK "QueryInterface", "AddRef", "Release"
#define RESOURCE UNK, "GetDevice", "SetPrivateData", "GetPrivateData", "FreePrivateData", "SetPriority", \
                 "GetPriority", "PreLoad", "GetType"
#define BASETEX RESOURCE, "SetLOD", "GetLOD", "GetLevelCount", "SetAutoGenFilterType", "GetAutoGenFilterType", \
                "GenerateMipSubLevels"

static const char *const m_d3d9[] = { UNK, "RegisterSoftwareDevice", "GetAdapterCount", "GetAdapterIdentifier",
    "GetAdapterModeCount", "EnumAdapterModes", "GetAdapterDisplayMode", "CheckDeviceType", "CheckDeviceFormat",
    "CheckDeviceMultiSampleType", "CheckDepthStencilMatch", "CheckDeviceFormatConversion", "GetDeviceCaps",
    "GetAdapterMonitor", "CreateDevice", 0 };
static const char *const m_device[] = { UNK, "TestCooperativeLevel", "GetAvailableTextureMem",
    "EvictManagedResources", "GetDirect3D", "GetDeviceCaps", "GetDisplayMode", "GetCreationParameters",
    "SetCursorProperties", "SetCursorPosition", "ShowCursor", "CreateAdditionalSwapChain", "GetSwapChain",
    "GetNumberOfSwapChains", "Reset", "Present", "GetBackBuffer", "GetRasterStatus", "SetDialogBoxMode",
    "SetGammaRamp", "GetGammaRamp", "CreateTexture", "CreateVolumeTexture", "CreateCubeTexture",
    "CreateVertexBuffer", "CreateIndexBuffer", "CreateRenderTarget", "CreateDepthStencilSurface", "UpdateSurface",
    "UpdateTexture", "GetRenderTargetData", "GetFrontBufferData", "StretchRect", "ColorFill",
    "CreateOffscreenPlainSurface", "SetRenderTarget", "GetRenderTarget", "SetDepthStencilSurface",
    "GetDepthStencilSurface", "BeginScene", "EndScene", "Clear", "SetTransform", "GetTransform",
    "MultiplyTransform", "SetViewport", "GetViewport", "SetMaterial", "GetMaterial", "SetLight", "GetLight",
    "LightEnable", "GetLightEnable", "SetClipPlane", "GetClipPlane", "SetRenderState", "GetRenderState",
    "CreateStateBlock", "BeginStateBlock", "EndStateBlock", "SetClipStatus", "GetClipStatus", "GetTexture",
    "SetTexture", "GetTextureStageState", "SetTextureStageState", "GetSamplerState", "SetSamplerState",
    "ValidateDevice", "SetPaletteEntries", "GetPaletteEntries", "SetCurrentTexturePalette",
    "GetCurrentTexturePalette", "SetScissorRect", "GetScissorRect", "SetSoftwareVertexProcessing",
    "GetSoftwareVertexProcessing", "SetNPatchMode", "GetNPatchMode", "DrawPrimitive", "DrawIndexedPrimitive",
    "DrawPrimitiveUP", "DrawIndexedPrimitiveUP", "ProcessVertices", "CreateVertexDeclaration",
    "SetVertexDeclaration", "GetVertexDeclaration", "SetFVF", "GetFVF", "CreateVertexShader", "SetVertexShader",
    "GetVertexShader", "SetVertexShaderConstantF", "GetVertexShaderConstantF", "SetVertexShaderConstantI",
    "GetVertexShaderConstantI", "SetVertexShaderConstantB", "GetVertexShaderConstantB", "SetStreamSource",
    "GetStreamSource", "SetStreamSourceFreq", "GetStreamSourceFreq", "SetIndices", "GetIndices",
    "CreatePixelShader", "SetPixelShader", "GetPixelShader", "SetPixelShaderConstantF", "GetPixelShaderConstantF",
    "SetPixelShaderConstantI", "GetPixelShaderConstantI", "SetPixelShaderConstantB", "GetPixelShaderConstantB",
    "DrawRectPatch", "DrawTriPatch", "DeletePatch", "CreateQuery", 0 };
static const char *const m_swapchain[] = { UNK, "Present", "GetFrontBufferData", "GetBackBuffer",
    "GetRasterStatus", "GetDisplayMode", "GetDevice", "GetPresentParameters", 0 };
static const char *const m_texture[] = { BASETEX, "GetLevelDesc", "GetSurfaceLevel", "LockRect", "UnlockRect",
    "AddDirtyRect", 0 };
static const char *const m_cubetexture[] = { BASETEX, "GetLevelDesc", "GetCubeMapSurface", "LockRect",
    "UnlockRect", "AddDirtyRect", 0 };
static const char *const m_volumetexture[] = { BASETEX, "GetLevelDesc", "GetVolumeLevel", "LockBox", "UnlockBox",
    "AddDirtyBox", 0 };
static const char *const m_surface[] = { RESOURCE, "GetContainer", "GetDesc", "LockRect", "UnlockRect", "GetDC",
    "ReleaseDC", 0 };
static const char *const m_volume[] = { UNK, "GetDevice", "SetPrivateData", "GetPrivateData", "FreePrivateData",
    "GetContainer", "GetDesc", "LockBox", "UnlockBox", 0 };
static const char *const m_vb[] = { RESOURCE, "Lock", "Unlock", "GetDesc", 0 };
static const char *const m_ib[] = { RESOURCE, "Lock", "Unlock", "GetDesc", 0 };
static const char *const m_vdecl[] = { UNK, "GetDevice", "GetDeclaration", 0 };
static const char *const m_vshader[] = { UNK, "GetDevice", "GetFunction", 0 };
static const char *const m_pshader[] = { UNK, "GetDevice", "GetFunction", 0 };
static const char *const m_stateblock[] = { UNK, "GetDevice", "Capture", "Apply", 0 };
static const char *const m_query[] = { UNK, "GetDevice", "GetType", "GetDataSize", "Issue", "GetData", 0 };
static const char *const m_ddraw7[] = { UNK, "Compact", "CreateClipper", "CreatePalette", "CreateSurface",
    "DuplicateSurface", "EnumDisplayModes", "EnumSurfaces", "FlipToGDISurface", "GetCaps", "GetDisplayMode",
    "GetFourCCCodes", "GetGDISurface", "GetMonitorFrequency", "GetScanLine", "GetVerticalBlankStatus",
    "Initialize", "RestoreDisplayMode", "SetCooperativeLevel", "SetDisplayMode", "WaitForVerticalBlank",
    "GetAvailableVidMem", "GetSurfaceFromDC", "RestoreAllSurfaces", "TestCooperativeLevel",
    "GetDeviceIdentifier", "StartModeTest", "EvaluateMode", 0 };
static const char *const m_d3dxbuffer[] = { UNK, "GetBufferPointer", "GetBufferSize", 0 };
/* d3dx9_25's ID3DXBaseEffect, then ID3DXEffect (no DeleteParameterBlock or SetRawValue before SDK 26/27) */
#define BASEEFFECT UNK, "GetDesc", "GetParameterDesc", "GetTechniqueDesc", "GetPassDesc", "GetFunctionDesc", \
    "GetParameter", "GetParameterByName", "GetParameterBySemantic", "GetParameterElement", "GetTechnique", \
    "GetTechniqueByName", "GetPass", "GetPassByName", "GetFunction", "GetFunctionByName", "GetAnnotation", \
    "GetAnnotationByName", "SetValue", "GetValue", "SetBool", "GetBool", "SetBoolArray", "GetBoolArray", "SetInt", \
    "GetInt", "SetIntArray", "GetIntArray", "SetFloat", "GetFloat", "SetFloatArray", "GetFloatArray", "SetVector", \
    "GetVector", "SetVectorArray", "GetVectorArray", "SetMatrix", "GetMatrix", "SetMatrixArray", "GetMatrixArray", \
    "SetMatrixPointerArray", "GetMatrixPointerArray", "SetMatrixTranspose", "GetMatrixTranspose", \
    "SetMatrixTransposeArray", "GetMatrixTransposeArray", "SetMatrixTransposePointerArray", \
    "GetMatrixTransposePointerArray", "SetString", "GetString", "SetTexture", "GetTexture", "GetPixelShader", \
    "GetVertexShader", "SetArrayRange"
static const char *const m_effect[] = { BASEEFFECT, "GetPool", "SetTechnique", "GetCurrentTechnique",
    "ValidateTechnique", "FindNextValidTechnique", "IsParameterUsed", "Begin", "BeginPass", "CommitChanges",
    "EndPass", "End", "GetDevice", "OnLostDevice", "OnResetDevice", "SetStateManager", "GetStateManager",
    "BeginParameterBlock", "EndParameterBlock", "ApplyParameterBlock", "CloneEffect", 0 };
static const char *const m_effectpool[] = { UNK, 0 };
static const char *const m_sprite[] = { UNK, "GetDevice", "GetTransform", "SetTransform", "SetWorldViewRH",
    "SetWorldViewLH", "Begin", "Draw", "Flush", "End", "OnLostDevice", "OnResetDevice", 0 };
static const char *const m_font[] = { UNK, "GetDevice", "GetDescA", "GetDescW", "GetTextMetricsA", "GetTextMetricsW",
    "GetDC", "GetGlyphData", "PreloadCharacters", "PreloadGlyphs", "PreloadTextA", "PreloadTextW", "DrawTextA",
    "DrawTextW", "OnLostDevice", "OnResetDevice", 0 };
static const char *const m_dinput8[] = { UNK, "CreateDevice", "EnumDevices", "GetDeviceStatus", "RunControlPanel",
    "Initialize", "FindDevice", "EnumDevicesBySemantics", "ConfigureDevices", 0 };
static const char *const m_didevice8[] = { UNK, "GetCapabilities", "EnumObjects", "GetProperty", "SetProperty",
    "Acquire", "Unacquire", "GetDeviceState", "GetDeviceData", "SetDataFormat", "SetEventNotification",
    "SetCooperativeLevel", "GetObjectInfo", "GetDeviceInfo", "RunControlPanel", "Initialize", "CreateEffect",
    "EnumEffects", "GetEffectInfo", "GetForceFeedbackState", "SendForceFeedbackCommand",
    "EnumCreatedEffectObjects", "Escape", "Poll", "SendDeviceData", "EnumEffectsInFile", "WriteEffectToFile",
    "BuildActionMap", "SetActionMap", "GetImageInfo", 0 };

#define G(a, b, c, d0, d1, d2, d3, d4, d5, d6, d7) \
    { (a) & 0xFF, ((a) >> 8) & 0xFF, ((a) >> 16) & 0xFF, ((a) >> 24) & 0xFF, (b) & 0xFF, ((b) >> 8) & 0xFF, \
      (c) & 0xFF, ((c) >> 8) & 0xFF, d0, d1, d2, d3, d4, d5, d6, d7 }

/* Each interface's IID, then its bases' (QueryInterface for any of them answers the object). */
const struct d3d_iface d3d_ifaces[] = {
    { "IDirect3D9", m_d3d9, { G(0x81BDCBCA, 0x64D4, 0x426D, 0xAE, 0x8D, 0xAD, 0x01, 0x47, 0xF4, 0x27, 0x5C) } },
    { "IDirect3DDevice9", m_device, { G(0xD0223B96, 0xBF7A, 0x43FD, 0x92, 0xBD, 0xA4, 0x3B, 0x0D, 0x82, 0xB9, 0xEB) } },
    { "IDirect3DSwapChain9", m_swapchain, { G(0x794950F2, 0xADFC, 0x458A, 0x90, 0x5E, 0x10, 0xA1, 0x0B, 0x0B, 0x50, 0x3B) } },
    { "IDirect3DTexture9", m_texture, { G(0x85C31227, 0x3DE5, 0x4F00, 0x9B, 0x3A, 0xF1, 0x1A, 0xC3, 0x8C, 0x18, 0xB5),
        G(0x580CA87E, 0x1D3C, 0x4D54, 0x99, 0x1D, 0xB7, 0xD3, 0xE3, 0xC2, 0x98, 0xCE),
        G(0x05EEC05D, 0x8F7D, 0x4362, 0xB9, 0x99, 0xD1, 0xBA, 0xF3, 0x57, 0xC7, 0x04) } },
    { "IDirect3DCubeTexture9", m_cubetexture, { G(0xFFF32F81, 0xD953, 0x473A, 0x92, 0x23, 0x93, 0xD6, 0x52, 0xAB, 0xA9, 0x3F),
        G(0x580CA87E, 0x1D3C, 0x4D54, 0x99, 0x1D, 0xB7, 0xD3, 0xE3, 0xC2, 0x98, 0xCE),
        G(0x05EEC05D, 0x8F7D, 0x4362, 0xB9, 0x99, 0xD1, 0xBA, 0xF3, 0x57, 0xC7, 0x04) } },
    { "IDirect3DVolumeTexture9", m_volumetexture, { G(0x2518526C, 0xE789, 0x4111, 0xA7, 0xB9, 0x47, 0xEF, 0x32, 0x8D, 0x13, 0xE6),
        G(0x580CA87E, 0x1D3C, 0x4D54, 0x99, 0x1D, 0xB7, 0xD3, 0xE3, 0xC2, 0x98, 0xCE),
        G(0x05EEC05D, 0x8F7D, 0x4362, 0xB9, 0x99, 0xD1, 0xBA, 0xF3, 0x57, 0xC7, 0x04) } },
    { "IDirect3DSurface9", m_surface, { G(0x0CFBAF3A, 0x9FF6, 0x429A, 0x99, 0xB3, 0xA2, 0x79, 0x6A, 0xF8, 0xB8, 0x9B),
        G(0x05EEC05D, 0x8F7D, 0x4362, 0xB9, 0x99, 0xD1, 0xBA, 0xF3, 0x57, 0xC7, 0x04) } },
    { "IDirect3DVolume9", m_volume, { G(0x24F416E6, 0x1F67, 0x4AA7, 0xB8, 0x8E, 0xD3, 0x3F, 0x6F, 0x31, 0x28, 0xA1) } },
    { "IDirect3DVertexBuffer9", m_vb, { G(0xB64BB1B5, 0xFD70, 0x4DF6, 0xBF, 0x91, 0x19, 0xD0, 0xA1, 0x24, 0x55, 0xE3),
        G(0x05EEC05D, 0x8F7D, 0x4362, 0xB9, 0x99, 0xD1, 0xBA, 0xF3, 0x57, 0xC7, 0x04) } },
    { "IDirect3DIndexBuffer9", m_ib, { G(0x7C9DD65E, 0xD3F7, 0x4529, 0xAC, 0xEE, 0x78, 0x58, 0x30, 0xAC, 0xDE, 0x35),
        G(0x05EEC05D, 0x8F7D, 0x4362, 0xB9, 0x99, 0xD1, 0xBA, 0xF3, 0x57, 0xC7, 0x04) } },
    { "IDirect3DVertexDeclaration9", m_vdecl, { G(0xDD13C59C, 0x36FA, 0x4098, 0xA8, 0xFB, 0xC7, 0xED, 0x39, 0xDC, 0x85, 0x46) } },
    { "IDirect3DVertexShader9", m_vshader, { G(0xEFC5557E, 0x6265, 0x4613, 0x8A, 0x94, 0x43, 0x85, 0x78, 0x89, 0xEB, 0x36) } },
    { "IDirect3DPixelShader9", m_pshader, { G(0x6D3BDBDC, 0x5B02, 0x4415, 0xB8, 0x52, 0xCE, 0x5E, 0x8B, 0xCC, 0xB2, 0x89) } },
    { "IDirect3DStateBlock9", m_stateblock, { G(0xB07C4FE5, 0x310D, 0x4BA8, 0xA2, 0x3C, 0x4F, 0x0F, 0x20, 0x6F, 0x21, 0x8B) } },
    { "IDirect3DQuery9", m_query, { G(0xD9771460, 0xA695, 0x4F26, 0xBB, 0xD3, 0x27, 0xB8, 0x40, 0xB5, 0x41, 0xCC) } },
    { "IDirectDraw7", m_ddraw7, { G(0x15E65EC0, 0x3B9C, 0x11D2, 0xB9, 0x2F, 0x00, 0x60, 0x97, 0x97, 0xEA, 0x5B) } },
    { "ID3DXBuffer", m_d3dxbuffer, { G(0x8BA5FB08, 0x5195, 0x40E2, 0xAC, 0x58, 0x0D, 0x98, 0x9C, 0x3A, 0x01, 0x02) } },
    { "ID3DXEffect", m_effect, { G(0xD165CCB1, 0x62B0, 0x4A33, 0xB3, 0xFA, 0xA9, 0x23, 0x00, 0x30, 0x6A, 0x3D),
        G(0x017C18AC, 0x103F, 0x4417, 0x8C, 0x51, 0x6B, 0xF6, 0xEF, 0x1E, 0x56, 0xBE) } },
    { "ID3DXEffectPool", m_effectpool, { G(0x9537AB04, 0x3250, 0x412E, 0x82, 0x13, 0xFC, 0xD2, 0xF8, 0x67, 0x79, 0x33) } },
    { "ID3DXSprite", m_sprite, { G(0xBA0B762D, 0x7D28, 0x43EC, 0xB9, 0xDC, 0x2F, 0x84, 0x44, 0x3B, 0x06, 0x14) } },
    { "ID3DXFont", m_font, { G(0xD79DBB70, 0x5F21, 0x4D36, 0xBB, 0xC2, 0xFF, 0x52, 0x5C, 0x21, 0x3C, 0xDC) } },
    { "IDirectInput8A", m_dinput8, { G(0xBF798030, 0x483A, 0x4DA2, 0xAA, 0x99, 0x5D, 0x64, 0xED, 0x36, 0x97, 0x00) } },
    { "IDirectInputDevice8A", m_didevice8, { G(0x54D41080, 0xDC15, 0x4833, 0xA4, 0x1B, 0x74, 0x8F, 0x73, 0xA3, 0x81, 0x79) } },
    { 0, 0, { { 0 } } },
};
