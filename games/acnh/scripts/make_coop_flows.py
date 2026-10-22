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
in Coop_LocalVisitor and Coop_LocalHost. EventFlow/Info/EventFlowInfoProduct.byml lists them (no subfiles). Needs the evfl and byml packages (pip install evfl byml). Nothing made here is committed: it is built from
the player's own game files."""
import argparse
import copy
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


def main():
    ap = argparse.ArgumentParser()
    here = Path(__file__).resolve().parent.parent
    ap.add_argument('--romfs', type=Path, default=here / 'extracted/romfs')
    ap.add_argument('--out', type=Path, default=here / 'build/romfs-coop')
    a = ap.parse_args()
    (a.out / 'EventFlow/Info').mkdir(parents=True, exist_ok=True)
    for name, source, add, entry in ((VISIT_FLOW, VISITOR, add_visit, 'Coop_Visit'), (HOST_FLOW, HOST, add_host, 'Coop_Host')):
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
        assert entry in [e.name for e in check.flowchart.entry_points], name
        print(f'{name}: {len(check.flowchart.events)} events, actions {[h.v for h in check.flowchart.actors[0].actions]}')
    info_path = a.romfs / 'EventFlow/Info/EventFlowInfoProduct.byml'
    raw = info_path.read_bytes()
    info = byml.Byml(raw).parse()
    info = copy.deepcopy(info)
    info[VISIT_FLOW] = {'Coop_Visit': []}
    info[HOST_FLOW] = {'Coop_Host': [], 'Coop_Test': []}
    version = raw[2] | raw[3] << 8
    with open(a.out / 'EventFlow/Info/EventFlowInfoProduct.byml', 'wb') as f:
        byml.Writer(info, be=False, version=version).write(f)
    print(f'overlay written to {a.out}')


if __name__ == '__main__':
    main()
