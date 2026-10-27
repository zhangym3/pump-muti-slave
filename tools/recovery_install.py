"""Trusted USB recovery credential installation. Never emits secrets to stdout/logs.

prepare writes the encrypted APP-compatible backup and a separate random recovery
code before any device write. initialize can resume after an uncertain USB reply:
it verifies the saved device credential via a fresh HMAC challenge, never replaces it.
"""
import argparse
import csv
import hashlib
import hmac
import json
import os
from pathlib import Path
import re
import secrets
import struct
import subprocess
import time

from cryptography.hazmat.primitives.ciphers.aead import AESGCM
from cryptography.hazmat.primitives.hashes import SHA256
from cryptography.hazmat.primitives.kdf.hkdf import HKDF

MAGIC = b'PMRECBA1'

def derive(code, salt):
    if len(code) != 32 or not any(code): raise ValueError('Invalid recovery code')
    return HKDF(algorithm=SHA256(), length=32, salt=salt, info=b'PUMP-RECOVERY-V1/backup-key').derive(code)

def encode(uuid, key, name):
    label = name.encode('utf-8', errors='strict')
    if len(uuid) != 16 or not any(uuid) or len(key) != 32 or not any(key): raise ValueError('Invalid credential')
    if len(label) > 128 or any(ord(c) < 32 or 127 <= ord(c) <= 159 for c in name): raise ValueError('Invalid name')
    return b'RCV1' + struct.pack('<HH', 1, 1) + uuid + struct.pack('<I', 1) + key + struct.pack('<H', len(label)) + label

def encrypt(uuid, key, name, code):
    plain = encode(uuid, key, name)
    salt, iv = secrets.token_bytes(16), secrets.token_bytes(12)
    header = MAGIC + struct.pack('<H', 1) + salt + iv + struct.pack('<I', len(plain) + 16)
    return header + AESGCM(derive(code, salt)).encrypt(iv, plain, header)

def decrypt(backup, code):
    if not 120 <= len(backup) <= 248 or backup[:10] != MAGIC + b'\1\0': raise ValueError('Invalid installation backup')
    if struct.unpack_from('<I', backup, 38)[0] != len(backup) - 42: raise ValueError('Invalid backup length')
    plain = AESGCM(derive(code, backup[10:26])).decrypt(backup[26:38], backup[42:], backup[:42])
    if plain[:8] != b'RCV1\1\0\1\0' or struct.unpack_from('<I', plain, 24)[0] != 1: raise ValueError('Invalid installation schema')
    length = struct.unpack_from('<H', plain, 60)[0]
    if length > 128 or len(plain) != 62 + length: raise ValueError('Invalid installation record')
    uuid, key, name = plain[8:24], plain[28:60], plain[62:].decode('utf-8', errors='strict')
    if encode(uuid, key, name) != plain: raise ValueError('Noncanonical installation record')
    return uuid, key, name

def exclusive_write(path, data):
    path.parent.mkdir(parents=True, exist_ok=True)
    # Restrict a new, still-empty file before placing recovery material in it.
    # On Windows, mode=0600 alone does not remove inherited ACL permissions.
    fd = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
    with os.fdopen(fd, 'wb') as output:
        if os.name == 'nt':
            identity = subprocess.run(['whoami.exe', '/user', '/fo', 'csv', '/nh'],
                capture_output=True, check=True, text=True)
            sid = list(csv.reader(identity.stdout.splitlines()))[0][-1]
            if not re.fullmatch(r'S-1-5-(?:\d+-)*\d+', sid): raise RuntimeError('Cannot determine installation owner')
            subprocess.run(['icacls.exe', str(path), '/inheritance:r', '/grant:r',
                '*' + sid + ':(F)', '*S-1-5-18:(F)'], capture_output=True, check=True)
        output.write(data); output.flush(); os.fsync(output.fileno())

class Console:
    def __init__(self, port):
        import serial
        self.port = serial.Serial(port=None, baudrate=115200, timeout=.1, write_timeout=2)
        self.port.dtr = False; self.port.rts = False; self.port.port = port
        self.port.open()
    def close(self): self.port.close()
    def command(self, text, prefix, timeout=6):
        self.port.reset_input_buffer()
        self.port.write(text.encode('ascii') + b'\n')
        deadline = time.monotonic() + timeout
        line = bytearray()
        while time.monotonic() < deadline:
            data = self.port.read(1)
            if not data: continue
            if data == b'\n':
                reply = line.decode('ascii', errors='ignore').strip(); line.clear()
                if reply.startswith(prefix): return reply
                # Raw device output deliberately never goes to stdout or a log.
            elif len(line) < 512: line.extend(data)
            else: line.clear()
        raise RuntimeError('USB reply unavailable; preserve backup/code and retry verification')

def verify(console, uuid, key):
    nonce = secrets.token_bytes(32)
    reply = console.command('recovery-check ' + nonce.hex(), 'RECOVERY_CHECK ')
    fields = dict(re.findall(r'(epoch|nonce|proof)=([0-9A-Fa-f]+)', reply))
    if fields.get('epoch') != '1': raise RuntimeError('Unexpected device epoch')
    peer = bytes.fromhex(fields.get('nonce', '')); proof = bytes.fromhex(fields.get('proof', ''))
    if len(peer) != 32 or not any(peer) or peer == nonce or len(proof) != 32: raise RuntimeError('Invalid device proof')
    transcript = b'PUMP-RECOVERY-V1' + struct.pack('<H', 1) + uuid + struct.pack('<I', 1) + nonce + peer
    expected = hmac.digest(key, b'device-proof' + transcript, 'sha256')
    if not hmac.compare_digest(expected, proof): raise RuntimeError('Device credential does not match installation backup')

def initialize(console, backup, code):
    uuid, key, _ = decrypt(backup, code)
    status = console.command('recovery-status', 'RECOVERY_CREDENTIAL ')
    fields = dict(re.findall(r'(schema|uuid|initialized|epoch|recovery_enabled)=([0-9A-Fa-f]+)', status))
    if fields.get('schema') != '1' or fields.get('uuid', '').lower() != uuid.hex() or fields.get('recovery_enabled') != '0':
        raise RuntimeError('USB device identity/schema does not match prepared backup')
    if fields.get('initialized') == '0' and fields.get('epoch') == '0':
        # The device performs its own serialised control-task safety check.
        reply = console.command('recovery-init CONFIRM ' + key.hex(), 'OK recovery_initialized ')
        if reply != 'OK recovery_initialized epoch=1 recovery_enabled=0': raise RuntimeError('Initialization not confirmed')
    elif fields.get('initialized') != '1' or fields.get('epoch') != '1':
        raise RuntimeError('Credential conflict; initialization will not replace a device key')
    verify(console, uuid, key)
    return {'uuid': uuid.hex(), 'epoch': 1, 'credentialVerified': True, 'recoveryEnabled': False,
            'backupSha256': hashlib.sha256(backup).hexdigest(), 'meshResetIssued': False, 'outputCommandIssued': False}

def main():
    ap = argparse.ArgumentParser(description=__doc__)
    sub = ap.add_subparsers(dest='mode', required=True)
    prepare = sub.add_parser('prepare'); prepare.add_argument('--uuid', required=True); prepare.add_argument('--name', required=True)
    install = sub.add_parser('initialize'); install.add_argument('--port', required=True); install.add_argument('--receipt', type=Path, required=True)
    for parser in (prepare, install):
        parser.add_argument('--backup', type=Path, required=True); parser.add_argument('--code-file', type=Path, required=True)
    args = ap.parse_args()
    if args.backup.resolve().parent == args.code_file.resolve().parent: raise ValueError('Store backup and recovery code in separate directories')
    if args.mode == 'prepare':
        if args.backup.exists() or args.code_file.exists(): raise FileExistsError('Installation material already exists; never overwrite')
        uuid, key, code = bytes.fromhex(args.uuid), secrets.token_bytes(32), secrets.token_bytes(32)
        backup = encrypt(uuid, key, args.name, code)
        if decrypt(backup, code) != (uuid, key, args.name): raise RuntimeError('Backup verification failed')
        # A failure here has not written anything to the device. Preserve both files.
        exclusive_write(args.code_file, ('-'.join(code.hex().upper()[i:i+8] for i in range(0,64,8)) + '\n').encode('ascii'))
        exclusive_write(args.backup, backup)
        if decrypt(args.backup.read_bytes(), bytes.fromhex(args.code_file.read_text().strip().replace('-', ''))) != (uuid,key,args.name):
            raise RuntimeError('Saved installation material verification failed')
        print('Encrypted backup and separate recovery code prepared and verified; device unchanged.')
    else:
        if args.receipt.exists(): raise FileExistsError('Receipt exists; choose a new receipt path')
        backup = args.backup.read_bytes()
        code = bytes.fromhex(args.code_file.read_text(encoding='ascii').strip().replace('-', ''))
        console = Console(args.port)
        try: receipt = initialize(console, backup, code)
        finally: console.close()
        exclusive_write(args.receipt, json.dumps(receipt, indent=2).encode('utf-8'))
        print('USB credential verified; Mesh and outputs unchanged. Receipt saved.')

if __name__ == '__main__':
    try: main()
    except Exception as error:
        # No arbitrary exception text/traceback: third-party errors can contain IO buffers.
        print('Installation stopped (' + type(error).__name__ + '); preserve backup and code. No overwrite fallback.')
        raise SystemExit(1)
