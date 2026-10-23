#!/usr/bin/env python3
"""Local play without the airport (docs/LAN-COOP-DESIGN.md section 7): builds the RomFS overlay the runtime reads from
ACNH_COOP_ROMFS (default build/romfs-coop).

    make_coop_flows.py [--romfs extracted/romfs] [--out build/romfs-coop]

Orville's local play scripts run every network step on the EventFlowSystemActor, so they can run anywhere. This writes
two flowcharts of the port's own with only that actor (taken from SNPC_dod_11_Airport_LocalVisitor and
SNPC_dod_10_Airport_LocalHost; a flowchart with Orville's actor is refused away from the airport):
  Coop_Visit  save, start local mode, search, the game's island list when several answer, the Dodo Code prompt when the
              island has one, join, lock the session, fly (NetEnterVillage, without the walk through the gate)
  Coop_Host   save, start local mode, open the session (no Dodo Code), open the gate
  Coop_Leave  a visitor goes home: "Getting ready to return home...", lock the session, NetLeaveSessionVillage (the
              minus menu's System_InetClose OkaeriVisitor_Core without its menu-choice branch)
  Coop_Close  the host ends the session and closes the gate once nobody visits (Orville's close-gate steps)
in Coop_LocalVisitor, Coop_LocalHost, Coop_LocalLeave and Coop_LocalClose. Leave and close have flowcharts of their own:
a start from Coop_LocalVisitor did nothing while its visit event was still in use. EventFlow/Info/
EventFlowInfoProduct.byml lists them (no subfiles).
It also writes museum/EventFlow/SNPC_owl.bfevfl: Blathers' script with its "is this my island?" branch (which sends a
visitor to small talk, SNPC_owl_90_OtherTown) going the resident's way, so a visitor can donate. The runtime reads
museum/ only with ACNH_MP_EDIT "museum", whose patch sends the donation to the host (runtime/coop_edit.c). And
museum/EventFlow/SNPC_owl_01_Museum_Donation.bfevfl: the donation script with its "the island has a visitor" refusal
(SP_owl_00_Common:101, which stops everyone, the host too, while anyone visits) going on to the donation. That file
has entry points with variables, which the evfl package cannot write, so its case table is patched in place. Needs the evfl and byml packages (pip install evfl byml). Nothing made here is committed: it is built from
the player's own game files."""
import argparse
import copy
import struct
from pathlib import Path

import byml
import evfl
from evfl.common import StringHolder
from evfl.container import Container
from evfl.entry_point import EntryPoint
from evfl.event import ActionEvent, Event, SwitchEvent
from evfl.util import IdGenerator, make_index, make_rindex

VISITOR, HOST = 'SNPC_dod_11_Airport_LocalVisitor', 'SNPC_dod_10_Airport_LocalHost'
VISIT_FLOW, HOST_FLOW = 'Coop_LocalVisitor', 'Coop_LocalHost'
LEAVE_FLOW, CLOSE_FLOW = 'Coop_LocalLeave', 'Coop_LocalClose'   # own flowcharts: the visit's is still in use on arrival


class Builder:
    def __init__(self, flowchart, prefix):
        self.fc, self.prefix, self.ids = flowchart, prefix, IdGenerator()
        self.system = next(a for a in flowchart.actors if a.identifier.name == 'EventFlowSystemActor')

    def _name(self, kind, name):
        lst = self.system.actions if kind == 'a' else self.system.queries
        for h in lst:
            if h.v == name:
                return h
        h = StringHolder(name)
        lst.append(h)
        return h

    def _add(self, data):
        e = Event()
        e.name = '%s%d' % (self.prefix, self.ids.gen_id())
        e.data = data
        self.fc.events.append(e)
        return e

    def action(self, name, nxt=None, **params):
        d = ActionEvent()
        d.actor = make_rindex(self.system)
        d.actor_action = make_rindex(self._name('a', 'EventFlowAction' + name))
        if params:
            d.params = Container()
            d.params.data.update(params)
        d.nxt = make_index(nxt)
        return self._add(d)

    def switch(self, name, cases, **params):
        d = SwitchEvent()
        d.actor = make_rindex(self.system)
        d.actor_query = make_rindex(self._name('q', 'EventFlowQuery' + name))
        if params:
            d.params = Container()
            d.params.data.update(params)
        d.cases = {k: make_rindex(v) for k, v in cases.items()}
        return self._add(d)

    def chain(self, steps, last=None):
        """steps: [(action, params)], linked in order; returns the first event."""
        nxt = last
        for name, params in reversed(steps):
            nxt = self.action(name, nxt, **params)
        return nxt

    def entry(self, name, first):
        ep = EntryPoint(name)
        ep.main_event = make_index(first)
        self.fc.entry_points.append(ep)


END = {'Command': 'cEndVillageSession', 'IsErrorHandling': False}
SAVE_STEPS = [('MessageLockNext', {'WaitIconType': 1}), ('NetUpdateFriendList', {'Type': 'cFriendOnly'}),
              ('OpenMessageWindow', {'MessageID': 'TalkSys/SYS_Save:001', 'IsCloseMessageWindow': False}),
              ('SaveSimpleSave', {'Process': 'cAll'})]


def add_visit(fc):
    b = Builder(fc, 'Coop_Visit_')
    fail = b.chain([('MessageSuspend', {}), ('MessageUnlockNext', {}), ('NetEndSessionVillage', END)])
    depart = b.action('NetEnterVillage')
    locked = b.switch('CheckErrorType', {0: fail, 1: depart}, ErrorType=2)
    after_join = b.switch('CheckErrorType', {
        0: fail,
        1: b.chain([('NetDemoLock', {'Situation': 0}), ('MessageSuspend', {}), ('MessageUnlockNext', {})], locked)},
        ErrorType=2)
    join = b.chain([('MessageLockNext', {'WaitIconType': 1}),
                    ('OpenMessageWindow', {'MessageID': 'TalkSys/SYS_Network:004', 'IsCloseMessageWindow': False}),
                    ('NetJoinSessionVillage', {})], after_join)
    password = b.action('UISoftwareKeyboardHandling', b.switch('UIResultMenuDecide', {0: join, 1: fail}),
                        FormType='cPasswordLocal', IsClearString=True)
    check_password = b.switch('NetIsNeedPassword', {0: password, 1: join}, Type='cNeedPassword')
    decided = b.switch('NetIsDecidedVillage', {0: check_password, 1: fail, 2: fail})
    village_list = b.chain([('EventFlagSystemOff', {'AccessKey': 'cPlayerTemp:AirportTripListError'}),
                            ('OpenVillageList', {})],
                           b.switch('EventFlagSystemBool', {0: fail, 1: decided},
                                    AccessKey='cPlayerTemp:AirportTripListError'))
    one_found = b.switch('NetAllSessionIsFull', {0: fail, 1: check_password})
    found = b.switch('NetFoundVillageNum', {0: fail, 1: one_found, 2: village_list})
    search = b.chain([('MessageSuspend', {}),
                      ('OpenMessageWindow', {'MessageID': 'TalkSys/SYS_Network:003', 'IsCloseMessageWindow': False}),
                      ('NetFindSessionVillage', {'Type': 'cLocal'}), ('MessageSuspend', {}), ('MessageUnlockNext', {})],
                     found)
    init = b.action('NetInitLocal', search, Command='cInitLocal', IsErrorHandling=False)
    restart = b.chain([('NetShutdown', {'Command': 'cNetShutdown', 'IsErrorHandling': False})], init)
    after_save = b.switch('NetMode', {0: init, 1: init, 2: restart})
    b.entry('Coop_Visit', b.chain(SAVE_STEPS, after_save))


def add_leave(fc):
    """System_InetClose's OkaeriVisitor_Core (the minus menu's "go home") up to its menu-choice branch, which only that
    menu sets, then straight to leaving."""
    b = Builder(fc, 'Coop_Leave_')
    stay = b.chain([('MessageSuspend', {}), ('MessageUnlockNext', {})])
    leave = b.chain([('MessageSuspend', {}), ('NetLeaveSessionVillage', {'Command': 'cLeaveSession', 'IsErrorHandling': False})])
    b.entry('Coop_Leave', b.chain([('MessageLockNext', {'WaitIconType': 1}),
                                   ('OpenMessageWindow', {'MessageID': 'TalkSys/SYS_Network:005', 'IsCloseMessageWindow': False}),
                                   ('NetDemoLock', {'Situation': 1}), ('MessageUnlockNext', {})],
                                  b.switch('CheckErrorType', {0: stay, 1: leave}, ErrorType=2)))


def add_close(fc):
    """The host closes the gate with nobody visiting, as at the airport (SNPC_dod_AirPort
    EntryPoint_CloseGateAndResetNetwork without Orville's turns): NetEndSessionVillage, CloseVillageGate. Stop Hosting
    first has every visitor leave (runtime/coop_flow.c): ending the session under a visitor shows them a communication
    error, and the minus menu's own ending (NetDemoLock 2) needs that menu's code to finish."""
    b = Builder(fc, 'Coop_Close_')
    b.entry('Coop_Close', b.chain([('NetEndSessionVillage', END), ('CloseVillageGate', {})]))


def add_host(fc):
    b = Builder(fc, 'Coop_Host_')
    open_gate = b.chain([('NetCreateSessionVillage', {'Type': 'cOpenLocal', 'IsOver8': False}), ('MessageSuspend', {}),
                         ('MessageUnlockNext', {}), ('OpenVillageGate', {'IsColDisable': False})])
    init = b.chain([('NetInitLocal', {'Command': 'cInitLocal', 'IsErrorHandling': False}), ('MessageSuspend', {})],
                   open_gate)
    restart = b.chain([('NetShutdown', {'Command': 'cNetShutdown', 'IsErrorHandling': False})], init)
    after_save = b.switch('NetMode', {0: init, 1: init, 2: restart})
    b.entry('Coop_Host', b.chain(SAVE_STEPS, after_save))
    b.entry('Coop_Test', b.action('OpenMessageWindow', None, MessageID='TalkSys/SYS_Save:001', IsCloseMessageWindow=True))   # diagnostics


def read_flow(data):
    """An EventFlow read only, with entry-point variables skipped (the evfl package asserts there are none)."""
    import evfl.entry_point as ep
    from evfl.util import SeekContext
    keep = ep.EntryPoint._do_read

    def read(self, stream):
        off = stream.read_u64(); stream.read_u64(); stream.read_u64(); n = stream.read_u16(); stream.read_u16()
        self.main_event._idx = stream.read_u16(); stream.read_u16()
        if n:
            with SeekContext(stream, off):
                self._sub_flow_event_indices = [stream.read_u16() for _ in range(n)]
    ep.EntryPoint._do_read = read
    try:
        ef = evfl.EventFlow(); ef.read(data)
    finally:
        ep.EntryPoint._do_read = keep
    return ef


def redirect_case(raw, name, event, query, case, to):
    """Patch switch <event> (asking <query>) of a flowchart in place so its case <case> goes where case <to> goes. The
    case table is rows {u32 value, u16 event index, u16 0} in the order the file lists them."""
    events = read_flow(bytes(raw)).flowchart.events
    sw = [e for e in events if e.name == event]
    assert len(sw) == 1 and isinstance(sw[0].data, SwitchEvent) and sw[0].data.actor_query.v.v == query, f'{name}: {event} is not a {query} switch'
    cases = sw[0].data.cases
    table = b''.join(struct.pack('<IHH', v, events.index(cases[v].v), 0) for v in cases)
    at = [i for i in range(0, len(raw) - len(table) + 1, 4) if raw[i:i + len(table)] == table]
    assert len(at) == 1, f'{name}: the case table of {event} not found once'
    row = at[0] + 8 * list(cases).index(case)
    target = cases[to].v
    struct.pack_into('<H', raw, row + 4, events.index(target))
    again = [e for e in read_flow(bytes(raw)).flowchart.events if e.name == event][0].data.cases
    assert again[case].v.name == target.name, f'{name}: {event} not patched'
    print(f'{name}: {event} ({query.replace("EventFlowQuery", "")}) case {case} now goes to {target.name}')


def visitor_refusal(data, patched=False):
    """The donation script's NetHasVisitor switch: (its name, the refusal's event index, the other case's index)."""
    events = read_flow(data).flowchart.events
    sws = [e for e in events if isinstance(e.data, SwitchEvent) and e.data.actor_query.v.v == 'EventFlowQueryNetHasVisitor']
    assert len(sws) == 1 and set(sws[0].data.cases) == {0, 1}, 'SNPC_owl_01_Museum_Donation: expected one NetHasVisitor switch'
    cases = sws[0].data.cases
    if patched:
        assert cases[0].v is cases[1].v
        return cases[0].v.name
    refuse = cases[0].v
    assert isinstance(refuse.data, ActionEvent) and refuse.data.params.data['MessageID'].endswith(':101'), 'refusal moved'
    return sws[0].name, events.index(refuse), events.index(cases[1].v)


patched_nook = set()


def main():
    ap = argparse.ArgumentParser()
    here = Path(__file__).resolve().parent.parent
    ap.add_argument('--romfs', type=Path, default=here / 'extracted/romfs')
    ap.add_argument('--out', type=Path, default=here / 'build/romfs-coop')
    a = ap.parse_args()
    (a.out / 'EventFlow/Info').mkdir(parents=True, exist_ok=True)
    for name, source, add, entries in ((VISIT_FLOW, VISITOR, add_visit, ('Coop_Visit',)), (HOST_FLOW, HOST, add_host, ('Coop_Host',)),
                                       (LEAVE_FLOW, VISITOR, add_leave, ('Coop_Leave',)), (CLOSE_FLOW, HOST, add_close, ('Coop_Close',))):
        ef = evfl.EventFlow()
        ef.read((a.romfs / f'EventFlow/{source}.bfevfl').read_bytes())
        fc = ef.flowchart   # a flowchart of its own: only the system actor (Orville, MainNpc, would not bind outside the airport)
        fc.name = name
        fc.actors = [x for x in fc.actors if x.identifier.name == 'EventFlowSystemActor']
        fc.actors[0].actions, fc.actors[0].queries, fc.actors[0].argument_entry_point = [], [], make_index(None)
        fc.events, fc.entry_points = [], []
        add(fc)
        with open(a.out / f'EventFlow/{name}.bfevfl', 'wb') as f:
            ef.write(f)
        check = evfl.EventFlow()
        check.read((a.out / f'EventFlow/{name}.bfevfl').read_bytes())
        assert set(entries) <= {e.name for e in check.flowchart.entry_points}, name
        print(f'{name}: {len(check.flowchart.events)} events, actions {[h.v for h in check.flowchart.actors[0].actions]}')
    owl = evfl.EventFlow()
    owl.read((a.romfs / 'EventFlow/SNPC_owl.bfevfl').read_bytes())
    gate = [e for e in owl.flowchart.events if type(e.data).__name__ == 'SwitchEvent'
            and e.data.actor_query.v.v == 'EventFlowQueryIsMyVillage']
    assert len(gate) == 1 and set(gate[0].data.cases) == {0, 1}, 'SNPC_owl: expected one IsMyVillage switch'
    gate[0].data.cases[1] = gate[0].data.cases[0]   # 1 (another island) -> the resident's path
    (a.out / 'museum/EventFlow').mkdir(parents=True, exist_ok=True)
    with open(a.out / 'museum/EventFlow/SNPC_owl.bfevfl', 'wb') as f:
        owl.write(f)
    check = evfl.EventFlow()
    check.read((a.out / 'museum/EventFlow/SNPC_owl.bfevfl').read_bytes())
    sw = [e for e in check.flowchart.events if e.name == gate[0].name][0].data
    assert sw.cases[0].v is sw.cases[1].v
    print(f'SNPC_owl: {gate[0].name} sends visitors the resident way ({sw.cases[0].v.name})')
    donation = 'EventFlow/SNPC_owl_01_Museum_Donation.bfevfl'
    raw = bytearray((a.romfs / donation).read_bytes())
    sw, refuse, go = visitor_refusal(bytes(raw))
    rows = [struct.pack('<IHH', 0, refuse, 0) + struct.pack('<IHH', 1, go, 0)]
    at = [i for i in range(0, len(raw) - 16, 8) if raw[i:i + 16] == rows[0]]
    assert len(at) == 1, 'SNPC_owl_01_Museum_Donation: the visitor switch\'s case table not found once'
    struct.pack_into('<H', raw, at[0] + 4, go)
    (a.out / f'museum/{donation}').write_bytes(raw)
    check = visitor_refusal(bytes(raw), patched=True)
    print(f'SNPC_owl_01_Museum_Donation: {sw} goes on to the donation with visitors ({check})')
    # Tom Nook, Isabelle and the Nook Stop terminal (nook/, ACNH_MP_EDIT "nook"; LAN-COOP-DESIGN.md 8.19)
    for flow, event, query, case, to in (
            ('SNPC_rco', 'Event27', 'EventFlowQueryNetHasVisitor', 0, 1),   # "abuzz with off-island company" (801) -> his usual talk
            ('SNPC_rco', 'Event0', 'EventFlowQueryIsMyVillage', 1, 0),      # a visitor's small talk -> the resident's
            ('Obj_RcoMachine', 'Event8', 'EventFlowQueryIsMyVillage', 1, 0),   # "Only registered residents" -> the menu
            ('Obj_RcoMachine', 'Event164', 'EventFlowQueryPlayerHouseLevel', 0, 1),   # no house here: "must be registered" -> the menu
            ('SNPC_sza', 'Event0', 'EventFlowQueryIsMyVillage', 1, 0),      # Isabelle: a visitor's small talk -> the resident's
            ('SNPC_sza_00_Common', 'Event47', 'EventFlowQueryNetHasVisitor', 0, 1)):   # "a bit overwhelmed" (801) -> her usual talk
        out = a.out / f'nook/EventFlow/{flow}.bfevfl'
        out.parent.mkdir(parents=True, exist_ok=True)
        raw = bytearray(out.read_bytes() if out.exists() and flow in patched_nook else (a.romfs / f'EventFlow/{flow}.bfevfl').read_bytes())
        patched_nook.add(flow)
        redirect_case(raw, flow, event, query, case, to)
        out.write_bytes(raw)
    info_path = a.romfs / 'EventFlow/Info/EventFlowInfoProduct.byml'
    raw = info_path.read_bytes()
    info = byml.Byml(raw).parse()
    info = copy.deepcopy(info)
    info[VISIT_FLOW] = {'Coop_Visit': []}
    info[HOST_FLOW] = {'Coop_Host': [], 'Coop_Test': []}
    info[LEAVE_FLOW] = {'Coop_Leave': []}
    info[CLOSE_FLOW] = {'Coop_Close': []}
    version = raw[2] | raw[3] << 8
    with open(a.out / 'EventFlow/Info/EventFlowInfoProduct.byml', 'wb') as f:
        byml.Writer(info, be=False, version=version).write(f)
    print(f'overlay written to {a.out}')


if __name__ == '__main__':
    main()
