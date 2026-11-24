#include "api.h"
#include "container_audio.h"
#include "video_settings.h"
#include "video_settings_resource.h"
#include "container_native_video.h"
#ifdef FRZZ_ENABLE_WINDOWS_HDR
#include "video_hdr_shader_cache.h"
#endif

#include <atomic>
#include <cstring>

#ifndef _WIN32
#include "swell/swell-dlggen.h"

SWELL_DEFINE_DIALOG_RESOURCE_BEGIN(IDD_FRZZ_VIDEO_SETTINGS,
  SWELL_DLG_WS_CHILD | SWELL_DLG_WS_FLIPPED | SWELL_DLG_WS_NOAUTOSIZE,
  "Media Import",300,80,1.7)
  {nullptr,0,nullptr,0,0,0,0,0,0},
  {"Enable multiple audio tracks",IDC_FRZZ_MULTI_AUDIO,"button",WS_CHILD|WS_VISIBLE|BS_AUTOCHECKBOX|WS_TABSTOP,10,8,280,12,0},
  {"Convert HDR and Dolby Vision to SDR",IDC_FRZZ_HDR_TO_SDR,"button",WS_CHILD|WS_VISIBLE|BS_AUTOCHECKBOX|WS_TABSTOP,10,25,280,12,0},
  {"Apply HEVC playback fixes",IDC_FRZZ_HEVC_PATCHES,"button",WS_CHILD|WS_VISIBLE|BS_AUTOCHECKBOX|WS_TABSTOP,10,42,280,12,0},
  {"Skip eligible frames when seeking HEVC",IDC_FRZZ_HEVC_SEEK_SKIP,"button",WS_CHILD|WS_VISIBLE|BS_AUTOCHECKBOX|WS_TABSTOP,22,58,268,12,0}
SWELL_DEFINE_DIALOG_RESOURCE_END(IDD_FRZZ_VIDEO_SETTINGS)
#endif

namespace frzz {
namespace {
constexpr char settings_section[]="FRZZ_MEDIA_IMPORT";
constexpr char settings_key[]="video_features";
constexpr int apply_button=0x478;
constexpr UINT apply_message=WM_USER*2;
#if defined(FRZZ_ENABLE_WINDOWS_HDR) || (defined(FRZZ_ENABLE_MACOS_HDR) && defined(__x86_64__))
constexpr bool supports_hdr=true;
#else
constexpr bool supports_hdr=false;
#endif
#if defined(_WIN32) || defined(__x86_64__)
constexpr bool supports_hevc_patches=true;
#else
constexpr bool supports_hevc_patches=false;
#endif
HINSTANCE module{};
std::atomic<std::uint32_t> active_settings{15};

void activate(const VideoSettings& settings) {
  const auto previous=video_settings();
  const auto generation=previous.seek_generation+
    (previous.hevc_seek_optimization()!=settings.hevc_seek_optimization());
  active_settings.store((generation<<4) | (settings.multiple_audio_tracks?1u:0u) |
    (settings.hdr_to_sdr?2u:0u) | (settings.hevc_patches?4u:0u) |
    (settings.skip_hevc_seek_frames?8u:0u),std::memory_order_release);
}

VideoSettings saved_settings() {
  const auto* stored=GetExtState(settings_section,settings_key);
  if(!stored || std::strlen(stored)!=4 || std::strspn(stored,"01")!=4)return {};
  return {stored[0]=='1',stored[1]=='1',stored[2]=='1',stored[3]=='1'};
}

bool checked(HWND page,int id) {
  return SendMessage(GetDlgItem(page,id),BM_GETCHECK,0,0)==BST_CHECKED;
}

void check(HWND page,int id,bool value) {
  SendMessage(GetDlgItem(page,id),BM_SETCHECK,value?BST_CHECKED:BST_UNCHECKED,0);
}

void show_settings(HWND page,const VideoSettings& settings) {
  check(page,IDC_FRZZ_MULTI_AUDIO,settings.multiple_audio_tracks);
  check(page,IDC_FRZZ_HDR_TO_SDR,supports_hdr && settings.hdr_to_sdr);
  check(page,IDC_FRZZ_HEVC_PATCHES,supports_hevc_patches && settings.hevc_patches);
  check(page,IDC_FRZZ_HEVC_SEEK_SKIP,supports_hevc_patches && settings.skip_hevc_seek_frames);
  EnableWindow(GetDlgItem(page,IDC_FRZZ_HDR_TO_SDR),supports_hdr);
  EnableWindow(GetDlgItem(page,IDC_FRZZ_HEVC_PATCHES),supports_hevc_patches);
  EnableWindow(GetDlgItem(page,IDC_FRZZ_HEVC_SEEK_SKIP),supports_hevc_patches && settings.hevc_patches);
}

void save_and_activate(const VideoSettings& settings) {
  const bool audio_changed=settings.multiple_audio_tracks!=video_settings().multiple_audio_tracks;
  const char stored[]={
    settings.multiple_audio_tracks?'1':'0',settings.hdr_to_sdr?'1':'0',
    settings.hevc_patches?'1':'0',settings.skip_hevc_seek_frames?'1':'0',0};
  SetExtState(settings_section,settings_key,stored,true);
  activate(settings);
  prepare_video_hdr_shaders();
  if(audio_changed)refresh_container_audio_routes();
}

INT_PTR CALLBACK preferences_dialog(HWND page,UINT message,WPARAM wparam,LPARAM) {
  switch(message) {
    case WM_INITDIALOG: {
      show_settings(page,video_settings());
      return 1;
    }
    case WM_COMMAND:
      if(HIWORD(wparam)!=BN_CLICKED || LOWORD(wparam)<IDC_FRZZ_MULTI_AUDIO || LOWORD(wparam)>IDC_FRZZ_HEVC_SEEK_SKIP)break;
      if(LOWORD(wparam)==IDC_FRZZ_HEVC_PATCHES)
        EnableWindow(GetDlgItem(page,IDC_FRZZ_HEVC_SEEK_SKIP),checked(page,IDC_FRZZ_HEVC_PATCHES));
      EnableWindow(GetDlgItem(GetParent(page),apply_button),true);
      return 1;
    case apply_message: {
      auto settings=video_settings();
      settings.multiple_audio_tracks=checked(page,IDC_FRZZ_MULTI_AUDIO);
      if(supports_hdr)settings.hdr_to_sdr=checked(page,IDC_FRZZ_HDR_TO_SDR);
      if(supports_hevc_patches) {
        settings.hevc_patches=checked(page,IDC_FRZZ_HEVC_PATCHES);
        settings.skip_hevc_seek_frames=checked(page,IDC_FRZZ_HEVC_SEEK_SKIP);
      }
      save_and_activate(settings);
      return 1;
    }
  }
  return 0;
}

HWND create_preferences(HWND parent) {
  return CreateDialogParam(module,MAKEINTRESOURCE(IDD_FRZZ_VIDEO_SETTINGS),parent,preferences_dialog,0);
}

prefs_page_register_t preferences{"frzz_media_import","Media Import",create_preferences,0x9a,"",0,nullptr,nullptr,{}};
}

VideoSettings video_settings() noexcept {
  const auto value=active_settings.load(std::memory_order_acquire);
  return {bool(value&1),bool(value&2),bool(value&4),bool(value&8),value>>4};
}

void load_video_settings() {activate(saved_settings());}

void prepare_video_hdr_shaders() noexcept {
#ifdef FRZZ_ENABLE_WINDOWS_HDR
  if(video_settings().hdr_to_sdr && windows_hardware_video_supported() && !frzz_hdr_shader_cache_ready())
    frzz_hdr_load_shader_cache(GetResourcePath());
#endif
}

int register_video_preferences(void* instance,int (*register_api)(const char*,void*)) {
  module=static_cast<HINSTANCE>(instance);
  return register_api("prefpage",&preferences);
}

void unregister_video_preferences(int (*register_api)(const char*,void*)) {
  register_api("-prefpage",&preferences);
}
}
