#include "api.h"
#include "container_audio_dialog.h"
#include "container_decoder.h"
#include <commctrl.h>
#include <windowsx.h>
#include <algorithm>

namespace frzz {
namespace {
constexpr int track_table=101;
constexpr short dialog_width=430;
constexpr int margin=8;
constexpr int button_width=112;
constexpr int button_gap=6;
constexpr int button_height=16;

struct DialogData {
  const std::vector<ContainerStream>& streams;
  const std::vector<std::string>* items;
  int visible_rows;
  TrackDialogMode mode;
};

std::wstring display(std::string value,const char* missing) {
  if(value.empty())value=missing;
  for(auto& c:value)if(static_cast<unsigned char>(c)<32)c=' ';
  return std::filesystem::u8path(value).wstring();
}

HWND add_control(HWND dialog,const wchar_t* cls,const wchar_t* label,DWORD style,int id,RECT bounds) {
  MapDialogRect(dialog,&bounds);
  const auto hwnd=CreateWindowExW(0,cls,label,WS_CHILD|WS_VISIBLE|style,
    bounds.left,bounds.top,bounds.right-bounds.left,bounds.bottom-bounds.top,
    dialog,reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),GetModuleHandleW(nullptr),nullptr);
  if(hwnd)SendMessageW(hwnd,WM_SETFONT,SendMessageW(dialog,WM_GETFONT,0,0),FALSE);
  return hwnd;
}

LRESULT CALLBACK table_mouse(HWND table,UINT msg,WPARAM w,LPARAM l,UINT_PTR id,DWORD_PTR) {
  if(msg==WM_LBUTTONDOWN || msg==WM_LBUTTONDBLCLK || msg==WM_RBUTTONDOWN) {
    LVHITTESTINFO hit{};
    hit.pt={GET_X_LPARAM(l),GET_Y_LPARAM(l)};
    if(ListView_SubItemHitTest(table,&hit)<0)return 0;
  }
  if(msg==WM_NCDESTROY)RemoveWindowSubclass(table,table_mouse,id);
  return DefSubclassProc(table,msg,w,l);
}

bool populate(HWND table,const DialogData& data) {
  ListView_SetExtendedListViewStyle(table,LVS_EX_FULLROWSELECT|LVS_EX_DOUBLEBUFFER|LVS_EX_LABELTIP);
  if(data.items) {
    wchar_t label[]=L"Video item";
    LVCOLUMNW column{};
    column.mask=LVCF_TEXT|LVCF_WIDTH;
    column.pszText=label;
    column.cx=32;
    if(SendMessageW(table,LVM_INSERTCOLUMNW,0,reinterpret_cast<LPARAM>(&column))<0)return false;
    for(size_t row=0;row<data.items->size();++row) {
      auto title=display((*data.items)[row],"Untitled");
      LVITEMW item{};
      item.mask=LVIF_TEXT;
      item.iItem=static_cast<int>(row);
      item.pszText=title.data();
      if(SendMessageW(table,LVM_INSERTITEMW,0,reinterpret_cast<LPARAM>(&item))<0)return false;
    }
    return true;
  }
  const auto& streams=data.streams;
  const wchar_t* labels[]={L"Track",L"Language",L"Title",L"Codec",L"Channels"};
  for(int col=0;col<5;++col) {
    LVCOLUMNW column{};
    column.mask=LVCF_TEXT|LVCF_FMT|LVCF_WIDTH;
    column.pszText=const_cast<wchar_t*>(labels[col]);
    column.fmt=LVCFMT_LEFT;
    column.cx=32;
    if(SendMessageW(table,LVM_INSERTCOLUMNW,col,reinterpret_cast<LPARAM>(&column))<0)return false;
  }
  for(size_t row=0;row<streams.size();++row) {
    const auto& track=streams[row];
    auto number=L"#"+std::to_wstring(track.index);
    auto title=display(track.title,"Untitled");
    if(track.frames<=0)title+=L" (unavailable)";
    auto channels=track.channels>0?std::to_string(track.channels):"?";
    if(!track.channel_order.empty())channels+=" · "+track.channel_order;
    std::wstring cells[]={std::move(number),display(track.language,"und"),std::move(title),
      display(track.codec,"Unknown"),display(std::move(channels),"?")};
    LVITEMW item{};
    item.mask=LVIF_TEXT;
    item.iItem=static_cast<int>(row);
    item.pszText=cells[0].data();
    if(SendMessageW(table,LVM_INSERTITEMW,0,reinterpret_cast<LPARAM>(&item))<0)return false;
    for(int col=1;col<5;++col) {
      item.iSubItem=col;
      item.pszText=cells[col].data();
      SendMessageW(table,LVM_SETITEMTEXTW,row,reinterpret_cast<LPARAM>(&item));
    }
  }
  return true;
}

void fit_table(HWND dialog,HWND table,int visible_rows,bool choosing_item) {
  RECT header{},row{},table_rect{},table_client{},dialog_rect{},dialog_client{};
  GetClientRect(ListView_GetHeader(table),&header);
  ListView_GetItemRect(table,0,&row,LVIR_BOUNDS);
  GetWindowRect(table,&table_rect);
  GetClientRect(table,&table_client);
  GetWindowRect(dialog,&dialog_rect);
  GetClientRect(dialog,&dialog_client);

  RECT spacing{0,0,margin,margin};
  RECT button{0,0,0,button_height};
  MapDialogRect(dialog,&spacing);
  MapDialogRect(dialog,&button);
  const int border=(table_rect.bottom-table_rect.top)-table_client.bottom;
  const int table_height=header.bottom+visible_rows*(row.bottom-row.top)+border;
  const int button_top=spacing.bottom+table_height+spacing.bottom;
  const int new_client_height=button_top+button.bottom+spacing.bottom;
  const int height_change=new_client_height-dialog_client.bottom;
  SetWindowPos(dialog,nullptr,dialog_rect.left,dialog_rect.top-height_change/2,
    dialog_rect.right-dialog_rect.left,dialog_rect.bottom-dialog_rect.top+height_change,SWP_NOZORDER|SWP_NOACTIVATE);
  SetWindowPos(table,nullptr,0,0,table_rect.right-table_rect.left,table_height,SWP_NOMOVE|SWP_NOZORDER|SWP_NOACTIVATE);

  const int ids[]={IDCANCEL,IDOK};
  int left=dialog_width-margin-button_width;
  for(int id:ids) {
    const auto control=GetDlgItem(dialog,id);
    if(!control)continue;
    RECT x{left,0,left+button_width,0};
    MapDialogRect(dialog,&x);
    SetWindowPos(control,nullptr,x.left,button_top,x.right-x.left,
      button.bottom,SWP_NOZORDER|SWP_NOACTIVATE);
    left-=button_width+button_gap;
  }

  GetClientRect(table,&table_client);
  if(choosing_item) {
    ListView_SetColumnWidth(table,0,table_client.right);
    return;
  }
  const int widths[]{34,54,0,52,132};
  int available=table_client.right;
  for(int col:{0,1,3,4}) {
    RECT size{0,0,widths[col],0};
    MapDialogRect(dialog,&size);
    ListView_SetColumnWidth(table,col,size.right);
    available-=size.right;
  }
  ListView_SetColumnWidth(table,2,std::max(0,available));
}

int importable_row(HWND dialog,const DialogData* data) {
  const int row=ListView_GetNextItem(GetDlgItem(dialog,track_table),-1,LVNI_SELECTED);
  if(!data || row<0)return -1;
  if(data->items)return static_cast<size_t>(row)<data->items->size()?row:-1;
  return static_cast<size_t>(row)<data->streams.size() &&
    data->streams[static_cast<size_t>(row)].frames>0?row:-1;
}

INT_PTR CALLBACK route(HWND window,UINT msg,WPARAM w,LPARAM l) {
  try {
    if(msg==WM_INITDIALOG) {
      const auto* data=reinterpret_cast<const DialogData*>(l);
      SetWindowLongPtrW(window,DWLP_USER,reinterpret_cast<LONG_PTR>(data));
      SetWindowTextW(window,data->items?L"Choose video item":L"Choose audio track");
      const auto table=add_control(window,WC_LISTVIEWW,L"",
        WS_BORDER|WS_TABSTOP|LVS_REPORT|LVS_SINGLESEL|LVS_SHOWSELALWAYS|LVS_NOSORTHEADER,
        track_table,{margin,margin,dialog_width-margin,margin+12*data->visible_rows+10});
      const bool video=data->mode==TrackDialogMode::import_video;
      const bool changing=data->mode==TrackDialogMode::change;
      const auto import=add_control(window,L"BUTTON",data->items?L"Choose audio track":(changing?L"Change track":(video?L"Import video with this track":L"Import audio")),WS_TABSTOP|BS_DEFPUSHBUTTON,
        IDOK,{0,0,button_width,button_height});
      const auto keep=add_control(window,L"BUTTON",changing?L"Cancel":L"Selected default audiotrack",WS_TABSTOP,
        IDCANCEL,{0,0,button_width,button_height});
      if(!table || !import || !keep || !SetWindowSubclass(table,table_mouse,1,0) ||
         !populate(table,*data)){EndDialog(window,0);return TRUE;}
      fit_table(window,table,data->visible_rows,data->items!=nullptr);
      EnableWindow(import,FALSE);
      SetFocus(table);
      return FALSE;
    }
    if(msg==WM_CLOSE || (msg==WM_COMMAND && LOWORD(w)==IDCANCEL)){EndDialog(window,0);return TRUE;}

    const auto* data=reinterpret_cast<const DialogData*>(GetWindowLongPtrW(window,DWLP_USER));
    if(msg==WM_COMMAND && LOWORD(w)==IDOK) {
      const int row=importable_row(window,data);
      if(row>=0)EndDialog(window,(data->items?row:data->streams[static_cast<size_t>(row)].index)+1);
      return TRUE;
    }
    if(msg==WM_NOTIFY) {
      const auto* notification=reinterpret_cast<const NMHDR*>(l);
      if(notification->idFrom!=track_table)return FALSE;
      if(notification->code==LVN_ITEMCHANGED) {
        EnableWindow(GetDlgItem(window,IDOK),importable_row(window,data)>=0);
      }
      if(notification->code==NM_DBLCLK) {
        const auto* hit=reinterpret_cast<const NMITEMACTIVATE*>(notification);
        const int row=importable_row(window,data);
        if(row>=0 && hit->iItem==row)EndDialog(window,(data->items?row:data->streams[static_cast<size_t>(row)].index)+1);
      }
    }
  }catch(...){EndDialog(window,0);return TRUE;}
  return FALSE;
}
int run_dialog(const DialogData& data) {
  const INITCOMMONCONTROLSEX controls{sizeof(controls),ICC_LISTVIEW_CLASSES};
  if(!InitCommonControlsEx(&controls))return -2;
  struct Template {
    DLGTEMPLATE dialog;
    WORD menu,window_class,title,point_size;
    wchar_t font[9];
  };
  alignas(DWORD) Template layout{{WS_POPUP|WS_CAPTION|WS_SYSMENU|DS_MODALFRAME|DS_CENTER|DS_SETFONT,0,0,0,0,
    dialog_width,static_cast<short>(12*data.visible_rows+50)},0,0,0,9,L"Segoe UI"};
  return static_cast<int>(DialogBoxIndirectParamW(GetModuleHandleW(nullptr),&layout.dialog,GetMainHwnd(),route,reinterpret_cast<LPARAM>(&data)));
}
}

int choose_container_stream(const std::vector<ContainerStream>& streams,TrackDialogMode mode) {
  const auto rows=static_cast<int>(std::clamp(streams.size(),size_t{3},size_t{10}));
  const int result=run_dialog({streams,nullptr,rows,mode});
  if(result>0)return static_cast<int>(result)-1;
  return result==0 && mode!=TrackDialogMode::change?default_container_stream(streams):-2;
}

int choose_video_item(const std::vector<std::string>& items) {
  const std::vector<ContainerStream> streams;
  const auto rows=static_cast<int>(std::clamp(items.size(),size_t{3},size_t{10}));
  return run_dialog({streams,&items,rows,TrackDialogMode::change})-1;
}
}
