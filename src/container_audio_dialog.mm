#import <Cocoa/Cocoa.h>
#include "container_audio_dialog.h"
#include "container_decoder.h"
#include <algorithm>

@interface FRZZTrackList : NSTableView
@end

@implementation FRZZTrackList
- (void)mouseDown:(NSEvent*)event {
  const auto point=[self convertPoint:event.locationInWindow fromView:nil];
  if([self rowAtPoint:point]>=0)[super mouseDown:event];
}
@end

@interface FRZZTrackTable : NSObject <NSTableViewDataSource, NSTableViewDelegate>
@property(assign) const std::vector<frzz::ContainerStream>* streams;
@property(assign) const std::vector<std::string>* items;
@property(assign) NSButton* confirm;
@end

static NSString* display(const std::string& value,NSString* fallback) {
  if(value.empty())return fallback;
  auto* result=[NSString stringWithUTF8String:value.c_str()];
  return result?result:fallback;
}

@implementation FRZZTrackTable
- (NSInteger)numberOfRowsInTableView:(NSTableView*)table {
  (void)table;
  return static_cast<NSInteger>(self.items?self.items->size():self.streams->size());
}
- (id)tableView:(NSTableView*)table objectValueForTableColumn:(NSTableColumn*)column row:(NSInteger)row {
  (void)table;
  if(self.items)return display((*self.items)[static_cast<size_t>(row)],@"Untitled");
  const auto& stream=(*self.streams)[static_cast<size_t>(row)];
  if([column.identifier isEqualToString:@"track"])return @(stream.index);
  if([column.identifier isEqualToString:@"language"])return display(stream.language,@"und");
  if([column.identifier isEqualToString:@"title"]) {
    auto* title=display(stream.title,@"Untitled");
    return stream.frames>0?title:[title stringByAppendingString:@" (unavailable)"];
  }
  if([column.identifier isEqualToString:@"codec"])return display(stream.codec,@"Unknown");
  auto channels=stream.channels>0?std::to_string(stream.channels):"?";
  if(!stream.channel_order.empty())channels+=" · "+stream.channel_order;
  return display(channels,@"?");
}
- (void)tableViewSelectionDidChange:(NSNotification*)notification {
  NSTableView* table=notification.object;
  const NSInteger row=table.selectedRow;
  const bool enabled=row>=0 && (self.items || (*self.streams)[static_cast<size_t>(row)].frames>0);
  self.confirm.enabled=enabled;
}
- (NSString*)tableView:(NSTableView*)table toolTipForCell:(NSCell*)cell rect:(NSRectPointer)rect
           tableColumn:(NSTableColumn*)column row:(NSInteger)row mouseLocation:(NSPoint)location {
  (void)cell;
  (void)rect;
  (void)location;
  if(!self.items && ![column.identifier isEqualToString:@"title"] && ![column.identifier isEqualToString:@"channels"])return nil;
  return [self tableView:table objectValueForTableColumn:column row:row];
}
@end

namespace frzz {
void* audio_track_mouse_window() {
  auto* event=[NSApp currentEvent];
  const auto type=event.type;
  if(type!=NSEventTypeLeftMouseDown && type!=NSEventTypeLeftMouseUp &&
     type!=NSEventTypeRightMouseDown && type!=NSEventTypeRightMouseUp)return nullptr;
  auto* content=event.window.contentView;
  if(!content)return nullptr;
  const auto point=[content.superview convertPoint:event.locationInWindow fromView:nil];
  auto* hit=[content hitTest:point];
  return hit?hit:content;
}

static int run_dialog(const std::vector<ContainerStream>& streams,TrackDialogMode mode,
                      const std::vector<std::string>* items) {
  @autoreleasepool {
    NSAlert* alert=[[NSAlert alloc] init];
    alert.messageText=items?@"Choose video item":@"Choose audio track";
    const bool video=mode==TrackDialogMode::import_video;
    const bool changing=mode==TrackDialogMode::change;
    NSButton* confirm=[alert addButtonWithTitle:items?@"Choose Audio Track":(changing?@"Change Track":(video?@"Import Video with This Track":@"Import Audio"))];
    [alert addButtonWithTitle:changing?@"Cancel":@"Selected default audiotrack"].keyEquivalent=@"\033";
    const auto count=items?items->size():streams.size();
    const auto rows=static_cast<CGFloat>(std::clamp(count,size_t{3},size_t{10}));
    NSRect bounds=NSMakeRect(0,0,716,rows*23+24);
    NSScrollView* scroll=[[NSScrollView alloc] initWithFrame:bounds];
    scroll.hasVerticalScroller=count>10;
    scroll.autohidesScrollers=YES;
    NSTableView* table=[[FRZZTrackList alloc] initWithFrame:bounds];
    table.rowHeight=21;
    table.intercellSpacing=NSMakeSize(4,2);
    table.usesAlternatingRowBackgroundColors=YES;
    table.allowsEmptySelection=YES;
    table.allowsColumnReordering=NO;
    table.columnAutoresizingStyle=NSTableViewUniformColumnAutoresizingStyle;
    NSString* keys[]={@"track",@"language",@"title",@"codec",@"channels"};
    NSString* names[]={@"Track",@"Language",@"Title",@"Codec",@"Channels"};
    CGFloat widths[]={53,79,250,84,211};
    for(int i=0;i<(items?1:5);++i) {
      NSTableColumn* column=[[NSTableColumn alloc] initWithIdentifier:keys[i]];
      column.title=items?@"Video item":names[i];
      column.width=items?bounds.size.width:widths[i];
      [table addTableColumn:column];
      [column release];
    }
    FRZZTrackTable* delegate=[[FRZZTrackTable alloc] init];
    delegate.streams=&streams;
    delegate.items=items;
    delegate.confirm=confirm;
    table.dataSource=delegate;
    table.delegate=delegate;
    scroll.documentView=table;
    alert.accessoryView=scroll;
    confirm.enabled=NO;
    [alert.window setInitialFirstResponder:table];
    const auto pressed=[alert runModal];
    const auto row=table.selectedRow;
    int selected=-2;
    if(pressed==NSAlertFirstButtonReturn && row>=0 && (items || streams[static_cast<size_t>(row)].frames>0))
      selected=items?static_cast<int>(row):streams[static_cast<size_t>(row)].index;
    else if(pressed==NSAlertSecondButtonReturn && !changing)
      selected=default_container_stream(streams);
    table.dataSource=nil;
    table.delegate=nil;
    [delegate release];
    [table release];
    [scroll release];
    [alert release];
    return selected;
  }
}

int choose_container_stream(const std::vector<ContainerStream>& streams,TrackDialogMode mode) {
  return run_dialog(streams,mode,nullptr);
}

int choose_video_item(const std::vector<std::string>& items) {
  return run_dialog({},TrackDialogMode::change,&items);
}
}
