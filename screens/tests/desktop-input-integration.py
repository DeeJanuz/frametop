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
