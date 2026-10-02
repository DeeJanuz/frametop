"""Only addresses upstream's isolated test compositor, never live input sockets."""
import json
import socket
import time

with socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM) as client:
    client.bind('')
    client.settimeout(2)
    def ask(command):
        client.sendto(command.encode(), '\0ft_screens_test')
        return client.recv(8192).decode()
    for command in ('mouse-layout 1 0 0 960 540 2',
                    'mouse-layout 2 960 0 540 960 2', 'mouse-output-off 24',
                    'mouse-mode desktop', 'mouse-position 1 100 100', 'mouse-move 10 5',
                    'mouse-button 272 1', 'mouse-move 900 0', 'mouse-button 272 0',
                    'mouse-wheel 0 0.125'):
        assert ask(command) == 'ok', command
    assert ask('mouse-output-off 25').startswith('error')
    assert ask('mouse-layout 25 0 0 960 540 1').startswith('error')
    ask('mouse-presence 1 1')
    before = json.loads(ask('mouse?'))
    ask('input 1 down 200 100 left'); ask('input 1 up 200 100 left')
    after = json.loads(ask('mouse?'))
    assert after['controllerEventsIgnored'] >= before['controllerEventsIgnored'] + 2
    ask('mouse-presence 0 1')
    time.sleep(1.1)
    assert json.loads(ask('mouse-presence?'))['controllerDesktop']
    before = json.loads(ask('mouse?'))
    ask('input 1 down 200 100 left')
    after = json.loads(ask('mouse?'))
    assert after['controllerEventsIgnored'] == before['controllerEventsIgnored']
    ask('mouse-presence 1 1')
    assert not json.loads(ask('mouse-presence?'))['controllerDesktop']
    ask('mouse-button 272 1'); ask('mouse-button 272 0')
print('Native motion/click/wheel, cross-output drag, output bounds and controller fallback passed')

with socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM) as client:
    client.bind(''); client.settimeout(2)
    def ask(command):
        client.sendto(command.encode(), '\0ft_screens_test')
        return client.recv(8192).decode()
    def policy(): return json.loads(ask('desktop-policy?'))
    assert ask('desktop-policy last-active') == 'ok'
    assert ask('mouse-presence 1 1') == 'ok'
    assert ask('mouse-move 3 0') == 'ok'
    assert policy()['owner'] == 'mouse'
    for _ in range(3): ask('input 1 move 200 100')
    assert policy()['owner'] == 'mouse', 'aiming must not claim ownership'
    time.sleep(.01)
    ask('input 1 down 200 100 left')
    assert policy()['owner'] == 'pointer'
    assert policy()['pointerButtons'] == 1
    before = json.loads(ask('mouse?'))
    ask('mouse-move 40 0'); ask('mouse-button 272 1'); ask('mouse-button 272 0'); ask('mouse-wheel 0 1')
    after = json.loads(ask('mouse?'))
    assert before['x'] == after['x'], 'mouse motion must not accumulate during pointer drag'
    assert before['buttonEvents'] == after['buttonEvents']
    assert before['wheelEvents'] == after['wheelEvents']
    assert policy()['owner'] == 'pointer'
    assert ask('desktop-policy mouse').startswith('error'), 'cannot change policy during drag'
    ask('mouse-presence 1 1')
    assert policy()['owner'] == 'pointer', 'presence heartbeat must not steal ownership'
    ask('input 1 up 200 100 left')
    assert policy()['pointerButtons'] == 0
    ask('mouse-wheel 0 1')
    assert policy()['owner'] == 'mouse'
    ask('mouse-button 272 1'); time.sleep(.01)
    ask('input 1 down 200 100 left'); ask('input 1 up 200 100 left')
    assert policy()['owner'] == 'mouse'
    assert policy()['mouseButtons'] == 1
    ask('mouse-button 272 0')
    time.sleep(.01); ask('input 1 down 200 100 left'); ask('input 1 up 200 100 left')
    assert policy()['owner'] == 'pointer'
    ask('mouse-move 0 0')
    assert policy()['owner'] == 'pointer', 'zero movement must not claim'
    ask('mouse-move 1 0')
    assert policy()['owner'] == 'mouse'
    time.sleep(.01)
    ask('input 1 move 200 100'); ask('input 1 scroll 0 1')
    assert policy()['owner'] == 'pointer', 'controller scroll must claim desktop'
    ask('mouse-wheel 0 1')
    assert policy()['owner'] == 'mouse'
    assert ask('desktop-policy pointer') == 'ok'
    ask('mouse-move 1 0')
    assert policy()['owner'] == 'pointer'
    assert ask('desktop-policy mouse') == 'ok'
    ask('input 1 down 200 100 left'); ask('input 1 up 200 100 left')
    assert policy()['owner'] == 'mouse'
print('Desktop handoff, aiming exclusion, drag locks, ignored releases, presence stability and policy controls passed')
