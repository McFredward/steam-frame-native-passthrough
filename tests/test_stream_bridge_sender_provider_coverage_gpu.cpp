#include "pc/stream-bridge-sender-provider-coverage-render.h"
#include <d3dcompiler.h>
#include <array>
#include <vector>
#include <cstring>
#include <cstdio>
#include <stdexcept>
using namespace stream_bridge::sender;
namespace {
unsigned checks=0;void check(bool b,const char* s){++checks;if(!b)throw std::runtime_error(s);}
template<class T>struct Com{T* p=nullptr;~Com(){if(p)p->Release();}T** out(){return &p;}T* operator->(){return p;}};
constexpr UINT Width=32,Height=16;
const char Shader[]=R"(
cbuffer C:register(b0){float4 value;};
float4 vs_main(uint id:SV_VertexID):SV_Position {float2 p=float2(-1.0,-1.0);if(id==1u)p.y=3.0;if(id==2u)p.x=3.0;return float4(p,0.0,1.0);}
float4 ps_main():SV_Target {return value;}
)";
std::vector<unsigned char> read(ID3D11DeviceContext* c,ID3D11Texture2D* native,ID3D11Texture2D* staging){c->CopyResource(staging,native);D3D11_MAPPED_SUBRESOURCE m{};check(SUCCEEDED(c->Map(staging,0,D3D11_MAP_READ,0,&m)),"actual GPU readback");std::vector<unsigned char> r(Width*Height);for(UINT y=0;y<Height;++y)std::memcpy(r.data()+y*Width,static_cast<const unsigned char*>(m.pData)+y*m.RowPitch,Width);c->Unmap(staging,0);return r;}
void planarTargets(ID3D11Device* device,ID3D11DeviceContext* context){
 for(auto format:{DXGI_FORMAT_NV12,DXGI_FORMAT_P010}){
  D3D11_TEXTURE2D_DESC desc{};desc.Width=Width;desc.Height=Height;desc.MipLevels=desc.ArraySize=desc.SampleDesc.Count=1;
  desc.Format=format;desc.Usage=D3D11_USAGE_DEFAULT;desc.BindFlags=D3D11_BIND_RENDER_TARGET;
  Com<ID3D11Texture2D> texture;
  const auto result=device->CreateTexture2D(&desc,nullptr,texture.out());
  if(FAILED(result)){std::printf("planar target GPU check unavailable: format=%u HRESULT=%08lx\n",unsigned(format),static_cast<unsigned long>(result));continue;}
  D3D11_RENDER_TARGET_VIEW_DESC view{};view.ViewDimension=D3D11_RTV_DIMENSION_TEXTURE2D;
  view.Format=format==DXGI_FORMAT_NV12?DXGI_FORMAT_R8_UNORM:DXGI_FORMAT_R16_UNORM;
  Com<ID3D11RenderTargetView> luma,chroma;check(SUCCEEDED(device->CreateRenderTargetView(texture.p,&view,luma.out())),"actual planar luma RTV");
  view.Format=format==DXGI_FORMAT_NV12?DXGI_FORMAT_R8G8_UNORM:DXGI_FORMAT_R16G16_UNORM;
  check(SUCCEEDED(device->CreateRenderTargetView(texture.p,&view,chroma.out())),"actual planar chroma RTV");
  context->OMSetRenderTargets(1,&luma.p,nullptr);check(nativeLumaTarget(context),"adaptive route recognizes actual planar luma");
  Com<ID3D11RenderTargetView> retained;context->OMGetRenderTargets(1,retained.out(),nullptr);check(retained.p==luma.p,"recognition preserves actual native target");
  context->OMSetRenderTargets(1,&chroma.p,nullptr);check(!nativeLumaTarget(context),"chroma draw cannot count as a second eye or layer");
 }
 context->OMSetRenderTargets(0,nullptr,nullptr);check(!nativeLumaTarget(context),"missing output has no luma authority");
}
void run(){
 Com<ID3D11Device> device;Com<ID3D11DeviceContext> ctx;D3D_FEATURE_LEVEL level{};
 auto result=D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_WARP,nullptr,0,nullptr,0,D3D11_SDK_VERSION,device.out(),&level,ctx.out());
 if(FAILED(result))result=D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_HARDWARE,nullptr,0,nullptr,0,D3D11_SDK_VERSION,device.out(),&level,ctx.out());
 check(SUCCEEDED(result)&&level>=D3D_FEATURE_LEVEL_11_0,"actual D3D11 device");
 planarTargets(device.p,ctx.p);
 Com<ID3DBlob> vsBytes,psBytes;check(SUCCEEDED(D3DCompile(Shader,sizeof Shader,nullptr,nullptr,nullptr,"vs_main","vs_5_0",0,0,vsBytes.out(),nullptr)),"own VS compiled");check(SUCCEEDED(D3DCompile(Shader,sizeof Shader,nullptr,nullptr,nullptr,"ps_main","ps_5_0",0,0,psBytes.out(),nullptr)),"own native-like PS compiled");
 Com<ID3D11VertexShader> vs;Com<ID3D11PixelShader> ps;device->CreateVertexShader(vsBytes->GetBufferPointer(),vsBytes->GetBufferSize(),nullptr,vs.out());device->CreatePixelShader(psBytes->GetBufferPointer(),psBytes->GetBufferSize(),nullptr,ps.out());check(vs.p&&ps.p,"actual shader objects");
 D3D11_TEXTURE2D_DESC td{};td.Width=Width;td.Height=Height;td.MipLevels=td.ArraySize=td.SampleDesc.Count=1;td.Format=DXGI_FORMAT_R8_UNORM;td.Usage=D3D11_USAGE_DEFAULT;td.BindFlags=D3D11_BIND_RENDER_TARGET;
 Com<ID3D11Texture2D> coverage,ordinary,staging;check(SUCCEEDED(device->CreateTexture2D(&td,nullptr,coverage.out()))&&SUCCEEDED(device->CreateTexture2D(&td,nullptr,ordinary.out())),"actual single-channel R8 targets");
 td.Usage=D3D11_USAGE_STAGING;td.BindFlags=0;td.CPUAccessFlags=D3D11_CPU_ACCESS_READ;check(SUCCEEDED(device->CreateTexture2D(&td,nullptr,staging.out())),"actual staging target");
 Com<ID3D11RenderTargetView> target,original;device->CreateRenderTargetView(coverage.p,nullptr,target.out());device->CreateRenderTargetView(ordinary.p,nullptr,original.out());
 Com<ID3D11BlendState> blend;check(createTransmittanceBlend(device.p,blend.out()),"production transmittance blend created");
 D3D11_BUFFER_DESC bd{};bd.ByteWidth=16;bd.Usage=D3D11_USAGE_DEFAULT;bd.BindFlags=D3D11_BIND_CONSTANT_BUFFER;Com<ID3D11Buffer> cb;check(SUCCEEDED(device->CreateBuffer(&bd,nullptr,cb.out())),"fixture constant buffer");
 ctx->VSSetShader(vs.p,nullptr,0);ctx->PSSetShader(ps.p,nullptr,0);ctx->PSSetConstantBuffers(0,1,&cb.p);ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);ctx->OMSetRenderTargets(1,&original.p,nullptr);check(!nativeLumaTarget(ctx.p),"ordinary R8 target is not native planar luma");
 D3D11_RASTERIZER_DESC rd{};rd.FillMode=D3D11_FILL_SOLID;rd.CullMode=D3D11_CULL_NONE;rd.DepthClipEnable=TRUE;Com<ID3D11RasterizerState> rs;device->CreateRasterizerState(&rd,rs.out());ctx->RSSetState(rs.p);
 D3D11_VIEWPORT viewport{0,0,float(Width),float(Height),0,1};ctx->RSSetViewports(1,&viewport);
 void** table=*reinterpret_cast<void***>(ctx.p);NativeDraw nativeDraw{};std::memcpy(&nativeDraw,table+13,sizeof nativeDraw);
 const FLOAT white[4]{1,1,1,1},black[4]{};
 auto draw=[&](float red,float alpha){const FLOAT color[4]{red,0.17f,0.83f,alpha};ctx->UpdateSubresource(cb.p,0,nullptr,color,0,0);check(drawTransmittance(ctx.p,target.p,blend.p,nativeDraw,3,0),"actual production helper draws original shader alpha");};
 for(float rgb:{0.0f,0.17f,0.8f,1.0f})for(float alpha:{0.0f,0.25f,0.5f,0.75f,1.0f}){
  ctx->ClearRenderTargetView(target.p,white);ctx->ClearRenderTargetView(original.p,black);draw(rgb,alpha);const auto mask=read(ctx.p,coverage.p,staging.p);const int expected=int((1-alpha)*255+0.5f);
  for(auto v:mask)check(std::abs(int(v)-expected)<=1,"R8 source-alpha factor independent of RGB, not destination alpha");
  Com<ID3D11RenderTargetView> restored;ctx->OMGetRenderTargets(1,restored.out(),nullptr);check(restored.p==original.p,"ordinary target restored");Com<ID3D11PixelShader> restoredPS;ctx->PSGetShader(restoredPS.out(),nullptr,nullptr);check(restoredPS.p==ps.p,"original native pixel shader retained");
  const auto unchanged=read(ctx.p,ordinary.p,staging.p);for(auto v:unchanged)check(v==0,"ordinary RGB target never modified by companion");
 }
 ctx->ClearRenderTargetView(target.p,white);draw(0,0.25f);draw(1,0.5f);for(auto v:read(ctx.p,coverage.p,staging.p))check(std::abs(int(v)-96)<=1,"multi-layer transmittance uses product of inverse alpha");
 viewport.Width=Width/2.0f;ctx->RSSetViewports(1,&viewport);ctx->ClearRenderTargetView(target.p,white);draw(0,1);viewport.TopLeftX=Width/2.0f;ctx->RSSetViewports(1,&viewport);draw(0,0.5f);const auto stereo=read(ctx.p,coverage.p,staging.p);for(UINT y=0;y<Height;++y)for(UINT x=0;x<Width;++x)check(x<Width/2?stereo[y*Width+x]==0:std::abs(int(stereo[y*Width+x])-128)<=1,"native per-eye viewport retained without independent UV");
}
}
int main(){try{run();std::printf("actual D3D11 R8 source-alpha companion: %u checks PASS\n",checks);return 0;}catch(const std::exception& e){std::fprintf(stderr,"D3D11 companion failed after %u: %s\n",checks,e.what());return 1;}}
