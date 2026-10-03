#include "mock_d3d9.h"
#include <algorithm>
#include <iostream>

static MockDirect3D9 *g_pMockD3D9 = nullptr;
static MockDirect3DDevice9 *g_pMockD3DDevice9 = nullptr;

MockDirect3D9 *GetGlobalMockD3D9() {
    if (!g_pMockD3D9) {
        g_pMockD3D9 = new MockDirect3D9();
    }
    return g_pMockD3D9;
}

MockDirect3DDevice9 *GetGlobalMockD3DDevice9() {
    return g_pMockD3DDevice9;
}

void ResetMockD3D9() {
    if (g_pMockD3DDevice9) {
        g_pMockD3DDevice9->ClearHistory();
    }
}

extern "C" {
IDirect3D9 * WINAPI Direct3DCreate9(UINT SDKVersion) {
    (void)SDKVersion;
    MockDirect3D9 *p = GetGlobalMockD3D9();
    p->AddRef();
    return p;
}

HRESULT WINAPI Direct3DCreate9Ex(UINT SDKVersion, IDirect3D9Ex **ppD3D) {
    (void)SDKVersion;
    if (!ppD3D) return D3DERR_INVALIDCALL;
    MockDirect3D9 *p = GetGlobalMockD3D9();
    p->AddRef();
    *ppD3D = p;
    return D3D_OK;
}
}

/* ================= MockDirect3DSurface9 ================= */

MockDirect3DSurface9::MockDirect3DSurface9(MockDirect3DDevice9 *pDevice, const D3DSURFACE_DESC &desc)
    : m_pDevice(pDevice), m_desc(desc) {
    UINT bpp = 4;
    switch (desc.Format) {
        case D3DFMT_R5G6B5:
        case D3DFMT_X1R5G5B5:
        case D3DFMT_A1R5G5B5:
        case D3DFMT_A4R4G4B4:
        case D3DFMT_D16:
        case D3DFMT_L16:
        case D3DFMT_V8U8:
            bpp = 2;
            break;
        case D3DFMT_L8:
        case D3DFMT_A8:
            bpp = 1;
            break;
        case D3DFMT_A16B16G16R16F:
        case D3DFMT_A16B16G16R16:
            bpp = 8;
            break;
        case D3DFMT_A32B32G32R32F:
            bpp = 16;
            break;
        default:
            bpp = 4;
            break;
    }
    UINT pitch = std::max(1U, desc.Width) * bpp;
    size_t size = (size_t)pitch * std::max(1U, desc.Height);
    m_data.resize(size, 0);
}

HRESULT WINAPI MockDirect3DSurface9::QueryInterface(const void *riid, void **ppvObject) {
    (void)riid;
    if (!ppvObject) return D3DERR_INVALIDCALL;
    *ppvObject = this;
    AddRef();
    return D3D_OK;
}

ULONG WINAPI MockDirect3DSurface9::AddRef(void) {
    return ++m_refCount;
}

ULONG WINAPI MockDirect3DSurface9::Release(void) {
    ULONG count = --m_refCount;
    if (count == 0) {
        delete this;
    }
    return count;
}

HRESULT WINAPI MockDirect3DSurface9::GetDevice(IDirect3DDevice9 **ppDevice) {
    if (!ppDevice) return D3DERR_INVALIDCALL;
    *ppDevice = (IDirect3DDevice9*)m_pDevice;
    if (m_pDevice) m_pDevice->AddRef();
    return D3D_OK;
}

HRESULT WINAPI MockDirect3DSurface9::SetPrivateData(const void *guid, const void *pData, DWORD SizeOfData, DWORD Flags) {
    (void)guid; (void)pData; (void)SizeOfData; (void)Flags;
    return D3D_OK;
}

HRESULT WINAPI MockDirect3DSurface9::GetPrivateData(const void *guid, void *pData, DWORD *pSizeOfData) {
    (void)guid; (void)pData; (void)pSizeOfData;
    return D3D_OK;
}

HRESULT WINAPI MockDirect3DSurface9::FreePrivateData(const void *guid) {
    (void)guid;
    return D3D_OK;
}

DWORD WINAPI MockDirect3DSurface9::SetPriority(DWORD PriorityNew) {
    return PriorityNew;
}

DWORD WINAPI MockDirect3DSurface9::GetPriority(void) {
    return 0;
}

void WINAPI MockDirect3DSurface9::PreLoad(void) {}

D3DRESOURCETYPE WINAPI MockDirect3DSurface9::GetType(void) {
    return D3DRTYPE_SURFACE;
}

HRESULT WINAPI MockDirect3DSurface9::GetContainer(const void *riid, void **ppContainer) {
    (void)riid;
    if (!ppContainer) return D3DERR_INVALIDCALL;
    *ppContainer = nullptr;
    return D3D_OK;
}

HRESULT WINAPI MockDirect3DSurface9::GetDesc(D3DSURFACE_DESC *pDesc) {
    if (!pDesc) return D3DERR_INVALIDCALL;
    *pDesc = m_desc;
    return D3D_OK;
}

HRESULT WINAPI MockDirect3DSurface9::LockRect(D3DLOCKED_RECT *pLockedRect, const RECT *pRect, DWORD Flags) {
    (void)Flags;
    if (!pLockedRect) return D3DERR_INVALIDCALL;
    UINT bpp = 4;
    switch (m_desc.Format) {
        case D3DFMT_R5G6B5:
        case D3DFMT_X1R5G5B5:
        case D3DFMT_A1R5G5B5:
        case D3DFMT_A4R4G4B4:
        case D3DFMT_D16:
        case D3DFMT_L16:
        case D3DFMT_V8U8:
            bpp = 2;
            break;
        case D3DFMT_L8:
        case D3DFMT_A8:
            bpp = 1;
            break;
        case D3DFMT_A16B16G16R16F:
        case D3DFMT_A16B16G16R16:
            bpp = 8;
            break;
        case D3DFMT_A32B32G32R32F:
            bpp = 16;
            break;
        default:
            bpp = 4;
            break;
    }
    UINT pitch = std::max(1U, m_desc.Width) * bpp;
    pLockedRect->Pitch = (INT)pitch;
    size_t offset = 0;
    if (pRect) {
        offset = (size_t)pRect->top * pitch + (size_t)pRect->left * bpp;
    }
    if (offset < m_data.size()) {
        pLockedRect->pBits = m_data.data() + offset;
    } else {
        pLockedRect->pBits = m_data.data();
    }
    return D3D_OK;
}

HRESULT WINAPI MockDirect3DSurface9::UnlockRect(void) {
    return D3D_OK;
}

HRESULT WINAPI MockDirect3DSurface9::GetDC(HDC *phdc) {
    (void)phdc;
    return D3D_OK;
}

HRESULT WINAPI MockDirect3DSurface9::ReleaseDC(HDC hdc) {
    (void)hdc;
    return D3D_OK;
}

/* ================= MockDirect3DTexture9 ================= */

MockDirect3DTexture9::MockDirect3DTexture9(MockDirect3DDevice9 *pDevice, UINT width, UINT height, UINT levels, DWORD usage, D3DFORMAT format, D3DPOOL pool)
    : m_pDevice(pDevice), m_width(width), m_height(height), m_levels(levels ? levels : 1), m_usage(usage), m_format(format), m_pool(pool) {
    UINT w = width;
    UINT h = height;
    for (UINT i = 0; i < m_levels; ++i) {
        D3DSURFACE_DESC desc;
        desc.Format = format;
        desc.Type = D3DRTYPE_SURFACE;
        desc.Usage = usage;
        desc.Pool = pool;
        desc.MultiSampleType = D3DMULTISAMPLE_NONE;
        desc.MultiSampleQuality = 0;
        desc.Width = std::max(1U, w);
        desc.Height = std::max(1U, h);
        m_surfaces.push_back(new MockDirect3DSurface9(pDevice, desc));
        w >>= 1;
        h >>= 1;
    }
}

MockDirect3DTexture9::~MockDirect3DTexture9() {
    for (auto *s : m_surfaces) {
        s->Release();
    }
}

HRESULT WINAPI MockDirect3DTexture9::QueryInterface(const void *riid, void **ppvObject) {
    (void)riid;
    if (!ppvObject) return D3DERR_INVALIDCALL;
    *ppvObject = this;
    AddRef();
    return D3D_OK;
}

ULONG WINAPI MockDirect3DTexture9::AddRef(void) {
    return ++m_refCount;
}

ULONG WINAPI MockDirect3DTexture9::Release(void) {
    ULONG count = --m_refCount;
    if (count == 0) {
        delete this;
    }
    return count;
}

HRESULT WINAPI MockDirect3DTexture9::GetDevice(IDirect3DDevice9 **ppDevice) {
    if (!ppDevice) return D3DERR_INVALIDCALL;
    *ppDevice = (IDirect3DDevice9*)m_pDevice;
    if (m_pDevice) m_pDevice->AddRef();
    return D3D_OK;
}

HRESULT WINAPI MockDirect3DTexture9::SetPrivateData(const void *guid, const void *pData, DWORD SizeOfData, DWORD Flags) {
    (void)guid; (void)pData; (void)SizeOfData; (void)Flags;
    return D3D_OK;
}

HRESULT WINAPI MockDirect3DTexture9::GetPrivateData(const void *guid, void *pData, DWORD *pSizeOfData) {
    (void)guid; (void)pData; (void)pSizeOfData;
    return D3D_OK;
}

HRESULT WINAPI MockDirect3DTexture9::FreePrivateData(const void *guid) {
    (void)guid;
    return D3D_OK;
}

DWORD WINAPI MockDirect3DTexture9::SetPriority(DWORD PriorityNew) {
    return PriorityNew;
}

DWORD WINAPI MockDirect3DTexture9::GetPriority(void) {
    return 0;
}

void WINAPI MockDirect3DTexture9::PreLoad(void) {}

D3DRESOURCETYPE WINAPI MockDirect3DTexture9::GetType(void) {
    return D3DRTYPE_TEXTURE;
}

DWORD WINAPI MockDirect3DTexture9::SetLOD(DWORD LODNew) {
    return LODNew;
}

DWORD WINAPI MockDirect3DTexture9::GetLOD(void) {
    return 0;
}

DWORD WINAPI MockDirect3DTexture9::GetLevelCount(void) {
    return (DWORD)m_surfaces.size();
}

HRESULT WINAPI MockDirect3DTexture9::SetAutoGenFilterType(D3DTEXTUREFILTERTYPE FilterType) {
    (void)FilterType;
    return D3D_OK;
}

D3DTEXTUREFILTERTYPE WINAPI MockDirect3DTexture9::GetAutoGenFilterType(void) {
    return D3DTEXF_NONE;
}

void WINAPI MockDirect3DTexture9::GenerateMipSubLevels(void) {}

HRESULT WINAPI MockDirect3DTexture9::GetLevelDesc(UINT Level, D3DSURFACE_DESC *pDesc) {
    if (Level >= m_surfaces.size()) return D3DERR_INVALIDCALL;
    return m_surfaces[Level]->GetDesc(pDesc);
}

HRESULT WINAPI MockDirect3DTexture9::GetSurfaceLevel(UINT Level, IDirect3DSurface9 **ppSurfaceLevel) {
    if (!ppSurfaceLevel || Level >= m_surfaces.size()) return D3DERR_INVALIDCALL;
    *ppSurfaceLevel = m_surfaces[Level];
    m_surfaces[Level]->AddRef();
    return D3D_OK;
}

HRESULT WINAPI MockDirect3DTexture9::LockRect(UINT Level, D3DLOCKED_RECT *pLockedRect, const RECT *pRect, DWORD Flags) {
    if (Level >= m_surfaces.size()) return D3DERR_INVALIDCALL;
    return m_surfaces[Level]->LockRect(pLockedRect, pRect, Flags);
}

HRESULT WINAPI MockDirect3DTexture9::UnlockRect(UINT Level) {
    if (Level >= m_surfaces.size()) return D3DERR_INVALIDCALL;
    return m_surfaces[Level]->UnlockRect();
}

HRESULT WINAPI MockDirect3DTexture9::AddDirtyRect(const RECT *pDirtyRect) {
    (void)pDirtyRect;
    return D3D_OK;
}

/* ================= MockDirect3DCubeTexture9 ================= */

MockDirect3DCubeTexture9::MockDirect3DCubeTexture9(MockDirect3DDevice9 *pDevice, UINT edgeLength, UINT levels, DWORD usage, D3DFORMAT format, D3DPOOL pool)
    : m_pDevice(pDevice), m_edgeLength(edgeLength), m_levels(levels ? levels : 1), m_usage(usage), m_format(format), m_pool(pool) {
    for (int face = 0; face < 6; ++face) {
        UINT dim = edgeLength;
        for (UINT i = 0; i < m_levels; ++i) {
            D3DSURFACE_DESC desc;
            desc.Format = format;
            desc.Type = D3DRTYPE_SURFACE;
            desc.Usage = usage;
            desc.Pool = pool;
            desc.MultiSampleType = D3DMULTISAMPLE_NONE;
            desc.MultiSampleQuality = 0;
            desc.Width = std::max(1U, dim);
            desc.Height = std::max(1U, dim);
            m_surfaces[face].push_back(new MockDirect3DSurface9(pDevice, desc));
            dim >>= 1;
        }
    }
}

MockDirect3DCubeTexture9::~MockDirect3DCubeTexture9() {
    for (int face = 0; face < 6; ++face) {
        for (auto *s : m_surfaces[face]) {
            s->Release();
        }
    }
}

HRESULT WINAPI MockDirect3DCubeTexture9::QueryInterface(const void *riid, void **ppvObject) {
    (void)riid;
    if (!ppvObject) return D3DERR_INVALIDCALL;
    *ppvObject = this;
    AddRef();
    return D3D_OK;
}

ULONG WINAPI MockDirect3DCubeTexture9::AddRef(void) {
    return ++m_refCount;
}

ULONG WINAPI MockDirect3DCubeTexture9::Release(void) {
    ULONG count = --m_refCount;
    if (count == 0) delete this;
    return count;
}

HRESULT WINAPI MockDirect3DCubeTexture9::GetDevice(IDirect3DDevice9 **ppDevice) {
    if (!ppDevice) return D3DERR_INVALIDCALL;
    *ppDevice = (IDirect3DDevice9*)m_pDevice;
    if (m_pDevice) m_pDevice->AddRef();
    return D3D_OK;
}

HRESULT WINAPI MockDirect3DCubeTexture9::SetPrivateData(const void *guid, const void *pData, DWORD SizeOfData, DWORD Flags) {
    (void)guid; (void)pData; (void)SizeOfData; (void)Flags; return D3D_OK;
}
HRESULT WINAPI MockDirect3DCubeTexture9::GetPrivateData(const void *guid, void *pData, DWORD *pSizeOfData) {
    (void)guid; (void)pData; (void)pSizeOfData; return D3D_OK;
}
HRESULT WINAPI MockDirect3DCubeTexture9::FreePrivateData(const void *guid) {
    (void)guid; return D3D_OK;
}
DWORD WINAPI MockDirect3DCubeTexture9::SetPriority(DWORD PriorityNew) { return PriorityNew; }
DWORD WINAPI MockDirect3DCubeTexture9::GetPriority(void) { return 0; }
void  WINAPI MockDirect3DCubeTexture9::PreLoad(void) {}
D3DRESOURCETYPE WINAPI MockDirect3DCubeTexture9::GetType(void) { return D3DRTYPE_CUBETEXTURE; }
DWORD WINAPI MockDirect3DCubeTexture9::SetLOD(DWORD LODNew) { return LODNew; }
DWORD WINAPI MockDirect3DCubeTexture9::GetLOD(void) { return 0; }
DWORD WINAPI MockDirect3DCubeTexture9::GetLevelCount(void) { return m_levels; }
HRESULT WINAPI MockDirect3DCubeTexture9::SetAutoGenFilterType(D3DTEXTUREFILTERTYPE FilterType) { (void)FilterType; return D3D_OK; }
D3DTEXTUREFILTERTYPE WINAPI MockDirect3DCubeTexture9::GetAutoGenFilterType(void) { return D3DTEXF_NONE; }
void WINAPI MockDirect3DCubeTexture9::GenerateMipSubLevels(void) {}

HRESULT WINAPI MockDirect3DCubeTexture9::GetLevelDesc(UINT Level, D3DSURFACE_DESC *pDesc) {
    if (Level >= m_levels) return D3DERR_INVALIDCALL;
    return m_surfaces[0][Level]->GetDesc(pDesc);
}

HRESULT WINAPI MockDirect3DCubeTexture9::GetCubeMapSurface(D3DCUBEMAP_FACES FaceType, UINT Level, IDirect3DSurface9 **ppCubeMapSurface) {
    if (!ppCubeMapSurface || (UINT)FaceType >= 6 || Level >= m_levels) return D3DERR_INVALIDCALL;
    *ppCubeMapSurface = m_surfaces[(UINT)FaceType][Level];
    m_surfaces[(UINT)FaceType][Level]->AddRef();
    return D3D_OK;
}

HRESULT WINAPI MockDirect3DCubeTexture9::LockRect(D3DCUBEMAP_FACES FaceType, UINT Level, D3DLOCKED_RECT *pLockedRect, const RECT *pRect, DWORD Flags) {
    if ((UINT)FaceType >= 6 || Level >= m_levels) return D3DERR_INVALIDCALL;
    return m_surfaces[(UINT)FaceType][Level]->LockRect(pLockedRect, pRect, Flags);
}

HRESULT WINAPI MockDirect3DCubeTexture9::UnlockRect(D3DCUBEMAP_FACES FaceType, UINT Level) {
    if ((UINT)FaceType >= 6 || Level >= m_levels) return D3DERR_INVALIDCALL;
    return m_surfaces[(UINT)FaceType][Level]->UnlockRect();
}

HRESULT WINAPI MockDirect3DCubeTexture9::AddDirtyRect(D3DCUBEMAP_FACES FaceType, const RECT *pDirtyRect) {
    (void)FaceType; (void)pDirtyRect; return D3D_OK;
}

/* ================= MockDirect3DVolumeTexture9 ================= */

MockDirect3DVolumeTexture9::MockDirect3DVolumeTexture9(MockDirect3DDevice9 *pDevice, UINT width, UINT height, UINT depth, UINT levels, DWORD usage, D3DFORMAT format, D3DPOOL pool)
    : m_pDevice(pDevice), m_width(width), m_height(height), m_depth(depth), m_levels(levels ? levels : 1), m_usage(usage), m_format(format), m_pool(pool) {
    UINT w = width, h = height, d = depth;
    for (UINT i = 0; i < m_levels; ++i) {
        size_t bytes = (size_t)std::max(1U, w) * std::max(1U, h) * std::max(1U, d) * 4;
        m_levelData.emplace_back(bytes, 0);
        w >>= 1; h >>= 1; d >>= 1;
    }
}

HRESULT WINAPI MockDirect3DVolumeTexture9::QueryInterface(const void *riid, void **ppvObject) {
    (void)riid; if (!ppvObject) return D3DERR_INVALIDCALL;
    *ppvObject = this; AddRef(); return D3D_OK;
}
ULONG WINAPI MockDirect3DVolumeTexture9::AddRef(void) { return ++m_refCount; }
ULONG WINAPI MockDirect3DVolumeTexture9::Release(void) {
    ULONG count = --m_refCount; if (count == 0) delete this; return count;
}
HRESULT WINAPI MockDirect3DVolumeTexture9::GetDevice(IDirect3DDevice9 **ppDevice) {
    if (!ppDevice) return D3DERR_INVALIDCALL;
    *ppDevice = (IDirect3DDevice9*)m_pDevice;
    if (m_pDevice) m_pDevice->AddRef();
    return D3D_OK;
}
HRESULT WINAPI MockDirect3DVolumeTexture9::SetPrivateData(const void *guid, const void *pData, DWORD SizeOfData, DWORD Flags) { (void)guid; (void)pData; (void)SizeOfData; (void)Flags; return D3D_OK; }
HRESULT WINAPI MockDirect3DVolumeTexture9::GetPrivateData(const void *guid, void *pData, DWORD *pSizeOfData) { (void)guid; (void)pData; (void)pSizeOfData; return D3D_OK; }
HRESULT WINAPI MockDirect3DVolumeTexture9::FreePrivateData(const void *guid) { (void)guid; return D3D_OK; }
DWORD WINAPI MockDirect3DVolumeTexture9::SetPriority(DWORD PriorityNew) { return PriorityNew; }
DWORD WINAPI MockDirect3DVolumeTexture9::GetPriority(void) { return 0; }
void WINAPI MockDirect3DVolumeTexture9::PreLoad(void) {}
D3DRESOURCETYPE WINAPI MockDirect3DVolumeTexture9::GetType(void) { return D3DRTYPE_VOLUMETEXTURE; }
DWORD WINAPI MockDirect3DVolumeTexture9::SetLOD(DWORD LODNew) { return LODNew; }
DWORD WINAPI MockDirect3DVolumeTexture9::GetLOD(void) { return 0; }
DWORD WINAPI MockDirect3DVolumeTexture9::GetLevelCount(void) { return m_levels; }
HRESULT WINAPI MockDirect3DVolumeTexture9::SetAutoGenFilterType(D3DTEXTUREFILTERTYPE FilterType) { (void)FilterType; return D3D_OK; }
D3DTEXTUREFILTERTYPE WINAPI MockDirect3DVolumeTexture9::GetAutoGenFilterType(void) { return D3DTEXF_NONE; }
void WINAPI MockDirect3DVolumeTexture9::GenerateMipSubLevels(void) {}

HRESULT WINAPI MockDirect3DVolumeTexture9::GetLevelDesc(UINT Level, D3DVOLUME_DESC *pDesc) {
    if (!pDesc || Level >= m_levels) return D3DERR_INVALIDCALL;
    pDesc->Format = m_format;
    pDesc->Type = D3DRTYPE_VOLUME;
    pDesc->Usage = m_usage;
    pDesc->Pool = m_pool;
    pDesc->Width = std::max(1U, m_width >> Level);
    pDesc->Height = std::max(1U, m_height >> Level);
    pDesc->Depth = std::max(1U, m_depth >> Level);
    return D3D_OK;
}

HRESULT WINAPI MockDirect3DVolumeTexture9::GetVolumeLevel(UINT Level, void **ppVolumeLevel) {
    (void)Level; (void)ppVolumeLevel; return D3D_OK;
}

HRESULT WINAPI MockDirect3DVolumeTexture9::LockBox(UINT Level, D3DLOCKED_BOX *pLockedVolumeBox, const D3DBOX *pBox, DWORD Flags) {
    (void)pBox; (void)Flags;
    if (!pLockedVolumeBox || Level >= m_levels) return D3DERR_INVALIDCALL;
    UINT w = std::max(1U, m_width >> Level);
    UINT h = std::max(1U, m_height >> Level);
    pLockedVolumeBox->RowPitch = (INT)(w * 4);
    pLockedVolumeBox->SlicePitch = (INT)(w * h * 4);
    pLockedVolumeBox->pBits = m_levelData[Level].data();
    return D3D_OK;
}

HRESULT WINAPI MockDirect3DVolumeTexture9::UnlockBox(UINT Level) {
    (void)Level; return D3D_OK;
}

HRESULT WINAPI MockDirect3DVolumeTexture9::AddDirtyBox(const D3DBOX *pDirtyBox) {
    (void)pDirtyBox; return D3D_OK;
}

/* ================= MockDirect3DVertexBuffer9 ================= */

MockDirect3DVertexBuffer9::MockDirect3DVertexBuffer9(MockDirect3DDevice9 *pDevice, UINT length, DWORD usage, DWORD fvf, D3DPOOL pool)
    : m_pDevice(pDevice) {
    m_desc.Format = D3DFMT_VERTEXDATA;
    m_desc.Type = D3DRTYPE_VERTEXBUFFER;
    m_desc.Usage = usage;
    m_desc.Pool = pool;
    m_desc.Size = length;
    m_desc.FVF = fvf;
    m_data.resize(length ? length : 16, 0);
}

HRESULT WINAPI MockDirect3DVertexBuffer9::QueryInterface(const void *riid, void **ppvObject) {
    (void)riid; if (!ppvObject) return D3DERR_INVALIDCALL; *ppvObject = this; AddRef(); return D3D_OK;
}
ULONG WINAPI MockDirect3DVertexBuffer9::AddRef(void) { return ++m_refCount; }
ULONG WINAPI MockDirect3DVertexBuffer9::Release(void) {
    ULONG count = --m_refCount; if (count == 0) delete this; return count;
}
HRESULT WINAPI MockDirect3DVertexBuffer9::GetDevice(IDirect3DDevice9 **ppDevice) {
    if (!ppDevice) return D3DERR_INVALIDCALL;
    *ppDevice = (IDirect3DDevice9*)m_pDevice;
    if (m_pDevice) m_pDevice->AddRef();
    return D3D_OK;
}
HRESULT WINAPI MockDirect3DVertexBuffer9::SetPrivateData(const void *guid, const void *pData, DWORD SizeOfData, DWORD Flags) { (void)guid; (void)pData; (void)SizeOfData; (void)Flags; return D3D_OK; }
HRESULT WINAPI MockDirect3DVertexBuffer9::GetPrivateData(const void *guid, void *pData, DWORD *pSizeOfData) { (void)guid; (void)pData; (void)pSizeOfData; return D3D_OK; }
HRESULT WINAPI MockDirect3DVertexBuffer9::FreePrivateData(const void *guid) { (void)guid; return D3D_OK; }
DWORD WINAPI MockDirect3DVertexBuffer9::SetPriority(DWORD PriorityNew) { return PriorityNew; }
DWORD WINAPI MockDirect3DVertexBuffer9::GetPriority(void) { return 0; }
void WINAPI MockDirect3DVertexBuffer9::PreLoad(void) {}
D3DRESOURCETYPE WINAPI MockDirect3DVertexBuffer9::GetType(void) { return D3DRTYPE_VERTEXBUFFER; }

HRESULT WINAPI MockDirect3DVertexBuffer9::Lock(UINT OffsetToLock, UINT SizeToLock, void **ppbData, DWORD Flags) {
    (void)SizeToLock; (void)Flags;
    if (!ppbData) return D3DERR_INVALIDCALL;
    if (OffsetToLock >= m_data.size()) OffsetToLock = 0;
    *ppbData = m_data.data() + OffsetToLock;
    return D3D_OK;
}

HRESULT WINAPI MockDirect3DVertexBuffer9::Unlock(void) {
    return D3D_OK;
}

HRESULT WINAPI MockDirect3DVertexBuffer9::GetDesc(D3DVERTEXBUFFER_DESC *pDesc) {
    if (!pDesc) return D3DERR_INVALIDCALL;
    *pDesc = m_desc;
    return D3D_OK;
}

/* ================= MockDirect3DIndexBuffer9 ================= */

MockDirect3DIndexBuffer9::MockDirect3DIndexBuffer9(MockDirect3DDevice9 *pDevice, UINT length, DWORD usage, D3DFORMAT format, D3DPOOL pool)
    : m_pDevice(pDevice) {
    m_desc.Format = format;
    m_desc.Type = D3DRTYPE_INDEXBUFFER;
    m_desc.Usage = usage;
    m_desc.Pool = pool;
    m_desc.Size = length;
    m_data.resize(length ? length : 16, 0);
}

HRESULT WINAPI MockDirect3DIndexBuffer9::QueryInterface(const void *riid, void **ppvObject) {
    (void)riid; if (!ppvObject) return D3DERR_INVALIDCALL; *ppvObject = this; AddRef(); return D3D_OK;
}
ULONG WINAPI MockDirect3DIndexBuffer9::AddRef(void) { return ++m_refCount; }
ULONG WINAPI MockDirect3DIndexBuffer9::Release(void) {
    ULONG count = --m_refCount; if (count == 0) delete this; return count;
}
HRESULT WINAPI MockDirect3DIndexBuffer9::GetDevice(IDirect3DDevice9 **ppDevice) {
    if (!ppDevice) return D3DERR_INVALIDCALL;
    *ppDevice = (IDirect3DDevice9*)m_pDevice;
    if (m_pDevice) m_pDevice->AddRef();
    return D3D_OK;
}
HRESULT WINAPI MockDirect3DIndexBuffer9::SetPrivateData(const void *guid, const void *pData, DWORD SizeOfData, DWORD Flags) { (void)guid; (void)pData; (void)SizeOfData; (void)Flags; return D3D_OK; }
HRESULT WINAPI MockDirect3DIndexBuffer9::GetPrivateData(const void *guid, void *pData, DWORD *pSizeOfData) { (void)guid; (void)pData; (void)pSizeOfData; return D3D_OK; }
HRESULT WINAPI MockDirect3DIndexBuffer9::FreePrivateData(const void *guid) { (void)guid; return D3D_OK; }
DWORD WINAPI MockDirect3DIndexBuffer9::SetPriority(DWORD PriorityNew) { return PriorityNew; }
DWORD WINAPI MockDirect3DIndexBuffer9::GetPriority(void) { return 0; }
void WINAPI MockDirect3DIndexBuffer9::PreLoad(void) {}
D3DRESOURCETYPE WINAPI MockDirect3DIndexBuffer9::GetType(void) { return D3DRTYPE_INDEXBUFFER; }

HRESULT WINAPI MockDirect3DIndexBuffer9::Lock(UINT OffsetToLock, UINT SizeToLock, void **ppbData, DWORD Flags) {
    (void)SizeToLock; (void)Flags;
    if (!ppbData) return D3DERR_INVALIDCALL;
    if (OffsetToLock >= m_data.size()) OffsetToLock = 0;
    *ppbData = m_data.data() + OffsetToLock;
    return D3D_OK;
}

HRESULT WINAPI MockDirect3DIndexBuffer9::Unlock(void) {
    return D3D_OK;
}

HRESULT WINAPI MockDirect3DIndexBuffer9::GetDesc(D3DINDEXBUFFER_DESC *pDesc) {
    if (!pDesc) return D3DERR_INVALIDCALL;
    *pDesc = m_desc;
    return D3D_OK;
}

/* ================= MockDirect3DVertexDeclaration9 ================= */

MockDirect3DVertexDeclaration9::MockDirect3DVertexDeclaration9(MockDirect3DDevice9 *pDevice, const D3DVERTEXELEMENT9 *pElements)
    : m_pDevice(pDevice) {
    if (pElements) {
        for (size_t i = 0; ; ++i) {
            m_elements.push_back(pElements[i]);
            if (pElements[i].Stream == 0xFF) break;
        }
    }
}

HRESULT WINAPI MockDirect3DVertexDeclaration9::QueryInterface(const void *riid, void **ppvObject) {
    (void)riid; if (!ppvObject) return D3DERR_INVALIDCALL; *ppvObject = this; AddRef(); return D3D_OK;
}
ULONG WINAPI MockDirect3DVertexDeclaration9::AddRef(void) { return ++m_refCount; }
ULONG WINAPI MockDirect3DVertexDeclaration9::Release(void) {
    ULONG count = --m_refCount; if (count == 0) delete this; return count;
}
HRESULT WINAPI MockDirect3DVertexDeclaration9::GetDevice(IDirect3DDevice9 **ppDevice) {
    if (!ppDevice) return D3DERR_INVALIDCALL;
    *ppDevice = (IDirect3DDevice9*)m_pDevice;
    if (m_pDevice) m_pDevice->AddRef();
    return D3D_OK;
}

HRESULT WINAPI MockDirect3DVertexDeclaration9::GetDeclaration(D3DVERTEXELEMENT9 *pElement, UINT *pNumElements) {
    if (pNumElements) *pNumElements = (UINT)m_elements.size();
    if (pElement) {
        for (size_t i = 0; i < m_elements.size(); ++i) {
            pElement[i] = m_elements[i];
        }
    }
    return D3D_OK;
}

/* ================= MockDirect3DVertexShader9 ================= */

MockDirect3DVertexShader9::MockDirect3DVertexShader9(MockDirect3DDevice9 *pDevice, const DWORD *pFunction)
    : m_pDevice(pDevice) {
    if (pFunction) {
        /* Copy up to end token 0x0000FFFF */
        for (size_t i = 0; i < 4096; ++i) {
            m_function.push_back(pFunction[i]);
            if (pFunction[i] == 0x0000FFFF) break;
        }
    }
}

HRESULT WINAPI MockDirect3DVertexShader9::QueryInterface(const void *riid, void **ppvObject) {
    (void)riid; if (!ppvObject) return D3DERR_INVALIDCALL; *ppvObject = this; AddRef(); return D3D_OK;
}
ULONG WINAPI MockDirect3DVertexShader9::AddRef(void) { return ++m_refCount; }
ULONG WINAPI MockDirect3DVertexShader9::Release(void) {
    ULONG count = --m_refCount; if (count == 0) delete this; return count;
}
HRESULT WINAPI MockDirect3DVertexShader9::GetDevice(IDirect3DDevice9 **ppDevice) {
    if (!ppDevice) return D3DERR_INVALIDCALL;
    *ppDevice = (IDirect3DDevice9*)m_pDevice;
    if (m_pDevice) m_pDevice->AddRef();
    return D3D_OK;
}
HRESULT WINAPI MockDirect3DVertexShader9::GetFunction(void *pData, UINT *pSizeOfData) {
    if (pSizeOfData) *pSizeOfData = (UINT)(m_function.size() * sizeof(DWORD));
    if (pData && !m_function.empty()) {
        memcpy(pData, m_function.data(), m_function.size() * sizeof(DWORD));
    }
    return D3D_OK;
}

/* ================= MockDirect3DPixelShader9 ================= */

MockDirect3DPixelShader9::MockDirect3DPixelShader9(MockDirect3DDevice9 *pDevice, const DWORD *pFunction)
    : m_pDevice(pDevice) {
    if (pFunction) {
        for (size_t i = 0; i < 4096; ++i) {
            m_function.push_back(pFunction[i]);
            if (pFunction[i] == 0x0000FFFF) break;
        }
    }
}

HRESULT WINAPI MockDirect3DPixelShader9::QueryInterface(const void *riid, void **ppvObject) {
    (void)riid; if (!ppvObject) return D3DERR_INVALIDCALL; *ppvObject = this; AddRef(); return D3D_OK;
}
ULONG WINAPI MockDirect3DPixelShader9::AddRef(void) { return ++m_refCount; }
ULONG WINAPI MockDirect3DPixelShader9::Release(void) {
    ULONG count = --m_refCount; if (count == 0) delete this; return count;
}
HRESULT WINAPI MockDirect3DPixelShader9::GetDevice(IDirect3DDevice9 **ppDevice) {
    if (!ppDevice) return D3DERR_INVALIDCALL;
    *ppDevice = (IDirect3DDevice9*)m_pDevice;
    if (m_pDevice) m_pDevice->AddRef();
    return D3D_OK;
}
HRESULT WINAPI MockDirect3DPixelShader9::GetFunction(void *pData, UINT *pSizeOfData) {
    if (pSizeOfData) *pSizeOfData = (UINT)(m_function.size() * sizeof(DWORD));
    if (pData && !m_function.empty()) {
        memcpy(pData, m_function.data(), m_function.size() * sizeof(DWORD));
    }
    return D3D_OK;
}

/* ================= MockDirect3DQuery9 ================= */

MockDirect3DQuery9::MockDirect3DQuery9(MockDirect3DDevice9 *pDevice, D3DQUERYTYPE type)
    : m_pDevice(pDevice), m_type(type) {}

HRESULT WINAPI MockDirect3DQuery9::QueryInterface(const void *riid, void **ppvObject) {
    (void)riid; if (!ppvObject) return D3DERR_INVALIDCALL; *ppvObject = this; AddRef(); return D3D_OK;
}
ULONG WINAPI MockDirect3DQuery9::AddRef(void) { return ++m_refCount; }
ULONG WINAPI MockDirect3DQuery9::Release(void) {
    ULONG count = --m_refCount; if (count == 0) delete this; return count;
}
HRESULT WINAPI MockDirect3DQuery9::GetDevice(IDirect3DDevice9 **ppDevice) {
    if (!ppDevice) return D3DERR_INVALIDCALL;
    *ppDevice = (IDirect3DDevice9*)m_pDevice;
    if (m_pDevice) m_pDevice->AddRef();
    return D3D_OK;
}
D3DQUERYTYPE WINAPI MockDirect3DQuery9::GetType(void) { return m_type; }
DWORD WINAPI MockDirect3DQuery9::GetDataSize(void) {
    if (m_type == D3DQUERYTYPE_OCCLUSION) return sizeof(DWORD);
    if (m_type == D3DQUERYTYPE_EVENT) return sizeof(BOOL);
    return sizeof(DWORD);
}
HRESULT WINAPI MockDirect3DQuery9::Issue(DWORD dwIssueFlags) {
    if (dwIssueFlags & D3DISSUE_BEGIN) m_issuedBegin = true;
    if (dwIssueFlags & D3DISSUE_END) m_issuedEnd = true;
    return D3D_OK;
}
HRESULT WINAPI MockDirect3DQuery9::GetData(void *pData, DWORD dwSize, DWORD dwGetDataFlags) {
    (void)dwGetDataFlags;
    if (pData && dwSize >= sizeof(DWORD)) {
        if (m_type == D3DQUERYTYPE_OCCLUSION) {
            *(DWORD*)pData = 256; /* Mock 256 visible pixels */
        } else if (m_type == D3DQUERYTYPE_EVENT) {
            *(BOOL*)pData = TRUE;
        } else {
            *(DWORD*)pData = 0;
        }
    }
    return S_OK;
}

/* ================= MockDirect3DDevice9 ================= */

MockDirect3DDevice9::MockDirect3DDevice9(IDirect3D9 *pD3D)
    : m_pD3D(pD3D) {
    g_pMockD3DDevice9 = this;
    memset(&m_viewport, 0, sizeof(m_viewport));
    m_viewport.Width = 1024;
    m_viewport.Height = 768;
    m_viewport.MaxZ = 1.0f;
    memset(&m_scissorRect, 0, sizeof(m_scissorRect));
    m_scissorRect.right = 1024;
    m_scissorRect.bottom = 768;
    memset(m_vsConstF, 0, sizeof(m_vsConstF));
    memset(m_psConstF, 0, sizeof(m_psConstF));
    memset(m_vsConstI, 0, sizeof(m_vsConstI));
    memset(m_psConstI, 0, sizeof(m_psConstI));
    memset(m_vsConstB, 0, sizeof(m_vsConstB));
    memset(m_psConstB, 0, sizeof(m_psConstB));
    memset(&m_material, 0, sizeof(m_material));

    D3DSURFACE_DESC bbDesc;
    bbDesc.Format = D3DFMT_X8R8G8B8;
    bbDesc.Type = D3DRTYPE_SURFACE;
    bbDesc.Usage = D3DUSAGE_RENDERTARGET;
    bbDesc.Pool = D3DPOOL_DEFAULT;
    bbDesc.MultiSampleType = D3DMULTISAMPLE_NONE;
    bbDesc.MultiSampleQuality = 0;
    bbDesc.Width = 1024;
    bbDesc.Height = 768;
    m_pBackBuffer = new MockDirect3DSurface9(this, bbDesc);
}

MockDirect3DDevice9::~MockDirect3DDevice9() {
    if (m_pBackBuffer) m_pBackBuffer->Release();
    if (g_pMockD3DDevice9 == this) g_pMockD3DDevice9 = nullptr;
}

void MockDirect3DDevice9::ClearHistory() {
    m_renderStates.clear();
    m_renderStateHistory.clear();
    m_textureStageStates.clear();
    m_textureStageHistory.clear();
    m_samplerStates.clear();
    m_samplerStateHistory.clear();
    m_transforms.clear();
    m_clearHistory.clear();
    m_drawHistory.clear();
    m_streamSources.clear();
    m_lights.clear();
    m_lightEnables.clear();
    m_clipPlanes.clear();
    memset(&m_material, 0, sizeof(m_material));
}

HRESULT WINAPI MockDirect3DDevice9::QueryInterface(const void *riid, void **ppvObject) {
    (void)riid; if (!ppvObject) return D3DERR_INVALIDCALL; *ppvObject = this; AddRef(); return D3D_OK;
}
ULONG WINAPI MockDirect3DDevice9::AddRef(void) { return ++m_refCount; }
ULONG WINAPI MockDirect3DDevice9::Release(void) {
    ULONG count = --m_refCount; if (count == 0) delete this; return count;
}

HRESULT WINAPI MockDirect3DDevice9::TestCooperativeLevel(void) { return D3D_OK; }
UINT    WINAPI MockDirect3DDevice9::GetAvailableTextureMem(void) { return 1024 * 1024 * 1024; /* 1GB */ }
HRESULT WINAPI MockDirect3DDevice9::EvictManagedResources(void) { return D3D_OK; }
HRESULT WINAPI MockDirect3DDevice9::GetDirect3D(IDirect3D9 **ppD3D9) {
    if (!ppD3D9) return D3DERR_INVALIDCALL;
    *ppD3D9 = m_pD3D;
    if (m_pD3D) m_pD3D->AddRef();
    return D3D_OK;
}
HRESULT WINAPI MockDirect3DDevice9::GetDeviceCaps(D3DCAPS9 *pCaps) {
    if (!m_pD3D) return D3DERR_INVALIDCALL;
    return m_pD3D->GetDeviceCaps(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, pCaps);
}
HRESULT WINAPI MockDirect3DDevice9::GetDisplayMode(UINT iSwapChain, void *pMode) { (void)iSwapChain; (void)pMode; return D3D_OK; }
HRESULT WINAPI MockDirect3DDevice9::GetCreationParameters(void *pParameters) { (void)pParameters; return D3D_OK; }
HRESULT WINAPI MockDirect3DDevice9::SetCursorProperties(UINT XHotSpot, UINT YHotSpot, IDirect3DSurface9 *pCursorBitmap) { (void)XHotSpot; (void)YHotSpot; (void)pCursorBitmap; return D3D_OK; }
void    WINAPI MockDirect3DDevice9::SetCursorPosition(int X, int Y, DWORD Flags) { (void)X; (void)Y; (void)Flags; }
BOOL    WINAPI MockDirect3DDevice9::ShowCursor(BOOL bShow) { return bShow; }
HRESULT WINAPI MockDirect3DDevice9::CreateAdditionalSwapChain(D3DPRESENT_PARAMETERS *pPresentationParameters, void **ppSwapChain) { (void)pPresentationParameters; (void)ppSwapChain; return D3D_OK; }
HRESULT WINAPI MockDirect3DDevice9::GetSwapChain(UINT iSwapChain, void **ppSwapChain) { (void)iSwapChain; (void)ppSwapChain; return D3D_OK; }
UINT    WINAPI MockDirect3DDevice9::GetNumberOfSwapChains(void) { return 1; }
HRESULT WINAPI MockDirect3DDevice9::Reset(D3DPRESENT_PARAMETERS *pPresentationParameters) { (void)pPresentationParameters; return D3D_OK; }
HRESULT WINAPI MockDirect3DDevice9::Present(const RECT *pSourceRect, const RECT *pDestRect, HWND hDestWindowOverride, const void *pDirtyRegion) {
    (void)pSourceRect; (void)pDestRect; (void)hDestWindowOverride; (void)pDirtyRegion; return D3D_OK;
}

HRESULT WINAPI MockDirect3DDevice9::GetBackBuffer(UINT iSwapChain, UINT iBackBuffer, D3DBACKBUFFER_TYPE Type, IDirect3DSurface9 **ppBackBuffer) {
    (void)iSwapChain; (void)iBackBuffer; (void)Type;
    if (!ppBackBuffer) return D3DERR_INVALIDCALL;
    *ppBackBuffer = m_pBackBuffer;
    if (m_pBackBuffer) m_pBackBuffer->AddRef();
    return D3D_OK;
}

HRESULT WINAPI MockDirect3DDevice9::GetRasterStatus(UINT iSwapChain, void *pRasterStatus) { (void)iSwapChain; (void)pRasterStatus; return D3D_OK; }
HRESULT WINAPI MockDirect3DDevice9::SetDialogBoxMode(BOOL bEnableDialogs) { (void)bEnableDialogs; return D3D_OK; }
void    WINAPI MockDirect3DDevice9::SetGammaRamp(UINT iSwapChain, DWORD Flags, const void *pRamp) { (void)iSwapChain; (void)Flags; (void)pRamp; }
void    WINAPI MockDirect3DDevice9::GetGammaRamp(UINT iSwapChain, void *pRamp) { (void)iSwapChain; (void)pRamp; }

HRESULT WINAPI MockDirect3DDevice9::CreateTexture(UINT Width, UINT Height, UINT Levels, DWORD Usage, D3DFORMAT Format, D3DPOOL Pool, IDirect3DTexture9 **ppTexture, HANDLE *pSharedHandle) {
    (void)pSharedHandle;
    if (!ppTexture) return D3DERR_INVALIDCALL;
    *ppTexture = new MockDirect3DTexture9(this, Width, Height, Levels, Usage, Format, Pool);
    return D3D_OK;
}

HRESULT WINAPI MockDirect3DDevice9::CreateVolumeTexture(UINT Width, UINT Height, UINT Depth, UINT Levels, DWORD Usage, D3DFORMAT Format, D3DPOOL Pool, IDirect3DVolumeTexture9 **ppVolumeTexture, HANDLE *pSharedHandle) {
    (void)pSharedHandle;
    if (!ppVolumeTexture) return D3DERR_INVALIDCALL;
    *ppVolumeTexture = new MockDirect3DVolumeTexture9(this, Width, Height, Depth, Levels, Usage, Format, Pool);
    return D3D_OK;
}

HRESULT WINAPI MockDirect3DDevice9::CreateCubeTexture(UINT EdgeLength, UINT Levels, DWORD Usage, D3DFORMAT Format, D3DPOOL Pool, IDirect3DCubeTexture9 **ppCubeTexture, HANDLE *pSharedHandle) {
    (void)pSharedHandle;
    if (!ppCubeTexture) return D3DERR_INVALIDCALL;
    *ppCubeTexture = new MockDirect3DCubeTexture9(this, EdgeLength, Levels, Usage, Format, Pool);
    return D3D_OK;
}

HRESULT WINAPI MockDirect3DDevice9::CreateVertexBuffer(UINT Length, DWORD Usage, DWORD FVF, D3DPOOL Pool, IDirect3DVertexBuffer9 **ppVertexBuffer, HANDLE *pSharedHandle) {
    (void)pSharedHandle;
    if (!ppVertexBuffer) return D3DERR_INVALIDCALL;
    *ppVertexBuffer = new MockDirect3DVertexBuffer9(this, Length, Usage, FVF, Pool);
    return D3D_OK;
}

HRESULT WINAPI MockDirect3DDevice9::CreateIndexBuffer(UINT Length, DWORD Usage, D3DFORMAT Format, D3DPOOL Pool, IDirect3DIndexBuffer9 **ppIndexBuffer, HANDLE *pSharedHandle) {
    (void)pSharedHandle;
    if (!ppIndexBuffer) return D3DERR_INVALIDCALL;
    *ppIndexBuffer = new MockDirect3DIndexBuffer9(this, Length, Usage, Format, Pool);
    return D3D_OK;
}

HRESULT WINAPI MockDirect3DDevice9::CreateRenderTarget(UINT Width, UINT Height, D3DFORMAT Format, D3DMULTISAMPLE_TYPE MultiSample, DWORD MultisampleQuality, BOOL Lockable, IDirect3DSurface9 **ppSurface, HANDLE *pSharedHandle) {
    (void)MultiSample; (void)MultisampleQuality; (void)Lockable; (void)pSharedHandle;
    if (!ppSurface) return D3DERR_INVALIDCALL;
    D3DSURFACE_DESC desc;
    desc.Format = Format;
    desc.Type = D3DRTYPE_SURFACE;
    desc.Usage = D3DUSAGE_RENDERTARGET;
    desc.Pool = D3DPOOL_DEFAULT;
    desc.MultiSampleType = MultiSample;
    desc.MultiSampleQuality = MultisampleQuality;
    desc.Width = Width;
    desc.Height = Height;
    *ppSurface = new MockDirect3DSurface9(this, desc);
    return D3D_OK;
}

HRESULT WINAPI MockDirect3DDevice9::CreateDepthStencilSurface(UINT Width, UINT Height, D3DFORMAT Format, D3DMULTISAMPLE_TYPE MultiSample, DWORD MultisampleQuality, BOOL Discard, IDirect3DSurface9 **ppSurface, HANDLE *pSharedHandle) {
    (void)MultiSample; (void)MultisampleQuality; (void)Discard; (void)pSharedHandle;
    if (!ppSurface) return D3DERR_INVALIDCALL;
    D3DSURFACE_DESC desc;
    desc.Format = Format;
    desc.Type = D3DRTYPE_SURFACE;
    desc.Usage = D3DUSAGE_DEPTHSTENCIL;
    desc.Pool = D3DPOOL_DEFAULT;
    desc.MultiSampleType = MultiSample;
    desc.MultiSampleQuality = MultisampleQuality;
    desc.Width = Width;
    desc.Height = Height;
    *ppSurface = new MockDirect3DSurface9(this, desc);
    return D3D_OK;
}

HRESULT WINAPI MockDirect3DDevice9::UpdateSurface(IDirect3DSurface9 *pSourceSurface, const RECT *pSourceRect, IDirect3DSurface9 *pDestinationSurface, const POINT *pDestPoint) {
    (void)pSourceSurface; (void)pSourceRect; (void)pDestinationSurface; (void)pDestPoint; return D3D_OK;
}

HRESULT WINAPI MockDirect3DDevice9::UpdateTexture(IDirect3DBaseTexture9 *pSourceTexture, IDirect3DBaseTexture9 *pDestinationTexture) {
    (void)pSourceTexture; (void)pDestinationTexture; return D3D_OK;
}

HRESULT WINAPI MockDirect3DDevice9::GetRenderTargetData(IDirect3DSurface9 *pRenderTarget, IDirect3DSurface9 *pDestSurface) {
    (void)pRenderTarget; (void)pDestSurface; return D3D_OK;
}

HRESULT WINAPI MockDirect3DDevice9::GetFrontBufferData(UINT iSwapChain, IDirect3DSurface9 *pDestSurface) {
    (void)iSwapChain; (void)pDestSurface; return D3D_OK;
}

HRESULT WINAPI MockDirect3DDevice9::StretchRect(IDirect3DSurface9 *pSourceSurface, const RECT *pSourceRect, IDirect3DSurface9 *pDestSurface, const RECT *pDestRect, D3DTEXTUREFILTERTYPE Filter) {
    (void)pSourceSurface; (void)pSourceRect; (void)pDestSurface; (void)pDestRect; (void)Filter; return D3D_OK;
}

HRESULT WINAPI MockDirect3DDevice9::ColorFill(IDirect3DSurface9 *pSurface, const RECT *pRect, D3DCOLOR color) {
    (void)pSurface; (void)pRect; (void)color; return D3D_OK;
}

HRESULT WINAPI MockDirect3DDevice9::CreateOffscreenPlainSurface(UINT Width, UINT Height, D3DFORMAT Format, D3DPOOL Pool, IDirect3DSurface9 **ppSurface, HANDLE *pSharedHandle) {
    (void)pSharedHandle;
    if (!ppSurface) return D3DERR_INVALIDCALL;
    D3DSURFACE_DESC desc;
    desc.Format = Format;
    desc.Type = D3DRTYPE_SURFACE;
    desc.Usage = 0;
    desc.Pool = Pool;
    desc.MultiSampleType = D3DMULTISAMPLE_NONE;
    desc.MultiSampleQuality = 0;
    desc.Width = Width;
    desc.Height = Height;
    *ppSurface = new MockDirect3DSurface9(this, desc);
    return D3D_OK;
}

HRESULT WINAPI MockDirect3DDevice9::SetRenderTarget(DWORD RenderTargetIndex, IDirect3DSurface9 *pRenderTarget) {
    (void)RenderTargetIndex;
    m_pCurrentRenderTarget = pRenderTarget;
    return D3D_OK;
}

HRESULT WINAPI MockDirect3DDevice9::GetRenderTarget(DWORD RenderTargetIndex, IDirect3DSurface9 **ppRenderTarget) {
    (void)RenderTargetIndex;
    if (!ppRenderTarget) return D3DERR_INVALIDCALL;
    *ppRenderTarget = m_pCurrentRenderTarget;
    if (m_pCurrentRenderTarget) m_pCurrentRenderTarget->AddRef();
    return D3D_OK;
}

HRESULT WINAPI MockDirect3DDevice9::SetDepthStencilSurface(IDirect3DSurface9 *pNewZStencil) {
    m_pCurrentDepthStencil = pNewZStencil;
    return D3D_OK;
}

HRESULT WINAPI MockDirect3DDevice9::GetDepthStencilSurface(IDirect3DSurface9 **ppZStencilSurface) {
    if (!ppZStencilSurface) return D3DERR_INVALIDCALL;
    *ppZStencilSurface = m_pCurrentDepthStencil;
    if (m_pCurrentDepthStencil) m_pCurrentDepthStencil->AddRef();
    return D3D_OK;
}

HRESULT WINAPI MockDirect3DDevice9::BeginScene(void) { return D3D_OK; }
HRESULT WINAPI MockDirect3DDevice9::EndScene(void) { return D3D_OK; }

HRESULT WINAPI MockDirect3DDevice9::Clear(DWORD Count, const D3DRECT *pRects, DWORD Flags, D3DCOLOR Color, float Z, DWORD Stencil) {
    (void)pRects;
    m_clearHistory.push_back({Count, Flags, Color, Z, Stencil});
    return D3D_OK;
}

HRESULT WINAPI MockDirect3DDevice9::SetTransform(D3DTRANSFORMSTATETYPE State, const D3DMATRIX *pMatrix) {
    if (pMatrix) {
        m_transforms[State] = *pMatrix;
    }
    return D3D_OK;
}

HRESULT WINAPI MockDirect3DDevice9::GetTransform(D3DTRANSFORMSTATETYPE State, D3DMATRIX *pMatrix) {
    if (!pMatrix) return D3DERR_INVALIDCALL;
    auto it = m_transforms.find(State);
    if (it != m_transforms.end()) {
        *pMatrix = it->second;
    } else {
        memset(pMatrix, 0, sizeof(D3DMATRIX));
        pMatrix->_11 = pMatrix->_22 = pMatrix->_33 = pMatrix->_44 = 1.0f;
    }
    return D3D_OK;
}

HRESULT WINAPI MockDirect3DDevice9::MultiplyTransform(D3DTRANSFORMSTATETYPE State, const D3DMATRIX *pMatrix) {
    (void)State; (void)pMatrix; return D3D_OK;
}

HRESULT WINAPI MockDirect3DDevice9::SetViewport(const D3DVIEWPORT9 *pViewport) {
    if (pViewport) m_viewport = *pViewport;
    return D3D_OK;
}

HRESULT WINAPI MockDirect3DDevice9::GetViewport(D3DVIEWPORT9 *pViewport) {
    if (!pViewport) return D3DERR_INVALIDCALL;
    *pViewport = m_viewport;
    return D3D_OK;
}

HRESULT WINAPI MockDirect3DDevice9::SetMaterial(const D3DMATERIAL9 *pMaterial) {
    if (pMaterial) m_material = *pMaterial;
    return D3D_OK;
}
HRESULT WINAPI MockDirect3DDevice9::GetMaterial(D3DMATERIAL9 *pMaterial) {
    if (pMaterial) *pMaterial = m_material;
    return D3D_OK;
}
HRESULT WINAPI MockDirect3DDevice9::SetLight(DWORD Index, const D3DLIGHT9 *pLight) {
    if (pLight) m_lights[Index] = *pLight;
    return D3D_OK;
}
HRESULT WINAPI MockDirect3DDevice9::GetLight(DWORD Index, D3DLIGHT9 *pLight) {
    if (!pLight) return D3DERR_INVALIDCALL;
    auto it = m_lights.find(Index);
    if (it != m_lights.end()) *pLight = it->second;
    return D3D_OK;
}
HRESULT WINAPI MockDirect3DDevice9::LightEnable(DWORD Index, BOOL Enable) {
    m_lightEnables[Index] = Enable;
    return D3D_OK;
}
HRESULT WINAPI MockDirect3DDevice9::GetLightEnable(DWORD Index, BOOL *pEnable) {
    if (!pEnable) return D3DERR_INVALIDCALL;
    auto it = m_lightEnables.find(Index);
    *pEnable = (it != m_lightEnables.end()) ? it->second : FALSE;
    return D3D_OK;
}
HRESULT WINAPI MockDirect3DDevice9::SetClipPlane(DWORD Index, const float *pPlane) {
    if (pPlane) {
        m_clipPlanes[Index] = std::vector<float>(pPlane, pPlane + 4);
    }
    return D3D_OK;
}
HRESULT WINAPI MockDirect3DDevice9::GetClipPlane(DWORD Index, float *pPlane) {
    if (!pPlane) return D3DERR_INVALIDCALL;
    auto it = m_clipPlanes.find(Index);
    if (it != m_clipPlanes.end()) {
        for (int i = 0; i < 4; ++i) pPlane[i] = it->second[i];
    }
    return D3D_OK;
}

HRESULT WINAPI MockDirect3DDevice9::SetRenderState(D3DRENDERSTATETYPE State, DWORD Value) {
    m_renderStates[State] = Value;
    m_renderStateHistory.push_back({State, Value});
    return D3D_OK;
}

HRESULT WINAPI MockDirect3DDevice9::GetRenderState(D3DRENDERSTATETYPE State, DWORD *pValue) {
    if (!pValue) return D3DERR_INVALIDCALL;
    auto it = m_renderStates.find(State);
    if (it != m_renderStates.end()) {
        *pValue = it->second;
    } else {
        *pValue = 0;
    }
    return D3D_OK;
}

HRESULT WINAPI MockDirect3DDevice9::CreateStateBlock(DWORD Type, void **ppSB) { (void)Type; (void)ppSB; return D3D_OK; }
HRESULT WINAPI MockDirect3DDevice9::BeginStateBlock(void) { return D3D_OK; }
HRESULT WINAPI MockDirect3DDevice9::EndStateBlock(void **ppSB) { (void)ppSB; return D3D_OK; }
HRESULT WINAPI MockDirect3DDevice9::SetClipStatus(const void *pClipStatus) { (void)pClipStatus; return D3D_OK; }
HRESULT WINAPI MockDirect3DDevice9::GetClipStatus(void *pClipStatus) { (void)pClipStatus; return D3D_OK; }

HRESULT WINAPI MockDirect3DDevice9::GetTexture(DWORD Stage, IDirect3DBaseTexture9 **ppTexture) {
    if (!ppTexture) return D3DERR_INVALIDCALL;
    auto it = m_textures.find(Stage);
    if (it != m_textures.end()) {
        *ppTexture = it->second;
        if (it->second) it->second->AddRef();
    } else {
        *ppTexture = nullptr;
    }
    return D3D_OK;
}

HRESULT WINAPI MockDirect3DDevice9::SetTexture(DWORD Stage, IDirect3DBaseTexture9 *pTexture) {
    m_textures[Stage] = pTexture;
    return D3D_OK;
}

HRESULT WINAPI MockDirect3DDevice9::GetTextureStageState(DWORD Stage, D3DTEXTURESTAGESTATETYPE Type, DWORD *pValue) {
    if (!pValue) return D3DERR_INVALIDCALL;
    auto it = m_textureStageStates.find({Stage, Type});
    if (it != m_textureStageStates.end()) {
        *pValue = it->second;
    } else {
        *pValue = 0;
    }
    return D3D_OK;
}

HRESULT WINAPI MockDirect3DDevice9::SetTextureStageState(DWORD Stage, D3DTEXTURESTAGESTATETYPE Type, DWORD Value) {
    m_textureStageStates[{Stage, Type}] = Value;
    m_textureStageHistory.push_back({Stage, Type, Value});
    return D3D_OK;
}

HRESULT WINAPI MockDirect3DDevice9::GetSamplerState(DWORD Sampler, D3DSAMPLERSTATETYPE Type, DWORD *pValue) {
    if (!pValue) return D3DERR_INVALIDCALL;
    auto it = m_samplerStates.find({Sampler, Type});
    if (it != m_samplerStates.end()) {
        *pValue = it->second;
    } else {
        *pValue = 0;
    }
    return D3D_OK;
}

HRESULT WINAPI MockDirect3DDevice9::SetSamplerState(DWORD Sampler, D3DSAMPLERSTATETYPE Type, DWORD Value) {
    m_samplerStates[{Sampler, Type}] = Value;
    m_samplerStateHistory.push_back({Sampler, Type, Value});
    return D3D_OK;
}

HRESULT WINAPI MockDirect3DDevice9::ValidateDevice(DWORD *pNumPasses) { if (pNumPasses) *pNumPasses = 1; return D3D_OK; }
HRESULT WINAPI MockDirect3DDevice9::SetPaletteEntries(UINT PaletteNumber, const void *pEntries) { (void)PaletteNumber; (void)pEntries; return D3D_OK; }
HRESULT WINAPI MockDirect3DDevice9::GetPaletteEntries(UINT PaletteNumber, void *pEntries) { (void)PaletteNumber; (void)pEntries; return D3D_OK; }
HRESULT WINAPI MockDirect3DDevice9::SetCurrentTexturePalette(UINT PaletteNumber) { (void)PaletteNumber; return D3D_OK; }
HRESULT WINAPI MockDirect3DDevice9::GetCurrentTexturePalette(UINT *PaletteNumber) { (void)PaletteNumber; return D3D_OK; }

HRESULT WINAPI MockDirect3DDevice9::SetScissorRect(const RECT *pRect) {
    if (pRect) m_scissorRect = *pRect;
    return D3D_OK;
}

HRESULT WINAPI MockDirect3DDevice9::GetScissorRect(RECT *pRect) {
    if (!pRect) return D3DERR_INVALIDCALL;
    *pRect = m_scissorRect;
    return D3D_OK;
}

HRESULT WINAPI MockDirect3DDevice9::SetSoftwareVertexProcessing(BOOL bSoftware) { (void)bSoftware; return D3D_OK; }
BOOL    WINAPI MockDirect3DDevice9::GetSoftwareVertexProcessing(void) { return FALSE; }
HRESULT WINAPI MockDirect3DDevice9::SetNPatchMode(float nSegments) { (void)nSegments; return D3D_OK; }
float   WINAPI MockDirect3DDevice9::GetNPatchMode(void) { return 0.0f; }

HRESULT WINAPI MockDirect3DDevice9::DrawPrimitive(D3DPRIMITIVETYPE PrimitiveType, UINT StartVertex, UINT PrimitiveCount) {
    m_drawHistory.push_back({false, PrimitiveType, 0, 0, 0, StartVertex, PrimitiveCount});
    return D3D_OK;
}

HRESULT WINAPI MockDirect3DDevice9::DrawIndexedPrimitive(D3DPRIMITIVETYPE PrimitiveType, INT BaseVertexIndex, UINT MinVertexIndex, UINT NumVertices, UINT startIndex, UINT primCount) {
    m_drawHistory.push_back({true, PrimitiveType, BaseVertexIndex, MinVertexIndex, NumVertices, startIndex, primCount});
    return D3D_OK;
}

HRESULT WINAPI MockDirect3DDevice9::DrawPrimitiveUP(D3DPRIMITIVETYPE PrimitiveType, UINT PrimitiveCount, const void *pVertexStreamZeroData, UINT VertexStreamZeroStride) {
    (void)pVertexStreamZeroData; (void)VertexStreamZeroStride;
    return DrawPrimitive(PrimitiveType, 0, PrimitiveCount);
}

HRESULT WINAPI MockDirect3DDevice9::DrawIndexedPrimitiveUP(D3DPRIMITIVETYPE PrimitiveType, UINT MinVertexIndex, UINT NumVertices, UINT PrimitiveCount, const void *pIndexData, D3DFORMAT IndexDataFormat, const void *pVertexStreamZeroData, UINT VertexStreamZeroStride) {
    (void)pIndexData; (void)IndexDataFormat; (void)pVertexStreamZeroData; (void)VertexStreamZeroStride;
    return DrawIndexedPrimitive(PrimitiveType, 0, MinVertexIndex, NumVertices, 0, PrimitiveCount);
}

HRESULT WINAPI MockDirect3DDevice9::ProcessVertices(UINT SrcStartIndex, UINT DestIndex, UINT VertexCount, IDirect3DVertexBuffer9 *pDestBuffer, IDirect3DVertexDeclaration9 *pVertexDecl, DWORD Flags) {
    (void)SrcStartIndex; (void)DestIndex; (void)VertexCount; (void)pDestBuffer; (void)pVertexDecl; (void)Flags; return D3D_OK;
}

HRESULT WINAPI MockDirect3DDevice9::CreateVertexDeclaration(const D3DVERTEXELEMENT9 *pVertexElements, IDirect3DVertexDeclaration9 **ppDecl) {
    if (!ppDecl) return D3DERR_INVALIDCALL;
    *ppDecl = new MockDirect3DVertexDeclaration9(this, pVertexElements);
    return D3D_OK;
}

HRESULT WINAPI MockDirect3DDevice9::SetVertexDeclaration(IDirect3DVertexDeclaration9 *pDecl) {
    m_pCurrentVertexDecl = pDecl;
    return D3D_OK;
}

HRESULT WINAPI MockDirect3DDevice9::GetVertexDeclaration(IDirect3DVertexDeclaration9 **ppDecl) {
    if (!ppDecl) return D3DERR_INVALIDCALL;
    *ppDecl = m_pCurrentVertexDecl;
    if (m_pCurrentVertexDecl) m_pCurrentVertexDecl->AddRef();
    return D3D_OK;
}

HRESULT WINAPI MockDirect3DDevice9::SetFVF(DWORD FVF) { (void)FVF; return D3D_OK; }
HRESULT WINAPI MockDirect3DDevice9::GetFVF(DWORD *pFVF) { if (pFVF) *pFVF = 0; return D3D_OK; }

HRESULT WINAPI MockDirect3DDevice9::CreateVertexShader(const DWORD *pFunction, IDirect3DVertexShader9 **ppShader) {
    if (!ppShader) return D3DERR_INVALIDCALL;
    *ppShader = new MockDirect3DVertexShader9(this, pFunction);
    return D3D_OK;
}

HRESULT WINAPI MockDirect3DDevice9::SetVertexShader(IDirect3DVertexShader9 *pShader) {
    m_pCurrentVertexShader = pShader;
    return D3D_OK;
}

HRESULT WINAPI MockDirect3DDevice9::GetVertexShader(IDirect3DVertexShader9 **ppShader) {
    if (!ppShader) return D3DERR_INVALIDCALL;
    *ppShader = m_pCurrentVertexShader;
    if (m_pCurrentVertexShader) m_pCurrentVertexShader->AddRef();
    return D3D_OK;
}

HRESULT WINAPI MockDirect3DDevice9::SetVertexShaderConstantF(UINT StartRegister, const float *pConstantData, UINT Vector4fCount) {
    if (pConstantData && StartRegister + Vector4fCount <= 256) {
        memcpy(&m_vsConstF[StartRegister * 4], pConstantData, Vector4fCount * 4 * sizeof(float));
    }
    return D3D_OK;
}

HRESULT WINAPI MockDirect3DDevice9::GetVertexShaderConstantF(UINT StartRegister, float *pConstantData, UINT Vector4fCount) {
    if (pConstantData && StartRegister + Vector4fCount <= 256) {
        memcpy(pConstantData, &m_vsConstF[StartRegister * 4], Vector4fCount * 4 * sizeof(float));
    }
    return D3D_OK;
}

HRESULT WINAPI MockDirect3DDevice9::SetVertexShaderConstantI(UINT StartRegister, const int *pConstantData, UINT Vector4iCount) {
    if (pConstantData && StartRegister + Vector4iCount <= 16) {
        memcpy(&m_vsConstI[StartRegister * 4], pConstantData, Vector4iCount * 4 * sizeof(int));
    }
    return D3D_OK;
}
HRESULT WINAPI MockDirect3DDevice9::GetVertexShaderConstantI(UINT StartRegister, int *pConstantData, UINT Vector4iCount) {
    if (pConstantData && StartRegister + Vector4iCount <= 16) {
        memcpy(pConstantData, &m_vsConstI[StartRegister * 4], Vector4iCount * 4 * sizeof(int));
    }
    return D3D_OK;
}
HRESULT WINAPI MockDirect3DDevice9::SetVertexShaderConstantB(UINT StartRegister, const BOOL *pConstantData, UINT BoolCount) {
    if (pConstantData && StartRegister + BoolCount <= 16) {
        memcpy(&m_vsConstB[StartRegister], pConstantData, BoolCount * sizeof(BOOL));
    }
    return D3D_OK;
}
HRESULT WINAPI MockDirect3DDevice9::GetVertexShaderConstantB(UINT StartRegister, BOOL *pConstantData, UINT BoolCount) {
    if (pConstantData && StartRegister + BoolCount <= 16) {
        memcpy(pConstantData, &m_vsConstB[StartRegister], BoolCount * sizeof(BOOL));
    }
    return D3D_OK;
}

HRESULT WINAPI MockDirect3DDevice9::SetStreamSource(UINT StreamNumber, IDirect3DVertexBuffer9 *pStreamData, UINT OffsetInBytes, UINT Stride) {
    m_streamSources[StreamNumber].streamNumber = StreamNumber;
    m_streamSources[StreamNumber].pStreamData = pStreamData;
    m_streamSources[StreamNumber].offsetInBytes = OffsetInBytes;
    m_streamSources[StreamNumber].stride = Stride;
    return D3D_OK;
}

HRESULT WINAPI MockDirect3DDevice9::GetStreamSource(UINT StreamNumber, IDirect3DVertexBuffer9 **ppStreamData, UINT *pOffsetInBytes, UINT *pStride) {
    auto it = m_streamSources.find(StreamNumber);
    if (it != m_streamSources.end()) {
        if (ppStreamData) *ppStreamData = it->second.pStreamData;
        if (pOffsetInBytes) *pOffsetInBytes = it->second.offsetInBytes;
        if (pStride) *pStride = it->second.stride;
    }
    return D3D_OK;
}

HRESULT WINAPI MockDirect3DDevice9::SetStreamSourceFreq(UINT StreamNumber, UINT Setting) {
    m_streamSources[StreamNumber].frequency = Setting;
    return D3D_OK;
}

HRESULT WINAPI MockDirect3DDevice9::GetStreamSourceFreq(UINT StreamNumber, UINT *pSetting) {
    auto it = m_streamSources.find(StreamNumber);
    if (pSetting && it != m_streamSources.end()) {
        *pSetting = it->second.frequency;
    }
    return D3D_OK;
}

HRESULT WINAPI MockDirect3DDevice9::SetIndices(IDirect3DIndexBuffer9 *pIndexData) { (void)pIndexData; return D3D_OK; }
HRESULT WINAPI MockDirect3DDevice9::GetIndices(IDirect3DIndexBuffer9 **ppIndexData) { (void)ppIndexData; return D3D_OK; }

HRESULT WINAPI MockDirect3DDevice9::CreatePixelShader(const DWORD *pFunction, IDirect3DPixelShader9 **ppShader) {
    if (!ppShader) return D3DERR_INVALIDCALL;
    *ppShader = new MockDirect3DPixelShader9(this, pFunction);
    return D3D_OK;
}

HRESULT WINAPI MockDirect3DDevice9::SetPixelShader(IDirect3DPixelShader9 *pShader) {
    m_pCurrentPixelShader = pShader;
    return D3D_OK;
}

HRESULT WINAPI MockDirect3DDevice9::GetPixelShader(IDirect3DPixelShader9 **ppShader) {
    if (!ppShader) return D3DERR_INVALIDCALL;
    *ppShader = m_pCurrentPixelShader;
    if (m_pCurrentPixelShader) m_pCurrentPixelShader->AddRef();
    return D3D_OK;
}

HRESULT WINAPI MockDirect3DDevice9::SetPixelShaderConstantF(UINT StartRegister, const float *pConstantData, UINT Vector4fCount) {
    if (pConstantData && StartRegister + Vector4fCount <= 224) {
        memcpy(&m_psConstF[StartRegister * 4], pConstantData, Vector4fCount * 4 * sizeof(float));
    }
    return D3D_OK;
}

HRESULT WINAPI MockDirect3DDevice9::GetPixelShaderConstantF(UINT StartRegister, float *pConstantData, UINT Vector4fCount) {
    if (pConstantData && StartRegister + Vector4fCount <= 224) {
        memcpy(pConstantData, &m_psConstF[StartRegister * 4], Vector4fCount * 4 * sizeof(float));
    }
    return D3D_OK;
}

HRESULT WINAPI MockDirect3DDevice9::SetPixelShaderConstantI(UINT StartRegister, const int *pConstantData, UINT Vector4iCount) {
    if (pConstantData && StartRegister + Vector4iCount <= 16) {
        memcpy(&m_psConstI[StartRegister * 4], pConstantData, Vector4iCount * 4 * sizeof(int));
    }
    return D3D_OK;
}
HRESULT WINAPI MockDirect3DDevice9::GetPixelShaderConstantI(UINT StartRegister, int *pConstantData, UINT Vector4iCount) {
    if (pConstantData && StartRegister + Vector4iCount <= 16) {
        memcpy(pConstantData, &m_psConstI[StartRegister * 4], Vector4iCount * 4 * sizeof(int));
    }
    return D3D_OK;
}
HRESULT WINAPI MockDirect3DDevice9::SetPixelShaderConstantB(UINT StartRegister, const BOOL *pConstantData, UINT BoolCount) {
    if (pConstantData && StartRegister + BoolCount <= 16) {
        memcpy(&m_psConstB[StartRegister], pConstantData, BoolCount * sizeof(BOOL));
    }
    return D3D_OK;
}
HRESULT WINAPI MockDirect3DDevice9::GetPixelShaderConstantB(UINT StartRegister, BOOL *pConstantData, UINT BoolCount) {
    if (pConstantData && StartRegister + BoolCount <= 16) {
        memcpy(pConstantData, &m_psConstB[StartRegister], BoolCount * sizeof(BOOL));
    }
    return D3D_OK;
}

HRESULT WINAPI MockDirect3DDevice9::DrawRectPatch(UINT Handle, const float *pNumSegs, const void *pRectPatchInfo) {
    (void)Handle; (void)pNumSegs; (void)pRectPatchInfo; return D3D_OK;
}
HRESULT WINAPI MockDirect3DDevice9::DrawTriPatch(UINT Handle, const float *pNumSegs, const void *pTriPatchInfo) {
    (void)Handle; (void)pNumSegs; (void)pTriPatchInfo; return D3D_OK;
}
HRESULT WINAPI MockDirect3DDevice9::DeletePatch(UINT Handle) { (void)Handle; return D3D_OK; }

HRESULT WINAPI MockDirect3DDevice9::CreateQuery(D3DQUERYTYPE Type, IDirect3DQuery9 **ppQuery) {
    if (!ppQuery) return D3DERR_INVALIDCALL;
    *ppQuery = new MockDirect3DQuery9(this, Type);
    return D3D_OK;
}

/* IDirect3DDevice9Ex stubs */
HRESULT WINAPI MockDirect3DDevice9::SetConvolutionMonoKernel(UINT width, UINT height, float *rows, float *columns) {
    (void)width; (void)height; (void)rows; (void)columns; return D3D_OK;
}
HRESULT WINAPI MockDirect3DDevice9::ComposeRects(IDirect3DSurface9 *pSrc, IDirect3DSurface9 *pDst, IDirect3DVertexBuffer9 *pSrcRectDescs, UINT NumRects, IDirect3DVertexBuffer9 *pDstRectDescs, DWORD Operation, int Xoffset, int Yoffset) {
    (void)pSrc; (void)pDst; (void)pSrcRectDescs; (void)NumRects; (void)pDstRectDescs; (void)Operation; (void)Xoffset; (void)Yoffset; return D3D_OK;
}
HRESULT WINAPI MockDirect3DDevice9::PresentEx(const RECT *pSourceRect, const RECT *pDestRect, HWND hDestWindowOverride, const void *pDirtyRegion, DWORD dwFlags) {
    (void)pSourceRect; (void)pDestRect; (void)hDestWindowOverride; (void)pDirtyRegion; (void)dwFlags; return D3D_OK;
}
HRESULT WINAPI MockDirect3DDevice9::GetGPUThreadPriority(INT *pPriority) { if (pPriority) *pPriority = 0; return D3D_OK; }
HRESULT WINAPI MockDirect3DDevice9::SetGPUThreadPriority(INT Priority) { (void)Priority; return D3D_OK; }
HRESULT WINAPI MockDirect3DDevice9::WaitForVBlank(UINT iSwapChain) { (void)iSwapChain; return D3D_OK; }
HRESULT WINAPI MockDirect3DDevice9::CheckResourceResidency(IDirect3DResource9 **pResourceArray, UINT NumResources) {
    (void)pResourceArray; (void)NumResources; return S_OK;
}
HRESULT WINAPI MockDirect3DDevice9::SetMaximumFrameLatency(UINT MaxLatency) { (void)MaxLatency; return D3D_OK; }
HRESULT WINAPI MockDirect3DDevice9::GetMaximumFrameLatency(UINT *pMaxLatency) { if (pMaxLatency) *pMaxLatency = 1; return D3D_OK; }
HRESULT WINAPI MockDirect3DDevice9::CheckDeviceState(HWND hDestinationWindow) { (void)hDestinationWindow; return S_OK; }
HRESULT WINAPI MockDirect3DDevice9::CreateRenderTargetEx(UINT Width, UINT Height, D3DFORMAT Format, D3DMULTISAMPLE_TYPE MultiSample, DWORD MultisampleQuality, BOOL Lockable, IDirect3DSurface9 **ppSurface, HANDLE *pSharedHandle, DWORD Usage) {
    (void)Usage;
    return CreateRenderTarget(Width, Height, Format, MultiSample, MultisampleQuality, Lockable, ppSurface, pSharedHandle);
}
HRESULT WINAPI MockDirect3DDevice9::CreateOffscreenPlainSurfaceEx(UINT Width, UINT Height, D3DFORMAT Format, D3DPOOL Pool, IDirect3DSurface9 **ppSurface, HANDLE *pSharedHandle, DWORD Usage) {
    (void)Usage;
    return CreateOffscreenPlainSurface(Width, Height, Format, Pool, ppSurface, pSharedHandle);
}
HRESULT WINAPI MockDirect3DDevice9::CreateDepthStencilSurfaceEx(UINT Width, UINT Height, D3DFORMAT Format, D3DMULTISAMPLE_TYPE MultiSample, DWORD MultisampleQuality, BOOL Discard, IDirect3DSurface9 **ppSurface, HANDLE *pSharedHandle, DWORD Usage) {
    (void)Usage;
    return CreateDepthStencilSurface(Width, Height, Format, MultiSample, MultisampleQuality, Discard, ppSurface, pSharedHandle);
}
HRESULT WINAPI MockDirect3DDevice9::ResetEx(D3DPRESENT_PARAMETERS *pPresentationParameters, void *pFullscreenDisplayMode) {
    (void)pFullscreenDisplayMode; return Reset(pPresentationParameters);
}
HRESULT WINAPI MockDirect3DDevice9::GetDisplayModeEx(UINT iSwapChain, void *pMode, void *pRotation) {
    (void)iSwapChain; (void)pMode; (void)pRotation; return D3D_OK;
}

/* ================= MockDirect3D9 ================= */

HRESULT WINAPI MockDirect3D9::QueryInterface(const void *riid, void **ppvObject) {
    (void)riid; if (!ppvObject) return D3DERR_INVALIDCALL; *ppvObject = this; AddRef(); return D3D_OK;
}
ULONG WINAPI MockDirect3D9::AddRef(void) { return ++m_refCount; }
ULONG WINAPI MockDirect3D9::Release(void) {
    ULONG count = --m_refCount; if (count == 0) delete this; return count;
}

HRESULT WINAPI MockDirect3D9::RegisterSoftwareDevice(void *pInitializeFunction) { (void)pInitializeFunction; return D3D_OK; }
UINT    WINAPI MockDirect3D9::GetAdapterCount(void) { return 1; }
HRESULT WINAPI MockDirect3D9::GetAdapterIdentifier(UINT Adapter, DWORD Flags, void *pIdentifier) { (void)Adapter; (void)Flags; (void)pIdentifier; return D3D_OK; }
UINT    WINAPI MockDirect3D9::GetAdapterModeCount(UINT Adapter, D3DFORMAT Format) { (void)Adapter; (void)Format; return 1; }
HRESULT WINAPI MockDirect3D9::EnumAdapterModes(UINT Adapter, D3DFORMAT Format, UINT Mode, void *pMode) { (void)Adapter; (void)Format; (void)Mode; (void)pMode; return D3D_OK; }
HRESULT WINAPI MockDirect3D9::GetAdapterDisplayMode(UINT Adapter, void *pMode) { (void)Adapter; (void)pMode; return D3D_OK; }
HRESULT WINAPI MockDirect3D9::CheckDeviceType(UINT Adapter, D3DDEVTYPE DevType, D3DFORMAT AdapterFormat, D3DFORMAT BackBufferFormat, BOOL bWindowed) {
    (void)Adapter; (void)DevType; (void)AdapterFormat; (void)BackBufferFormat; (void)bWindowed; return D3D_OK;
}

HRESULT WINAPI MockDirect3D9::CheckDeviceFormat(UINT Adapter, D3DDEVTYPE DeviceType, D3DFORMAT AdapterFormat, DWORD Usage, D3DRESOURCETYPE RType, D3DFORMAT CheckFormat) {
    (void)Adapter; (void)DeviceType; (void)AdapterFormat; (void)Usage; (void)RType; (void)CheckFormat;
    /* Return D3D_OK to claim support for all formats including FOURCC INTZ, NULL, etc. */
    return D3D_OK;
}

HRESULT WINAPI MockDirect3D9::CheckDeviceMultiSampleType(UINT Adapter, D3DDEVTYPE DeviceType, D3DFORMAT SurfaceFormat, BOOL Windowed, D3DMULTISAMPLE_TYPE MultiSampleType, DWORD *pQualityLevels) {
    (void)Adapter; (void)DeviceType; (void)SurfaceFormat; (void)Windowed; (void)MultiSampleType;
    if (pQualityLevels) *pQualityLevels = 1;
    return D3D_OK;
}

HRESULT WINAPI MockDirect3D9::CheckDepthStencilMatch(UINT Adapter, D3DDEVTYPE DeviceType, D3DFORMAT AdapterFormat, D3DFORMAT RenderTargetFormat, D3DFORMAT DepthStencilFormat) {
    (void)Adapter; (void)DeviceType; (void)AdapterFormat; (void)RenderTargetFormat; (void)DepthStencilFormat;
    return D3D_OK;
}

HRESULT WINAPI MockDirect3D9::CheckDeviceFormatConversion(UINT Adapter, D3DDEVTYPE DeviceType, D3DFORMAT SourceFormat, D3DFORMAT TargetFormat) {
    (void)Adapter; (void)DeviceType; (void)SourceFormat; (void)TargetFormat; return D3D_OK;
}

HRESULT WINAPI MockDirect3D9::GetDeviceCaps(UINT Adapter, D3DDEVTYPE DeviceType, D3DCAPS9 *pCaps) {
    (void)Adapter; (void)DeviceType;
    if (!pCaps) return D3DERR_INVALIDCALL;

    memset(pCaps, 0, sizeof(D3DCAPS9));
    pCaps->DeviceType = D3DDEVTYPE_HAL;
    pCaps->AdapterOrdinal = 0;
    pCaps->Caps = 0;
    pCaps->Caps2 = D3DCAPS2_FULLSCREENGAMMA | D3DCAPS2_CANAUTOGENMIPMAP;
    pCaps->Caps3 = 0;
    pCaps->PresentationIntervals = D3DPRESENT_INTERVAL_DEFAULT | D3DPRESENT_INTERVAL_ONE | D3DPRESENT_INTERVAL_IMMEDIATE;
    pCaps->CursorCaps = 0;
    pCaps->DevCaps = D3DDEVCAPS_HWTRANSFORMANDLIGHT | D3DDEVCAPS_PUREDEVICE | D3DDEVCAPS_TEXTURENONLOCALVIDMEM;
    pCaps->PrimitiveMiscCaps = 0;
    pCaps->RasterCaps = 0;
    pCaps->ZCmpCaps = 0xFFFFFFFF;
    pCaps->SrcBlendCaps = 0xFFFFFFFF;
    pCaps->DestBlendCaps = 0xFFFFFFFF;
    pCaps->AlphaCmpCaps = 0xFFFFFFFF;
    pCaps->ShadeCaps = 0xFFFFFFFF;
    pCaps->TextureCaps = D3DPTEXTURECAPS_POW2 | D3DPTEXTURECAPS_NONPOW2CONDITIONAL | D3DPTEXTURECAPS_PROJECTED |
                         D3DPTEXTURECAPS_CUBEMAP | D3DPTEXTURECAPS_VOLUMEMAP | D3DPTEXTURECAPS_MIPMAP;
    pCaps->TextureFilterCaps = D3DPTFILTERCAPS_MINFPOINT | D3DPTFILTERCAPS_MINFLINEAR | D3DPTFILTERCAPS_MINFANISOTROPIC |
                               D3DPTFILTERCAPS_MAGFPOINT | D3DPTFILTERCAPS_MAGFLINEAR | D3DPTFILTERCAPS_MAGFANISOTROPIC |
                               D3DPTFILTERCAPS_MIPFPOINT | D3DPTFILTERCAPS_MIPFLINEAR;
    pCaps->CubeTextureFilterCaps = pCaps->TextureFilterCaps;
    pCaps->VolumeTextureFilterCaps = pCaps->TextureFilterCaps;
    pCaps->TextureAddressCaps = D3DPTADDRESSCAPS_WRAP | D3DPTADDRESSCAPS_MIRROR | D3DPTADDRESSCAPS_CLAMP |
                                D3DPTADDRESSCAPS_BORDER | D3DPTADDRESSCAPS_INDEPENDENTUV | D3DPTADDRESSCAPS_MIRRORONCE;
    pCaps->VolumeTextureAddressCaps = pCaps->TextureAddressCaps;
    pCaps->LineCaps = 0;
    pCaps->MaxTextureWidth = 8192;
    pCaps->MaxTextureHeight = 8192;
    pCaps->MaxVolumeExtent = 2048;
    pCaps->MaxTextureRepeat = 8192;
    pCaps->MaxTextureAspectRatio = 8192;
    pCaps->MaxAnisotropy = 16;
    pCaps->MaxVertexW = 1e10f;
    pCaps->GuardBandLeft = -1e9f;
    pCaps->GuardBandTop = -1e9f;
    pCaps->GuardBandRight = 1e9f;
    pCaps->GuardBandBottom = 1e9f;
    pCaps->ExtentsAdjust = 0.0f;
    pCaps->StencilCaps = 0xFFFFFFFF;
    pCaps->FVFCaps = 0;
    pCaps->TextureOpCaps = 0xFFFFFFFF;
    pCaps->MaxTextureBlendStages = 8;
    pCaps->MaxSimultaneousTextures = 8;
    pCaps->VertexProcessingCaps = 0;
    pCaps->MaxActiveLights = 8;
    pCaps->MaxUserClipPlanes = 6;
    pCaps->MaxVertexBlendMatrices = 4;
    pCaps->MaxVertexBlendMatrixIndex = 4;
    pCaps->MaxPointSize = 64.0f;
    pCaps->MaxPrimitiveCount = 0x555555;
    pCaps->MaxVertexIndex = 0xFFFFFF;
    pCaps->MaxStreams = 16;
    pCaps->MaxStreamStride = 256;
    pCaps->VertexShaderVersion = D3DVS_VERSION(3, 0);
    pCaps->MaxVertexShaderConst = 256;
    pCaps->PixelShaderVersion = D3DPS_VERSION(3, 0);
    pCaps->PixelShader1xMaxValue = 8.0f;
    pCaps->DevCaps2 = D3DDEVCAPS2_STREAMOFFSET;
    pCaps->MaxNpatchTessellationLevel = 1.0f;
    pCaps->Reserved5 = 0;
    pCaps->NumSimultaneousRTs = 4;
    pCaps->ExtentsAdjust = 0.0f;
    pCaps->MaxVertexShader30InstructionSlots = 4096;
    pCaps->MaxPixelShader30InstructionSlots = 4096;

    pCaps->VS20Caps.Caps = 0;
    pCaps->VS20Caps.DynamicFlowControlDepth = 24;
    pCaps->VS20Caps.NumTemps = 32;
    pCaps->VS20Caps.StaticFlowControlDepth = 4;

    pCaps->PS20Caps.Caps = 0;
    pCaps->PS20Caps.DynamicFlowControlDepth = 24;
    pCaps->PS20Caps.NumTemps = 32;
    pCaps->PS20Caps.StaticFlowControlDepth = 4;
    pCaps->PS20Caps.NumInstructionSlots = 512;

    return D3D_OK;
}

HMONITOR WINAPI MockDirect3D9::GetAdapterMonitor(UINT Adapter) { (void)Adapter; return (HMONITOR)1; }

HRESULT WINAPI MockDirect3D9::CreateDevice(UINT Adapter, D3DDEVTYPE DeviceType, HWND hFocusWindow, DWORD BehaviorFlags, D3DPRESENT_PARAMETERS *pPresentationParameters, IDirect3DDevice9 **ppReturnedDeviceInterface) {
    (void)Adapter; (void)DeviceType; (void)hFocusWindow; (void)BehaviorFlags; (void)pPresentationParameters;
    if (!ppReturnedDeviceInterface) return D3DERR_INVALIDCALL;
    MockDirect3DDevice9 *pDev = new MockDirect3DDevice9(this);
    m_pLastCreatedDevice = pDev;
    *ppReturnedDeviceInterface = pDev;
    return D3D_OK;
}

UINT    WINAPI MockDirect3D9::GetAdapterModeCountEx(UINT Adapter, const void *pFilter) { (void)Adapter; (void)pFilter; return 1; }
HRESULT WINAPI MockDirect3D9::EnumAdapterModesEx(UINT Adapter, const void *pFilter, UINT Mode, void *pMode) { (void)Adapter; (void)pFilter; (void)Mode; (void)pMode; return D3D_OK; }
HRESULT WINAPI MockDirect3D9::GetAdapterDisplayModeEx(UINT Adapter, void *pMode, void *pRotation) { (void)Adapter; (void)pMode; (void)pRotation; return D3D_OK; }

HRESULT WINAPI MockDirect3D9::CreateDeviceEx(UINT Adapter, D3DDEVTYPE DeviceType, HWND hFocusWindow, DWORD BehaviorFlags, D3DPRESENT_PARAMETERS *pPresentationParameters, void *pFullscreenDisplayMode, IDirect3DDevice9Ex **ppReturnedDeviceInterface) {
    (void)Adapter; (void)DeviceType; (void)hFocusWindow; (void)BehaviorFlags; (void)pPresentationParameters;
    (void)pFullscreenDisplayMode;
    if (!ppReturnedDeviceInterface) return D3DERR_INVALIDCALL;
    MockDirect3DDevice9 *pDev = new MockDirect3DDevice9(this);
    m_pLastCreatedDevice = pDev;
    *ppReturnedDeviceInterface = pDev;
    return D3D_OK;
}

HRESULT WINAPI MockDirect3D9::GetAdapterLUID(UINT Adapter, void *pLUID) { (void)Adapter; (void)pLUID; return D3D_OK; }
