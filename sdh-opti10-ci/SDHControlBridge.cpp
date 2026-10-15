// SPDX-License-Identifier: GPL-3.0-or-later
// SDH Control Bridge: experimental integration into HelixSR Scaler (OptiScaler 10).
// This source is built into OptiScaler, never injected into an unrelated binary.
#include "pch.h"
#include "Config.h"
#include "State.h"
#include "helixsr/HelixSR.h"
#include <algorithm>
#include <cmath>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <unordered_map>
namespace {
using P = std::unordered_map<std::string,std::string>;
std::string val(const P& p,const char* name,const char* def=""){auto it=p.find(name);return it==p.end()?def:it->second;}
bool on(const P& p,const char* k){auto s=val(p,k);return s=="1"||s=="true"||s=="yes"||s=="on";}
P parse(const char* s){P p;if(!s)return p;std::string a(s);if(a.size()>8192)return p;size_t pos=0;while(pos<a.size()){auto e=a.find_first_of("\r\n",pos);auto line=a.substr(pos,e==std::string::npos?e:e-pos);auto eq=line.find('=');if(eq!=std::string::npos&&eq>0&&eq<96)p.emplace(line.substr(0,eq),line.substr(eq+1));if(e==std::string::npos)break;pos=e+1;}return p;}
void error(char*out,uint32_t n,const char*s){if(!out||n==0)return;std::strncpy(out,s,n-1);out[n-1]=0;}
float flt(const std::string& s,float d){try{size_t n=0;float f=std::stof(s,&n);return n==s.size()&&std::isfinite(f)?f:d;}catch(...){return d;}}
int num(const std::string& s,int d){try{size_t n=0;int a=std::stoi(s,&n);return n==s.size()?a:d;}catch(...){return d;}}
bool tri(const std::string&s,std::optional<bool>&o){if(s=="auto"){o.reset();return true;}if(s=="on"){o=true;return true;}if(s=="off"){o=false;return true;}return false;}
bool ready(){
  if(!HelixSR::Found())return false;
  const auto dir=HelixSR::Folder();
  for(const auto*s:{"nvexec64.dll","_nvngx.dll","nvngx_dlssg.dll"})if(!std::filesystem::is_regular_file(dir/"helixsr-fg"/s))return false;
  std::ifstream f(dir/"helixsr.dll",std::ios::binary|std::ios::ate);if(!f||f.tellg()<std::streampos(32))return false;
  char x[8]{};f.seekg(-32,std::ios::end);f.read(x,8);return f.gcount()==8&&std::memcmp(x,"HXSRNET1",8)==0;
}
int ffxIndex(const State& st,const std::string& name){
  const auto& names=st.api==Vulkan?st.ffxUpscalerVersionsVk.names:st.ffxUpscalerVersionsDx12.names;
  for(size_t i=0;i<names.size();++i){std::string v=names[i]?names[i]:"";std::transform(v.begin(),v.end(),v.begin(),[](unsigned char c){return (char)std::tolower(c);});
    if((name=="fsr411"&&v.find("4.1.1")!=std::string::npos)||(name=="ffx315"&&v.find("3.1.5")!=std::string::npos)||(name=="ffx234"&&v.find("2.3.4")!=std::string::npos))return (int)i;}
  return -1;
}
bool upscale(const std::string& s,Upscaler& u){if(s=="xess")u=Upscaler::XeSS;else if(s=="fsr21")u=Upscaler::FSR21;else if(s=="fsr22")u=Upscaler::FSR22;
  else if(s=="ffx"||s=="fsr411"||s=="ffx315"||s=="ffx234")u=Upscaler::FFX;else if(s=="helixsr")u=Upscaler::DLSS;else return false;return true;}
const char* input(FGInput v){switch(v){case FGInput::NvngxFG:return "nvngxfg";case FGInput::DLSSG:return "dlssg";case FGInput::Upscaler:return "upscaler";case FGInput::FSRFG:return "fsrfg";case FGInput::XeFG:return "xefg";case FGInput::FSRFG30:return "fsrfg30";default:return "nofg";}}
const char* output(FGOutput v){switch(v){case FGOutput::DLSSG:return "dlssg";case FGOutput::FSRFG:return "fsrfg";case FGOutput::XeFG:return "xefg";default:return "nofg";}}
const char* provider(FGNvngxReplacement v){switch(v){case FGNvngxReplacement::HelixSR:return "helixsr";case FGNvngxReplacement::Nukems:return "nukems";case FGNvngxReplacement::Arturs:return "arturs";case FGNvngxReplacement::FFX:return "ffx";case FGNvngxReplacement::Combo:return "combo";default:return "none";}}
}
extern "C" __declspec(dllexport) uint32_t __cdecl SDH_Control_Version(){return 11;}
extern "C" __declspec(dllexport) uint32_t __cdecl SDH_Control_ConfigPathW(wchar_t* out,uint32_t cap){
 auto* c=Config::Instance();if(!c||!out||cap<2)return 0;auto s=c->IniPath().wstring();if(s.size()>=cap)return 0;std::copy(s.begin(),s.end(),out);out[s.size()]=0;return (uint32_t)s.size();}
extern "C" __declspec(dllexport) void* __cdecl SDH_Control_RealSwapchain(){return State::Instance().currentRealSwapchain;}
extern "C" __declspec(dllexport) uint32_t __cdecl SDH_Control_Snapshot(char*out,uint32_t cap){
 auto*c=Config::Instance();if(!c||!out)return 0;auto&st=State::Instance();
 auto u=st.api==DX11?c->Dx11Upscaler.value_or_default():st.api==Vulkan?c->VulkanUpscaler.value_or_default():c->Dx12Upscaler.value_or_default();
 std::string n=u==Upscaler::DLSS?(HelixSR::Found()?"helixsr":"dlss"):
   u==Upscaler::XeSS?"xess":u==Upscaler::FSR21?"fsr21":u==Upscaler::FSR22?"fsr22":"other";
 if(u==Upscaler::FFX){
   n="ffx";
   const auto& names=st.api==Vulkan?st.ffxUpscalerVersionsVk.names:st.ffxUpscalerVersionsDx12.names;
   int index=c->FfxUpscalerIndex.value_or_default();
   if(index>=0 && static_cast<size_t>(index)<names.size()){
     std::string label=names[static_cast<size_t>(index)]?names[static_cast<size_t>(index)]:"";
     std::transform(label.begin(),label.end(),label.begin(),[](unsigned char ch){return (char)std::tolower(ch);});
     if(label.find("4.1.1")!=std::string::npos)n="fsr411";
     else if(label.find("3.1.5")!=std::string::npos)n="ffx315";
     else if(label.find("2.3.4")!=std::string::npos)n="ffx234";
   }
 }
 std::string s="api="+std::to_string((int)st.api)+"\nffx_index="+std::to_string(c->FfxUpscalerIndex.value_or_default())+"\nupscaler="+n+"\nbackend="+n+
 "\nfg_input="+input(st.activeFgInput)+"\nfg_output="+output(st.activeFgOutput)+
 "\nfg_provider="+provider(st.activeFgNvngx)+"\nfg_provider_requested="+provider(c->FGNvngxReplacement.value_or_default())+
 "\nfg_enabled="+(c->IsFGEnabled()?std::string("1"):std::string("0"))+
 "\nhelixsr_fg_ready="+(ready()?std::string("1"):std::string("0"))+
 "\nquality_enabled="+(c->UpscaleRatioOverrideEnabled.value_or_default()?std::string("1"):std::string("0"))+
 "\nquality_ratio="+std::to_string(c->UpscaleRatioOverrideValue.value_or_default())+
 "\nsharpness_enabled="+(c->OverrideSharpness.value_or_default()?std::string("1"):std::string("0"))+
 "\nsharpness="+std::to_string(c->Sharpness.value_or_default())+
 "\nscale_enabled="+(c->OutputScalingEnabled.value_or_default()?std::string("1"):std::string("0"))+
 "\nscale="+std::to_string(c->OutputScalingMultiplier.value_or_default())+
 "\nshader_enabled="+(c->RcasEnabled.value_or_default()?std::string("1"):std::string("0"))+
 "\nmotion_sharpness_enabled="+(c->MotionSharpnessEnabled.value_or_default()?std::string("1"):std::string("0"))+
 "\nmotion_sharpness="+std::to_string(c->MotionSharpness.value_or_default())+
 "\ncontrast_enabled="+(c->ContrastEnabled.value_or_default()?std::string("1"):std::string("0"))+
 "\ncontrast="+std::to_string(c->Contrast.value_or_default())+
 "\nlatencyflex_mode="+std::to_string((int)c->FN_LatencyFlexMode.value_or_default())+
 "\nhudfix="+(c->FGHUDFix.value_or_default()?std::string("1"):std::string("0"))+
 "\nhud_limit="+std::to_string(c->FGHUDLimit.value_or_default())+
 "\n";
 if(s.size()+1>cap)return 0;std::memcpy(out,s.c_str(),s.size()+1);return (uint32_t)s.size();
}
extern "C" __declspec(dllexport) uint32_t __cdecl SDH_Control_Apply(const char* cmd,char*err,uint32_t cap){
 auto*c=Config::Instance();if(!c){error(err,cap,"config_unavailable");return 0;}const P p=parse(cmd);auto&st=State::Instance();
 if(p.empty()||val(p,"seq").empty()){error(err,cap,"invalid_command");return 0;}
 const char* supported[]={"has_upscaler","has_quality","has_sharpness","has_scale","has_shader","has_motion_sharpness","has_contrast","has_fg_enabled","has_hudfix","has_hud_limit","has_latencyflex_mode","has_fg_smoothness","has_color_space","has_auto_exposure","has_motion_vectors","has_root_signature_fix","has_resource_barrier_fix","has_precompiled_shaders","has_xess_build_pipelines","has_xess_create_heaps","has_shortcut_key","has_fg_provider"};
 for(const auto& [k,v]:p){if(k.rfind("has_",0)!=0||!on(p,k.c_str()))continue;bool ok=false;for(auto*s:supported)if(k==s){ok=true;break;}if(!ok){error(err,cap,"unknown_control_command");return 0;}}
 Upscaler u=Upscaler::Reset;const std::string name=val(p,"upscaler");
 if(on(p,"has_upscaler")&&!upscale(name,u)){error(err,cap,"invalid_upscaler");return 0;}
 if(on(p,"has_upscaler")&&(name=="ffx315"||name=="fsr411"||name=="ffx234")&&ffxIndex(st,name)<0){error(err,cap,"ffx_version_unavailable");return 0;}
 if(on(p,"has_upscaler")&&name=="helixsr"&&!HelixSR::Found()){error(err,cap,"helixsr_upscaler_unavailable");return 0;}
 const bool helix=on(p,"has_fg_provider")&&val(p,"fg_provider")=="helixsr";
 if(on(p,"has_fg_provider")&&!helix&&val(p,"fg_provider")!="off"){error(err,cap,"fg_provider_unsupported");return 0;}
 if(helix&&(!ready()||st.api!=DX12)){error(err,cap,"helixsr_fg_not_ready");return 0;}
 if(helix&&st.activeFgInput!=FGInput::NvngxFG&&c->FGInput.value_or_default()!=FGInput::NvngxFG){error(err,cap,"native_nvngx_fg_input_required");return 0;}
 if(on(p,"has_latencyflex_mode")&&(num(val(p,"latencyflex_mode"),-1)<0||num(val(p,"latencyflex_mode"),-1)>2)){error(err,cap,"invalid_latencyflex_mode");return 0;}

 // Transactional validation: a rejected multi-control command must not partially change live Config.
 if(on(p,"has_upscaler")&&st.api!=DX12&&st.api!=DX11&&st.api!=Vulkan){error(err,cap,"api_not_ready");return 0;}
 if(on(p,"has_shader")){
   const auto v=val(p,"shader");
   if(v!="off"&&v!="rcas"&&v!="da_rcas"&&v!="da_das"){error(err,cap,"invalid_shader");return 0;}
 }
 if(on(p,"has_fg_smoothness")){const auto v=val(p,"fg_smoothness");if(v!="auto"&&v!="stability"&&v!="performance"){error(err,cap,"invalid_fg_smoothness");return 0;}}
 const char* tristate[]={"auto","on","off"};
 for(const auto& c : {std::pair<const char*,const char*>{"has_auto_exposure","auto_exposure"},{"has_root_signature_fix","root_signature_fix"},{"has_precompiled_shaders","precompiled_shaders"},{"has_xess_build_pipelines","xess_build_pipelines"},{"has_xess_create_heaps","xess_create_heaps"}}){
   if(!on(p,c.first))continue;
   std::optional<bool> v;if(!tri(val(p,c.second),v)){error(err,cap,"invalid_tristate");return 0;}
 }
 if(on(p,"has_color_space")){const auto v=val(p,"color_space");if(v!="auto"&&v!="linear"&&v!="non_linear"&&v!="srgb"&&v!="pq"){error(err,cap,"invalid_color_space");return 0;}}
 if(on(p,"has_motion_vectors")){const auto v=val(p,"motion_vectors");if(v!="auto"&&v!="display"&&v!="render"){error(err,cap,"invalid_motion_vectors");return 0;}}
 if(on(p,"has_resource_barrier_fix")){const auto v=val(p,"resource_barrier_fix");if(v!="auto"&&v!="on"&&v!="off"){error(err,cap,"invalid_resource_barrier_fix");return 0;}}
 if(on(p,"has_shortcut_key")){const auto v=val(p,"shortcut_key");if(v!="auto"&&v!="home"&&v!="end"){error(err,cap,"invalid_shortcut_key");return 0;}}
 if(on(p,"has_motion_vectors")&&val(p,"motion_vectors")=="display"&&on(p,"has_scale")){error(err,cap,"display_vectors_scaling_conflict");return 0;}
 if(on(p,"has_upscaler")){if(st.api==DX12)c->Dx12Upscaler.set_volatile_value(u);else if(st.api==DX11)c->Dx11Upscaler.set_volatile_value(u);else if(st.api==Vulkan)c->VulkanUpscaler.set_volatile_value(u);else {error(err,cap,"api_not_ready");return 0;}
 if(name=="ffx315"||name=="fsr411"||name=="ffx234")c->FfxUpscalerIndex.set_volatile_value(ffxIndex(st,name));st.newBackend=u;for(auto& e:st.changeBackend)e.second=true;}
 if(on(p,"has_quality")){c->UpscaleRatioOverrideEnabled.set_volatile_value(on(p,"quality_enabled"));c->UpscaleRatioOverrideValue.set_volatile_value(std::clamp(flt(val(p,"quality_ratio"),1.3f),1.0f,3.0f));}
 if(on(p,"has_sharpness")){c->OverrideSharpness.set_volatile_value(on(p,"sharpness_enabled"));c->Sharpness.set_volatile_value(std::clamp(flt(val(p,"sharpness"),0.3f),0.0f,1.3f));}

 if(on(p,"has_shader")){
   const auto v=val(p,"shader");
   c->RcasEnabled.set_volatile_value(v!="off");
   c->SharpnessShader.set_volatile_value(v=="da_rcas"?SharpenShader::DepthAware:v=="da_das"?SharpenShader::LocalContrastDepthAware:SharpenShader::RCAS);
 }
 if(on(p,"has_scale")){
   const float a=std::clamp(flt(val(p,"scale"),1.0f),0.5f,3.0f);
   c->OutputScalingEnabled.set_volatile_value(std::abs(a-1.f)>0.001f);
   c->OutputScalingMultiplier.set_volatile_value(a);
   st.newBackend=st.api==DX11?c->Dx11Upscaler.value_or_default():st.api==Vulkan?c->VulkanUpscaler.value_or_default():c->Dx12Upscaler.value_or_default();
   for(auto&e:st.changeBackend)e.second=true;
 }
 if(on(p,"has_motion_sharpness")){
   c->MotionSharpnessEnabled.set_volatile_value(on(p,"motion_sharpness_enabled"));
   c->MotionSharpness.set_volatile_value(std::clamp(flt(val(p,"motion_sharpness"),0.2f),-1.f,1.f));
 }
 if(on(p,"has_contrast")){
   c->ContrastEnabled.set_volatile_value(on(p,"contrast_enabled"));
   c->Contrast.set_volatile_value(std::clamp(flt(val(p,"contrast"),-0.3f),-2.f,2.f));
 }
 if(on(p,"has_fg_smoothness")){
   auto v=val(p,"fg_smoothness");
   if(v=="auto"){
     c->FGFramePacingTuning.reset();c->FGFPTSafetyMarginInMs.reset();c->FGFPTVarianceFactor.reset();
     c->FGFPTAllowHybridSpin.reset();c->FGFPTHybridSpinTime.reset();c->FGFPTAllowWaitForSingleObjectOnFence.reset();
   }else{
     c->FGFramePacingTuning.set_volatile_value(true);
     c->FGFPTSafetyMarginInMs.set_volatile_value(v=="stability"?0.75f:0.01f);
     c->FGFPTVarianceFactor.set_volatile_value(v=="stability"?0.1f:0.3f);
     c->FGFPTAllowHybridSpin.set_volatile_value(false);
     c->FGFPTHybridSpinTime.set_volatile_value(2);
     c->FGFPTAllowWaitForSingleObjectOnFence.set_volatile_value(false);
   }
   st.fsrfgFramePaceTuningChanged=true;
 }
 if(on(p,"has_color_space")){
   auto v=val(p,"color_space");
   if(v=="auto"){c->FsrNonLinearColorSpace.reset();c->FsrNonLinearSRGB.reset();c->FsrNonLinearPQ.reset();}
   else {c->FsrNonLinearColorSpace.set_volatile_value(v!="linear");c->FsrNonLinearSRGB.set_volatile_value(v=="srgb");c->FsrNonLinearPQ.set_volatile_value(v=="pq");}
   for(auto&e:st.changeBackend)e.second=true;
 }
 if(on(p,"has_auto_exposure")){
   std::optional<bool>v;tri(val(p,"auto_exposure"),v);
   if(v.has_value())c->AutoExposure.set_volatile_value(*v);else c->AutoExposure.reset();
   for(auto&e:st.changeBackend)e.second=true;
 }
 if(on(p,"has_motion_vectors")){
   auto v=val(p,"motion_vectors");if(v=="auto")c->DisplayResolution.reset();
   else c->DisplayResolution.set_volatile_value(v=="display");
   for(auto&e:st.changeBackend)e.second=true;
 }
 if(on(p,"has_root_signature_fix")){
   std::optional<bool>v;tri(val(p,"root_signature_fix"),v);
   if(v.has_value()){c->RestoreComputeSignature.set_volatile_value(*v);c->RestoreGraphicSignature.set_volatile_value(*v);}
   else{c->RestoreComputeSignature.reset();c->RestoreGraphicSignature.reset();}
 }
 if(on(p,"has_resource_barrier_fix")){
   auto v=val(p,"resource_barrier_fix");if(v=="on")c->ColorResourceBarrier.set_volatile_value(4);else c->ColorResourceBarrier.reset();
 }
 if(on(p,"has_precompiled_shaders")){
   std::optional<bool>v;tri(val(p,"precompiled_shaders"),v);
   if(v.has_value())c->UsePrecompiledShaders.set_volatile_value(*v);else c->UsePrecompiledShaders.reset();
   for(auto&e:st.changeBackend)e.second=true;
 }
 if(on(p,"has_xess_build_pipelines")){
   std::optional<bool>v;tri(val(p,"xess_build_pipelines"),v);
   if(v.has_value())c->BuildPipelines.set_volatile_value(*v);else c->BuildPipelines.reset();
 }
 if(on(p,"has_xess_create_heaps")){
   std::optional<bool>v;tri(val(p,"xess_create_heaps"),v);
   if(v.has_value())c->CreateHeaps.set_volatile_value(*v);else c->CreateHeaps.reset();
 }
 if(on(p,"has_shortcut_key")){
   auto v=val(p,"shortcut_key");
   if(v=="auto")c->ShortcutKey.reset();else c->ShortcutKey.set_volatile_value(v=="home"?0x24:0x23);
 }
 if(on(p,"has_fg_enabled")){c->FGEnabled.set_volatile_value(on(p,"fg_enabled"));st.fgChanged=true;}
 if(on(p,"has_hudfix")){c->FGHUDFix.set_volatile_value(on(p,"hudfix"));st.clearCapturedHudlesses=true;st.fgChanged=true;}
 if(on(p,"has_hud_limit"))c->FGHUDLimit.set_volatile_value(std::clamp(num(val(p,"hud_limit"),1),1,999));
 if(on(p,"has_latencyflex_mode"))c->FN_LatencyFlexMode.set_volatile_value(static_cast<LFXMode>(num(val(p,"latencyflex_mode"),0)));
 if(helix){c->FGNvngxReplacement.set_volatile_value(FGNvngxReplacement::HelixSR);st.fgChanged=true;}
 if(on(p,"has_fg_provider")&&val(p,"fg_provider")=="off"){c->FGEnabled.set_volatile_value(false);st.fgChanged=true;}
 error(err,cap,"");return 1;
}
