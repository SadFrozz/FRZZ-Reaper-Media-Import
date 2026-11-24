#include "api.h"
#include "video_settings.h"
#include "container_audio.h"
#include "container_audio_dialog.h"
#include "container_decoder.h"
#include "support.h"
#include <algorithm>
#include <fstream>
#include <string_view>

namespace frzz {
namespace {
constexpr int explode_multichannel=40894;
constexpr int edit_take_marker_at_cursor=42385;
constexpr int delete_take_markers_in_item=42387;
constexpr int build_missing_peaks=40047;
constexpr int rebuild_all_peaks=40048;
constexpr int build_missing_peaks_for_selected_items=40245;
constexpr int rebuild_peaks_for_selected_items=40441;
constexpr std::size_t max_inline_track_commands=256;
int (*registration)(const char*,void*);
int submenu_id{};
int change_track_id{};
gaccel_register_t change_track_action{};
std::vector<int> commands;
#ifdef _WIN32
HHOOK input_hook{};
HWND mouse_action_window{};
#endif

struct MenuTarget {
  ReaProject* project{};
  MediaItem* item{};
  MediaItem_Take* take{};
  PCM_source* source{};
  ContainerAudio audio;
  std::string path;
  std::string label;
  int current=-1;
};
struct MenuChoice {
  std::size_t item{};
  int stream=-1;
};
std::vector<MenuTarget> menu_targets;
std::vector<MenuChoice> menu_choices;

struct MenuOwner {
  HMENU menu=CreatePopupMenu();
  ~MenuOwner(){if(menu)DestroyMenu(menu);}
};

bool insert_menu_item(HMENU menu,int position,int command,const std::string& text,UINT state=0,HMENU submenu=nullptr) {
  std::string escaped;
  for(char ch:text) {
    if(ch=='&')escaped+='&';
    escaped+=static_cast<unsigned char>(ch)<32?' ':ch;
  }
#ifdef _WIN32
  auto name=std::filesystem::u8path(escaped).wstring();
  MENUITEMINFOW item{};
  item.dwTypeData=name.data();
#else
  MENUITEMINFO item{};
  item.dwTypeData=escaped.data();
#endif
  item.cbSize=sizeof(item);
  item.fMask=MIIM_TYPE|MIIM_ID|MIIM_STATE;
  item.fType=MFT_STRING;
  item.fState=state;
  item.wID=static_cast<UINT>(command);
  if(submenu) {
    item.fMask|=MIIM_SUBMENU;
    item.hSubMenu=submenu;
  }
#ifdef _WIN32
  return InsertMenuItemW(menu,static_cast<UINT>(position),TRUE,&item)!=FALSE;
#else
  const int before=GetMenuItemCount(menu);
  InsertMenuItem(menu,position,TRUE,&item);
  return GetMenuItemCount(menu)>before;
#endif
}

int menu_id(HMENU menu,int position) {
#ifdef _WIN32
  MENUITEMINFOW item{};
#else
  MENUITEMINFO item{};
#endif
  item.cbSize=sizeof(item);
  item.fMask=MIIM_ID;
#ifdef _WIN32
  if(!GetMenuItemInfoW(menu,static_cast<UINT>(position),TRUE,&item))return 0;
#else
  if(!GetMenuItemInfo(menu,position,TRUE,&item))return 0;
#endif
  return static_cast<int>(item.wID);
}

int audio_track_position(HMENU root) {
  int after_processing=-1;
  for(int i=0;i<GetMenuItemCount(root);++i) {
    const auto submenu=GetSubMenu(root,i);
    if(!submenu)continue;
    for(int j=0;j<GetMenuItemCount(submenu);++j) {
      const int command=menu_id(submenu,j);
      if(command==edit_take_marker_at_cursor || command==delete_take_markers_in_item)return i+1;
      if(command==explode_multichannel)after_processing=i+1;
    }
  }
  return after_processing;
}

MenuTarget item_target(ReaProject* project,MediaItem* item) {
  MenuTarget result;
  result.project=project;
  result.item=item;
  result.take=item?GetActiveTake(item):nullptr;
  result.source=result.take?GetMediaItemTake_Source(result.take):nullptr;
  if(const auto* path=container_track_file(result.source))result.path=path;
  return result;
}

struct VideoSelection {
  std::vector<MenuTarget> items;
  bool single=false;
  bool unavailable=false;
};

std::string item_label(const MenuTarget& item) {
  const auto* name=GetTakeName(item.take);
  char track[256]{},time[64]{};
  if(auto* parent=GetMediaItemTrack(item.item))GetTrackName(parent,track,sizeof(track));
  format_timestr_pos(GetMediaItemInfo_Value(item.item,"D_POSITION"),time,sizeof(time),-1);
  return std::string(name && *name?name:"Video item")+"  ["+track+", "+time+"]";
}

VideoSelection selected_track_targets(ReaProject* project,bool switching_only=false) {
  VideoSelection result;
  if(!project)return result;
  VideoTrackProbes probes;
  const auto count=CountSelectedMediaItems(project);
  for(int i=0;i<count;++i) {
    auto item=item_target(project,GetSelectedMediaItem(project,i));
    switch(video_track_options(item.source,item.audio,item.current,probes,switching_only)) {
      case VideoTrackOptions::selectable:
        item.label=item_label(item);
        result.items.push_back(std::move(item));
        break;
      case VideoTrackOptions::single: result.single=true; break;
      case VideoTrackOptions::unavailable: result.unavailable=true; break;
      case VideoTrackOptions::not_video: break;
    }
  }
  if(result.items.empty()) {
    // Prefer switchable video items, with audio-only container imports available as a fallback.
    for(int i=0;i<count;++i) {
      auto item=item_target(project,GetSelectedMediaItem(project,i));
      if(!container_track_options(item.source,item.audio,item.current,true))continue;
      item.label=item_label(item);
      result.items.push_back(std::move(item));
    }
  }
  return result;
}

std::string stream_label(const ContainerStream& stream,int current) {
  return "#"+std::to_string(stream.index)+"  "+
    (stream.title.empty()?"Untitled":stream.title)+"  ["+
    (stream.language.empty()?"und":stream.language)+", "+stream.codec+", "+
    std::to_string(stream.channels)+" ch"+
    (stream.channel_order.empty()?"":" · "+stream.channel_order)+"]"+
    (stream.index==current?" (current)":"");
}

int route_command(std::vector<MenuChoice>& routes,MenuChoice choice) {
  const auto slot=routes.size();
  if(slot==commands.size()) {
    const auto key="FRZZ_CONTAINER_TRACK_"+std::to_string(slot+1);
    const int id=registration("command_id",const_cast<char*>(key.c_str()));
    if(id<=0 || id>65535)return 0;
    commands.push_back(id);
  }
  routes.push_back(choice);
  return commands[slot];
}

bool append_tracks(HMENU menu,const MenuTarget& target,std::size_t item,std::vector<MenuChoice>* routes=nullptr) {
  for(std::size_t i=0;i<target.audio.streams.size();++i) {
    const auto& stream=target.audio.streams[i];
    UINT state=stream.index==target.current?MFS_CHECKED:0;
    if(stream.frames<=0)state|=MFS_GRAYED;
    int command=static_cast<int>(i)+1;
    if(routes)command=route_command(*routes,{item,stream.index});
    if(!command || !insert_menu_item(menu,static_cast<int>(i),command,stream_label(stream,target.current),state))return false;
  }
  return true;
}

void menu_hook(const char* name,void* handle,int flag) {
  if(flag!=1 || !handle || !name)return;
  const std::string_view menu_name(name);
  if(menu_name!="Media item context" && menu_name!="Main item")return;
  menu_targets.clear();
  menu_choices.clear();
  try {
    const auto root=static_cast<HMENU>(handle);
    for(int i=GetMenuItemCount(root)-1;i>=0;--i)
      if(GetSubMenu(root,i) && menu_id(root,i)==submenu_id)DeleteMenu(root,static_cast<UINT>(i),MF_BYPOSITION);
    if(!video_settings().multiple_audio_tracks)return;
    const int position=audio_track_position(root);
    if(position<0)return;

    auto* project=EnumProjects(-1,nullptr,0);
    if(!project)return;
    std::vector<MenuTarget> items;
    if(CountSelectedMediaItems(project)==1) {
      auto next=item_target(project,GetSelectedMediaItem(project,0));
      if(container_track_options(next.source,next.audio,next.current))items.push_back(std::move(next));
    } else {
      items=selected_track_targets(project,true).items;
    }
    if(items.empty())return;
    MenuOwner submenu;
    if(!submenu.menu)return;
    std::vector<MenuChoice> routes;
    std::size_t inline_count=0;
    for(const auto& item:items) {
      inline_count+=item.audio.streams.size();
      if(inline_count>max_inline_track_commands)break;
    }
    if(inline_count>max_inline_track_commands) {
      // REAPER's command IDs persist for the session. Use local popup IDs for large selections.
      const int command=route_command(routes,{});
      if(!command || !insert_menu_item(submenu.menu,0,command,items.size()>1?"Choose item…":"Choose audio track…"))return;
    } else if(items.size()==1) {
      if(!append_tracks(submenu.menu,items.front(),0,&routes))return;
    } else {
      for(std::size_t i=0;i<items.size();++i) {
        MenuOwner tracks;
        if(!tracks.menu || !append_tracks(tracks.menu,items[i],i,&routes) ||
           !insert_menu_item(submenu.menu,static_cast<int>(i),0,items[i].label,0,tracks.menu))return;
        tracks.menu=nullptr;
      }
    }
    if(!insert_menu_item(root,position,submenu_id,"Audio track",0,submenu.menu))return;
    submenu.menu=nullptr;
    menu_targets=std::move(items);
    menu_choices=std::move(routes);
  } catch(const std::exception& e) {log("container track menu error: %s",e.what());}
}

bool peak_command(int command,int flag) {
  static bool running=false;
  if(running)return false;
  const bool all=command==build_missing_peaks || command==rebuild_all_peaks;
  const bool rebuild=command==rebuild_all_peaks || command==rebuild_peaks_for_selected_items;
  auto* project=EnumProjects(-1,nullptr,0);
  if(!project)return false;
  struct Item {
    MediaItem* pointer;
    bool selected;
    bool handled;
  };
  std::vector<Item> items;
  std::vector<PCM_source*> native_sources;
  bool any_handled=false,any_native=false;
  const auto count=all?CountMediaItems(project):CountSelectedMediaItems(project);
  for(int i=0;i<count;++i) {
    auto* item=all?GetMediaItem(project,i):GetSelectedMediaItem(project,i);
    if(!item)continue;
    const int takes=CountTakes(item);
    std::vector<PCM_source*> legacy_sources;
    bool has_stock_take=false;
    for(int t=0;t<takes;++t) {
      if(auto* take=GetTake(item,t)) {
        bool physical_key=false;
        auto* scoped=container_peak_source_override(GetMediaItemTake_Source(take),physical_key);
        if(scoped && !physical_key)legacy_sources.push_back(scoped);
        else {
          if(scoped)native_sources.push_back(scoped);
          has_stock_take=true;
        }
      }
    }
    // Keep the fallback for virtual keys. A stock action can also inspect
    // those takes in a mixed item, where it should see their audio leaves.
    if(has_stock_take)native_sources.insert(native_sources.end(),legacy_sources.begin(),legacy_sources.end());
    for(auto* source:legacy_sources)queue_item_source_peaks(source,rebuild);
    const bool handled=!has_stock_take && !legacy_sources.empty();
    items.push_back({item,IsMediaItemSelected(item),handled});
    any_handled|=!legacy_sources.empty();
    any_native|=!handled;
  }
  if(!any_handled) {
    if(native_sources.empty())return false;
    run_container_native_peak_command(command,flag,project,native_sources);
    return true;
  }
  if(any_native) {
    running=true;
    struct RestoreSelection {
      ReaProject* project;
      const std::vector<Item>& items;
      bool& running;
      ~RestoreSelection() {
        for(const auto& item:items)
          if(item.selected==item.handled && ValidatePtr2(project,item.pointer,"MediaItem*"))
            SetMediaItemSelected(item.pointer,item.selected);
        running=false;
      }
    } restore{project,items,running};
    for(const auto& item:items)
      if(item.selected==item.handled)SetMediaItemSelected(item.pointer,!item.handled);
    run_container_native_peak_command(rebuild?rebuild_peaks_for_selected_items:build_missing_peaks_for_selected_items,
                                      flag,project,native_sources);
  }
  return true;
}

void change_track(MenuTarget chosen,int index) {
  if(!video_settings().multiple_audio_tracks)return;
  try {
    if(!chosen.project || EnumProjects(-1,nullptr,0)!=chosen.project ||
       !ValidatePtr2(chosen.project,chosen.item,"MediaItem*") || !IsMediaItemSelected(chosen.item) ||
       GetActiveTake(chosen.item)!=chosen.take || GetMediaItemTake_Source(chosen.take)!=chosen.source)return;
    const auto* filename=container_track_file(chosen.source);
    if(!filename || chosen.path!=filename)return;
    const auto stream=std::find_if(chosen.audio.streams.begin(),chosen.audio.streams.end(),[&](const auto& row){return row.index==index;});
    if(index==chosen.current || stream==chosen.audio.streams.end() || stream->frames<=0)return;
    std::unique_ptr<PCM_source> replacement(create_selected_container_source(chosen.source,std::move(chosen.audio),index));
    if(!replacement) {
      ShowMessageBox("The selected stream could not be opened by FFmpeg.","FRZZ audio track",0);
      return;
    }
    Undo_BeginBlock2(chosen.project);
    GetSetMediaItemTakeInfo(chosen.take,"P_SOURCE",replacement.get());
    if(GetMediaItemTake_Source(chosen.take)!=replacement.get()) {
      Undo_EndBlock2(chosen.project,nullptr,0);
      return;
    }
    auto* source=replacement.release();
    delete chosen.source;
    source->Extended(PCM_SOURCE_EXT_SETITEMCONTEXT,chosen.item,chosen.take,nullptr);
    Undo_EndBlock2(chosen.project,"Change item audio track",UNDO_STATE_ITEMS);
    UpdateItemInProject(chosen.item);
    if(!has_video_container_audio(source))queue_automatic_item_source_peaks(source);
    UpdateArrange();
  } catch(const std::exception& e) {log("container track change error: %s",e.what());}
}

int choose_popup(HMENU menu,int x,int y) {
  return static_cast<int>(TrackPopupMenu(menu,TPM_RETURNCMD|TPM_NONOTIFY|TPM_RIGHTBUTTON,x,y,0,GetMainHwnd(),nullptr));
}

std::optional<std::size_t> choose_item_popup(const std::vector<MenuTarget>& items,int x,int y) {
  if(items.size()==1)return 0;
  MenuOwner menu;
  if(!menu.menu)return std::nullopt;
  for(std::size_t i=0;i<items.size();++i)
    if(!insert_menu_item(menu.menu,static_cast<int>(i),static_cast<int>(i)+1,items[i].label))return std::nullopt;
  const int picked=choose_popup(menu.menu,x,y);
  if(picked<=0 || static_cast<std::size_t>(picked)>items.size())return std::nullopt;
  return static_cast<std::size_t>(picked-1);
}

int choose_track_popup(const MenuTarget& target,int x,int y) {
  MenuOwner tracks;
  if(!tracks.menu || !append_tracks(tracks.menu,target,0))return -1;
  const int picked=choose_popup(tracks.menu,x,y);
  if(picked<=0 || static_cast<std::size_t>(picked)>target.audio.streams.size())return -1;
  return target.audio.streams[static_cast<std::size_t>(picked-1)].index;
}

int peak_dispatch_id{};

bool show_disabled_audio_action(int command) {
  if(command!=change_track_id || video_settings().multiple_audio_tracks)return false;
  MessageBox(GetMainHwnd(),"Multiple audio tracks are disabled. Enable them in Preferences > Plug-ins > Media Import and try again.",
             change_track_action.desc,MB_OK|MB_ICONINFORMATION);
  return true;
}

bool command_hook(int command,int flag) {
  if(change_track_id && show_disabled_audio_action(command))return true;
  if(peak_dispatch_id && command==peak_dispatch_id) {
    try {
      dispatch_native_peak_requests();
    } catch(const std::exception& e) {log("container peak dispatch error: %s",e.what());}
    return true;
  }
  if(command==build_missing_peaks || command==rebuild_all_peaks ||
     command==build_missing_peaks_for_selected_items || command==rebuild_peaks_for_selected_items) {
    if(native_peak_request_active())return false;
    try {
      return peak_command(command,flag);
    } catch(const std::exception& e) {log("container peak command error: %s",e.what());}
    return false;
  }
  const auto found=std::find(commands.begin(),commands.end(),command);
  if(found==commands.end())return false;
  if(!video_settings().multiple_audio_tracks)return true;
  const auto slot=static_cast<std::size_t>(found-commands.begin());
  if(slot>=menu_choices.size())return true;
  const auto choice=menu_choices[slot];
  auto items=std::move(menu_targets);
  menu_choices.clear();
  try {
    if(choice.stream>=0)change_track(std::move(items[choice.item]),choice.stream);
    else {
      int x{},y{};
      GetMousePosition(&x,&y);
      if(const auto item=choose_item_popup(items,x,y)) {
        auto& target=items[*item];
        const int index=choose_track_popup(target,x,y);
        if(index>=0)change_track(std::move(target),index);
      }
    }
  } catch(const std::exception& e) {log("container track menu command error: %s",e.what());}
  return true;
}

bool toolbar_title(std::string_view title) {
  constexpr int without_localization_cache=2;
  if(title.empty())return false;
  if(title=="Main toolbar" || title.substr(0,8)=="Toolbar ")return true;
  if(title==LocalizeString("Main toolbar","menus",without_localization_cache))return true;
  for(int i=1;i<=32;++i) {
    const auto name="Toolbar "+std::to_string(i);
    if(title==LocalizeString(name.c_str(),"menus",without_localization_cache))return true;
  }
  std::ifstream menus(std::filesystem::u8path(GetResourcePath())/"reaper-menu.ini");
  std::string line;
  bool toolbar=false;
  while(std::getline(menus,line)) {
    if(!line.empty() && line.back()=='\r')line.pop_back();
    if(line.compare(0,3,"\xef\xbb\xbf")==0)line.erase(0,3);
    const std::string_view entry(line);
    if(entry.empty())continue;
    if(entry.front()=='[')toolbar=entry=="[Main toolbar]" || entry.substr(0,18)=="[Floating toolbar ";
    if(toolbar && entry.substr(0,6)=="title=" && entry.substr(6)==title)return true;
  }
  return false;
}

bool toolbar_window(HWND hwnd) {
  for(int depth=0;hwnd && depth<12;++depth,hwnd=GetParent(hwnd)) {
#ifdef _WIN32
    char name[128]{};
    GetClassNameA(hwnd,name,sizeof(name));
    const std::string_view cls(name);
    if(cls!="REAPERVirtWndDlgHost" && cls!="#32770")continue;
    wchar_t title[256]{};
    GetWindowTextW(hwnd,title,256);
    if(toolbar_title(std::filesystem::path(title).u8string()))return true;
#else
    char title[1024]{};
    GetWindowText(hwnd,title,sizeof(title));
    if(toolbar_title(title))return true;
#endif
  }
  return false;
}

#ifdef _WIN32
LRESULT CALLBACK action_input(int code,WPARAM removed,LPARAM message) {
  if(code>=0 && removed==PM_REMOVE) {
    const auto& msg=*reinterpret_cast<const MSG*>(message);
    mouse_action_window=nullptr;
    switch(msg.message) {
      case WM_LBUTTONDOWN: case WM_LBUTTONUP: case WM_LBUTTONDBLCLK:
      case WM_RBUTTONDOWN: case WM_RBUTTONUP:
        mouse_action_window=msg.hwnd;
        break;
    }
  }
  return CallNextHookEx(nullptr,code,removed,message);
}
#endif

bool from_toolbar() {
  // REAPER passes its main window for both main toolbar and Action List invocations.
#ifdef _WIN32
  return toolbar_window(mouse_action_window);
#else
  return toolbar_window(static_cast<HWND>(audio_track_mouse_window()));
#endif
}

bool action_hook(KbdSectionInfo* section,int command,int,int,int,HWND) {
  if(command!=change_track_id || (section && section->uniqueID!=0))return false;
  if(show_disabled_audio_action(command))return true;
  static bool choosing=false;
  if(choosing)return true;
  choosing=true;
  struct ResetChoosing {
    bool& value;
    ~ResetChoosing(){value=false;}
  } reset{choosing};
  try {
    int x{},y{};
    GetMousePosition(&x,&y);
    const bool popup=from_toolbar();
    auto selection=selected_track_targets(EnumProjects(-1,nullptr,0));
    if(selection.items.empty()) {
      const char* message=selection.single?
        "The selected video item contains only one audio track. Select a different video item and try again.":
        (selection.unavailable?"No audio tracks are available for switching in the selected video items.":
         "Select a video item and try again.");
      ShowMessageBox(message,change_track_action.desc,0);
      return true;
    }
    std::optional<std::size_t> item=0;
    if(popup)item=choose_item_popup(selection.items,x,y);
    else if(selection.items.size()>1) {
      std::vector<std::string> labels;
      labels.reserve(selection.items.size());
      for(const auto& candidate:selection.items)labels.push_back(candidate.label);
      const int row=choose_video_item(labels);
      if(row<0)return true;
      item=static_cast<std::size_t>(row);
    }
    if(!item)return true;
    auto& chosen=selection.items[*item];
    const int index=popup?choose_track_popup(chosen,x,y):choose_container_stream(chosen.audio.streams,TrackDialogMode::change);
    if(index>=0)change_track(std::move(chosen),index);
  } catch(const std::exception& e) {log("change audio track action error: %s",e.what());}
  return true;
}
}

void register_container_track_menu(int (*register_api)(const char*,void*)) {
  submenu_id=register_api("command_id",const_cast<char*>("FRZZ_CONTAINER_TRACK_SUBMENU"));
  if(submenu_id<=0 || !register_api("hookcommand",reinterpret_cast<void*>(command_hook)))return;
  if(!register_api("hookcustommenu",reinterpret_cast<void*>(menu_hook))) {
    register_api("-hookcommand",reinterpret_cast<void*>(command_hook));
    return;
  }
  registration=register_api;
  peak_dispatch_id=registration("command_id",const_cast<char*>("FRZZ_CONTAINER_NATIVE_PEAK_REQUEST"));
  if(peak_dispatch_id>0 && peak_dispatch_id<=65535)set_native_peak_dispatch_command(peak_dispatch_id);
  change_track_id=registration("command_id",const_cast<char*>("FRZZ_CHANGE_VIDEO_AUDIO_TRACK"));
  if(change_track_id<=0 || change_track_id>65535 ||
     !registration("hookcommand2",reinterpret_cast<void*>(action_hook))){
    change_track_id=0;
    return;
  }
  change_track_action.accel.cmd=static_cast<WORD>(change_track_id);
  change_track_action.desc="Change Audio Track for selected video item";
  if(!registration("gaccel",&change_track_action)) {
    registration("-hookcommand2",reinterpret_cast<void*>(action_hook));
    change_track_id=0;
    return;
  }
#ifdef _WIN32
  input_hook=SetWindowsHookExW(WH_GETMESSAGE,action_input,nullptr,GetCurrentThreadId());
#endif
}

void unregister_container_track_menu() {
  if(!registration)return;
  set_native_peak_dispatch_command(0);
  peak_dispatch_id=0;
  registration("-hookcustommenu",reinterpret_cast<void*>(menu_hook));
  registration("-hookcommand",reinterpret_cast<void*>(command_hook));
  if(change_track_id) {
#ifdef _WIN32
    if(input_hook)UnhookWindowsHookEx(input_hook);
    input_hook=nullptr;
    mouse_action_window=nullptr;
#endif
    registration("-gaccel",&change_track_action);
    registration("-hookcommand2",reinterpret_cast<void*>(action_hook));
  }
  registration=nullptr;
  change_track_id=0;
  menu_targets.clear();
  menu_choices.clear();
  commands.clear();
}
}
