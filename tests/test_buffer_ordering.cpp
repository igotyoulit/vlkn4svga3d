/* Real Vulkan regression: a queued draw retains vertices after a CPU update. */
#include "svga3_device.h"
#include <cstdio>
#include <vector>
#include <cstring>
#define DST(t,n) (0x800f0000u|(((t)&7)<<28)|(((t)&24)<<8)|(n))
#define SRC(t,n) (0x80e40000u|(((t)&7)<<28)|(((t)&24)<<8)|(n))
int main(){
 Svga3VlknConfig cfg{};cfg.apiVersion=VK_API_VERSION_1_1;cfg.enableValidationLayers=true;
 auto*d=svga3_vlkn_device_create(&cfg);if(!d)return 1;
 svga3_vlkn_context_create(d,1);SVGA3dSize rt{64,64,1};
 svga3_vlkn_surface_define(d,1,SVGA3D_SURFACE_HINT_RENDERTARGET,SVGA3D_A8R8G8B8,&rt,1);
 svga3_vlkn_context_set_render_target(d,1,SVGA3D_RT_COLOR0,1,0,0);
 SVGA3dRect vp{0,0,64,64};svga3_vlkn_context_set_viewport(d,1,&vp);
 svga3_vlkn_context_set_render_state(d,1,SVGA3D_RS_CULLMODE,SVGA3D_FACE_NONE);
 const uint32_t vs[]={0xfffe0300,31|(2<<24),0x80000000,DST(1,0),1|(2<<24),DST(4,0),SRC(1,0),0xffff};
 const uint32_t ps[]={0xffff0300,1|(2<<24),DST(8,0),SRC(2,0),0xffff};
 printf("shader statuses=%d/%d\n",svga3_vlkn_context_define_shader(d,1,1,SVGA3D_SHADERTYPE_VS,vs,sizeof(vs)/4),svga3_vlkn_context_define_shader(d,1,1,SVGA3D_SHADERTYPE_PS,ps,sizeof(ps)/4));
 svga3_vlkn_context_set_shader(d,1,SVGA3D_SHADERTYPE_VS,1);svga3_vlkn_context_set_shader(d,1,SVGA3D_SHADERTYPE_PS,1);
 float red[4]={1,0,0,1};svga3_vlkn_context_set_shader_const(d,1,0,SVGA3D_SHADERTYPE_PS,SVGA3D_CONST_TYPE_FLOAT,reinterpret_cast<uint32_t*>(red));
 float verts[3][4]={{-1,-1,.5,1},{0,-1,.5,1},{-1,1,.5,1}};
 SVGA3dSize vb{sizeof(verts),1,1};svga3_vlkn_surface_define(d,2,SVGA3D_SURFACE_HINT_VERTEXBUFFER,SVGA3D_BUFFER,&vb,1);
 SVGA3dBox box{0,0,0,sizeof(verts),1,1};svga3_vlkn_surface_dma_upload(d,2,0,&box,verts,sizeof(verts));
 svga3_vlkn_context_clear(d,1,SVGA3D_CLEAR_COLOR,0xff000000,1,0,nullptr,0);
 SVGA3dVertexDecl decl{};decl.identity.type=SVGA3D_DECLTYPE_FLOAT4;decl.identity.usage=SVGA3D_DECLUSAGE_POSITION;decl.array.surfaceId=2;decl.array.stride=16;
 SVGA3dPrimitiveRange r{};r.primType=SVGA3D_PRIMITIVE_TRIANGLELIST;r.primitiveCount=1;r.indexArray.surfaceId=SVGA3D_INVALID_ID;
 printf("draw status=%d\n",svga3_vlkn_context_draw(d,1,SVGA3D_PRIMITIVE_TRIANGLELIST,&decl,1,&r,1));
 for(auto &v:verts)v[0]+=1;
 svga3_vlkn_surface_dma_upload(d,2,0,&box,verts,sizeof(verts));
 std::vector<uint32_t> pixels(64*64);svga3_vlkn_surface_dma_download(d,1,0,nullptr,pixels.data(),64*4);
 unsigned left=0,right=0;for(int y=0;y<64;y++)for(int x=0;x<64;x++)if(pixels[y*64+x]&0x00ff0000)(x<32?left:right)++;
 printf("red pixels left=%u right=%u (expected left>0 right=0)\n",left,right);
 bool correct = left > 0 && right == 0;
 SVGA3dSize compressedSize{4,4,1};
 correct &= svga3_vlkn_surface_define(d,3,SVGA3D_SURFACE_HINT_TEXTURE,SVGA3D_DXT1,&compressedSize,1)==SVGA3_VLKN_SUCCESS;
 const uint8_t block[8]={0,0xf8,0xe0,7,0x55,0xaa,0x33,0xcc};
 uint8_t output[8]={};
 correct &= svga3_vlkn_surface_dma_upload(d,3,0,nullptr,block,8)==SVGA3_VLKN_SUCCESS;
 correct &= svga3_vlkn_surface_dma_download(d,3,0,nullptr,output,8)==SVGA3_VLKN_SUCCESS;
 correct &= memcmp(block,output,sizeof(block))==0;
 correct &= svga3_vlkn_surface_define(d,4,SVGA3D_SURFACE_HINT_RENDERTARGET,SVGA3D_DXT1,&compressedSize,1)==SVGA3_VLKN_ERROR_UNSUPPORTED_FORMAT;
 printf("compressed texture block roundtrip and attachment rejection: %s\n",correct ? "PASS" : "FAIL");
 d->contextMgr->clear();d->surfaceMgr->clear();
 correct &= d->backend->waitIdle()==SVGA3_VLKN_SUCCESS;
 d->backend->shutdown();
 correct &= d->backend->validationErrors()==0 && d->backend->validationWarnings()==0;
 svga3_vlkn_device_destroy(d);
 return correct ? 0 : 1;
}
