"""Cross-platform backup format, tamper rejection and uncertain USB commit tests."""
import importlib.util
from pathlib import Path
import struct
import tempfile
import unittest

spec=importlib.util.spec_from_file_location('installer',Path(__file__).resolve().parents[1]/'tools/recovery_install.py')
installer=importlib.util.module_from_spec(spec); spec.loader.exec_module(installer)
UUID=bytes(range(97,113)); KEY=bytes(range(1,33)); CODE=bytes(range(129,161))

class Console:
    def __init__(self, initialized=False, uuid=UUID, key=KEY):
        self.initialized=initialized; self.uuid=uuid; self.key=key; self.writes=0
    def command(self,text,prefix):
        if text=='recovery-status':
            return f'RECOVERY_CREDENTIAL schema=1 uuid={self.uuid.hex()} initialized={int(self.initialized)} epoch={int(self.initialized)} recovery_enabled=0'
        if text.startswith('recovery-init CONFIRM '):
            if self.initialized: raise AssertionError('Must not replace an initialized credential')
            self.writes+=1; self.initialized=True; self.key=bytes.fromhex(text.split()[-1])
            return 'OK recovery_initialized epoch=1 recovery_enabled=0'
        if text.startswith('recovery-check '):
            nonce=bytes.fromhex(text.split()[-1]); peer=bytes(range(65,97))
            transcript=b'PUMP-RECOVERY-V1'+struct.pack('<H',1)+self.uuid+struct.pack('<I',1)+nonce+peer
            proof=installer.hmac.digest(self.key,b'device-proof'+transcript,'sha256')
            return f'RECOVERY_CHECK epoch=1 nonce={peer.hex()} proof={proof.hex()}'
        raise AssertionError('Unexpected USB command')

class InstallTests(unittest.TestCase):
    def test_exclusive_material_write_never_overwrites(self):
        with tempfile.TemporaryDirectory() as directory:
            path=Path(directory)/'code.txt'
            installer.exclusive_write(path,b'public synthetic fixture')
            self.assertEqual(b'public synthetic fixture',path.read_bytes())
            with self.assertRaises(FileExistsError): installer.exclusive_write(path,b'replaced')
            self.assertEqual(b'public synthetic fixture',path.read_bytes())
    def test_backup_roundtrip_and_every_byte_authenticated(self):
        backup=installer.encrypt(UUID,KEY,'阀门测试',CODE)
        self.assertEqual((UUID,KEY,'阀门测试'),installer.decrypt(backup,CODE))
        for i in range(len(backup)):
            changed=bytearray(backup); changed[i]^=1
            with self.assertRaises(Exception): installer.decrypt(bytes(changed),CODE)
        for length in range(len(backup)):
            with self.assertRaises(Exception): installer.decrypt(backup[:length],CODE)
        with self.assertRaises(Exception): installer.decrypt(backup,CODE[::-1])
    def test_first_install_and_uncertain_commit_resume(self):
        backup=installer.encrypt(UUID,KEY,'A',CODE); console=Console()
        self.assertTrue(installer.initialize(console,backup,CODE)['credentialVerified'])
        self.assertEqual(1,console.writes)
        self.assertTrue(installer.initialize(console,backup,CODE)['credentialVerified'])
        self.assertEqual(1,console.writes)
    def test_wrong_device_or_existing_key_never_overwritten(self):
        backup=installer.encrypt(UUID,KEY,'A',CODE)
        for console in (Console(False,UUID[::-1]),Console(True,UUID,KEY[::-1])):
            with self.assertRaises(RuntimeError): installer.initialize(console,backup,CODE)
            self.assertEqual(0,console.writes)
    def test_android_backup_vector(self):
        # Produced by Android's real AES-GCM provider; loaded by host from a fixture.
        fixture=Path(__file__).parent/'fixtures/recovery_backup_android.hex'
        if not fixture.exists(): self.skipTest('Android vector not yet captured')
        self.assertEqual((UUID,KEY,'阀门测试'),installer.decrypt(bytes.fromhex(fixture.read_text()),CODE))

if __name__=='__main__': unittest.main()
