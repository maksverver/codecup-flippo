#!/usr/bin/python3

import json
import socket
import subprocess
import sys
from urllib.request import urlopen

BASE_URL = 'http://localhost:8027'

def ReadEventStream(f):
    data = b''
    while True:
        line = f.readline()
        if not line:
            break
        if line.startswith(b'data:'):
            if len(line) > 5 and line[5] == b' ':
                data += line[6:]
            else:
                data += line[5:]
        elif not line.strip():
            if data:
                yield data.decode('utf-8')
                data = b''


def ParseCaiaMove(s):
    assert len(s) == 2
    row = ord(s[0]) - ord('A')
    col = ord(s[1]) - ord('1')
    assert 0 <= row < 8
    assert 0 <= col < 8
    return (row, col)

def FormatCaiaMove(row, col):
    assert 0 <= row < 8
    assert 0 <= col < 8
    return chr(ord('A') + row) + chr(ord('1') + col)

if len(sys.argv) < 3:
    print('Usage: {} <player> <command> <args...>'.format(sys.argv[0]))
    sys.exit(1)

if sys.argv[1] not in ('1', '2'):
    print('Player argument must be 1 or 2 (not: {})'.format(sys.argv[1]))
    sys.exit(1)

myPlayer = int(sys.argv[1])
commandArgs = sys.argv[2:]

popen = subprocess.Popen(args=commandArgs, stdin=subprocess.PIPE, stdout=subprocess.PIPE)
for update in ReadEventStream(urlopen(BASE_URL + '/game-updates')):
    update = json.loads(update)
    if update['mode'] == 'move' and update['state']['public']['nextPlayer'] == myPlayer:
        # Write other player's last move (or "Start" if my player moves first).
        if 'lastMove' in update:
            row, col = update['lastMove']
            lastMove = FormatCaiaMove(row, col)
        else:
            lastMove = 'Start'
        popen.stdin.write((lastMove + '\n').encode('utf-8'))
        popen.stdin.flush()

        # Read my player's move.
        move = popen.stdout.readline().decode('utf-8').strip()
        row, col = ParseCaiaMove(move)
        moveData = {
            'action': 'move',
            'player': str(myPlayer),
            'move': [row, col]}

        # Send my player's move to the server.
        urlopen(BASE_URL + '/update-game', json.dumps(moveData).encode('utf-8'))
