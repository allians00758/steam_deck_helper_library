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
    if((name=="fsr411"&&v.find("4.1.1")!=std::string::npos)||(name=="ffx315"&&v.find("3.1.5")!=std::string::npos))return (int)i;}
  return -1;
}
bool upscale(const std::string& s,Upscaler& u){if(s=="xess")u=Upscaler::XeSS;else if(s=="fsr21")u=Upscaler::FSR21;else if(s=="fsr22")u=Upscaler::FSR22;
  else if(s=="ffx"||s=="fsr411"||s=="ffx315")u=Upscaler::FFX;else if(s=="helixsr")u=Upscaler::DLSS;else return false;return true;}
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
 std::string n=u==Upscaler::DLSS?"helixsr":u==Upscaler::FFX?"ffx315":u==Upscaler::XeSS?"xess":u==Upscaler::FSR22?"fsr22":"other";
 std::string s="api="+std::to_string((int)st.api)+"\nupscaler="+n+"\nbackend="+n+
 "\nfg_input="+input(st.activeFgInput)+"\nfg_output="+output(st.activeFgOutput)+
 "\nfg_provider="+provider(st.activeFgNvngx)+"\nfg_provider_requested="+provider(c->FGNvngxReplacement.value_or_default())+
 "\nfg_enabled="+(c->IsFGEnabled()?std::string("1"):std::string("0"))+
 "\nhelixsr_fg_ready="+(ready()?std::string("1"):std::string("0"))+
 "\nquality_enabled="+(c->UpscaleRatioOverrideEnabled.value_or_default()?std::string("1"):std::string("0"))+
 "\nquality_ratio="+std::to_string(c->UpscaleRatioOverrideValue.value_or_default())+
 "\nsharpness_enabled="+(c->OverrideSharpness.value_or_default()?std::string("1"):std::string("0"))+
 "\nsharpness="+std::to_string(c->Sharpness.value_or_default())+
 "\nscale_enabled="+(c->OutputScalingEnabled.value_or_default()?std::string("1"):std::string("0"))+
 "\nscale="+std::to_string(c->OutputScalingMultiplier.value_or_default())+"\n";
 if(s.size()+1>cap)return 0;std::memcpy(out,s.c_str(),s.size()+1);return (uint32_t)s.size();
}
extern "C" __declspec(dllexport) uint32_t __cdecl SDH_Control_Apply(const char* cmd,char*err,uint32_t cap){
 auto*c=Config::Instance();if(!c){error(err,cap,"config_unavailable");return 0;}const P p=parse(cmd);auto&st=State::Instance();
 if(p.empty()||val(p,"seq").empty()){error(err,cap,"invalid_command");return 0;}
 const char* supported[]={"has_upscaler","has_quality","has_sharpness","has_scale","has_shader","has_motion_sharpness","has_contrast","has_fg_enabled","has_hudfix","has_hud_limit","has_latencyflex_mode","has_fg_smoothness","has_color_space","has_auto_exposure","has_motion_vectors","has_root_signature_fix","has_resource_barrier_fix","has_precompiled_shaders","has_xess_build_pipelines","has_xess_create_heaps","has_shortcut_key","has_fg_provider"};
 for(const auto& [k,v]:p){if(k.rfind("has_",0)!=0||!on(p,k.c_str()))continue;bool ok=false;for(auto*s:supported)if(k==s){ok=true;break;}if(!ok){error(err,cap,"unknown_control_command");return 0;}}
 Upscaler u=Upscaler::Reset;const std::string name=val(p,"upscaler");
 if(on(p,"has_upscaler")&&!upscale(name,u)){error(err,cap,"invalid_upscaler");return 0;}
 if(on(p,"has_upscaler")&&(name=="ffx315"||name=="fsr411")&&ffxIndex(st,name)<0){error(err,cap,"ffx_version_unavailable");return 0;}
 if(on(p,"has_upscaler")&&name=="helixsr"&&!HelixSR::Found()){error(err,cap,"helixsr_upscaler_unavailable");return 0;}
 const bool helix=on(p,"has_fg_provider")&&val(p,"fg_provider")=="helixsr";
 if(on(p,"has_fg_provider")&&!helix&&val(p,"fg_provider")!="off"){error(err,cap,"fg_provider_unsupported");return 0;}
 if(helix&&(!ready()||st.api!=DX12)){error(err,cap,"helixsr_fg_not_ready");return 0;}
 if(helix&&st.activeFgInput!=FGInput::NvngxFG&&c->FGInput.value_or_default()!=FGInput::NvngxFG){error(err,cap,"native_nvngx_fg_input_required");return 0;}
 if(on(p,"has_latencyflex_mode")&&(num(val(p,"latencyflex_mode"),-1)<0||num(val(p,"latencyflex_mode"),-1)>2)){error(err,cap,"invalid_latencyflex_mode");return 0;}
 if(on(p,"has_upscaler")){if(st.api==DX12)c->Dx12Upscaler.set_volatile_value(u);else if(st.api==DX11)c->Dx11Upscaler.set_volatile_value(u);else if(st.api==Vulkan)c->VulkanUpscaler.set_volatile_value(u);else {error(err,cap,"api_not_ready");return 0;}
 if(name=="ffx315"||name=="fsr411")c->FfxUpscalerIndex.set_volatile_value(ffxIndex(st,name));st.newBackend=u;for(auto& e:st.changeBackend)e.second=true;}
 if(on(p,"has_quality")){c->UpscaleRatioOverrideEnabled.set_volatile_value(on(p,"quality_enabled"));c->UpscaleRatioOverrideValue.set_volatile_value(std::clamp(flt(val(p,"quality_ratio"),1.3f),1.0f,3.0f));}
 if(on(p,"has_sharpness")){c->OverrideSharpness.set_volatile_value(on(p,"sharpness_enabled"));c->Sharpness.set_volatile_value(std::clamp(flt(val(p,"sharpness"),0.3f),0.0f,1.3f));}
 if(on(p,"has_fg_enabled")){c->FGEnabled.set_volatile_value(on(p,"fg_enabled"));st.fgChanged=true;}
 if(on(p,"has_hudfix")){c->FGHUDFix.set_volatile_value(on(p,"hudfix"));st.clearCapturedHudlesses=true;st.fgChanged=true;}
 if(on(p,"has_hud_limit"))c->FGHUDLimit.set_volatile_value(std::clamp(num(val(p,"hud_limit"),1),1,999));
 if(on(p,"has_latencyflex_mode"))c->FN_LatencyFlexMode.set_volatile_value(static_cast<LFXMode>(num(val(p,"latencyflex_mode"),0)));
 if(helix){c->FGNvngxReplacement.set_volatile_value(FGNvngxReplacement::HelixSR);st.fgChanged=true;}
 if(on(p,"has_fg_provider")&&val(p,"fg_provider")=="off"){c->FGEnabled.set_volatile_value(false);st.fgChanged=true;}
 if(on(p,"has_shader")||on(p,"has_scale")||on(p,"has_motion_sharpness")||on(p,"has_contrast")||on(p,"has_fg_smoothness")||on(p,"has_color_space")||on(p,"has_auto_exposure")||on(p,"has_motion_vectors")||on(p,"has_root_signature_fix")||on(p,"has_resource_barrier_fix")||on(p,"has_precompiled_shaders")||on(p,"has_xess_build_pipelines")||on(p,"has_xess_create_heaps")||on(p,"has_shortcut_key")){error(err,cap,"control_requires_full_bridge_dev1");return 0;}
 error(err,cap,"");return 1;
}
