/* The Local Play window (Mac, ⌘L; docs/LAN-COOP-DESIGN.md section 7): host your island or visit one from anywhere,
   without walking to the airport. The islands answering on the network (LAN, ZeroTier, other copies on this Mac) are
   listed by coop_lan.c; islands on networks without discovery are added by address. Host, Visit, Stop Hosting and
   Leave Island queue the game's own Local Play steps (coop_flow.c); the game saves first, as at the airport. Each
   button is enabled only where it applies: Host and Visit outside a session, Stop Hosting as the host, Leave Island as
   a visitor. */
#import <Cocoa/Cocoa.h>
#include "coop.h"

@interface CoopController : NSObject <NSTableViewDataSource, NSTableViewDelegate, NSWindowDelegate>
@property(strong) NSWindow *window;
@property(strong) NSTableView *table;
@property(strong) NSTextField *status, *address, *result;
@property(strong) NSButton *visit, *remove, *hostButton, *stopButton, *leaveButton;
@property(strong) NSTimer *timer;
@end

@implementation CoopController {
    CoopIsland islands[32]; int count;
}
- (NSInteger)numberOfRowsInTableView:(NSTableView *)t { (void)t; return count; }
- (NSView *)tableView:(NSTableView *)t viewForTableColumn:(NSTableColumn *)col row:(NSInteger)row {
    (void)t;
    const CoopIsland *i = &islands[row];
    NSString *s;
    if ([col.identifier isEqual:@"island"]) s = @(i->island[0] ? i->island : "?");
    else if ([col.identifier isEqual:@"player"]) s = @(i->player);
    else if ([col.identifier isEqual:@"players"]) s = i->seen_ns ? [NSString stringWithFormat:@"%d / %d", i->players, i->max_players] : @"–";
    else s = [NSString stringWithFormat:@"%s%s", i->address, i->manual ? " (added)" : ""];
    NSTextField *f = [NSTextField labelWithString:s]; f.lineBreakMode = NSLineBreakByTruncatingTail;
    return f;
}
- (void)tableViewSelectionDidChange:(NSNotification *)n { (void)n; [self updateButtons]; }
- (void)updateButtons {
    NSInteger r = self.table.selectedRow;
    self.visit.enabled = r >= 0 && r < count && islands[r].seen_ns;
    self.remove.enabled = r >= 0 && r < count && islands[r].manual;
    int self_node = coop_lan_self_node();
    self.hostButton.enabled = self_node < 0; self.stopButton.enabled = self_node == 0; self.leaveButton.enabled = self_node > 0;
    if (self_node >= 0) self.visit.enabled = NO;
}
- (void)reload {
    char keep[64] = ""; NSInteger r = self.table.selectedRow;
    if (r >= 0 && r < count) snprintf(keep, sizeof keep, "%s", islands[r].address);
    count = coop_browser_list(islands, 32);
    [self.table reloadData];
    for (int i = 0; i < count; i++) if (keep[0] && !strcmp(islands[i].address, keep)) [self.table selectRowIndexes:[NSIndexSet indexSetWithIndex:(NSUInteger)i] byExtendingSelection:NO];
    self.status.stringValue = @(coop_status());
    const char *res = coop_flow_result(); self.result.stringValue = res[0] ? @(res) : @"";
    [self updateButtons];
}
- (void)tick { coop_browser_refresh(); [self performSelector:@selector(reload) withObject:nil afterDelay:0.8]; }
- (void)host:(id)s { (void)s; coop_flow_request(2); self.result.stringValue = @"Opening your island… (the game saves first)"; }
- (void)stopHosting:(id)s { (void)s; coop_flow_request(5); self.result.stringValue = @"Ending the session and closing the gate… (visitors fly home)"; }
- (void)leaveIsland:(id)s { (void)s; coop_flow_request(4); self.result.stringValue = @"Flying home… (everyone saves)"; }
- (void)visitIsland:(id)s {
    (void)s; NSInteger r = self.table.selectedRow; if (r < 0 || r >= count) return;
    coop_browser_select(islands[r].address);   /* the game's search then finds only this island */
    coop_flow_request(1);
    self.result.stringValue = [NSString stringWithFormat:@"Flying to %s… (the game saves first)", islands[r].island];
}
- (void)add:(id)s {
    (void)s; NSString *a = [self.address.stringValue stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceCharacterSet];
    if (!a.length) return;
    coop_browser_add(a.UTF8String); self.address.stringValue = @""; [self tick];
}
- (void)removeIsland:(id)s { (void)s; NSInteger r = self.table.selectedRow; if (r >= 0 && r < count && islands[r].manual) coop_browser_remove(islands[r].address); [self reload]; }
- (void)windowWillClose:(NSNotification *)n { (void)n; [self.timer invalidate]; self.timer = nil; }
- (NSTableColumn *)column:(NSString *)ident title:(NSString *)title width:(CGFloat)w {
    NSTableColumn *c = [[NSTableColumn alloc] initWithIdentifier:ident]; c.title = title; c.width = w; return c;
}
- (void)show {
    if (!self.window) {
        NSTextField *hint = [NSTextField wrappingLabelWithString:@"Host your island or visit one from anywhere, without going to the airport, and stop hosting or fly home the same way. Islands on this network (and on ZeroTier) show up by themselves; add others by address (ip or ip:port). Both copies need the same game version."];
        hint.textColor = NSColor.secondaryLabelColor; [hint.widthAnchor constraintEqualToConstant:560].active = YES;
        self.status = [NSTextField labelWithString:@""]; self.status.font = [NSFont boldSystemFontOfSize:NSFont.systemFontSize];
        self.hostButton = [NSButton buttonWithTitle:@"Host My Island" target:self action:@selector(host:)];
        self.stopButton = [NSButton buttonWithTitle:@"Stop Hosting" target:self action:@selector(stopHosting:)];
        self.leaveButton = [NSButton buttonWithTitle:@"Leave Island" target:self action:@selector(leaveIsland:)];
        NSStackView *host = [NSStackView stackViewWithViews:@[self.hostButton, self.stopButton, self.leaveButton]];
        self.table = [NSTableView new]; self.table.dataSource = self; self.table.delegate = self; self.table.usesAlternatingRowBackgroundColors = YES;
        [self.table addTableColumn:[self column:@"island" title:@"Island" width:150]];
        [self.table addTableColumn:[self column:@"player" title:@"Host" width:120]];
        [self.table addTableColumn:[self column:@"players" title:@"Players" width:70]];
        [self.table addTableColumn:[self column:@"address" title:@"Address" width:200]];
        self.table.doubleAction = @selector(visitIsland:); self.table.target = self;
        NSScrollView *scroll = [NSScrollView new]; scroll.documentView = self.table; scroll.hasVerticalScroller = YES;
        [scroll.widthAnchor constraintEqualToConstant:560].active = YES; [scroll.heightAnchor constraintEqualToConstant:200].active = YES;
        self.visit = [NSButton buttonWithTitle:@"Visit" target:self action:@selector(visitIsland:)]; self.visit.keyEquivalent = @"\r";
        self.remove = [NSButton buttonWithTitle:@"Remove" target:self action:@selector(removeIsland:)];
        self.address = [NSTextField textFieldWithString:@""]; self.address.placeholderString = @"192.168.1.20 or 10.147.17.5:11452";
        [self.address.widthAnchor constraintEqualToConstant:260].active = YES;
        NSButton *add = [NSButton buttonWithTitle:@"Add by Address" target:self action:@selector(add:)];
        NSStackView *row1 = [NSStackView stackViewWithViews:@[self.visit, self.remove]];
        NSStackView *row2 = [NSStackView stackViewWithViews:@[self.address, add]];
        self.result = [NSTextField labelWithString:@""]; self.result.textColor = NSColor.secondaryLabelColor;
        NSStackView *stack = [NSStackView stackViewWithViews:@[hint, self.status, host, scroll, row1, row2, self.result]];
        stack.orientation = NSUserInterfaceLayoutOrientationVertical; stack.alignment = NSLayoutAttributeLeading;
        stack.edgeInsets = NSEdgeInsetsMake(16, 16, 16, 16); [stack setCustomSpacing:16 afterView:host];
        self.window = [[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, 592, 470) styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable
                                                    backing:NSBackingStoreBuffered defer:NO];
        self.window.title = @"Local Play"; self.window.contentView = stack; self.window.releasedWhenClosed = NO; self.window.delegate = self;
        [self.window center];
    }
    if (!self.timer) self.timer = [NSTimer scheduledTimerWithTimeInterval:2 target:self selector:@selector(tick) userInfo:nil repeats:YES];
    [self tick];
    [self.window makeKeyAndOrderFront:nil]; [NSApp activateIgnoringOtherApps:YES];
}
- (void)showFromMenu:(id)s { (void)s; [self show]; }
@end

static CoopController *controller;
void coop_ui_add_menu_item(NSMenu *app_menu) {
    if (!controller) controller = [CoopController new];
    NSMenuItem *item = [[NSMenuItem alloc] initWithTitle:@"Local Play…" action:@selector(showFromMenu:) keyEquivalent:@"l"];
    item.keyEquivalentModifierMask = NSEventModifierFlagCommand; item.target = controller;
    [app_menu insertItem:item atIndex:1];
}
