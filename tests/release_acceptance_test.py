"""Owner acceptance must not silently carry over to different firmware inputs."""
import hashlib
import importlib.util
import json
from pathlib import Path
import tempfile
import subprocess
import unittest

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('firmware_manifest', ROOT/'tools/firmware_manifest.py')
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)

class AcceptanceTests(unittest.TestCase):
    def test_current_product_matches_accepted_candidate(self):
        record=json.loads((ROOT/'docs/release/v0.94-hardware-acceptance.json').read_text())
        self.assertTrue(module.accepted_product_matches(ROOT,record))
        self.assertEqual(record['accepted_source'],'e49d1d69755ca5814c1a894cdbcadde5066ea8ce')

    def test_changes_cannot_inherit_acceptance(self):
        with tempfile.TemporaryDirectory() as directory:
            root=Path(directory)
            for folder in ('src','include','third_party'):(root/folder).mkdir()
            path=root/'CMakeLists.txt';path.write_text('old firmware')
            b=path.read_bytes()
            record={'schema_version':1,'status':'USER_ACCEPTED','product_blobs':{
                'CMakeLists.txt':hashlib.sha1(b'blob '+str(len(b)).encode()+b'\0'+b).hexdigest()}}
            self.assertTrue(module.accepted_product_matches(root,record))
            path.write_text('changed firmware')
            self.assertFalse(module.accepted_product_matches(root,record))
            path.write_bytes(b)
            (root/'src/new.cpp').write_text('new feature')
            self.assertFalse(module.accepted_product_matches(root,record))
            (root/'src/new.cpp').unlink();path.unlink()
            self.assertFalse(module.accepted_product_matches(root,record))
            path.write_bytes(b);record['status']='PENDING'
            self.assertFalse(module.accepted_product_matches(root,record))
            record['status']='USER_ACCEPTED';record['schema_version']=999
            self.assertFalse(module.accepted_product_matches(root,record))

    def test_sdk_dirty_or_changed_checkout_is_not_verified(self):
        with tempfile.TemporaryDirectory() as directory:
            sdk=Path(directory)
            def git(*args):subprocess.run(['git','-C',str(sdk),*args],check=True,capture_output=True)
            git('init');git('config','user.name','Test');git('config','user.email','test@example.invalid')
            (sdk/'header.h').write_text('accepted SDK')
            git('add','.');git('commit','-m','accepted SDK')
            first=module.sdk_identity(sdk)
            self.assertTrue(first['verified']);self.assertEqual(first['submodules'],{})
            (sdk/'header.h').write_text('modified SDK')
            self.assertFalse(module.sdk_identity(sdk)['verified'])
            git('checkout','--','header.h')
            (sdk/'extra.h').write_text('untracked build input')
            self.assertFalse(module.sdk_identity(sdk)['verified'])
            git('add','.');git('commit','-m','different SDK content')
            second=module.sdk_identity(sdk)
            self.assertTrue(second['verified']);self.assertNotEqual(first['commit'],second['commit'])
            self.assertFalse(module.sdk_identity(sdk/'missing')['verified'])

if __name__=='__main__':unittest.main()
