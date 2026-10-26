"""Dependency validation must fail before any output/device action."""
import json,sys,tempfile,unittest
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'tools'))
import assemble
class Inputs(unittest.TestCase):
    def test_missing_inputs_fail(self):
        with tempfile.TemporaryDirectory() as t:
            with self.assertRaisesRegex(ValueError,'missing ordinary input'):
                assemble.resolve_inputs(Path(t),Path(t))
    def test_corrupt_inputs_fail(self):
        spec=json.loads((assemble.ROOT/'profiles/runtime-inputs.json').read_text())
        with tempfile.TemporaryDirectory() as t:
            root=Path(t)
            for rel,item in spec.items():
                p=root/rel if item['redistributable'] else root/Path(rel).name
                p.parent.mkdir(parents=True,exist_ok=True);p.write_bytes(b'wrong')
            with self.assertRaisesRegex(ValueError,'hash/size mismatch'):
                assemble.resolve_inputs(root,root)
    def test_four_proprietary_inputs(self):
        spec=json.loads((assemble.ROOT/'profiles/runtime-inputs.json').read_text())
        actual={Path(k).name for k,v in spec.items() if not v['redistributable']}
        self.assertEqual(actual,{'MoseyApp.apk','mosey_server','libmosey_daemon_ffi.so','amss20.bin'})
    def test_template_no_binary_or_secret(self):
        root=assemble.ROOT/'module/template'
        self.assertFalse(any(p.suffix in ['.apk','.dex','.ko','.p12','.pem','.key'] for p in root.rglob('*')))
    def test_no_auto_activation_marker(self):
        self.assertFalse((assemble.ROOT/'module/template/auto/enabled').exists())
if __name__=='__main__':unittest.main()
